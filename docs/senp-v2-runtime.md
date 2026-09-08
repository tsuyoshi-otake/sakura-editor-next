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

[CSenpOwnerRequests](../sakura_core/senp/SenpOwnerRequests.h)はcurrent ownerとnative consumerの間で
owner全体のrequest generationを発行する。Busyなど未受理の要求ではgenerationを消費しない。
`ToolCompleted`などの派生eventは新しいoperation IDを持つが、元要求のowner/workspace/account/request
scopeを維持する。最大16 lineageとterminalを保持し、取消は同じlineageの全invocationを対象にする。

ownerの`Poll`中は再入を拒否するため、publicationの`Apply`はconsumer callbackを直接呼ばず、
検証済みterminalをこのbrokerへ積むだけにする。native compositionは`Poll`が戻った後に取り出して
Tree/document/commandへ反映し、派生処理がなければ明示的に`Finish`する。この順序により、Tree page
反映の末尾で次の可視loadが必要になっても、途中のBusyを実際のprovider failureへ変換しない。

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

## U03: readonly Editorの登録とnative切替

`SenpReadonlyWorkbench`は既存のEditorCoreServiceに最大16 inputを登録し、別groupや
独自active flagを作らない。extension ID・owner/workspace/account generation・resource IDから
`senp:` URIを作り、同じscope/resourceは再利用する。表示titleはidentityに使わない。
openはinactiveで、native content surfaceを準備・bindしてからShowする。操作IDはcontrollerの
単調増加instance IDも含み、controllerの再作成で過去のreplayと衝突しない。

`SenpEditorSurfaceSwitcher`は同じparent下の保持済みHWNDをinput IDへbindする。
既存legacy surfaceも同じ選択経路を通り、本文、dirty、undo、選択とscrollを破棄しない。
未準備・失効・別ownerのHWNDは拒否し、未知のactive inputへlegacy文書を代用しない。
非表示やPanel最大化はinputを閉じない。所有者はcore通知をcoalesceしてApplyし、
Close/失効後はnative bindingを回収してからsurfaceを破棄する。

native callback中のCloseはprojectionが戻ってから完了する。Unbindの再入はfalseを返すので、
呼出側はHWNDを保持して次のUI messageで再試行する。coreのCloseに失敗した登録は所有権を残し、
Shutdownの結果をcomposition ownerが観測して明示的に再試行する。
読取専用inputからSave/Save As/Revert/Undo/editへは進まない。Copy/Select All/Findは内容surfaceへ、
Closeはreadonly登録へ、Save Allとglobal open/window commandはWorkbenchへ返す。
保存・終了・backupでは選択中のreadonly inputからCEditDocを推測せず、保持したlegacy inputを参照する。

追加18件と既存Editor/working-copyを含む86 tests、3,762件のinventory照合が合格。
実native EDITのUndo実行とCEditDocのundo block保持を別々に検証した。
`tools/verify-senp-view-rendering.ps1 -ProbeSet ReadonlyEditors`の2回の新規起動、
dark/light/system High Contrast、96/144/192 DPI、216試行は実画面と強制再描画の差分0%。
PrintWindow単独の欠落4件は、同じgeometryの独立noise floorと実画面差分0%で区別した。
fixtureのtheme設定は各matrixの準備段階に限定し、同じtheme/DPIでのresize操作を強制再描画しない。
native probeは180秒、runnerは200秒の期限とexact PID cleanupを持つ。
この工程はsurface切替の境界を検証し、U04/U05の本文rendererやU06のCEditWnd・tab・command・
backup・sample公開の完成を意味しない。schema 2の公開gateは維持する。

形式モデルは[対応表](formal/senp-github-models.md)に従う抽象仕様の検査であり、
Win32 API・具体的なwire sequence・実メモリ上限の証明ではない。


## U04: 構造化readonly本文

`SenpReadonlyDocument`は既存readonly inputのscope/resourceを保持し、runtimeで受理された
要求をBeginへ渡す。supersede、Apply、Fail、Expire、Closeはretired contextを返し、
compositionがsubscriberを取り消す。実process・toolの回収はbrokerの所有権のままとする。
異なるowner/workspace/account/requestは本文を置き換えない。同一revisionのrefreshは
全payloadが同じ場合だけ受理し、下降revisionと同一revisionの変更を区別して拒否する。
Expire/Closeは終端で、cacheを解放して新しい要求を拒否する。

