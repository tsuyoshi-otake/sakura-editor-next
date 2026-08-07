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
  Disposable,
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
  UnsupportedCapabilityError,
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
  assert.deepEqual(updates[0].params.tooltip,
    { markdown: 'No problems', isTrusted: false, supportThemeIcons: false });

  assert.throws(() => session.api.window.createStatusBarItem('lint.status'), /already exists/);
  item.dispose();
  assert.equal(transport.notified('workbench/statusBar/remove').length, 1);
  const replacement = session.api.window.createStatusBarItem('lint.status');
  replacement.dispose();
  session.dispose();
});

// `$(name)` を「アイコン」として描くか「そのままの文字」として描くかは、
// MarkdownString の supportThemeIcons だけが決める。この値がワイヤーから
// 落ちると、ネイティブ側は意図されたアイコンとリテラルの "$(name)" を
// 区別できなくなるため、必ず送出されること自体を検証する。
test('markdown tooltips carry supportThemeIcons over the wire', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 2, transport);
  const item = session.api.window.createStatusBarItem('icons.status');
  item.tooltip = new MarkdownString('$(rocket) Launch', true);
  item.show();
  await tick();

  const updates = transport.notified('workbench/statusBar/update');
  assert.equal(updates.length, 1);
  assert.deepEqual(updates[0].params.tooltip,
    { markdown: '$(rocket) Launch', isTrusted: false, supportThemeIcons: true });

  item.dispose();
  session.dispose();
});

test('markdown tooltips preserve trusted link permission over the wire', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 3, transport);
  const item = session.api.window.createStatusBarItem('trusted.status');
  const tooltip = new MarkdownString('[Run](command:sample.run)');
  tooltip.isTrusted = true;
  item.tooltip = tooltip;
  item.show();
  await tick();

  const updates = transport.notified('workbench/statusBar/update');
  assert.equal(updates.length, 1);
  assert.deepEqual(updates[0].params.tooltip,
    { markdown: '[Run](command:sample.run)', isTrusted: true, supportThemeIcons: false });

  item.dispose();
  session.dispose();
});

test('markdown tooltips preserve trusted command allowlists over the wire', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 3, transport);
  const item = session.api.window.createStatusBarItem('trusted.allowlist.status');
  const tooltip = new MarkdownString('[Run](command:sample.run)');
  tooltip.isTrusted = { enabledCommands: ['sample.run'] };
  item.tooltip = tooltip;
  item.show();
  await tick();

  const updates = transport.notified('workbench/statusBar/update');
  assert.equal(updates.length, 1);
  assert.deepEqual(updates[0].params.tooltip,
    {
      markdown: '[Run](command:sample.run)',
      isTrusted: { enabledCommands: ['sample.run'] },
      supportThemeIcons: false,
    });

  item.dispose();
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
  assert.match(transport.notified('workbench/output/append')[0].params.operationId, /^output-op-s\d+-g3-\d+$/);

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

test('output mutations carry one bounded, unique operation ID across channels', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.output', 12, transport);
  const first = session.api.window.createOutputChannel('Mutable output name');
  first.append('mutable output text');
  first.flush();
  first.replace('replacement');
  first.clear();
  first.show(2, true);
  first.hide();

  const second = session.api.window.createOutputChannel('Second output');
  second.append('second text');
  second.flush();
  second.dispose();
  first.dispose();

  const mutations = transport.notifications.filter((item) => item.method.startsWith('workbench/output/'));
  assert.deepEqual(new Set(mutations.map((item) => item.method)), new Set([
    'workbench/output/create',
    'workbench/output/append',
    'workbench/output/replace',
    'workbench/output/clear',
    'workbench/output/show',
    'workbench/output/hide',
    'workbench/output/dispose',
  ]));
  const ids = mutations.map((item) => item.params.operationId);
  assert.equal(new Set(ids).size, ids.length);
  for (const id of ids) {
    assert.match(id, /^output-op-s\d+-g12-\d+$/);
    assert.match(id, /^[\x21-\x7e]+$/);
    assert.ok(id.length <= 64);
    assert.equal(id.includes('Mutable output name'), false);
    assert.equal(id.includes('mutable output text'), false);
  }
  for (const { params } of mutations) {
    assert.equal(params.extensionId, 'sample.output');
    assert.equal(params.generation, 12);
    assert.equal(typeof params.handle, 'string');
  }
  assert.equal(transport.notified('workbench/output/show').at(-1).params.preserveFocus, true);
  assert.equal('column' in transport.notified('workbench/output/show').at(-1).params, false);
  assert.equal(transport.notified('workbench/output/replace').at(-1).params.value, 'replacement');
  assert.equal(transport.notified('workbench/output/append')[0].params.handle, first.handle);
  assert.equal(transport.notified('workbench/output/append')[1].params.handle, second.handle);
  session.dispose();
});

