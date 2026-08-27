# 起動パフォーマンスの比較計測

[`measure-startup-performance.ps1`](measure-startup-performance.ps1) は、同じ
Sakura 実行ファイルと同じ Markdown 入力について、プロセス起動から画面表示・文書読込・
全文レイアウトと描画同期までの時点を繰り返し計測する開発者用ベンチマークです。変更前後やビルド構成間の
**比較**に用いるものであり、特定のミリ秒値や全環境での性能を保証するものではありません。

固定入力には [`startup-benchmark-sample.md`](startup-benchmark-sample.md) を使います。この
サンプルは再現可能な合成データであり、機密の社内文書を複製していません。2026-07-30 の
`Desktop\社内規定.md` に由来する比較値は、リポジトリ直下の
[`CLAUDE.md`](../CLAUDE.md) を参照してください。本サンプルへその内容をコピーしては
いけません。現在の固定サンプルは 1,249,037 bytes、5,604 行、SHA-256
`80156fc08b7c91988fd79d7230342862ea0fe534147089ac9e6f5b29461c61b6` です。計測前にこの値または
ファイルハッシュを確認し、入力が変わった結果を同列に比較しないでください。

## C++ / Rust GUI 起動のペア計測

Issue #274 の GUI 起動証拠には [`measure-output-startup.ps1`](measure-output-startup.ps1)
を使います。これは `tests1.exe` の Output provider マイクロベンチマークとは別の計測です。
後者の結果を GUI 起動時間の証拠として扱ってはいけません。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 `
  -CppSakuraExe .\build\evidence\output-startup\cpp\sakura.exe `
  -RustSakuraExe .\build\evidence\output-startup\rust\sakura.exe `
  -StartupSample .\tools\startup-benchmark-sample.md `
  -AffinityMask 1 `
  -CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1
```

既定では各バックエンドについて 5 回の warmup と 30 回の measured launch を行います。
各スロットでは C++ / Rust を交互に先頭へ置く決定的な順序で起動し、同じ固定サンプル、
同じ fresh-per-launch プロファイル方針、同じ非ゼロ CPU affinity mask を使います。qualified mode では
各 backend の canonical runtime stage と `.sakura-runtime-stage.json` receipt が必須で、receipt が列挙した
全ファイルを計測用 campaign bundle にコピーし、サイズと SHA-256 を各起動前と campaign 終了後に
再検証します。bundle 直下へ UTF-16LE BOM/CRLF の `sakura.exe.ini` を生成して
`[Settings] MultiUser=0` を厳密に検証します。これにより `-PROF` は bundle 内だけへ解決され、
`%APPDATA%` や利用者の設定へフォールバックしません。sidecar が欠落・改変・再解析点の場合は
起動せず fail closed します。隣接 DLL を探索して暗黙にコピーすることはありません。exe と sidecar
だけの fallback は `-CollectOnly` に限定され、dependency closure の証拠にはなりません。
プロセスは suspended 状態で作成し、run-owned の kill-on-close Job Object へ割り当て、CPU affinity を
設定して read-back した後にだけ resume します。bundle を working directory とし、cleanup 後は Job
membership と exact bundle image path の両方で残存がないことを確認します。

既定の qualified mode では各 backend の warmup 5 回 / measured 30 回を下回る指定を拒否します。
GUI の疎通だけを確認する場合は `-CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1` を明示的に
指定できますが、その結果は `acceptance.qualified=false` および `pass=false` となり、性能証拠には
なりません。

出力 JSON は payload-free です。commit、C++ / Rust artifact、host、sample、profile policy と
各 launch の profile の SHA-256、sidecar contract、bundle の source/copy ハッシュ前後と cleanup、
起動マイルストーンの中央値 / p95、失敗種別と cleanup の結果
だけを含み、絶対パス、本文、ウィンドウキャプション、コマンドライン、例外本文、環境変数の値は
含みません。失敗またはプロセス残存の launch は `excluded=true` の typed record として残りますが、
統計には入りません。親プロセスを先に identity 再照合して終了させる cleanup は全体で 3 秒に制限され、
残存があれば証拠全体を不合格にします。

