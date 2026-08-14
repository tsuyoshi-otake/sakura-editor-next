# 形式検証 (TLA+ / TLC)

このディレクトリは、並行性の高いプロトコルを TLA+ でモデル化し、TLC で網羅的に
モデル検査するためのスペックを置く。スクリーンショットや単体テストでは到達で
きないインターリービング（プロセスの任意時点での死亡、PID 再利用など）を全数
探索することが目的である。

## ExtensionHostLease — 拡張ホストのエディタリースプロトコル

### 対象コード

develop `36f0c2550` 時点の以下を対象とする。

| モデル上のアクション | 実装 |
|---|---|
| `BeginAcquire` (check1) | `CControlTray.cpp` `MYWM_EXTENSION_HOST_ACQUIRE` の 1 回目 `IsRegisteredEditorLeaseOwner` |
| `CtrlAcquire*`（原子的） | `CExtensionHostController::AcquireLease`（`OpenProcess(SYNCHRONIZE)` ピン留め、死亡チェック、ネスト数、vault/broker 獲得と `RollbackAcquiredLease`） |
| `RecheckPass` / `RecheckFail` | `CControlTray.cpp:728` の 2 回目 `IsRegisteredEditorLeaseOwner`、失敗時 `ReleaseLease` |
| `ProcessRelease` | `MYWM_EXTENSION_HOST_RELEASE`（検証後 `ReleaseTrackedLease`） |
| `Tick` | `IDT_EXTENSION_HOST` タイマー → `ReclaimTerminatedEditorLeases` |
| `Die` / `Recycle` | OS によるプロセス終了と PID 再利用（世代カウンタ `proc[p].gen` で表現） |

並行性の根拠: トレイは単一スレッドのメッセージループなので、ハンドラ同士と
Tick は互いにインターリーブしない（コントローラ呼び出しを原子ステップにできる）。
一方 OS はどの機械語命令の間でもエディタプロセスを殺し、PID を再利用しうるので、
acquire ハンドラだけは check1 / AcquireLease / check2 の 3 ステップに分割し、
その間に OS アクションが割り込めるようにしてある。これは実装自身が
`CControlTray.cpp:726-728` のコメントで認めているレース窓と一致する。

### 検査する性質

| 性質 | 種別 | 主張 |
|---|---|---|
| `TypeOK` | 不変条件 | 型と有界性 |
| `ComponentBalance` | 不変条件 | broker / Secret Vault / コントローラのリース数が全ロールバック経路で一致し続ける |
| `HandleLedger` | 不変条件 | SYNCHRONIZE ハンドルは追跡中の所有者につき正確に 1 個開き、追跡終了時に正確に 1 回閉じる |
| `QuiescentAttribution` | 不変条件 | トレイが空いていて、かつピン留めされた所有者が生きている間、リース数はその世代が受けた許可数と一致する（死んだ所有者の残存は下の liveness が担保） |
| `NoForeignLeaseExtension` | アクション特性 | 追跡中の所有者のリース数は、ピン留めされた世代自身の要求でしか増えない — **PID 再利用は古いリースを延長できない** |
| `GrantsGoToPinnedGeneration` | アクション特性 | 完了した許可は必ず要求した世代自身にピン留めされる — **PID 再利用は新しい許可を横取りできない** |
| `DeadOwnerLeasesReclaimed` | liveness | ピン留めされたプロセスが死んだら、そのリースはいずれ必ず全回収される |

### 構成と結果 (2026-08-14, tla2tools 1.7.4 / OpenJDK 11.0.31)

