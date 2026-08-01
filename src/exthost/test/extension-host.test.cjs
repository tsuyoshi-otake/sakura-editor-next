'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const process = require('node:process');
const test = require('node:test');

const {
  ExtensionHost,
  FrameDecoder,
  ProtocolError,
  encodeFrame,
  parseRpcPayload,
  readHostConfig,
} = require('../src/extension-host.cjs');

function uniquePipeName() {
  const id = `sakura-exthost-${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return process.platform === 'win32'
    ? `\\\\.\\pipe\\${id}`
    : path.join(os.tmpdir(), `${id}.sock`);
}

class TestClient {
  constructor(pipeName) {
    this.socket = net.connect(pipeName);
    this.decoder = new FrameDecoder();
    this.messages = [];
    this.waiters = [];
    this.nextId = 1;
    this.socket.on('data', (chunk) => {
      for (const payload of this.decoder.feed(chunk)) {
        const message = parseRpcPayload(payload);
        if (Object.prototype.hasOwnProperty.call(message, 'id') && message.method === 'client/echo') {
          this.socket.write(encodeFrame({ jsonrpc: '2.0', id: message.id, result: message.params }));
        } else {
          this.messages.push(message);
          this.flush();
        }
      }
    });
  }

  async connect() {
    if (!this.socket.readyState || this.socket.readyState === 'opening') {
      await new Promise((resolve, reject) => {
        this.socket.once('connect', resolve);
        this.socket.once('error', reject);
      });
    }
  }

  request(method, params) {
    const id = `test-${this.nextId++}`;
    this.socket.write(encodeFrame({ jsonrpc: '2.0', id, method, params }));
    return this.waitFor((message) => message.id === id);
  }

  waitFor(predicate, timeoutMs = 2000) {
    const found = this.messages.findIndex(predicate);
    if (found >= 0) return Promise.resolve(this.messages.splice(found, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { predicate, resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        this.waiters = this.waiters.filter((item) => item !== waiter);
        reject(new Error('timed out waiting for extension host message'));
      }, timeoutMs);
      this.waiters.push(waiter);
    });
  }

  flush() {
    for (const waiter of [...this.waiters]) {
      const found = this.messages.findIndex(waiter.predicate);
      if (found < 0) continue;
      this.waiters = this.waiters.filter((item) => item !== waiter);
      clearTimeout(waiter.timer);
      waiter.resolve(this.messages.splice(found, 1)[0]);
    }
  }

  close() {
    this.socket.destroy();
  }
}

test('frame decoder handles fragmentation, coalescing, and big-endian lengths', () => {
  const first = encodeFrame({ jsonrpc: '2.0', method: 'one' });
  const second = encodeFrame({ jsonrpc: '2.0', method: 'two' });
  assert.equal(first.readUInt32BE(0), first.length - 4);
  const decoder = new FrameDecoder();
  assert.deepEqual(decoder.feed(first.subarray(0, 2)), []);
  const frames = decoder.feed(Buffer.concat([first.subarray(2), second]));
  assert.deepEqual(frames.map((item) => parseRpcPayload(item).method), ['one', 'two']);
});

test('frame decoder enters a terminal state after an oversized payload', () => {
  const decoder = new FrameDecoder(3);
  const header = Buffer.alloc(4);
  header.writeUInt32BE(4);
  assert.throws(() => decoder.feed(header), ProtocolError);
  assert.throws(() => decoder.feed(Buffer.alloc(0)), ProtocolError);
});

test('host configuration binds the pipe to profile hash, boot ID, generation, and broker', () => {
  const profileHash = '1'.repeat(32);
  const bootId = '2'.repeat(32);
  const environment = {
    SAKURA_EXTENSION_HOST: '1',
    SAKURA_PROFILE_HASH: profileHash,
    SAKURA_BOOT_ID: bootId,
    SAKURA_PIPE_NAME: `\\\\.\\pipe\\sakura-exthost-${profileHash}-${bootId}`,
    SAKURA_GENERATION: '7',
    SAKURA_BROKER_PID: '1234',
  };
  assert.deepEqual(readHostConfig(environment), {
    profileHash,
    bootId,
    pipeName: environment.SAKURA_PIPE_NAME,
    generation: 7,
    brokerProcessId: 1234,
  });
  assert.throws(() => readHostConfig({ ...environment, SAKURA_PIPE_NAME: '\\\\.\\pipe\\other' }));
  assert.throws(() => readHostConfig({ ...environment, SAKURA_GENERATION: '0' }));
});

test('shared host accepts multiple clients and performs full-duplex RPC', async (t) => {
  const config = {
    profileHash: 'a'.repeat(32),
    bootId: 'b'.repeat(32),
    pipeName: uniquePipeName(),
    generation: 3,
    brokerProcessId: process.pid,
  };
  const host = new ExtensionHost(config);
  await host.start();
  t.after(() => host.stop());

  const first = new TestClient(config.pipeName);
  const second = new TestClient(config.pipeName);
  t.after(() => first.close());
  t.after(() => second.close());
  await Promise.all([first.connect(), second.connect()]);

  const [hello1, hello2] = await Promise.all([
    first.waitFor((message) => message.method === 'host/hello'),
    second.waitFor((message) => message.method === 'host/hello'),
  ]);
  assert.equal(hello1.params.generation, 3);
  assert.equal(hello2.params.processId, process.pid);

  const ping = await first.request('host/ping', { nonce: 'roundtrip' });
  assert.equal(ping.result.nonce, 'roundtrip');
  assert.equal(ping.result.bootId, config.bootId);

  const duplex = await first.request('host/requestClientEcho', { value: 'from-host' });
  assert.deepEqual(duplex.result, { value: 'from-host' });

  const diagnostic = await second.request('host/getDiagnostics', {});
  assert.equal(diagnostic.result.connectionCount, 2);

  const unsupported = await second.request('missing/method', {});
  assert.equal(unsupported.error.code, -32601);
  assert.match(unsupported.error.message, /missing\/method/);
});

test('shared host loads one extension instance and broadcasts its workbench state', async (t) => {
  const extensionRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-shared-extension-'));
  fs.writeFileSync(path.join(extensionRoot, 'package.json'), JSON.stringify({
    name: 'shared', publisher: 'test', main: './extension.js',
    contributes: { commands: [{ command: 'test.shared.count', title: 'Shared Count' }] },
  }));
  fs.writeFileSync(path.join(extensionRoot, 'extension.js'), `
    const vscode = require('vscode');
    let activations = 0;
    exports.activate = (context) => {
      activations += 1;
      context.subscriptions.push(vscode.commands.registerCommand('test.shared.count', () => activations));
    };
  `);
  t.after(() => fs.rmSync(extensionRoot, { recursive: true, force: true }));

  const config = {
    profileHash: 'c'.repeat(32), bootId: 'd'.repeat(32), pipeName: uniquePipeName(),
    generation: 4, brokerProcessId: process.pid,
  };
  const host = new ExtensionHost(config);
  await host.start();
  t.after(() => host.stop());
  const first = new TestClient(config.pipeName);
  const second = new TestClient(config.pipeName);
  t.after(() => first.close());
  t.after(() => second.close());
  await Promise.all([first.connect(), second.connect()]);
  await Promise.all([
    first.waitFor((message) => message.method === 'host/hello'),
    second.waitFor((message) => message.method === 'host/hello'),
  ]);

  const registeredFirst = await first.request('host/registerExtensions', { extensions: [extensionRoot] });
  const registeredSecond = await second.request('host/registerExtensions', { extensions: [extensionRoot] });
  assert.equal(registeredFirst.result.failed.length, 0);
  assert.equal(registeredSecond.result.failed.length, 0);
  assert.equal(registeredSecond.result.registered[0].extensionId, 'test.shared');

  const firstResult = await first.request('extension/commands/execute', {
    command: 'test.shared.count', args: [],
  });
  const secondResult = await second.request('extension/commands/execute', {
    command: 'test.shared.count', args: [],
  });
  assert.deepEqual(firstResult.result, { value: 1 });
  assert.deepEqual(secondResult.result, { value: 1 });
  await Promise.all([
    first.waitFor((message) => message.method === 'workbench/commands/registerHandler'),
    second.waitFor((message) => message.method === 'workbench/commands/registerHandler'),
  ]);
});
