# デッドコード調査（2026-09-22）

現行コードで、削除候補として根拠を確認できた未使用関数は **8個**、値が読まれない代入は **2か所**。
この調査では製品コードを削除・変更していない。以前のリソース寿命修正とは別の調査結果である。

## 確認できた未使用関数

参照検索はコメント・文字列も含めた保守的な識別子検索であり、下記8個はいずれも定義箇所の1回だけだった。
さらに現在のソースを使った Cppcheck で全8個の `unusedFunction` を再確認した。
対象の全ソースは `sakura_core/sakura.vcxproj` に登録されている。

| 所属・場所 | 関数 | 未使用と判断した根拠 |
|---|---|---|
| `sakura_core/workbench/explorer/CExplorerTool.cpp:214` | `LoadRasterBitmap` | 無名名前空間内の画像読み込み処理。定義以外の参照なし。 |
| `sakura_core/terminal/window/CTerminalTool.cpp:910` | `BindPaneModels` | `.cpp` 内で定義された `CTerminalTool::Impl` のメソッド。呼び出しなし。 |
| `sakura_core/terminal/runtime/TerminalRuntimeService.cpp:102` | `FindPaneForInstance` | 無名名前空間内のペイン検索。定義以外の参照なし。 |
| `sakura_core/workbench/scm/CScmWorkbenchTool.cpp:1114` | `ListTop` | `.cpp` 内の `Impl` の補助メソッド。呼び出しなし。 |
| `sakura_core/workbench/scm/CScmWorkbenchTool.cpp:1861` | `FormatHistoryDate` | 同じ `Impl` の日付整形メソッド。呼び出しなし。 |
| `sakura_core/theme/CColorThemeRegistry.cpp:172` | `KindForUiTheme` | 無名名前空間内のテーマ種別変換。定義以外の参照なし。 |
| `sakura_core/accessibility/CustomUiAutomationProvider.cpp:278` | `ClientBounds` | private の非仮想メソッド。COM インターフェースの実装ではなく、呼び出しもない。 |
| `sakura_core/prop/CPropComKeybind.cpp:75` | `CPropComKeybindWndProc` | コールバック形式だが、宣言・呼び出し・登録・エクスポートが見つからない。実際の登録先は `CPropCommon.cpp:205` の `CPropKeybind::DlgProc_page`。 |

`LoadRasterBitmap` は WIC デコード、縮小、DIB 作成を含むまとまった処理で、最初に整理する対象として分かりやすい。
未使用関数の削除による実行速度・メモリ使用量の改善は測定していない。今回確認できた効果は保守対象の削減であり、リークの証拠ではない。

## 値が読まれない代入

変数自体は使用されているため、変数全体を削除する対象ではない。

| 場所 | 対象 | 制御フロー上の根拠 |
|---|---|---|
| `sakura_core/convert/CConvert_SpaceToTab.cpp:28` | `bSpace = FALSE` の初期値 | 使用前に行処理内の58行目で必ず再代入される。変数のスコープを使用箇所に寄せる整理も可能。 |
| `sakura_core/doc/layout/CTsvModeInfo.cpp:60` | 最終フィールド処理の `nField++` | 増分後に値が読まれずループ本体が終了する。次の行では `nField` が新たに0で初期化される。 |

現在の Cppcheck でも両方を `unreadVariable` として再確認した。

## 検証と対象範囲

| 基準 | Verify | Expect / 実績 |
|---|---|---|
| 定義以外に参照がない | 識別子インデックスと下記の `rg` 参照検索を照合 | 各関数につき定義1か所。全8個で一致。 |
| 現行ソースでも指摘される | 下記の Cppcheck コマンドを実行し、XML の関数名と `unreadVariable` の場所を照合 | 8関数・2代入の全10件を再検出。解析終了コード0。 |
| ビルド除外ファイルを混同しない | Git 管理下の core `.cpp` と、生成済みコンポーネントを含む `.vcxproj` の `ClCompile` を照合 | 699ファイル中698ファイルが登録済み。残り1個は下記の意図的な除外。 |
| 解析プロセスが残らない | `Get-CimInstance Win32_Process -Filter "Name = 'cppcheck.exe'"` | 解析完了後に該当プロセスなし。 |

