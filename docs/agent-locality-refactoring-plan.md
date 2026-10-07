# AI エージェント局所変更性リファクタリング計画（Sakura Editor NEXT）

- 追跡 Issue: #289
- 基準コミット: `main` `19eb17c58`（2026-09-13）
- 状態: 計画書（コード変更は含まない。各ステップは別 Issue で実行する）
- 検証: `.claude/goal-loop/agent-locality-refactoring-plan/check.py`（11 基準 C1〜C11。本書の数値はすべてこのスクリプトが Workdir で再計測して照合する）

この計画書は 8 節から成る。1 節で目的と非目的、2 節で現状の実測値、3 節で判定規則、4 節で目標構造、5 節で 45 の実行ステップ（`S01`〜`S40d`。`S16a`〜`S16c` と `S40a`〜`S40d` は分割後の独立ステップ）、6 節で継続計測の指標、7 節でリスクと未確定事項、8 節で Issue 分割（`I-01`〜`I-26`）を示す。

---

## 1. 目的と非目的

### 1.1 目的

最終目標は構造を分割することではない。**AI エージェントが 1 つの Issue を理解・調査・変更・検証するために読まなければならない量（IRV: Issue Reading Volume）を、継続的に減らし続けること**である。ディレクトリ分割・契約化・component 化・`CLAUDE.md` の縮小は、すべて IRV を下げるための手段であり、IRV が下がらない分割は本計画では成功と数えない。

> **判定基準: 代表的な Issue について、エージェントが読む必要のある集合（ガイド / 製品ソース / 契約 / テスト / ビルド定義 / 形式仕様）が、着手前より小さく、かつ予算（1.1.2）に収まっているか。**

#### 1.1.1 IRV の定義（一次指標。計測は `tools/architecture/irv.py`、6.1 節）

IRV は「その Issue を安全に変更するために読む必要がある」ファイルの集合であり、**変更したファイルの集合ではない**。集合の各要素は次の 7 区分のいずれかに属し、区分名が「含める理由」になる。変更したという事実だけでは含めない。逆に、変更していなくても安全な変更に必要なら含める。

| 区分（含める理由） | 内容 | 典型例 |
|---|---|---|
| `guide` | Issue の理解に必要なガイド。IRV 集合に入る全ファイル（変更対象だけでなく契約・テストも）の祖先パス上の `CLAUDE.md`（ルート含む）と、`@` import で自動読込されるファイル | `CLAUDE.md`、`sakura_core/workbench/CLAUDE.md` |
| `owning_source` | 変更対象を所有する component（`modules.json`）の製品ソース。所有 component が `sakura_app`（モノリス）の場合は変更対象と同じディレクトリの `.cpp`/`.h` | `sakura_core/workbench/scm/*.cpp` |
| `contract` | 変更に必要な公開契約・port・スキーマ。所有 component の `public_headers`、`modules.json` の `contracts[]`、変更した製品ソースからの前方 include 閉包（2 ホップ。1 ホップでは較正の recall が落ちる。6.2 節） | `sakura_core/include/sakura/editor/EditorFrameEvents.h` |
| `test` | 変更を安全に判定するために必要なテスト。変更ヘッダを include するテスト、名前が鏡写しのテスト、変更ファイルの末端ディレクトリを鏡写しにした `src/test/cpp/tests1/<leaf>/` 直下、所有 component の `*_tests`、`tools/build/pilots/` | `src/test/cpp/tests1/workbench/scm/*.cpp` |
| `manifest` | 必要なビルド定義・component 台帳。ビルド定義を変更した Issue ではビルドドライバ（`tools/build/sakura_build.py`、`sakura_build_lib/*.py`、`src/main/cmake/*.cmake`）も含む | `modules.json`、`sakura.vcxproj`、`test-inventory.json` |
| `formal` | 適用される形式仕様と検証台帳 | `docs/formal/SearchRequestLifecycle.tla` + `.cfg` |
| `dependency_owner` | 変更ヘッダを include する未変更ファイル（1 ホップ、上限 30 件。総数は `dependents_total` に別記） | `CEditWnd.h` を include する 66 ファイル |

出力は単一のスカラーにしない。最低限 `guide_files` / `guide_lines` / `source_files` / `source_lines` / `test_files` / `contract_files` / `formal_files` / `manifest_files` / `component_count` を列で出し、`explain` サブコマンドで各ファイルの含める理由を列挙できることを要件にする。

#### 1.1.2 IRV 予算（典型的なバグ修正 1 件）

| 区分 | 予算 | 現状（6.1 節の代表 Issue。#217 / #266 / #276 = 変更 4 / 4 / 14 ファイル） |
|---|---|---|
| `guide` | **3 ファイル以下、合計 450 行以下** | 超過: 9 / 1,657 行、8 / 1,563 行、13 / 2,068 行（最小の Issue でも予算の約 4 倍） |
| `owning_source` | **2,500 行以下** | 超過: 5,125 行、13,630 行、18,318 行（`CEditWnd.cpp` に触れると 1 本で 16,806 行） |
| `test` | **10 ファイル以下** | 8、9、17（#276 が超過） |

Phase ごとの到達目標は 6.5 節に置く。本計画の各ステップは「そのステップで IRV のどの区分が、どの代表 Issue について、どれだけ減るか」を Expect に書く（書けないステップは構造の整理であって IRV 改善ではないので、その旨を明記する）。

#### 1.1.3 Change Footprint（事後指標）との区別

`tools/architecture/issue_footprint.py` がコミット履歴から計算する値（変更ファイル数、変更ファイルの行数合計、祖先 `CLAUDE.md` バイト、ビルド定義ファイル数、テストファイル数）は、**事後の Change Footprint** であり IRV ではない。Change Footprint は「変更がどこまで広がったか」を示す結果指標として引き続き計測する（6.3 節）が、「読む必要があった量」の推定には使わない。両者の役割:

| | IRV（一次） | Change Footprint（二次・事後） |
|---|---|---|
| 問い | 安全に変更するには何を読む必要があるか | 変更は実際にどこまで広がったか |
| 入力 | Issue の変更対象 + `modules.json` の所有 + include グラフ + テスト配置 | Issue に紐づくコミットの diff |
| 含める根拠 | 7 区分の理由が必ず付く | 変更されたという事実 |
| 主な用途 | ステップの Expect、Phase 完了ゲート、`CLAUDE.md` 予算 | 変更局所性（Change Locality）の追跡、ビルド定義変更の検出 |

#### 1.1.4 12 の指標

判定基準を分解した 12 の指標を、以降の設計・判定・計測に用いる（3.3 節で各指標をステップに対応付ける）。各指標は IRV のどの区分に効くかを右列に示す。

| 指標 | 本書での意味 | 主に効く IRV 区分 |
|---|---|---|
| High Cohesion | 1 つの業務目的・不変条件・状態を共有するコードが 1 つのディレクトリに閉じている | `owning_source` |
| Low Coupling | 他ディレクトリへの参照が契約ヘッダ（`sakura_core/include/sakura/`）経由だけになっている | `contract`、`dependency_owner` |
| Single Responsibility | 1 ファイル・1 クラスが 1 つの変更理由しか持たない | `owning_source` |
| One-way Dependency | include の向きが常に「不安定 → 安定」で、上向き include が 0 に向かって単調減少する | `dependency_owner` |
| Change Locality | 1 Issue の変更が 1〜2 ディレクトリに収まり、ビルド定義ファイルを触らない | `manifest`（Change Footprint でも追跡） |
| Build/Test Isolation | 変更したディレクトリだけをビルドしてテストできる（アプリ全体をリンクしない） | `test` |
| Explicit Contracts | ディレクトリ間の依存が型で表現され、散文（`CLAUDE.md`）に頼らない | `contract`、`guide` |
| Small Agent Context | 1 Issue で読む `CLAUDE.md` が 3 ファイル 450 行以下に収まる | `guide` |
| Independently Testable Modules | ディレクトリごとに専用のテストランナーがあり、`tests1.exe` を待たない | `test` |
| Independently Refactorable Modules | あるディレクトリの内部構造を変えても他ディレクトリのビルドが壊れない | `dependency_owner` |
| Formal Verification Boundary | TLA+ で検証した状態機械とコードの対応が機械可読で、乖離がゲートで検出される | `formal` |
| 誤認しにくいディレクトリ構造 | ディレクトリ名から所有者・層・テスト位置が一意に決まり、同じ目的の実装経路が 2 つ存在しない | `owning_source`、`test` |

### 1.2 非目的

- **Clean Architecture への準拠は目的ではない。** 層名や依存規則は「エージェントが読む量」を減らすための道具であり、形式が目的になる場合はその規則を捨てる。
- **書き直しはしない。** すべてのステップは既存コードの移動・分割・契約化であり、機能追加や挙動変更を含まない。
- **VS Code 互換ルールは変えない。** ルート `CLAUDE.md` の最上位ルール（本物の VS Code 挙動互換）は本計画の全ステップに優先する。View の登録経路を変える S31 では、View ID・既定配置・キーバインドを移行前後で突合する。
- **上流（sakura-editor/sakura）には一切触れない。** 本計画は fork `tsuyoshi-otake/sakura-editor-next` の `main` だけを対象にする。
- **既存基盤を再発明しない。** #15 の L4 component build（`src/main/modules/modules.json`、`py -3 tools/build/sakura_build.py graph check --all-contexts`）、#18 の semantic inventory ratchet（`py -3 tools/build/sakura_build.py inventory semantic --strict`）、`docs/formal/` の TLA+ 検証をそのまま使い、計測対象を広げる。

### 1.3 読み方

- 各ステップ（5 節）は `- 対象:` / `- 内容:` / `- 新しい境界:` / `- テスト:` / `- Verify:` / `- Expect:` / `- リスク:` / `- ロールバック:` を持つ。`Verify` は Workdir で実行できるコマンド、`Expect` は観測できる値である。Expect にはそのステップ単独で再現できる値だけを書き、複数ステップの累積でしか到達しない値は Phase 完了ゲート（Phase 3 の E3 など）に置く。ステップ ID の英字接尾辞（`S16a`〜`S16c`、`S40a`〜`S40d`）は分割後の独立ステップで、それぞれ別 Issue で実行する。
- 数値はすべて `19eb17c58` での実測値である。「【推定】」と書いた値だけが見積もりである。
- 「（新規）」を付けたパスはまだ存在しない。付いていないパスは存在することを check.py の C4 が確認している。
- 各ステップの実行時は、必ず `.claude/memory/rules.md` を先に読む（ビルド・テスト・CI の既知の落とし穴が記録されている）。

---

## 2. 現状ベースライン

### 2.1 計測表（check.py C5 が再計測して照合する）

| ID | 指標 | 実測値（`19eb17c58`） | 計測コマンド | 本計画の到達目標 |
|---|---|---:|---|---|
| B-01 | `CEditWnd.cpp` 行数 | 16,806 | `wc -l sakura_core/window/CEditWnd.cpp` | 5,500 以下（Phase 3 完了ゲート E3-1。補助指標） |
| B-02 | `CEditWnd.h` 行数 | 1,509 | `wc -l sakura_core/window/CEditWnd.h` | 500 以下（E3-2） |
| B-03 | `sakura.vcxproj` の `<ClCompile Include=` 明示エントリ数 | 662 | `rg -c "<ClCompile Include=" sakura_core/sakura.vcxproj` | 3 以下（S07、glob 化） |
| B-04 | `tests1.vcxproj` の `<ClCompile Include=` 明示エントリ数 | 306 | `rg -c "<ClCompile Include=" sakura_core/tests1.vcxproj` | 3 以下（S05） |
| B-05 | 登録テスト数（`test_count`） | 4,157 | `py -3 -c "import json;print(json.load(open('src/test/test-inventory.json'))['test_count'])"` | 変えない（消失 0） |
| B-06 | テストスイート数 | 452 | `py -3 -c "import json;d=json.load(open('src/test/test-inventory.json'));print(len({t['test_id'].split(':',1)[-1].split('.')[0] for t in d['tests']}))"` | 変えない |
| B-07 | 追跡されている `CLAUDE.md` の本数 | 59 | `git ls-files -- CLAUDE.md "*/CLAUDE.md" \| wc -l` | 70 前後（分割で増える） |
| B-08 | `CLAUDE.md` 合計バイト | 658,157 | `git ls-files -- CLAUDE.md "*/CLAUDE.md" \| xargs wc -c` | 350,000 以下（S35。二次指標。一次は IRV `guide` 予算） |
| B-09 | `modules.json` の component 数 | 40 | `py -3 -c "import json;print(len(json.load(open('src/main/modules/modules.json'))['components']))"` | 52 以上 |
| B-10 | semantic inventory の finding 合計 | 13,881 | `py -3 tools/build/sakura_build.py inventory semantic` → `build/evidence/r0/editor-core-semantic.json` の `metrics.finding_count` | 単調減少（ratchet） |
| B-11 | `GetDllShareData()` 直接呼び出し | 705 | `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.direct_global_getter_calls.GetDllShareData` | 450 以下（S16c） |
| B-12 | `GetEditWnd()` 直接呼び出し | 258 | `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.direct_global_getter_calls.GetEditWnd` | 60 以下（S29） |
| B-13 | `boundary.win32_type` finding | 4,856 | `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.finding_count_by_rule["boundary.win32_type"]` | 単調減少 |
| B-14 | `state.public_mutable_field` finding | 6,297 | `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.finding_count_by_rule["state.public_mutable_field"]` | 単調減少 |
| B-15 | 上向き include エッジ合計 | 402 | `py -3 tools/architecture/include_layers.py report --json` → `upward_edges_total` | 60 以下（S13〜S21） |
| B-16 | `window/CEditWnd.h` を include するファイル数 | 66 | `rg -o --no-filename "#include \"window/CEditWnd\.h\"" sakura_core src \| wc -l` | 12 以下（S29） |
| B-17 | semantic inventory の対象行数 | 553,899 | `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.source_line_count` | 参考値 |

### 2.2 現行ゲートの状態（check.py C6 が再実行する）

`architecture-gates.yml` が main で実行しているゲートを Workdir で再実行した結果。

| ゲート | 結果 | 備考 |
|---|---|---|
| `py -3 tools/build/sakura_build.py graph check --all-contexts` | exit 0 | 40 component、全 context で非巡回 |
| `py -3 tools/build/sakura_build.py generate --check` | exit 0 | `stale: []` |
| `py -3 tools/dependency_ledger.py check` | exit 0 | `src/main/dependencies/dependencies.json` と台帳が一致 |
| `py -3 tools/build/sakura_build.py inventory semantic --strict` | exit 0 | ベースライン `62b851c95`（2026-09-09 受入）に対し新規 finding 0、増加ルール 0。`19eb17c58` で `resource.stop_required_acquisition` の新規 4 件が解消（ルール差分 −22） |

`inventory semantic --strict` は 1 つ前のコミット `e035f0edb` までは **exit 11** だった（rule_id `resource.stop_required_acquisition` の新規 finding 4 件、同ルールの消失 1 件を差し引いた純増 +3）。`19eb17c58`（2026-09-13、#305「Give every stop-required acquisition an owner that releases it」）が 4 件すべてを解消し、基準コミットではゲートは緑である。GitHub Actions の `architecture-gates` も `3c6432c2b`（2026-09-11）から `e035f0edb` まで 7 連続 failure（失敗ステップ `Verify semantic ratchet`）だったが、`19eb17c58` で success に戻った。解消の仕方は S01 が運用規則にする「取得と解放を同じ型に持たせる」の実例なので、履歴として残す:

| ファイル | 行（`e035f0edb`） | rule_id | 内容 | `19eb17c58` での解消 |
|---|---:|---|---|---|
| `sakura_core/platform/controlipc/ControlSenpComposition.cpp` | 451 | `resource.stop_required_acquisition` | `m_worker = std::thread(...)`。494 行に `join()` あり | `foundation::CNativeWorkerThread::Start<CControlSenpComposition, &CControlSenpComposition::Run>(this)`。ハンドル型 `sakura_core/platform/foundation/NativeWorkerThread.h`（move-only、コンストラクタが spawn、デストラクタが join）。スレッド本体は Rust `rust/native/sakura_native_ffi/src/worker_thread.rs` が所有 |
| `sakura_core/senp/github/SenpGitHubToolExecutor.cpp` | 389 | `resource.stop_required_acquisition` | `m_worker = std::thread(...)`。405 行に `join()` あり | 同上（`CNativeWorkerThread::Start<CSenpGitHubToolExecutor, &CSenpGitHubToolExecutor::Run>`） |
| `sakura_core/senp/github/SenpGitHubToolExecutor.cpp` | 871 | `resource.stop_required_acquisition` | `m_scheduler.Subscribe(...)`（`SubscribeRead`）。解放は `Unsubscribe`（693 / 720 行）と `Close()`（400 行）に分散 | `GhReadSubscription`（`sakura_core/senp/github/GhReadScheduler.h`）の move-only ハンドルに置換。構築が admission、破棄が release。read レコードがハンドルをメンバとして所有するため、レコード削除がそのまま解放になる。ID を取る `Subscribe` / `Unsubscribe` / `Poll` / `SetVisible` / `RequestRefresh` と `SubscribeRead` は削除 |
| `sakura_core/workbench/editor/SenpControlToolReads.cpp` | 145 | `resource.stop_required_acquisition` | `StartWorker()` の `m_worker = std::thread(...)`（2 つの ctor から呼ぶ単一取得点）。471 行に `join()` あり | `CNativeWorkerThread::Start<CSenpControlToolReads, &CSenpControlToolReads::Worker>(this)`。単一取得点の集約は維持 |

他ルールも純減（`boundary.win32_type` −16、`error.catch_all` −43、`state.public_mutable_field` −112、`global.get_dll_share_data` −5、`state.mutable_member` −1）。**緑の今なら以後の全ステップで「触ったファイルで純減」の判定が使える。S01 はこの状態を維持するための運用（所有者・期限つき waiver 台帳）を Phase 0 で固定する。**

`resource.stop_required_acquisition` の検出式（`tools/build/sakura_build_lib/semantic_inventory.py`）は `std::thread` と `std::jthread` の両方に一致するため、`jthread` 化では finding は消えない。`19eb17c58` が取った方法は「取得をハンドル型に包み、デストラクタが解放する」であり、検出式の語彙（`std::thread(` / `Subscribe(`）自体をコードから消している。S01 はこれを規則として書き、今後の新規 finding には finding ごとに「修正するか、所有者・期限つき waiver にするか」を決める。`--accept-current` は waiver が台帳に載ってから 1 回だけ実行し、追跡 Issue は #289（計画 Issue）ではなく負債 Issue を指定する。

### 2.3 構造的事実（調査レポート 1〜4 の要約）

**神クラス `CEditWnd`**（`sakura_core/window/CEditWnd.cpp` / `CEditWnd.h`）

- 333 メンバ関数 14,299 行、`m_` メンバ 179、`#include` 144。
- メソッド名規則による責務クラスタ 17 個: MSG 2,406 行 / LIFE 1,831 / WB 1,595 / SCM 1,210 / MENU 1,193 / DOC 1,069 / DRAW 962 / WS 602 / VIEW 598 / CHROME 547 / BARS 511 / MD 465 / EXPL 327 / THEME 320 / SENP 278 / TERM 224 / UPD 161。
- 最長関数: `InitializeWorkbench` 1,275 行、`DispatchEvent` 1,026 行（88 `case`、`MYWM_*` 22 種、typed seam `EditorFrameEvents.h` の被覆率約 11%）、`OnSize2` 438 行。
- 38 個の `m_` が 5 クラスタ以上から共有される。`m_workbenchRuntime` は 17 クラスタ中 15 から参照される。
- 表示フラグ（`m_nWinSizeType`、`m_bDarkMode`、`m_bottomWorkbenchMaximized`、`m_bRightPanelVisible`、`m_bDispTabWnd`、`m_bDispSTATUSBAR`）が `sakura_core/workbench/layout/WorkbenchLayoutStateService.h` と二重管理されている。
- 2026-06-01 以降 `CEditWnd.cpp` を触ったコミット 89 件のうち 42% が 4 クラスタ以上にまたがる。
- `GetEditWnd()` 254 参照の要求内容: ペインアクセス 78 / モードレスダイアログ 32 / HWND 21 / ミニマップ 16 / ホイール 16（すべて `view/`） / タイポグラフィ約 15 / レイアウト再計算 13 / ステータスバー 9 / キャプション 8。
- テスト: `src/test/cpp/tests1/test-window.cpp` 2,896 行、`EditWndTest` 131 件はすべて `src/test/cpp/tests1/window/EditorTestSuite.hpp` のフルアプリ fixture を要求する。

**Workbench spine**（`sakura_core/workbench/`）

- `sakura_core/workbench/IWorkbenchRuntime.h` 253 行 / virtual 35（純粋仮想 24、既定実装 10）。本番 consumer は `CEditWnd.cpp` のみ（サービスロケータ）。本番呼び出し 0 のメソッドが 3（`OutputProviderHealth` / `TaskExecution` / `TaskCatalogForFolder`）。
- `sakura_core/workbench/CWorkbenchRuntime.cpp` 2,107 行のうち約 49% がドメインロジック（workspace 設定マージ、layout memento、output provider 初期化、task catalog）。
- workbench を触った 153 コミット中 51 が `CEditWnd.cpp` を同時変更。
- View の追加経路が 2 つある: `sakura_core/workbench/viewcontainer/CViewContainerPages.h` が 5 View を hard-include する閉じた経路と、`sakura_core/workbench/viewcontainer/ViewContainerPageRegistry.h` の factory 経路。実測で Projects View 追加は 81 ファイル（spine 7 ファイル）、SENP Tree Views 追加は 25 ファイル（spine 0）。
- 142 個の workbench `.cpp` のうち 139 が `StdAfx.h` を include。`HWND` を持つヘッダが `win32/` の外に 31 本。`sakura_core/workbench/scm/` は 16,039 行がフラットで、`sakura_core/workbench/scm/CLAUDE.md` は 112 KB。
- 良い先例が同じリポジトリにある: `sakura_core/workbench/ports/`（純粋モデル + HWND なし + 専用スイート）、`sakura_core/workbench/output/`（`sakura_core/workbench/output/IOutputService.h` 52 行の契約 + provider factory）、`sakura_core/workbench/editor/`（`sakura_core/include/sakura/editor/` 公開ヘッダ + PCH 非依存）。

**レガシー中核**（`env/` `doc/` `view/` `cmd/` `types/` `_main/`）

