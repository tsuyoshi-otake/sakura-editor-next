'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const { fileURLToPath, pathToFileURL } = require('node:url');
const { ExtensionApiSession } = require('./vscode-api.cjs');

const MAX_MANIFEST_BYTES = 4 * 1024 * 1024;
const moduleApis = new Map();
let originalModuleLoad = null;
let esmModuleHooks = null;
const esmRegistrySymbol = Symbol.for('sakura.editor.extensionApis');

function normalizeRoot(value) {
  const resolved = path.resolve(value);
  return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

function isWithin(root, candidate) {
  const normalizedRoot = normalizeRoot(root);
  const normalizedCandidate = normalizeRoot(candidate);
  return normalizedCandidate === normalizedRoot || normalizedCandidate.startsWith(`${normalizedRoot}${path.sep}`);
}

function installVscodeBridge() {
  if (originalModuleLoad) return;
  originalModuleLoad = Module._load;
  Module._load = function sakuraLoad(request, parent, isMain) {
    if (request === 'vscode' && parent?.filename) {
      const filename = normalizeRoot(parent.filename);
      let best = null;
      for (const [root, api] of moduleApis) {
        if ((filename === root || filename.startsWith(`${root}${path.sep}`)) && (!best || root.length > best.root.length)) {
          best = { root, api };
        }
      }
      if (best) return best.api;
    }
    return originalModuleLoad.call(this, request, parent, isMain);
  };
  if (typeof Module.registerHooks === 'function' && !esmModuleHooks) {
    globalThis[esmRegistrySymbol] = moduleApis;
    esmModuleHooks = Module.registerHooks({
      resolve(specifier, context, nextResolve) {
        if (specifier !== 'vscode' || !context?.parentURL?.startsWith('file:')) return nextResolve(specifier, context);
        let filename;
        try { filename = normalizeRoot(fileURLToPath(context.parentURL)); } catch { return nextResolve(specifier, context); }
        let bestRoot = null;
        for (const root of moduleApis.keys()) {
          if ((filename === root || filename.startsWith(`${root}${path.sep}`)) && (!bestRoot || root.length > bestRoot.length)) {
            bestRoot = root;
          }
        }
        if (!bestRoot) return nextResolve(specifier, context);
        return { url: `sakura-vscode:${Buffer.from(bestRoot, 'utf8').toString('base64url')}`, shortCircuit: true };
      },
      load(url, context, nextLoad) {
        if (!url.startsWith('sakura-vscode:')) return nextLoad(url, context);
        const root = Buffer.from(url.slice('sakura-vscode:'.length), 'base64url').toString('utf8');
        const api = moduleApis.get(root);
        if (!api) throw new Error(`VS Code ESM bridge has no API session for ${root}`);
        const exports = Object.keys(api).filter((key) => /^[A-Za-z_$][\w$]*$/.test(key) && key !== 'default');
        const source = [
          `const api = globalThis[Symbol.for('sakura.editor.extensionApis')].get(${JSON.stringify(root)});`,
          `if (!api) throw new Error('VS Code API session is no longer active');`,
          'export default api;',
          ...exports.map((key) => `export const ${key} = api[${JSON.stringify(key)}];`),
        ].join('\n');
        return { format: 'module', source, shortCircuit: true };
      },
    });
  }
}

function maybeUninstallVscodeBridge() {
  if (!originalModuleLoad || moduleApis.size !== 0) return;
  Module._load = originalModuleLoad;
  originalModuleLoad = null;
  esmModuleHooks?.deregister?.();
  esmModuleHooks = null;
  delete globalThis[esmRegistrySymbol];
}

function readManifest(extensionPath) {
  const root = fs.realpathSync.native(path.resolve(extensionPath));
  const stat = fs.lstatSync(root);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('extension root must be a non-symlink directory');
  const manifestPath = path.join(root, 'package.json');
  const manifestStat = fs.lstatSync(manifestPath);
  if (!manifestStat.isFile() || manifestStat.isSymbolicLink() || manifestStat.size > MAX_MANIFEST_BYTES) {
    throw new Error('extension package.json is missing, unsafe, or too large');
  }
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) throw new Error('extension package.json must be an object');
  const publisher = typeof manifest.publisher === 'string' && manifest.publisher ? manifest.publisher : undefined;
  const name = typeof manifest.name === 'string' && manifest.name ? manifest.name : undefined;
  if (!publisher || !name || !/^[\w.-]+$/.test(publisher) || !/^[\w.-]+$/.test(name)) {
    throw new Error('extension publisher and name must be safe non-empty identifiers');
  }
  return { root, manifest, extensionId: `${publisher}.${name}`.toLowerCase() };
}

function contributionCommands(manifest) {
  const commands = Array.isArray(manifest?.contributes?.commands) ? manifest.contributes.commands : [];
  return commands.flatMap((entry) => {
    if (!entry || typeof entry.command !== 'string' || !entry.command) return [];
    return [{
      id: entry.command,
      title: typeof entry.title === 'string' ? entry.title : entry.command,
      category: typeof entry.category === 'string' ? entry.category : '',
      enablement: typeof entry.enablement === 'string' ? entry.enablement : '',
    }];
  });
}

//! マニフェスト由来の任意文字列。型が違う値は「無い」として扱い、例外にはしない。
//! contributes は拡張作者の手書きなので、1 項目の型崩れで拡張全体を落とすのは過剰。
function optionalString(value) {
  return typeof value === 'string' ? value : '';
}

