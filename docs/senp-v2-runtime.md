# SENP v2 runtime session（#296）

G03aはv2の要求・応答・終了状態、G03bはWin32 workerとの接続、G04は
owner登録候補・置換・失効・回収の契約を実装する。native page/commandとbroker grantの
adapterはU01/U06/T02で接続する。低位のhostとownerを実行できても、
schema 2 packageのinstall/enableを許可したことにはならない。

## 所有と終端

- [CSenpRuntimeSession](../sakura_core/senp/SenpRuntimeSession.h)はOS非依存の状態所有者。
  `Begin`・`Submit`で受理した呼出しを、effects ready・cancelled・timed out・rejected・
  host unavailable・protocol errorのいずれかへ一度だけ終端させる。
- `EffectsReady`は`activate/on-event`が戻ったという意味。`StartToolRead`のtool処理や、
  `CompleteCommand`が担う利用者のコマンド完了とは区別する。
- `Cancel`と`Expire`は未送信要求をキューから除き、送信済み要求の遅着結果はackして破棄する。
  `Stop`・切断・protocol faultは未回収の成功結果からもeffectsを消去する。
  既に取り出された結果の表示には、G04のowner失効確認が別途必要。
- `Idle → Handshaking → Activating → Active → Stopping → Stopped`。
  起動途中の取消・異常終了は直接`Stopped`へ到達できる。停止したinstanceは再起動しない。
  新instanceには呼出し側が新しいsession generationを与える。
- actorの呼出しを直列化するのはnative runtime owner。sessionからcaller callbackを呼ばない。

## 順序、再送、容量

wire sequenceは方向ごとに1から増やす。nativeはpipeへ渡す時にsequenceを割り当て、
未送信取消がsequenceの欠番を作らないようにする。ackにはackを返さない。
operation IDはnativeが発行する`s<sessionGeneration>:o<ticket>`。
ticketは正の整数で単調増加し、wire sequenceとは独立する。取消によるticket欠番は許す。

[Rust Session](../rust/senp/sakura_senp_host/src/effect_session.rs)は受理応答をackまで最大16件保持する。
同一sequence・同一正規化payloadの再送には同じ応答byteを返し、guestを再実行しない。
保持中のpayload変更はConflict、回収済みsequenceはStale、新sequenceでの古いticket再使用はConflict。
新ticketの採用前に容量を予約し、満杯なら実行前にBusyを返す。
受信sequenceの欠番・世代不一致・壊れたJSONは接続を終端させる。

nativeのpendingと受取待ちcompletionの合計も16件。要求キューは4 MiBで、ack等のcontrol用に
64 KiBを予約する。キュー全体は64 frameまで。大きなpayloadをコピーしてから検査せず、
型付きgraphをそのまま検査・encodeし、正規化時の最大sequence桁数で容量を予約する。
各frameの1 MiB / 65,536 nodes制限と、最大16件の再送照合用receiptも維持する。
過負荷はBusyまたは明示的なprotocol終了となり、無制限の再試行を行わない。

## Hostと検証

`sakura-senp-host --component <path> --component-sha256 <sha256>`は既存v1を選ぶ。
末尾の`--protocol 2`だけが[専用v2 dispatcher](../rust/senp/sakura_senp_host/src/runtime_v2.rs)を選ぶ。
受信JSONからv1/v2を推測しない。SHA-256を照合した同じcomponent byteをWasmtimeへ渡し、WASIを付けない。
Wasm memoryは32 MiB、fuelは1,000万、epochは10 msごと・20 ticks。
instantiateにもWasm実行が含まれるため、epoch clockはinstantiateより先に開始して失敗時もjoinする。
WITからliftしたhost値とコンパイルのメモリ・時間はWasm Storeの制限だけでは覆えないため、
G03bのnative jobとIPC期限が追加の所有者になる。

## Win32 processの所有

[CSenpEffectRuntime](../sakura_core/senp/SenpEffectRuntime.h)は1つのsession・worker・jobを所有する。
管理側が検証したmoduleのpath/digestとhost pathをnativeの構成時に渡す。
extensionのevent/effectから実行ファイルや引数を指定する経路はない。
`Start`・`Submit`・`Cancel`・結果取得はhostのI/Oを待たず、workerだけがpipeを操作する。
`Stop`は非同期で終了を要求し、`Join`はStopも行って唯一のjoin所有者を確保する。
destructorもStop/Joinを担当する。workerから利用者callbackを呼ばないためjoin再入は発生しない。