test('output mutation IDs remain stable when a transport resends the same notification', () => {
  class RetryingOutputTransport extends RecordingTransport {
    notify(method, params) {
      super.notify(method, params);
      if (method === 'workbench/output/append') super.notify(method, params);
    }
  }

  const transport = new RetryingOutputTransport();
  const session = new ExtensionApiSession('sample.output', 13, transport);
  const output = session.api.window.createOutputChannel('Replay');
  output.append('retry me');
  output.flush();

  const attempts = transport.notified('workbench/output/append');
  assert.equal(attempts.length, 2);
  assert.strictEqual(attempts[0].params, attempts[1].params);
  assert.equal(attempts[0].params.operationId, attempts[1].params.operationId);
  output.dispose();
  session.dispose();
});

test('output operation ID namespaces are session-scoped and fail explicitly on exhaustion', () => {
  const firstTransport = new RecordingTransport();
  const secondTransport = new RecordingTransport();
  const firstSession = new ExtensionApiSession('sample.output', 14, firstTransport);
  const secondSession = new ExtensionApiSession('sample.output', 15, secondTransport);
  firstSession.api.window.createOutputChannel('First generation');
  secondSession.api.window.createOutputChannel('Second generation');
  const firstId = firstTransport.notified('workbench/output/create')[0].params.operationId;
  const secondId = secondTransport.notified('workbench/output/create')[0].params.operationId;
  assert.notEqual(firstId, secondId);
  assert.match(firstId, /^output-op-s\d+-g14-1$/);
  assert.match(secondId, /^output-op-s\d+-g15-1$/);
  firstSession.dispose();
  secondSession.dispose();

  const exhaustedTransport = new RecordingTransport();
  const exhaustedSession = new ExtensionApiSession('sample.output', 16, exhaustedTransport);
  exhaustedSession.nextOutputOperation = Number.MAX_SAFE_INTEGER;
  const exhaustedChannel = exhaustedSession.api.window.createOutputChannel('Bounded');
  const lastId = exhaustedTransport.notified('workbench/output/create')[0].params.operationId;
  assert.ok(lastId.length <= 64);
  assert.throws(() => exhaustedChannel.hide(), /Output operation ID space exhausted/);
  exhaustedSession.dispose();
});