いずれかの launch で process cleanup または profile cleanup を検証できなかった場合、後続の
backend を起動せず、その時点で campaign を停止します。JSON の `termination` に typed な
`cleanup-unverified`、完了数、抑止数を残し、`acceptance.qualified` と `pass` は false になります。
これにより残存プロセスや汚染された profile による後続 launch の増殖を防ぎます。

`acceptance.qualified` は必要な launch 数と cleanup がそろった収集判定です。qualified mode は
`-CppBuildManifest` / `-RustBuildManifest` と
`-CppRuntimeStageDirectory` / `-RustRuntimeStageDirectory` を必須とします。manifest は現在の source
state、artifact、Output/UTF-16 selector、Debug/Release、runtime receipt と dependency closure、Windows
image、power mode、parallelism、MSVC/Rust toolchain、Cargo lock、package plan、build command の identity
を含み、runner は二つの manifest の共通条件も照合します。build 後に手書きで補うものではありません。
再現可能な manifest producer が未用意なら `-CollectOnly` だけを使い、qualified 証拠とは扱いません。

これとは別に `performance` が measured `documentReadyMs` の C++ / Rust paired delta を集計し、Rust の
median は相対 2% かつ絶対 1 ms、p95 は相対 5% 以内というゲートを評価します。トップレベルの
`pass` は両方を満たした場合だけ true です。実行した二つの PowerShell スクリプト、CPU
manufacturer/model、OS version、physical/logical core 数も hash とともに記録されます。runner は
`startupGatePass` と明示的な `adoption.decision=HOLD` / `adoptionEligible=false` を別々に残します。
起動ゲートが通っても、Issue #274 の correctness、provider workload、build/package、Debug/Release、
MinGW、複数 hardware のゲートを代替せず、Rust default の採用を意味しません。

実機計測を行わずに順序、統計、スキーマ、affinity metadata、PID identity / parent-first cleanup
helper を確認するには次を使います。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 -SelfTest
```

`-SelfTest` は GUI を起動しません。通常の `measure-startup-performance.ps1` は単一の
`sakura.exe` のマイルストーン比較用であり、ペア証拠の既定 35 回 / backend の代用ではありません。

## 前提条件

- Windows 上で実行すること。対象の Release または Debug の `sakura.exe` が実在し、起動
  できること。
- 計測を始める前に、**同じ実行ファイル**を使う Sakura プロセスが存在しないこと。別ビルド
  や別パスのプロセスも、比較結果に影響し得るなら終了して条件を記録します。
- OS ファイルキャッシュの状態、電源モード、バックグラウンド負荷（ウイルス対策、同期、
  ビルド、ブラウザー等）を結果と一緒に記録すること。同一条件での変更前後比較を優先します。
- 入力は 31 物理行以上にします。本文レイアウト完了の外部判定が、Sakura の初期スクロール範囲
  （30 行）を誤って準備完了と見なさないためです。固定サンプルはこの条件を満たします。
- `CaptureScreenshot` を使うときは、対象ウィンドウを他のウィンドウで覆わないでください。実際に
  画面へ見えているウィンドウ矩形を保存するためです。

スクリプトは既定で試行ごとの一意なプロファイルを用います。通常の開発プロファイルを
ベンチマーク用に指定したり、結果だけを見てキャッシュ状態の異なる試行を混在させたりしないで
ください。

## 実行例

リポジトリのルートから実行します。パスは手元のビルド出力に合わせて置き換えてください。

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5
```

既存プロファイルとの比較を追加する場合:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5 -CompareExistingProfile
```

表示結果を確認する画像も残す場合:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 `
  -SakuraExe .\x64\Release\sakura.exe `
  -SampleMarkdown .\tools\startup-benchmark-sample.md `
  -Iterations 5 -CaptureScreenshot
```

PowerShell の計測補助だけを確認するセルフテストは次のように実行します。

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure-startup-performance.ps1 -SelfTest
```

セルフテストは JSONL のスキーマ検査、QPC 差分からの `firstContentPaintedMs` の算出、破損行の
隔離も確認します。Sakura 自体は起動しません。

## アプリ内スタートアップトレース

計測スクリプトは、各 `condition` / `iteration` ごとに出力先の下へ専用の
`startup-trace-<runId>-iteration-<n>-<condition>` ディレクトリを作成し、対象プロセスへ
`SAKURA_STARTUP_TRACE_DIR` として渡します。アプリケーションは、この**既存のディレクトリ**が
明示的に指定されたときだけ、プロセスごとの `startup-trace-<pid>.jsonl` を書きます。エディタと
非表示コントロールプロセスは同じディレクトリに個別ファイルを出すため、プロセス間の待機や
ready 通知も同じ測定単位で相関できます。