| 構成 | フラグ | 結果 |
|---|---|---|
| `ExtensionHostLease_Current.cfg` | 現行設計（全防御有効）、2 PID × 3 世代 | **合格**。2,506,733 生成 / 841,670 到達状態、深さ 65、全性質成立（64 秒） |
| `ExtensionHostLease_NoPin.cfg` | `PinHandles = FALSE`（数値 PID で生存判定する仮想の旧設計） | **反例 10 ステート**: gen1 が獲得→解放せず死亡→PID が gen2 に再利用→gen2 の獲得要求が「所有者は生きている」と誤認され、死んだ gen1 のリースを延長（ABA）。`NoForeignLeaseExtension` 違反 |
| `ExtensionHostLease_NoRecheck.cfg` | `RecheckAfterPin = FALSE`（`CControlTray.cpp:728` の 2 回目検証なし） | **反例 6 ステート**: check1 通過→送信者死亡→PID 再利用→`OpenProcess` が傍観者プロセスをピン留めしたまま許可完了。`QuiescentAttribution` 違反 |

つまり `_main/CLAUDE.md` の Phase 4 チェックポイントが主張する
「recycled PID cannot inherit the former editor's lease」は、
**SYNCHRONIZE ハンドルのピン留めと、ピン留め後の再検証の両方が揃って初めて成立
する**ことを TLC が全数探索で確認した。どちらか一方を外した設計は、それぞれ
数ステートの具体的な反例を持つ。

### 実行方法

TLC は Java 11+ と `tla2tools.jar`
(<https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar>)
だけで動く。jar はリポジトリに置かず、`~/tmp/tla/` などに取得する。

```console
cd docs/formal
java -XX:+UseParallelGC -cp %USERPROFILE%\tmp\tla\tla2tools.jar tlc2.TLC ^
    -workers auto -metadir %TEMP%\tlc-meta ^
    -config ExtensionHostLease_Current.cfg ExtensionHostLease.tla
```

- 変種は `-config` を `_NoPin.cfg` / `_NoRecheck.cfg` に替える。反例が出ると
  終了コードは非 0（12 = 不変条件違反、13 = アクション特性違反）になる。
  これは欠陥ではなく期待結果である。
- `-metadir` を作業ディレクトリ外に向けないと、TLC が `states/` 等の一時
  ディレクトリをこのディレクトリに作る。
- 検査後は `java` プロセスが残っていないことを確認する。

### モデル化していないもの（意図的な抽象化）

- broker のホスト状態機械（Starting/Ready/quiesce/retry）と Secret Vault の
  activation CAS は「獲得が受理されるか拒否されるか」の非決定的分岐 1 個に
  畳んである。拒否分岐はすべて `RollbackAcquiredLease` 経路を通る。
- `kMaximumTrackedEditorLeaseOwners`(256) は小さな PID 集合では到達不能なので
  対象外。`kMaximumEditorLeasesPerOwner` は `CapPerOwner` として有界化。
- `Shutdown()` とハンドシェイク / host-lost フローは対象外（リース生存期間のみ）。
- メッセージ輸送はトレイの FIFO キュー。エディタは `SendMessage` でブロック
  するため、各世代の同時飛行メッセージは最大 1 通。送信者が死んでもメッセージ
  は配送されうる（保守的）。
- 世代数 `MaxGen` と PID 数は有限なので、これは有界モデル検査である。状態空間
  を広げたいときは cfg の定数を上げる。

### 検証で得た知見

`QuiescentAttribution` の初版は「追跡中のリースは常にピン留め世代の許可数と
一致する」と主張していたが、TLC が 8 ステートの正当な反例を返した:
エディタが解放を送信（この時点で自分の保持数を減らす）→ 処理前に死亡 →
トレイが古い解放メッセージを正しく破棄 → リース数 1 / 許可帰属 0 の残存状態。
これは実システムでも起きる正当な状態で、残存は次の Tick の
`ReclaimTerminatedEditorLeases` が回収する。不変条件は「ピン留めされた所有者が
生きている間」に弱め、残存の回収は liveness 特性側で担保する形に直した。
「安全性で常時成立を主張できるのはどこまでで、どこからが公平性つきの liveness
なのか」という境界が、この修正で明確になった。
