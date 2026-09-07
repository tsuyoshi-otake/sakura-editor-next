# SENP v2 runtime session（#296）

G03aはv2の要求・応答・終了状態を実装する。Win32 workerとの接続はG03b、
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

G03aの受入検査は`SenpRuntimeLifecycle.*`（11件）とRustの`effect_session`（6件）。
送信前取消、完了容量の予約、再送、ack、ID再使用、contextの全世代、deadline、失効後の受取待ちeffects、
JSON escapeを含むキューbyte上限を検査する。旧v1 component実行とG02の双方向codec fixtureも回帰対象。

形式モデルは[対応表](formal/senp-github-models.md)に従う抽象仕様の検査であり、
Win32 API・具体的なwire sequence・実メモリ上限の証明ではない。