parent側はOVERLAPPED付きのローカルbyte pipeを使用する。読取り・書込みを同じ絶対期限へ
束ね、部分frameのたびに期限を延ばさない。handshakeは最大10秒、通常の交換は最大1秒で、
要求deadlineが早ければそれに従う。終了処理は250 msの猶予を使う。取消や期限切れでの
I/O失敗はsession全体を終了し、他の受理済み要求も明示的な失敗にする。
待機中はevent/process handleを待ち、idle pollingや自動再起動を行わない。

pipeは現在のユーザーだけのDACL、最初のinstance、remote拒否で作成する。
子へ継承するhandleはstdin/stdout/stderrの3つに限定し、jobへ原子的に割り当てて起動する。
jobはkill-on-close、process数1、process memory 512 MiB。設定を読み戻して一致しなければ起動しない。
workerはjob終了とprocessの終了確認を終えてから`workerExited`を通知する。
`processExitConfirmed`は実際のwait結果であり、停止要求を出しただけではtrueにしない。

[`CancelIoEx`](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)は
取消の要求であってI/O完了ではない。pipe peerを終了させた上でkernelの完了を回収してから
OVERLAPPEDとbufferを解放する。ここにはOSのlocal pipe completion契約への依存が残る。
壊れたkernel/driverまで含む厳密な実時間上限を、アプリのdeadlineだけで保証するものではない。

## ownerの登録・更新・失効（G04）

[CSenpContributionOwners](../sakura_core/senp/SenpContributionOwners.h)は、nativeが検証した
path/digestと、全descriptor/page/commandを準備するpublication transactionを受け取る。
publicationがない場合はhostを起動する前に`Unsupported`を返す。候補を作り、hostの
activateとeffect検証が成功してから公開する。準備失敗・期限切れ・候補の競合では旧ownerを維持する。
generationは単調増加で、extension ID、package digest、workspace/account revisionと一緒に保持する。

[WorkbenchContributionRegistry](../sakura_core/workbench/layout/WorkbenchContributionRegistry.h)は
ownerごとの完全なcatalog候補を作り、registry identityとbase revisionを照合して公開する。
候補の破棄は何も公開しない。各View/ViewContainerはownerを持ち、組み込みや他ownerのID衝突は拒否する。
解除は割り当てを行わず正確なgenerationだけを取り除く。登録側は64 owner分の終端revisionを予約し、
revision枯渇を理由に解除不能なownerを残さない。検証はID indexを使うO(N)で、owner登録は最大64、
各ownerのcontainerは16、Viewは64。v1の起動時batchは従来どおり。

