'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { randomBytes } = require('node:crypto');
const net = require('node:net');
const { performance } = require('node:perf_hooks');

const bundlePath = process.argv[2];
const requestedSamples = Number(process.argv[3] || 30);
if (!bundlePath || !Number.isInteger(requestedSamples) || requestedSamples < 20 || requestedSamples > 200) {
  throw new Error('usage: node extension-host-performance.cjs <extension-host.js> <samples:20..200>');
}

const PROFILE_HASH = randomBytes(16).toString('hex');
const activeHosts = new Set();

function encodeFrame(message) {
  const payload = Buffer.from(JSON.stringify(message), 'utf8');
  const frame = Buffer.allocUnsafe(payload.length + 4);
  frame.writeUInt32BE(payload.length, 0);
  payload.copy(frame, 4);
  return frame;
}

class ClientPeer {
  constructor(socket) {
    this.socket = socket;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.incomingWaiters = [];
    this.hello = new Promise((resolve, reject) => {
      this.resolveHello = resolve;
      this.rejectHello = reject;
    });
    socket.on('data', (chunk) => this.accept(chunk));
    socket.once('error', (error) => this.close(error));
    socket.once('close', () => this.close(new Error('extension host pipe closed')));
  }

  accept(chunk) {
    this.buffer = this.buffer.length === 0 ? chunk : Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 4) {
      const length = this.buffer.readUInt32BE(0);
      if (length > 16 * 1024 * 1024) return this.socket.destroy(new Error('oversized frame'));
      if (this.buffer.length < length + 4) return;
      const payload = this.buffer.subarray(4, length + 4);
      this.buffer = this.buffer.subarray(length + 4);
      this.dispatch(JSON.parse(payload.toString('utf8')));
    }
  }

  dispatch(message) {
    if (message.method === 'host/hello' && message.id === undefined) {
      this.resolveHello(message.params);
      return;
    }
    if (typeof message.method === 'string' && message.id !== undefined) {
      const waiter = this.incomingWaiters.shift();
      if (waiter) waiter.resolve(message);
      return;
    }
    if (message.id === undefined) return;
    const pending = this.pending.get(String(message.id));
    if (!pending) return;
    this.pending.delete(String(message.id));
    clearTimeout(pending.timer);
    if (message.error) {
      const error = new Error(message.error.message || 'extension host request failed');
      error.code = message.error.code;
      pending.reject(error);
    } else {
      pending.resolve(message.result);
    }
  }

  request(method, params, timeoutMs = 5000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(String(id));
        reject(new Error(`request timed out: ${method}`));
      }, timeoutMs);
      this.pending.set(String(id), { resolve, reject, timer });
      this.socket.write(encodeFrame({ jsonrpc: '2.0', id, method, params }));
    });
  }

  waitForIncoming(timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
      const entry = { resolve, reject };
      this.incomingWaiters.push(entry);
      setTimeout(() => {
        const index = this.incomingWaiters.indexOf(entry);
        if (index >= 0) this.incomingWaiters.splice(index, 1);
        reject(new Error('timed out waiting for host request'));
      }, timeoutMs).unref();
    });
  }

  close(error) {
    if (this.closed) return;
    this.closed = true;
    this.rejectHello(error);
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
    for (const waiter of this.incomingWaiters.splice(0)) waiter.reject(error);
  }
}

function waitForExit(child, timeoutMs) {
  if (child.exitCode !== null) return Promise.resolve(child.exitCode);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('extension host did not exit in time')), timeoutMs);
    child.once('exit', (code) => {
      clearTimeout(timer);
      resolve(code);
    });
  });
}

function connectPipe(pipeName, child, deadline) {
  return new Promise((resolve, reject) => {
    const attempt = () => {
      if (child.exitCode !== null) return reject(new Error(`extension host exited before connect: ${child.exitCode}`));
      if (performance.now() >= deadline) return reject(new Error('timed out connecting to extension host pipe'));
      const socket = net.createConnection(pipeName);
      socket.once('connect', () => resolve(socket));
      socket.once('error', (error) => {
        socket.destroy();
        if (error.code === 'ENOENT' || error.code === 'ECONNREFUSED') setTimeout(attempt, 5);
        else reject(error);
      });
    };
    attempt();
  });
}

