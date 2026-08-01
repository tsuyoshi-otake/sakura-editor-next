'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const { ExtensionLoader } = require('../src/extension-loader.cjs');

class RecordingTransport {
  constructor() {
    this.notifications = [];
    this.requests = [];
  }

  notify(method, params) {
    this.notifications.push({ method, params });
  }

  async request(method, params) {
    this.requests.push({ method, params });
    return {};
  }

  notified(method) {
    return this.notifications.filter((item) => item.method === method);
  }
}

function createExtension(manifest, source) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-exthost-'));
  fs.writeFileSync(path.join(root, 'package.json'), JSON.stringify(manifest));
  if (source !== undefined) fs.writeFileSync(path.join(root, 'extension.js'), source);
  return root;
}

test('registers metadata and lazily activates a command through the vscode bridge', async (t) => {
  const root = createExtension({
    name: 'sample',
    publisher: 'test',
    version: '1.2.3',
    main: './extension',
    contributes: {
      commands: [{ command: 'test.sample.run', title: 'Run Sample', category: 'Test' }],
      views: { explorer: [{ id: 'test.sample.view', name: 'Sample View' }] },
    },
  }, `
    const vscode = require('vscode');
    exports.activate = (context) => {
      context.subscriptions.push(vscode.commands.registerCommand('test.sample.run', (value) => 'ran:' + value));
      return { activated: true };
    };
  `);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(7, transport);
  t.after(() => loader.dispose());

  const result = loader.register([root]);
  assert.equal(result.failed.length, 0);
  assert.equal(result.registered[0].extensionId, 'test.sample');
  assert.deepEqual(result.registered[0].activationEvents.sort(), [
    'onCommand:test.sample.run', 'onView:test.sample.view',
  ]);
  assert.equal(transport.notified('workbench/commands/registerHandler').length, 0);

  const executed = await loader.handleRequest('extension/commands/execute', {
    command: 'test.sample.run', args: ['ok'],
  });
  assert.deepEqual(executed, { value: 'ran:ok' });
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 1);
  assert.equal(transport.notified('workbench/commands/registerHandler').length, 1);

  await loader.dispose();
  assert.equal(transport.notified('workbench/extensions/removeGeneration').length, 1);
});

test('activation failure reaches a terminal state and disposes the API session', async (t) => {
  const root = createExtension({
    name: 'unsafe', publisher: 'test', main: '../outside.cjs',
  });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(3, transport);
  t.after(() => loader.dispose());

  assert.equal(loader.register([root]).failed.length, 0);
  await assert.rejects(loader.activate('test.unsafe'), /escapes extension root/);
  const record = loader.extensions.get('test.unsafe');
  assert.equal(record.state, 'failed');
  assert.equal(record.session, null);
  assert.equal(record.context, null);
  assert.equal(transport.notified('workbench/extensions/didFailActivation').length, 1);
  assert.equal(transport.notified('workbench/extensions/removeGeneration').length, 1);
});

test('a manifest-only extension activates without loading JavaScript', async (t) => {
  const root = createExtension({ name: 'declarative', publisher: 'test' });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(2, transport);
  t.after(() => loader.dispose());

  loader.register([root]);
  assert.deepEqual(await loader.activate('test.declarative', '*'), {});
  assert.equal(loader.extensions.get('test.declarative').state, 'active');
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 1);
});

test('ESM extensions import named vscode exports through the per-extension bridge', async (t) => {
  const root = createExtension({ name: 'esm', publisher: 'test', type: 'module', main: './extension.js',
    contributes: { commands: [{ command: 'test.esm.position', title: 'Position' }] } }, `
    import { commands, Position } from 'vscode';
    export function activate(context) {
      context.subscriptions.push(commands.registerCommand('test.esm.position', () => new Position(2, 3).character));
    }
  `);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(4, transport);
  t.after(() => loader.dispose());
  assert.equal(loader.register([root]).failed.length, 0);
  assert.deepEqual(await loader.handleRequest('extension/commands/execute', {
    command: 'test.esm.position', args: [],
  }), { value: 3 });
});

test('activation runs extension code without asking the workbench for permission', async (t) => {
  const root = createExtension({ name: 'immediate', publisher: 'test', main: './extension.js' }, `
    exports.activate = () => ({ ran: true });
  `);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(5, transport);
  t.after(() => loader.dispose());
  loader.register([root]);
  assert.deepEqual(await loader.activate('test.immediate'), { ran: true });
  assert.equal(loader.extensions.get('test.immediate').state, 'active');
  // VS Code has Workspace Trust, never a per-extension "may this run?" round trip.
  assert.equal(transport.requests.filter((item) => /trust/i.test(item.method)).length, 0);
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 1);
});