SENPの対応先は自分のcontainerまたはproduct-owned containerに限定する。
別extensionのcontainerは`Unsupported`、未知のcontainerはInvalid。
実際のVS Codeは、container消失時に残りのViewsをExplorerへ移動し、再登録時に元へ戻す。
[参照したupstream実装](https://github.com/microsoft/vscode/blob/e6aeab60511647b9b00f0bf2f0f02b15f278abb5/src/vs/workbench/api/browser/viewsExtensionPoint.ts)。
このcross-provider保持とfallback移動は現在のnative page poolが扱えないため、対応済みとして
表示しない。制限と解除条件はlayoutの`CLAUDE.md`へ記録した。

runtimeの上限4にはactiveだけでなくpreparing・retiring・cleanup-failedを含める。
受理した準備は16件のtransition receiptの1枠を先に予約し、結果を取り出すまで枠を保持する。
満杯ではhostを追加せずBusy。Pollは各instance最大16件を取り出す有界drainで、I/O待ちや
自動restartを行わない。呼出しのスケジュールはnative compositionの責務。

失効はownerを非currentにし、publication側のgrant・command・可視内容・購読を取り消し、
高位のtool/UI要求を終端してからhostのStopを要求する。結果の反映は毎回ownerと
workspace/accountを照合する。既に外へ取り出した値も、使用直前の`IsCurrent`が必要。
View折り畳みやPart非表示は失効ではない。

Pollはworker終了後にだけjoinする。join失敗を次のPollで再試行せず、cleanup-failedとして
所有権と上限枠を保持する。Closeは全ownerを失効・Stopしてからjoinし、明示的なClose一回につき
失敗joinを一度再試行できる。processの実終了を確認できなければfalseを返し、成功扱いやdetachはしない。
G03同様、OS故障まで含むcleanup完了の保証ではない。

## native ViewContainer/Viewの投影（U01）

[CSenpViewContainers](../sakura_core/workbench/viewcontainer/SenpViewContainer.h)はowner generationごとに
全containerとView本文を非表示で準備する。本文factoryはnative compositionの必須入力で、
未対応の本文をplaceholderで代用しない。PagePoolはcontainerをPartへ取り付け、View本文は
独立して保持する。同じowner内でViewを移動して元containerを閉じても、そのViewのHWND・
選択・折り畳み状態は残る。全ownerのCloseはcallbackを先に失効し、本文を含むnative資源を回収する。

配置・visibilityは確定済みlayout snapshotから反映する。未知の移動先や重複Viewは
windowを動かす前に拒否し、native移動失敗は元parentへ補償する。補償不能・予期しない
HWND破棄は`IsUsable == false`またはCloseという明示的な終端になる。
最大64 Viewの最低高さと比例配分をO(N)で計算し、収まらない場合は外側のscrollを使う。

SCMと共有する見出しは、[VS Code paneview.css](https://github.com/microsoft/vscode/blob/e6aeab60511647b9b00f0bf2f0f02b15f278abb5/src/vs/base/browser/ui/splitview/paneview.css)
を確認して22 DIPへそろえた。16-DIP icon、11-DIP uppercase title、CJKのnormal weight、
title actionのpadding/gapも共通化する。Enter/Space/Left/Rightで開閉し、Up/Downで隣の見出しへ
移る。本文と見出しのfocus通知をまとめ、無効commandは実行しない。
既存UIA/MSAA providerへ展開状態を追加し、実クライアントの操作と破棄後の拒否を検査した。
単独では通ったUIA testが一括実行で失敗したため、[Microsoftの破棄契約](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcoreapi/nf-uiautomationcoreapi-uiareturnrawelementprovider)
に従ってWM_DESTROYでWindows側のHWND/event mapも解放する。

U01ではnative body portとpage lifetimeを扱い、U02のTree本文は以下の境界で接続する。
U06の動的公開はまだ接続しない。別cohortへのView移動はnative retention契約がないため`Unsupported`。
collapse/sizeはSnapshotで回収でき、初期値を構築時に渡せるが、永続化はU06のcompositionが担う。
schema 2のpackage gateは引き続きUnsupportedRuntimeである。

```powershell
build-sln.bat x64 Debug
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/verify-senp-view-rendering.ps1 -Repetitions 2
```

描画runnerはdisabledの専用probeだけを明示実行し、`SAKURA_SENP_VIEW_PROBE=1`を子processに設定する。
probeは120秒で自ら終了し、driverは140秒を上限としてfinallyで起動PIDを回収・再照合する。
`-ProbeSet`（ViewContainers/TreeViews）、`-Tests1`、`-OutputDirectory`、`-Repetitions`（1–4）、
`-AllowedExcessPercent`（既定0.05）を受け取る。
既定出力は`~/tmp/senp-view-rendering/`。実アプリのprofile・設定・packageは変更しない。

CopyFromScreen/PrintWindow、同じ形状での全面再描画後のnoise floor、再描画前後の実画面を保存する。
初回表示はpaintを強制せず実画面3 frameの安定を待ち、各試行は遮蔽gridと実geometry変化を検査する。
stock EDIT本文がresize直後のPrintWindowでだけ文字を省く場合は、実画面が全面再描画前後で一致し、
その後のPrintWindowとも一致した場合に限り`printOnlyOmission`として分離する。画像は残す。
Dark/Light/システムHigh Contrast palette × 96/144/192 DPIで、折り畳み・幅変更・View移動・
container移動を反復する。High Contrastはpalette投影の検査で、OSの設定を変更する検査ではない。
専用本文のEDITは選択・scroll・HWND保持を観測するfixtureであり、完成したGitHub画面ではない。

## lazy Tree View（U02）

[TreeViewModel](../sakura_core/workbench/tree/TreeViewModel.h)はOS/SENP非依存で、stable ID、
親子関係、展開・選択、page/refreshと取消ticketを所有する。2,000 items、深さ16、256 items/page、
保持文字列・metadata計8 MiB、同時8 parent loadsを上限とする。page全体を検証し、変更するnodeと
兄弟列だけを準備してから反映する。全payloadの複製を避け、重複・循環・付替え・古いrevisionや
cursor不整合は既存snapshotを残して失敗にする。refreshはstable IDの選択と利用者の展開を保持する。

[SenpTreeProvider](../sakura_core/workbench/tree/SenpTreeProvider.h)はvisible rootから必要な枝だけを
非同期で取得し、葉・閉じた枝・非表示Viewは取得しない。同じ親の処理を重複させず、30秒の期限、
hide/collapse/refresh/owner失効でsubscriberを終端する。busy/error/timeoutは明示retryまで再送しない。
native portがowner全体でrequest generationを発行し、派生tool結果のoperation IDが変わっても
owner/workspace/account/requestのscopeを照合する。実toolの回収はbrokerが引き続き所有する。

[CSenpTreeView](../sakura_core/workbench/tree/SenpTreeView.h)は実Win32 TreeViewをU01 bodyへ載せる。
22-DIP row、16-DIP icon、label/description、SCM共通palette/overlay scrollbarを使い、Loading、
Empty、Retry、Load moreはextension item IDと分離したnative状態行とする。選択だけではcommandを
実行せず、対になったclickまたはEnterで実行する。command付きの枝はtwistieでのみ開閉する。
native hierarchy/keyboard/MSAA/UIA ExpandCollapseを使う。native accessible nameはlabelとdescription、
詳細tooltipはinfo tipに対応し、VS Codeのtooltip優先aria nameとは異なる。このWin32側の制約は
[所属境界](../sakura_core/workbench/tree/CLAUDE.md)に記録する。

`TVM_EXPAND`は初回展開後に通知を省くため、要求入口でもmodelへ反映する。scroll復元時に閉じた枝の
子を指定すると親が自動展開されるので、閉じた親へanchorを戻す。native stateはbitを明示的にmaskする。
native生成・描画の致命的失敗はbodyを破棄して`projectionFailed`でcohortへ伝え、普通の取得失敗とは分ける。

```powershell
build-sln.bat x64 Debug
pwsh -NoProfile -File tools/verify-senp-view-rendering.ps1 -ProbeSet TreeViews -Repetitions 2
```

通常の`TreeViewModel.*:SenpTreeProviderTest.*:SenpTreeView.*`は25 tests。
実ウィンドウでlazy load、page/retry、取消、反復開閉、command、focus/scroll維持、owner失敗とUIAを検証する。
別起動の描画probeは3テーマ・3 DPIでexpand/resize/scroll/refreshを往復し、実native row text/stateと
geometryの変化を確認してからdual captureする。provider portのfixtureであり、GitHubのlive dataや
Wasm/tool brokerの結合はU06/T工程で検査する。v2 package gateは引き続きUnsupportedRuntime。

## 再現可能な実プロセス検査

```powershell
build-sln.bat x64 Debug
py -3 tools/verify-senp-runtime.py --offline
```

[runner](../tools/verify-senp-runtime.py)は既存lockfileで専用peerと実WIT v2 Wasm componentを
buildし、既定では`~/tmp/senp-runtime-fixtures/`へ置く。`--output-dir`で保存先、`--tests1`で
検査対象を指定できる。`--offline`は事前に取得済みのlocked dependencyだけを使う。
`--prepare-only`はfixture作成だけで、結果は`prepared`となり検査合格とはしない。
native helperは`test-fixtures` featureでのみbuildし、package catalogや配布先へ入れない。

runnerが`SAKURA_SENP_RUNTIME_FIXTURES`を設定して6つの`SenpRuntimeProcess` testsを実行する。
変数がない通常の単体実行ではこのsuiteは明示的にskipするが、受入runnerはskipを不合格にする。
実componentのactivate/on-event/fuel trap、読取り停止、書込み停止、partial/oversized frame、
crash、jobメモリ上限、複数pendingのdeadline、同時Stop/Join、起動失敗に加え、
実Wasmのowner更新・digest不一致での旧版維持・全process回収を検査する。
副作用はテスト用job/processと作業ディレクトリだけで、package installや可視windowの起動はない。

Rust codec出力→C++読取・出力→Rust読取の順で交換し、両言語の正規化byteも比較する。
owner/catalog 15件と旧v1 component実行を含め、native 37件とRust host 15件が合格した。
runnerはchildごとの期限・log SHA-256・終了codeと、実行後のprocess照合を`evidence.json`へ保存する。
これはローカルの受入記録であり、未実行のremote CIを合格と扱わない。

G03aの受入検査は`SenpRuntimeLifecycle.*`（11件）とRustの`effect_session`（6件）。
送信前取消、完了容量の予約、再送、ack、ID再使用、contextの全世代、deadline、失効後の受取待ちeffects、
JSON escapeを含むキューbyte上限を検査する。旧v1 component実行とG02の双方向codec fixtureも回帰対象。

形式モデルは[対応表](formal/senp-github-models.md)に従う抽象仕様の検査であり、
Win32 API・具体的なwire sequence・実メモリ上限の証明ではない。
