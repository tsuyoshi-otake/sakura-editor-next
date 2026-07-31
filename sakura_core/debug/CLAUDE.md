# P3 Debug Backend Guidance

This directory owns diagnostics utilities and, for Issue #6 P3, the Debug
Adapter Protocol/session backend. Keep startup tracing independent from DAP.

Debug sessions own adapter process/RPC lifetime, breakpoints, stack/variables,
evaluation, cancellation, and teardown. DEBUG CONSOLE is a view over the
session REPL and does not own the adapter. Every request and session transition
has one terminal result; stale session generations cannot update a newer
session. Launch configuration comes through the configuration/workspace
service, including trust and variable resolution.

## Scoped Phase 6 Guidance

- Launch catalog ownership and last-good source fencing:
  [`launch/CLAUDE.md`](launch/CLAUDE.md)
- Strict DAP framing and pure session ownership:
  [`dap/CLAUDE.md`](dap/CLAUDE.md)
- Debug Console session/transcript/evaluation state:
  [`console/CLAUDE.md`](console/CLAUDE.md)

The Launch catalog, DAP codec, injected-transport DAP session, and pure Debug
Console foundations are verified. They deliberately do not claim adapter
discovery/process launch, production pipe transport, initialize/launch
sequencing, breakpoint/stack/variable models, the DAP-to-evaluation adapter,
runtime composition, or native Debug Console/Run and Debug compatibility. Keep
those layers separate so transport failure and UI disposal cannot become
accidental session terminals.