構造化本文は既存の1 MiB protocol validatorをコピーなしで再利用し、最大32 sections、
64 metadata fields、16 columns、256 rows/table、4,096 render blocksで有界にする。
readonly inputに合わせてtitleは256 UTF-16 unitsとする。Markdownだけを解析し、
metadata/table値は直接native tableへ渡す。NUL等のcontrol文字は表示可能な置換文字へ変換する。
parserへdocument pathやworkspace rootは与えず、全resource referenceのpath/rootを消去して
blockedにする。普通のMarkdown previewのStrict HTTPS設定は変更しない。

`SenpReadonlyDocumentView`はroot内に既存CMarkdownPreviewWndとscrollbarを所有する。
rootをU03のsurface switcherへbindするため、非表示・切替でoverlayだけが残らない。
構造化本文は既存workerの1実行/最新1待ちqueueで準備し、UI側で結果をcommitする。
新しいqueueは古いdrag-deferred結果を破棄し、staleな本文がdrag終了時に復活する経路を閉じる。
Preparedは解析結果のcommitで、native reflowの完了は別のViewportSnapshotで確認する。
render失敗は明示messageとなり、自動retryしない。view終了は既存の有界retirement ownerへworkerを渡す。

この段階ではtext-resource sectionはtyped Unsupported。U05のtext/search/copyとU06の
command、tab、backup、sample公開を通すまで、schema-2詳細機能をアプリから公開しない。
検証用runtime portのfixtureをlive GitHubの本文取得と同一視しない。

U04はRust peer fixtureを含む90 focused tests、3,780件のruntime inventory照合、
同一binaryの2回起動による216描画試行で検証した。3テーマ・3 DPIの各操作を一方の起動で2回、
他方で1回実施し、visibility/resize/scroll/refreshの実変化を観測した。
再描画差分・PrintWindow欠落・画像fetchは0、worker retirementとprocess終了も確認した。


## U05: 有界text resourceとnativeログ表示

`SenpTextResourceStore`はControlのbroker threadが所有するmemory-only resourceである。
profile、extension、package digest、grant、owner/workspace/account generation、revisionを
全操作で一致させる。opaque handleだけでは認可にならず、brokerはgrantの現在の有効性も確認する。
64 KiB固定pageを使い、1 resource 32 MiB、Control内64 MiBの割当payload、64 handleで有界にする。
offsetは単調増加し、1回のappend/readは64 KiB以下。1 byteずつ受け取ってもchunkごとのmetadataを増やさない。
allocationは状態commit前に準備し、例外時のprefixを保持する。callerがFinishとproducer停止を所有する。
上限では別resourceを暗黙evictせず、受理済みprefixをPartial/LimitExceededとして残す。
Expireは本文を消去してtombstoneを残し、Releaseはslotを返す。Closeは全領域を回収する。

decoderはUTF-8を追加分だけO(B)で処理し、保留byteは最大3。CRLF境界を正規化し、ANSI/OSC、
NUL、control、bidi制御文字を実行せず表示用に変換する。不正・途切れたscalarは当該chunkを拒否し、
前のprefixがあれば失敗した部分ログとして残す。

`SenpTextResourceView`はSystem32のWindows Rich Editをplain text/readonlyで使用する。
chunkの長さ・offset・scope・revisionとsourceの終端を検証して追記し、選択とscrollを保持する。
全文置換、RTF、URL、OLE content、端末制御やnetwork処理は持たない。
Ctrl+F、Enter/F3・Shiftで前後検索とwrap、Escape、Ctrl+A、文字単位のCopyに対応する。
選択がないCopyは現在行をLF付きでコピーする。native readonly属性とUIA text selectionを検証する。
Loading/空/Complete/Partial/Failed/Expiredは別表示となり、期限切れは本文と検索を消去する。
挿入中のnative failureは不確かな表示を閉じ、native破棄からLoadingへ戻らない。

U06でtext-resource sectionをrendererへ振り分け、tab/command/backupとsampleへ接続する。
T02/T08は有効grant、download、subscriberと実processの回収を所有する。
この工程だけではschema 2やGitHub network機能を公開しない。