test('vscode.extensions exposes the registry and the UI extension kind', async (t) => {
  const observer = createExtension({ name: 'observer', publisher: 'test', main: './extension.js' }, `
    const vscode = require('vscode');
    exports.activate = () => ({
      self: vscode.extensions.getExtension('test.observer')?.extensionKind === vscode.ExtensionKind.UI,
      peer: vscode.extensions.getExtension('TEST.PEER')?.id,
      missing: vscode.extensions.getExtension('test.absent'),
      ids: vscode.extensions.all.map((item) => item.id).sort(),
      peerActive: vscode.extensions.getExtension('test.peer').isActive,
    });
  `);
  const peer = createExtension({ name: 'peer', publisher: 'test', main: './extension.js' }, `
    exports.activate = () => ({});
  `);
  t.after(() => {
    fs.rmSync(observer, { recursive: true, force: true });
    fs.rmSync(peer, { recursive: true, force: true });
  });
  const loader = new ExtensionLoader(8, new RecordingTransport());
  t.after(() => loader.dispose());
  assert.equal(loader.register([observer, peer]).failed.length, 0);
  assert.deepEqual(await loader.activate('test.observer'), {
    self: true,
    peer: 'test.peer',
    missing: undefined,
    ids: ['test.observer', 'test.peer'],
    // An unactivated peer is visible in the registry but is not active yet.
    peerActive: false,
  });
});

test('setStatusBarMessage stacks messages and reveals the one beneath on dispose', async (t) => {
  // Item updates are coalesced onto a microtask, so the extension flushes between
  // steps to make each rendered state individually observable.
  const root = createExtension({ name: 'message', publisher: 'test', main: './extension.js' }, `
    const vscode = require('vscode');
    const flush = () => new Promise((resolve) => setTimeout(resolve, 0));
    exports.activate = async () => {
      const first = vscode.window.setStatusBarMessage('first');
      await flush();
      const second = vscode.window.setStatusBarMessage('second');
      await flush();
      second.dispose();
      await flush();
      first.dispose();
      await flush();
      return {};
    };
  `);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(9, transport);
  t.after(() => loader.dispose());
  loader.register([root]);
  await loader.activate('test.message');
  const rendered = transport.notified('workbench/statusBar/update')
    .map((item) => `${item.params.text}:${item.params.visible}`);
  assert.deepEqual(rendered, ['first:true', 'second:true', 'first:true', 'first:false']);
});

// 実機で odangoo.otak-monitor の activate() が投げ、直列 await ループがそこで打ち切られた
// ため odangoo.otak-usage が 'registered' のまま永久に起動しなかった。上流の
// AbstractExtensionService._activateByEvent は Promise.all で並行に起動し、1 拡張の
// 失敗は _onExtensionActivationError でその拡張だけの失敗として記録される。
test('one extension failing to activate never blocks another for the same event', async (t) => {
  const failing = createExtension({
    name: 'broken', publisher: 'test', version: '1.0.0', main: './extension',
    activationEvents: ['onStartupFinished'],
  }, `exports.activate = () => { throw new Error('boom'); };`);
  const healthy = createExtension({
    name: 'healthy', publisher: 'test', version: '1.0.0', main: './extension',
    activationEvents: ['onStartupFinished'],
  }, `exports.activate = () => ({ ok: true });`);
  t.after(() => {
    fs.rmSync(failing, { recursive: true, force: true });
    fs.rmSync(healthy, { recursive: true, force: true });
  });
  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(3, transport);
  t.after(() => loader.dispose());

  // 壊れたほうを先に登録する。直列 await ループなら後続がここで巻き添えになる。
  assert.equal(loader.register([failing, healthy]).failed.length, 0);

  const result = await loader.activateByEvent('onStartupFinished');
  assert.deepEqual(result.activated, ['test.healthy']);
  assert.equal(result.failed.length, 1);
  assert.equal(result.failed[0].extensionId, 'test.broken');
  assert.match(result.failed[0].message, /boom/);

  assert.equal(loader.extensions.get('test.broken').state, 'failed');
  assert.equal(loader.extensions.get('test.healthy').state, 'active');

  // 失敗は握りつぶさず didFailActivation としてネイティブ側の Extension Host ログへ届く。
  const failures = transport.notified('workbench/extensions/didFailActivation');
  assert.equal(failures.length, 1);
  assert.equal(failures[0].params.extensionId, 'test.broken');
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 1);
});
