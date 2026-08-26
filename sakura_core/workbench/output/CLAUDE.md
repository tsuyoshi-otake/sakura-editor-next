# Phase 6 Output Service Guidance

## Ownership

`OutputService` is the process-local, HWND-free authority for Output and
structured Log channels. Native panel tabs and extension output-channel RPC are
projections/adapters; neither owns channel content, active-channel selection, or
owner lifetime.

## Channel and Operation Rules

- Output and Log channels are distinct kinds. Plain text is retained only by an
  Output channel; structured log entries remain typed and receive a separate,
  bounded deterministic text projection for the current native view.
- One owner generation may create multiple channels. A newer generation
  atomically fences and replaces the older generation; exact old-generation
  disposal cannot remove newer channels.
- Every mutation carries a bounded stable operation ID and optional expected
  service revision. Exact replay returns the remembered result; reuse with
  different input is a conflict.
- Bound channels, mutation payloads, retained UTF-8 text, log entries,
  remembered operations, owner identities, subscriptions, and pending
  notifications. Truncation must preserve UTF-8 character boundaries and
  report the dropped amount; drop counters saturate rather than wrap.
- Show/hide and `preserveFocus` are model metadata. A Win32 adapter applies them
  after reading one accepted snapshot; showing Output does not imply stealing
  editor focus. Every fresh `show` operation publishes reveal intent even when
  that channel is already visible; only an exact operation-ID replay suppresses
  the duplicate mutation/notification.
- Deliver committed notifications outside the model lock and contain listener
  faults. An external `Stop` waits for callbacks to drain; a callback-originated
  Stop returns its typed deferred result and is finalized by the safe outer
  delivery boundary. Destruction from inside a callback is unsupported because
  the callback still borrows the service. Terminal Stop clears
  channels/listeners and rejects later work.

## Verified Checkpoint

`CWorkbenchRuntime` owns `OutputService`; `CEditWnd` renders authoritative
channel snapshots through a coalesced UI-thread projection; and extension
Output RPC mutates the service before best-effort legacy cache projection.
Native channel selection is model-first with exact owner/generation, expected
revision, a stable non-wrapping operation ID, and `preserveFocus`.

The canonical `workbench.action.output.toggleOutput` command remains the user
toggle, while extension `OutputChannel.show` is a non-toggle reveal. Every
extension-host mutation carries one bounded session-scoped operation ID and
reuses it for transport replay. The integrated native cohort passes 210/210 and
the extension-host API cohort passes 15/15.

## Rust Migration Boundary

Issue #270's first Rust candidate is replay-only and test-owned. It receives
copied requests through the existing one-staticlib C ABI and returns copied
snapshots; it owns no callback, UI, filesystem, process, transport, cache, or
production notification path. `CWorkbenchRuntime::m_output` and the C++
`OutputService` remain the only production authority.

Issue #271 adds the prerequisite ordered accepted-commit feed. Snapshot and
cursor bootstrap are atomic; only fresh successful mutations enter the copied
stream. A bounded lag or allocation failure becomes a terminal explicit gap,
and Stop drains already accepted commits before its terminal stopped event.
Feed callbacks run outside the model lock, are exception-contained, and share
the service's external-drain/reentrant-deferred lifetime fence. The advisory UI
notification queue and its drop counter remain a separate best-effort path.
With no feed consumer, the service advances only the cursor and retains no
request DTOs; caught-up consumers also release journal entries immediately.

Issue #271 now attaches the replay-only Rust model to this feed as an
observational candidate when `SAKURA_UTF16_RUST_CANDIDATE` is compiled. Runtime
composition constructs it immediately after the authoritative `OutputService`
with the exact same limits and before any mutation can be published. It accepts
only the fresh empty revision-one/cursor-zero bootstrap, applies copied future
commits synchronously, and exposes copied diagnostics plus an explicit on-demand
snapshot comparison. It does not publish state, operation results,
notifications, or fallback behavior to any production consumer.

The candidate has explicit unavailable, attaching, live, faulted, and stopped
states. A subscribe failure, cursor mismatch, bounded-feed gap, FFI/result
mismatch, callback failure, or snapshot mismatch is terminal: it detaches,
destroys the Rust model, and never resynchronizes or replays from the C++
snapshot. Runtime shutdown stops `OutputService` first so its feed callback
lifetime is fenced, then destroys the candidate token. Without the candidate
macro the same composition surface reports typed unavailable state and has no
Rust symbol reference.

A production cutover still requires a stable provider boundary because native
code currently borrows the concrete `OutputService`. That boundary must select
one authority for an entire lifecycle; it may not be approximated with dual
writes, silent fallback, snapshot-based state transfer, or an in-place provider
swap after an accepted mutation.

`sakura_core/workbench/scm/GitOutputChannel.h`/`.cpp` (Issue #221) is this
service's first real production content producer: an HWND-free adapter that
mirrors `workbench::scm::RunGit` invocations into a "Git" `Log` channel,
reproducing upstream VS Code's own built-in Git extension Output channel
format. It is documented in `sakura_core/workbench/scm/CLAUDE.md`'s "Git Output
Channel" section rather than here, since its format contract and divergences
are SCM-specific; this file records only that the producer exists and that it
adds no new `OutputService` invariant. It is not yet wired into any production
`RunGit` call site (`CEditWnd.cpp`, `CScmWorkbenchTool.cpp`).