この環境変数が未設定、空、または既存ディレクトリを指さない通常起動では、アプリはトレース
ファイルやディレクトリを作成せず、イベント記録もしません。したがって通常の利用・配布ビルドの
起動経路にファイル I/O を追加しません。旧バイナリなど内部トレース未対応の実行ファイルをこの
スクリプトで測定した場合も、外形計測は有効で、`startupTrace.records` が空になります。

各 JSONL 行は schema version 1 の次の固定フィールドです。QPC は Windows 全体で単調な高分解能
カウンタであり、`qpc / frequency` の比較には同じ `frequency` を確認します。

| フィールド | 内容 |
|---|---|
| `schemaVersion` | 固定値 `1`。未知の値は集計対象にしません。 |
| `qpc`, `frequency` | イベント時刻と QPC 周波数。 |
| `pid`, `tid`, `role` | プロセス、スレッド、役割（`editor` / `control` など）。 |
| `event` | フェーズ境界・選択理由・同期状態を表す安定した ASCII 名。 |
| `value1`, `value2`, `detail` | 数値の補助値と短い分類文字列。レイアウト worker 数・判定理由などを格納します。 |

スクリプトは run-owned の全 Sakura プロセスが終了したことを確認してから JSONL を読みます。各 run の
`startupTrace` には `records` と `majorPhases`、既知の begin/end 組から算出した `phaseDurations`、解析
エラー、最初の `first_content_painted` レコード、および QPC 周波数が互換なときの
`firstContentPaintedMs` を統合します。`majorPhases` は将来のイベント名追加を取りこぼさないよう、現在は
記録された順序付きイベント集合そのものです。壊れた行は
`invalidLineCount` / `parseErrors` に隔離し、外形タイミングを黙って書き換えません。

`firstContentPaintedMs` は、起動描画トランザクションのコミット後、主本文ビュー（index 0）が実際の
レンダリングを完了して初めて記録する内部時点です。外部観測の `documentReadyMs` は全文レイアウトを
スクロール範囲で確認する完了時点なので、同じ指標ではありません。どちらも残し、初回の見え始めと全
レイアウトの回帰を別々に比較します。

### `phaseDurations` とイベントの補助値

`phaseDurations` は、同一 QPC 周波数で対応する begin/end レコードを結べた場合だけ算出します。通常の
同一プロセス phase は `factory`、`control_spawn`、`control_wait`、`control_ready_event`、`editor_spawn`、
`editor_wait`、`editor_ready_event`、`uipi_check`、`read`、`layout`、`startup_document`、
`startup_draw_commit` です。`read` は初期文書の同期読込、`layout` はその初期レイアウト、`uipi_check` は
エディタとコントロール間の UIPI 確認、`startup_draw_commit` は表示・一回のサイズ処理・再描画をまとめた
コミットを表します。`control_wait` / `editor_wait` は対応イベントを待った時間、`*_ready_event` は
`SetEvent` 呼出し自体の時間です。

さらに `control_ready_handoff` は control の `control_ready_event_end` から editor の
`control_wait_end`、`editor_ready_handoff` は editor の `editor_ready_event_end` から control の
`editor_wait_end` までを、PID ではなく役割で対応付けます。これは `SetEvent` が終わってから相手
プロセスが待機を抜けるまでのクロスプロセス handoff であり、個々の待機／通知 phase と混同しません。

`value1` / `value2` はイベント固有の結果を補います。生成成功などの真偽結果と
`*_ready_event_end`、`startup_draw_commit_end` では `value1` が成功 `1`、失敗 `0`、イベントハンドルまたは
IPC HWND が存在しない明示的な経路では `-1` です。後者の通知イベントの `value2` は Win32 error、コミットが
途中で中止された場合も `value2` は中止理由の Win32 error です。`*_wait_result` は `value1` に待機 API の
結果コードを持ちます。`uipi_check_end` は応答を得たかを `value1`（`1` / `0`、tray HWND なしは `-1`）に、
送信 error を `value2` に入れます。`layout_decision` は `value1` が行数、`value2` が worker 数、`detail` が
`below_minimum_lines` / `range_based_color` 等の固定理由です。未定義のイベント値を全イベント共通の成功値と
解釈してはいけません。