//! アイコンは文字列（画像への相対パス）と `{ light, dark }` の 2 形態がある。
//! ネイティブ側が扱えるのは 1 本のパスなので dark を代表に選ぶ（既定テーマが暗色のため）。
function optionalIcon(value) {
  if (typeof value === 'string') return value;
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    return optionalString(value.dark) || optionalString(value.light);
  }
  return '';
}

//! VS Code は `$(codicon-name)` 形式のアイコン ID も受け付ける。パスと区別できるよう分離する。
function isCodiconReference(icon) {
  return /^\$\([\w-]+\)$/.test(icon);
}

function contributionViews(manifest) {
  const result = [];
  const containers = manifest?.contributes?.views;
  if (!containers || typeof containers !== 'object' || Array.isArray(containers)) return result;
  for (const [containerId, views] of Object.entries(containers)) {
    if (!Array.isArray(views)) continue;
    for (const view of views) {
      if (!view || typeof view.id !== 'string' || !view.id) continue;
      /*
        `type` は VS Code では省略時 "tree"。Claude Code の sidebar は "webview" を使う。
        ここで型を落とすと、ネイティブ側は webview ビューを tree として登録してしまい、
        「登録は成功したのに永久に空」という最悪の壊れ方をする。必ず伝える。
      */
      const type = optionalString(view.type).toLowerCase();
      result.push({
        id: view.id,
        name: optionalString(view.name) || view.id,
        containerId,
        type: type === 'webview' ? 'webview' : 'tree',
        when: optionalString(view.when),
        icon: optionalIcon(view.icon),
        contextualTitle: optionalString(view.contextualTitle),
        visibility: optionalString(view.visibility),
      });
    }
  }
  return result;
}

/*!
  @brief viewsContainers として VS Code が受け付ける location キー

  VS Code の `viewsContainers` は location をキーにした object で、キーは
  `activitybar` / `panel` / `secondarySidebar` の 3 つ。ここに無いキーは
  VS Code 側でも contribution エラーとして無視されるので、既定値へ丸めない。
*/
const VIEW_CONTAINER_LOCATIONS = new Set(['activitybar', 'panel', 'secondarySidebar']);

/*!
  @brief `contributes.viewsContainers` を location 付きの平坦な配列へ変換する

  ネイティブ側は Activity Bar（Primary Side Bar）・Panel・Secondary Side Bar で別の
  受け皿を持つので、どこに属するかを失わないよう location を各要素へ畳み込む。
  未知の location を既定値へ丸めると、VS Code が名前を持つ概念を別の場所へ配ってしまう
  ので、白名簿に無いキーはコンテナごと落とす。

  `when` はコンテナにも付く。拡張は排他的な複数コンテナを宣言して `when` で 1 つだけ
  見せる（Claude Code の Primary/Secondary 切り替えなど）ので、ここで落とすと排他が
  壊れて全部が同時に出る。評価はネイティブ側の context key に依るため、句は素通しする。
*/
function contributionViewsContainers(manifest) {
  const result = [];
  const containers = manifest?.contributes?.viewsContainers;
  if (!containers || typeof containers !== 'object' || Array.isArray(containers)) return result;
  for (const [location, entries] of Object.entries(containers)) {
    if (!Array.isArray(entries) || !VIEW_CONTAINER_LOCATIONS.has(location)) continue;
    for (const entry of entries) {
      if (!entry || typeof entry.id !== 'string' || !entry.id) continue;
      const icon = optionalIcon(entry.icon);
      result.push({
        id: entry.id,
        title: optionalString(entry.title) || entry.id,
        location,
        icon: isCodiconReference(icon) ? '' : icon,
        codicon: isCodiconReference(icon) ? icon.slice(2, -1) : '',
        when: optionalString(entry.when),
      });
    }
  }
  return result;
}

/*!
  @brief `contributes.menus` をメニュー面ごとの項目一覧へ変換する

  面（`editor/title` などのキー）は VS Code が増やし続けるので、ここでは白名簿を作らず
  形の妥当性だけを見る。どの面を実際に描くかはネイティブ側の判断に委ねる。
  そうしないと VS Code が新しい面を足すたびに拡張ホストの改修が必要になる。
*/
function contributionMenus(manifest) {
  const result = Object.create(null);
  const menus = manifest?.contributes?.menus;
  if (!menus || typeof menus !== 'object' || Array.isArray(menus)) return result;
  for (const [location, entries] of Object.entries(menus)) {
    if (!Array.isArray(entries)) continue;
    const items = [];
    for (const entry of entries) {
      if (!entry || typeof entry !== 'object') continue;
      const command = optionalString(entry.command);
      const submenu = optionalString(entry.submenu);
      // command も submenu も無い項目は何も起動できない。持たせても害にしかならない。
      if (!command && !submenu) continue;
      items.push({
        command,
        submenu,
        alt: optionalString(entry.alt),
        when: optionalString(entry.when),
        group: optionalString(entry.group),
      });
    }
    if (items.length !== 0) result[location] = items;
  }
  return result;
}

//! `contributes.submenus`。`menus` の項目が `submenu` で参照する入れ子メニューの定義。
function contributionSubmenus(manifest) {
  const submenus = Array.isArray(manifest?.contributes?.submenus) ? manifest.contributes.submenus : [];
  return submenus.flatMap((entry) => {
    if (!entry || typeof entry.id !== 'string' || !entry.id) return [];
    return [{ id: entry.id, label: optionalString(entry.label) || entry.id, icon: optionalIcon(entry.icon) }];
  });
}