async function launchHost(generation) {
  const bootId = randomBytes(16).toString('hex');
  const pipeName = `\\\\.\\pipe\\sakura-exthost-${PROFILE_HASH}-${bootId}`;
  const environment = {};
  for (const name of ['SystemRoot', 'WINDIR', 'ComSpec', 'TEMP', 'TMP', 'PATH', 'PATHEXT']) {
    if (process.env[name]) environment[name] = process.env[name];
  }
  Object.assign(environment, {
    SAKURA_EXTENSION_HOST: '1',
    SAKURA_PROFILE_HASH: PROFILE_HASH,
    SAKURA_BOOT_ID: bootId,
    SAKURA_PIPE_NAME: pipeName,
    SAKURA_GENERATION: String(generation),
    SAKURA_BROKER_PID: String(process.pid),
  });
  const startedAt = performance.now();
  const child = spawn(process.execPath, [bundlePath], {
    env: environment,
    stdio: ['ignore', 'ignore', 'pipe'],
    windowsHide: true,
  });
  activeHosts.add(child);
  child.once('exit', () => activeHosts.delete(child));
  let stderr = '';
  child.stderr.on('data', (chunk) => { if (stderr.length < 8192) stderr += chunk.toString('utf8'); });
  try {
    const socket = await connectPipe(pipeName, child, startedAt + 5000);
    const peer = new ClientPeer(socket);
    const hello = await Promise.race([
      peer.hello,
      new Promise((_, reject) => setTimeout(() => reject(new Error('host/hello timed out')), 5000)),
    ]);
    assert.equal(hello.generation, generation);
    return { child, peer, generation, launchMs: performance.now() - startedAt, stderr: () => stderr };
  } catch (error) {
    child.kill('SIGKILL');
    throw new Error(`${error.message}${stderr ? `\n${stderr}` : ''}`);
  }
}

async function stopHost(host) {
  if (!host || host.child.exitCode !== null) return;
  try {
    await host.peer.request('host/quiesce', { generation: host.generation });
    await waitForExit(host.child, 5000);
  } catch (error) {
    host.child.kill('SIGKILL');
    await waitForExit(host.child, 2000).catch(() => {});
    throw new Error(`${error.message}${host.stderr() ? `\n${host.stderr()}` : ''}`);
  }
}

function summarize(values) {
  assert.ok(values.length >= 5, 'performance cohorts require at least five samples');
  const sorted = [...values].sort((left, right) => left - right);
  const percentile = (fraction) => sorted[Math.max(0, Math.ceil(fraction * sorted.length) - 1)];
  return {
    samples: sorted.length,
    p50Ms: percentile(0.50),
    p95Ms: percentile(0.95),
    maxMs: sorted.at(-1),
  };
}

async function main() {
  const coldLaunches = [];
  const warmLaunches = [];
  const ipcRoundTrips = [];
  const hostLossPendingRejects = [];
  let generation = 1;
  for (let index = 0; index < 5; index += 1) {
    const host = await launchHost(generation++);
    coldLaunches.push(host.launchMs);
    await stopHost(host);
  }
  for (let index = 0; index < requestedSamples; index += 1) {
    const host = await launchHost(generation++);
    warmLaunches.push(host.launchMs);
    await stopHost(host);
  }

  const host = await launchHost(generation++);
  for (let index = 0; index < requestedSamples; index += 1) {
    const startedAt = performance.now();
    const response = await host.peer.request('host/ping', { nonce: index });
    assert.equal(response.nonce, index);
    ipcRoundTrips.push(performance.now() - startedAt);
  }
  await stopHost(host);

  for (let index = 0; index < 5; index += 1) {
    const crashHost = await launchHost(generation++);
    const incoming = crashHost.peer.waitForIncoming();
    const pending = crashHost.peer.request('host/requestClientEcho', { value: 'pending-on-crash' }, 5000);
    const hostRequest = await incoming;
    assert.equal(hostRequest.method, 'client/echo');
    const crashStartedAt = performance.now();
    crashHost.child.kill('SIGKILL');
    await assert.rejects(pending);
    hostLossPendingRejects.push(performance.now() - crashStartedAt);
    await waitForExit(crashHost.child, 2000);
  }

  const metrics = {
    coldHostLaunch: summarize(coldLaunches),
    warmHostLaunch: summarize(warmLaunches),
    warmIpcRoundTrip: summarize(ipcRoundTrips),
    hostLossPendingReject: summarize(hostLossPendingRejects),
  };
  const budgets = {
    coldHostLaunchP95Ms: 1500,
    warmHostLaunchP95Ms: 300,
    warmIpcRoundTripP95Ms: 10,
    hostLossPendingRejectMs: 500,
  };
  const passed = metrics.coldHostLaunch.p95Ms <= budgets.coldHostLaunchP95Ms
    && metrics.warmHostLaunch.p95Ms <= budgets.warmHostLaunchP95Ms
    && metrics.warmIpcRoundTrip.p95Ms <= budgets.warmIpcRoundTripP95Ms
    && metrics.hostLossPendingReject.p95Ms <= budgets.hostLossPendingRejectMs;
  process.stdout.write(`EXTENSION_HOST_PERF_JSON=${JSON.stringify({ passed, budgets, metrics })}\n`);
  if (!passed) process.exitCode = 1;
}

process.once('exit', () => {
  for (const child of activeHosts) {
    try { child.kill('SIGKILL'); } catch {}
  }
});

main().catch((error) => {
  for (const child of activeHosts) {
    try { child.kill('SIGKILL'); } catch {}
  }
  console.error(error);
  process.exitCode = 1;
});
