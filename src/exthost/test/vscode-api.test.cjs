'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const {
  ExtensionApiSession,
  Diagnostic,
  DiagnosticSeverity,
  EndOfLine,
  MarkdownString,
  Position,
  Range,
  Selection,
  StatusBarAlignment,
  TextEdit,
  ThemeColor,
  TreeItem,
  TreeItemCollapsibleState,
  Uri,
  WorkspaceEdit,
} = require('../src/vscode-api.cjs');

class RecordingTransport {
  constructor() {
    this.notifications = [];
    this.requests = [];
    this.responses = new Map();
  }

  notify(method, params) {
    this.notifications.push({ method, params });
  }

  async request(method, params) {
    this.requests.push({ method, params });
    const response = this.responses.get(method);
    return typeof response === 'function' ? response(params) : response;
  }

  notified(method) {
    return this.notifications.filter((item) => item.method === method);
  }
}

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

test('commands register, execute, set context, and dispose with extension ownership', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('workbench/commands/list', { commands: ['sakura.file.open', '_internal'] });
  const session = new ExtensionApiSession('Sample.Extension', 4, transport);
  const api = session.api;
  const owner = { prefix: 'ok' };
  const disposable = api.commands.registerCommand('sample.run', function (value) {
    return `${this.prefix}:${value}`;
  }, owner);

  assert.equal(await api.commands.executeCommand('sample.run', 7), 'ok:7');
  assert.deepEqual(await session.handleRequest('extension/commands/execute', {
    command: 'sample.run', args: ['remote'],
  }), { value: 'ok:remote' });
  await api.commands.executeCommand('setContext', 'sample.enabled', true);
  assert.deepEqual(transport.requests.at(-1), {
    method: 'workbench/context/set',
    params: { key: 'sample.enabled', value: true, extensionId: 'sample.extension', generation: 4 },
  });
  assert.deepEqual(await api.commands.getCommands(true), ['sakura.file.open', 'sample.run']);
  assert.throws(() => api.commands.registerCommand('sample.run', () => {}), /already registered/);
  disposable.dispose();
  assert.equal(transport.notified('workbench/commands/unregisterHandler').length, 1);
  session.dispose();
});

test('every extension request carries non-spoofable ownership metadata', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('sample/request', { accepted: true });
  const session = new ExtensionApiSession('Sample.Extension', 4, transport);

  assert.deepEqual(await session.request('sample/request', {
    extensionId: 'spoofed.extension', generation: 99, value: 7,
  }), { accepted: true });
  assert.deepEqual(transport.requests.at(-1), {
    method: 'sample/request',
    params: { extensionId: 'sample.extension', generation: 4, value: 7 },
  });
  session.dispose();
});

test('status bar item preserves VS Code overloads, priority, visibility, and disposal', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 2, transport);
  const item = session.api.window.createStatusBarItem('lint.status', StatusBarAlignment.Right, 50);
  item.name = 'Lint Status';
  item.text = '$(check) Clean';
  item.tooltip = new MarkdownString('No problems');
  item.color = new ThemeColor('statusBar.foreground');
  item.command = { command: 'lint.show', title: 'Show lint output', arguments: [1] };
  item.show();
  await tick();

  const updates = transport.notified('workbench/statusBar/update');
  assert.equal(updates.length, 1);
  assert.equal(updates[0].params.itemId, 'lint.status');
  assert.equal(updates[0].params.alignment, 'right');
  assert.equal(updates[0].params.priority, 50);
  assert.equal(updates[0].params.visible, true);
  assert.deepEqual(updates[0].params.color, { themeColor: 'statusBar.foreground' });
  assert.deepEqual(updates[0].params.tooltip, { markdown: 'No problems', isTrusted: false });

  assert.throws(() => session.api.window.createStatusBarItem('lint.status'), /already exists/);
  item.dispose();
  assert.equal(transport.notified('workbench/statusBar/remove').length, 1);
  const replacement = session.api.window.createStatusBarItem('lint.status');
  replacement.dispose();
  session.dispose();
});