U05は60 focused testsと、Page Heap設定を保持したまま別名の同一binaryで実行した26件が合格。
32 MiB全体の文字一致・長さ・追記方式の検査は最終版で7.789秒、runtime inventoryは3,807件が一致した。
2回の新規起動で360+90の描画試行が合格。実画面の再描画差分は最大0.00309%で、
非ゼロ1件の全変化pixel（8 pixel）は本文外の下端8 pixel以内の丸い角に限られた。
PrintWindowだけの欠落37件は同一geometryのnoise floorと実画面の安定性で区別した。
probe/runnerには500/520秒の期限があり、全起動のprocess終了を確認した。
検査用windowだけを一時的に最前面へ出し、他のwindowや診断設定は変更しない。

検索やコピー失敗の通知中もLoading/Partial等の取得状態を必ず先に表示する。
WM_SETREDRAWの再開後と子windowの配置変更ではRDW_FRAMEを含め、native scrollbarも再描画する。


### U06途中: ログの共有スクロールバー

readonlyログの縦横バーを既存の`COverlayScrollbar`へ接続した。Rich Editの
非表示nativeバーの`SCROLLINFO`は移動後に更新されないため、`EN_REQUESTRESIZE`の
内容寸法と`EM_GETSCROLLPOS`の実位置を既存`ExplicitModel`へ渡す。64 KiBを超える
位置のドラッグ、追記後の選択・縦横位置保持、hidden/expired/closedをnative試験で確認した。
`WS_CLIPSIBLINGS`により本文の再描画が重なるバーを消さないようにした。

同一Debug binaryで27件の関連試験、Page Heap設定を変更しない61件の回帰試験、
別起動の180+90回のdual captureが合格。描画差の最大値は0.000000000%、
既存0.05%閾値は変更していない。runtime inventoryは3,808件で照合する。
これはU06の途中の変更であり、本文の選択/検索、mixed section routing、実アプリの
tab/command/backup、owner/page公開、sampleは引き続きU06の完了条件。


### U06途中: 構造化本文の選択・コピー・検索

native Markdown rendererの実文字位置から本文を選択し、Ctrl+A、Ctrl+C/WM_COPY、
Shift-clickを処理する。折り返しでコピー文字列を変えず、元の改行と表のセル区切りを保持する。
選択用indexは既存の有界layout continuationで構築し、paint中のparseやworkerを追加しない。
新しいdocumentの受理時に古い選択・コピー・検索を無効にし、workerとreflowの完了後に公開する。

SENP本文にはCtrl+F、Enter/F3、Shiftによる前検索、wrap、Escapeのnative find操作を接続した。
検索語は1,024 UTF-16 unitに制限し、不正なsurrogateを拒否する。コピーの失敗はstatusに表示し、
callbackの例外で不確かな表示を閉じる。hidden refreshは他のcontrolからfocusを奪わず、
閉じる・capture cancellation・native破棄の各経路で所有者が操作を終端させる。

find入力は既存CInputBoxGeometryと親描画のWorkbench frameを使う。native EDITの
WS_EX_CLIENTEDGEではPrintWindow時にfocus境界が変化し、画面に1 pixelの差が出たため、
Searchと同じ入力枠の所有方式で修正した。移動後のquery/statusと本文を一緒に再描画する。

これは構造化本文surfaceの途中の変更であり、mixed section routing、実アプリのtab/command/
backup、owner/page公開とsampleはU06に残る。独自rendererのUIA TextPatternと通常の
Markdownアプリの検索統合は未実装であり、ログのnative Rich Editの機能とは区別する。

構造化本文checkpointは分離した作業ツリーでDebug solutionをビルドし、120件の関連回帰試験が
合格した。最後に加えたthrowing-copyのnative終端も含む。runtime inventoryは3,811件が一致。
同じbinaryの別起動による216+108回のdual captureが合格し、実画面の再描画差は最大
0.003969334%、非ゼロ1件だった。全変化14 pixelは本文外の右下端8 pixel以内に限られた。
PrintWindowだけの欠落
0件は同一geometryのnoise floorで区別し、既存0.05%閾値は変更していない。
runnerとbuild processの終了を確認した。通常アプリの巨大段落測定は起動後のdocument確認に
到達せず測定値を得ていないため、性能合格とは扱わず、実アプリ統合時の検証に残す。

### U06途中: 構造化本文とログの混在routing