/*!
  @brief `contributes.keybindings` を、このホストが解釈できる 1 本のキー式へ正規化する

  VS Code は `key`（既定）と `mac`/`linux`/`win` の上書きを持つ。Windows 専用ホストなので
  `win` があればそれを、無ければ `key` を採る。`mac`/`linux` は捨てる（保持しても
  このホストでは決して使われず、ネイティブ側に「どれを使うか」の判断を持ち込むだけになる）。
*/
function contributionKeybindings(manifest) {
  const keybindings = Array.isArray(manifest?.contributes?.keybindings) ? manifest.contributes.keybindings : [];
  return keybindings.flatMap((entry) => {
    if (!entry || typeof entry.command !== 'string' || !entry.command) return [];
    const key = optionalString(entry.win) || optionalString(entry.key);
    if (!key) return [];
    const result = { command: entry.command, key, when: optionalString(entry.when) };
    if (entry.args !== undefined) result.args = entry.args;
    return [result];
  });
}

/*!
  @brief `contributes.languages` を言語 ID とその識別規則へ変換する

  `onLanguage:` の発火と `vscode.languages.getLanguages()` の土台になる。
  `configuration`（言語設定 JSON）は拡張ルートからの相対パスなので、
  ルート外への脱出を防ぐ検証は呼び出し側（register）で行う。
*/
function contributionLanguages(manifest) {
  const languages = Array.isArray(manifest?.contributes?.languages) ? manifest.contributes.languages : [];
  const stringArray = (value) => (Array.isArray(value) ? value.filter((item) => typeof item === 'string' && item) : []);
  return languages.flatMap((entry) => {
    if (!entry || typeof entry.id !== 'string' || !entry.id) return [];
    return [{
      id: entry.id,
      aliases: stringArray(entry.aliases),
      extensions: stringArray(entry.extensions),
      filenames: stringArray(entry.filenames),
      filenamePatterns: stringArray(entry.filenamePatterns),
      mimetypes: stringArray(entry.mimetypes),
      firstLine: optionalString(entry.firstLine),
      icon: optionalIcon(entry.icon),
      configuration: optionalString(entry.configuration),
    }];
  });
}

//! `contributes.snippets`。`path` は拡張ルート相対で、検証は register 側で行う。
function contributionSnippets(manifest) {
  const snippets = Array.isArray(manifest?.contributes?.snippets) ? manifest.contributes.snippets : [];
  return snippets.flatMap((entry) => {
    if (!entry || typeof entry.path !== 'string' || !entry.path) return [];
    const language = optionalString(entry.language);
    if (!language) return [];
    return [{ language, path: entry.path }];
  });
}

/*!
  @brief 未実装だが「宣言されていること自体は正常」な contribution の一覧

  これを返さないと、ネイティブ側は知らないキーを見て UnsupportedCapability を出すか、
  逆に何も言わずに落とすかのどちらかになる。前者は正常な拡張を壊れているように見せ、
  後者は本当の欠落を隠す。「受理したが未実装」という第 3 の状態を明示的に持つ。
*/
const ACKNOWLEDGED_CONTRIBUTIONS = Object.freeze([
  'jsonValidation',
  'walkthroughs',
  'grammars',
  'themes',
  'iconThemes',
  'productIconThemes',
  'debuggers',
  'breakpoints',
  'taskDefinitions',
  'problemMatchers',
  'problemPatterns',
  'terminal',
  'customEditors',
  'notebooks',
  'semanticTokenScopes',
  'semanticTokenTypes',
  'semanticTokenModifiers',
  'resourceLabelFormatters',
  'authentication',
  'colors',
  'icons',
  'localizations',
  'markdown.previewStyles',
  'typescriptServerPlugins',
]);

function acknowledgedContributions(manifest) {
  const contributes = manifest?.contributes;
  if (!contributes || typeof contributes !== 'object' || Array.isArray(contributes)) return [];
  return ACKNOWLEDGED_CONTRIBUTIONS.filter((key) => Object.prototype.hasOwnProperty.call(contributes, key));
}

function contributionConfigurationDefaults(manifest) {
  const defaults = Object.create(null);
  const configurations = Array.isArray(manifest?.contributes?.configuration)
    ? manifest.contributes.configuration : [manifest?.contributes?.configuration];
  for (const configuration of configurations) {
    if (!configuration?.properties || typeof configuration.properties !== 'object') continue;
    for (const [key, schema] of Object.entries(configuration.properties)) {
      if (schema && Object.prototype.hasOwnProperty.call(schema, 'default')) defaults[key] = schema.default;
      else if (schema?.type === 'array') defaults[key] = [];
      else if (schema?.type === 'object') defaults[key] = {};
    }
  }
  if (manifest?.contributes?.configurationDefaults && typeof manifest.contributes.configurationDefaults === 'object') {
    Object.assign(defaults, manifest.contributes.configurationDefaults);
  }
  return defaults;
}

function activationEvents(record) {
  const explicit = Array.isArray(record.manifest.activationEvents)
    ? record.manifest.activationEvents.filter((value) => typeof value === 'string') : [];
  const inferred = [
    ...record.commands.map((command) => `onCommand:${command.id}`),
    ...record.views.map((view) => `onView:${view.id}`),
  ];
  return new Set([...explicit, ...inferred]);
}