識別子照合は、Git 管理下と未追跡の非 ignore ファイルを合わせた3,084ファイルを対象に実施した。
C/C++、ヘッダー、リソース、プロジェクト、設定、スクリプト、文書を含み、`externals/`、`tools/vcpkg/`、`.codex/`、`.claude/` は除外した。
既存の Debug / Release 解析結果は候補発見にのみ使用し、今回の結論は現在の参照と再解析で確認した。
公開ヘッダーの未使用 API、仮想関数、条件付き構成のコードは、参照が少ないという理由だけでは削除候補として確定していない。

未登録の1個は `sakura_core/terminal/vendor/windows_terminal/src/terminal/parser/tracing.cpp`。
`sakura_core/terminal/CLAUDE.md` が ETW tracing の除外を明記し、CMake も vendor ソースを明示的に選択している。
このファイルは放置された自作ソースとして数えない。未追跡の作業中ソースも、登録漏れの判定から除外した。

今回再解析したのは下記9翻訳単位であり、全構成・全言語の到達可能性を網羅した調査ではない。
一部翻訳単位だけの `unusedFunction` は単独では証明にならないため、実装内の可視性、全体の参照検索、コールバック登録を併せて判定した。
製品コードを変更していないため、この調査のためのビルド・単体テストは実行していない。

### 再解析コマンド

参照検索:

```powershell
$symbols = 'BindPaneModels|CPropComKeybindWndProc|ClientBounds|FindPaneForInstance|FormatHistoryDate|KindForUiTheme|ListTop|LoadRasterBitmap'
rg -n -w $symbols sakura_core src/main src/test tools .github
```

リポジトリルートの PowerShell で実行する。`cppcheck-build-dir` はあらかじめ作成する。
TKW の新規実行では新しい request ID を使う。過去の実行結果を取得するだけなら再実行せず、保存済み request を参照する。

```powershell
$auditRoot = Join-Path $env:USERPROFILE ('tmp/sakura-dead-code-audit-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force "$auditRoot/cppcheck-cache" | Out-Null
$auditRequest = [guid]::NewGuid().ToString('N')
Set-Content -LiteralPath "$auditRoot/request-id.txt" -Value $auditRequest -Encoding ascii
& "$env:LOCALAPPDATA/Programs/TKW/tkw.exe" run --caller codex `
  --request-id $auditRequest --reason new-diagnostic `
  --mode auto --timeout-ms 180000 -- 'C:/Program Files/Cppcheck/cppcheck.exe' `
  --force --enable=unusedFunction,style --xml --platform=win64 `
  -D_WIN64 -U_M_IX86 -D_M_X64 -D_DEBUG --quiet -j 4 `
  --project=sakura_core/sakura.vcxproj '--project-configuration=Debug|x64' `
  "--cppcheck-build-dir=$auditRoot/cppcheck-cache" `
  '--file-filter=*CTerminalTool.cpp' '--file-filter=*TerminalRuntimeService.cpp' `
  '--file-filter=*CColorThemeRegistry.cpp' '--file-filter=*CScmWorkbenchTool.cpp' `
  '--file-filter=*CustomUiAutomationProvider.cpp' '--file-filter=*CExplorerTool.cpp' `
  '--file-filter=*CPropComKeybind.cpp' '--file-filter=*CConvert_SpaceToTab.cpp' `
  '--file-filter=*CTsvModeInfo.cpp' "--output-file=$auditRoot/cppcheck-current.xml"
```

今回の完了済み実行の request ID は `dc0922a2a2244aaab016dbe1608d0804`。
ローカル証拠は `~/tmp/sakura-dead-code-audit-20260922/` の `cppcheck-current.xml`、`sparse-reference-candidates.json`、`all-reference-counts.json`、`source-registration.json` に保存した。

## IRV と責務への影響

- **A. 実際に必要だった範囲:** Explorer、Terminal、SCM、Theme、Accessibility、キー設定、文字変換、TSV の上記9ソース、呼び出し・コールバック登録、MSBuild/CMake のソース所有、既存解析結果。横断検索の対象数と実装を精読した対象数は区別する。
- **B. 調査範囲の拡大:** 「参照なし」と「ビルド未登録」を区別するため、生成されたコンポーネントのプロジェクト登録と `$(SakuraRepoRoot)` の解決まで確認した。従来の単一アプリプロジェクトだけでは、抽出済みコンポーネントを誤判定する。
- **C. アーキテクチャへの影響:** 変更なし。関数・状態の所有者、公開契約、依存方向は維持している。
- **D. 次回の IRV:** 同じ候補の調査は、この一覧、対象関数、登録箇所から開始できる。全体の候補探索を繰り返す必要はないが、削除時には最新の参照と構成を再確認し、ビルド・関連回帰テストを行う必要がある。