test('notifications and quick input return the original selected values', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('workbench/notification/show', { selectedIndex: 1 });
  transport.responses.set('workbench/quickInput/showQuickPick', { selectedIndices: [1, 0] });
  transport.responses.set('workbench/quickInput/showInputBox', { value: '' });
  const session = new ExtensionApiSession('sample.extension', 1, transport);
  const action = { title: 'Details', isCloseAffordance: true, metadata: 7 };
  const selected = await session.api.window.showWarningMessage('Problem', { modal: true, detail: 'Details' }, 'Ignore', action);
  assert.equal(selected, action);
  assert.equal(transport.requests[0].params.modal, true);
  assert.deepEqual(transport.requests[0].params.actions, [
    { title: 'Ignore' }, { title: 'Details', isCloseAffordance: true },
  ]);

  const items = [{ label: 'First', value: 1 }, { label: 'Second', value: 2 }];
  const picked = await session.api.window.showQuickPick(items, { canPickMany: true });
  assert.deepEqual(picked, [items[1], items[0]]);
  assert.equal(await session.api.window.showInputBox({ prompt: 'Value' }), '');
  session.dispose();
});

test('output batches writes and progress always reaches an explicit end', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 3, transport);
  const output = session.api.window.createOutputChannel('Extension');
  output.append('one');
  output.appendLine('two');
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(transport.notified('workbench/output/append').length, 1);
  assert.equal(transport.notified('workbench/output/append')[0].params.value, 'onetwo\n');

  await assert.rejects(session.api.window.withProgress({ title: 'Work', cancellable: true }, async (progress) => {
    progress.report({ increment: 50, message: 'half' });
    throw new Error('failure');
  }), /failure/);
  assert.equal(transport.notified('workbench/progress/start').length, 1);
  assert.equal(transport.notified('workbench/progress/report').length, 1);
  assert.equal(transport.notified('workbench/progress/end').length, 1);
  output.dispose();
  session.dispose();
});

test('SecretStorage is extension-namespaced and fires changes after successful mutations', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('secrets/get', { value: 'secret-value' });
  transport.responses.set('secrets/store', {});
  transport.responses.set('secrets/delete', {});
  transport.responses.set('secrets/keys', { keys: ['token'] });
  const session = new ExtensionApiSession('sample.extension', 5, transport);
  const secrets = session.createExtensionContext().secrets;
  const changes = [];
  const subscription = secrets.onDidChange((event) => changes.push(event.key));

  assert.equal(await secrets.get('token'), 'secret-value');
  await secrets.store('token', 'new-secret');
  assert.deepEqual(await secrets.keys(), ['token']);
  await secrets.delete('token');
  await session.handleRequest('extension/secrets/didChange', { key: 'remote' });
  assert.deepEqual(changes, ['token', 'token', 'remote']);
  for (const request of transport.requests) assert.equal(request.params.extensionId, 'sample.extension');

  subscription.dispose();
  session.dispose();
});

test('TreeView preserves element identity, refresh, selection, visibility, reveal, and disposal', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('workbench/views/reveal', { accepted: true });
  const session = new ExtensionApiSession('sample.extension', 6, transport);
  const root = { name: 'Root' };
  const child = { name: 'Child' };
  const changes = new session.api.EventEmitter();
  const provider = {
    onDidChangeTreeData: changes.event,
    getChildren(element) { return element === root ? [child] : element ? [] : [root]; },
    getTreeItem(element) {
      const item = new TreeItem(element.name, element === root
        ? TreeItemCollapsibleState.Expanded : TreeItemCollapsibleState.None);
      item.id = element.name.toLowerCase();
      item.contextValue = element === root ? 'folder' : 'file';
      item.command = element === child ? { command: 'sample.open', title: 'Open', arguments: [1] } : undefined;
      return item;
    },
  };
  const view = session.api.window.createTreeView('sample.view', {
    treeDataProvider: provider, canSelectMany: true, showCollapseAll: true,
  });
  assert.equal(transport.notified('workbench/views/register').length, 1);

  const roots = await session.handleRequest('extension/views/getChildren', { handle: view.handle });
  assert.equal(roots.items.length, 1);
  assert.equal(roots.items[0].label, 'Root');
  assert.equal(roots.items[0].collapsibleState, TreeItemCollapsibleState.Expanded);
  const rootHandle = roots.items[0].handle;
  const children = await session.handleRequest('extension/views/getChildren', {
    handle: view.handle, parentHandle: rootHandle,
  });
  assert.equal(children.items[0].label, 'Child');
  assert.equal(children.items[0].parentHandle, rootHandle);
  const childHandle = children.items[0].handle;

  const selections = [];
  const visibility = [];
  view.onDidChangeSelection((event) => selections.push(event.selection));
  view.onDidChangeVisibility((event) => visibility.push(event.visible));
  await session.handleRequest('extension/views/didSelect', {
    handle: view.handle, itemHandles: [childHandle],
  });
  await session.handleRequest('extension/views/didChangeVisibility', { handle: view.handle, visible: true });
  assert.deepEqual(selections, [[child]]);
  assert.deepEqual(visibility, [true]);

  changes.fire(root);
  assert.equal(transport.notified('workbench/views/refresh').at(-1).params.itemHandle, rootHandle);
  await view.reveal(child, { select: true, focus: true, expand: 2 });
  assert.equal(transport.requests.at(-1).params.itemHandle, childHandle);

  view.dispose();
  assert.equal(transport.notified('workbench/views/unregister').length, 1);
  await assert.rejects(session.handleRequest('extension/views/getChildren', { handle: view.handle }), /not registered/);
  changes.dispose();
  session.dispose();
});