test('SecretStorage is extension-namespaced and fires changes after successful mutations', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('secrets/get', { value: 'secret-value' });
  transport.responses.set('secrets/store', {});
  transport.responses.set('secrets/delete', {});
  const session = new ExtensionApiSession('sample.extension', 5, transport);
  const secrets = session.createExtensionContext().secrets;
  const changes = [];
  const subscription = secrets.onDidChange((event) => changes.push(event.key));

  assert.equal(await secrets.get('token'), 'secret-value');
  await secrets.store('token', 'new-secret');
  await assert.rejects(secrets.keys(), (error) => {
    assert.ok(error instanceof UnsupportedCapabilityError);
    assert.equal(error.code, 'UnsupportedCapability');
    assert.equal(error.extensionId, 'sample.extension');
    assert.equal(error.capability, 'SecretStorage.keys');
    return true;
  });
  await secrets.delete('token');
  await session.handleRequest('extension/secrets/didChange', { key: 'remote' });
  assert.deepEqual(changes, ['token', 'token', 'remote']);
  for (const request of transport.requests) assert.equal(request.params.extensionId, 'sample.extension');
  assert.equal(transport.requests.some((request) => request.method === 'secrets/keys'), false);

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

test('config.update() confirms the native write before mutating the local cache or firing change events', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.settings', 3, transport, {
    configurationDefaults: { 'sample.retries': 1 },
  });
  const api = session.api;
  const config = api.workspace.getConfiguration('sample');
  let changeCount = 0;
  api.workspace.onDidChangeConfiguration(() => { changeCount += 1; });

  await config.update('retries', 5, true, 'plaintext');
  const request = transport.requests.at(-1);
  assert.equal(request.method, 'workspace/configuration/update');
  assert.equal(request.params.key, 'sample.retries');
  assert.equal(request.params.value, 5);
  assert.equal(request.params.configurationTarget, true);
  assert.equal(request.params.overrideInLanguage, 'plaintext');
  assert.equal(config.get('retries'), 5);
  assert.equal(changeCount, 1);

  // value === undefined must delete the key and must serialize to an absent
  // "value" property (never a literal null) so the native side can tell
  // "remove this setting" apart from "set it to null".
  await config.update('retries', undefined, true);
  const deleteRequest = transport.requests.at(-1);
  assert.equal(Object.prototype.hasOwnProperty.call(
    JSON.parse(JSON.stringify(deleteRequest.params)), 'value'), false);
  assert.equal(config.get('retries'), 1); // falls back to configurationDefaults
  assert.equal(changeCount, 2);

  session.dispose();
});

test('config.update() rejection leaves the local cache and change events untouched', async () => {
  const transport = new RecordingTransport();
  transport.responses.set('workspace/configuration/update', () => {
    throw new Error('UnsupportedConfigurationTarget: workspace settings are not writable yet');
  });
  const session = new ExtensionApiSession('sample.settings', 4, transport, {
    configurationDefaults: { 'sample.retries': 1 },
  });
  const api = session.api;
  const config = api.workspace.getConfiguration('sample');
  let changeCount = 0;
  api.workspace.onDidChangeConfiguration(() => { changeCount += 1; });

  await assert.rejects(() => config.update('retries', 9, false), /UnsupportedConfigurationTarget/);
  assert.equal(config.get('retries'), 1);
  assert.equal(changeCount, 0);

  session.dispose();
});

// 実機で odangoo.otak-monitor が activate() 中に `vscode.window.state.focused` を読んで
// TypeError で落ちていた。上流の ExtHostWindow は `_state` を InitialState
// ({ focused: true, active: true }) で初期化するので、`window.state` が undefined に
// なる瞬間は存在しない。
test('window.state always exists and tracks the native window state notification', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.window', 3, transport);
  const api = session.api;

  assert.deepEqual(api.window.state, { focused: true, active: true });

  const seen = [];
  api.window.onDidChangeWindowState((state) => {
    // ハンドラの中から読んだ window.state は、通知された値と必ず一致する。
    seen.push({ event: state, current: api.window.state });
  });

  assert.deepEqual(await session.handleRequest('extension/window/didChangeState', {
    focused: false, active: true,
  }), { accepted: true });
  assert.deepEqual(api.window.state, { focused: false, active: true });
  assert.equal(seen.length, 1);
  assert.deepEqual(seen[0].event, { focused: false, active: true });
  assert.deepEqual(seen[0].current, { focused: false, active: true });

  await session.handleRequest('extension/window/didChangeState', { focused: true, active: false });
  assert.deepEqual(api.window.state, { focused: true, active: false });

  session.dispose();
});

// workspace.isTrusted is fail-closed: absent/non-boolean options never grant trust, and
// onDidGrantWorkspaceTrust only fires on the untrusted -> trusted transition, matching
// upstream (there is no downstream "revoke" event).
test('workspace.isTrusted defaults to false when workspaceTrusted is not specified', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 5, transport);
  assert.equal(session.api.workspace.isTrusted, false);
  session.dispose();
});