- `sakura_core/StdAfx.h` 176 行目が `env/DLLSHAREDATA.h` を include しているため、PCH 経由で 651 TU から `GetDllShareData()` が無条件に見える。705 呼び出しのうち `.m_Common`（設定）が 71%、書き込みは 42 箇所（設定ダイアログ系に集中）。ディレクトリ別: cmd 174 / view 114 / env 60 / macro 45。
- `sakura_core/cmd/CViewCommander_inline.h`（47 行）が `view/CEditView.h`（889 行）+ `window/CEditWnd.h`（1,509 行）+ `doc/CEditDoc.h`（152 行）を `cmd/*.cpp` 30 本中 22 本に配っている。`m_pCommanderView->` は 902 回 / 86 メンバで、`GetSelectionInfo` だけで 36%。戻り値 `CViewSelect` は `sakura_core/view/CViewSelect.h` 単独で include できる。
- co-change（2026-06-01 以降 952 コミット、jaccard）: `cmd`↔`func` 0.41、`cmd`↔`env` 0.28、`cmd`↔`view` 0.23、`doc`↔`view` 0.23。ファイル対最多は `sakura_core/_main/CNormalProcess.cpp`↔`CEditWnd.cpp` 16 回、`sakura_core/cmd/CViewCommander.cpp`↔`CEditWnd.cpp` 13 回、`sakura_core/env/CShareData_IO.cpp`↔`CEditWnd.cpp` 13 回。
- L3-domain → L4-legacy-ui の上向き include 111 本の内訳: A 誤配置リーフヘッダ 41（`sakura_core/view/colors/EColorIndexType.h` 20、`sakura_core/outline/CFuncInfoArr.h` 14 ほか）/ B Undo 操作 `sakura_core/cmd/COpe.h` 系 7 / C `CEditWnd.h`・`CEditView.h` への実機能要求 22 / D `CEditDoc` が UI（agent、macro、plugin、dlg）を値所有 41。
- L4-legacy-ui → L5-composition 124 本の内訳: `sakura_core/CSelectLang.h` 104、`sakura_core/_main/global.h` 36（重複含む）、`sakura_core/CEditApp.h` 31、`sakura_core/_main/CAppMode.h` 14、`sakura_core/_main/CControlTray.h` 11、`sakura_core/_main/CMutex.h` 7。
- 残存シングルトン `::getInstance()` 467 + `::Instance()` 62: `CEditWnd` 114、`CAppNodeManager` 76、`CEditApp` 59、`CAppMode` 44、`CJackManager` 39、`CCommandLine` 35、`CShareData` 29。

**ビルド／テスト**

- `tools/build/sakura_build.py`（1,884 行）+ `tools/build/sakura_build_lib/`（20,010 行）。`modules.json` は 40 component（legacy 2 / candidate 34 / independent 4）。生成 component が覆う製品コードは 24,884 / 426,334 行（5.8%）。
- `py -3 tools/build/sakura_build.py test component sakura_uri_tests --context msvc-x64-debug` は閉包 2 ノードのみをビルドし、記録では 1〜7 秒（`docs/l4-component-build-r1a-status.md`）。フルは 151〜258 秒、CI の `Native` job は 13〜21 分。しかし `.github/workflows/` に `build component` / `test component` の呼び出しは 0 件。
- `tools/build/sakura_build_lib/ci_plan.py` の判定は `docs_only` / `full_native` の 2 値。`tools/build/sakura_build_lib/coverage_map.py` の `select_tests()` は成果物がリポジトリに無く CI からも呼ばれない。force-full パターンに 952 コミット中 428（45%）が該当。
- `sakura.vcxproj` の明示リスト 662 対ディスク 700（差 37 は生成 component 所有で `src/main/modules/generated/msbuild/consumers/sakura_app.props` の `<ClCompile Remove>` が除外、1 は vendor の tracing.cpp）。`tests1.vcxproj` は 306 対 306 で完全一致。新規 `.cpp` を足すコミットの 92% が vcxproj/filters を同時編集。最頻変更ファイル上位 4 が `sakura.vcxproj` 112 / `.filters` 103 / `tests1.vcxproj` 96 / `.filters` 88。
- CMake 側（`src/main/cmake/sakura.cmake`、`src/test/cmake/tests1.cmake`）は既に `GLOB_RECURSE` + OPTIONAL `src/main/modules/generated/cmake/legacy/source-ownership.cmake`。`tools/build/sakura_build_lib/generator.py` の `_msbuild_source_item_spec()` はワイルドカード spec をスキップする（glob 化の唯一の技術的障害）。
- `src/test/test-inventory.json` の runner は `12eee6960`（2026-08-14）で 7 runner → `tests1` 単一に退行し、検出するゲートがない。
- テスト: 4,157 件 / 452 スイート、`src/test/cpp/tests1/` 直下にフラットな `test-*.cpp` 57 本 + サブディレクトリ 16。`src/test/cpp/tests1/workbench/` は 104 ファイル（サブディレクトリ 0。対する `sakura_core/workbench/` は 30 サブディレクトリ）。
- co-change（調査 7、2026-06-01 以降で製品ソースを触った 386 コミット）: ビルド定義 4 点セットを同時に触るもの 125 件（32.4%）、`CLAUDE.md` 155 件（40.2%）、`src/test/` 251 件（65.0%）。confidence 0.67 以上のクロスディレクトリ co-change 組 22 件のうち 21 件が「実装 ↔ 鏡写しの単体テスト」（例: `sakura_core/window/CCustomFrameController.cpp` ↔ `src/test/cpp/tests1/window/CustomFrameTest.cpp` 0.88）。残る 1 件 `rust/senp/sakura_senp_host/src/effect_protocol.rs` ↔ `sakura_core/senp/SenpEffectProtocol.h`（0.71）だけが本物の統合候補。`src/test/test-inventory.json` は同期間に 79 回変更されるテスト追加のボトルネック。
- 2026-06-01 以降の 89 Issue（2 コミット以上）の読解行数総和 2,736,139 行のうち `sakura_core/window/CEditWnd.cpp` 単体で 638,628 行（23.3%、38 Issue に出現）。上位 10 ファイル（`CEditWnd.cpp`、`src/test/cpp/tests1/workbench/CWorkbenchRuntimeTest.cpp` 27 回、`sakura_core/workbench/scm/CScmWorkbenchTool.cpp` 17 回、`sakura_core/workbench/CWorkbenchRuntime.cpp` 27 回、`CEditWnd.h` 34 回、`sakura_core/workbench/explorer/CExplorerTool.cpp` 19 回、`sakura_core/terminal/window/CTerminalTool.cpp` 17 回ほか）で 39.7%。`CEditWnd.cpp` の hunk 解析では 89 コミット・202 関数が 9 責務に散在し、26 コミットは 1 責務しか触っていない。L4 leaf component に属する製品ファイルは 1,454 中 42（2.9%）で、C++ 製品コードを触った 63 Issue のうち component 内で閉じたものは 0。

**エージェント文脈と形式検証**

- `CLAUDE.md` 59 本 658,157 バイト。上位: `sakura_core/workbench/scm/CLAUDE.md` 112,432 / `sakura_core/window/CLAUDE.md` 51,756 / `sakura_core/terminal/CLAUDE.md` 36,520 / `sakura_core/workbench/CLAUDE.md` 35,851 / `sakura_core/senp/CLAUDE.md` 33,842 / `sakura_core/workbench/editor/CLAUDE.md` 26,838 / `.github/CLAUDE.md` 26,372。
- `CLAUDE.md` の行分類（調査 5、行単位の機械分類）: 設計解説 51.5% / 日付付き履歴 31.1% / 不変条件 9.8% / 禁止事項 4.8% / VS Code 互換根拠 1.8% / 手順 0.9%。契約に相当する行（不変条件 + 禁止事項 + 互換根拠 + 手順）は 17.3% にとどまる。日付付き・Checkpoint 節は 25 ファイル・85 節・190,832 バイト（29.2%）。`sakura_core/window/CLAUDE.md` は 83%、`sakura_core/workbench/CLAUDE.md` は 66% が履歴。祖先チェーン合計は `sakura_core/workbench/scm/` で 170,108 バイト、`sakura_core/workbench/editor/` 84,514、`sakura_core/window/` 73,581。
- 重複はテキスト一致ではなく意味的（2 文以上一致する正規化文 2 件、8-gram 共有 120 以上のペア 0）。一方 `fails closed` の説明は 16 ファイル、`Checkpoint (2026-07-31)` は 9 ファイルに分散する。「所有しないもの」を明示する節を持つ `CLAUDE.md` は 3 本のみ。
- 誤認源はファイル名衝突ではなくディレクトリ名衝突と生きた二重実装: `window` ディレクトリが 4 箇所（`sakura_core/window/`、`sakura_core/terminal/window/` ほか）、`agent` が 3 箇所、`recent` が 2 箇所。`sakura_core/outline/CDlgFuncList.h` と `sakura_core/workbench/outline/COutlineWorkbenchTool.h` が両方 `CEditWnd.cpp` から参照され、`CMRUFile` と `RecentlyOpenedWorkspaceService` が同じ `CLAUDE.md` に併記される。`docs/` 配下に設計解説や履歴の退避先ディレクトリ（`docs/subsystems/`（新規））が存在しない。
- `docs/formal/` に `.tla` 6 本（`ControlStartupHandshake`、`ProjectSwitchState`、`SearchRequestLifecycle`、`SenpContributionOwner`、`SenpGhConnection`、`SenpGhRequests`）と `.cfg` 22 本。CI で TLC を回すのは 15 本（Search 3 + SENP 12）で、`ControlStartupHandshake`（3 cfg）と `ProjectSwitchState`（1 cfg、mutant 0）は一度手で回して README に貼っただけで再実行されない。`docs/formal/ExtensionHostLease_Current.cfg` ほか 3 本は対応する `.tla` が `12eee6960`（#169、2026-08-14）で削除されて以降、約 1 か月孤児のまま残っており、どのゲートも検出しなかった。
- モデルとコードをつなぐ機械的参照は 0: `rg -ni "docs/formal|TLA\+|形式検証" sakura_core/` は 0 件、`docs/formal` を参照する `CLAUDE.md` も 0 本。対応表は `docs/formal/README.md` の行番号付き散文で、基準コミット `36f0c2550` 以降 `sakura_core/_main/` に 54 コミット入っているため既に腐っている。runner は `tools/verify-search-lifecycle.py` と `tools/verify-senp-github-models.py` の 2 本に分岐し、後者が前者の厳密な上位互換（temporal 反例、探索完全性の数値検査、`sources_unchanged`、`--model` 分割）。`std::expected` の使用は 0 件。

---

## 3. 設計原則と判定規則

### 3.1 原則（すべて機械判定できる形で書く）

| # | 原則 | 判定規則（コマンド） | 違反時の扱い |
|---|---|---|---|
| P1 | **契約は `include/` に、実装は所有ディレクトリに** | `rg -l "class I[A-Z]" sakura_core --glob '!include/**' --glob '!**/win32/**'` の結果が増えない | 新規インタフェースは `sakura_core/include/sakura/<subsystem>/` に置く |
| P2 | **上向き include は単調減少** | `py -3 tools/architecture/include_layers.py report --json` の `upward_edges_total` がベースライン以下 | S02 で CI ゲート化 |
| P3 | **HWND は `win32/` と `window/` の外に書かない** | `rg -l "\bHWND\b" sakura_core/workbench --glob '!**/win32/**'` が 31 から減る | 新規ファイルでの違反は reject |
| P4 | **PCH に依存しない純粋層** | `rg -L "StdAfx.h" <dir>/*.cpp` で対象ディレクトリの `.cpp` が `StdAfx.h` を include しない | component 化の前提 |
| P5 | **同じ目的の経路は 1 つ** | View 追加・コマンド追加・設定読み取りの経路がそれぞれ 1 つ（S31・S19・S16b） | 旧経路は削除、残す場合は `[[deprecated]]` |
| P6 | **ビルド定義は生成物か glob** | 新規 `.cpp` 追加コミットの `git show --stat` に `.vcxproj` が現れない（S05〜S07 後） | 明示リストへの追加は reject |
| P7 | **テストはコードを鏡写しにする** | `sakura_core/<dir>/` に対して `src/test/cpp/tests1/<dir>/` が存在する（S12・S36） | フラットな `test-*.cpp` を新規作成しない |
| P8 | **`CLAUDE.md` は不変条件だけ、履歴は `docs/`** | 各 `CLAUDE.md` ≤ 16 KB、祖先チェーン合計 ≤ 60 KB（S35） | 日付付き履歴節は `docs/subsystems/`（新規）へ |
| P9 | **1 ステップ = 1 Issue = 独立にビルド・テスト・revert 可能** | 各ステップの `Verify` が単独で通る | 2 ステップを 1 コミットに混ぜない |
| P10 | **semantic ratchet は緑のまま** | `py -3 tools/build/sakura_build.py inventory semantic --strict` exit 0 | 移動で一時的に増える場合は `--accept-current --reason --tracking-issue` で台帳に記録 |

### 3.2 各ステップ共通の受入ゲート

すべてのステップは以下 G1〜G7 を通してから完了とする（既存の CI と同じ内容。ローカルで先に回す）。

| # | Verify | Expect |
|---|---|---|
| G1 | `py -3 tools/build/sakura_build.py --format json lint checkout-invariance` | exit 0（`tools/CLAUDE.md` の必須 preflight） |
| G2 | `py -3 tools/build/sakura_build.py graph check --all-contexts` | exit 0 |
| G3 | `py -3 tools/build/sakura_build.py generate --check` | `stale: []` |
| G4 | `py -3 tools/build/sakura_build.py inventory semantic --strict` | exit 0（増加ルール 0） |
| G5 | `build-sln.bat x64 Debug` | exit 0、`sakura.exe` と `tests1.exe` が生成 |
| G6 | `tests1.exe` を `src/test/headless-suite-selection.env` のフィルタで実行 | 失敗 0 |
| G7 | `powershell -File src/main/ps1/check-encoding.ps1` 相当の CI ステップ | exit 0（新規 `.cpp`/`.h` は ASCII-only か UTF-8+BOM） |

ファイル移動を含むステップは加えて `build-gnu.bat MinGW Debug`（CMake 経路）を通す。レイアウト・再描画に触るステップ（S22・S24・S25・S30）は `.claude/skills/stale-pixel-verification/SKILL.md` の二重撮影を行う。

### 3.3 12 指標とステップの対応（check.py C2 が照合する）

| 指標 | 主に効かせるステップ | 計測 |
|---|---|---|
| High Cohesion | S25, S26, S27, S28, S32, S33 | 代表 Issue の IRV `source_lines`（Phase 3 の K5）、`CEditWnd.cpp` 行数（B-01、補助） |
| Low Coupling | S16a, S16b, S16c, S17, S29, S34 | `GetDllShareData` / `GetEditWnd`（B-11・B-12）、`CEditWnd.h` 被 include 数（B-16） |
| Single Responsibility | S22, S23, S26, S33 | 表示フラグ宣言数 0、`CWorkbenchRuntime.cpp` 1,100 行以下 |
| One-way Dependency | S02, S13, S14, S15, S18, S20, S21 | 上向き include（B-15） |
| Change Locality | S05, S06, S07, S19, S31 | IRV の `manifest_files`、Change Footprint の「変更ファイル」「ビルド定義ファイル」（6.2 節） |
| Build/Test Isolation | S08, S09, S10, S11, S12 | `test component` の所要秒数、`ci plan` の `component_native` 率 |
| Explicit Contracts | S16b, S17, S18, S29, S34, S40a, S40b | `sakura_core/include/sakura/` 配下の契約ヘッダ数、既定実装つき virtual 0 |
| Small Agent Context | S04, S35, S36, S37 | IRV の `guide_files` / `guide_lines`（一次、6.1 節）、`CLAUDE.md` 合計（B-08、二次） |
| Independently Testable Modules | S10, S12, S27, S32, S33 | `modules.json` の `*_tests` component 数（B-09） |
| Independently Refactorable Modules | S23, S31, S32, S34 | View 追加時の spine ファイル変更 0 |
| Formal Verification Boundary | S38, S39, S40a, S40b, S40c, S40d | `docs/formal/models.json`（新規）の対応件数、`verify-*` ゲート数 |
| 誤認しにくいディレクトリ構造 | S12, S31, S36, S37 | 所有マップの未割当パス 0、同目的の経路 1 |

---

## 4. 目標構造

### 4.1 層モデル（`modules.json` から導出。`tools/architecture/layers.json` は導出規則と例外だけを持つ）

層の正本は `src/main/modules/modules.json` の component 所有（`sources` / `public_headers` / `family` / `contracts[]` / `edges[]`）である。`layers.json`（schema_version 2）は次の 3 つだけを持ち、ディレクトリを層に手で割り当てる表は持たない。

1. `derivation.family_rank`: family → rank の写像（下表）。component が所有するファイルはこの rank の層に入る。`public_headers` と `contracts[]` 配下は family に関係なく `L0-contracts`。`*-tests` family は `L6-tests`（何にでも依存してよいので上向きエッジを生まない）。
2. `monolith_overrides`: `sakura_app`（レガシーモノリス）配下のパスにだけ許される例外。48 件。各件が `reason` と `removal_condition`（「component X が path Y を所有したら削除」）を持つ。component が所有するパスに override を書くと `check-derivation` が exit 1 になる。
3. `layers[]`: 層 ID と rank の一覧（`L3-workbench-model` は `derived_target: true` で rank 3 の導出先、`L3-domain` は override 専用）。

| family | rank / 層 | 根拠 |
|---|---:|---|
| `shared-state` | 0 / `L0-contracts` | `sakura_shareddata_contract` は翻訳単位を持たず SharedData の ABI ヘッダだけを出す |
| `platform-primitives` | 2 / `L2-platform` | uri / serialization / security / storage / filesystem / request は OS・ファイル形式の primitive |
| `authority-process-platform` | 2 / `L2-platform` | control-IPC の protocol / security / transport / endpoint はプロセス間配管 |
| `terminal-harness` | 2 / `L2-platform` | tmux core / harness bridge / CLI / tools は document・workbench モデルに依存しない自動化基盤 |
| `document-editor` | 3 / `L3-workbench-model` | SelectionSession / EditorFrameEvents / DocumentSession は抽出済みのエディタ・ドメインモデル |
| `win32-adapters` | 4 / `L4-legacy-ui` | `Win32EditorFrameAdapter` は frame 契約を HWND に束ねる |
| `process-lifecycle` | 5 / `L5-composition` | `EditorAppLifecycle` は `_main` からプロセス起動・終了を順序付ける |
| `legacy-language-resources` | 4 / `L4-legacy-ui` | `sakura_lang` はローカライズ済みリソース（C++ スキャン対象外） |
| `legacy`（`sakura_app`） | 5（向きの判定にだけ使う） | ファイル分類は override だけで決まる |

導出結果の層（実測。`report --json` の `layers[].files_with_includes`）:

| 層 ID | rank | 導出元 | files_with_includes |
|---|---:|---|---:|
| `L0-contracts` | 0 | `sakura_core/include/` + 各 component の `public_headers` | 9 |
| `L1-foundation` | 1 | override 11 件（`StdAfx.h`、`src/main/cpp/cxx/`、`basis/`、`util/`、`mem/`、`charset/`、`apiwrap/`、`convert/`、`parse/`、`debug/`、`_os/`） | 175 |
| `L2-platform` | 2 | family 3 種 + override 5 件（`platform/`、`io/`、`extmodule/`、`terminal/tmux/`、`terminal/cli/`） | 87 |
| `L3-domain` | 3 | override 11 件（`doc/`、`docplus/`、`types/`、`env/`、`config/`、`theme/`、`textmate/`、`markdown/`、`grep/`、`func/`、`recent/`） | 219 |
| `L3-workbench-model` | 3 | family `document-editor` + override 4 件（`workbench/`、`terminal/`、`senp/` ほか） | 340 |
| `L4-legacy-ui` | 4 | family `win32-adapters` + override 15 件（`view/`、`cmd/`、`window/`、`dlg/`、`prop/`、`typeprop/`、`outline/`、`macro/`、`plugin/`、`print/`、`uiparts/`、`agent/`、`update/`、`accessibility/`、`workbench/win32/`、`terminal/window/`） | 343 |
| `L5-composition` | 5 | family `process-lifecycle` + override 2 件（`_main/`、`sakura_core/` 直下） | 34 |
| `L6-tests` | 6 | `*-tests` family | 0 |

分類の出所（`classification_source_counts`）: `derived_component` 48 / `derived_contract` 50 / `override_monolith` 1,353 / `unclassified` 0。**override が 1,353 ファイルを占めるのは `sakura_app` が 1,353 ファイルを抱えているからで、この数が減ることが component 化（4.4 節）の進捗そのものになる。**

上向き include（低 rank のソースが高 rank のヘッダを include）の現状。**合計と各ペアは check.py C8 が `py -3 tools/architecture/include_layers.py report --json` と照合し、C10 が `check-derivation --strict` の exit 0 を確認する。**

| from | to | エッジ数 | ファイル数 | 解消ステップ |
|---|---|---:|---:|---|
| L4-legacy-ui | L5-composition | 124 | 92 | S13, S14, S21 |
| L3-domain | L4-legacy-ui | 111 | 58 | S15, S17, S18 |
| L3-domain | L5-composition | 62 | 43 | S13, S14, S16a |
| L1-foundation | L3-domain | 37 | 26 | S20 |
| L1-foundation | L5-composition | 18 | 14 | S13, S14 |
| L3-workbench-model | L4-legacy-ui | 12 | 10 | S21 |
| L3-workbench-model | L5-composition | 10 | 10 | S13, S21 |
| L2-platform | L3-workbench-model | 9 | 5 | S20, S21 |
| L1-foundation | L4-legacy-ui | 5 | 4 | S20 |
| L2-platform | L3-domain | 5 | 3 | S20 |
| L2-platform | L5-composition | 3 | 3 | S13 |
| L1-foundation | L2-platform | 2 | 2 | S20 |
| L2-platform | L4-legacy-ui | 2 | 2 | S20 |
| L0-contracts | L3-workbench-model | 1 | 1 | S21 |
| L1-foundation | L3-workbench-model | 1 | 1 | S20 |

| 集計 | 値 |
|---|---:|
| 合計（上向き include） | 402 |
| 上向き include を持つファイル | 237 |
| 対象ソースファイル | 1,451 |
| include エッジ総数 | 5,032 |
| 未解決 include | 167 |

手書き分類（旧 `layers.json` schema 1、合計 400）から導出に切り替えて変わったのは terminal-harness 周辺の 25 ファイルだけである。`Tmux*.h` / `SakuraCliTypes.h` / `SakuraHarnessCli.h` / `SakuraTmuxCli.h` の 9 本は component の `public_headers` なので `L0-contracts` へ、terminal-harness family 所有の 12 本と `terminal/tmux/` override の 4 本（`TmuxRuntimeAdapter.*`、`TmuxWaitChannelService.*`）は `L2-platform` へ移った。その結果 `HarnessBridgeOperationDispatcher.h → TmuxCli.h` は契約間エッジになって上向きから外れ（−1）、`TmuxCommandTypes.h → terminal/runtime/TerminalRuntimeTypes.h`（L0→L3）と `TmuxRuntimeAdapter.h → ITerminalRuntimeService.h` / `TerminalCollectionModel.h`（L2→L3）の 3 本が新たに見える上向きエッジになった（+3）。この 3 本はハーネスがモノリスの terminal runtime に依存している実在の逆依存であり、S20 で解消する。

`check-derivation` は現在 errors 0 / warnings 5（`doc/`、`docplus/`、`terminal/tmux/`、`terminal/cli/` の override が、`removal_condition` に書いた component が既に存在するため「stale の可能性」と警告される。部分抽出中の領域なので意図どおり）。

