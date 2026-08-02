# 端末描画ベンチマーク・レポート例

これは `terminal-rendering-baseline-v1.json` の形を示すための、非機密で
合成した短い例です。数値は実機の性能や受入基準を表しません。実際の計測では
`tools/measure-terminal-rendering.ps1` が 5 バックエンド × 3 コーパス × 4 DPI
（指定したサンプル数）の全レコードを出力します。

## JSON の例（1 レコードの抜粋）

```json
{
  "schemaVersion": 1,
  "measurement": "terminal-rendering-comparative-baseline",
  "comparative": true,
  "configuration": "Release",
  "conditions": {
    "warmupFrames": 2,
    "framesPerSample": 20,
    "samples": 2,
    "dpis": [96, 120, 144, 192],
    "columns": 120,
    "rows": 8,
    "preferredFont": "Cascadia Mono",
    "fallbackFont": "Consolas",
    "surface": "top-down 32-bpp DIB memory only"
  },
  "backends": ["legacy-gdi", "gdi-plus", "directwrite", "directwrite-d2d", "candidate-native"],
  "corpora": ["ascii-shell", "tui-box-block-shade", "mixed-unicode"],
  "requiredCounters": [
    "frames", "extTextOutCalls", "gdiBatchCount", "drawStringCalls",
    "textLayoutCreates", "fallbackRuns", "fallbackCacheHits",
    "fallbackCacheMisses", "mapCharactersCalls", "analysisCalls", "glyphsCalls",
    "placementCalls", "d2dTargetCreates", "d2dTargetBinds",
    "d2dDrawCalls", "d2dEndDrawCalls", "d2dTargetLosses"
  ],
  "summary": [
    {
      "backend": "legacy-gdi",
      "corpus": "mixed-unicode",
      "dpi": 144,
      "sampleCount": 2,
      "frameCountPerSample": 20,
      "frameMedianMs": 0.42,
      "frameP95Ms": 0.61,
      "coldInitializationMedianMs": 0.03,
      "measurementInitializationMedianMs": 0.02,
      "privateBytesDeltaMedian": 65536,
      "gdiObjectsDeltaMedian": 0,
      "processGlobalGdiObjectsDeltaMedian": 1,
      "counters": {
        "framesMedian": 20,
        "extTextOutCallsMedian": 128,
        "gdiBatchCountMedian": 128,
        "drawStringCallsMedian": 0,
        "textLayoutCreatesMedian": 0,
        "fallbackRunsMedian": 0,
        "fallbackCacheHitsMedian": 0,
        "fallbackCacheMissesMedian": 0,
        "mapCharactersCallsMedian": 0,
        "analysisCallsMedian": 0,
        "glyphsCallsMedian": 0,
        "placementCallsMedian": 0,
        "d2dTargetCreatesMedian": 0,
        "d2dTargetBindsMedian": 0,
        "d2dDrawCallsMedian": 0,
        "d2dEndDrawCallsMedian": 0,
        "d2dTargetLossesMedian": 0
      }
    }
  ],
  "records": [
    {
      "schemaVersion": 1,
      "available": true,
      "unavailableReason": "",
      "backend": "legacy-gdi",
      "corpus": "mixed-unicode",
      "conditions": {
        "dpi": 144,
        "columns": 120,
        "rows": 8,
        "cellWidth": 11,
        "cellHeight": 22,
        "fontPixelHeight": 18,
        "warmupFrames": 2,
        "frameCount": 20,
        "fontFamily": "Cascadia Mono"
      },
      "corpusStats": {
        "cellCount": 1610,
        "utf16CodeUnits": 1884,
        "occupiedColumns": 1920
      },
      "timings": {
        "coldInitializationMs": 0.031,
        "measurementInitializationMs": 0.021,
        "frameDurationMs": [0.39, 0.42, 0.44]
      },
      "resources": {
        "processGlobalPrivateBytesBefore": 8000000,
        "processGlobalPrivateBytesAfter": 12000000,
        "processGlobalPrivateBytesDelta": 4000000,
        "processGlobalGdiObjectsBefore": 1,
        "processGlobalGdiObjectsAfter": 2,
        "processGlobalGdiObjectsDelta": 1,
        "privateBytesBefore": 8000000,
        "privateBytesAfter": 8065536,
        "privateBytesDelta": 65536,
        "gdiObjectsBefore": 1,
        "gdiObjectsAfter": 1,
        "gdiObjectsDelta": 0
      },
      "counters": {
        "frames": 20,
        "extTextOutCalls": 128,
        "gdiBatchCount": 128,
        "drawStringCalls": 0,
        "textLayoutCreates": 0,
        "fallbackRuns": 0,
        "fallbackCacheHits": 0,
        "fallbackCacheMisses": 0,
        "mapCharactersCalls": 0,
        "analysisCalls": 0,
        "glyphsCalls": 0,
        "placementCalls": 0,
        "d2dTargetCreates": 0,
        "d2dTargetBinds": 0,
        "d2dDrawCalls": 0,
        "d2dEndDrawCalls": 0,
        "d2dTargetLosses": 0
      }
    }
  ]
}
```

`records` の `frameDurationMs` は実際には `framesPerSample` 個、`summary` は
各バックエンド・コーパス・DPI の組み合わせごとに 1 件になります。この抜粋
では読みやすさのため 1 件だけを示しています。

## 解釈上の注意

* `comparative: true` は、変更前の 4 backend と `candidate-native` を同じ条件で
  比較するレポートであることを示します。文書化された 10 サンプル × 500 フレームの
  実行が権威ある比較受入 gate で、短い smoke run は診断用です。
* `gdiObjectsDelta` は 0 が必須です。`gdiObjectsBefore` が 1 なのは、測定前の
  1×1 DIB/DC 準備で遅延 GDI 会計を基準化したためです。
* `coldInitializationMs` と `processGlobal*` は、setup frame を含む最初の
  コールドインスタンスの観測値です。`measurementInitializationMs` と
  `privateBytes*`/`gdiObjects*` は、後続の測定インスタンスに対応します。
  後者のリソース基準値は、別の安定化用 setup インスタンスを破棄した後に
  取得されます。
* `directwrite` の `mapCharactersCalls`、`analysisCalls`、`glyphsCalls`、
  `placementCalls` は成功した DirectWrite API 呼び出しだけを数えます。
  grouped layout-only の `directwrite-d2d` では、これらと fallback/cache
  カウンターは 0 になります。
* `privateBytesDelta` や各時間は OS、フォント、ビルド、負荷に依存する観測値で、
  数値そのものを固定するものではありません。
* カウンターは呼び出し経路の回帰を検出するために保存します。画素一致や表示の
  美しさを検証する画像テストではありません。

The comparative report includes a fifth `candidate-native` backend. Its ASCII
records must show zero `d2dTargetCreates`, `d2dTargetBinds`, `d2dDrawCalls`,
`d2dEndDrawCalls`, and explicit DirectWrite analysis counters; shaped records
map the production renderer counters into the same versioned schema.