test('workspace.isTrusted is true when the session is constructed with workspaceTrusted: true', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 6, transport, { workspaceTrusted: true });
  assert.equal(session.api.workspace.isTrusted, true);
  session.dispose();
});

test('workspace.isTrusted stays false for a non-boolean truthy workspaceTrusted value', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 7, transport, { workspaceTrusted: 'yes' });
  assert.equal(session.api.workspace.isTrusted, false);
  session.dispose();
});

test('didChangeTrust flips isTrusted and fires onDidGrantWorkspaceTrust exactly once on the untrusted -> trusted transition', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 8, transport);
  const api = session.api;
  assert.equal(api.workspace.isTrusted, false);

  const seen = [];
  api.workspace.onDidGrantWorkspaceTrust(() => {
    // ハンドラの中から読んだ isTrusted は、通知より先にフィールドが更新されているので
    // 必ず true になる。
    seen.push(api.workspace.isTrusted);
  });

  assert.deepEqual(await session.handleRequest('extension/workspace/didChangeTrust', { trusted: true }), { accepted: true });
  assert.equal(api.workspace.isTrusted, true);
  assert.equal(seen.length, 1);
  assert.equal(seen[0], true);

  session.dispose();
});

test('didChangeTrust does not re-fire onDidGrantWorkspaceTrust when the session is already trusted', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 9, transport, { workspaceTrusted: true });
  const api = session.api;
  assert.equal(api.workspace.isTrusted, true);

  let fireCount = 0;
  api.workspace.onDidGrantWorkspaceTrust(() => { fireCount += 1; });

  assert.deepEqual(await session.handleRequest('extension/workspace/didChangeTrust', { trusted: true }), { accepted: true });
  assert.equal(api.workspace.isTrusted, true);
  assert.equal(fireCount, 0);

  session.dispose();
});

test('onDidGrantWorkspaceTrust returns a real Disposable that stops delivering events once disposed', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 10, transport);
  const api = session.api;

  let fireCount = 0;
  const disposable = api.workspace.onDidGrantWorkspaceTrust(() => { fireCount += 1; });
  assert.ok(disposable instanceof Disposable);

  disposable.dispose();
  assert.deepEqual(await session.handleRequest('extension/workspace/didChangeTrust', { trusted: true }), { accepted: true });
  assert.equal(api.workspace.isTrusted, true);
  assert.equal(fireCount, 0);

  session.dispose();
});

// 上流に revoke イベントは無い（上流は信頼降格時に拡張ホストごと再起動するため、降格を
// live なセッションへ伝える手段自体が存在しない）。降格でイベントを捏造しないことの回帰。
test('didChangeTrust with trusted: false demotes isTrusted without firing onDidGrantWorkspaceTrust', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 11, transport, { workspaceTrusted: true });
  const api = session.api;
  assert.equal(api.workspace.isTrusted, true);

  let fireCount = 0;
  api.workspace.onDidGrantWorkspaceTrust(() => { fireCount += 1; });

  assert.deepEqual(await session.handleRequest('extension/workspace/didChangeTrust', { trusted: false }), { accepted: true });
  assert.equal(api.workspace.isTrusted, false);
  assert.equal(fireCount, 0);

  session.dispose();
});

test('didChangeTrust treats a non-boolean truthy trusted value as false', async () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.trust', 12, transport);
  const api = session.api;

  assert.deepEqual(await session.handleRequest('extension/workspace/didChangeTrust', { trusted: 'yes' }), { accepted: true });
  assert.equal(api.workspace.isTrusted, false);

  session.dispose();
});