/*
  workspaceContains: の探索予算。
  VS Code はワークスペース全体を検索インデックス越しに引けるが、このホストは持たない。
  無制限に歩くと、巨大なリポジトリを開いた瞬間に拡張ホストが数秒固まる。
  「深さ 4・2 万エントリ」を超えたら探索を打ち切り、その拡張は起動しない。
  取りこぼす可能性はあるが、固まらないことの方が重要で、しかもコマンド実行や
  onLanguage: など他の経路で起動する道は残る。
*/
const WORKSPACE_CONTAINS_MAX_DEPTH = 4;
const WORKSPACE_CONTAINS_MAX_ENTRIES = 20000;
const WORKSPACE_CONTAINS_SKIP = new Set(['node_modules', '.git', '.hg', '.svn', 'out', 'dist', 'build', '.venv']);

//! glob メタ文字を含むか。含まなければ存在確認 1 回で済むので、歩かずに答えを出せる。
function hasGlobMetacharacters(pattern) {
  return /[*?{}[\]]/.test(pattern);
}

/*!
  @brief VS Code の relative glob を RegExp へ変換する

  対応するのは `**`（0 個以上のパスセグメント）・`*`（区切りを跨がない任意文字列）・
  `?`（区切り以外の 1 文字）・`{a,b}`（択一）。それ以外のメタ文字はリテラルとして
  エスケープする。未対応の構文を黙って近似すると、意図しない拡張が起動してしまうため
  「解釈できない部分は一致しない」側へ倒す。
*/
function globToRegExp(pattern) {
  let source = '';
  for (let index = 0; index < pattern.length; index += 1) {
    const char = pattern[index];
    if (char === '*') {
      if (pattern[index + 1] === '*') {
        // `**/` は 0 個以上のセグメント。末尾の `**` は残り全部。
        if (pattern[index + 2] === '/') { source += '(?:[^/]+/)*'; index += 2; } else { source += '.*'; index += 1; }
      } else {
        source += '[^/]*';
      }
    } else if (char === '?') source += '[^/]';
    else if (char === '{') source += '(?:';
    else if (char === '}') source += ')';
    else if (char === ',') source += '|';
    else source += char.replace(/[.+^$()|[\]\\/]/g, '\\$&');
  }
  return new RegExp(`^${source}$`);
}

//! 予算内でワークスペースを歩き、相対パスを 1 件ずつ visit へ渡す。true を返したら打ち切る。
function walkWorkspace(folder, visit) {
  let budget = WORKSPACE_CONTAINS_MAX_ENTRIES;
  const stack = [{ dir: folder, prefix: '', depth: 0 }];
  while (stack.length !== 0) {
    const { dir, prefix, depth } = stack.pop();
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { continue; }
    for (const entry of entries) {
      if (budget-- <= 0) return false;
      const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (visit(relative)) return true;
      if (entry.isDirectory() && !entry.isSymbolicLink()
        && depth + 1 < WORKSPACE_CONTAINS_MAX_DEPTH && !WORKSPACE_CONTAINS_SKIP.has(entry.name)) {
        stack.push({ dir: path.join(dir, entry.name), prefix: relative, depth: depth + 1 });
      }
    }
  }
  return false;
}

/*!
  @brief `workspaceContains:<pattern>` が、与えられたワークスペースフォルダーに該当するか
  @param folders 絶対パスの配列
  @param pattern `workspaceContains:` を取り除いた残り
*/
function workspaceContainsMatches(folders, pattern) {
  if (!pattern) return false;
  for (const folder of folders) {
    if (!hasGlobMetacharacters(pattern)) {
      try { if (fs.existsSync(path.join(folder, pattern))) return true; } catch {}
      continue;
    }
    const matcher = globToRegExp(pattern);
    if (walkWorkspace(folder, (relative) => matcher.test(relative))) return true;
  }
  return false;
}

function resolveMainEntry(root, main) {
  const candidate = path.resolve(root, main);
  if (!isWithin(root, candidate)) throw new Error(`extension main escapes extension root: ${main}`);
  let resolved;
  try {
    resolved = require.resolve(candidate);
  } catch {
    throw new Error(`extension main is missing or unsafe: ${main}`);
  }
  if (!isWithin(root, resolved)) throw new Error(`extension main escapes extension root: ${main}`);
  const stat = fs.lstatSync(resolved);
  const realEntry = fs.realpathSync.native(resolved);
  if (!stat.isFile() || stat.isSymbolicLink() || !isWithin(root, realEntry)) {
    throw new Error(`extension main is missing or unsafe: ${main}`);
  }
  return realEntry;
}

/*!
  @brief 拡張ルート相対のパスを絶対パスへ解決する。ルート外へ出るものは空文字列。

  スニペットや言語設定のパスはマニフェスト由来なので `../../` を仕込める。
  resolveMainEntry と同じ規律をここにも適用し、脱出するものは「無かったこと」にする。
*/
function safeContributedPath(root, relative) {
  if (typeof relative !== 'string' || !relative) return '';
  const candidate = path.resolve(root, relative);
  if (!isWithin(root, candidate)) return '';
  try {
    const stat = fs.lstatSync(candidate);
    if (!stat.isFile() || stat.isSymbolicLink()) return '';
    if (!isWithin(root, fs.realpathSync.native(candidate))) return '';
  } catch {
    return '';
  }
  return candidate;
}

