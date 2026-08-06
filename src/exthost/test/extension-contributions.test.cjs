'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const {
  ExtensionLoader,
  acknowledgedContributions,
  contributionKeybindings,
  contributionLanguages,
  contributionMenus,
  contributionSubmenus,
  contributionViews,
  contributionViewsContainers,
  globToRegExp,
  workspaceContainsMatches,
} = require('../src/extension-loader.cjs');

class RecordingTransport {
  constructor() {
    this.notifications = [];
  }

  notify(method, params) {
    this.notifications.push({ method, params });
  }

  async request() {
    return {};
  }

  notified(method) {
    return this.notifications.filter((item) => item.method === method);
  }
}

function createExtension(manifest, files = {}) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-contrib-'));
  fs.writeFileSync(path.join(root, 'package.json'), JSON.stringify(manifest));
  for (const [relative, content] of Object.entries(files)) {
    const target = path.join(root, relative);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, content);
  }
  return root;
}

/*
  Claude Code for VS Code (Anthropic.claude-code 2.1.223) の contributes を写したもの。
  この拡張が動かないことが今回の作業の発端なので、形の再現をここに固定する。
*/
const CLAUDE_CODE_LIKE = {
  name: 'claude-code',
  publisher: 'anthropic',
  version: '2.1.223',
  displayName: 'Claude Code for VS Code',
  activationEvents: ['onStartupFinished', 'onWebviewPanel:claudeVSCodePanel'],
  contributes: {
    commands: [
      { command: 'claude-code.run', title: 'Run Claude', category: 'Claude Code' },
      { command: 'claude-code.focus', title: 'Focus Claude' },
    ],
    viewsContainers: {
      activitybar: [{ id: 'claude-code', title: 'Claude Code', icon: 'resources/claude.svg' }],
    },
    views: {
      'claude-code': [{ id: 'claude-code.sidebar', name: 'Claude', type: 'webview' }],
    },
    menus: {
      'editor/title': [{ command: 'claude-code.run', when: 'editorIsOpen', group: 'navigation@1' }],
      commandPalette: [{ command: 'claude-code.focus', when: 'claudeReady' }],
    },
    keybindings: [
      { command: 'claude-code.run', key: 'ctrl+escape', mac: 'cmd+escape', when: 'editorTextFocus' },
    ],
    jsonValidation: [{ fileMatch: '.claude/settings.json', url: './schema.json' }],
    walkthroughs: [{ id: 'claude-code.welcome', title: 'Welcome', steps: [] }],
  },
};

test('carries the full Claude Code contribution surface into register metadata', (t) => {
  const root = createExtension(CLAUDE_CODE_LIKE);
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const loader = new ExtensionLoader(1, new RecordingTransport());
  t.after(() => loader.dispose());

  const { registered, failed } = loader.register([root]);
  assert.deepEqual(failed, []);
  const metadata = registered[0];

  assert.equal(metadata.extensionId, 'anthropic.claude-code');
  assert.equal(metadata.extensionPath, fs.realpathSync.native(root));

  // サイドバーは webview。tree として登録されると「登録は成功したのに永久に空」になる。
  assert.deepEqual(metadata.views, [{
    id: 'claude-code.sidebar',
    name: 'Claude',
    containerId: 'claude-code',
    type: 'webview',
    when: '',
    icon: '',
    contextualTitle: '',
    visibility: '',
  }]);

  assert.deepEqual(metadata.viewsContainers, [{
    id: 'claude-code', title: 'Claude Code', location: 'activitybar', icon: 'resources/claude.svg', codicon: '',
  }]);

  assert.deepEqual(metadata.menus['editor/title'], [{
    command: 'claude-code.run', submenu: '', alt: '', when: 'editorIsOpen', group: 'navigation@1',
  }]);
  assert.equal(metadata.menus.commandPalette.length, 1);

  assert.deepEqual(metadata.keybindings, [{
    command: 'claude-code.run', key: 'ctrl+escape', when: 'editorTextFocus',
  }]);

  // 未実装だが宣言自体は正常なものは、握り潰さず「受理済み」として明示する。
  assert.deepEqual(metadata.acknowledgedContributions.sort(), ['jsonValidation', 'walkthroughs']);

  assert.ok(metadata.activationEvents.includes('onWebviewPanel:claudeVSCodePanel'));
});

test('defaults a view without an explicit type to a tree view', () => {
  const views = contributionViews({ contributes: { views: { explorer: [{ id: 'a.view', name: 'A' }] } } });
  assert.equal(views[0].type, 'tree');
});

test('prefers the Windows keybinding override and drops entries without any key', () => {
  const keybindings = contributionKeybindings({
    contributes: {
      keybindings: [
        { command: 'a.win', key: 'ctrl+k', win: 'ctrl+alt+k' },
        { command: 'a.mac-only', mac: 'cmd+k' },
        { key: 'ctrl+j' },
        { command: 'a.args', key: 'ctrl+l', args: { value: 1 } },
      ],
    },
  });
  assert.deepEqual(keybindings, [
    { command: 'a.win', key: 'ctrl+alt+k', when: '' },
    { command: 'a.args', key: 'ctrl+l', when: '', args: { value: 1 } },
  ]);
});

test('splits codicon references from image paths and keeps the container location', () => {
  const containers = contributionViewsContainers({
    contributes: {
      viewsContainers: {
        activitybar: [{ id: 'a', title: 'A', icon: '$(beaker)' }],
        panel: [{ id: 'b', title: 'B', icon: { dark: 'dark.svg', light: 'light.svg' } }],
      },
    },
  });
  assert.deepEqual(containers, [
    { id: 'a', title: 'A', location: 'activitybar', icon: '', codicon: 'beaker' },
    { id: 'b', title: 'B', location: 'panel', icon: 'dark.svg', codicon: '' },
  ]);
});