test('the Terminal API surface reports UnsupportedCapability instead of being absent', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 11, transport);
  const api = session.api;

  const expectUnsupported = (capability, run) => {
    assert.throws(run, (error) => {
      assert.ok(error instanceof UnsupportedCapabilityError, `${capability} threw ${error}`);
      assert.equal(error.code, 'UnsupportedCapability');
      assert.equal(error.extensionId, 'sample.extension');
      assert.equal(error.capability, capability);
      return true;
    });
  };

  expectUnsupported('window.createTerminal', () => api.window.createTerminal({ name: 'Claude Code' }));
  // ネイティブのターミナルは実在するので、`[]` や `undefined` を返すのは
  // 「存在しない」という誤った事実表明になる。読み取り自体を型付きで失敗させる。
  expectUnsupported('window.terminals', () => api.window.terminals);
  expectUnsupported('window.activeTerminal', () => api.window.activeTerminal);
  expectUnsupported('window.registerTerminalLinkProvider', () => api.window.registerTerminalLinkProvider({}));
  expectUnsupported('window.registerTerminalQuickFixProvider', () => api.window.registerTerminalQuickFixProvider('id', {}));

  // 発火しない購読を返すのではなく、購読した時点で失敗させる。
  for (const name of ['onDidOpenTerminal', 'onDidCloseTerminal', 'onDidChangeActiveTerminal',
    'onDidChangeTerminalState', 'onDidChangeTerminalShellIntegration', 'onDidStartTerminalShellExecution',
    'onDidEndTerminalShellExecution', 'onDidWriteTerminalData']) {
    expectUnsupported(`window.${name}`, () => api.window[name](() => {}));
  }

  assert.deepEqual(transport.notifications, []);
  session.dispose();
});

test('registerTerminalProfileProvider names its own unsupported capability on the wire', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 12, transport);

  // registerWebviewViewProvider と同じ扱い: activation を殺さず、ネイティブ側の
  // Extension Compatibility 出力へ未対応として届ける。
  const disposable = session.api.window.registerTerminalProfileProvider('sample.profile', {
    provideTerminalProfile() { return undefined; },
  });

  const [notification] = transport.notified('workbench/terminal/registerProfileProvider');
  assert.deepEqual(notification.params, {
    id: 'sample.profile',
    extensionId: 'sample.extension',
    generation: 12,
    // capability を payload で名乗るので、ネイティブ側が method の接頭辞から
    // 名前を推測する必要がなくなる。
    error: { code: 'UnsupportedCapability', capability: 'window.registerTerminalProfileProvider' },
  });

  disposable.dispose();
  assert.equal(transport.notified('workbench/terminal/unregisterProfileProvider').length, 1);
  session.dispose();
});

test('ExtensionContext.environmentVariableCollection is a typed unsupported namespace', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 13, transport);
  const collection = session.createExtensionContext().environmentVariableCollection;

  assert.notEqual(collection, undefined);
  for (const [member, run] of [
    ['replace', () => collection.replace('CLAUDE_CODE_SSE_PORT', '1234')],
    ['append', () => collection.append('PATH', ';C:\tools')],
    ['prepend', () => collection.prepend('PATH', 'C:\tools;')],
    ['clear', () => collection.clear()],
    ['getScoped', () => collection.getScoped({})],
    ['persistent', () => collection.persistent],
  ]) {
    assert.throws(run, (error) => {
      assert.ok(error instanceof UnsupportedCapabilityError, `${member} threw ${error}`);
      assert.equal(error.capability, `ExtensionContext.environmentVariableCollection.${member}`);
      assert.equal(error.extensionId, 'sample.extension');
      return true;
    });
  }

  assert.deepEqual(transport.notifications, []);
  session.dispose();
});

test('Terminal value enums match upstream VS Code', () => {
  const transport = new RecordingTransport();
  const session = new ExtensionApiSession('sample.extension', 14, transport);
  const api = session.api;

  assert.deepEqual({ ...api.TerminalLocation }, { Panel: 1, Editor: 2 });
  assert.deepEqual({ ...api.TerminalExitReason }, { Unknown: 0, Shutdown: 1, Process: 2, User: 3, Extension: 4 });
  assert.deepEqual({ ...api.EnvironmentVariableMutatorType }, { Replace: 1, Append: 2, Prepend: 3 });
  assert.deepEqual({ ...api.TerminalShellExecutionCommandLineConfidence }, { Low: 0, Medium: 1, High: 2 });
  assert.ok(Object.isFrozen(api.TerminalLocation));

  session.dispose();
});
