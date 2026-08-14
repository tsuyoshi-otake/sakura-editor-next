# 形式検証 (TLA+ / TLC)

このディレクトリは、並行性の高いプロトコルを TLA+ でモデル化し、TLC で網羅的に
モデル検査するためのスペックを置く。スクリーンショットや単体テストでは到達で
きないインターリービング（プロセスの任意時点での死亡、PID 再利用など）を全数
探索することが目的である。

| モデル | 対象プロトコル |
|---|---|
| [`ControlStartupHandshake`](#controlstartuphandshake--コントロールプロセス起動ハンドシェイク) | コントロールプロセスのシングルトン化と ready イベント公開 |

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
