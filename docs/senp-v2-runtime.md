# SENP v2 runtime session（#296）

G03aはv2の要求・応答・終了状態、G03bはWin32 workerとの接続を実装する。
packageからのowner登録と寄与の公開はG04で接続する。低位のhostを実行できても、
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

runnerが`SAKURA_SENP_RUNTIME_FIXTURES`を設定して5つの`SenpRuntimeProcess` testsを実行する。
変数がない通常の単体実行ではこのsuiteは明示的にskipするが、受入runnerはskipを不合格にする。
実componentのactivate/on-event/fuel trap、読取り停止、書込み停止、partial/oversized frame、
crash、jobメモリ上限、複数pendingのdeadline、同時Stop/Join、起動失敗を検査する。
副作用はテスト用job/processと作業ディレクトリだけで、package installや可視windowの起動はない。

Rust codec出力→C++読取・出力→Rust読取の順で交換し、両言語の正規化byteも比較する。
旧v1 component実行を含め、native 21件とRust host 15件が合格した。
runnerはchildごとの期限・log SHA-256・終了codeと、実行後のprocess照合を`evidence.json`へ保存する。
これはローカルの受入記録であり、未実行のremote CIを合格と扱わない。

G03aの受入検査は`SenpRuntimeLifecycle.*`（11件）とRustの`effect_session`（6件）。
送信前取消、完了容量の予約、再送、ack、ID再使用、contextの全世代、deadline、失効後の受取待ちeffects、
JSON escapeを含むキューbyte上限を検査する。旧v1 component実行とG02の双方向codec fixtureも回帰対象。

形式モデルは[対応表](formal/senp-github-models.md)に従う抽象仕様の検査であり、
Win32 API・具体的なwire sequence・実メモリ上限の証明ではない。