到達目標: 合計 60 以下（残るのは `StdAfx.h` 経由の推移依存を除いた真の合成依存のみ）、`override_monolith` を 4.4 節の component 化に合わせて単調減少。S02 で CI ゲート化し、以後は「ステップ前の値以下」を必須にする。

### 4.2 目標ディレクトリ構造

既存ディレクトリは動かさず、**新設は 6 箇所だけ**にする。エージェントが変更対象を誤認しないための命名規則は S37 で `sakura_core/CLAUDE.md` に 1 表として固定する。

```
sakura_core/
  include/sakura/                    L0 契約ヘッダ（実装を持たない）
    editor/                          既存: EditorFrameEvents.h, document/, lifecycle/, win32/
      view/ dialog/ chrome/ layout/ input/ command/   （新規）S29・S17 の契約
    shareddata/                      既存: SharedDataCapabilities.h（S16b で CommonSettingsReader を追加）
    workbench/                       （新規）S34 の役割別インタフェース
  basis/ util/ mem/ ...              L1（S13 で CSelectLang.h、S14 で EditorPrimitives.h が加わる）
  doc/
    undo/                            （新規）S15: cmd/COpe*.h の移動先
    outline/                         （新規）S15: outline/CFuncInfo*.h の移動先
  workbench/
    kernel/                          （新規）S33: ListenerGate / OwnerRegistry / Revision
    composition/                     （新規）S23: WorkbenchComposition
    <view>/model/ service/ win32/    S32: scm/ で先行適用する標準レイアウト
    win32/                           既存: HWND を持つ projection はここだけ
  window/
    editwnd/                         （新規）S26: CEditWnd から剥がした Win32 adapter 断片
src/test/cpp/tests1/<sakura_core と同じ階層>   S12: フラットな test-*.cpp を解消
tools/architecture/                  layers.json / include_layers.py / issue_footprint.py / irv.py / irv_calibrate.py / issue_commits.py / component_ownership.py（S02 でコミット）
docs/subsystems/<dir>/history.md     （新規）S35: CLAUDE.md から追い出した日付付き履歴
docs/formal/models.json              （新規）S38: TLA+ モデル ↔ コードの機械可読対応
```

### 4.3 `CEditWnd` の到達形

`CEditWnd.cpp` の行数（16,806 → 5,500 以下）は補助指標である。巨大な `.cpp` を複数ファイルに移しただけで、1 Issue のためにそれら全部を読む必要が残るなら失敗と数える。各クラスタ抽出は Phase 3 冒頭の共通受入 K1〜K5（責務所有者 1 つ / 明示契約 / 旧 `CEditWnd` への逆依存なし / 独立テストコマンド / 代表変更要求の IRV 前後）を満たしてはじめて完了になる。

| 残すもの | 行数目標【推定】 |
|---|---:|
| Win32 ウィンドウプロシージャ（MSG の一部）、`DispatchEvent` の typed 化済み転送 | 2,000 |
| VIEW / BARS / CHROME の Win32 adapter | 1,600 |
| フォーカス分岐（`m_emptyEditorSurface` の所有）と DOC 連携の薄い転送 | 900 |
| `WorkbenchComposition` の生成・破棄 | 200 |
| 合計 | 5,500 以下 |

外へ出すもの: WB 1,595 → `sakura_core/workbench/win32/PaneCompositeProjectionService.h`・`BuiltinPartProjection.h`（S24）、WS 602 → `sakura_core/workbench/workspace/`（S25）、MENU 1,193 → `window/editwnd/` + `workbench/commands/`（S26）、SCM 1,210 → `sakura_core/workbench/scm/`（S27）、MD/EXPL/TERM/UPD/SENP 1,455 → 各所有ディレクトリ（S28）、`InitializeWorkbench` 1,275 → phase 化（S25）。

`GetEditWnd()` 254 参照は 7 契約に置換する（S29）: `IEditorFrameHandle`（HWND 21）、`IEditorWheelPolicy`（16、`view/` へ状態ごと移す）、`IEditorTypography`（約 15）、`IEditorChromeNotifier`（約 33）、`IEditorLayoutRefresh`（20）、`IEditorDialogRegistry`（32）、`IEditorPaneAccess`（78）。

### 4.4 component の到達形（`src/main/modules/modules.json`）

| 新 component | owner ディレクトリ | ステップ |
|---|---|---|
| `sakura_workspace_transition` + `_tests` | `sakura_core/workbench/workspace/` | S25 |
| `sakura_menu_model` + `sakura_win32_menu_builder` | `sakura_core/workbench/commands/`、`sakura_core/window/editwnd/`（新規） | S26 |
| `sakura_scm_commands` | `sakura_core/workbench/scm/` | S27 |
| `sakura_editor_views`（契約のみ） | `sakura_core/include/sakura/editor/` | S29 |
| `sakura_editor_command_contract` | `sakura_core/include/sakura/editor/command/`（新規） | S17 |
| `sakura_scm_model` + `_tests` | `sakura_core/workbench/scm/model/`（新規） | S32 |
| `sakura_workbench_kernel` + `_tests` | `sakura_core/workbench/kernel/`（新規） | S33 |
| `sakura_workbench_contracts` | `sakura_core/include/sakura/workbench/`（新規） | S34 |

40 → 52 以上。既存 4 つの `sakura_editor_*` と `sakura_win32_editor_frame` の 2 層パターン（純粋 + Win32）をそのまま踏襲する。

---

## 5. 実行計画

順序の原則: **Phase 0（ゲートと計測）→ Phase 1（ビルド／テスト隔離）→ Phase 2（依存方向）→ Phase 3（`CEditWnd`）→ Phase 4（workbench spine）→ Phase 5（エージェント文脈）→ Phase 6（形式検証）**。Phase 1 を先にするのは、以降のすべてのファイル移動が `.vcxproj` 編集を伴わなくなり、各ステップの差分が読めるようになるためである。Phase 内のステップは番号順に依存する。

各ステップの所要は「エージェント 1 セッション」を単位とする【推定】: 小 = 1 セッション（30 分〜2 時間）、中 = 2〜4 セッション、大 = 5 セッション以上。

### Phase 0: ゲートを緑にし、計測基盤を固定する

### S01 semantic ratchet を緑に保つ運用（所有者・期限つき waiver 台帳）を固定する

- 対象: `tools/build/baselines/editor-core-semantic.json`、`tools/build/baselines/editor-core-semantic-history/`、`tools/build/sakura_build_lib/semantic_inventory.py`、`tools/build/baselines/semantic-waivers.json`（新規）、`tools/build/tests/test_semantic_waivers.py`（新規）
- 内容: 2.2 節の 4 finding は `19eb17c58`（#305）で解消済みで、本ステップで直すコードは無い。代わりに、次に新規 finding が出たときに **一括で台帳受入しない**ための仕組みを入れる（#289 は計画 Issue であり、ライフサイクル負債の所有者ではない）。(1) `semantic-waivers.json` を新設する。1 レコード = `rule_id` / `path` / `symbol` / `reason` / `owner` / `expires` / `issue`。(2) `semantic_inventory.py` の `--strict` が、waiver に一致する new finding を除外し、`expires` を過ぎた waiver は new finding として扱う（期限切れで再び赤になる）。(3) 処置の判断表を運用規則として置き、`19eb17c58` の 4 件を判断の実例として記録する:

  | # | finding（`e035f0edb` 時点） | 判断 | 処置（`19eb17c58`） | 証跡 |
  |---|---|---|---|---|
  | F1 | `ControlSenpComposition.cpp:451` `m_worker = std::thread(...)` | `join()` は 494 行にあったが、取得と解放が別の行にあり、後の編集で解放行を落とせる | **修正**: `CNativeWorkerThread` ハンドル（デストラクタが join）。join を名指しする行が消えた | `19eb17c58` |
  | F2 | `SenpGitHubToolExecutor.cpp:389` `m_worker = std::thread(...)` | 同上（`join()` 405 行） | **修正**: 同上 | `19eb17c58` |
  | F3 | `SenpGitHubToolExecutor.cpp:871` `m_scheduler.Subscribe(...)`（`SubscribeRead`） | 解放が `Unsubscribe`（693 / 720 行）と `Close()`（400 行）に分散し、停止時に必ず到達する保証が無かった | **修正**: `GhReadSubscription` move-only ハンドル。read レコードがハンドルを所有し、レコード削除 = 解放。ID を取る API は削除 | `19eb17c58` |
  | F4 | `SenpControlToolReads.cpp:145` `StartWorker()` の単一取得点 | 取得点の集約は正しかったが、`join()`（471 行）は別の行 | **修正**: `CNativeWorkerThread`。集約は維持 | `19eb17c58` |

  判断規則: 解放を「同じ型のデストラクタ」に寄せられるなら修正で消す。寄せられない理由（例: OS ハンドルの所有を Rust 側に移すと `windows-sys` が最初の外部依存になる。`19eb17c58` が `CGhReadScheduler` を Rust に移さなかった理由）があるときだけ waiver にし、`expires` は 90 日以内、`issue` は修正 Issue の番号にする。
- 新しい境界: 「finding は所有者と期限を持つ」。理由なしの一括受入が禁止される。
- テスト: `tools/build/tests/test_semantic_waivers.py`（新規）: 期限切れ waiver で exit 11、有効な waiver で exit 0、waiver 0 件で従来どおり。
- Verify: `py -3 -c "import json;w=json.load(open('tools/build/baselines/semantic-waivers.json'));print(len(w['waivers']), all(k in r for r in w['waivers'] for k in ('rule_id','path','symbol','reason','owner','expires','issue')))"` と `py -3 tools/build/sakura_build.py inventory semantic --strict; echo $?`
- Expect: `0 True`（基準コミットでは waiver 0 件。将来足すときも 3 件を超えない）。`--strict` が exit 0 で `comparison.new_findings` が空。`py -3 -m pytest tools/build/tests/test_semantic_waivers.py` が 3 件 pass。台帳履歴に `tracking_issue` が #289 のレコードが無い。
- リスク: 低。`semantic_inventory.py` の変更は `--strict` の除外経路だけで、finding の計数（B-10〜B-14）は変えない。
- ロールバック: waiver 台帳と `semantic_inventory.py` の変更を 1 コミット revert。
- 規模: 小
### S02 `tools/architecture/` をコミットし、上向き include を CI ゲートにする

- 対象: `tools/architecture/layers.json`、`tools/architecture/include_layers.py`、`tools/architecture/issue_footprint.py`、`.github/workflows/architecture-gates.yml`、`tools/build/baselines/include-layers.json`（新規）、`tools/build/tests/test_architecture_gates_workflow_contracts.py`
- 内容: 3 ツールを追跡対象に入れる。`include_layers.py` に `check --baseline <json>` サブコマンドを足し、`upward_edges_total` と各ペアがベースライン以下なら exit 0、超えたら exit 1 にする。`architecture-gates.yml` に `Verify include layers` ステップを `Verify dependency ledger` の直後に追加する（ubuntu、Python 標準ライブラリのみ、1 秒未満）。ベースラインは 4.1 節の値（合計 402）。`check-derivation --strict` も同じワークフローステップで実行し、`modules.json` と `layers.json` の矛盾（component 所有パスへの override、family rank と矛盾する override、理由・削除条件なし）を exit 1 にする。
- 新しい境界: 「上向き include は減らすことしかできない」という ratchet。
- テスト: `tools/build/tests/test_architecture_gates_workflow_contracts.py` に新ステップ名の存在を追加。`tools/architecture/tests/test_include_layers.py`（新規）でベースライン超過時に exit 1 になることを確認。
- Verify: `py -3 tools/architecture/include_layers.py report --json`
- Expect: `upward_edges_total` が 402 以下。`py -3 tools/architecture/include_layers.py check-derivation --strict` が exit 0。`rg -c "Verify include layers" .github/workflows/architecture-gates.yml` が 1。
- リスク: 低。読み取り専用ゲート。`sakura_app` 配下は override で分類しているため粒度は粗いが、増加のみを検出するので既存違反で赤にはならない。
- ロールバック: ワークフローのステップ削除。
- 規模: 小

### S03 IRV と Change Footprint を Issue クローズ時に自動計測する

- 対象: `tools/architecture/issue_footprint.py`、`tools/architecture/irv.py`、`tools/architecture/irv_calibrate.py`、`tools/architecture/issue_commits.py`、`.github/workflows/develop-issue-closure.yml`、`.github/workflows/pr-gate.yml`、`docs/evidence/`
- 内容: (1) 6.3 節の誤差のうち残っている D2 / D3 / D5 / D6 / D7 を `issue_footprint.py` で修正する: `--no-merges` 既定化、`--attribution exclusive|shared`（既定 exclusive。多重参照コミットは `shared_files` / `shared_with` に分離）、`--find-renames`、ファイル種別の分解（`code_files` / `test_files` / `evidence_files` / `generated_files`）、`read_footprint_lines` → `changed_file_lines` 改名 + `max_file_lines` / `max_file_path` 追加、`coverage --since <日付>` サブコマンド（未帰属率・多重参照率）。旧定義は `--attribution legacy` として残し、C7 の照合と時系列の連続性を保つ。D1（上流混入）は `issue_commits.py` の fork 起点限定で解消済みなので触らない。(2) `pr-gate.yml` にコミットメッセージの `#N` 参照 lint を足し、未帰属率を新規コミットで 0 にする。(3) `develop-issue-closure.yml` の `Resolve closing references` の後に、クローズ対象 Issue ごとに `irv.py estimate --issue N --json` と `issue_footprint.py measure --issue N --json` を実行し、ワークフロー artifact として保存する（リポジトリを汚さない）。(4) 較正は CI では走らせない（セッション記録は開発機にしかない）。月次で `irv_calibrate.py compare --table` を開発機で実行し、6.2 節の表を更新する手順を `tools/architecture/README.md`（新規）に書く。
- 新しい境界: 「1 Issue で読む必要があった量（IRV）」と「変更が広がった範囲（Change Footprint）」が別々の列で時系列に残る。
- テスト: `src/test/py/`（既存 pytest 配置）に `test_issue_footprint.py`（新規）と `test_irv.py`（新規）を追加し、固定 SHA の Issue（#266、`pr` 対応で 1 コミット / 4 ファイル）で `changed_files == 4`、`irv.py estimate --issue 266` の区分ごとの件数（`guide` 8、`owning_source` 27、`test` 9、`contract` 26、`manifest` 5）を確認する。`issue_commits.py` は #291 で `association == "pr"` かつ上流コミット `890362767` を含まないことを確認する。
- Verify: `py -3 tools/architecture/issue_footprint.py table --issues 289,266` と `py -3 tools/architecture/irv.py table --issues 289,266`
- Expect: exit 0。それぞれ 2 行の Markdown 表が出る。`measure --issue 226 --json` が `changed_files: 1`、`shared_files: 76`、`shared_with: [227]` を返す。`coverage --since 2026-07-29` が未帰属率と多重参照率を報告し、lint 導入後のコミットの未帰属が 0 件。`--attribution legacy` では 6.3 節の値を再現する。
- リスク: 低。
- ロールバック: ワークフローのステップ削除。

### S04 `CLAUDE.md` サイズ予算の ratchet

- 対象: `tools/architecture/claude_md_budget.py`（新規）、`tools/build/baselines/claude-md-budget.json`（新規）、`.github/workflows/architecture-gates.yml`
- 内容: `git ls-files -- CLAUDE.md "*/CLAUDE.md"` の各ファイルのバイト数と、各ディレクトリの祖先チェーン合計（ルート → `sakura_core/` → `sakura_core/workbench/` → `.../scm/` のように読み込まれる合計）をベースライン JSON に固定する。増加は exit 1。S35 完了後に絶対上限（単一 16 KB、祖先チェーン 60 KB）へ切り替える。
- 新しい境界: `CLAUDE.md` に「書き足す」ことがコストになる。
- テスト: `tools/architecture/tests/test_claude_md_budget.py`（新規）。
- Verify: `py -3 tools/architecture/claude_md_budget.py check --baseline tools/build/baselines/claude-md-budget.json`
- Expect: exit 0。ベースラインに 59 ファイル、`sakura_core/workbench/scm/CLAUDE.md` の祖先チェーン合計が 174,000 バイト前後（ルート 13,163 + `sakura_core/CLAUDE.md` 8,662 + `sakura_core/workbench/CLAUDE.md` 35,851 + 112,432）として記録される。
- リスク: 低。
- ロールバック: ステップ削除。
- 規模: 小

### Phase 1: ビルド／テスト隔離

### S05 `tests1.vcxproj` を再帰ワイルドカード化する

- 対象: `sakura_core/tests1.vcxproj`、`sakura_core/tests1.vcxproj.filters`、`sakura_core/CLAUDE.md`、`src/test/CLAUDE.md`
- 内容: 306 件の `<ClCompile Include="..\src\test\...">` を `..\src\test\cpp\**\*.cpp` と `..\src\test\resources\**\*.cpp` の 2 項目に置換する。メタデータ付き 2 件（`pch.cpp` の `PrecompiledHeader Create`、`coverage.cpp` の `stdcpp17`）は `<ClCompile Update="...">` で付与する。`.filters` の明示エントリは削除する（VS 上はフラット表示になる。S07 の後で自動生成に戻す）。`sakura_core/CLAUDE.md` の「`.vcxproj` と `.filters` の両方を更新」規約を「`tests1` はワイルドカード、`sakura` は S07 まで明示」に書き換える。
- 新しい境界: テストファイル追加で触るファイルが 1 になる。
- テスト: 既存 4,157 件のフル実行が受入条件（リンク順変化の検出）。
- Verify: `rg -c "<ClCompile Include=" sakura_core/tests1.vcxproj`
- Expect: 3 以下（現在 306）。`tests1.exe --gtest_list_tests` の件数が 4,157 で一致。無変更で `build-sln.bat x64 Debug` を 2 回実行して 2 回目が no-op。`src/test/cpp/tests1/` に空の `.cpp` を 1 本追加して `.vcxproj` を編集せずにビルドが通る。
- リスク: 高（リンク順）。`src/test/CLAUDE.md` に記録されたリンク順依存の COM apartment 事故が再発する可能性がある。フィルタなしでフル実行し `CSakuraEnvironmentTest.ResolvePath001` が pass することを確認する。FileTracker の incremental 判定への影響は未検証（7.3 節 U8）。
- ロールバック: 1 コミット revert（生成物に影響しない）。
- 規模: 中

### S06 生成器にワイルドカード対応の `Remove` 発行を入れる

- 対象: `tools/build/sakura_build_lib/generator.py`（`_msbuild_source_item_spec()`）、`tools/build/tests/test_msbuild_generation_ownership.py`、`src/main/modules/generated/msbuild/consumers/sakura_app.props`
- 内容: 「item spec にワイルドカードを含むものはスキップ」という分岐を、「ワイルドカードの `Include` があっても component が所有する具体ファイルパスに対して `<ClCompile Remove="...">` を発行する」に変える。MSBuild の `Remove` はワイルドカード展開後の項目集合に作用するため、glob + `Remove` は成立する。`py -3 tools/build/sakura_build.py generate` で全 context の生成物を更新しコミットする。
- 新しい境界: 生成物が手書き vcxproj の形式に依存しなくなる。
- テスト: `tools/build/tests/test_msbuild_generation_ownership.py` に「glob 項目が存在するときも具体パスの `Remove` が 37 件出る」ケースを追加。この pytest は `norecursedirs` の都合で CI に収集されないためローカル実行が必須（`.claude/memory/rules.md` 参照）。
- Verify: `py -3 -m pytest tools/build/tests/test_msbuild_generation_ownership.py -q`
- Expect: 全 pass。`rg -c "<ClCompile Remove=" src/main/modules/generated/msbuild/consumers/sakura_app.props` が 37。`py -3 tools/build/sakura_build.py generate --check` が `stale: []`。
- リスク: 中。生成物差分が全 context に出る。
- ロールバック: revert + `generate`。
- 規模: 小

### S07 `sakura.vcxproj` を再帰ワイルドカード化する

- 対象: `sakura_core/sakura.vcxproj`、`sakura_core/sakura.vcxproj.filters`、`src/main/modules/modules.json`（`ownership_exclusions` の確認のみ）、`sakura_core/CLAUDE.md`、ルート `CLAUDE.md`
- 内容: 662 件の `<ClCompile Include>` を `..\sakura_core\**\*.cpp`（`Exclude` に vendor の `tracing.cpp` 1 件）と `..\src\main\cpp\cxx\**\*.cpp` に置換する。メタデータ付き 11 件（PCH Create/NotUsing、`/arch:AVX*`、`WholeProgramOptimization false`）は `<ClCompile Update>` に変換する。`<ClInclude>` 669 件は触らない。生成 component 所有 37 件は S06 の `Remove` に任せる。ルート `CLAUDE.md` の「`.vcxproj` と `.filters` の両方を更新」規則を「新規 `.cpp` はディレクトリに置くだけでよい。`.filters` は S12 後に生成物」に更新する。
- 新しい境界: 製品ソース追加で触るファイルが 1 になる。P6 が成立する。
- テスト: `build-sln.bat x64 Debug` / `x64 Release`、`tests1.exe` フル、`sakura.exe` 起動・終了、`build-all.bat x64 Release`（`.asm` 生成経路）。
- Verify: `rg -c "<ClCompile Include=" sakura_core/sakura.vcxproj`
- Expect: 3 以下（現在 662）。リンクされる `.obj` 数が変更前後で 662 で一致（MAP ファイル比較）。Release ビルドログで `CpuDispatchAvx512.cpp` に `/arch:AVX512` と `WholeProgramOptimization=false` が適用されている。`sakura_core/` に空の `.cpp` を追加して `build-dev.bat x64 Debug` が `.vcxproj` 編集なしで通る。
- リスク: 高（静的初期化順、AVX ディスパッチの `/arch` 誤適用は無音の誤コード生成になる）。x64 Release 配布ビルドの `/m:1` `/CGTHREADS:1` 経路（#203）を必ず通す。
- ロールバック: revert。`sakura_app.props` は S06 の実装で具体パス `Remove` を出し続けるので明示リストに戻しても矛盾しない。
- 規模: 中

### S08 テスト台帳のマルチ runner を復旧し、退行を CI で検出する

- 対象: `src/test/test-inventory.json`、`tools/build/sakura_build_lib/test_inventory.py`、`.github/workflows/architecture-gates.yml`、`.github/workflows/build-sakura.yml`
- 内容: `12eee6960` 以前の 6 runner（`sakura_uri_tests` ほか）を `py -3 tools/build/sakura_build.py test inventory refresh-runtime --runner <id>=<exe> --remap <test_id>=<runner>::<selector>` で復旧する（`collect` は使わない。`test_id` を新規採番すると `src/test/CLAUDE.md` の安定 ID 契約を破る）。`architecture-gates.yml` に「`modules.json` の `*_tests` component がすべて台帳の runner 集合に含まれる」静的ゲート（ubuntu、1 秒未満）を追加する。`build-sakura.yml` のテスト実行後に `test inventory verify-runtime` を走らせ `missing_selectors` / `unexpected_selectors` が空であることを確認する。
- 新しい境界: 「runner が減ると CI が赤」。S11 の前提。
- テスト: 静的ゲートを `12eee6960` 時点の台帳に対して実行して fail することを確認。
- Verify: `py -3 -c "import json;d=json.load(open('src/test/test-inventory.json'));print(len({t['runtime']['runner_id'] for t in d['tests']}))"`
- Expect: 7 以上（現在 1）。既存 `test_id` の集合が復旧前後で 1 件も変わらない。
- リスク: 中。component runner の契約テストは GoogleTest ではなく `--gtest_list_tests` エミュレータなので、S10 で gtest 化する際に selector 表記を見直す。`12eee6960` で runner が消えた経緯（意図的か `collect` 誤用か）は未確認（U9）。
- ロールバック: 台帳とゲートを revert。
- 規模: 中