class ExtensionLoader {
  constructor(generation, transport, options = {}) {
    if (!Number.isSafeInteger(generation) || generation <= 0) throw new TypeError('generation must be positive');
    this.generation = generation;
    this.transport = transport;
    this.options = options;
    this.extensions = new Map();
    this.commandOwners = new Map();
    this.viewOwners = new Map();
    this.workspaceFolders = Array.isArray(options.workspaceFolders)
      ? options.workspaceFolders.filter((value) => typeof value === 'string' && value) : [];
    this.disposed = false;
  }

  /*!
    @brief ワークスペースフォルダーを差し替え、`workspaceContains:` を再評価する

    フォルダーは後から開かれる（起動時には空のことがある）ので、register とは
    独立に呼べる必要がある。戻り値は activateByEvent と同じ要約。
  */
  async setWorkspaceFolders(folders) {
    if (this.disposed) throw new Error('extension loader is disposed');
    this.workspaceFolders = Array.isArray(folders)
      ? folders.filter((value) => typeof value === 'string' && value) : [];
    return this.activateByWorkspaceContains();
  }

  /*!
    @brief `workspaceContains:` を満たす未起動の拡張をまとめて起動する

    activateByEvent と違いイベント名が固定でないので、登録済みの拡張それぞれが
    宣言した pattern を評価する。ワークスペースが無ければ何もしない。
  */
  async activateByWorkspaceContains() {
    if (this.workspaceFolders.length === 0) return { activated: [], failed: [] };
    const targets = [];
    for (const record of this.extensions.values()) {
      if (record.state !== 'registered') continue;
      for (const event of record.events) {
        if (!event.startsWith('workspaceContains:')) continue;
        if (workspaceContainsMatches(this.workspaceFolders, event.slice('workspaceContains:'.length))) {
          targets.push({ extensionId: record.extensionId, event });
          break;
        }
      }
    }
    const activated = [];
    const failed = [];
    const results = await Promise.all(targets.map(({ extensionId, event }) => this.activate(extensionId, event).then(
      () => ({ extensionId, error: null }),
      (error) => ({ extensionId, error: error instanceof Error ? error.message : String(error) }))));
    for (const result of results) {
      if (result.error === null) activated.push(result.extensionId);
      else failed.push({ extensionId: result.extensionId, message: result.error });
    }
    return { activated, failed };
  }

  /*!
    @brief ネイティブから届いた configuration / workspaceFolders スナップショットを session options
    へ反映する。

    register() は再接続の度に extensions を丸ごと送り直す契約（`workbench/extensions/register` の
    冪等な再送）で、これも同じく差分ではなく最新スナップショットへの総入れ替えとして扱う。ここでの
    workspaceFolders は `{uri, name}` 形の vscode.workspace.workspaceFolders 用の形であり、
    setWorkspaceFolders が保持する `workspaceContains:` 判定専用の文字列配列 (this.workspaceFolders)
    とは別物 — 互いに上書きしない。configurationDefaults と同様、ここに積んだ options は
    activateRecord が ...this.options で spread するので、以後アクティベートされる拡張から反映される
    （既にアクティブなセッションへは遡って反映しない。反映するには
    extension/workspace/didChangeConfiguration の能動的な通知が別途必要で、これは現状ネイティブ側から
    送られていない既知のギャップ）。workspaceTrusted も同じく最新スナップショットへの総入れ替えとして扱う。
  */
  mergeSessionOptions(sessionOptions) {
    if (!sessionOptions || typeof sessionOptions !== 'object') return;
    if (sessionOptions.configuration && typeof sessionOptions.configuration === 'object') {
      this.options.configuration = { ...sessionOptions.configuration };
    }
    if (Array.isArray(sessionOptions.workspaceFolders)) {
      this.options.workspaceFolders = sessionOptions.workspaceFolders;
    }
    if (typeof sessionOptions.workspaceTrusted === 'boolean') {
      this.options.workspaceTrusted = sessionOptions.workspaceTrusted;
    }
  }