起動文書の内訳は、次の集計イベントで確認します。時間はすべて QPC tick であり、同じレコードの
`frequency` で換算します。`read` は読込全体、`decode` と `line_build` はその内訳なので、単純加算して
トランザクション全体と解釈してはいけません。

| イベント | `value1` | `value2` / `detail` |
|---|---|---|
| `startup_document_subphase_summary` | subphase の合計 QPC tick | 処理回数 / `pre_read_settings`, `read`, `decode`, `line_build`, `layout`, `post_load_finalize`, `workbench_ui`, `draw_commit` |
| `startup_read_decision_summary` | 入力 byte 数 | 有効な行境界 partition 数 |
| `startup_read_result_summary` | 公開した論理行数 | `EConvertResult` |
| `startup_read_worker_summary` | worker の合計 QPC tick | 完了した worker 計測数 |
| `startup_read_worker_lifecycle_summary` | 起動した worker 数 | 回収した worker 数 |
| `startup_read_transfer_summary` | 行 buffer の copy 回数 | move 回数 |
| `startup_layout_input_summary` | レイアウト対象行数 | 折返し幅 |
| `startup_minimap_cache_summary` | キャッシュ hit 数 | miss 数 |
| `startup_minimap_build_summary` | 再構築の合計 QPC tick | 走査したレイアウト行数 |
| `startup_make_one_line_summary` | `_MakeOneLine()` の合計 QPC tick | 呼出回数 |
| `startup_make_one_line_work_summary` | `_MakeOneLine()` の呼出回数 | 入力 UTF-16 code unit 数 |
| `startup_make_one_line_cost_summary` | 分類別の合計 QPC tick | 処理回数 / `kinsoku_and_word_inclusive`, `color_boundary`, `character_width`, `layout_allocation` |

`startup_read_worker_lifecycle_summary` の両値が一致しない結果は、速度比較に使わず読込終了経路を調査します。
`kinsoku_and_word_inclusive` は内側の文字幅・レイアウト生成を含み得るため、分類別 QPC tick も互いに排他的な
合計として扱いません。

`isa_dispatch` はプロセスごとに起動直後の一度だけ記録します。`value1` は選択した実装
（`1` = AVX、`2` = AVX2、`3` = AVX-512F/BW）、`value2` は CPUID・XGETBV と
ディスパッチ表初期化に要した QPC tick 数です。ミリ秒へ換算するときは、同じレコードの
`frequency` を使って `value2 * 1000 / frequency` とします。

トレースには文書本文、文書パス、ウィンドウキャプション、ユーザー設定値などの機密情報を入れては
いけません。`detail` は `range_based_color` のような固定分類値に限定します。保存された JSONL と
`report.json` を外部共有するときも、通常の出力に含まれる実行ファイル・サンプルの絶対パスを確認し、
機密入力を使った結果はリポジトリへコミットしません。

## 現状の起動ロジックと順序

通常起動の `sakura.exe -PROF=<name> <markdown>` は、必要なら同じプロファイル用の非表示
コントロールプロセスを先に起動し、その初期化を待ってからエディタウィンドウを作ります。現在の重要な
順序は、**子ビューと各バーの生成後に起動描画トランザクションを開始し、workbench の枠組み初期化と
初期文書の同期ロードを非表示・描画抑止のまま完了してから、一度だけ表示・描画をコミットする**ことです。
起動時の空文書に依存するアウトライン解析と拡張向け文書公開は実行せず、実文書の初回描画後に一つの
内部メッセージへまとめて完了します。