test('base document types preserve positions, ranges, line indexing, URI paths, and editor state', async () => {
  const transport = new RecordingTransport();
  const filePath = path.join(os.tmpdir(), 'sakura-document.md');
  const session = new ExtensionApiSession('sample.document', 7, transport, {
    documents: [{ documentId: '42:1', uri: Uri.file(filePath).toString(), fileName: filePath,
      languageId: 'markdown', version: 3, text: 'first\r\nsecond line\n', eol: EndOfLine.CRLF }],
    activeDocumentId: '42:1',
  });
  const document = session.api.workspace.textDocuments[0];
  assert.equal(document.lineCount, 3);
  assert.equal(document.lineAt(1).text, 'second line');
  assert.equal(document.offsetAt(new Position(1, 3)), 10);
  assert.deepEqual(document.positionAt(10), new Position(1, 3));
  assert.equal(document.getText(new Range(1, 0, 1, 6)), 'second');
  assert.equal(document.uri.fsPath.toLowerCase(), filePath.toLowerCase());
  assert.equal(new Selection(1, 4, 1, 1).isReversed, true);

  session.api.window.activeTextEditor.options = { tabSize: 2, insertSpaces: true };
  const editorOptions = transport.notified('window/editor/setOptions').at(-1).params;
  assert.deepEqual(editorOptions.options, { tabSize: 2, insertSpaces: true });
  assert.equal(editorOptions.extensionId, 'sample.document');
  assert.equal(editorOptions.generation, 7);
  session.dispose();
});

test('workspace lifecycle, will-save edits, versioned applyEdit, and providers have explicit outcomes', async () => {
  const transport = new RecordingTransport();
  const uri = Uri.file(path.join(os.tmpdir(), 'sakura-workspace.txt'));
  const snapshot = { documentId: '50:2', uri: uri.toString(), languageId: 'plaintext', version: 1, text: 'bad  ', eol: 1 };
  const session = new ExtensionApiSession('sample.workspace', 8, transport, { documents: [snapshot], activeDocumentId: '50:2' });
  const api = session.api;
  const events = [];
  api.workspace.onDidChangeTextDocument((event) => events.push(`change:${event.document.version}`));
  api.workspace.onDidSaveTextDocument((document) => events.push(`save:${document.version}`));
  api.workspace.onWillSaveTextDocument((event) => event.waitUntil(Promise.resolve([
    TextEdit.delete(new Range(0, 3, 0, 5)),
  ])));
  await session.handleRequest('extension/workspace/didChange', { snapshot: { ...snapshot, version: 2, text: 'good  ' } });
  const willSave = await session.handleRequest('extension/workspace/willSave', { documentId: '50:2', reason: 1 });
  assert.equal(willSave.expectedVersion, 2);
  assert.equal(willSave.edits.length, 1);
  await session.handleRequest('extension/workspace/didSave', { snapshot: { ...snapshot, version: 2, text: 'good', isDirty: false } });
  assert.deepEqual(events, ['change:2', 'save:2']);

  transport.responses.set('workspace/applyEdit', { applied: false, reason: 'VersionMismatch' });
  const edit = new WorkspaceEdit();
  edit.insert(uri, new Position(0, 0), 'x');
  assert.equal(await api.workspace.applyEdit(edit), false);
  assert.equal(transport.requests.at(-1).params.expectedVersions['50:2'], 2);

  api.languages.registerDocumentFormattingEditProvider('plaintext', {
    provideDocumentFormattingEdits(document) { return [TextEdit.replace(new Range(0, 0, 0, 4), document.getText().toUpperCase())]; },
  });
  const provided = await session.handleRequest('extension/languages/provide', { kind: 'formatDocument', documentId: '50:2', options: {} });
  assert.equal(provided.expectedVersion, 2);
  assert.equal(provided.value[0].newText, 'GOOD');
  session.dispose();
});

