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
    if (method === 'workbench/extensions/ensureTrusted') return { trusted: true };
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

test('first activation is blocked before extension code runs when trust is denied', async (t) => {
  const root = createExtension({ name: 'denied', publisher: 'test', main: './extension.js' }, `
    throw new Error('extension code executed');
  `);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  transport.request = async (method, params) => {
    transport.requests.push({ method, params });
    return method === 'workbench/extensions/ensureTrusted' ? { trusted: false } : {};
  };
  const loader = new ExtensionLoader(5, transport);
  t.after(() => loader.dispose());
  loader.register([root]);
  await assert.rejects(loader.activate('test.denied'), /ExtensionTrustDenied: test.denied/);
  assert.equal(loader.extensions.get('test.denied').state, 'blocked');
  assert.equal(loader.extensions.get('test.denied').session, null);
  assert.equal(transport.notified('workbench/extensions/didBlockActivation').length, 1);
});