  register(extensionPaths, sessionOptions) {
    if (this.disposed) throw new Error('extension loader is disposed');
    if (!Array.isArray(extensionPaths)) throw new TypeError('extensions must be an array');
    this.mergeSessionOptions(sessionOptions);
    const registered = [];
    const failed = [];
    let added = false;
    for (const descriptor of extensionPaths) {
      try {
        const extensionPath = typeof descriptor === 'string' ? descriptor : descriptor?.path;
        if (typeof extensionPath !== 'string' || !extensionPath) throw new TypeError('extension path is required');
        const parsed = readManifest(extensionPath);
        const existing = this.extensions.get(parsed.extensionId);
        if (existing) {
          if (normalizeRoot(existing.root) !== normalizeRoot(parsed.root)) {
            throw new Error(`extension ID is already registered from a different path: ${parsed.extensionId}`);
          }
          registered.push(existing.metadata);
          this.transport.notify('workbench/extensions/register', existing.metadata);
          continue;
        }
        const record = {
          ...parsed,
          commands: contributionCommands(parsed.manifest),
          views: contributionViews(parsed.manifest),
          viewsContainers: contributionViewsContainers(parsed.manifest),
          menus: contributionMenus(parsed.manifest),
          submenus: contributionSubmenus(parsed.manifest),
          keybindings: contributionKeybindings(parsed.manifest),
          // 相対パスはここで実ファイルへ解決する。解決できないものは落とす:
          // 存在しないパスをネイティブへ渡しても、あちらで同じ失敗を繰り返すだけ。
          languages: contributionLanguages(parsed.manifest).map((language) => ({
            ...language, configuration: safeContributedPath(parsed.root, language.configuration),
          })),
          snippets: contributionSnippets(parsed.manifest)
            .map((snippet) => ({ ...snippet, path: safeContributedPath(parsed.root, snippet.path) }))
            .filter((snippet) => snippet.path !== ''),
          acknowledged: acknowledgedContributions(parsed.manifest),
          configurationDefaults: contributionConfigurationDefaults(parsed.manifest),
          events: null,
          state: 'registered',
          session: null,
          context: null,
          exports: undefined,
          activation: null,
          descriptor: null,
          paths: typeof descriptor === 'object' && descriptor ? descriptor : {},
        };
        record.events = activationEvents(record);
        this.extensions.set(record.extensionId, record);
        added = true;
        for (const command of record.commands) {
          if (!this.commandOwners.has(command.id)) this.commandOwners.set(command.id, record.extensionId);
        }
        for (const view of record.views) {
          if (!this.viewOwners.has(view.id)) this.viewOwners.set(view.id, record.extensionId);
        }
        const metadata = {
          extensionId: record.extensionId,
          generation: this.generation,
          version: typeof record.manifest.version === 'string' ? record.manifest.version : '',
          displayName: typeof record.manifest.displayName === 'string' ? record.manifest.displayName : record.extensionId,
          commands: record.commands,
          views: record.views,
          viewsContainers: record.viewsContainers,
          menus: record.menus,
          submenus: record.submenus,
          keybindings: record.keybindings,
          languages: record.languages,
          snippets: record.snippets,
          // 「宣言されているが未実装」の一覧。ネイティブ側が UnsupportedCapability を
          // 出すかどうかの判断材料であって、欠落を隠すための握り潰しではない。
          acknowledgedContributions: record.acknowledged,
          extensionPath: record.root,
          activationEvents: [...record.events],
        };
        record.metadata = metadata;
        registered.push(metadata);
        this.transport.notify('workbench/extensions/register', metadata);
      } catch (error) {
        failed.push({ path: typeof descriptor === 'string' ? descriptor : descriptor?.path, message: error instanceof Error ? error.message : String(error) });
      }
    }
    if (added) {
      this.fireExtensionsChanged();
      // register は同期契約なので待てない。workspaceContains: の起動は独立に走らせ、
      // 結果は didActivate / didFailActivation 通知でネイティブ側へ届く。
      if (this.workspaceFolders.length !== 0) this.activateByWorkspaceContains().catch(() => {});
    }
    return { registered, failed };
  }

  async activate(extensionId, reason = '*') {
    if (this.disposed) throw new Error('extension loader is disposed');
    const record = this.extensions.get(String(extensionId).toLowerCase());
    if (!record) throw new Error(`extension is not registered: ${extensionId}`);
    if (record.state === 'active') return record.exports;
    // 実 VS Code の ExtensionsActivator は一度失敗した activate() を呼び出す
    // たびに再実行しない — ActivationFailedError をキャッシュして再スローする
    // だけである。ここで毎回再試行すると、下の activateRecord が (このコミット
    // 以降) 失敗前に完了していた登録を保持したまま抜けるようになったのと組み
    // 合わさって、失敗するたびに新しい ExtensionApiSession を作って古い
    // セッションを迷子にし続ける (リソースリーク)。失敗は一度だけ記録し、以降は
    // 同じエラーを再スローする。
    if (record.state === 'failed') {
      throw record.activationError instanceof Error
        ? record.activationError
        : new Error(`extension failed to activate: ${record.extensionId}`);
    }
    if (record.activation) return record.activation;
    record.activation = this.activateRecord(record, reason);
    try {
      return await record.activation;
    } finally {
      record.activation = null;
    }
  }

  //! Stable public description of one record, reused so `vscode.extensions`
  //! hands out the same object identity for the same extension.
  descriptorFor(record) {
    if (record.descriptor) return record.descriptor;
    const loader = this;
    record.descriptor = Object.freeze({
      extensionId: record.extensionId,
      extensionPath: record.root,
      packageJSON: record.manifest,
      get isActive() { return record.state === 'active'; },
      get exports() { return record.state === 'active' ? record.exports : undefined; },
      activate: () => loader.activate(record.extensionId, 'api'),
    });
    return record.descriptor;
  }

  extensionRegistryPort() {
    const loader = this;
    return {
      all: () => [...loader.extensions.values()].map((record) => loader.descriptorFor(record)),
      describe: (extensionId) => {
        const record = loader.extensions.get(String(extensionId).toLowerCase());
        return record ? loader.descriptorFor(record) : undefined;
      },
    };
  }

  //! VS Code raises `extensions.onDidChange` when the installed set changes.
  fireExtensionsChanged() {
    for (const record of this.extensions.values()) {
      try { record.session?.extensionsChangeEmitter?.fire(); } catch {}
    }
  }

