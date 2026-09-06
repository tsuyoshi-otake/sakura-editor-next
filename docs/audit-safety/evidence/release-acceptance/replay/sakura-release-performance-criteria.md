# Release acceptance measurement protocol (declared before measurement)

## Search native painting and input

Use the actual CSearchWorkbenchTool with a generated temporary workspace and no editor profile. A fresh tests1 process owns one visible native view and parent window; it must destroy them and drain worker retirement. No user workspace or profile is read.

Repeat clear, native EDIT debounce input, and root-to-empty changes 10 times each (30 trials). Before every gesture prove the real result list contains a file header and a match, and after it prove zero rows. Between gesture and capture, service painting while holding WM_TIMER dispatch so the debounce state remains under observation. This measures that state; it does not measure a natural timer firing distribution.

Compare Graphics.CopyFromScreen with PrintWindow(2), with cursor outside and a 5x5 WindowFromPoint/GetAncestor occlusion grid. Save both images and the difference heatmap. Force RedrawWindow(ERASE|INVALIDATE|ALLCHILDREN|UPDATENOW) and repeat to measure capture noise. A valid trial has noise <=0.5% and original difference <= noise+0.05 percentage points. Higher noise invalidates the measurement; do not reinterpret it as a pass.

Observe synchronous input dispatch with QPC separately from capture and test runtime. Accept median <=50ms, p95 <=100ms and maximum <=250ms. Actual Close plus retirement drain must finish <=2000ms. These are machine-specific upper budgets; no cross-machine guarantee.

## Read and preview comparison

Same checkout, toolchain, Release configuration, generated fixed corpus and cache-warm conditions. Compare a documented in-place reconstruction of the old cost with fixed source; retain exact source hashes and restore bytes. Run at least five independent process pairs and reverse A/B order between pairs. Compute metrics across process samples, not corpus rows as independent observations.

FileLoad: fixed 8 MiB UTF-8 corpus, 1/2/4/8 prepared readers scanning a fixed total byte count (not duplicating work per reader). Measure preparation time and scan time separately, verify total lines/UTF-16 units. Record converter creation count; expect O(P), one converter per reader in fixed code. O(P) setup allocation is intentional for independent mutable converter ownership. Reject median scan regression >15% or p95 >30%. Preparation must stay below 10ms median at P=8; investigate any higher cost before accepting.

Preview: fixed corpus with sparse hits and one long line, verify hit/source-coordinate contracts in fixed code. The old preview omits distant hits, so its output is known incorrect; speed is a diagnostic comparison, never a reason to accept the old output. Expect output storage bounded to <=250 UTF-16 units per match, no allocation proportional to full line length in BuildPreview itself, and median end-to-end scan regression <=15% (p95 <=30%) over five process pairs.

All launched processes have timeouts and exact ownership cleanup. A measurement failure must be investigated and recorded; do not widen these thresholds after seeing a failure merely to obtain green results.