### S09 `plan impact` サブコマンド（変更パス → component → 最小検証コマンド）

- 対象: `tools/build/sakura_build.py`、`tools/build/sakura_build_lib/coverage_map.py`（`build_module_index()` を再利用）、`tools/build/sakura_build_lib/ci_plan.py`、`CLAUDE.md`、`sakura_core/CLAUDE.md`、`src/test/CLAUDE.md`、`tools/build.md`
- 内容: `py -3 tools/build/sakura_build.py plan impact --changed <path>...` を追加し `{component_id, runner_id, command}` を返す。所有 component を特定できないパスと force-full パターンは fail-closed で `full` を返す。4 つの `CLAUDE.md`／`build.md` に「変更したら最初にこのコマンドを打つ」表を置く。
- 新しい境界: エージェントの編集サイクルの最小検証が 1〜7 秒になる（component 所有領域）。
- テスト: `tools/build/tests/test_coverage_map.py` に 3 ケース追加。
- Verify: `py -3 tools/build/sakura_build.py plan impact --changed sakura_core/platform/uri/UriIdentity.cpp`
- Expect: `component_id` が `sakura_uri`、`runner_id` が `sakura_uri_tests`。`--changed sakura_core/window/CEditWnd.cpp` は `full`。`--changed sakura_core/sakura.vcxproj` は `full`。`rg -l "plan impact" CLAUDE.md sakura_core/CLAUDE.md src/test/CLAUDE.md tools/build.md` が 4 件。
- リスク: 低（読み取り専用）。
- ロールバック: サブコマンド削除。
- 規模: 小

### S10 component runner の GoogleTest 化と重複テスト 192 件の移設

- 対象: `src/main/modules/modules.json`、`tools/build/pilots/uri_contract_test.cpp`、`tools/build/pilots/editor_frame_contract_test.cpp`、`tools/build/sakura_build_lib/generator.py`、`src/test/cpp/tests1/platform/controlipc/`、`src/test/cpp/tests1/platform/request/`、`src/test/cpp/tests1/platform/harnessbridge/`
- 内容: 生成 `*_tests` component が gtest/gmock をリンクできるようにする（既存の `#error` による private header 到達禁止テストは共存）。第 1 段として既に component 化済みで `tests1` と重複している `platform/controlipc`（150 テスト）、`platform/request`（25）、`platform/harnessbridge`（17）を対応 component へ移し `tests1` から削除する。`test_id` は保持し `refresh-runtime --remap` で runner だけ付け替える。第 2 段以降（`markdown` 100 → `update` 64 → `debug` 46 → `config` 112 → `senp` 162 → `textmate` 46、合計約 696）は S12 の再配置後に同じ手順で行う。`workbench/` 1,508 テストは対象外（U10）。
- 新しい境界: アプリ全体をリンクしないテストランナーが実在する。
- テスト: 移設段ごとに `tests1.exe --gtest_list_tests` と component runner の件数を合算し、移設前と一致することを確認。
- Verify: `py -3 tools/build/sakura_build.py test component sakura_uri_tests --context msvc-x64-debug`
- Expect: exit 0、所要 10 秒未満、`--gtest_output=xml:` で JUnit XML が出る。`py -3 tools/build/sakura_build.py verify component-boundary sakura_uri_tests` が成功。合計テスト数 4,157 が不変。
- リスク: 中。vcpkg パッケージ閉包が増え `package plan` のハッシュが変わる。fresh worktree では submodule 未初期化で `TOOL_VCPKG_NOT_FOUND`（`.claude/memory/rules.md`）。
- ロールバック: 各段を独立コミットにし revert。
- 規模: 大

### S11 CI に `component_native` 判定を入れる

- 対象: `tools/build/sakura_build_lib/ci_plan.py`、`.github/workflows/pr-gate.yml`、`.github/workflows/build-sakura.yml`、`.github/workflows/test-results.yml`
- 内容: `ci plan` の出力語彙に `component_native` を追加する。返す条件は「変更パスがすべて `maturity ∈ {candidate, independent}` の生成 component に所有され、force-full パターンに当たらず、削除／リネームが無く、fork head でなく、閉包に `sakura_app` / `tests1` を含まない」場合のみ。それ以外は `full_native`（fail-closed）。パスフィルタは付けず、job 名（`Plan CI` / `Target policy` / `Architecture` / `Cppcheck` / `Doxygen` / `Native` / `PR Gate`）は変えず、`Native` job の内部で `build component` + `test component` に分岐する。component の XML を `tests*-googletest.xml` の命名で出し `Test Results` チェック名を維持する。`build/components/<context>/<id>/` を manifest graph hash 付きでキャッシュする。
- 新しい境界: 小さな Issue の PR が 5 分未満で緑になる。
- テスト: `src/test/py/` の ci_plan テストに 3 ケース（URI 1 本変更 → `component_native`、`CEditWnd.cpp` 変更 → `full_native`、`.vcxproj` 変更 → `full_native`）。
- Verify: `py -3 tools/build/sakura_build.py ci plan --help`
- Expect: `component_native` が語彙に載る。PR の必須チェック 7 件がすべて報告される（pending なし）。`component_native` 経路の `Native` job が 5 分未満。
- リスク: 高（選択誤りで欠陥がすり抜ける）。S08 のゲート無しに入れると「テストが消えても緑」になるため S08 を前提にする。
- ロールバック: `ci_plan.py` から `component_native` を除去すれば全 PR が `full_native` に戻る。
- 規模: 中

### S12 テストをコードと同じ階層に再配置する

- 対象: `src/test/cpp/tests1/`（直下の `test-*.cpp` 57 本）、`src/test/cpp/tests1/workbench/`（104 ファイル）、`src/test/test-inventory.json`、`sakura_core/tests1.vcxproj.filters`
- 内容: `test-window.cpp` → `src/test/cpp/tests1/window/`、`test-csharedata.cpp` → `env/` のように、テスト対象ディレクトリと同じ相対パスへ `git mv` する。`workbench/` 直下 104 ファイルは `workbench/<view>/` へ。テスト名・`test_id` は変えない。`.filters` はディレクトリ構造から生成するスクリプトを `generate` の出力に加える（`sakura_core/` に置き、force-full パターンから除外する）。
- 新しい境界: P7（テストはコードを鏡写し）。「`sakura_core/<dir>/` を直したら `src/test/cpp/tests1/<dir>/` を読む」が成立する。
- テスト: 移動のみ。`tests1.exe --gtest_list_tests` の件数と名前が不変。
- Verify: `find src/test/cpp/tests1 -maxdepth 1 -name "*.cpp" | wc -l`
- Expect: 0（現在 57）。`find src/test/cpp/tests1/workbench -maxdepth 1 -name "*.cpp" | wc -l` が 0（現在 104）。`test_count` 4,157 不変。
- リスク: 中。S05 の glob 化が前提（前提なしでは 306 行の vcxproj 編集になる）。リンク順が変わる。
- ロールバック: `git mv` を戻す 1 コミット。
- 規模: 中

### Phase 2: 依存方向を一方向にする

### S13 `CSelectLang.h` を `basis/` へ移動する

- 対象: `sakura_core/CSelectLang.h`、`sakura_core/CSelectLang.cpp`、include する 111 ファイル、`src/main/resources/sakura_rc.h`、`tools/architecture/layers.json`
- 内容: `sakura_core/basis/CSelectLang.h`（新規）へ `git mv` し、`#include "CSelectLang.h"` を `#include "basis/CSelectLang.h"` に一括置換する。`LS()` の API は変えない。`layers.json` は変更不要（`basis/` は override で L1。`CSelectLang.h` の override は `sakura_core/` 直下の catch-all から `basis/` へ自動的に移る）。
- 新しい境界: ローカライズ文字列取得が L1 の基盤になる。
- テスト: ビルドのみ。
- Verify: `py -3 tools/architecture/include_layers.py explain --from L4-legacy-ui --to L5-composition`
- Expect: `CSelectLang.h` を含むエッジ 0（現在 104）。`upward_edges_total` が 402 → 292 以下。
- リスク: 中。`src/main/cpp/cxx/` と `sakura_rc.h` への相対 include が MSBuild と CMake で同じに解決されるか要確認（`build-gnu.bat MinGW Debug` を通す）。
- ロールバック: `git mv` を戻す。
- 規模: 小

### S14 `_main/global.h` を型と合成に分割し、`_main/CMutex.h` を `_os/` へ移す

- 対象: `sakura_core/_main/global.h`、`sakura_core/basis/EditorPrimitives.h`（新規）、`sakura_core/_main/CMutex.h`、include する 47 ファイル
- 内容: `global.h` の型・定数（`SSearchOption`、`ESearchMode`、`ESearchDirection`、`ESelectCountMode`、`WRAP_TEXT_WRAP_METHOD`、`ETabWndNotifyType`、`EBarChangeNotifyType`、`COLOR_ATTRIB_*`、`SColorAttributeData` ほか）を `basis/EditorPrimitives.h` へ移し、`G_AppInstance()` / `GetProfileName()` / `IDW_STATUSBAR` だけを `global.h` に残す。`CMutex.h` は Win32 ラッパーなので `sakura_core/_os/` へ。
- 新しい境界: L1 の型を L4 が参照するだけになり、L5 への依存が消える。
- テスト: ビルドのみ。
- Verify: `py -3 tools/architecture/include_layers.py explain --from L4-legacy-ui --to L5-composition`
- Expect: `_main/global.h` を含むエッジが 36 → 6 以下、`_main/CMutex.h` が 0。`L4-legacy-ui → L5-composition` 合計 124 → 40 以下。
- リスク: 低〜中。`state.public_mutable_field` の finding が `SSearchOption` 分だけ移動する（純減ではないので G4 で `--accept-current` が要る場合がある）。
- ロールバック: revert。
- 規模: 小

### S15 誤配置リーフヘッダを所有層へ降格する

- 対象: `sakura_core/view/colors/EColorIndexType.h` → `sakura_core/types/`、`sakura_core/outline/CFuncInfoArr.h` + `sakura_core/outline/CFuncInfo.h` → `sakura_core/doc/outline/`（新規）、`sakura_core/cmd/COpe.h` + `sakura_core/cmd/COpeBlk.h` + `sakura_core/cmd/COpeBuf.h` → `sakura_core/doc/undo/`（新規）
- 内容: いずれも `basis/` / `mem/` にしか依存しない純データ型で、置き場所だけが UI 層にある。`git mv` と include 書き換えのみ。`sakura_core/view/CTextMetrics.h` は `view/` 内で完結するので動かさない。`COpe*` は既存契約 `sakura_core/include/sakura/editor/document/UndoHistory.h` の隣に置く候補だが S15 では移動のみ。
- 新しい境界: `doc/` が `view/` `outline/` `cmd/` を include しなくなる（A 群 41 + B 群 7）。
- テスト: ビルドのみ。
- Verify: `py -3 tools/architecture/include_layers.py explain --from L3-domain --to L4-legacy-ui`
- Expect: 111 → 65 以下。`EColorIndexType.h` / `CFuncInfoArr.h` / `COpe.h` を含むエッジ 0。
- リスク: 中。`COpeBuf.h` が `_main/global.h` に依存するため S14 の後に行う。
- ロールバック: `git mv` を戻す。
- 規模: 小

### S16a `StdAfx.h` から `env/DLLSHAREDATA.h` を外す

- 対象: `sakura_core/StdAfx.h`（176 行目）、`sakura_core/env/DLLSHAREDATA.h`、明示 include が必要になる TU（70 件以上【推定】。`build-dev.bat x64 Debug` のエラーで確定する）
- 内容: PCH から `env/DLLSHAREDATA.h` の include を外し、コンパイルに必要な TU にだけ `#include "env/DLLSHAREDATA.h"` を足す。呼び出しの置換は行わない（S16b・S16c）。`build-sln.bat x64 Debug` のフルビルド所要時間を変更前後で各 3 回実測し中央値を比較する。
- 新しい境界: 共有メモリは include を書いたファイルからしか見えない（アンビエントでない）。IRV の `contract` 区分に `DLLSHAREDATA.h` が入るのは、明示 include したファイルを変更する Issue だけになる。
- テスト: ビルドのみ + `tests1.exe` ヘッドレス集合（G6）。
- Verify: `rg -n "DLLSHAREDATA" sakura_core/StdAfx.h`
- Expect: 0 行。`rg -l "#include \"env/DLLSHAREDATA.h\"" sakura_core src | wc -l` の実測値を台帳に記録する（上限 120【推定】）。フルビルド時間の中央値の悪化が 5% 以下（超える場合は本ステップを保留し S16b へ進む。保留は 7.3 節に U として記録する）。
- リスク: 高（ビルド時間。`StdAfx.h` のコメントが「PCH の有無がビルド性能に大きく影響」と明記）。
- ロールバック: `StdAfx.h` の 1 行を戻す（足した明示 include は残しても無害）。
- 規模: 中

### S16b `CommonSettingsReader` 契約を追加し、`view/` の 114 呼び出しを契約経由に置換する

- 対象: `sakura_core/include/sakura/shareddata/SharedDataCapabilities.h`、`sakura_core/view/`（114 呼び出し: `sakura_core/view/CEditView_Mouse.cpp` 30、`sakura_core/view/CEditView.cpp` 27 ほか）、`sakura_core/view/CEditView.h`、`src/main/modules/modules.json`（`sakura_shareddata_contract`）
- 内容: (1) `SharedDataCapabilities.h` に `legacy::shareddata::CommonSettingsReader`（`EditSettings()` / `SearchSettings()` / `ViewSettings()` を `const&` で返す）と `CommonSettingsWriter`（グループ単位差し替え）を追加する。(2) `CEditView` に `const CommonSettingsReader&` を注入し、`view/` の 114 件を置換する。(3) `.m_sFlags` / `.m_sHandles` の `view/` 内該当分は既存 `SharedDataMacroReader/Writer` / `SharedDataWindowEndpointReader/Writer` へ差し替える。`view/` から書き込む 1 件（`sakura_core/view/CEditView_Cmdisrch.cpp`）は writer 経由にする。
- 新しい境界: `view/` は共有メモリを契約経由でしか読めない。
- テスト: `sakura_shareddata_contract_tests` に reader/writer の契約テスト（fake を注入し `EditSettings()` が同一参照を返す、writer がグループ単位で差し替える）を追加。
- Verify: `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.direct_global_getter_calls_by_file` のうち `sakura_core/view/` 配下の合計
- Expect: 0（現在 114）。`metrics.direct_global_getter_calls.GetDllShareData` が 705 → 591 以下。`rg -c "CommonSettingsReader" sakura_core/include/sakura/shareddata/SharedDataCapabilities.h` が 1 以上。`py -3 tools/build/sakura_build.py test component sakura_shareddata_contract_tests --context msvc-x64-debug` が exit 0。IRV: `view/` 配下 1 ファイルを変更する代表 Issue で `irv.py explain` の `contract` が `SharedDataCapabilities.h` 1 本になり `DLLSHAREDATA.h` を含まない。
- リスク: 中。`CEditView` の生成箇所（`CEditWnd` のペイン生成）に注入点を足す必要があり S29 の `IEditorPaneAccess` と接する。
- ロールバック: 契約ヘッダを残したまま `view/` の置換だけ revert できる。
- 規模: 大

### S16c 残る consumer（`cmd/` → `macro/`+`agent/` → `typeprop/`+`dlg/` → `env/`）を契約経由に移行する

- 対象: `sakura_core/cmd/`（174 呼び出し）、`sakura_core/macro/`（45）、`sakura_core/agent/`、`sakura_core/typeprop/`、`sakura_core/dlg/`、`sakura_core/env/`（60）
- 内容: ディレクトリごとに 1 コミットで、S16b の `CommonSettingsReader` / `CommonSettingsWriter` に置換する。書き込み 42 箇所（設定ダイアログ系に集中）は `CommonSettingsWriter` のグループ単位差し替えにする。順序は cmd → macro+agent → typeprop+dlg → env。
- 新しい境界: 共有メモリの読み書き経路が 1 つ（P5）。
- テスト: ディレクトリごとに既存テストが通ること + `sakura_shareddata_contract_tests`。
- Verify: `py -3 tools/build/sakura_build.py inventory semantic` → `metrics.direct_global_getter_calls.GetDllShareData`
- Expect: 450 以下（S16b 完了時 591 以下から）。`direct_global_getter_calls_by_file` で `sakura_core/cmd/` と `sakura_core/macro/` の合計が各 0。`rg -c "GetDllShareData\(\)" sakura_core/cmd` が 0。
- リスク: 中。`cmd/` は S17 と同じファイル群を触るため、S17 と同時に進めない（I-12 → I-13 の順）。
- ロールバック: ディレクトリ単位で revert。
- 規模: 大

### S17 `CViewCommander_inline.h` を `ICommandTarget` / `IViewContext` で解体する

- 対象: `sakura_core/cmd/CViewCommander_inline.h`、`sakura_core/cmd/CViewCommander.h`、`cmd/*.cpp` 22 本、`sakura_core/view/CEditView.h`、`sakura_core/include/sakura/editor/command/ICommandTarget.h`（新規）、`sakura_core/include/sakura/editor/command/IViewContext.h`（新規）
- 内容: `ICommandTarget`（`Selection() -> CViewSelect&`、`OwnerHandle()`、`TextArea()`、`Caret()`、`OpeBlk()` / `SetOpeBlk()`、`Document()`、編集 4 本、再描画 3 本の 15〜20 メソッド。実測 902 呼び出し / 86 メンバの上位で 80% を覆う）を `CEditView` が実装する。`IViewContext`（`Views()`、`Chrome()`、`OwnerHandle()`）は S29 の契約を合成する。`CViewCommander_inline.h` の 4 include を契約 2 本に置き換える。public field 直接アクセス 80 件（`m_pTypeData` 21、`m_bDoing_UndoRedo` 24 ほか）は先に getter 化する。
- 新しい境界: コマンド 1 本を直すときの include 文脈が約 2,550 行から 200 行未満になる【推定】。
- テスト: `sakura_editor_command_contract_tests`（新規 component）で各契約の fake がコンパイルできることを確認。
- Verify: `rg -c '#include "(view|window|doc)/' sakura_core/cmd/CViewCommander_inline.h`
- Expect: 0（ファイル削除でも可）。`py -3 tools/architecture/include_layers.py file sakura_core/cmd/CViewCommander_Edit.cpp` の includes に `window/CEditWnd.h` と `view/CEditView.h` が現れない。
- リスク: 高。`CViewCommander` の 300 超のメンバ関数が `CEditView` の public field に触っている可能性が高く、段階実施（Selection → Win32Handle → TextEdit → Redraw の順）が必要。S29 の `IEditorPaneAccess` と同時設計する（U11）。
- ロールバック: 契約ヘッダを残したまま `CViewCommander_inline.h` の include を戻せる。
- 規模: 大

### S18 `doc/` から UI を追い出す（observer 化と port 化）

- 対象: `sakura_core/doc/CEditDoc.h`、`sakura_core/doc/CEditDoc.cpp`、`sakura_core/doc/CDocFileOperation.cpp`、`sakura_core/doc/CDocVisitor.cpp`、`sakura_core/include/sakura/editor/document/IDocumentObserver.h`（新規）、`sakura_core/include/sakura/editor/document/IFileDialogService.h`（新規）
- 内容: `CEditDoc.cpp` 825〜847 行の全 view への `OnChangeSetting()` / `AdjustScrollBars()` ブロードキャストを `IDocumentObserver`（`OnSettingChanged` / `OnReloaded` / `OnCaptionInvalidated` / `OnTypeChanged`）に反転し `CEditWnd` が sink を実装する。`CDocFileOperation.cpp` の `CDlgOpenFile` 直接提示を `IFileDialogService` port にする。`CEditDoc.cpp` 592〜615 行（ペイン切替、`GetCommander().HandleCommand()`）は observer では消えないので `window/` へ移設する。`CEditDoc.h` が値所有する `CAutoReloadAgent` / `CAutoSaveAgent` / `CBackupAgent` は `CEditApp` 側の所有へ移す。
- 新しい境界: `doc/` が L4 を include しない。
- テスト: 既存 `sakura_editor_document_core` に `IDocumentObserver.h` を追加し契約テスト。
- Verify: `py -3 tools/architecture/include_layers.py explain --from L3-domain --to L4-legacy-ui`
- Expect: `sakura_core/doc/*` を source とするエッジが 5 本以下（現在 `CEditDoc.cpp` 13、`CDocFileOperation.cpp` 10 ほか）。`direct_global_getter_calls_by_file` の `sakura_core/doc/CEditDoc.cpp`（38）と `doc/CDocFileOperation.cpp`（25）が各 10 未満。
- リスク: 高。Agent 3 種の生存期間が `CEditDoc` と一致する前提のコードが未調査（U12）。
- ロールバック: observer を残したまま直接呼び出しに戻せる。
- 規模: 大

### S19 `CommandDescriptor` 一元表で func / cmd / env / window の 4 点同時変更を解消する

- 対象: `sakura_core/func/Funccode.cpp`、`sakura_core/func/Funccode.h`、`sakura_core/env/CShareData_IO.cpp`（キーバインド永続化）、`sakura_core/window/CEditWnd.cpp`（メニュー構築）、`sakura_core/include/sakura/editor/command/CommandDescriptor.h`（新規）
- 内容: `EFunctionCode` / 既定キー / メニュー文字列 ID / 実装エントリを 1 レコードにした表を `func/` に集約し、`CShareData_IO.cpp` と `CEditWnd.cpp` はその表を読むだけにする。生成 `Funccode_define.h` / `Funccode_enum.h` は編集しない（入力側を変える）。
- 新しい境界: 「コマンドを 1 本足す」が `func/` + `cmd/` の 2 ディレクトリで閉じる。
- テスト: 表の一意性（機能コード重複 0）を `sakura_core/func/` のテストで検証。
- Verify: `rg -c "CommandDescriptor" sakura_core/func/Funccode.cpp sakura_core/env/CShareData_IO.cpp sakura_core/window/CEditWnd.cpp`
- Expect: 3 ファイルすべてにヒット。導入後の「新規コマンド追加」PR の変更ディレクトリ数が 4 → 2 以下。3 か月後の再計測で `cmd`↔`func` jaccard 0.41 → 0.15 以下。
- リスク: 中。効果測定に時間がかかる。
- ロールバック: 表を残して旧経路を復活できる。
- 規模: 中

### S20 L1-foundation と L2-platform からの上向き include を解消する