  async activateRecord(record, reason) {
    record.state = 'activating';
    const session = new ExtensionApiSession(record.extensionId, this.generation, this.transport, {
      ...this.options,
      configurationDefaults: { ...(this.options.configurationDefaults || {}), ...record.configurationDefaults },
      extensionRegistry: this.extensionRegistryPort(),
    });
    record.session = session;
    const normalizedRoot = normalizeRoot(record.root);
    try {
      record.context = session.createExtensionContext({
        extensionPath: record.root,
        storagePath: record.paths.storagePath,
        globalStoragePath: record.paths.globalStoragePath,
         logPath: record.paths.logPath,
        packageJSON: record.manifest,
      });
      const main = typeof record.manifest.main === 'string' ? record.manifest.main : undefined;
      if (!main) {
        record.exports = {};
        record.state = 'active';
        this.transport.notify('workbench/extensions/didActivate', {
          extensionId: record.extensionId, generation: this.generation, reason,
        });
        return record.exports;
      }
      const entry = resolveMainEntry(record.root, main);
      installVscodeBridge();
      moduleApis.set(normalizedRoot, session.api);
      let moduleExports;
      try {
        moduleExports = require(entry);
      } catch (error) {
        if (error?.code !== 'ERR_REQUIRE_ESM') throw error;
        moduleExports = await import(pathToFileURL(entry).href);
      }
      record.exports = typeof moduleExports?.activate === 'function'
        ? await moduleExports.activate(record.context) : moduleExports;
      record.module = moduleExports;
      record.state = 'active';
      this.transport.notify('workbench/extensions/didActivate', {
        extensionId: record.extensionId, generation: this.generation, reason,
      });
      return record.exports;
    } catch (error) {
      record.state = 'failed';
      record.activationError = error instanceof Error ? error : new Error(String(error));
      // 実 VS Code (AbstractExtensionService._doActivateExtension) は activate()
      // が例外を投げても、それより前に完了していた個々の登録 (registerCommand
      // など) を巻き戻さない。コマンド登録は MainThreadCommands への即時 RPC
      // であり、activate() 全体を包む「トランザクション」という概念はどこにも
      // 存在しない。
      //
      // そのためここでは意図的に session.dispose() を呼ばず、record.session /
      // record.context も null にしない: 呼んでしまうと session.disposables に
      // 積まれた「例外より前に registerCommand が成功させていた」Disposable
      // まで一緒に破棄され、後発の未対応 API 呼び出し 1 つの失敗で、その拡張が
      // 既に登録できていたコマンド・キーバインドまで丸ごと消えてしまう
      // (Issue #23 のコマンドパレット/キーバインド互換ゲートが、例えば
      // window.createWebviewPanel のような未対応 API を activate() の後半で
      // 呼ぶだけの拡張によって壊れる)。未対応 API 自体は呼び出しごとに
      // UnsupportedCapabilityError (vscode-api.cjs) として型付きで診断可能に
      // 失敗しており、ここで成功を装っているわけではない — 失敗は
      // record.state / record.activationError / didFailActivation 通知として
      // 隠さず記録しつつ、それより前に完了していた登録だけは生かす。
      //
      // 以降の再アクティブ化試行を打ち切るのは上の activate() 側 (state ===
      // 'failed' の早期リターン) の役目であり、ここでは record.session を
      // 残すことで extension/commands/execute のセッション参照が引き続き
      // 有効なままになる。
      moduleApis.delete(normalizedRoot);
      maybeUninstallVscodeBridge();
      this.transport.notify('workbench/extensions/didFailActivation', {
        extensionId: record.extensionId, generation: this.generation,
        message: error instanceof Error ? error.message : String(error),
      });
      throw error;
    }
  }

  // 実 VS Code の AbstractExtensionService._activateByEvent は該当する拡張を Promise.all で
  // 並行に起動し、1 つの activate() が投げてもそれは _onExtensionActivationError で
  // その拡張だけの失敗として記録される。他の拡張の起動は止まらない。
  // ここも同じ契約にする: 直列の await ループで最初の例外を伝播させると、たまたま先に
  // 並んだ 1 つが落ちただけで後続の拡張が 'registered' のまま永久に起動しなくなる
  // (実機で odangoo.otak-monitor の失敗が odangoo.otak-usage を巻き添えにした)。
  async activateByEvent(event) {
    const targets = [];
    for (const record of this.extensions.values()) {
      if (record.state === 'registered' && (record.events.has(event) || record.events.has('*'))) {
        targets.push(record.extensionId);
      }
    }
    const activated = [];
    const failed = [];
    // 失敗は didFailActivation 通知で既にネイティブ側の Extension Host ログへ届いているので、
    // ここでは呼び出し元が結果を見られるよう要約するだけで、再スローはしない。
    const results = await Promise.all(targets.map((extensionId) => this.activate(extensionId, event).then(
      () => ({ extensionId, error: null }),
      (error) => ({ extensionId, error: error instanceof Error ? error.message : String(error) }))));
    for (const result of results) {
      if (result.error === null) activated.push(result.extensionId);
      else failed.push({ extensionId: result.extensionId, message: result.error });
    }
    return { activated, failed };
  }

  sessionForHandle(kind, handle) {
    for (const record of this.extensions.values()) {
      if (!record.session) continue;
      if (kind === 'view' && record.session.views.has(handle)) return record.session;
      if (kind === 'progress' && record.session.progress.has(handle)) return record.session;
      if (kind === 'scm' && (record.session.sourceControls.has(handle) || record.session.globalSourceControlInputBox?.handle === handle)) return record.session;
      if (kind === 'provider') {
        for (const providers of record.session.languageProviders.values()) if (providers.has(handle)) return record.session;
      }
    }
    return null;
  }