```mermaid
sequenceDiagram
    autonumber
    participant B as 外部ベンチマーク
    participant E as sakura.exe<br/>エディタプロセス
    participant F as CProcessFactory
    participant C as sakura.exe -NOWIN<br/>コントロールプロセス
    participant W as CEditApp / CEditWnd
    participant L as CDocFileOperation / CLoadAgent
    participant V as 主本文 CEditView
    participant Q as エディタのメッセージキュー

    B->>E: Process.Start("-PROF=&lt;name&gt; &lt;markdown&gt;")
    E-->>B: プロセス情報を返す
    Note right of B: processApiReturnMs<br/>子の初期化とは並行し得る
    E->>F: wWinMain → Create(command line)
    F->>F: コマンド解析・プロファイル確定
    F-->>E: CNormalProcess
    E->>E: Run → InitializeProcess

    alt 同じプロファイルのコントロールプロセスがない
        E->>C: CreateProcess("-NOWIN -PROF=&lt;name&gt;")
        C->>C: mutex・共有データ・tray window を初期化
        C->>C: SetEvent(control ready)
        C-->>E: control_ready_handoff<br/>待機解除
    else 既に存在する
        E->>E: コントロールプロセス起動を省略
    end

    E->>W: CEditApp::Create → CEditWnd::Create
    W->>W: CreateWindowEx("TextEditorWindow…")
    Note right of W: topLevelHwndMs
    W->>Q: SetTimer(IDT_FIRST_IDLE, 0)
    W->>W: view・bar・workbench を生成
    W->>W: BeginStartupDrawTransaction<br/>全初期 view の描画を抑止
    W->>C: UIPI check（tray HWND が無い場合も結果を記録）

    E->>W: OpenDocumentWhenStart
    W->>L: FileLoadWithoutAutoMacro
    L->>L: Check → Before → Load
    L->>L: 同期読込（read）→ SetLayoutInfo / 同期 layout
    L->>L: After → CLoadAgent::OnAfterLoad
    L->>W: UpdateCaption → SetWindowText
    Note right of W: captionReadyMs
    L->>L: Final → caret・scroll bar 更新<br/>再描画は抑止中

    E->>W: CommitStartupDrawTransaction
    W->>W: ShowWindow
    Note right of W: visibleMs
    W->>W: 保留した WM_SIZE / redraw を一回の OnSize2 に集約
    W->>W: 全 view の描画を復元<br/>scroll bar range を非描画で同期
    W->>W: startup document trace を complete
    W->>V: RedrawWindow(... RDW_UPDATENOW)
    V->>V: 実際の主本文レンダリング完了
    V-->>W: first_content_painted
    Note right of W: firstContentPaintedMs
    W->>W: 以前のタブはこの時点まで前面に残し、その後 hide
    B->>B: DwmFlush
    Note right of B: dwmFlushMs

    E->>Q: MainLoop
    Q->>W: WM_TIMER(IDT_FIRST_IDLE)
    W->>W: ready 設定・extension service 開始<br/>MYWM_FIRST_IDLE を post → KillTimer
    W->>Q: MYWM_COMPLETE_STARTUP_WORKBENCH を post
    Q->>W: 実文書を extension へ公開<br/>表示中なら outline を一度だけ解析
    B->>E: WaitForInputIdle
    Note right of B: inputIdleMs<br/>OS queue idle。IDT_FIRST_IDLE の証明ではない
    B->>W: 縦 scrollbar range を poll
    Note right of B: documentReadyMs<br/>全物理行を含む layout range を確認
    B->>B: CopyFromScreen（指定時）
    Note right of B: スクリーンショットで最終表示を確認
```

