# Terminal Runtime Test Guidance

## Scope

- Keep runtime authority, topology, input batching, and capture-index tests independent of HWND, ConPTY, and the workbench UI.
- Use deterministic fake sessions or callbacks for lifecycle tests. Real backend and cross-process coverage belongs under `src/test/integration/`.

## Invariants

- Runtime, session, window, pane, and instance identifiers are stable identities and are never reused within one runtime generation.
- Every lifecycle branch reaches one explicit terminal outcome. Late callbacks must not mutate a replacement instance.
- Topology mutations preserve unique IDs, positive split weights, and valid active window/pane references.
- Input validation is all-or-nothing: a rejected batch commits no bytes.
- Capture history is bounded. A stale cursor reports a typed gap/resync result instead of guessing or returning unrelated content.

## Verification

- Run each new suite in isolation with `tests1.exe --gtest_filter=<Suite>.*` before relying on the combined smoke filter.
- Follow the process-cleanup requirements in `src/test/CLAUDE.md` after every automated test run.
