# PR #291 追加受入検証（2026-09-06）

対象は Search の失効・受付・preview、FileLoad の option/reader 寿命、
先行して公開した Terminal/Rust テスト修正である。全37項目や Clipboard の完了記録ではない。
各 receipt の source hash が実行対象を特定する。公開済み head
`01e1f25e90de833f032768e64d9929c378f53090` は Native Debug/Release、
Architecture、Cppcheck、MinGW Release、PR Gate が成功した。
ここで追加した `WM_PRINTCLIENT` を含む最終 head の CI、
main source checks、配布物 smoke と release-promotion は、公開時に別途確認する。

## 受付・MIME の負例

`search-acceptance-probe.json` と patch は実際の結果メッセージ受付と、
ReadBoxes 後の Replace All 受付に一つずつ矛盾した identity を注入する。
正常、generation、root、text、case、whole-word、regex、closed の8条件で、
到達回数・採用回数・実 list の行数・変更通知回数・実ファイル内容を検査した。
これは防御境界の故障注入であり、各矛盾状態が通常操作で到達可能という証明ではない。
generation/root/pattern/closed の各 guard を両受付箇所から外した4変異は、
いずれも2つの診断テストで検出した。計測用変更を除去した通常23テストも成功した。

MIME の option 設定を converter 作成後に戻す変異は、常設の
`MimeOptionIsAppliedOnEachOpenAndReopen` で失敗した。復元後は9/9成功。
常設の Search lifecycle/preview/FileLoad 回帰と、手動で再実行するこれらの
境界注入・変異試験は区別する。診断用の公開 C++ API は追加していない。

## 描画と入力

Search の通常 WM_PAINT は保持バッファへ描き、native surface に提出する。
追加した WM_PRINTCLIENT は借用 DC に現在の widget を描き、DC 状態を復元する。
提出・画面更新は行わない。既存実装が sentinel 色を残した診断回帰は
`search-print-client-red.xml`、修正後は `search-print-client-green.xml`。
この直接メッセージ試験は `replay/sakura-search-local-paint-test.txt` に収録し、
可視 fixture と一緒に実行する。通常 CI に含まれる常設テストではない。
PrintWindow に置き換えた試験は描画経路を検証できず、採用していない。

外部 PrintWindow(0/1/2) は今回のキャリブレーションで、WM_PRINTCLIENT だけに
入れた色変更を検出しなかった。flags=2 は入力欄背景にも断続的な差を生じた。
そのため、外部 PrintWindow の差分0を独立した現在描画の証拠には採用しない。
失敗した計測を PASS に変更したり、許容値を広げたりしていない。
`capture-calibration/` に棄却した receipt を残した。

校正した方法では、実 CSearchWorkbenchTool を使う可視の native fixture 内で
WM_PRINT(PRF_CLIENT|PRF_ERASEBKGND|PRF_CHILDREN) を送り、所有 DIB に現在の描画を保存する。
別プロセスからの CopyFromScreen と PrintWindow(2) も保存する3経路の比較である。
外部 DC を製品 HWND に渡す代わりに、fixture 内で DC と描画を所有させた。
原画・現在描画・差分画像の見本と全30試行の値を `paint/` に収録した。
これは native view の試験であり、実エディターの Folder を閉じる window transition
全体の試験ではない。

入力クリア、native EDIT の debounce 入力、SetRoot による root 解除を各10回実行。
毎回、実検索で2行を表示してから0行へ失効したことを確かめる。
描画は通常 dispatch し、debounce timer の dispatch だけを capture handshake 中保留する。
force-redraw 前後で別々に現在描画を採り、同じ閾値（noise <=0.5%、超過 <=0.05pp）を適用した。
25点の遮蔽確認、カーソル退避、bounded event、close/retirement、所有 process 残存0も確認した。

| 構成 | 試行 | 最大差分 / noise | 入力中央値 / p95 / 最大 | Close + retirement |
|---|---:|---:|---:|---:|
| Debug | 30 | 0% / 0% | 0.648 / 1.139 / 1.552 ms | 16 ms |
| Release | 30 | 0% / 0% | 0.502 / 0.752 / 0.815 ms | 0 ms（tick 分解能内） |

`paint-canary/` は同じ current-render 経路だけを magenta にした負例。
画面は元の色を保ち、比較は30/30で約12.48%の差を検出して不合格になる。
子 control が描き直す領域を除く親 widget が変わるため、全画面が magenta になる試験ではない。
検出用変更は byte hash 一致で復元し、通常 solution を再ビルドした。
入力時間は同期 dispatch の時間であり、自然な debounce 待ち時間や全 UI latency ではない。

## 性能

基準を測定前に定め、Release の比較版/修正版を5組の独立 process で交互に実行した。
8 MiB の固定総量を1/2/4/8 prepared readersで読み、reader数だけ全量を複製しない。
比較版は生存する親の converter を借用する所有権コストだけの再構成であり、
同じ8-slot bookkeepingを両版へ加えている。読み取りは直列で、旧 FileLoad 全体や
並列 throughput の比較ではない。修正版は親を含め P+1 個の converter を作る。

| 対象 | 中央値の変化 | p95 の変化 |
|---|---:|---:|
| Read P=1 | -3.73% | -21.84% |
| Read P=2 | -2.93% | -3.09% |
| Read P=4 | +0.89% | +9.57% |
| Read P=8 | +3.33% | +1.96% |
| Preview end-to-end | -6.15% | +7.04% |

P=8 の準備中央値は2.8 μs（基準10 ms以内）。scanの事前上限は中央値+15%、p95+30%。
プロセス内の反復をまとめた後に5標本を集計しており、5標本のp95は最大値となる。
統計的有意差や他のマシンでの保証は主張しない。旧 preview は遠方 hit を欠落させる
既知の不正な出力であり、速度を理由に採用可能な代替ではない。
修正版の出力上限250 UTF-16単位と元のhit座標を検査した。
生CSV、XML、source/exe hash、集計値は `performance/`。
測定用変更を除去した通常 Release solution と当時の関連24 testsも成功した。
最終構成では常設22 testsが Debug/Release とも成功し、所有 process は0件。
一時診断テストの追加・除去で件数が異なるため、各 XML の一覧を実行範囲とする。

## 再実行

`replay/` は実行済みのローカル診断 harness を保存したもの。
通常 CI の構成に偽の backend や test hook を加えるものではない。
`prepare_replay.py --checkout <disposable-checkout> --destination <directory-under-tmp>`
は固定のローカルパスを指定 checkout/tmp に置換して harness をコピーするだけで、実行はしない。
標準の Windows/MSVC/vcpkg 環境が必要で、canary の画像検査には Pillow を用いる。
依存 package の取得・更新は行わない。同じ checkout で他のビルド・テストと同時に実行しない。

生成先の `sakura-search-acceptance-probe.py`、`sakura-release-mime-mutation.py`、
`sakura-release-performance.py`、`sakura-search-local-paint-build.py`、
`sakura-search-local-paint-release-build.py`、`sakura-local-paint-canary.py` をそれぞれ
`py -3 <script>` で実行する。変異を戻す finally と通常ビルドを含む。
performance は同じ出力 exe が既にあれば明示的に停止するため、別 checkout の初回実行を使う。
常設テストは通常 solution build 後、保存した bounded runner を
`-Configuration Debug` / `Release`、
`-Filter SearchRequestSafetyTest.*:FileLoadOptionsTest.*:SearchWorkbenchToolGeometry.*`
で実行する。手動診断の成功を、通常 CI が同じ変異まで実行したと読み替えない。