`SenpReadonlyDocumentHost`は1つのreadonly inputを、隣接するMarkdown/metadata/tableの本文pageと、
各text-resource pageへ順序を保って分割する。pageが複数ある場合だけnative COMBOBOXを表示し、
1 pageでは追加chromeを出さない。このsection selectorはdocument内部の移動であり、Editor tabや
ViewContainerを増やさない。戻ったpageは同じnative HWNDを使い、選択、検索、scrollを保持する。

text-resourceの権限はWasmのhandleやdocument revisionから組み立てず、broker側adapterが返す
profile/package digest/grant/revision付きscopeを使う。owner/workspace/accountの全次元を照合し、
表示、command、readの前にgrantがcurrentか再確認する。どれか1件でも解決不能・失効なら、旧本文と
ログを全て消してDeniedにする。読み込み要求は表示中のLoading pageだけ、同時に1件、最大64 KiBとし、
空の非終端応答後はproducerからの明示通知まで再要求しない。同一scope/handleを複数sectionが参照する
場合は1つのnative bodyとread streamを共有する。I/O、deadline、取得済み要求のfinalizeはcompositionが
所有し、hostの世代更新やcloseで暗黙に完了扱いにしない。

分離したDebug solution buildは警告0・error 0。混在hostを含むdocument試験23件、追加後のhost試験5件、
既存focus試験の単独再実行が合格した。暗色/明色/High Contrast、96/144/192 DPIでsection切替、resize、
表示、find、selectionを2反復したdual captureは180/180件合格し、probe processの終了も確認した。
最初の描画runは外側surfaceを縮めた旧領域にselector/scrollbarが残ることを検出し、outer moveの再描画と
親theme transactionを修正した後に同じ閾値で合格した。既存32 MiBログ性能試験は変更後binaryの単独実行で
60.37秒となり、既存60秒条件を0.37秒超えた。閾値は変更せず、混在host固有の合格とは分けて残余とする。

### U06途中: 実Editor Groupのtab・command・backup接続

`SenpReadonlyEditorController`を`CEditWnd`がwindow lifetimeで所有し、保持したlegacy splitterと
SENP native surfaceを同じ`EditorCoreService`のactive inputから投影する。readonly選択中はlegacy
splitter、minimap、Markdown previewを隠し、Editor Partが非表示またはPanel最大化でもinputと各native
surfaceの選択・検索・scrollを保持する。Copy、Select All、Find、次/前検索、Closeはstable command IDで
選択中surfaceへ到達し、Save/Save As/Revert等はreadonlyで`NotApplicable`として終端する。

readonly inputが1件でも存在するとき、`CTabWnd`は同じCore groupの順序、title、active inputをopaque IDで
表示する。この間は従来の`TCITEM.lParam`をprocess HWNDとして扱う切替、drag、reorder、context commandを
実行しない。clickとcloseはcontrollerへ戻し、callbackは同期Core通知でprojectionが更新されても安全な
copyを実行する。最後のreadonly inputが閉じた時とworkbench shutdown時にはprojectionを消し、従来の
process-window tab表示へ戻る。現段階は1 window内の1 Editor Groupだけで、cross-window group統合やtab dragは
対応範囲外である。

working-copy captureはactive readonly inputをCEditDocへ読み替えず、inactiveに保持されたpersistable legacy
inputがちょうど1件ならそのidentity/revisionを採用する。候補0件または複数件はfail closedとする。
readonly titleはtop-level captionにも反映し、終了時はtab callbackをcontrollerより先に破棄する。

owner/page公開、GitHub-free sampleと実アプリのend-to-end検証はU06に残る。独自Markdown rendererのUIA
TextPatternと全pageを横断する検索は実装済みとは扱わない。

### U06途中: GitHub非依存の実v2 guest

`sakura-senp-sample`はWASI/importを持たない実Componentとして、`sample.senp` container内の
`sample.projects`と`sample.states`という2つのTree Viewを実装する。固定fixtureだけでroot、2 page、
Empty、Failedを返し、commandからMarkdown/metadata/tableとLoading text resourceを含む詳細documentを開く。
network、filesystem、GitHub、`gh`には依存しない。

unit testに加え、`wasm32-unknown-unknown`のcore moduleを`componentize`し、実
`sakura-senp-host --protocol 2`へHello、Activate、TreeRequestをframe送信して、activationの2 invalidationと
Projectsの2 itemを確認した。この段階はguest側の縦切りであり、owner effectをnative View/page/editorへ
transactionalに公開するadapterと実アプリend-to-endはU06に残る。
