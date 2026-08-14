# 形式検証 (TLA+ / TLC)

このディレクトリは、並行性の高いプロトコルを TLA+ でモデル化し、TLC で網羅的に
モデル検査するためのスペックを置く。スクリーンショットや単体テストでは到達で
きないインターリービング（プロセスの任意時点での死亡、PID 再利用など）を全数
探索することが目的である。

| モデル | 対象プロトコル |
|---|---|
| [`ExtensionHostLease`](#extensionhostlease--拡張ホストのエディタリースプロトコル) | 拡張ホストのエディタ PID リース（ピン留め・再検証・回収） |
| [`ControlStartupHandshake`](#controlstartuphandshake--コントロールプロセス起動ハンドシェイク) | コントロールプロセスのシングルトン化と ready イベント公開 |

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

- 他の構成は `-config` を対象の `.cfg` に替える（`.tla` 本体も対応するモデル名に
  合わせる）。反例が出ると終了コードは非 0（12 = 不変条件違反、13 = アクション
  特性違反）になる。変種構成では欠陥ではなく期待結果である。
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

## ControlStartupHandshake — コントロールプロセス起動ハンドシェイク

### 対象コード

develop `36f0c2550` 時点の以下を対象とする。

| モデル上のアクション | 実装 |
|---|---|
| `LWarm` / `LCold` | `CProcessFactory::IsExistControlProcess` (`CProcessFactory.cpp:163-176`) — CP ミューテックスの `OpenMutex` |
| `LCreateEv` / `LBusy` | `StartControlProcess` の `CreateEvent`。`ERROR_ALREADY_EXISTS` は「ビジー」失敗 (`CProcessFactory.cpp:202-206`) |
| `LSpawn` / `LWake*` | `CreateProcess(-NOWIN)` と `WaitForMultipleObjects({event, child}, 15s)` — イベントのみが成功、子プロセス死亡とタイムアウトは明示的失敗 |
| `COpenEv` | `CControlProcess::InitializeProcess` 冒頭の `OpenEvent` |
| `CMuxWin` / `CMuxLose` | CP ミューテックスの `CreateMutex`。`ERROR_ALREADY_EXISTS` なら敗者は即退出 |
| `CShared` → `CPlatOk`/`CPlatFail` → `CTrayPub` → `CSignal` | 共有メモリ初期化 → プラットフォームランタイム開始（失敗は fail closed）→ トレイ作成と `m_hwndTray` 公開 → 最後に `SetEvent` (`CControlProcess.cpp:141-274`) |
| `CDie` | 任意時点のプロセス死亡（カーネルがハンドルを閉じ、名前付きオブジェクトが消える） |

名前付きカーネルオブジェクトは「生きたプロセスがハンドルを保持している間だけ
名前が存在し、全保持者が消えた後の `CreateEvent` は新しい未シグナル世代を作る」
というライフタイム意味論でモデル化している。ランチャーが撤退してもコントロール
が `OpenEvent` 済みならイベント名は生き続ける、という実挙動がそのまま出る。

### 検査する性質と結果 (2026-08-14, tla2tools 1.7.4 / OpenJDK 11.0.31)

| 構成 | フラグ | 結果 |
|---|---|---|
| `ControlStartupHandshake_Current.cfg` | 現行設計、ランチャー 2 + コントロール 3 + 直接 `-NOWIN` 起動 1 | **合格**。324,388 生成 / 91,087 到達状態、深さ 31、全性質成立（3 秒） |
| `ControlStartupHandshake_SignalEarly.cfg` | `SignalAfterReady = FALSE`（ミューテックス獲得直後にシグナルする仮想設計） | **反例 7 ステート**: イベント作成→コントロール起動→ミューテックス獲得→トレイ未公開のまま `SetEvent`。`SignalOnlyAfterTrayPublished` 違反 — `CControlProcess.cpp:195-198` が禁じる「中間状態の広告」 |
| `ControlStartupHandshake_WarmWindow.cfg` | 現行設計 + 意図的に強すぎる不変条件 `WarmImpliesSawTray` | **反例 7 ステート**（欠陥ではなく到達可能性の証明書）: 1 人目のランチャーが起動したコントロールがミューテックスを獲得した直後（トレイ未公開）、2 人目のエディタが「コントロール有り」と判定し未 Ready のまま続行。マルチワーカー実行ではトレース形状が揺れ、タイムアウト撤退で取り残された孤児コントロール経由の 8 ステート版が報告されることもある |

成立した主性質:

- `AtMostOneSurvivor` / `MuxConsistency` — 複数コントロールが同時起動しても、
  CP ミューテックスを最初に獲得した 1 個だけが生き残る
  (`CProcessFactory.cpp:72-77` のコメントの主張)。
- `SignalOnlyAfterTrayPublished` / `LauncherOkImpliesTray` — ready イベントは
  プラットフォーム Running とトレイ公開の後にしかシグナルされず、
  `StartControlProcess` 成功時には `m_hwndTray` が必ず公開済み。
- `EveryWaitResolves` (liveness) — ランチャーの有界待ちは必ず成功・タイムアウト・
  子プロセス死亡のいずれかに決着する。

`WarmWindow` が証明書化した窓は実設計が許容しているもの: ミューテックスは
コントロール初期化の最初期に作られるため、`IsExistControlProcess` 経由の
エディタは ready を待たずに進む。トレイ依存メッセージは各所で fail closed し、
実エディタ起動はその後の platform Hello でゲートされるため欠陥ではないが、
「窓がどこで開くか」の機械的証明として保持する。

### モデル化していないもの（意図的な抽象化）

- エディタ側 `GSTR_EVENT_SAKURA_EP_INITIALIZED`（トレイ→エディタ起動）の
  ハンドシェイクは別プロトコルであり対象外。
- 共有メモリ作成は両プロセスとも先着者初期化で、ここで意味を持つのは
  `m_hwndTray` 公開のみ（`tray` ブール 1 個に畳んだ）。
- 敗者コントロールの CP ミューテックスハンドルは即閉じ（デストラクタ）とし、
  ミューテックス名は「勝者が生きている間だけ存在」とした。
- ランチャープロセス自体の死亡、共有メモリのバージョン不一致失敗、放棄された
  初期化ミューテックスの回復 (`MYWM_RECOVER_APPNODE`) は対象外。

## 検証で得た知見

### ExtensionHostLease: 安全性と liveness の境界

`QuiescentAttribution` の初版は「追跡中のリースは常にピン留め世代の許可数と
一致する」と主張していたが、TLC が 8 ステートの正当な反例を返した:
エディタが解放を送信（この時点で自分の保持数を減らす）→ 処理前に死亡 →
トレイが古い解放メッセージを正しく破棄 → リース数 1 / 許可帰属 0 の残存状態。
これは実システムでも起きる正当な状態で、残存は次の Tick の
`ReclaimTerminatedEditorLeases` が回収する。不変条件は「ピン留めされた所有者が
生きている間」に弱め、残存の回収は liveness 特性側で担保する形に直した。
「安全性で常時成立を主張できるのはどこまでで、どこからが公平性つきの liveness
なのか」という境界が、この修正で明確になった。
