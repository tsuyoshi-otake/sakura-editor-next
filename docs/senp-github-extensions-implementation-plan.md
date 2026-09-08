# Issue #296: 小さなコミットによる工程と形式検証

対象: [#296](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/296)。
仕様: [SENP GitHub拡張設計](senp-github-extensions-design.md)。
設計の形式検証に続き、製品基盤を工程順に実装する。
2026-09-07: D01/F01–F04は完了。正例3件・安全性負例8件・進行性負例1件が合格。
[検証結果と対応表](formal/senp-github-models.md)、[機械可読の証跡](formal/senp-github-evidence.json)。
G01も完了。manifestのversion判別を共通入口へ移し、v1互換とv2の
`UnsupportedRuntime`をpack/verify/installed listingで検証した。
`cargo test -p sakura-senp --locked --offline`は16 tests合格、`cargo fmt --check`合格、runner残存0。
2026-09-08: G02のWIT/event/effect codecも実装。共通fixture 71件（受理26・拒否45）、
WITとの型変換、C++/Rustの双方向JSON交換が合格し、両出力のSHA-256が一致した。
native 14 tests、旧component実行を含むRust host 8 testsが合格。1 MiBと65,536 nodesの
集約上限を送受信へ適用し、JSONC設定互換を保持した。
G03は責務に沿ってG03a（要求状態）とG03b（Win32 process接続）へ分割し、全34工程とする。
G03aのsession実装・11件のnative lifecycle tests・6件のRust dispatch testsが合格。
[session契約](senp-v2-runtime.md)にack、再送、取消と未回収結果の失効規則を保存した。
G03bのnative worker/jobと双方向の期限付きIPCも接続した。実Wasm、読書き停止、
crash、メモリ超過、同時Stop/Joinを含む受入runnerでnative 21件とRust host 15件が合格し、
skip・子process残存は0。
G04のowner transaction・純粋catalog登録・runtime回収契約も実装し、owner/catalog 15件と
実Wasm owner更新を追加した受入runnerでnative 37件、Rust 15件が合格。
join失敗時の再試行ループを負例で再現して修正した。native page/commandの具体的な公開は
U01/U06、broker grantはT02、package受理はそれらの接続後に検証する。
U01の複数View native page、SCM共通22-DIP header、独立したbody保持・collapse・移動を実装。
native focused 71 testsで実window、UIA/MSAA、失効と回収を検証した。
描画runnerは3テーマ・3 DPI、collapse/resize/View移動/container移動の往復を検査し、
実画面、PrintWindow、全面再描画のnoise floorとprocess終了を記録する。
SCM比較は固定repository fixtureで実画面差分0%。子surfaceのPrintWindow差分は
旧headerのA/Bにも存在（旧6.00%、新5.63%、各4試行中3件）する取得側の制約で、
全体frameと実画面の再描画前後は両版とも差分0%だった。
U02のlazy Tree model/providerと実Win32 Tree bodyも実装。追加25件、既存View/SCMを含む96 testsで
page/expand/refresh、scope/limit、取消、反復開閉、native command/keyboard、focus/scroll保持とUIAが合格。
同一binaryによる3回の起動、3テーマ・3 DPIのexpand/resize/scroll/refresh計288試行が合格し、
実画面の再描画差分は全試行0%。PrintWindow単独の欠落42件は同じgeometryのnoise floorで検証した。
3,743件のtest inventory照合、encodingとno-op buildも合格し、runner残存0。
U03のreadonly input登録とnative surface切替も実装。同一scope/resourceを既存Editor groupで再利用し、
legacy文書を保持して切り替える。追加18件とEditor/working-copy回帰を含む86 tests、
同一binaryの2回起動・3テーマ・3 DPIによる216描画試行が合格し、再描画前後の差分は全件0%。
3,762件のtest inventory照合とprocess終了も確認した。U03は実native controlとCEditDoc/undoの接続境界を
検証する工程であり、CEditWndのtab/command/backup接続とsample公開はU06で検証する。
U04の構造化readonly本文も実装。Markdownだけを既存parserへ渡し、metadata/tableはliteral native modelへ変換する。
scope/request/revisionのfence、同一revisionの一致検査、明示failure/expiryと既存workerのlatest-only更新を検証した。
Rust peer fixture読取を含む90 testsと3,780件のruntime inventory照合が合格。同一binaryの2回起動で
3テーマ・3 DPIのvisibility/resize/scroll/refresh計216描画試行が合格し、再描画差分・capture欠落・process残存は0。
U05のchunk本文・native検索/コピーも実装。owner/grant/revisionごとのmemory-only storeは
64 KiB chunk、32 MiB/resource、Control内64 MiBの割当上限を持ち、partial/失効を区別する。
60 focused tests、同一binaryの別名実行26件（32 MiB本文一致を含む）、3,807件のinventoryが合格。
450描画試行の再描画差分は最大0.00309%で、非ゼロは本文外の丸い角だけだった。
schema 2は引き続きUnsupportedRuntimeで、公開・永続化adapterとsample接続はU06の工程。
CI workflowは追加済みで、remote CI実行はpush後の確認事項。
U06のGitHub非依存sample guestは実v2 Componentとして2 View、page/empty/error、構造化詳細とLoading logを返し、
実hostとのHello/Activate/TreeRequest往復まで検証した。native owner publicationと実アプリ結合は引き続きU06に残る。
G02のsemantic台帳はexact source commit `f881170f28b3c17d195c145b6f2396704cffa47a`
から正規手順で受理した。追加35件は既定のconst equality operator 33件と
`static constexpr` 2件の誤検出と確認し、JSONC APIの既存報告13件を保持する理由も
台帳履歴に保存した。exact source SHAが必要な証跡は製品コードに続く小さなcommitとする。

## コミットの進め方

- `main`上で1つの責務・不変条件ごとに検証してコミットする。pushは別の指示があるまで行わない。
- commit messageは英語、変更理由・検証対象を本文に書き、`Refs #296`を付ける。
- 各行は原則1コミット。独立した挙動が混在する場合は行を追加して分割する。
  失敗した検査を残したまま次の製品工程へ進まない。
- 意図的な壊れたモデルは負例fixtureとしてコミットし、正しい設計と混同しない。
  TLCの構文エラーや無関係な不変条件違反を「負例成功」に数えない。
- test・モデル・実装・guidanceの対応を同じコミットで更新する。
  テスト後は起動したrunnerと子processの終了を確認する。
- 既存の未コミット変更を含めず、対象ファイルを列挙してstageする。

## 依存順の工程

`G`は契約、`U`は表示、`T`はtool、`E`は拡張、`R`は配布。
コマンドやsuite名のうち未作成のものは、その工程で実装する受入runnerの名前である。
存在しないsuiteで0件成功を返す検証は不合格。

| ID | 1コミットの内容 | 前提 | Verify | Expect |
|---|---|---|---|---|
| D01 | 設計と本工程表を保存 | #296 | local link/JSON例と`git diff --check` | 壊れた参照0、実装済みと未実装を区別 |
| F01 | 認証candidate/接続解除のTLA+、有界TLC runnerとgate検証 | D01 | `verify-senp-github-models.py --model connection` | 正例全探索・liveness合格、本人照合省略/旧candidate採用の反例 |
| F02 | owner登録・失効・世代付き結果のTLA+ | F01 | 同runner `--model owner` | 原子的登録、失効後の結果拒否、回収のliveness、負例 |
| F03 | 共有要求・購読解除・cooldown・cleanupのTLA+ | F01 | 同runner `--model requests` | single-flight、他subscriber保護、deadline停止処理と受理要求の終端、負例 |
| F04 | 全モデルをCI必須gateへ追加、結果と対応表を保存 | F02/F03 | runner全件、unit test、checkout-invariance | hash-pinned tool、狙った反例、終了済みprocess、evidenceのhash |
| G01 | schema 2/ABI判別と既存v1互換 | F04 | `cargo test -p sakura-senp --locked` | 旧package合格、未知schema/ABI拒否 |
| G02 | WIT event/effectとC++/Rust往復fixture | G01 | `SenpEffectProtocol.*`とRust host tests、相互serializer出力の読取 | casing、sequenceの整数範囲、集約上限、batch内ID再使用、WIT型変換を検査 |
| G03a | bounded dispatch・ack・要求終端のsession実装 | G02 | `SenpEffectProtocol.*:SenpRuntimeLifecycle.*`とRust `effect_session` | sequence順序・ID再送/Conflict、pendingが全て一度終端、切断後の古いevent反映0 |
| G03b | Win32 process・期限付き双方向IPC・実Wasm接続 | G03a | `py -3 tools/verify-senp-runtime.py --offline`と終了後のprocess照合 | 読書き停止・途中frame・crash・メモリ超過でも終端、実v2 Wasm実行、child残存0 |
| G04 | owner単位のcontribution登録・dispose契約とruntime回収 | G03b/F02 | `SenpViewLifecycle.*`と実Wasm owner lifecycle | 部分登録なし、更新失敗は旧owner維持、失効後の反映0、回収失敗の所有権保持 |
| U01 | SCMを参照したcontainer内の複数View nativeページ | G04 | `SenpViewContainer.*:ViewPaneStackLayout.*`、`verify-senp-view-rendering.ps1`、同一条件のSCM比較 | View独立、ヘッダー/余白/action整合、resize/移動/focusに描画残りなし |
| U02 | lazy TreeDataProviderとstable item選択 | U01 | `TreeViewModel.*:SenpTreeProviderTest.*:SenpTreeView.*`、`verify-senp-view-rendering.ps1 -ProbeSet TreeViews`、SCM行密度/選択/scroll比較 | page/expand/refresh、重複・循環・stale拒否、テーマ/DPI/keyboard整合 |
| U03 | readonly Editor input/surface切替 | G04 | `SenpReadonlyWorkbench.*`、`verify-senp-view-rendering.ps1 -ProbeSet ReadonlyEditors` | dirty/undo・選択/scrollを保持して切替、未準備surfaceは拒否、描画残りなし |
| U04 | readonly Markdown/metadata renderer | U03 | `SenpReadonlyDocument.*`、`ReadonlyDocuments` dual capture | 本文表示、script無効、local asset権限漏れ0 |
| U05 | chunk付きtext resourceと検索・コピー | U03 | `SenpTextResource.*:SenpTextResourceViewTest.*`、`TextResources` dual capture | UTF-8境界・上限・partial・失効、native検索/コピー・選択保持 |
| U06 | command/menu/activation・page公開/状態永続化と汎用sample拡張 | U02/U04/U05 | sampleのnative総合試験、profileのcollapse/size復元、SCM比較の状態matrix | GitHub固有コードなしで2 View/本文/ログ表示、loading/empty/error/focus明示 |
| T01 | Git runnerからprocess primitiveのみ抽出 | F04 | `BoundedProcessRunner.*:GitCommandRunner.*` | SCM無回帰、argv・pipe・job cleanup |
| T02 | Control brokerのowner/grant認可 | T01/G03b/G04 | `SenpToolGrants.*` | 別owner/profile/digest/失効handleを拒否 |
| T03 | gh検出・version・read-only argv/env policy | T02 | `GhToolPolicy.*` | shell/任意flag/別repo/env注入0、未導入は明示状態 |
| T04 | gh接続状態・account固定・identity検証 | T03/F01 | `GhConnectionLifecycle.*` | Unknownをsigned-outにしない、account混在0 |
| T05 | 有界web login・cancel・接続解除UI | T04 | fake ghとopt-in認証試験 | 全分岐終端、共有gh logoutなし、code表示/保管先表示 |
| T06 | HTTP envelope・page・ETag・read DTO | T04 | `GhRepositoryRead.*` | 304、403/404、必須field、部分pageを区別 |
| T07 | single-flight・fair queue・cooldown・可視poll | T06/F03 | `GhReadScheduler.*` | 複数windowでも重複1本、最後のunsubscribeでcancel |
| T08 | jobログのredirect・chunk受信・cleanup | T06/U05 | `GhLogResource.*` | credential転送0、上限、途中失敗、URL非保持 |
| E01 | repository snapshotとremote選択 | T06 | `GhRepositorySelection.*` | multi-root/fork/SSH alias/remote削除の区別 |
| E02 | 共通github-clientとIssue一覧 | E01/U06 | 新crate testsとIssue View実表示 | PR除外後のnext page保持、状態filter |
| E03 | Issue本文・コメントの詳細 | E02/U04 | fixtureとopt-in本文表示 | 本文/コメントpage、空と失敗の区別 |
| E04 | PR一覧・本文・base/head/merged状態 | E03 | fixtureとPR View実表示 | Issue番号との混線0、別forkの暗黙取得なし |
| E05 | Actions Workflow/Run/Attemptの一覧 | E01/U06 | 新crate testsとActions View | WorkflowとRunを区別、attempt固定 |
| E06 | Job/Step状態とreadonly概要 | E05 | matrix/unknown/nullのfixture | job名でidentityを代用しない、進行中を成功にしない |
| E07 | ActionsログのEditor接続と可視poll | E06/T07/T08 | opt-in Run→Job→ログ | アプリ内で読める、取得前/partial/expiredを区別 |
| R01 | 独立package install/disable/updateと旧版回帰 | E04/E07 | packageとowner lifecycle tests | 他方の拡張/旧SENPを壊さず停止・更新 |
| R02 | x64 Debug/Release配布と未対応backend境界 | R01 | solution build、変更したCMake、audit/encoding | payload完備、未対応はUnavailable、runner残存0 |
| R03 | 全rubric・native実表示・形式仕様対応の最終確認 | R02 | 設計V1–V13 + 全TLC + process audit | 必須条件が全て合格してから機能完成 |

大きな工程を赤い状態で積み上げるための分割ではない。
例えばG01はv2の未実装能力をUnsupportedで返し、UIボタンをまだ公開しない。
U06は実動するsample、E03/E04/E07は本文やログまで読める縦切りとして検証する。

## 形式モデルの意味と限界

| モデル | 検査する不変条件/進行性 | 負例で外す条件 |
|---|---|---|
| `SenpGhConnection` | 本人照合前の接続公開禁止、接続解除前のcandidate再採用禁止、旧接続維持、開始済み操作の終端 | identity fence / generation fence |
| `SenpContributionOwner` | 原子的contribution登録、失効後の表示/権限なし、遅延結果の世代整合、停止ownerの回収 | result generation fence / revoke時の表示消去 |
| `SenpGhRequests` | 一意resourceの実行1本、他subscriberをcancelしない、cooldown中のdispatchなし、process回収後の終端、全受理要求の終端 | dedupe / last-subscriber条件 / cooldown / cleanup ownership / cleanup公平性 |

初回は有限の小さい集合を全探索する。無限数のwindowや実際の秒数を証明したとはしない。
外部ユーザーの操作・network成功は公平とは仮定せず、受理後の内部advance/timeout/cleanupに
弱公平性を置く。いつまでも待機する反例を排除する責任は実装のdeadlineとfinalizerが負う。
全状態のstutteringが仕様上可能でも、受理済み要求のlivenessを別途検査する。

モデルにはcredentialsの実byte、OAuth server、Win32 process API、native pixels、
JSON parser、TLS、実メモリ上限を含めない。それらはunit/integration/native試験で検査する。
モデルが合格しても未実装コードの正しさを証明したことにはならない。
実装時はモデルaction → owning method → fixtureの対応表を更新する。

## 検査の実行と記録

共通runnerを使う:

```powershell
py -3 tools/verify-senp-github-models.py --jar C:/Users/developer/AppData/Local/Programs/TLAplus/tla2tools.jar --output C:/Users/developer/tmp/senp-github-formal
```

既存Search gateと同じ公式tla2tools v1.7.4 / TLC 2.19をSHA-256で固定する。
各Javaに512 MiB、60秒、workers 2を設定し、state/outputは指定した作業ディレクトリへ置く。
正例はexit 0、全探索完了、queue 0を要求する。
安全性負例はexit 12と指定invariant名を要求する。進行性負例はexit 13、
temporal violationと循環traceに加え、configのPROPERTIESが指定した1条件だけであることを要求する。
tool不在・hash不一致・構文エラー・timeoutは失敗。

証跡にはmodel/config/runner/toolのhash、source commit、command、終了code、
生成/到達状態数、探索深さ、所要時間、負例の対象を記録する。
再利用した既存の`docs/formal/states/`を消さず、検査ごとに固有のmetadirを使う。
証跡は今回実行した入力byteに対する記録。改行コードを含め入力が変わった場合は再実行する。
