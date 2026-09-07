# SENP GitHub拡張: 有限モデルと検証証跡

対象は[#296](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/296)の
[設計](../senp-github-extensions-design.md)と
[工程F01–F04](../senp-github-extensions-implementation-plan.md)。
2026-09-07にTLCで正例3件、安全性の意図的負例8件、進行性の意図的負例1件を検査し、
全ケースが期待した結果になった。[機械可読の実行証跡](senp-github-evidence.json)を保存する。
これは製品実装前の有限設計モデルの結果であり、C++/Rust/API/画面の完成証明ではない。

## 正例の全探索

| モデル | 有限化した範囲 | 生成状態 | 到達状態 | 深さ | 結果 |
|---|---|---:|---:|---:|---|
| [SenpGhConnection](SenpGhConnection.tla) | 2 account、epoch 1–3、1 candidate操作、既存接続あり/なし | 233 | 137 | 7 | safety/liveness合格、queue 0、exit 0 |
| [SenpContributionOwner](SenpContributionOwner.tla) | 1 owner、2 contribution、generation 1–3、1実行と1 mailbox | 246 | 84 | 14 | safety/liveness合格、queue 0、exit 0 |
| [SenpGhRequests](SenpGhRequests.tla) | 1 resource key、2 subscriber、clock 0–4、retry最大1回 | 1,145 | 569 | 13 | safety/liveness合格、queue 0、exit 0 |

`CHECK_DEADLOCK FALSE`は終端後や要求未受理のstutteringを許容するため。
受理済み要求、進行中の準備、drainingの放置は別のtemporal propertyで拒否する。
外部ユーザーの操作やnetwork成功には公平性を仮定しない。接続の内部advance/abort、
ownerのprepare/finish/drain、要求のclock/expire/cleanupには弱公平性を置く。
これは「内部finalizerが永久に放置されない」という実装義務であり、OSが必ず何秒以内に
processを回収するという証明ではない。dispatchされずdeadlineでtimeoutになる要求も許容する。

3モデルは独立して検査する。認証失効とowner失効とschedulerを同時に合成した系、
複数resource間の公平性、任意数のwindow、繰り返す無限retryを証明してはいない。
実装の契約が変われば、必要なモデル範囲を拡張して再検査する。

## 意図的に壊した条件と検出された反例

全て同じ`.tla`に対する別`.cfg`。正例configは全ての安全条件を有効にする。

| config末尾 | 外した条件 | 期待・実測した違反 |
|---|---|---|
| Connection `_NoIdentity` | 本人照合前の公開禁止 | `IdentityBeforeGrant`、exit 12 |
| Connection `_NoGeneration` | 接続解除前のcandidate拒否 | `NoStaleConnection`、exit 12 |
| Owner `_NoGeneration` | 旧owner generationの結果拒否 | `CurrentView`、exit 12 |
| Owner `_NoClear` | revoke時の可視内容消去 | `NoRevokedView`、exit 12 |
| Requests `_NoSingleFlight` | 合流時のprocess重複排除 | `SingleFlight`、exit 12 |
| Requests `_NoLastSubscriber` | 他subscriberが残る間の共有処理維持 | `SharedLeasePreserved`、exit 12 |
| Requests `_NoCooldown` | retry待ち時間のdispatch禁止 | `RespectRateWindow`、exit 12 |
| Requests `_NoReap` | process回収後の終端 | `TerminalOwnsNoProcess`、exit 12 |
| Requests `_NoCleanupFairness` | cleanupの進行義務 | `EveryAcceptedTerminates`のtemporal反例、exit 13 |

最後の負例では要求が`cleaning`に残ったままclockがdeadlineへ到達し、
`Stuttering`を続けてterminalを返さない経路が見つかった。
TLC 2.19はtemporal違反名を出力しないため、runnerはこのconfigの`PROPERTIES`が
`EveryAcceptedTerminates`だけであることを実行前に検査し、exit 13・temporal違反・
初期状態からのtrace・循環の全てを要求する。
安全性負例はexit 12・指定invariant名・初期状態からのtraceを要求する。
構文/評価エラー、別の違反、timeoutは負例成功に数えない。
workers 2で最初の反例に停止するため、負例の探索途中の状態数は実行間で変わりうる。

## 実装とテストへの対応

以下のmethod/suiteは**後続工程で実装する責務**。現時点で存在すると主張しない。

| model action / property | 実装を置く工程・境界 | 実装時に必要なテスト |
|---|---|---|
| `Begin/VerifyIdentity/Publish/Disconnect` | T04/T05、Control brokerのconnection candidateとgrant公開 | `GhConnectionLifecycle.*`、fake gh、本人不一致・cancel・disconnect後の遅延完了 |
| `Prepare/BuildStage/CommitUpdate/AbortUpdate` | G04、ownerのcontribution transaction | `SenpViewLifecycle.*`、部分登録不可、準備失敗で旧版維持 |
| `FinishRead/Apply/Revoke/Drain` | G03aの`CSenpRuntimeSession::Receive/Stop/TransportFailed`とG04のowner fence | `SenpRuntimeLifecycle.*`で遅着・未回収effectsの破棄を検査。G03bのworker回収とG04の可視内容・権限回収は別の実装境界 |
| `Subscribe/Dispatch/Unsubscribe` | T07、Control brokerの共有要求と購読lease | `GhReadScheduler.*`、複数window合流、最後の購読解除だけでcancel |
| `ReceiveRateLimit/Tick/Expire` | T07、deadline・Retry-After・cooldown | fake clock、provider window、backoff/jitter、queue中timeout |
| `Cleanup/TerminalOwnsNoProcess/EveryAcceptedTerminates` | T01/T07/T08、process runnerとcompletion所有権 | `BoundedProcessRunner.*:GhLogResource.*`、pipe飽和、子孫kill/reap、cancel/timeoutの一度終端 |

実credential、TLS、HTTP/JSON parser、Wasmのfuel/memory、Win32 Job Object、
複数processのIPC、SCMを参照したnative描画はモデル外。
UI工程では設計のSCM比較・dual-capture・keyboard/UIAを別に実行する。

## 再実行とCI

```powershell
py -3 tools/verify-senp-github-models.py --jar C:/Users/developer/AppData/Local/Programs/TLAplus/tla2tools.jar --output C:/Users/developer/tmp/senp-github-formal
py -3 -m unittest discover -s src/test/py -p test_senp_github_models.py -v
py -3 tools/build/sakura_build.py --format json lint checkout-invariance
py -3 tools/build/sakura_build.py --format json inventory semantic --strict
```

`--jar`/`--output`は各環境の絶対パスを指定する。`--model connection|owner|requests`で分割実行可能。
公式tla2tools v1.7.4 / TLC 2.19を既存Search gateと同じSHA-256で固定する。
Javaをshellなしで直接起動し、各caseを512 MiB・workers 2・60秒に制限。
timeout時はkillしてwaitし、異常終了を成功へ変えない。各実行・caseに固有のmetadirを置く。

証跡はモデル/config/runner/tool/logのSHA-256、入力source commit、実行command、
終了code、状態数・深さ、所要時間、processの終了を記録する。
今回の入力source commitは`c08c00804c34f5ac123566a0ee93743a0272518e`。
hashは検査に使用した入力byteに対する値で、改行の正規化をしない。
別checkoutで改行が変わった場合はrunnerを再実行し、そのcheckoutの証跡を保存する。
ローカルlogのパスは今回の作業用パスであり、他の環境に同じパスがあるとは仮定しない。

[Architecture gates](../../.github/workflows/architecture-gates.yml)はSENP検査を必須stepとして実行し、
成功/失敗時ともreceiptと各caseの`tlc.log`をartifactへ保存する。state DBはuploadしない。
今回ローカルではgateの6 unit tests、既存CI path契約3 tests、checkout-invariance、
semantic ratchet、証跡のhash照合とprocess監査が合格した。Java/Python runner残存は0。
workflowのstep・条件・timeout・artifact構造を確認済み。remote CIは未実行。

検証中に、Booleanの次状態代入の右辺で`\/`を括弧に入れないと、
意図的負例でsuccessorの変数が未定義になるケースを実測した。
式を修正し、全ケースを再検査した。狙った違反の種類とtraceまで照合するgateが、
この評価エラー(exit 75)を不合格として止めることも確認できた。