test('drops menu items that can neither run a command nor open a submenu', () => {
  const menus = contributionMenus({
    contributes: {
      menus: {
        'editor/context': [
          { command: 'a.run' },
          { submenu: 'a.more', group: 'z' },
          { when: 'always' },
        ],
        'view/title': [{ when: 'never' }],
      },
    },
  });
  assert.equal(menus['editor/context'].length, 2);
  // 有効な項目が 1 つも無い面は、そもそも面ごと出さない。
  assert.equal(menus['view/title'], undefined);
  assert.deepEqual(contributionSubmenus({ contributes: { submenus: [{ id: 'a.more', label: 'More' }] } }), [
    { id: 'a.more', label: 'More', icon: '' },
  ]);
});

test('resolves contributed file paths and refuses ones that escape the extension root', (t) => {
  const root = createExtension({
    name: 'paths',
    publisher: 'test',
    contributes: {
      snippets: [
        { language: 'javascript', path: './snippets/js.json' },
        { language: 'evil', path: '../../../etc/passwd' },
        { language: 'missing', path: './snippets/absent.json' },
      ],
      languages: [
        { id: 'sakura', extensions: ['.skr'], aliases: ['Sakura'], configuration: './language-configuration.json' },
        { id: 'escaped', configuration: '../outside.json' },
      ],
    },
  }, {
    'snippets/js.json': '{}',
    'language-configuration.json': '{}',
  });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const loader = new ExtensionLoader(1, new RecordingTransport());
  t.after(() => loader.dispose());

  const metadata = loader.register([root]).registered[0];
  assert.equal(metadata.snippets.length, 1);
  assert.equal(metadata.snippets[0].language, 'javascript');
  assert.equal(metadata.snippets[0].path, path.join(fs.realpathSync.native(root), 'snippets', 'js.json'));

  const [sakura, escaped] = metadata.languages;
  assert.deepEqual(sakura.extensions, ['.skr']);
  assert.deepEqual(sakura.aliases, ['Sakura']);
  assert.equal(sakura.configuration, path.join(fs.realpathSync.native(root), 'language-configuration.json'));
  assert.equal(escaped.configuration, '');
});

test('parses language identifiers even when every optional field is absent', () => {
  assert.deepEqual(contributionLanguages({ contributes: { languages: [{ id: 'plain' }, { name: 'no id' }] } }), [{
    id: 'plain',
    aliases: [],
    extensions: [],
    filenames: [],
    filenamePatterns: [],
    mimetypes: [],
    firstLine: '',
    icon: '',
    configuration: '',
  }]);
});

test('reports only the acknowledged contributions the manifest actually declares', () => {
  assert.deepEqual(acknowledgedContributions({ contributes: { grammars: [], commands: [] } }), ['grammars']);
  assert.deepEqual(acknowledgedContributions({}), []);
});

test('translates workspaceContains globs without over-matching', () => {
  assert.ok(globToRegExp('**/.eslintrc').test('packages/app/.eslintrc'));
  assert.ok(globToRegExp('**/.eslintrc').test('.eslintrc'));
  assert.ok(globToRegExp('*.sln').test('sakura.sln'));
  assert.ok(!globToRegExp('*.sln').test('nested/sakura.sln'));
  assert.ok(globToRegExp('{package.json,pom.xml}').test('pom.xml'));
  assert.ok(!globToRegExp('{package.json,pom.xml}').test('build.gradle'));
});

test('matches workspaceContains patterns against real folders within the walk budget', (t) => {
  const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-ws-'));
  t.after(() => fs.rmSync(workspace, { recursive: true, force: true }));
  fs.mkdirSync(path.join(workspace, 'packages', 'app'), { recursive: true });
  fs.writeFileSync(path.join(workspace, 'packages', 'app', '.eslintrc'), '{}');
  fs.mkdirSync(path.join(workspace, 'node_modules', 'ignored'), { recursive: true });
  fs.writeFileSync(path.join(workspace, 'node_modules', 'ignored', 'marker.txt'), '');

  assert.ok(workspaceContainsMatches([workspace], '**/.eslintrc'));
  assert.ok(workspaceContainsMatches([workspace], 'packages/app/.eslintrc'));
  assert.ok(!workspaceContainsMatches([workspace], '**/tsconfig.json'));
  // node_modules は探索から外す。ここを歩くと巨大リポジトリで拡張ホストが固まる。
  assert.ok(!workspaceContainsMatches([workspace], '**/marker.txt'));
});

test('activates an extension when the workspace folders start matching workspaceContains', async (t) => {
  const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'sakura-ws-'));
  t.after(() => fs.rmSync(workspace, { recursive: true, force: true }));
  const root = createExtension({
    name: 'ws',
    publisher: 'test',
    main: './extension',
    activationEvents: ['workspaceContains:.sakuraproject'],
  }, { 'extension.js': 'exports.activate = () => ({ activated: true });' });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));

  const transport = new RecordingTransport();
  const loader = new ExtensionLoader(3, transport);
  t.after(() => loader.dispose());
  loader.register([root]);

  // まだマーカーが無いので起動しない。
  assert.deepEqual(await loader.setWorkspaceFolders([workspace]), { activated: [], failed: [] });
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 0);

  fs.writeFileSync(path.join(workspace, '.sakuraproject'), '');
  assert.deepEqual(await loader.setWorkspaceFolders([workspace]), { activated: ['test.ws'], failed: [] });
  assert.equal(transport.notified('workbench/extensions/didActivate').length, 1);

  // 2 回目は既に active なので再起動しない。
  assert.deepEqual(await loader.setWorkspaceFolders([workspace]), { activated: [], failed: [] });
});