test('coalesced full snapshots recover skipped native document versions without a gap request', async () => {
  const transport = new RecordingTransport();
  const snapshot = { documentId: '60:1', uri: Uri.file(path.join(os.tmpdir(), 'coalesced.txt')).toString(),
    languageId: 'plaintext', version: 1, text: 'one' };
  const session = new ExtensionApiSession('sample.coalesced', 10, transport, { documents: [snapshot] });
  await session.handleRequest('extension/workspace/didChange', {
    snapshot: { ...snapshot, version: 4, text: 'four' }, snapshotOnly: true, coalescedChanges: 3,
  });
  assert.equal(session.api.workspace.textDocuments[0].version, 4);
  assert.equal(session.api.workspace.textDocuments[0].getText(), 'four');
  assert.equal(transport.notified('workspace/document/versionGap').length, 0);
  session.dispose();
});

test('non-coalesced version gaps request an owned full snapshot recovery', async () => {
  const transport = new RecordingTransport();
  const snapshot = { documentId: '61:1', uri: Uri.file(path.join(os.tmpdir(), 'gap.txt')).toString(),
    languageId: 'plaintext', version: 1, text: 'one' };
  const session = new ExtensionApiSession('sample.gap', 11, transport, { documents: [snapshot] });
  await session.handleRequest('extension/workspace/didChange', {
    snapshot: { ...snapshot, version: 4, text: 'four' }, snapshotOnly: false,
  });
  const request = transport.notified('workspace/document/versionGap').at(-1).params;
  assert.equal(request.documentId, '61:1');
  assert.equal(request.expectedVersion, 2);
  assert.equal(request.actualVersion, 4);
  assert.equal(request.extensionId, 'sample.gap');
  assert.equal(request.generation, 11);
  session.dispose();
});

test('diagnostics, configuration defaults, state, and file-system API retain extension ownership', async (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-vscode-api-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.state', 9, transport, {
    configurationDefaults: { 'sample.enabled': true, 'sample.patterns': [] },
  });
  const api = session.api;
  const config = api.workspace.getConfiguration('sample');
  assert.equal(config.enabled, true);
  assert.deepEqual({ ...config }.patterns, []);

  const context = session.createExtensionContext({ globalStoragePath: root, extensionPath: root });
  await context.globalState.update('count', 2);
  const reloaded = session.createExtensionContext({ globalStoragePath: root, extensionPath: root });
  assert.equal(reloaded.globalState.get('count'), 2);

  const file = Uri.file(path.join(root, 'nested', 'value.txt'));
  await api.workspace.fs.writeFile(file, Buffer.from('value'));
  assert.equal(Buffer.from(await api.workspace.fs.readFile(file)).toString(), 'value');

  const diagnostics = api.languages.createDiagnosticCollection('sample');
  const diagnostic = new Diagnostic(new Range(0, 0, 0, 5), 'problem', DiagnosticSeverity.Warning);
  diagnostic.source = 'sample.state';
  diagnostics.set(file, [diagnostic]);
  assert.equal(transport.notified('languages/diagnostics/set').at(-1).params.diagnostics[0].severity,
    DiagnosticSeverity.Warning);
  diagnostics.dispose();
  session.dispose();
});
