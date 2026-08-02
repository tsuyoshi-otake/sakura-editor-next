# 端末描画の固定ベースライン

この資料は、候補の端末レンダラーを実装する前の状態を、テスト専用の
オフスクリーン描画で固定するための手順です。ここで計測するコードは
`tests1.exe` にだけリンクされます。製品の端末描画コード、ウィンドウ、
スクリーンショット、画素ダンプは変更・生成しません。

## 計測範囲

`TerminalLegacyRendererBaseline` は、同じセル列・同じ矩形・同じ色を次の
5 つのバックエンドで描画します。

| 識別子 | 描画呼び出し |
| --- | --- |
| `legacy-gdi` | `ExtTextOutW`（隣接セルをまとめたラン） |
| `gdi-plus` | GDI+ `Graphics::DrawString` |
| `directwrite` | DirectWrite の明示的なフォールバック・スクリプト解析・グリフ整形を実行した後、レイアウトを DCRenderTarget へ描画 |
| `directwrite-d2d` | 現行の grouped `CreateTextLayout`/`DrawTextLayout` Direct2D 経路（明示的な整形段階は計測しない） |
| `candidate-native` | 製品の `TerminalRenderPlan`、組み込みグリフ、GDI fast path、遅延 `TerminalDWriteRenderer` |

入力コーパスは `ascii-shell`、`tui-box-block-shade`、`mixed-unicode` の
3 種類です。混在コーパスには、CJK の 2 桁セル、結合文字、絵文字の
サロゲートペア、肌色修飾子、リージョナルインジケーターを含めます。
UTF-16 コード単位数と表示占有桁数は別々に集計します。

既定の測定矩形は 120 列 × 8 行、DPI は 96/120/144/192、ウォームアップは
2 フレームです。フォントは `Cascadia Mono` を優先し、無い場合は
`Consolas` を使います。描画先は top-down 32-bit DIB のメモリだけです。

リソース計測の境界にも意味があります。`statsBefore` の前に 1×1 の DIB/DC
を一度だけ生成・破棄し、コンソールプロセスで遅延生成される GDI 会計用の
オブジェクトを基準値へ含めます。その後の `gdiObjectsDelta` はレンダラーが
保持したオブジェクトだけを表し、テストは必ず 0 を要求します。

各子プロセスは同じバックエンドを三つのインスタンスで実行します。最初の
インスタンスは `CreateBackend` の直後から `coldInitializationMs` を計測し、
1 回の描画を計測外の setup frame として実行します。終了後に残った GDI/Private
Bytes は `processGlobal*Delta` として記録します（フォントサービスや GDI+ の
プロセス共有キャッシュを隠さないためです）。2 番目の安定化用インスタンスは同じ setup frame を 2 回実行して破棄します。その後の 3 番目を測定対象とし、
その生成時間を `measurementInitializationMs`、ウォームアップと timed frame、
後始末を `privateBytes*` と `gdiObjects*` に記録します。測定側のリソース基準値は安定化 probe の終了後、全ての
バックエンドオブジェクトが閉じた状態で取得します。これにより初回の遅延
キャッシュ生成をリークとして扱いません。バックエンドは DIB/DC
を閉じる前に解放されるため、後者の `gdiObjectsDelta` は厳密に 0 でなければ
なりません。

## ビルドと一括計測

リポジトリのルートから実行します。

```text
rtk proxy cmd.exe /d /c build-sln.bat x64 Release
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-terminal-rendering.ps1 `
  -Configuration Release -Samples 10 -FramesPerSample 500 `
  -OutputDirectory .\tmp\terminal-rendering
```

`-OutputDirectory` は必須です。`-Samples` と `-FramesPerSample` は再現性と
実行時間のトレードオフを明示するための引数で、既定値はそれぞれ 10 と 500
です。スクリプトは 5 バックエンド × 3 コーパス × 4 DPI を各サンプルで
別プロセスとして起動します。各子プロセスには 120 秒の上限があり、終了を
確認してから次の組み合わせへ進みます。