- 対象: `sakura_core/util/file.cpp`（6）、`sakura_core/util/shell.cpp`（6）、`sakura_core/_os/CDropTarget.cpp`（5）、`sakura_core/env/CSakuraEnvironment.cpp`（8）ほか L1→L3 26 ファイル、L2→L3/L3-workbench-model/L4 の 10 ファイル（`sakura_core/terminal/tmux/TmuxRuntimeAdapter.h` と `sakura_core/platform/harnessbridge/` 配下の `TmuxCommandTypes.h` を含む）
- 内容: `util/` が `env/` の設定を読む箇所は引数注入に変える（`GetDllShareData()` 経由の暗黙依存が多い。S16b の contract を使う）。`_os/CDropTarget.cpp` が `view/` を include する箇所は `sakura_core/include/sakura/editor/input/`（新規）の drop 契約に置換する。`platform/` から `workbench/` への 7 本は `platform/controlipc/` のワークスペース通知型を `include/sakura/` へ出す。terminal-harness から `terminal/runtime/` への 3 本（4.1 節）は `ITerminalRuntimeService` / `TerminalRuntimeTypes` を `include/sakura/terminal/`（新規）へ出して解消する。
- 新しい境界: L1・L2 は rank 2 以下しか include しない。
- テスト: ビルドのみ。
- Verify: `py -3 tools/architecture/include_layers.py report --json`
- Expect: `L1-foundation → L3-domain` 37 → 5 以下、`L1-foundation → L4-legacy-ui` 5 → 0、`L2-platform → *` 合計 19 → 3 以下、`L0-contracts → *` 1 → 0。
- リスク: 中。`util/` の関数シグネチャが変わり呼び出し元が広い。
- ロールバック: revert。
- 規模: 中

### S21 workbench から L4/L5 への 22 本と残存シングルトンの DI 化

- 対象: `sakura_core/workbench/tasks/TaskExecutionService.h`（terminal 依存の露出）、`sakura_core/agent/CSearchAgent.h`、`sakura_core/window/CWnd.h`、`sakura_core/accessibility/`、`sakura_core/_main/CCommandLine.h`、`sakura_core/_main/CAppMode.h`
- 内容: accessibility と `CWnd.h` 依存は `sakura_core/workbench/win32/` の adapter へ移す。`outline` / `terminal` / `agent` は契約ヘッダ（`IOutlineSnapshotProvider` / `ITerminalSurface` / `IWorkspaceSearchEngine`）を `sakura_core/include/sakura/workbench/`（新規）に置き、実装を legacy 側から注入する。シングルトンは影響半径の小さい順に DI 化する: `CCommandLine`（35、起動時 immutable、`const&` を流す）→ `CAppMode`（44、`IEditorModeState` に置換）→ `CColorStrategyPool`（16、`view/` 内で閉じる）。`CEditWnd`（114）と `CEditApp`（59）は S29 以降。
- 新しい境界: `L3-workbench-model → L4-legacy-ui` が 0。
- テスト: ビルドのみ。
- Verify: `py -3 tools/architecture/include_layers.py explain --from L3-workbench-model --to L4-legacy-ui`
- Expect: 0 本（現在 12）。`L3-workbench-model → L5-composition` 10 → 2 以下。`rg -c "CAppMode::getInstance\(\)" sakura_core` が 44 → 0。
- リスク: 中。terminal 依存はヘッダに出ているため影響範囲が広い。S32 の View 再配置と同時に行わない。
- ロールバック: revert。
- 規模: 中

### Phase 3: `CEditWnd` の分解

**クラスタ抽出の共通受入 K1〜K5**（S23〜S28 の各 Expect が「K1〜K5 を満たす」と書くときの定義。物理的な行数移動は受入にしない）:

| # | 条件 | Verify（`<dst>` は抽出先ディレクトリ） | Expect |
|---|---|---|---|
| K1 | 責務所有者が 1 つ | `py -3 -c "import json;d=json.load(open('src/main/modules/modules.json'));print([c['id'] for c in d['components'] if any('<dst>'.startswith(s) for s in c['sources'])])"` | 1 件（既存 component か、そのステップで追加した component） |
| K2 | 公開契約が明示 | `rg -l "window/CEditWnd\.h" <dst>` | 0 件。`<dst>` が公開するヘッダは `sakura_core/include/sakura/` 配下か `<dst>` 直下の 1 本 |
| K3 | 旧 `CEditWnd` への逆依存なし | `rg -c "GetEditWnd\(\)|CEditWnd\b" <dst>` | 0（S29 完了前は `IEditor*` 契約経由のみ許可し、`CEditWnd` 型名は 0） |
| K4 | 独立テストコマンド | `tests1.exe --gtest_filter=<suite>.*` または `py -3 tools/build/sakura_build.py test component <id>` | `EditorTestSuite` 非継承で全 pass |
| K5 | 代表変更要求の IRV 前後と旧経路の削除 | `py -3 tools/architecture/irv.py estimate --commits <抽出前 SHA>` と `--commits <抽出後 SHA>`（代表要求は各ステップに記載） | `source_lines` が減少し、`explain` の `owning_source` が `<dst>` 1 ディレクトリだけ。旧メソッドは `rg -c "CEditWnd::<旧メソッド名>" sakura_core/window/CEditWnd.cpp` が 0（転送 1 行を除く） |

### S22 レイアウト表示状態を `WorkbenchLayoutStateService` に一元化する

- 対象: `sakura_core/window/CEditWnd.h`（`m_nWinSizeType`、`m_bDarkMode`、`m_bottomWorkbenchMaximized`、`m_bRightPanelVisible`、`m_bDispTabWnd`、`m_bDispTabWndMultiWin`、`m_bDispSTATUSBAR`）、`sakura_core/workbench/layout/WorkbenchLayoutStateService.h`、`sakura_core/window/CEditWnd.cpp`（`OnSize2` 438 行）
- 内容: 7 フラグを `CEditWnd` から削除し、`WorkbenchLayoutStateService` を唯一の真実にする。`CEditWnd` は private アクセサ経由で読むだけ。フラグ 1 個ずつ 7 コミット。**このステップを飛ばすと以降の全ステップで二重管理バグが再生産される。**
- 新しい境界: 表示状態の書き込み元が 1 つ。
- テスト: `src/test/cpp/tests1/workbench/WorkbenchLayoutStateServiceTest.cpp` に「`CEditWnd` を介さず状態遷移が決まる」ケースを追加。
- Verify: `rg -c "m_nWinSizeType|m_bottomWorkbenchMaximized|m_bRightPanelVisible|m_bDispTabWnd|m_bDispSTATUSBAR" sakura_core/window/CEditWnd.h`
- Expect: 0（宣言が消えている）。`tests1.exe --gtest_filter=WorkbenchLayoutStateServiceTest.*` 全 pass。`stale-pixel-verification` の二重撮影で差分がノイズフロア以下。
- リスク: 中〜高。書き込みタイミングが変わると `OnSize2` の分岐順が変わりレイアウトが 1 フレームずれる。
- ロールバック: フラグ単位で revert。
- 規模: 中

### S23 `WorkbenchComposition` を `CEditWnd` から抽出する

- 対象: `sakura_core/window/CEditWnd.h`、`sakura_core/window/CEditWnd.cpp`（`RegisterBuiltinCommands` 3467 行付近、`RegisterGitCommands` 3814、`RegisterExplorerCommands` 3897、`m_workbenchRuntime->` 呼び出し約 130 箇所）、`sakura_core/workbench/composition/WorkbenchComposition.h`（新規）、`sakura_core/workbench/composition/WorkbenchComposition.cpp`（新規）、`sakura_core/workbench/commands/WorkbenchCommandRegistry.h`
- 内容: View への配線（`SetSourceControlService` 等）とコマンド executor のバインドを `WorkbenchComposition` へ移す。`CEditWnd` は `WorkbenchComposition` を 1 個保持し、生成と破棄だけを行う。`m_workbenchRuntime->` の直接呼び出しを 0 にする。移送は View 1 つずつ（呼び出しは 2,246〜10,087 行に散在するため、先に機能別にクラスタリングする）。
- 新しい境界: composition root が Win32 ウィンドウの外に出る。workbench 変更の 3 回に 1 回 `CEditWnd.cpp` を巻き込む構造（51/153）が解消される。
- テスト: `src/test/cpp/tests1/workbench/composition/WorkbenchCompositionTest.cpp`（新規）で fake runtime を渡し「全 View に必要なサービスが配られる」「Stop 時に逆順で外れる」を検証。`EditorTestSuite` 非継承。
- Verify: `rg -c "m_workbenchRuntime->" sakura_core/window/CEditWnd.cpp`
- Expect: 0（現在約 130）。K1〜K5 を `sakura_core/workbench/composition/`（本ステップで新規）について満たす（K5 の代表要求: 「View 1 つに新しいサービスを配線する」。抽出前は `CEditWnd.cpp` 16,806 行が `owning_source`、抽出後は `WorkbenchComposition.cpp` のみ）。`wc -l sakura_core/window/CEditWnd.cpp` の減少量は台帳に記録する（受入条件ではない）。
- リスク: 高。`Start()` の 9 箇所の `terminalResult()` チェックが示すとおり起動順に意味がある。順序表を先に文書化し、関数本体だけを移す。
- ロールバック: View 単位で revert。
- 規模: 大

### S24 WB クラスタを既存 projection サービスへ収束させる

- 対象: `sakura_core/window/CEditWnd.cpp`（`SetWorkbenchPanelVisible` 119 行、`ApplyCurrentWorkbenchLayoutState` 82、`ActivateBuiltinWorkbenchView` 79、`ReorderViewContainerInActivityBar` 89 ほか WB 47 メソッド 1,595 行）、`sakura_core/workbench/win32/PaneCompositeProjectionService.h`、`sakura_core/workbench/win32/BuiltinPartProjection.h`
- 内容: Part 1 種類ずつ（Primary Side Bar → Panel → Auxiliary Bar → Activity Bar）投影サービスへ移す。`CEditWnd` には `m_left/right/bottomWorkbenchPanel`、`m_viewContainerPages`、`m_activityBar`、`m_auxiliaryActivityBar` の所有だけを残す。S22 完了が前提。
- 新しい境界: 3 パネルの可視性変更が投影サービス内で原子的に行われる（既存コミット `daa3008cc` の設計を維持）。
- テスト: `src/test/cpp/tests1/workbench/PaneCompositeProjectionServiceTest.cpp`、`src/test/cpp/tests1/workbench/BuiltinPartProjectionTest.cpp`、`src/test/cpp/tests1/workbench/HostViewContainerProjectionTest.cpp` を拡張。
- Verify: `rg -c "m_leftWorkbenchPanel|m_rightWorkbenchPanel|m_bottomWorkbenchPanel" sakura_core/window/CEditWnd.cpp`
- Expect: 10 以下（現在 65）。K1〜K5 を `sakura_core/workbench/win32/`（投影サービス）について満たす（K5 の代表要求: 「Panel の可視性トグルの不具合を直す」。抽出後の `owning_source` は投影サービス 2 ファイル）。`stale-pixel-verification` で 3 パネルのトグルが 1 フレームで反映され、中間状態のフレームが観測されない。
- リスク: 高（原子性）。原子性を崩すコミットは即 revert。
- ロールバック: Part 単位で revert。
- 規模: 大

### S25 `InitializeWorkbench` の phase 分割と WS クラスタの抽出

- 対象: `sakura_core/window/CEditWnd.cpp`（`InitializeWorkbench` 2828〜4102 行、`CloseWorkbench` 109 行、`ApplyFolderWorkspace` 98 行ほか WS 18 メソッド 602 行）、`sakura_core/workbench/WorkbenchBootstrapSequence.h`（新規）、`sakura_core/workbench/workspace/WorkspaceTransitionCoordinator.h`（新規）、`sakura_core/include/sakura/editor/lifecycle/EditorAppLifecycle.h`、`sakura_core/workbench/workspace/WorkspaceWindowTransitionService.h`
- 内容: `InitializeWorkbench` を 5〜7 個の `Phase*` 関数に分け、`EditorAppLifecycle.h` の既存 phase 型を再利用する。phase ごとに対応する teardown を同じコミットで書く（`CloseWorkbench` との対称性）。WS クラスタは `WorkspaceTransitionCoordinator` へ移し、`CEditWnd&` ではなく `IWorkbenchRuntime&` + `WorkbenchLayoutStateService&` を受ける。`modules.json` に `sakura_workspace_transition` + `_tests` を追加する。
- 新しい境界: 起動順が phase 列挙として読める。
- テスト: `src/test/cpp/tests1/workbench/WorkbenchBootstrapContextTest.cpp` に phase 順序の表明を追加。`src/test/cpp/tests1/workbench/WorkspaceWindowTransitionServiceTest.cpp`、`src/test/cpp/tests1/workbench/ProjectsTest.cpp` を新契約に接続。
- Verify: `rg -n "CEditWnd::InitializeWorkbench" sakura_core/window/CEditWnd.cpp`
- Expect: K1〜K5 を `sakura_core/workbench/workspace/` について満たす（K5 の代表要求: 「フォルダワークスペース切替時の状態復元バグを直す」）。関数本体が 120 行以下（現在 1,275）。`rg -c "CloseWorkbench|ApplyFolderWorkspace" sakura_core/window/CEditWnd.cpp` が各 1〜2（転送のみ）。`py -3 tools/build/sakura_build.py graph check --all-contexts` で component 41 個目が解決される。`tests1.exe --gtest_filter=EditWndTest.*` 131 件 pass。
- リスク: 高。破棄順序が非対称になると二重解放（`memory.raw_delete` 18 件）。
- ロールバック: phase 単位で revert。
- 規模: 大

### S26 MENU クラスタを `MenuModel`（純粋）+ `EditWndMenuBuilder`（Win32）に分ける

- 対象: `sakura_core/window/CEditWnd.cpp`（`InitMenu` 133 行、`InitMenu_Function` 161、`InitMenu_Special` 111、`CreateFileDropDownMenu` 83 ほか MENU 36 メソッド 1,193 行）、`sakura_core/uiparts/CMenuDrawer.h`、`sakura_core/workbench/commands/MenuModel.h`（新規）、`sakura_core/window/editwnd/EditWndMenuBuilder.h`（新規）
- 内容: 「どの項目が有効か」を Win32 なしで決める純粋関数群を `MenuModel` に、`HMENU` に焼く処理を `EditWndMenuBuilder` に分ける。`modules.json` に `sakura_menu_model`（純粋）+ `sakura_win32_menu_builder` を追加。
- 新しい境界: メニュー有効判定が HWND なしでテストできる。
- テスト: `src/test/cpp/tests1/workbench/commands/MenuModelTest.cpp`（新規）。HWND を 1 つも作らない。
- Verify: `rg -c "HMENU" sakura_core/window/CEditWnd.cpp`
- Expect: `HMENU` の出現がメニュー構築の Win32 焼き込み（`EditWndMenuBuilder` への転送）だけになる（実測値を台帳に記録）。K1〜K5 を `sakura_core/workbench/commands/`（`MenuModel`）について満たす（K5 の代表要求: 「メニュー項目 1 つの有効/無効判定を直す」。抽出後の `owning_source` は `MenuModel.h/.cpp` とそのテストのみで、`CEditWnd.cpp` を含まない）。`tests1.exe --gtest_filter=MenuModelTest.*` 全 pass。
- リスク: 中。有効/無効判定はコマンド状態に依存し、タイミングを誤るとグレーアウトが残る。
- ロールバック: revert。
- 規模: 中

### S27 SCM クラスタを `ScmCommandExecutor` に抽出する

- 対象: `sakura_core/window/CEditWnd.cpp`（`ExecuteGit*` 12 メソッド 1,210 行: `ExecuteGitSyncCommand` 191、`ExecuteGitBranchCommand` 187、`ExecuteGitSelectedRangesCommand` 179、`ExecuteGitCommitCommand` 168 ほか）、`sakura_core/workbench/scm/ScmCommandExecutor.h`（新規）、`sakura_core/include/sakura/editor/scm/ScmCommandRequest.h`（新規）
- 内容: HWND を渡さず、結果は S29 の `IEditorChromeNotifier` 経由で返す。`CEditWnd` には `m_scmTool` の所有と 1 行の転送だけを残す。`modules.json` に `sakura_scm_commands` を追加。
- 新しい境界: SCM コマンドが `window/` を知らない。
- テスト: 既存 `src/test/cpp/tests1/workbench/GitSourceControlTest.cpp` ほか 6 本を新契約に接続。
- Verify: `rg -c "CEditWnd::ExecuteGit" sakura_core/window/CEditWnd.cpp`
- Expect: 0。K1〜K5 を `sakura_core/workbench/scm/` について満たす（K5 の代表要求: 「Git commit コマンドのエラー表示を直す」）。`tests1.exe --gtest_filter=Git*Test.*` 全 pass。`py -3 tools/build/sakura_build.py inventory semantic --collect-only` の `boundary.win32_type`（`CEditWnd.cpp`）が 199 から減少（値を台帳に記録）。
- リスク: 低〜中。Git 処理中のモーダル UI とメッセージポンプの相互作用。
- ロールバック: revert。
- 規模: 中

### S28 MD / EXPL / TERM / UPD / SENP クラスタを所有ディレクトリへ物理移動する

- 対象: `sakura_core/window/CEditWnd.cpp`（MD 465 行 / EXPL 327 / TERM 224 / UPD 161 / SENP 278 = 1,455 行）、`sakura_core/markdown/`、`sakura_core/workbench/explorer/`、`sakura_core/terminal/window/CTerminalTool.h`、`sakura_core/workbench/win32/ProblemsOutputPanelProjection.h`、`sakura_core/update/UpdateComposition.h`、`sakura_core/senp/SenpRuntimeService.h`
- 内容: MD → `sakura_core/markdown/MarkdownPreviewHost.h`（新規）、EXPL → `sakura_core/workbench/explorer/ExplorerCommandExecutor.h`（新規）、TERM → 既存 `CTerminalTool` と `ProblemsOutputPanelProjection` へ吸収、UPD → 既存 `UpdateComposition` へ吸収、SENP → `sakura_core/workbench/editor/SenpEditorHost.h`（新規）。いずれも専用テストが既に存在する（`src/test/cpp/tests1/markdown/` 5 本、ExplorerToolTest 系 4 本、TerminalToolTest、WorkbenchUpdateCommandsTest、Senp 系 28 本）。
- 新しい境界: 各機能の Issue が `CEditWnd.cpp` を開かずに済む。
- テスト: 新規テスト追加なしで既存テストが通ることを確認。
- Verify: `wc -l sakura_core/window/CEditWnd.cpp`
- Expect: 5 クラスタそれぞれについて K1〜K5 を抽出先（`markdown/`、`workbench/explorer/`、`terminal/window/`、`update/`、`workbench/editor/`）について満たす（K5 の代表要求はクラスタごとに 1 つ: Markdown プレビューの更新遅延 / Explorer のコンテキストメニュー / Terminal パネルの再表示 / 更新確認の失敗表示 / SENP ホストの再起動）。`CEditWnd.cpp` の減少量（1,455 行前後【推定】）は台帳に記録する。`tests1.exe --gtest_filter=MarkdownPreview*:Explorer*:TerminalToolTest.*:*Senp*` 全 pass。
- リスク: 低。
- ロールバック: クラスタ単位で revert。
- 規模: 中

### S29 `GetEditWnd()` を 7 契約に分割する（ISP）

- 対象: `sakura_core/window/CEditWnd.h`、`sakura_core/window/CEditWnd.cpp`、`sakura_core/include/sakura/editor/win32/Win32EditorFrameAdapter.h`、`sakura_core/include/sakura/editor/EditorFrameEvents.h`、`sakura_core/view/`、`sakura_core/doc/`、`sakura_core/outline/`、`sakura_core/macro/`、`sakura_core/agent/`、`sakura_core/func/`
- 内容: 1 コミット = 1 インタフェース、対象ディレクトリ単位でまとめて置換する（ratchet の「触ったファイルで純減」を満たすため）。順序: (1) `IEditorFrameHandle`（HWND 21、`Win32EditorFrameAdapter.h` を拡張）→ (2) `IEditorWheelPolicy`（16、`view/` 100% なので契約を切らず状態ごと `view/` へ移す）→ (3) `IEditorTypography`（約 15）→ (4) `IEditorChromeNotifier`（約 33、`EditorFrameEvents.h` の `EditorFrameEffectKind` を拡張）→ (5) `IEditorLayoutRefresh`（20）→ (6) `IEditorDialogRegistry`（32）→ (7) `IEditorPaneAccess`（78、最大かつ最後。S17 の `ICommandTarget` と同時設計）。契約は `sakura_core/include/sakura/editor/view/`（新規）ほか 4.2 節のディレクトリに置く。`modules.json` に `sakura_editor_views`（契約のみ）を追加。
- 新しい境界: `CEditWnd.h`（1,509 行）を include せずに機能要求ができる。
- テスト: 契約ごとに fake を作る contract test。
- Verify: `rg -c "GetEditWnd\(\)" sakura_core`
- Expect: 254 → 40 以下。`py -3 tools/build/sakura_build.py inventory semantic` の `direct_global_getter_calls.GetEditWnd` が 258 → 60 以下。`rg -o --no-filename "#include \"window/CEditWnd\.h\"" sakura_core src | wc -l` が 66 → 12 以下。
- リスク: 中〜高。`IEditorPaneAccess` が返す `CEditView&` を consumer がそのまま使うと `CEditView.h` の include が残る。二次契約（S17 の `ICommandTarget`）を同時に切る。
- ロールバック: インタフェース単位で revert。
- 規模: 大

### S30 `DispatchEvent` の typed 化と `CEditWnd` サイズ ratchet

- 対象: `sakura_core/window/CEditWnd.cpp`（`DispatchEvent` 12334〜13359 行、88 `case`、`MYWM_*` 22 種）、`sakura_core/include/sakura/editor/EditorFrameEvents.h`、`tools/architecture/size_ratchet.py`（新規）、`tools/build/baselines/ceditwnd-size.json`（新規）、`.github/workflows/architecture-gates.yml`
- 内容: (1) `MYWM_*` 22 種を実施時に `rg -n "case MYWM_" sakura_core/window/CEditWnd.cpp` で列挙し、送信元が workbench 側の通知であるものから 1 case ずつ `EditorFrameEventKind` へ移す（`HandleEditorFrameEvent` の既存パターン）。本ステップで移す数は 12 種（残り 10 種は Win32 メッセージポンプに密結合のため次の Issue）。移した case の旧分岐は削除する。(2) `wc -l` の値を `ceditwnd-size.json` に固定し、`size_ratchet.py check --baseline` が増加を exit 1 にする。`architecture-gates.yml` に載せる。typed 化率 100% は複数四半期の作業であり、本ステップの到達点ではない。
- 新しい境界: `CEditWnd.cpp` は増えない。新しい `MYWM_*` を足す経路が `EditorFrameEvents.h` 側に一本化される。
- テスト: case 移動ごとに `EditWndTest.*` 131 件と `stale-pixel-verification`。`tools/architecture/tests/test_size_ratchet.py`（新規）でベースライン +1 行の入力に対して exit 1。
- Verify: `rg -c "case MYWM_" sakura_core/window/CEditWnd.cpp`
- Expect: 10 以下（現在 22）。`rg -c "EditorFrameEventKind::" sakura_core/window/CEditWnd.cpp` が移した case 数（12）以上増える。`py -3 tools/architecture/size_ratchet.py check --baseline tools/build/baselines/ceditwnd-size.json` が exit 0。IRV: 代表要求「workbench 通知 1 種を新設して `CEditWnd` に届ける」で `irv.py explain` の `owning_source` に `DispatchEvent` 本体（1,026 行）が入らず、`EditorFrameEvents.h` + ハンドラ 1 ファイルで閉じる。**`CEditWnd.cpp` 5,500 行以下は本ステップの受入条件ではなく Phase 3 完了ゲート E3-1 である。**
- リスク: 高（メッセージ順序）。`WM_SIZE` → `WM_PAINT` の順が変わると起動時に 1 フレーム空白が出る（#226 と同型）。
- ロールバック: case 単位で revert。ベースライン JSON は同じコミットで戻す。
- 規模: 大