根拠となる主な実装位置は、エントリが
[`WinMain.cpp:50`](../sakura_core/_main/WinMain.cpp#L50)、プロセス選択と control ready の待機が
[`CProcessFactory.cpp:49`](../sakura_core/_main/CProcessFactory.cpp#L49) と
[`CProcessFactory.cpp:234`](../sakura_core/_main/CProcessFactory.cpp#L234)、editor ready の待機が
[`CControlTray.cpp:1291`](../sakura_core/_main/CControlTray.cpp#L1291) です。初期化 mutex の
`WAIT_FAILED` はその場で失敗終端となり、`WAIT_ABANDONED` は mutex を保持したまま control 側へ
`MYWM_RECOVER_APPNODE` を同期送信して stale AppNode を復旧できた場合だけ続行します
([`CNormalProcess.cpp:166`](../sakura_core/_main/CNormalProcess.cpp#L166)、
[`CNormalProcess.cpp:593`](../sakura_core/_main/CNormalProcess.cpp#L593))。

`CEditWnd::Create` は子ビュー／bar／workbench の生成後に描画トランザクションを開始し、UIPI check は
tray HWND がない分岐でも明示的に終端イベントを残します
([`CEditWnd.cpp:1860`](../sakura_core/window/CEditWnd.cpp#L1860)、
[`CEditWnd.cpp:1931`](../sakura_core/window/CEditWnd.cpp#L1931))。起動ファイルの
`OpenDocumentWhenStart` は描画抑止中に `FileLoadWithoutAutoMacro` を同期実行します
([`CEditWnd.cpp:1978`](../sakura_core/window/CEditWnd.cpp#L1978))。実読込と `SetLayoutInfo`、タイトル更新、
Final 時の抑止判定は [`CLoadAgent.cpp:234`](../sakura_core/agent/CLoadAgent.cpp#L234) と
[`CLoadAgent.cpp:294`](../sakura_core/agent/CLoadAgent.cpp#L294) にあります。

通常の文書起動では [`CNormalProcess.cpp:433`](../sakura_core/_main/CNormalProcess.cpp#L433) の後に
[`CNormalProcess.cpp:517`](../sakura_core/_main/CNormalProcess.cpp#L517) が一回だけ
`CommitStartupDrawTransaction` を呼びます。コミットは `ShowWindow`、許可済みの一回の `OnSize2`、
描画スイッチ復元、全 view の scrollbar range 同期、startup document trace の complete、同期
`RedrawWindow` を順に実行します。抑止中の view `WM_SIZE` では `AdjustScrollBars` が何もしないため、
この明示的な同期を省くと全文 layout が完了していても外部の scrollbar maximum が初期値 `1` のまま
残ることがあります
([`CEditWnd.cpp:438`](../sakura_core/window/CEditWnd.cpp#L438))。主本文の実レンダリング後だけ
`first_content_painted` を発行し、前のタブを隠すのもその後です
([`CEditView_Paint.cpp:761`](../sakura_core/view/CEditView_Paint.cpp#L761)、
[`CEditWnd.cpp:422`](../sakura_core/window/CEditWnd.cpp#L422))。

workbench の初期化では、ロード直後に捨てる空文書のアウトライン解析と文書 snapshot を保留します。
`OnAfterLoad` は起動中の要求を重複させません。起動レイアウト中の `BlockingHook()` が 0 ms の
`IDT_FIRST_IDLE` を先に dispatch する場合もあるため、ready 状態だけでは完了処理を許可しません。
**first-idle、描画トランザクションの `Committed`、主本文の初回描画完了**がすべて成立した時だけ
`MYWM_COMPLETE_STARTUP_WORKBENCH` を一度 post します。各状態を確定する経路から同じ判定を再試行するため、
到達順序には依存しません。受信側は保留フラグを callback より先に消費し、実文書の公開と、右パネルが
表示中かつ outline が展開中の場合だけの解析を完了します。post に失敗した場合は、安全な三状態が成立
した時点で同じ完了処理を同期実行し、close 分岐では全保留・post済みフラグを破棄するため、保留状態が
暗黙の終端になりません
([`CEditWnd.cpp:803`](../sakura_core/window/CEditWnd.cpp#L803)、
[`CEditWnd.cpp:1680`](../sakura_core/window/CEditWnd.cpp#L1680)、
[`CEditWnd.cpp:4474`](../sakura_core/window/CEditWnd.cpp#L4474))。

したがって `visibleMs` はロード前の空の枠ではなく、コミット中の表示要求を観測する値です。ただし
`visibleMs`、`captionReadyMs`、`firstContentPaintedMs` はそれぞれ別の境界であり、全文レイアウトの外部確認を
表す主指標 `documentReadyMs` を置き換えません。起動表示を変更する際は、描画抑止・一回だけの commit・
実描画後までの前タブ保持を壊さず、各境界を分けて比較してください。

## 引数

| 引数 | 用途 |
|---|---|
| `SakuraExe` | 計測対象の `sakura.exe`。実在する Release または Debug 実行ファイルを指定します。 |
| `SampleMarkdown` | 開く固定 Markdown ファイル。通常は `tools/startup-benchmark-sample.md` を指定します。 |
| `Iterations` | 各条件の反復回数。比較の基本は 5 回です。 |
| `CompareExistingProfile` | 各反復で fresh 計測が生成した同じ一意プロファイルを再利用する条件を追加します。普段使いのプロファイルは使用しません。 |
| `CaptureScreenshot` | 各条件の第 1 反復について、全文レイアウト確定直後の表示確認用画像を画面から出力します。対象ウィンドウを覆わないでください。タイミング値の代替ではありません。 |
| `OutputDirectory` | 結果 JSON、必要に応じて画像を置くディレクトリ。未指定時は `~/tmp/sakuracode-startup-performance` です。 |
| `SelfTest` | 実アプリのベンチマークではなく、スクリプトの自己診断を実行します。 |

`-SelfTest` では通常の測定引数を組み合わせません。実測の前に自己診断を通し、実測では
`SakuraExe` と `SampleMarkdown` を明示指定する運用を推奨します。

## 計測時点

各試行は以下のマイルストーンをミリ秒で記録します。これらは「最初のコンテンツピクセル」の
直接計測ではなく、Windows API とアプリケーションの観測可能な状態を組み合わせた比較用の
proxy です。

| マイルストーン | 意味と注意点 |
|---|---|
| `processApiReturnMs` | Sakura 起動要求から、プロセス起動 API がプロセス情報を返すまで。 |
| `topLevelHwndMs` | 対象エディタのトップレベルウィンドウを検出した時点。 |
| `visibleMs` | そのウィンドウが可視状態になった時点。 |
| `dwmFlushMs` | DWM フラッシュ完了時点。合成へ反映を要求した指標であり、画面を人が見た時点そのものではありません。 |
| `captionReadyMs` | `CLoadAgent::OnLoad` のファイル読込と `SetLayoutInfo` の後、`OnAfterLoad` から更新される文書タイトルを外部観測する proxy。first content pixel ではありません。 |
| `inputIdleMs` | `WaitForInputIdle` が完了した時点。アプリ内部の `IDT_FIRST_IDLE` と同義ではありません。 |
| `documentReadyMs` | 本文ビューの縦スクロール範囲が入力の全物理行を含むまで拡張された時点。折返し後の layout 行数は入力の物理行数以上になるため、外部から全文レイアウト完了を確認する主指標です。 |

## 結果 JSON の読み方

出力 JSON のトップレベルは `generatedAtUtc`、`scriptVersion`、`runId`、`reportPath`、
`configuration`、`environment`、`input`、`conditions`、`runs`、`summaries`、
`cleanupVerified` です。`input` は byte 数、行数、SHA-256 を含みます。`runs` の各要素には
`condition`、`iteration`、`processApiReturnMs`、`topLevelHwndMs`、`visibleMs`、`dwmFlushMs`、
`captionReadyMs`、`inputIdleMs`、`documentReadyMs`、`verticalScrollMaximum`、
`startupTrace`、`inputIdleReached`、`success`、`error`、`screenshotPath`、
`processCleanupVerified`、`profileCleanupVerified`、`cleanupVerified` を記録します。`summaries` は
7 マイルストーンごとに `count`、`medianMs`、`p95Ms`（nearest-rank ceiling）、`minMs`、`maxMs`、
`meanMs` を条件別に集計します。
`CaptureScreenshot` 時の画像名は
`startup-performance-<runId>-fresh-iteration-1.png` または
`startup-performance-<runId>-existing-profile-iteration-1.png` です。

`scriptVersion` をスキーマ上の版として扱い、キー名と全フィールドは生成された JSON を正本と
してください。未知の版では、旧版の集計スクリプトで機械的に解釈しません。

最終 `DwmFlush` と layout 後の 2 回目の `WaitForInputIdle` は採用していません。非アクティブな
ベンチマークウィンドウで、それぞれ約 23 秒ブロックする試行を再現したためです。どちらも
`documentReadyMs` で確認済みの本文レイアウト完了より後の compositor / workbench の静止待ちであり、
初回表示の回帰ゲートにすると結果を歪めます。

同じ理由で `PrintWindow` も使いません。大きな本文のウィンドウ 1 枚に約 23 秒かかる試行を再現した
ため、画像は `CopyFromScreen` で実際の可視ピクセルを取得します。スクリーンショット処理は全タイミング
値の記録後に実行されます。

読み取る際は次を守ります。

- 第一の比較値は各マイルストーンの**中央値**です。単発値はスケジューリングやキャッシュで
  大きく揺れます。
- 各試行の成功数、最小値・最大値（range）、失敗理由、プロセス／プロファイルの cleanup 結果も
  確認します。残存がある試行は成功として比較に混ぜません。
- `n=5` は軽量な比較のための標本です。`p95`、SLO、統計的な性能保証を主張する根拠には
  なりません。必要なら試行数を増やし、環境条件を固定して別途分析します。

## fresh と existing の意味

通常の **fresh** 条件では、入力した `sakura.exe` を計測専用の artifact bundle へコピーし、
bundle と同じ階層に UTF-16LE BOM/CRLF の `sakura.exe.ini`（`[Settings] MultiUser=0`）を
生成します。試行ごとに一意なプロファイルを bundle 内へ作って Sakura を起動するため、
`%APPDATA%` や利用者の設定へフォールバックしません。sidecar の欠落・改変・再解析点は
起動前に拒否されます。bundle は exe と sidecar だけで、DLL や隣接リソースを暗黙にコピーしません。
これは設定・履歴による差を抑え、再現性のある変更比較の基準にします。

`-CompareExistingProfile` を付けると、各反復の fresh 起動で作成・保存された同じ一意プロファイルを
もう一度使う **existingProfile** 条件も併記します。これは「設定ファイルが既にある 2 回目の起動」
との差を見るものであり、普段使いのプロファイルを読み込む機能ではありません。履歴や保存済み設定の
影響を含み得るため、結果では条件を区別し、existingProfile の値で fresh ベースラインを置き換えないで
ください。

一意なプロファイルと artifact bundle は試行終了後に削除されます。source/copy の SHA-256 は
前後で照合し、sidecar の contract と bundle cleanup も結果へ記録します。削除失敗、または
Sakura／その子プロセスの残存は失敗として扱い、原因を解消してから再計測します。

## 安全性と後始末

計測は全待機に timeout を設けます。timeout、ウィンドウ検出失敗、文書タイトル未到達、全文レイアウト未到達、入力待機
失敗、または cleanup 後の survivor は試行失敗です。スクリプトが終了できるのは、成功または明示的な
失敗という終端状態に到達した場合だけです。

cleanup の対象は当該試行が所有する Sakura プロセスと、その run-owned 子プロセスに限られます。
無関係な Sakura、他アプリケーション、ユーザーの作業中プロセスを kill しません。既定の出力先が
`~/tmp/` 配下なのも、リポジトリと通常のユーザーデータを汚さないためです。

終了は二段階です。まず可視の編集ウィンドウへ `WM_CLOSE` を送り、全ウィンドウの消滅を確認して
finalization を完了させます。次に、同じ試行が所有する非表示コントロールウィンドウへ終了要求を送り
ます。可視ウィンドウを所有していた PID がそのまま制御役として残る場合もあるため、PID の終了ではなく
可視ウィンドウの消滅を境界にします。非表示制御プロセスは編集ウィンドウがなくても常駐し得るため 1 秒の
猶予に限定します。期限内に残った場合だけ、親から先に強制終了し、最後に PID・生成時刻・実行パスを
再照合します。可視ウィンドウが応答しない場合も、測定専用プロファイルと未編集の固定入力に限った処理
なので、正常終了の総猶予は 3 秒です。

## 再現性の記録テンプレート

比較結果を Issue、PR、または検証記録へ残すときは、少なくとも次を記録してください。

```text
commit:
SakuraExe: （絶対パス、Release/Debug、file version、計測日時）
sample: startup-benchmark-sample.md （bytes、lines、hash）
machine / OS:
power mode:
cache state: （cold / warm、前処理の有無）
background load: （実行中の主な負荷）
condition: fresh / existingProfile
per-run: （各マイルストーンの値、成功/失敗）
median / range: （条件・マイルストーン別）
cleanup: （run-owned process と一意プロファイルが残っていないこと）
screenshot: （有無と出力パス）
internal trace: （`firstContentPaintedMs`、主要 phase、worker 数/理由、read/layout/UIPI/control wait/SetEvent の時間、解析エラー）
```

## 既存の Workbench 計測との違い

[`measure-workbench-performance.ps1`](measure-workbench-performance.ps1) は terminal / workbench の
メモリ等を扱う別責務の計測です。本書の startup benchmark は、固定文書を開く Sakura の起動表示・
読込・入力待機の時点を比較するためのものです。目的、入力、指標、結果を相互に混在させないで
ください。