出力先には次のファイルが作られます。

* `terminal-rendering-baseline-v1.json` — 全レコード、条件、中央値、p95、
  呼び出しカウンターを含む機械可読レポート。
* `terminal-rendering-baseline-v1.md` — 同じ実行の簡易表。
* `runs-v1-...\*.json` — 子プロセスごとの生レコード。

レポートの `comparative` は `true` です。先頭 4 backend は変更前の固定点で、
`candidate-native` は同じ条件で測る製品候補です。性能 gate は 10 sample ×
500 frame の結果で別途判定し、短い smoke run の値から優劣を結論しません。

## 単一ケースの実行

スクリプトを使わずに 1 ケースを確認する場合は、環境変数を設定して
`EmitsConfiguredBenchmarkJson` だけを実行します。

```powershell
$env:SAKURA_TERMINAL_BASELINE_OUTPUT = (Resolve-Path .\tmp).Path + '\terminal.json'
$env:SAKURA_TERMINAL_BASELINE_BACKEND = 'legacy-gdi'
$env:SAKURA_TERMINAL_BASELINE_CORPUS = 'mixed-unicode'
$env:SAKURA_TERMINAL_BASELINE_DPI = '144'
$env:SAKURA_TERMINAL_BASELINE_WARMUP_FRAMES = '2'
$env:SAKURA_TERMINAL_BASELINE_FRAMES = '20'
$env:SAKURA_TERMINAL_BASELINE_COLUMNS = '120'
$env:SAKURA_TERMINAL_BASELINE_ROWS = '8'
& .\x64\Debug\tests1.exe --gtest_filter=TerminalLegacyRendererBaseline.EmitsConfiguredBenchmarkJson
```

JSON の `schemaVersion` は 1 でなければなりません。条件、`frameDurationMs`
の長さ、必須カウンターが要求値と一致し、`available` が `true` であることを
確認します。GDI の後始末に失敗した場合は、テストが `gdiObjectsDelta != 0`
として失敗します。失敗時は生成物を削除して再試行するのではなく、該当する
RAII の所有者と子プロセスの終了状態を調べてください。

## カウンターの読み方

`legacy-gdi` は `extTextOutCalls` と `gdiBatchCount`、GDI+ は
`drawStringCalls`、両方の DirectWrite 経路は grouped run ごとの
`textLayoutCreates` と `d2dTarget*`/`d2d*Calls` を出力します。`directwrite`
は成功した `MapCharacters`、`AnalyzeScript`、`GetGlyphs`、
`GetGlyphPlacements` をそれぞれ `mapCharactersCalls`、`analysisCalls`、
`glyphsCalls`、`placementCalls` として数えます。`directwrite-d2d` は現行の
レイアウト専用経路なので、これらの明示的な整形・フォールバック・キャッシュ
カウンターは 0 です。カウンターは実装上の呼び出し回数を固定するためのもの
で、秒数の保証や画質の判定ではありません。

## クリーンアップ

スクリプトは各 `tests1.exe` を待機し、終了コードと JSON を検証します。途中で
中断した場合は、リポジトリパスを含む `tests1.exe` が残っていないことを確認し、
残ったプロセスを親から順に終了させてください。出力ディレクトリの JSON は
非機密の条件と集計だけを含め、実行環境固有の入力パスや画面画像を保存しないで
ください。

### Candidate-native comparative lane

The measurement driver also runs the `candidate-native` backend. It consumes
the production `TerminalRenderPlan`, `TerminalBuiltinGlyphRenderer`, and lazy
`TerminalDWriteRenderer` against the same top-down DIB, corpus, font, DPI,
warm-up, frame count, dimensions, and colors as the four frozen backends.
    ASCII candidate frames must leave all DirectWrite/Direct2D initialization,
    analysis, and draw counters at zero. The documented 10-sample × 500-frame
    invocation is the authoritative comparative acceptance gate; shorter runs are
    smoke/diagnostic checks only.