### Phase 3 完了ゲート E3（I-18 のクローズ条件。単一ステップの受入ではない）

| # | Verify | Expect |
|---|---|---|
| E3-1 | `wc -l sakura_core/window/CEditWnd.cpp` | 5,500 以下（現在 16,806）。補助指標 |
| E3-2 | `wc -l sakura_core/window/CEditWnd.h` | 500 以下（現在 1,509） |
| E3-3 | `py -3 tools/architecture/irv.py estimate --issue <N> --json` を Phase 3 中に閉じた代表 Issue 3 件（メニュー / SCM / レイアウト各 1）で実行 | 各 Issue で `source_lines` ≤ 2,500、`guide_files` ≤ 3、`explain` の `owning_source` に `CEditWnd.cpp` が含まれない |
| E3-4 | `rg -l "window/CEditWnd\.h" sakura_core/workbench/composition sakura_core/workbench/win32 sakura_core/workbench/workspace sakura_core/workbench/commands sakura_core/workbench/scm sakura_core/markdown`（`composition/` と `workspace/` は Phase 3 で新規） | 0 件（抽出先 6 箇所に逆依存なし） |
| E3-5 | S23〜S28 の K1〜K5 表 | 6 クラスタ × 5 項目がすべて Verify 結果つきで Issue に記録されている |
| E3-6 | `rg -c "GetEditWnd\(\)" sakura_core` | 40 以下（S29） |

### Phase 4: workbench spine

### S31 組み込み View の登録経路を `ViewContainerPageRegistry` に一本化する

- 対象: `sakura_core/workbench/viewcontainer/CViewContainerPages.h`、`sakura_core/workbench/viewcontainer/CViewContainerPages.cpp`、`sakura_core/workbench/viewcontainer/ViewContainerPageRegistry.h`、`sakura_core/workbench/explorer/`、`sakura_core/workbench/scm/`、`sakura_core/workbench/search/`
- 内容: `CViewContainerPages.h` の 5 本の `#include "workbench/<view>/C*Tool.h"` を削除し、各 View が自ディレクトリで `RegisterViewContainerPage(...)` を提供する self-registration に置き換える。View ID（`workbench.view.*`）と既定配置を移行前後で表にして突合し、差分があれば実 VS Code を確認する。
- 新しい境界: View 追加が「新ディレクトリ 1 つ + registry への 1 行」で済む。実績のある経路（`4b94e75e2` SENP Tree Views、25 ファイル・spine 0）に統一する。
- テスト: 既存 ViewContainer 系テストに「未登録 View ID は typed failure を返す」ケースを追加。
- Verify: `rg -c "#include \"workbench/.*/C.*Tool\.h\"" sakura_core/workbench/viewcontainer/CViewContainerPages.h`
- Expect: 0（現在 5）。次回の View 追加コミットで `git show --stat --name-only` に `IWorkbenchRuntime.h` / `CWorkbenchRuntime.*` / `CEditWnd.*` が現れない。
- リスク: 中。静的構築順に依存した初期化が隠れている可能性。VS Code 互換ルール（R6）。
- ロールバック: revert。
- 規模: 中

### S32 標準 View レイアウト（model / service / win32）を `scm/` で先行適用する

- 対象: `sakura_core/workbench/scm/`（37 ファイル 16,039 行）: `sakura_core/workbench/scm/GitDiffModel.h` ほか Git モデル群 → `model/`、`sakura_core/workbench/scm/SourceControlService.h`、`sakura_core/workbench/scm/GitScmPublisher.h` → `service/`、`sakura_core/workbench/scm/CScmWorkbenchTool.h`（3,783 行）、`sakura_core/workbench/scm/ScmNativeSurfaceAdapter.h` → `win32/`；`src/test/cpp/tests1/workbench/scm/`（新規）（S12 で作成）
- 内容: `model/` は `StdAfx.h` を include せず HWND を書かない。`service/` は `win32/` を include しない。`modules.json` に `sakura_scm_model` + `sakura_scm_model_tests` + `tools/build/pilots/scm_model_contract_test.cpp`（新規）を追加。`CScmWorkbenchTool.h` の `State()` を private にし `GitScmViewModel BuildViewModel(const GitScmState&)` だけを公開する（「`GitScmState` は render source ではない」規約を型で守る）。次に `explorer/`（`sakura_core/workbench/explorer/CExplorerTool.cpp` 2,597 行）へ同じレイアウトを適用する。
- 新しい境界: 「`sakura_core/workbench/<view>/model/`」という位置だけで純粋性が分かる。
- テスト: `src/test/cpp/tests1/workbench/scm/model/`（新規）と `service/` に分割。
- Verify: `rg -l "StdAfx\.h" sakura_core/workbench/scm/model/`
- Expect: 0 件（`model/` は新規ディレクトリのため、この行の `（新規）` 印は不要。S32 実施後に存在する）。`rg -l "\bHWND\b" sakura_core/workbench/scm --glob "!win32/*"` が 0 件（コメント除去後）。`find sakura_core/workbench/scm -maxdepth 1 -name "*.cpp" | wc -l` が 0。`rg -n "const GitScmState& State" sakura_core/workbench/scm/` が 0。
- リスク: 中。`CScmWorkbenchTool.cpp` 3,783 行の分割で状態共有が露出する可能性（未読、U13）。
- ロールバック: ディレクトリ移動を戻す。
- 規模: 大

### S33 `CWorkbenchRuntime.cpp` のドメインロジックを所有ドメインへ返し、共有カーネルを 1 つにする

- 対象: `sakura_core/workbench/CWorkbenchRuntime.cpp`（2,107 行）、`sakura_core/workbench/CWorkbenchRuntime.h`（`ListenerGate` 172〜178 行）、`sakura_core/workbench/kernel/`（新規）（`ListenerGate.h`、`OwnerRegistry.h`、`Revision.h`、`StopOutcome.h`、`CLAUDE.md` を置く）、`sakura_core/config/`、`sakura_core/workbench/layout/`、`sakura_core/workbench/statusbar/`、`sakura_core/workbench/output/`、`sakura_core/workbench/workspace/`、`sakura_core/workbench/tasks/`、`sakura_core/workbench/ports/`、`sakura_core/workbench/problems/`
- 内容: `ReloadWorkspaceSettingsNow`（254 行）/ `ApplyWorkspaceSettings` → `config/`、`RestoreInitialLayoutMemento` / `PersistFinalLayoutMemento` → `layout/`、`RestoreStatusbarVisibilityMemento` → `statusbar/`、`InitializeOutputProvider`（73 行）→ `output/`、`StartWorkspaceArtifacts` / `ReconcileTaskCatalogs` → `workspace/` + `tasks/`、無名 namespace ヘルパ 243 行 → 各ドメインの純粋関数ヘッダ。`CWorkbenchRuntime.cpp` に残すのは ctor 注入 / `Start()` 順序 / `Stop()` 逆順 / revision 管理だけ。`ListenerGate` を先に `kernel/` へ昇格して単体テスト可能にしてから着手する。6 ディレクトリで重複する owner+generation / tombstone / `expectedRevision` CAS / `callbackDrainDeferred` 規約を `kernel/` の型（`OwnerRegistry<T>`、`CasResult`、`StopOutcome`）に集約する（先に 6 実装の差分表を作る）。`modules.json` に `sakura_workbench_kernel` + `_tests` を追加。
- 新しい境界: サービスカーネル規約が型で 1 箇所。
- テスト: `src/test/cpp/tests1/workbench/kernel/`（新規）。`CWorkbenchRuntimeTest.cpp` は「順序」と「終端結果」のみに縮小。
- Verify: `wc -l sakura_core/workbench/CWorkbenchRuntime.cpp`
- Expect: 1,100 以下（現在 2,107）。`rg -l "maximumOwners|callbackDrainDeferred|expectedRevision" sakura_core/workbench --glob "!kernel/*" --glob "!*CLAUDE.md"` のディレクトリ数が 6 → 1。
- リスク: 高。`Start()` の `terminalResult()` チェック 9 箇所と `callbackDrainDeferred` の Stop 意味論を壊すと停止時デッドロックか二重解放。
- ロールバック: 関数単位で revert。
- 規模: 大

### S34 `IWorkbenchRuntime` を役割別インタフェースに分割する

- 対象: `sakura_core/workbench/IWorkbenchRuntime.h`（virtual 35）、`sakura_core/workbench/CWorkbenchRuntime.h`、`sakura_core/include/sakura/workbench/`（新規）、`src/main/modules/modules.json`
- 内容: S23・S33 の後に行う（先にやっても分割した IF が全部 `CEditWnd` に渡るだけ）。12 の役割別 IF（`IWorkbenchBootstrapAccess`、`IConfigurationAccess`、`IWorkspaceContextAccess`、`IWorkspaceDocumentAccess`、`IWorkspaceNavigationAccess`、`ILayoutStateAccess`、`IMarkerAuthority`、`IOutputAuthority`、`IScmAuthority`、`ISenpHostAccess`、`ITaskExecutionAccess`、`IWorkbenchLifecycleObserver`）に分け、本番 consumer 0 の 3 メソッドは契約から外して具象の public に降格、既定実装つき virtual 10 個は廃止し `WorkbenchRuntimeTestDouble` に吸収する。`modules.json` に `sakura_workbench_contracts`（ヘッダのみ）を追加。
- 新しい境界: 各 View は必要な役割だけを受け取る（最小権限）。
- テスト: 各 IF に fake を 1 つずつ作れることを確認する contract test。
- Verify: `rg -c "virtual " sakura_core/workbench/IWorkbenchRuntime.h`
- Expect: 単一ヘッダの 35 が消え、分割後の各契約ヘッダの `virtual` が 6 以下。`= 0;` でない virtual が契約ヘッダに 0。`rg -n "OutputProviderHealth|TaskCatalogForFolder" sakura_core/workbench/IWorkbenchRuntime.h` が 0。
- リスク: 中。機械的分割だがヘッダ数が増える。`ITaskExecutionAccess` を作るか `TaskExecution` 系を削除するかは未決（U14）。
- ロールバック: revert。
- 規模: 中

### Phase 5: エージェント文脈

### S35 `CLAUDE.md` を不変条件だけに絞り、履歴を `docs/subsystems/`（新規）へ移す

- 対象: `sakura_core/workbench/scm/CLAUDE.md`（112,432）、`sakura_core/window/CLAUDE.md`（51,756）、`sakura_core/terminal/CLAUDE.md`（36,520）、`sakura_core/workbench/CLAUDE.md`（35,851）、`sakura_core/senp/CLAUDE.md`（33,842）、`sakura_core/workbench/editor/CLAUDE.md`（26,838）、`.github/CLAUDE.md`（26,372）、`docs/subsystems/`（新規）、`tools/build/baselines/claude-md-budget.json`（新規、S04）
- 内容: 各 `CLAUDE.md` を「不変条件 / 境界 / 検証コマンド / 禁止事項 / 所有しないもの」の 5 節テンプレートに書き直し、日付付き履歴節（25 ファイル・85 節・190,832 バイト。Issue 番号・日付・経緯）は `docs/subsystems/<dir>/history.md` へ節ごと移す（Issue 番号と日付は保持）。設計解説（全体の 51.5%）は `docs/subsystems/<dir>/design.md` へ。履歴節には現役の不変条件が埋もれている（例: 「status bar は WS_EX_COMPOSITED にしない」は 2026-08-20 の #226 節にある）ため、85 節を目視で「不変条件 / 履歴」に二分類してから移す（3〜5 時間【推定】）。SCM の VS Code 差分カタログ（約 46 KB）は「Divergence requires a written reason」規則の対象なので `CLAUDE.md` に残すか `design.md` に移してリンクするかを I-21 の着手時に決める（U17）。共通のサービスカーネル規約は S33 の `sakura_core/workbench/kernel/CLAUDE.md`（新規）1 本に集約し他はリンクにする。S04 のゲートを絶対上限（単一 16 KB、祖先チェーン 60 KB）に切り替える。
- 新しい境界: 「`CLAUDE.md` を読む量」が Issue の規模と無関係な下限を持たなくなる。
- テスト: S04 のゲート。
- Verify: `py -3 tools/architecture/irv.py estimate --issue <代表バグ修正 Issue> --json`（代表は 6.1 節の小規模 Issue 3 件 #217 / #266 / #276。IRV の `guide` 区分は祖先 `CLAUDE.md` と `@` import の自動読込分）
- Expect: **一次**: 代表 3 件すべてで `guide_files` ≤ 3 かつ `guide_lines` ≤ 450（現状は 6.1 節の値: 9 / 1,657、8 / 1,563、13 / 2,068）。履歴を別文書へ「移しただけ」でないこと: `rg -c "@docs/subsystems" CLAUDE.md $(git ls-files "*/CLAUDE.md")` が 0（`@` import で自動読込に戻さない）、`rg -n "先に.*history\.md|history\.md.*を(先に|必ず)読" $(git ls-files "*/CLAUDE.md")` が 0 行（毎回読めという指示を置かない）、`irv.py explain` の `guide` に `docs/subsystems/`（S35 で新規）が現れない。**二次**: `find sakura_core -name CLAUDE.md -size +16k | wc -l` が 0（現在 `scm` `window` `terminal` `workbench` `senp` `workbench/editor` の 6 件が超過）。`git ls-files -- CLAUDE.md "*/CLAUDE.md" | xargs wc -c` の合計が 658,157 → 350,000 以下。6.3 節の #285（`claude_md_bytes` 182,172）を再計測して 100,000 以下。
- リスク: 中。履歴を移す際に「なぜその不変条件があるか」の根拠が切れる。各不変条件に `history.md` の節へのリンクを残す。
- ロールバック: `git mv` を戻す。
- 規模: 大

### S36 所有マップと鏡写し規則のゲート

- 対象: `tools/architecture/ownership.json`（新規）、`tools/architecture/ownership_check.py`（新規）、`.github/workflows/architecture-gates.yml`
- 内容: `sakura_core/<dir>/` ごとに「層 ID / component ID / テストディレクトリ / `CLAUDE.md` / 形式モデル（あれば）」を 1 レコードにした所有マップを置き、(1) `git ls-files` の全 `.cpp`/`.h` が正確に 1 レコードに属す、(2) レコードのテストディレクトリが存在する、(3) `modules.json` の owner と矛盾しない、を検査する。S09 の `plan impact` はこのマップを一次情報にする。
- 新しい境界: 「このファイルは誰のものか」がツールで答えられる。
- テスト: `tools/architecture/tests/test_ownership_check.py`（新規）。
- Verify: `py -3 tools/architecture/ownership_check.py`
- Expect: exit 0、未割当パス 0、テストディレクトリ欠落 0。
- リスク: 低。
- ロールバック: ステップ削除。
- 規模: 小

### S37 命名・配置規則の 1 表化とテンプレート

- 対象: `sakura_core/CLAUDE.md`、`src/test/CLAUDE.md`、`sakura_core/include/sakura/editor/`、`sakura_core/window/editwnd/`（新規）
- 内容: 「契約ヘッダは `sakura_core/include/sakura/<subsystem>/<Concern>.h`、Win32 実装は `sakura_core/workbench/win32/` か `sakura_core/window/editwnd/`、純粋ロジックは `sakura_core/workbench/<view>/model/`、テストは同じ相対パス、`CEditWnd` に置くことは既定選択肢にしない」を 1 表にして `sakura_core/CLAUDE.md` に置く。新規ディレクトリ用の `CLAUDE.md` テンプレート（4 節）を `docs/subsystems/TEMPLATE.md`（新規）に置く。
- 新しい境界: エージェントが「どこに置くか」を推測しなくてよい。
- テスト: S36 のゲートがテンプレート準拠を検査。
- Verify: `rg -c "既定選択肢にしない" sakura_core/CLAUDE.md`
- Expect: 1 以上。`ls sakura_core/include/sakura/editor` に `view` `dialog` `chrome` `layout` `input` `command` が存在する。
- リスク: なし。
- ロールバック: 文書のみ。
- 規模: 小

### Phase 6: 形式検証境界

### S38 モデル ↔ コード対応を機械可読にしてゲート化する

- 対象: `docs/formal/README.md`、`docs/formal/models.json`（新規）、`tools/verify-formal-map.py`（新規）、`.github/workflows/architecture-gates.yml`、`tools/verify-search-lifecycle.py`、`tools/verify-senp-github-models.py`
- 内容: 6 本の `.tla` それぞれについて「モデル名 / `.cfg` 一覧と mutant の期待結果 / 対応するコードのシンボル（型名・関数名）/ 対応するテスト / `verified_commit`」を `models.json` に置く。`verify-formal-map.py` は各シンボルが `rg` で実在することと、対応コードファイルの最終変更コミットが `verified_commit` より新しい場合に警告（`--strict` で exit 1）を出すドリフトゲート。逃げ道として `verified_commit` の更新を許し、「モデルを直せ」ではなく「読んだと記録しろ」を強制する（初回は warn モードで運用）。`architecture-gates.yml` の `Verify SENP GitHub models` の直後に追加する。対応コードのヘッダ先頭に `// formal: docs/formal/<Model>.tla` の 1 行を置き、コード側から 1 ホップで到達できるようにする。
- 新しい境界: モデルとコードの乖離が散文でなくゲートで検出される。
- テスト: `src/test/py/test_verify_formal_map.py`（新規）。
- Verify: `py -3 tools/verify-formal-map.py --strict`
- Expect: exit 0。`models.json` に 6 モデル（`ControlStartupHandshake`、`ProjectSwitchState`、`SearchRequestLifecycle`、`SenpContributionOwner`、`SenpGhConnection`、`SenpGhRequests`）と、それぞれ 1 件以上のコードシンボルが登録されている。
- リスク: 低。
- ロールバック: ステップ削除。
- 規模: 小

### S39 `ExtensionHostLease` の孤立 `.cfg` を解消し、全モデルを CI で回す

- 対象: `docs/formal/ExtensionHostLease_Current.cfg`、`docs/formal/ExtensionHostLease_NoPin.cfg`、`docs/formal/ExtensionHostLease_NoRecheck.cfg`、`docs/formal/ExtensionHostLease.tla`（新規）、`.github/workflows/architecture-gates.yml`
- 内容: `.tla` が無い `ExtensionHostLease_*.cfg` 3 本は、`.tla` が `12eee6960`（#169「VS Code extension compatibility の削除」）で意図的に削除されたものなので `.cfg` を削除する（復元しない）。`tools/verify-search-lifecycle.py` は `tools/verify-senp-github-models.py` の下位互換なので後者へ統合し、`tools/verify-formal-models.py`（新規）1 本にする（`--model` で分割実行）。`ControlStartupHandshake` と `ProjectSwitchState` と `SenpContributionOwner` は `.tla` があるのに CI で TLC を回していないので、`tools/verify-search-lifecycle.py` と同じ形式の runner を追加して `architecture-gates.yml` に載せる（TLC は既存の pinned ダウンロードを使う）。
- 新しい境界: `docs/formal/` にあるモデルはすべて CI で検証される。
- テスト: 各 mutant `.cfg` が fail し、`_Current` が pass することを runner が確認。
- Verify: `ls docs/formal/*.tla | wc -l`
- Expect: 6。`ls docs/formal/ExtensionHostLease_*.cfg` が 0 件。`ProjectSwitchState` に mutant `.cfg` が 2 本以上追加されている。`rg -c "Verify .* models|Verify .* lifecycle|Verify .* handshake" .github/workflows/architecture-gates.yml` が 4 以上（現在 2）。
- リスク: 低。TLC の実行時間が増える（現行 2 モデルで数十秒）。
- ロールバック: ステップ削除。
- 規模: 小

### S40a 既存 4 モデルの状態を型で写す（遷移関数 1 本）

- 対象: `sakura_core/workbench/search/`（`SearchRequestLifecycle` の実装）、`sakura_core/senp/github/`（`SenpGhConnection` / `SenpGhRequests`）、`sakura_core/platform/controlipc/`（`ControlStartupHandshake`）、`docs/formal/models.json`（S38 で新規）
- 内容: 4 モデルそれぞれについて、状態集合と遷移を `enum class` + 遷移関数 1 本（`Transition(State, Event) -> State`）に写し、`models.json` の `transition_symbol` からそのシンボルを指す。遷移関数の単体テストは `.cfg` の不変条件（`NoGenerationCheck` などの mutant が落ちる性質）を 1 対 1 で持つ。モデル 1 つ = 1 コミット。
- 新しい境界: 形式検証境界がコードの型と一致する。
- テスト: `src/test/cpp/tests1/workbench/search/`（新規）（S12 で作成）、`src/test/cpp/tests1/senp/`、`src/test/cpp/tests1/platform/controlipc/` に遷移関数テスト。
- Verify: `py -3 tools/verify-formal-map.py --strict`
- Expect: exit 0。`models.json` の 4 モデルすべてに `transition_symbol` が登録され `rg` で実在する。`rg -c "Transition\(" sakura_core/workbench/search sakura_core/senp/github sakura_core/platform/controlipc` が各ディレクトリで 1 以上。`tests1.exe --gtest_filter=*Transition*` が mutant ごとのケースを含み全 pass。
- リスク: 中。既存の状態表現（bool の組）を enum に置き換える際に到達不能状態が露出し挙動が変わる可能性がある（そのときは TLA+ モデル側の修正が先）。
- ロールバック: 遷移関数を残したまま呼び出しを戻す。
- 規模: 中

### S40b 契約の置き場所 3 分類規則と、最小コストの新規モデル `ViewContainerPageTransaction`

