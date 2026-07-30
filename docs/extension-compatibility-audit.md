# Open VSX 拡張互換性監査

調査日: 2026-07-30
対象: 拡張ホスト設計で候補にした Open VSX の現行 4 パッケージ

## 結論

4 パッケージはいずれも、当初定義した Tier 0 API だけでは動かない。最初の実装では独自の最小 fixture で host／activation／RPC を検証し、実物拡張は要求 API に応じた互換レベルへ分けて固定バージョンで評価する。

| 拡張 | 監査版 | 最低ランタイム | 判定 | 主な追加要件 |
| --- | ---: | --- | --- | --- |
| `EditorConfig.EditorConfig` | 0.18.2 | Node.js 20+ / CommonJS | Tier 0.5 候補 | workspace folder、save events、completion、`workspace.fs`、WASM asset |
| `esbenp.prettier-vscode` | 12.4.0 | ESM-capable Node.js | Tier 1 以降 | formatting、code action、watcher、workspace trust、status UI、child process |
| `DavidAnson.vscode-markdownlint` | 0.62.0 | Node.js bundle | Tier 1 以降 | diagnostics、code action、watcher、tasks、workspace folder/trust、child process |
| `streetsidesoftware.code-spell-checker` | 4.6.0 pre-release | Node.js 22.20+ | Tier 2 以降 | language client、TreeView、WebView、Terminal、watcher、file events、notebook APIs |

この分類は各機能の価値ではなく、ホストの最小 API 面からの距離を表す。bundle 内の文字列だけでは既定経路で必ず実行されるとは断定できないため、静的依存は「未実装なら安全に shim または拒否すべき API」として扱う。

## 1. EditorConfig.EditorConfig 0.18.2

- Open VSX: <https://open-vsx.org/api/EditorConfig/EditorConfig/0.18.2>
- entry: `./out/editorConfigMain.js`（CommonJS）
- activation: `onStartupFinished`
- `engines.vscode`: `^1.100.0`
- VSIX SHA-256: `cbc0370ff204bc16d84a3ee6830536fc04355856fa0e895ab064e1a03cfcb84a`

主要 API:

- `commands.registerCommand` / `executeCommand`
- `languages.registerCompletionItemProvider`
- `workspace.getConfiguration`、`workspaceFolders`、`asRelativePath`、`openTextDocument`
- `workspace.onWillSaveTextDocument` / `onDidSaveTextDocument`
- `workspace.fs.stat` / `writeFile`
- `window.activeTextEditor`、active editor／window-state events、output channel、error message
- `TextEdit`、`Position`、`Range`、`EndOfLine`、`Uri`

直接または runtime dependency から Node.js の `fs`、`os`、`path`、`util` と Node.js 20 以上を要求する。`@one-ini/wasm` の約 89 KiB の asset を同期読込する。native addon、child process、WebView、language client は無い。

最初の実物候補としては最も狭いが、`workspaceFolders = undefined` の初期設計とは整合しない経路がある。互換試験では「単一文書だけで成立する保存時処理」と「folder を要求する生成 command」を分け、後者は capability 不足を明示して拒否する。

## 2. esbenp.prettier-vscode 12.4.0

- Open VSX: <https://open-vsx.org/api/esbenp/prettier-vscode/12.4.0>
- entry: `./dist/extension.js`（ESM）
- browser entry: `./dist/web-extension.cjs`
- activation: `onStartupFinished`
- `engines.vscode`: `^1.101.0`
- VSIX SHA-256: `fb730ea4306d09cdc0a3aaa9e9baae28058cc97a4fbfce8b056b377a0639a9fe`

主要 API:

- document/range formatting provider、code action provider、`WorkspaceEdit`
- configuration change、workspace folder／trust
- `workspace.fs.writeFile`
- package/config/editorconfig/ignore 用 filesystem watcher
- status bar、language status、output channel、open dialog、warning message
- active editor event、`Uri`、`Range`、`TextEdit`

Node.js の `fs`、`path`、`os`、`url`、`module.createRequire` と動的 module/plugin 解決を使う。global package 探索では `npm`、`yarn`、`pnpm` を child process として起動する。従って formatter API だけを実装しても通常の workspace 経路は成立せず、watcher、実 filesystem path、process policy が必要になる。

## 3. DavidAnson.vscode-markdownlint 0.62.0

- Open VSX: <https://open-vsx.org/api/DavidAnson/vscode-markdownlint/0.62.0>
- entry: `./bundle.js`
- browser entry: `./bundle.web.js`
- activation: `onLanguage:markdown`
- `engines.vscode`: `^1.97.0`

主要 API:

- document open/change/save/close events
- active editor、selection、visible editor events
- diagnostic collection、code action、range formatting
- configuration、workspace folder／trust、workspace-folder change
- `workspace.fs`、filesystem watcher、`RelativePattern`
- commands、tasks provider／execute、output channel

desktop bundle は `node:child_process` と `execFile` を含む。browser bundle を選んでも diagnostics、watcher、workspace filesystem、tasks と editor events の不足は残る。WebView と language client は無い。

## 4. streetsidesoftware.code-spell-checker 4.6.0

- Open VSX: <https://open-vsx.org/api/streetsidesoftware/code-spell-checker/4.6.0>
- entry: `./packages/client/dist/extension.cjs`
- activation: `onStartupFinished`
- `engines.vscode`: `^1.104.0`
- `engines.node`: `>=22.20.0`

主要 API:

- document lifecycle、editor/selection/theme events、diagnostics、code actions、inline completion
- `workspace.fs`、`findFiles`、watcher、file create/delete/rename events、workspace edit/save
- TreeView、WebView view/panel、Terminal profile／terminal、decorations
- quick pick、input box、message、output channel、external URI、telemetry
- notebook、tab groups、language list
- bundled `vscode-languageclient` 9.x / `vscode-jsonrpc` と child process transport

現行版は初期版の明示的な対象外を複数必要とするため、Tier 0 の負荷試験には使わない。高頻度 document event の性能試験は、同じイベント密度を生成する制御可能な fixture で先に行う。

## 5. 改訂した受け入れ順

1. 最小 fixture: activate/deactivate、command、双方向 RPC、cancel、host loss
2. document fixture: open/change/save/close、version gap、bounded event aggregation、diagnostics
3. EditorConfig 0.18.2 の単一文書保存経路
4. Prettier 12.4.0 の bundled formatter 経路。local/global module 解決は別 scenario
5. markdownlint 0.62.0 の document diagnostics 経路。tasks は別 capability
6. CSpell は language-client と複合 UI capability の実装後に再評価

受け入れテストはバージョン、VSIX hash、使用 entry point、許可 capability を固定する。未対応 API は `undefined` や無反応にせず、拡張 ID と API 名を含む観測可能な `UnsupportedCapability` エラーで終了させる。
