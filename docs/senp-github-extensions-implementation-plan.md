# Issue #296: 小さなコミットによる工程と形式検証

対象: [#296](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/296)。
仕様: [SENP GitHub拡張設計](senp-github-extensions-design.md)。
今回の作業範囲は工程の詳細化とTLA+/TLCによる設計のモデル検査。
製品のAPI・画面・GitHub拡張の実装は後続工程として区別する。

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
| F03 | 共有要求・購読解除・cooldown・cleanupのTLA+ | F01 | 同runner `--model requests` | single-flight、他subscriber保護、期限内終端、負例 |
| F04 | 全モデルをCI必須gateへ追加、結果と対応表を保存 | F02/F03 | runner全件、unit test、checkout-invariance | hash-pinned tool、狙った反例、終了済みprocess、evidenceのhash |
| G01 | schema 2/ABI判別と既存v1互換 | F04 | `cargo test -p sakura-senp --locked` | 旧package合格、未知schema/ABI拒否 |
| G02 | WIT event/effectとC++/Rust往復fixture | G01 | `SenpEffectProtocol.*`とRust host tests | casing、sequence、上限、ID再使用を検査 |
| G03 | bounded dispatch・ack・host終了のsession実装 | G02 | `SenpEffectProtocol.*:SenpRuntimeLifecycle.*` | pendingが全て一度終端、切断後の古いevent反映0 |
| G04 | owner単位のcontribution登録・dispose | G03/F02 | `SenpViewLifecycle.*` | 部分登録なし、失効・更新失敗の所有権保持 |
| U01 | container内の複数View nativeページ | G04 | `SenpViewContainer.*`、dual-capture | View独立、resize/移動/focusに描画残りなし |
| U02 | lazy TreeDataProviderとstable item選択 | U01 | `SenpTreeProvider.*` | page/expand/refresh、重複・循環・stale拒否 |
| U03 | readonly Editor input/surface切替 | G04 | `SenpReadonlyWorkbench.*` | dirty/undoを保持して詳細と編集を往復 |
| U04 | readonly Markdown/metadata renderer | U03 | `SenpReadonlyDocument.*`、native UI | 本文表示、script無効、local asset権限漏れ0 |
| U05 | chunk付きtext resourceと検索・コピー | U03 | `SenpTextResource.*` | UTF-8境界・上限・partial・失効を区別 |
| U06 | command/menu/activationと汎用sample拡張 | U02/U04/U05 | sampleのnative総合試験 | GitHub固有コードなしで2 View/本文/ログ表示 |
| T01 | Git runnerからprocess primitiveのみ抽出 | F04 | `BoundedProcessRunner.*:GitCommandRunner.*` | SCM無回帰、argv・pipe・job cleanup |
| T02 | Control brokerのowner/grant認可 | T01/G03 | `SenpToolGrants.*` | 別owner/profile/digest/失効handleを拒否 |
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
| `SenpGhRequests` | 一意resourceの実行1本、他subscriberをcancelしない、cooldown中のdispatchなし、process回収後の終端、全受理要求の終端 | last-subscriber条件 / cooldown / cleanup ownership |

初回は有限の小さい集合を全探索する。無限数のwindowや実際の秒数を証明したとはしない。
外部ユーザーの操作・network成功は公平とは仮定せず、受理後の内部advance/timeout/cleanupに
弱公平性を置く。いつまでも待機する反例を排除する責任は実装のdeadlineとfinalizerが負う。
全状態のstutteringが仕様上可能でも、受理済み要求のlivenessを別途検査する。

モデルにはcredentialsの実byte、OAuth server、Win32 process API、native pixels、
JSON parser、TLS、実メモリ上限を含めない。それらはunit/integration/native試験で検査する。
モデルが合格しても未実装コードの正しさを証明したことにはならない。
実装時はモデルaction → owning method → fixtureの対応表を更新する。

## 検査の実行と記録

作成予定の共通runnerを使う:

```powershell
py -3 tools/verify-senp-github-models.py --jar C:/Users/developer/AppData/Local/Programs/TLAplus/tla2tools.jar --output C:/Users/developer/tmp/senp-github-formal
```

既存Search gateと同じ公式tla2tools v1.7.4 / TLC 2.19をSHA-256で固定する。
各Javaに512 MiB、60秒、workers 2を設定し、state/outputは指定した作業ディレクトリへ置く。
正例はexit 0、全探索完了、queue 0を要求する。
安全性負例はexit 12と指定invariant名、進行性負例は指定したtemporal violationとtraceを要求する。
tool不在・hash不一致・構文エラー・timeoutは失敗。

証跡にはmodel/config/runner/toolのhash、source commit、command、終了code、
生成/到達状態数、探索深さ、所要時間、負例の対象を記録する。
再利用した既存の`docs/formal/states/`を消さず、検査ごとに固有のmetadirを使う。
この文書を実施済みにする時は実行結果の証跡へリンクする。