  async handleRequest(method, params) {
    if (method === 'extension/commands/execute') {
      const command = params?.command;
      const owner = this.commandOwners.get(command);
      if (owner) {
        try {
          await this.activate(owner, `onCommand:${command}`);
        } catch {
          // activate() の失敗は didFailActivation で既に記録済み
          // (activateRecord のコメント参照)。ここで再スローすると、
          // activate() の後半 (例えば未対応の webview API 呼び出し) で
          // 失敗した拡張が、その前半で登録できていたコマンドまで実行不能に
          // なってしまう。実際に呼び出せるかどうかは下の session の有無、
          // および session.handleRequest 自身の
          // 「command handler is not registered」チェックに委ねる。
        }
      }
      const session = owner ? this.extensions.get(owner)?.session : null;
      if (!session) throw new Error(`command handler is not registered: ${command}`);
      return session.handleRequest(method, params);
    }
    if (method.startsWith('extension/views/')) {
      let session = this.sessionForHandle('view', params?.handle);
      if (!session && typeof params?.viewId === 'string') {
        const owner = this.viewOwners.get(params.viewId);
        if (owner) await this.activate(owner, `onView:${params.viewId}`);
        session = owner ? this.extensions.get(owner)?.session : null;
      }
      if (!session) throw new Error(`tree view is not registered: ${params?.handle || params?.viewId}`);
      return session.handleRequest(method, params);
    }
    if (method === 'extension/progress/cancel') {
      return this.sessionForHandle('progress', params?.handle)?.handleRequest(method, params) ?? { accepted: false };
    }
    if (method === 'extension/scm/inputChange') {
      const byOwner = this.extensions.get(String(params?.extensionId || '').toLowerCase())?.session;
      const session = byOwner || this.sessionForHandle('scm', params?.handle);
      return session?.handleRequest(method, params) ?? { accepted: false };
    }
    if (method === 'extension/secrets/didChange') {
      const record = this.extensions.get(String(params?.extensionId || '').toLowerCase());
      return record?.session?.handleRequest(method, params) ?? { accepted: false };
    }
    if (method === 'extension/workspace/didChangeTrust') {
      // これから activate される拡張にも現在の信頼状態で起動させる。既存セッションへは
      // 下の fan-out が届く。session.options は activate 時の spread コピーなので、
      // この 2 経路の両方が必要。
      this.options.workspaceTrusted = params?.trusted === true;
    }
    if (method.startsWith('extension/workspace/')) {
      if (method === 'extension/workspace/didOpen' && typeof params?.snapshot?.languageId === 'string') {
        await this.activateByEvent(`onLanguage:${params.snapshot.languageId}`);
      }
      const sessions = [...this.extensions.values()].map((record) => record.session).filter(Boolean);
      if (method === 'extension/workspace/willSave') {
        const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
        return {
          edits: results.flatMap((result) => result?.edits || []).slice(0, 10000),
          expectedVersion: results.find((result) => result?.expectedVersion !== undefined)?.expectedVersion,
        };
      }
      const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
      return { accepted: results.filter((result) => result?.accepted).length };
    }
    if (method.startsWith('extension/window/')) {
      const sessions = [...this.extensions.values()].map((record) => record.session).filter(Boolean);
      const results = await Promise.all(sessions.map((session) => session.handleRequest(method, params)));
      return { accepted: results.filter((result) => result?.accepted).length };
    }
    if (method === 'extension/languages/provide') {
      const direct = typeof params?.handle === 'string' ? this.sessionForHandle('provider', params.handle) : null;
      if (direct) return direct.handleRequest(method, params);
      const values = [];
      for (const record of this.extensions.values()) {
        if (!record.session) continue;
        const result = await record.session.handleRequest(method, params);
        if (result?.value !== undefined) values.push(result);
      }
      if (values.length === 0) return { value: undefined };
      if (values.length === 1) return values[0];
      return {
        value: values.every((result) => Array.isArray(result.value)) ? values.flatMap((result) => result.value) : values.map((result) => result.value),
        expectedVersion: values[0].expectedVersion,
      };
    }
    throw new Error(`unsupported extension client method: ${method}`);
  }

  async dispose() {
    if (this.disposed) return;
    this.disposed = true;
    for (const record of [...this.extensions.values()].reverse()) {
      if (record.state === 'active' && typeof record.module?.deactivate === 'function') {
        try { await record.module.deactivate(); } catch {}
      }
      if (record.context?.subscriptions) {
        for (const subscription of [...record.context.subscriptions].reverse()) {
          try { subscription?.dispose?.(); } catch {}
        }
      }
      record.session?.dispose();
      moduleApis.delete(normalizeRoot(record.root));
      record.state = 'disposed';
    }
    this.extensions.clear();
    this.commandOwners.clear();
    this.viewOwners.clear();
    maybeUninstallVscodeBridge();
  }
}

module.exports = {
  ACKNOWLEDGED_CONTRIBUTIONS,
  ExtensionLoader,
  MAX_MANIFEST_BYTES,
  acknowledgedContributions,
  contributionCommands,
  contributionConfigurationDefaults,
  contributionKeybindings,
  contributionLanguages,
  contributionMenus,
  contributionSnippets,
  contributionSubmenus,
  contributionViews,
  contributionViewsContainers,
  globToRegExp,
  isWithin,
  readManifest,
  resolveMainEntry,
  safeContributedPath,
  workspaceContainsMatches,
};