- 対象: `sakura_core/CLAUDE.md`（規則の 1 表）、`docs/formal/ViewContainerPageTransaction.tla`（新規）、`docs/formal/ViewContainerPageTransaction_Current.cfg`（新規）+ mutant `.cfg` 2 本（新規）、`sakura_core/workbench/viewcontainer/`、`docs/formal/models.json`（S38 で新規）
- 内容: 契約の置き場所を 3 分類で決める規則を `sakura_core/CLAUDE.md` に 1 表で置く: **分類 A** = 型（戻り値・寿命で観測でき時間順序が不要）、**分類 B** = TLA+（独立主体 2 つ以上 × 遅延完了 / ロールバック / プロセス死 / 再入。決め手は「破る到達順序を単体テストで全列挙できるか。できなければ B」）、**分類 C** = 散文（根拠がリポジトリ外、意図の記録、見た目。必ず「なぜ型でもモデルでもないか」を 1 行添える）。規則の適用例として、遷移段階が既に enum で存在し最小コストの `ViewContainerPageTransaction` をモデル化し（mutant 2 本以上必須）、S39 の runner に載せ、S40a と同じ遷移関数化を行う。
- 新しい境界: 「これは型か、モデルか、散文か」がエージェントの判断でなく規則で決まる。
- テスト: mutant `.cfg` 2 本が fail し `_Current` が pass することを runner が確認。
- Verify: `ls docs/formal/ViewContainerPageTransaction*.cfg | wc -l`
- Expect: 3 以上。`ls docs/formal/*.tla | wc -l` が 7（現在 6）。`rg -c "分類 A|分類 B|分類 C" sakura_core/CLAUDE.md` が 3 以上。`py -3 tools/verify-formal-models.py --model ViewContainerPageTransaction` が `_Current` pass / mutant fail を報告する。
- リスク: 低。
- ロールバック: モデルと規則の削除。
- 規模: 小

### S40c 残る候補モデル 4 本（`EditorWorkingCopyLifecycle` → `TerminalSessionRetirement` → `WorkbenchLayoutTransaction` → `UpdateInstallRelaunch`）

- 対象: `docs/formal/`（新規 `.tla` 4 本 + `.cfg` 各 3 本以上）、`sakura_core/workbench/editor/`、`sakura_core/terminal/`、`sakura_core/workbench/layout/`、`sakura_core/update/`、`docs/formal/models.json`（S38 で新規）
- 内容: 費用対効果順に 1 モデル = 1 コミットで追加する: `EditorWorkingCopyLifecycle`（最も厚い散文契約、48 コミット）→ `TerminalSessionRetirement`（#250/#266/#276/#290、`CompletionExactlyOnce`）→ `WorkbenchLayoutTransaction`（replay 窓 256 が定数で存在）→ `UpdateInstallRelaunch`（`docs/audit-safety/ledger.md` の F16 が要求）。各モデルは S40b の規則で「分類 B」と判定した根拠 1 行を `models.json` に持ち、mutant 2 本以上と S40a 同様の遷移関数を伴う。
- 新しい境界: 散文で書かれていた 4 つの生存期間契約がモデルと型で表現される。
- テスト: 各 mutant が fail、`_Current` が pass。遷移関数テスト。
- Verify: `ls docs/formal/*.tla | wc -l`
- Expect: 11（S40b 完了時 7 から +4）。`py -3 -c "import json;m=json.load(open('docs/formal/models.json'));print(sum(1 for x in m['models'] if x.get('classification')=='B' and x.get('transition_symbol')))"` が 9 以上（既存 4 + S40b 1 + 本ステップ 4）。
- リスク: 中。`EditorWorkingCopyLifecycle` は `sakura_core/workbench/editor/CLAUDE.md`（26,838 バイト）の散文契約を先に読み切る必要がある。
- ロールバック: モデル単位で削除。
- 規模: 大

### S40d semantic inventory のディレクトリ別 finding 予算

- 対象: `tools/build/baselines/editor-core-semantic.json`、`tools/build/sakura_build_lib/semantic_inventory.py`、`tools/build/tests/`（予算ゲートの pytest、新規）
- 内容: ratchet を「全体純減」から「ディレクトリ別上限」へ切り替える。初期予算: `boundary.win32_type` を `sakura_core/workbench/win32/`・`sakura_core/window/`・`sakura_core/terminal/window/` 以外で 0、`global.get_edit_wnd` を `sakura_core/include/` 配下で 0、その他のディレクトリは現在値を上限に固定。Phase 3・4 の完了後に着手する（それ以前は上限を満たせない）。
- 新しい境界: 境界の外側で Win32 型が増えない。
- テスト: 予算外ディレクトリに finding を 1 件足した合成入力で exit 11 になる pytest。
- Verify: `py -3 tools/build/sakura_build.py inventory semantic --strict`
- Expect: exit 0。`py -3 -c "import json;print('directory_budgets' in json.load(open('tools/build/baselines/editor-core-semantic.json')))"` が `True`。`rg -l "\bHWND\b" sakura_core/workbench --glob "!win32/*"` が 0 件（P3 の到達）。
- リスク: 低。
- ロールバック: 予算キーを削除すれば従来の全体 ratchet に戻る。
- 規模: 中

---

## 6. 指標と継続計測

### 6.1 一次指標 IRV（静的推定。check.py C11 が再計測して照合する）

定義は 1.1.1 節。計測は `tools/architecture/irv.py`（`estimate --issue N --json` / `table --issues ...` / `explain --issue N`）。Issue と commit の対応は `tools/architecture/issue_commits.py` が解決する（6.3 節）。集合の各ファイルは 7 区分のうちちょうど 1 つの理由を持ち、複数に該当する場合は `guide > manifest > formal > contract > test > owning_source > dependency_owner` の優先順で 1 つに決める。

`py -3 tools/architecture/irv.py table --issues 296,274,267,253,277,228,227,226,281,285,278,292,276,250,256,266,217,291` の出力（`19eb17c58`。列の意味: `guide_files` / `guide_lines` = 区分 `guide`、`source_files` / `source_lines` = 区分 `owning_source`、`test_files` / `contract_files` / `formal_files` / `manifest_files` = 各区分のファイル数、`components` = 変更対象を所有する component 数、`budget_ok` = 1.1.2 節の予算をすべて満たすか、`contamination` = 6.3 節の汚染注記）。

| Issue | commits | changed_files | guide_files | guide_lines | source_files | source_lines | test_files | contract_files | formal_files | manifest_files | components | budget_ok | contamination |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| #296 | 139 | 321 | 47 | 8796 | 418 | 223529 | 168 | 503 | 16 | 47 | 4 | no | multi:1,unrelated:72 |
| #274 | 94 | 121 | 27 | 6427 | 117 | 59992 | 106 | 120 | 5 | 46 | 1 | no | multi:5,unrelated:25,upstream:1 |
| #267 | 9 | 323 | 42 | 8809 | 348 | 192494 | 158 | 364 | 0 | 47 | 3 | no | multi:8,unrelated:7,upstream:1 |
| #253 | 6 | 194 | 39 | 7206 | 361 | 192334 | 105 | 393 | 0 | 42 | 2 | no | unrelated:1 |
| #277 | 7 | 205 | 41 | 7502 | 304 | 173530 | 131 | 361 | 0 | 47 | 6 | no | multi:2,unrelated:3,upstream:1 |
| #228 | 6 | 145 | 33 | 6331 | 372 | 182924 | 114 | 381 | 0 | 44 | 2 | no | multi:3,upstream:1 |
| #227 | 5 | 128 | 34 | 6373 | 358 | 174487 | 94 | 355 | 0 | 42 | 2 | no | multi:3 |
| #226 | 2 | 77 | 34 | 5403 | 330 | 160332 | 76 | 324 | 0 | 42 | 2 | no | multi:1,unrelated:1,upstream:1 |
| #281 | 3 | 83 | 42 | 7767 | 340 | 190906 | 110 | 419 | 0 | 47 | 3 | no | multi:1,unrelated:1,upstream:2 |
| #285 | 4 | 34 | 40 | 7719 | 275 | 169735 | 114 | 371 | 7 | 7 | 2 | no | multi:2,unrelated:2,upstream:2 |
| #278 | 7 | 33 | 36 | 7007 | 241 | 149846 | 92 | 315 | 0 | 47 | 2 | no | unrelated:1,upstream:1 |
| #292 | 4 | 11 | 37 | 7237 | 234 | 148367 | 93 | 302 | 0 | 6 | 2 | no | multi:1,unrelated:2,upstream:1 |
| #276 | 6 | 14 | 13 | 2068 | 29 | 18318 | 17 | 44 | 0 | 5 | 1 | no | unrelated:4,upstream:1 |
| #250 | 3 | 18 | 32 | 6204 | 245 | 145153 | 29 | 316 | 0 | 6 | 2 | no | unrelated:1,upstream:1 |
| #256 | 1 | 6 | 35 | 6592 | 264 | 152732 | 79 | 348 | 0 | 6 | 2 | no | unrelated:1,upstream:1 |
| #266 | 1 | 4 | 8 | 1563 | 27 | 13630 | 9 | 26 | 0 | 5 | 1 | no | upstream:1 |
| #217 | 1 | 4 | 9 | 1657 | 10 | 5125 | 8 | 13 | 0 | 6 | 1 | no | multi:1,upstream:1 |
| #291 | 22 | 275 | 28 | 4719 | 107 | 74072 | 96 | 127 | 11 | 5 | 1 | no | multi:3,unrelated:3,upstream:1 |

読み取れること:

- **18 件すべてが予算外**（`budget_ok` = `no`）。最小の Issue（#217、変更 4 ファイル）でもコードを読む前に `guide` 9 ファイル / 1,657 行を読まされる。予算（3 ファイル / 450 行）の約 4 倍で、これが S35 の一次根拠である。
- `guide` は変更ファイルの祖先だけでなく、IRV 集合の全ファイルの祖先 `CLAUDE.md` を数える（較正で判明した実態。6.2 節）。したがって `contract` / `test` が `workbench/` や `terminal/` に広がるほど `guide` も増える。**ガイドの削減は、ガイドを短くするだけでなく集合の広がりを止めることでも達成される。**
- `source_lines` は #292 / #250 / #256（変更 6〜18 ファイル）で 145,000〜153,000 行に達する。原因は `contract` の 2 ホップ include 閉包が `workbench/` のヘッダ経由で `CEditWnd.h` などモノリスのヘッダに到達し、その対になる `.cpp` を `owning_source` に引き込むことにある（`py -3 tools/architecture/irv.py explain --issue 256` で確認できる）。S20 / S21 で契約ヘッダを `sakura_core/include/sakura/` に出し、閉包がモノリスに入らないようにするのが本質的な対策で、`CEditWnd.cpp` の行数削減（B-01）はその補助である。
- `manifest` が 42〜47 件になる Issue はビルド定義を変更した Issue（`.vcxproj` / `.filters` / CMake / `sakura_build_lib/*.py` を読む必要が生じる）。S05〜S07 でビルド定義変更を 0 にすると、この列は `modules.json` + `test-inventory.json` の 5〜6 件に落ちる。
- `components` が 1 の Issue（#274 / #276 / #266 / #217 / #291）は所有 component が `sakura_app` だけ、つまりモノリス内で完結した変更である。component 化（4.4 節）が進むと `owning_source` は「同じディレクトリの `.cpp`/`.h`」から「component の `sources`」に切り替わり、集合が根拠付きで閉じる。

### 6.2 IRV の較正（静的推定と実セッションの読取記録の比較）

静的推定が「実際にエージェントが読んだ集合」をどれだけ捉えているかを、`~/.claude/projects/` 配下の Claude Code セッション記録（JSONL）から計測する。`tools/architecture/irv_calibrate.py`（`sessions --issue N` で対象セッションの一覧、`compare --issue N` で 1 件の詳細、`compare --table --issue N ...` で複数件の表）。

- 実読取集合: セッション記録の `Read` / `Edit` / `Write` の `file_path` と、`Bash` コマンド文字列中のパストークン（このリポジトリのエージェントは `sed -n` / `cat` で読むことが多く、#228 では `Bash` 739 回に対し `Read` は 7 回）。`Grep` / `Glob` の結果パスは既定で数えない（`--include-grep-results` で加算）。
- Issue への帰属: ユーザー発話が `#N` を名指ししたターンから次のユーザー発話までを「窓」とし、窓の中の読取だけを Issue N に帰属させる（issue-windowed）。1 セッションが複数 Issue を扱う場合に読取集合が全 Issue へ二重帰属するのを防ぐ。窓を 1 つも持たないセッションは除外する。
- `recall` = |静的 ∩ 実読取| / |実読取|（静的推定が実読取をどれだけ含むか）、`precision` = |静的 ∩ 実読取| / |静的|。IRV は「読む必要がある集合の上界」なので precision は低くてよく、**recall を主に見る**。

`py -3 tools/architecture/irv_calibrate.py compare --table --issue 217 --issue 274 --issue 228 --issue 266` の出力（2026-09-13 時点のセッション記録 28 本。`tool_calls` は R = Read / B = Bash / G = Grep の回数、`top_missing_dir` は実読取にあって静的集合に無いファイルが最も多いディレクトリ）:

| Issue | sessions | window_turns | tool_calls | static_files | actual_read_files | intersection | recall | precision | missing_total | top_missing_dir |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|
| #217 | 1684e3a6,9f9d803c | 4 | R25/B206/G15 | 47 | 43 | 13 | 0.302 | 0.277 | 30 | sakura_core/workbench/scm/ (4) |
| #274 | b0b9917d | 1 | R0/B105/G0 | 425 | 20 | 10 | 0.500 | 0.024 | 10 | sakura_core/senp/github/ (4) |
| #228 | 5a7da80d | 10 | R7/B739/G0 | 958 | 77 | 64 | 0.831 | 0.067 | 13 | tools/ (3) |
| #266 | ee824fce | 2 | R2/B81/G0 | 75 | 10 | 6 | 0.600 | 0.080 | 4 | ./ (2) |

代表 4 件の選定理由: 18 件のうち #250 / #292 / #253 / #277 / #278 / #276 / #256 の 7 件はユーザー発話が Issue 番号を名指ししたセッションが存在せず帰属できない。帰属できる Issue から小（#217 / #266）・中（#274）・大（#228）を選んだ。

較正で判明して **計測規則を直した** 点（直す前 → 後の recall: #217 0.163 → 0.302、#274 0.15 → 0.50、#228 0.714 → 0.831）:

1. `guide` は変更ファイルの祖先だけでは足りない。#217 のセッションは `sakura_core/workbench/CLAUDE.md` と各 view の `CLAUDE.md` を読んでいた。→ IRV 集合の全ファイルの祖先 `CLAUDE.md` を数える。
2. テストは名前の類似だけでは見つからない。#217 は `src/test/cpp/tests1/window/*`、#292 は `tests1/workbench/*` を読んでいた。→ 変更ファイルの末端ディレクトリ名を鏡写しにした `tests1/<leaf>/` 直下を `test` に含める。
3. 直接 include（1 ホップ）では #228 の recall が 0.805 に落ちる。→ 前方 include 閉包を 2 ホップにし、解決できたヘッダを `contract`、その対の `.cpp` を `owning_source` に入れる。
4. ビルド定義を変更した Issue（#274）は `tools/build/sakura_build.py` / `sakura_build_lib/*.py` / `src/main/cmake/*.cmake` を読んでいた。→ ビルド定義が変更対象に含まれるときだけ `manifest` に加える。

残っている限界（U20〜U22 として 7.3 節に載せる）:

- #217（0.302）と #274（0.500）は recall 0.6 の目安に届かない。不足分は `workbench/scm/` / `explorer/` / `activity/` や `.rc` への横断的な探索読みで、所有関係にも include グラフにも現れない。**この「所有にも契約にも現れないのに読まされる」集合そのものが構造の問題**であり、S21 / S31 で `workbench/` の各 view を component 化すると、探索読みの必要が減るか（recall が上がるか）を再較正で確認する。
- 静的集合は Issue の最終コミット時点、実読取パスは HEAD 基準で正規化しているため、後から追加された include（#217 の `CCustomFrameController.cpp` → `workbench/WorkbenchLayout.h`）は捕捉できない。
- `.claude/memory/rules.md` は意図的に未追跡なので git ベースの推定に入らないが、リポジトリの規則では作業開始時に読む。
- 帰属できるセッションは 18 件中 11 件で、較正は 4 Issue・5 セッションに依存する。S03 で Issue クローズ時に較正を自動実行し、四半期ごとに本節の表を更新する。

### 6.3 事後指標 Change Footprint（変更がどこまで広がったか。check.py C7 が再計測して照合する）

定義（`tools/architecture/issue_footprint.py measure --issue N --json`）: 追跡 Issue N に紐づくコミットを対象に、最後のコミット時点で次を測る。IRV との役割分担は 1.1.3 節。

| 項目 | 定義 |
|---|---|
| `commits` | Issue に紐づくコミット数 |
| `changed_files` | 変更した相異なるパス数 |
| `changed_subdirs` | 変更した所有ディレクトリ数（2 階層。`workbench` / `terminal` / `platform` / `senp` 配下は 3 階層） |
| `build_definition_files` | 変更した `.vcxproj` / `.filters` / CMake / `modules.json` の数 |
| `read_footprint_lines` | 変更した `.cpp/.h/.hpp/.inl/.rs/.py/.md/.tla/.cfg/.ps1` の最終コミット時点の行数合計（生成物と `externals/` 除外）。**「読む量」ではなく「変更したファイルの全長」**。S03 で `changed_file_lines` に改名する |
| `claude_md_bytes` | 変更ファイルの祖先パス上の全 `CLAUDE.md` バイト合計（ルート含む） |
| `test_files` | `src/test/` または `tools/build/pilots/` 配下の変更ファイル数 |
| `contamination` | 6.3.1 の汚染注記 |
| `association` | 6.3.1 の対応付けの出所 |

#### 6.3.1 Issue と commit の対応付け（`tools/architecture/issue_commits.py`）

`git log --grep '#N'` だけでは対応付けにならない。この checkout の履歴は 2012 年の上流 sakura-editor から始まり、`#291` は上流の 2012 年のパッチ番号にも一致し、fork のコミットはしばしば複数 Issue を同時参照する。対応付けは次の優先順で決め、commit ごとに出所を記録する。

| 優先 | 出所 | 内容 |
|---|---|---|
| 1 | `manual` | `tools/architecture/issue_commits.json`（`{"289": {"include": [...], "exclude": [...]}}`）。対応付けが証明可能に誤っているときだけ書く。現在は空 |
| 2 | `pr` | `gh` で取得した Issue のタイムライン（リンクされた PR の `committed` イベント、`referenced` / `merged` の commit、`gh pr list --search N` で見つかる merged PR のマージコミット）。結果は `tools/architecture/.cache/` にキャッシュ |
| 3 | `grep` | `git log --grep '(^\|[^0-9])#N([^0-9]\|$)'` を **fork 起点以降**に限定 |

fork 起点は「`tsuyoshi-otake` が author の最初のコミット」`2581d2972`（2026-07-29）で、実行時に計算する。「2026-01-01 以降の最初のコミット」は上流 author のコミットが 2026 年にも続くため起点にならない（旧 S03 案の誤りを訂正）。`association` 列の値: `pr` = タイムラインだけで決まった（14 件）、`pr+grep` = タイムラインに grep が追加した（#228 / #227）、`pr_empty+grep` = タイムラインが空で grep だけ（#226 / #217）、`grep_only` = `gh` が使えなかった（0 件）。

汚染注記（`contamination` 列。除外せずに注記する）:

| 種別 | 意味 | 対処 |
|---|---|---|
| `multi:N` | メッセージが 2 つ以上の相異なる `#N` を参照するコミットが N 件 | S03 の `--attribution exclusive` で分離 |
| `unrelated:N` | コミットの変更パスの過半が、その Issue の多数派 component 以外に所有されるコミットが N 件 | 追跡 Issue（#296 の 72 件、#274 の 25 件）を母集団から除く運用（U18） |
| `upstream:N` | grep が fork 起点より古いコミットに一致した件数。**集合からは除外済み**で、件数だけ残す | なし（18 件中 15 件に 1 件以上ある。naive な grep ならこれを取り込んでいた） |

#### 6.3.2 ベースライン（直近 Issue 18 件）

`py -3 tools/architecture/issue_footprint.py table --issues 296,274,267,253,277,228,227,226,281,285,278,292,276,250,256,266,217,291` の出力（`19eb17c58`）。

| Issue | コミット数 | 変更ファイル | 変更サブディレクトリ | ビルド定義ファイル | 読解行数 | CLAUDE.md バイト | テストファイル | contamination | association |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| #296 | 139 | 321 | 28 | 4 | 112,804 | 407,477 | 55 | multi:1,unrelated:72 | pr |
| #274 | 94 | 121 | 15 | 14 | 85,086 | 275,284 | 10 | multi:5,unrelated:25,upstream:1 | pr |
| #267 | 9 | 323 | 29 | 15 | 66,687 | 343,527 | 11 | multi:8,unrelated:7,upstream:1 | pr |
| #253 | 6 | 194 | 33 | 4 | 52,848 | 229,778 | 18 | unrelated:1 | pr |
| #277 | 7 | 205 | 21 | 34 | 48,659 | 178,045 | 23 | multi:2,unrelated:3,upstream:1 | pr |
| #228 | 6 | 145 | 26 | 12 | 85,475 | 325,412 | 27 | multi:3,upstream:1 | pr+grep |
| #227 | 5 | 128 | 31 | 4 | 78,937 | 340,459 | 22 | multi:3 | pr+grep |
| #226 | 2 | 77 | 26 | 4 | 53,865 | 260,696 | 8 | multi:1,unrelated:1,upstream:1 | pr_empty+grep |
| #281 | 3 | 83 | 21 | 4 | 47,299 | 183,422 | 15 | multi:1,unrelated:1,upstream:2 | pr |
| #285 | 4 | 34 | 12 | 0 | 40,158 | 182,172 | 6 | multi:2,unrelated:2,upstream:2 | pr |
| #278 | 7 | 33 | 8 | 4 | 31,128 | 125,100 | 5 | unrelated:1,upstream:1 | pr |
| #292 | 4 | 11 | 5 | 0 | 19,553 | 128,189 | 4 | multi:1,unrelated:2,upstream:1 | pr |
| #276 | 6 | 14 | 8 | 0 | 13,061 | 89,384 | 2 | unrelated:4,upstream:1 | pr |
| #250 | 3 | 18 | 6 | 0 | 27,013 | 105,786 | 4 | unrelated:1,upstream:1 | pr |
| #256 | 1 | 6 | 3 | 0 | 27,970 | 221,841 | 2 | unrelated:1,upstream:1 | pr |
| #266 | 1 | 4 | 3 | 0 | 3,215 | 59,199 | 1 | upstream:1 | pr |
| #217 | 1 | 4 | 2 | 0 | 2,718 | 77,074 | 1 | multi:1,upstream:1 | pr_empty+grep |
| #291 | 22 | 275 | 11 | 0 | 13,515 | 146,751 | 6 | multi:3,unrelated:3,upstream:1 | pr |

対応付けを PR ベースに変えて動いた値（旧 grep ベース、`206a57218` 計測との差）: #291 は grep で 4 コミット / 4 ファイルだったが、タイムラインでは PR 本体の 22 コミット / 275 ファイル（`multi:3,unrelated:3` を含む）。#274 は 95 → 94、#267 は 10 → 9、#285 は 42 → 34 ファイル、#292 は 16 → 11 ファイル（いずれも上流一致コミットの除外）。#226 の 77 ファイルは `multi:1` のとおり #227 と同時参照する 1 コミット由来で、#226 単独の変更は 1 ファイル。

計測ツールの既知の誤差と現状（S03 で修正する）:

- D1 上流履歴の混入: **解消**。fork 起点以降に限定し、除外件数を `upstream:N` として残す。
- D2 多重参照の二重計上: **注記のみ**（`multi:N`）。#226 / #227 / #228 / #217 が該当。分離（`--attribution exclusive|shared`）は S03。
- D3 帰属漏れ: **未解消**。差分を持つ 816 コミット中 203（24.9%）が Issue 番号を持たない（2026-09-12 計測）。PR タイムラインで一部は回収されるが、PR を経由しない直接コミットは残る。S03 の `#N` 参照 lint で新規分を 0 にする。
- D5/D6: **未解消**。`--no-renames` のためリネームが 2 倍に数えられる（#267 の 323 は A / D / M の内訳で実質 210 前後）。#291 の 275 ファイルの多くは `docs/audit-safety/` の証跡。`changed_files` は locality 指標として単独では使えない。
- D7: **役割を変更**。`read_footprint_lines` は「変更したファイルの全長」であり読む量ではない。読む量は IRV（6.1 節）が担い、本指標は `changed_file_lines` に改名して変更の広がりだけを表す。

読み取れること:

- 変更ファイル 4〜6 の小さな Issue（#266 / #217 / #256）でも `CLAUDE.md` バイトは 59,000〜222,000。#256 は 6 ファイルの変更で 221,841 バイトの `CLAUDE.md` を読まされる（`workbench/scm/` 配下のため）。**doc コストが Issue の規模と無関係な下限を持つ**（S35 の根拠。IRV では `guide_lines` として現れる）。
- ビルド定義ファイルの変更は 18 件中 10 件で発生し、最大 34 ファイル（#277）。**S05〜S07 でこの列を 0 にする。**
- 変更サブディレクトリが 20 を超える Issue が 8 件。目標は中央値 5 以下。

### 6.4 二次指標

| 指標 | コマンド | 現在値 | Phase 3 完了時目標 | 最終目標 |
|---|---|---:|---:|---:|
| 上向き include（B-15） | `py -3 tools/architecture/include_layers.py report --json` | 402 | 120 | 60 |
| `CEditWnd.cpp` 行数（B-01） | `wc -l sakura_core/window/CEditWnd.cpp` | 16,806 | 5,500 | 5,500 |
| `GetEditWnd` 直接呼び出し（B-12） | semantic inventory | 258 | 60 | 40 |
| `GetDllShareData` 直接呼び出し（B-11） | semantic inventory | 705 | 450 | 200 |
| `CLAUDE.md` 合計バイト（B-08） | `git ls-files -- CLAUDE.md "*/CLAUDE.md" \| xargs wc -c` | 658,157 | 658,157（S35 は Phase 5） | 350,000 |
| component 数（B-09） | `modules.json` | 40 | 46 | 52 |
| `.vcxproj` 明示エントリ（B-03 + B-04） | `rg -c "<ClCompile Include="` | 968 | 6 | 6 |
| `CEditWnd.cpp` の直近 3 か月変更コミット数 | `git log --oneline --since=3.months -- sakura_core/window/CEditWnd.cpp \| wc -l` | 86 | 30 | 15 |
| workbench 変更に占める `CEditWnd.cpp` 同時変更率 | `git log --name-only -- sakura_core/workbench` から集計 | 33% | 10% | 5% |
| PR の `Native` job 中央値 | GitHub Actions | 13〜21 分 | 5 分未満（component 経路） | 同左 |

### 6.5 目標値と計測頻度

一次指標（IRV）の目標。母集団は「変更ファイル 20 以下の Issue」（6.1 節の表では #292 / #276 / #250 / #256 / #266 / #217 の 6 件。典型的なバグ修正の規模）とし、各列の中央値で判定する。現在値は 6.1 節の表から計算した中央値（6 件の 3 番目と 4 番目の平均）。

| 時点 | `guide_files` | `guide_lines` | `source_lines` | `test_files` | `manifest_files` | `budget_ok` の件数 |
|---|---:|---:|---:|---:|---:|---:|
| 現在（6 件） | 22.5 | 4,136 | 81,735 | 23 | 6 | 0 / 6 |
| Phase 1 完了（S01〜S12） | 22.5 | 4,136 | 81,735 | 23 | **5** | 0 / 6 |
| Phase 3 完了（E3 ゲート） | 12 | 2,500 | **30,000** | 15 | 5 | 2 / 6 |
| Phase 5 完了（S35） | **3** | **450** | 30,000 | 15 | 5 | 2 / 6 |
| 最終 | 3 | 450 | **2,500** | **10** | 5 | **6 / 6** |

`source_lines` の削減は S20 / S21（契約ヘッダの `include/sakura/` 化で 2 ホップ閉包がモノリスに入らない）と S23〜S28（`CEditWnd` からの抽出）で、`guide` は S35 で、`test_files` は S21 / S31 の view 単位テストで達成する。目標に到達しない Phase は完了にしない（Phase 3 は E3-3、Phase 5 は S35 の Expect がこの表を参照する）。

事後指標（Change Footprint）の目標（直近 10 Issue の中央値）:

| 時点 | `changed_file_lines` | `claude_md_bytes` | `build_definition_files` | `changed_subdirs` |
|---|---:|---:|---:|---:|
| 現在（6.3 節の 18 件） | 43,000 前後 | 180,000 前後 | 4 | 13 |
| Phase 1 完了 | 43,000 | 180,000 | **0** | 13 |
| Phase 3 完了 | 25,000 | 180,000 | 0 | 8 |
| Phase 5 完了 | 25,000 | **60,000** | 0 | 5 |
| 最終 | **15,000** | 40,000 | 0 | 4 |

計測: Issue クローズ時に S03 のワークフローが IRV（`irv.py estimate`）と Change Footprint（`issue_footprint.py measure`）を自動計測して artifact 化し、帰属できるセッション記録があれば較正（`irv_calibrate.py compare`）も走らせる。四半期ごとに 6.1 / 6.2 / 6.3 節の表を本節の中央値とともに更新する。ステップの Verify で使う二次指標は各 PR で計測する。

---

## 7. リスク・ロールバック・未確定事項

### 7.1 主要リスク

| # | リスク | 該当ステップ | 兆候 | 緩和策 |
|---|---|---|---|---|
| R1 | メッセージ順序の変化（`WM_SIZE` → `WM_PAINT`）で起動時に 1 フレーム空白 | S30 | #226 と同型の stale pixel | 1 case ずつ移し、毎回 `stale-pixel-verification` |
| R2 | フォーカスの喪失（`HandleEditorFrameEvent` の `FocusGained` が `m_nTimerCount` と `m_emptyEditorSurface->Focus()` を分岐。`m_emptyEditorSurface` は 8 クラスタが触る） | S24, S28 | 起動直後にキャレットが出ない | `m_emptyEditorSurface` の所有とフォーカス分岐は `CEditWnd` に残す |
| R3 | 破棄順序の非対称（`CloseWorkbench` 109 行と `InitializeWorkbench` 1,275 行） | S25, S33 | 終了時リーク / 二重解放（`memory.raw_delete` 18 件） | phase と teardown を同じコミットで書く。`ListenerGate` を先に独立テスト可能にする |
| R4 | 3 パネルの非原子的更新 | S24 | 再描画破綻（`daa3008cc` 以前の症状） | S22 完了を前提にし、原子性を崩すコミットは即 revert |
| R5 | `EditWndTest` 131 件が回帰網として重く flaky | S22〜S30 | テスト時間の増大 | 新規テストは `EditorTestSuite` 非継承を必須にし、131 件は「移動で壊れない」確認だけに使う |
| R6 | VS Code 互換の退行（View の既定配置・キーバインド・空状態） | S31 | View ID や配置の差分 | 移行前後で ID・配置の表を突合し、差があれば実 VS Code を確認 |
| R7 | ワイルドカード化でリンク順が変わり、潜在バグが顕在化 | S05, S07, S12 | `CSakuraEnvironmentTest.ResolvePath001` の失敗 | フル実行を受入条件に。壊れた場合は既に潜在していたバグとして別 Issue |
| R8 | AVX ディスパッチの `/arch` 誤適用（無音の誤コード生成） | S07 | Release ログのコンパイル行 | `Update` メタデータの適用をビルドログで確認 |
| R9 | PCH から `DLLSHAREDATA.h` を外してビルド時間が悪化 | S16a | `build-sln.bat` の所要増 | 変更前後で実測し 5% 超なら PCH 除去だけ保留 |
| R10 | CI の選択実行で欠陥がすり抜ける | S11 | `component_native` 経路の PR が本番で壊れる | S08 のゲートを前提にし、条件を満たさない PR は `full_native` |
| R11 | `sakura.vcxproj` のコンフリクト（3 か月で 112 回変更） | Phase 1 以前の全ステップ | rebase 失敗 | S05〜S07 を最初に終える。それまでのステップは直列化 |
| R12 | semantic ratchet の「触ったファイルで純減」を抽出が一時的に破る | S23〜S29 | `--strict` exit 11 | `--accept-current --reason --tracking-issue` で台帳に記録する運用を各ステップの手順に含める |
| R13 | エンコーディング違反（ASCII-only ファイルに非 ASCII コメント） | 新規ファイルを作る全ステップ | `check-encoding.ps1` 失敗 | 新規 `.cpp`/`.h` は ASCII のみ。説明は `CLAUDE.md` に書く |
| R14 | MSBuild と CMake の片側だけ更新 | ファイル移動を含む全ステップ | MinGW 赤 | `build-gnu.bat MinGW Debug` を通す |

### 7.2 ロールバック方針

- すべてのステップは 1 コミット（またはクラスタ／View／インタフェース単位の連続コミット）で構成し、`git revert` で単独に戻せることを PR の必須条件にする。
- 契約ヘッダ（`sakura_core/include/sakura/`）は revert しても残してよい（実装を持たないため無害）。
- 生成物（`src/main/modules/generated/`）は revert 後に必ず `py -3 tools/build/sakura_build.py generate` を再実行し `generate --check` を通す。
- ベースライン JSON（semantic / include layers / CLAUDE.md budget）は revert と同じコミットで戻す。ratchet の値だけ進んだ状態を残さない。
- Phase 1（S05〜S07）だけは revert 時に `.vcxproj` の明示リストが復活するため、その後に足したファイルを明示リストへ手で追加する必要がある。Phase 1 の revert は最後の手段にする。

### 7.3 未確定事項（実施前に確認する）

| # | 未確定 | 影響するステップ | 確認方法 |
|---|---|---|---|
| U1 | `m_Common` / `m_bDarkMode` の実際の参照経路（`m_pShareData->m_Common` 経由か） | S22 | `sed -n` で参照式を数十件サンプリング |
| U2 | `IWorkbenchRuntime` の 35 メソッドがどのクラスタから呼ばれるかの内訳 | S23, S34 | `rg -o "m_workbenchRuntime->[A-Za-z]+" sakura_core/window/CEditWnd.cpp` を行番号でクラスタに割り当て |
| U3 | 新規 component 12 本を足したときの `graph check --all-contexts` 所要時間と循環の有無 | S25〜S34 | 実際に `modules.json` に足して実行 |
| U4 | `IEditorPaneAccess`（78 参照）が `view/` 14 ファイルの他メンバ参照と同時に成立するか | S29 | `view/` 側の `GetEditWnd()` 参照を全件分類 |
| U5 | `test-window.cpp` 131 テストのうち `EditorTestSuite` を本当に必要とする件数 | S12, R5 | fixture 依存の実測 |
| U6 | `window/CLAUDE.md` 22 節の各節がどのディレクトリへ属すか | S35 | 節ごとの Issue 番号とクラスタの対応付け |
| U7 | クラスタ分類はメソッド名規則に基づくため、名前と実装が乖離したメソッドがあると行数配分がずれる | S24〜S28 | 上位 20 メソッドの本文を読んで分類を検証 |
| U8 | MSBuild ワイルドカードが FileTracker の incremental 判定と `RemoveTests1Exe` 前提を壊さないか | S05 | 無変更 2 回ビルドで no-op を実測 |
| U9 | `12eee6960` で 6 runner が消えた経緯 | S08 | 該当コミットの diff を精読 |
| U10 | `workbench/` 1,508 テストのうち monolith 非依存で移設できる件数 | S10 | `pch.h` 経由の間接依存を個別調査 |
| U11 | `ICommandTarget` に必要なメソッド数（15〜20 は実測 86 メンバからの見積もり） | S17 | public field 80 件の getter 化後に再集計 |
| U12 | `CAutoReloadAgent` / `CAutoSaveAgent` / `CBackupAgent` の生存期間が `CEditDoc` と一致する前提のコード | S18 | `rg` で 3 型の参照元を全件確認 |
| U13 | `CScmWorkbenchTool.cpp` 3,783 行の分割で露出する状態共有 | S32 | 分割前に本文を読む |
| U14 | `ITaskExecutionAccess`（本番 consumer 0）を作るか `TaskExecution` 系を削除するか | S34 | 開発者判断（テストだけが支えている） |
| U15 | `externals/` submodule 未初期化のため Rust output provider（`rust/`）の ABI に S33 のカーネル統合が影響しないか | S33 | 初期化済みチェックアウトで確認 |
| U16 | ~~`ExtensionHostLease.tla` が git 履歴に存在するか~~ → 確認済み: `12eee6960`（#169）で削除。S39 は `.cfg` 削除で確定 | S39 | 解決済み |
| U17 | SCM の VS Code 差分カタログ（約 46 KB）を `CLAUDE.md` から `docs/subsystems/`（新規）へ移せるか（ルート `CLAUDE.md` の「Divergence requires a written reason」規則との整合） | S35 | I-21 着手時に開発者判断 |
| U18 | #296（139 コミット、`unrelated:72`）と #274（94 コミット、`unrelated:25`）が真に 1 Issue か（プログラム規模の追跡 Issue は母集団から除外する運用が要る） | S03, 6.3 節 | `program` ラベルの導入と付与 |
| U19 | `effect_protocol.rs` ↔ `SenpEffectProtocol.h` が同一スキーマの二重保持か（co-change 0.71 からの推測） | 対象外（別 Issue） | 両ソースの突合 |
| U20 | IRV 較正の recall が #217 0.302 / #274 0.500 で目安 0.6 未満（6.2 節）。不足分は所有にも include にも現れない横断的な探索読み。S21 / S31 の view component 化で探索読みが減るか | S03, S21, S31 | Phase 3 完了時に `irv_calibrate.py compare --table` を再実行し、recall が上がらなければ IRV 定義に `sibling_view`（同じ `ViewContainer` の view）区分を追加する |
| U21 | 静的 IRV は Issue 最終コミット時点、実読取は HEAD 基準のため、後から足された include を捕捉できない（6.2 節） | S03 | `irv_calibrate.py` に `--rev <Issue 最終コミット>` でパスを正規化するモードを足す |
| U22 | 帰属できるセッション記録が 18 件中 11 件しかなく、較正が 4 Issue・5 セッションに依存する（6.2 節） | S03 | S03 の月次較正で母集団を増やす。ユーザー発話に `#N` を含める運用規則を `.claude/memory/rules.md` に置く |

### 7.4 本計画の限界

- 調査は読み取り専用で、ビルド・テスト・TLC は実行していない。5 節の Verify コマンドはいずれも実施時に初めて実行される。
- `CEditWnd.cpp`、`CWorkbenchRuntime.cpp`、`CScmWorkbenchTool.cpp` は全文を読んでいない（関数境界・メンバ参照・include は機械解析、責務分類は名前ベース）。
- 各ステップの期待値（行数・件数）は関数境界からの概算で、実際の削減量は移送時に変わる。ratchet で「増えない」ことだけを機械的に保証し、目標値は四半期ごとに見直す。

---

## 8. Issue 分割と依存関係

45 ステップを 26 の Issue に束ねる。1 Issue は 1 セッション〜数セッションで閉じる粒度にし、依存はすべて前方参照のみ（閉路なし。check.py C9 が「全ステップがちょうど 1 つの Issue に属す / 欠落・重複なし / DAG」を検査する）。並行できる Issue は「依存」列が同じもの。

| ID | 題名 | 含むステップ | 依存 | 規模 | 主に効く指標 |
|---|---|---|---|---|---|
| I-01 | semantic ratchet の waiver 台帳（所有者・期限）を導入する | S01 | なし | 小 | Explicit Contracts（ゲート運用） |
| I-02 | 計測基盤（layers / footprint / IRV / CLAUDE.md 予算）を CI に載せる | S02, S03, S04 | I-01 | 小 | One-way Dependency, Small Agent Context |
| I-03 | `tests1.vcxproj` の glob 化 | S05 | I-01 | 中 | Change Locality |
| I-04 | 生成器の `Remove` 対応と `sakura.vcxproj` の glob 化 | S06, S07 | I-03 | 中 | Change Locality |
| I-05 | テスト台帳のマルチ runner 復旧とゲート | S08 | I-02 | 中 | Build/Test Isolation |
| I-06 | `plan impact` と component runner の gtest 化 | S09, S10 | I-05 | 大 | Build/Test Isolation, Independently Testable Modules |
| I-07 | CI の `component_native` 選択実行 | S11 | I-06 | 中 | Build/Test Isolation |
| I-08 | テストのディレクトリ再配置 | S12 | I-03 | 中 | 誤認しにくいディレクトリ構造 |
| I-09 | ヘッダの降格（`CSelectLang` / `global.h` / 誤配置リーフ） | S13, S14, S15 | I-04 | 小 | One-way Dependency |
| I-10 | PCH からの `DLLSHAREDATA.h` 除去 | S16a | I-09 | 中 | Low Coupling |
| I-11 | `CommonSettingsReader` 契約と `view/` の移行 | S16b | I-10 | 大 | Low Coupling, Explicit Contracts |
| I-12 | 残る共有メモリ consumer の契約化（cmd / macro / typeprop / dlg / env） | S16c | I-11 | 大 | Low Coupling |
| I-13 | コマンド層の契約化（`ICommandTarget` / `CommandDescriptor`） | S17, S19 | I-12 | 大 | Low Coupling, Change Locality |
| I-14 | `doc/` からの UI 追い出し | S18 | I-09 | 大 | One-way Dependency |
| I-15 | L1/L2/workbench の残存上向き include と DI 化 | S20, S21 | I-12 | 中 | One-way Dependency |
| I-16 | 表示状態の一元化と `WorkbenchComposition` 抽出 | S22, S23, S24 | I-04 | 大 | Single Responsibility, High Cohesion |
| I-17 | `CEditWnd` クラスタ抽出（WS / MENU / SCM / 機能モジュール） | S25, S26, S27, S28 | I-16 | 大 | High Cohesion, Independently Testable Modules |
| I-18 | `GetEditWnd()` の ISP 化と `DispatchEvent` typed 化（Phase 3 完了ゲート E3 を含む） | S29, S30 | I-13, I-17 | 大 | Low Coupling, Explicit Contracts |
| I-19 | View 登録経路の一本化と `scm/` 標準レイアウト | S31, S32 | I-08, I-17 | 大 | 誤認しにくいディレクトリ構造, Independently Refactorable Modules |
| I-20 | workbench runtime のドメイン返却と役割別 IF | S33, S34 | I-15, I-19 | 大 | Single Responsibility, Explicit Contracts |
| I-21 | `CLAUDE.md` の分割・所有マップ・命名規則 | S35, S36, S37 | I-18, I-20 | 大 | Small Agent Context, 誤認しにくいディレクトリ構造 |
| I-22 | 形式検証の台帳とドリフトゲート、孤立 `.cfg` の解消 | S38, S39 | I-02 | 小 | Formal Verification Boundary |
| I-23 | 既存 4 モデルの遷移関数化 | S40a | I-22 | 中 | Formal Verification Boundary, Explicit Contracts |
| I-24 | 契約 3 分類規則と `ViewContainerPageTransaction` モデル | S40b | I-22, I-19 | 小 | Formal Verification Boundary |
| I-25 | 候補モデル 4 本の追加 | S40c | I-24 | 大 | Formal Verification Boundary |
| I-26 | semantic inventory のディレクトリ別予算 | S40d | I-18, I-20 | 中 | Explicit Contracts |

### 8.1 クリティカルパスと並行性

```
I-01 → I-02 → I-05 → I-06 → I-07
  │      └──→ I-22 → I-23
  │             └──→ I-24 → I-25
  └──→ I-03 → I-04 → I-09 → I-10 → I-11 → I-12 → I-13 ─┐
              │        │                    └──→ I-15 ──┼→ I-20 → I-21
              │        └──→ I-14                        │    └──→ I-26
              │                                         │
              └──→ I-16 → I-17 → I-18 ─────────────────┘
                     │        └──→ I-19 ──→ I-20, I-24
                     └── I-08 ───→ I-19
```

- **最初の 3 Issue（I-01 → I-02 → I-03）は直列**。I-01 は 1 セッション、I-02 は 1〜2 セッション、I-03 は 2〜4 セッション。
- I-04 完了後は **I-09 系（依存方向）と I-16 系（`CEditWnd`）を並行**できる。ただし `sakura.vcxproj` が glob 化されるまで（I-04）は並行させない（R11）。
- I-10 → I-11 → I-12 は同じ契約を段階的に広げるため直列。I-14（`doc/`）は I-09 だけに依存するので I-10 と並行できる。
- I-18 は I-13 と I-17 の両方を待つ。ここが最長経路の合流点になる。Phase 3 完了ゲート E3 は I-18 のクローズ条件。
- I-22〜I-25（形式検証）は I-02 の後なら早く始められる。I-26（ディレクトリ別予算）だけは Phase 3・4 の構造変更が終わるまで上限を満たせないので最後。
- I-21（`CLAUDE.md` 分割）は構造が固まった後にしか意味がないため最後に近い。ただし S04 の予算 ratchet は I-02 で先に入れ、それ以降 `CLAUDE.md` が増えないようにする。

### 8.2 各 Issue の共通テンプレート

Issue 本文には次を必ず書く（S09 の `plan impact` と S36 の所有マップがあれば自動生成できる）。

1. 含むステップ ID と本計画書の節へのリンク。
2. 変更を許可するディレクトリの列挙（それ以外に差分が出たら scope 外として別 Issue）。
3. 最小検証コマンド（`py -3 tools/build/sakura_build.py plan impact --changed ...` の出力）。
4. 受入条件: 該当ステップの `Verify` / `Expect` と、3.2 節の G1〜G7。Phase 3 のクラスタ抽出は K1〜K5 の表を埋める。
5. IRV の前後: 代表変更要求を 1 つ決め、`py -3 tools/architecture/irv.py estimate --commits <前> / <後>` の出力を貼る（減らないステップは「構造整理のみ」と明記）。
6. ロールバック手順（revert 対象コミットの範囲）。
7. 完了時に更新する数値: 2.1 節の B-ID と 6.3 節の指標。

### 8.3 最初の 1 手

```bash
py -3 tools/build/sakura_build.py inventory semantic --strict
```

exit 0 であることを確認する（`19eb17c58` で 2.2 節の 4 finding は解消済み）。新規 finding が出ていれば S01 の判断表に従って finding ごとに修正か waiver かを決める。そのうえで I-01 を起票し、S01 の `tools/build/baselines/semantic-waivers.json` と `tools/build/tests/test_semantic_waivers.py`（いずれも新規）を作る。`--accept-current` は waiver が台帳に載ってからしか実行しない。
