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

Issue #272 introduces the prerequisite stable provider boundary without changing
authority. Provider-neutral DTOs and validation live in `OutputServiceTypes.h`,
and `IOutputService` carries all nine mutations, copied snapshots, advisory
subscription, and Stop/drain semantics. `IWorkbenchRuntime`, `CEditWnd`, and the
Git Output adapter borrow only that interface; the runtime still owns one
concrete C++ `OutputService`, publishes the same identity only while Ready, and
retains the stopped object until runtime destruction so existing borrows never
dangle. The accepted-commit feed intentionally remains outside the production
interface as a migration-only observation contract.

Issue #273 completes the first production-capable cutover boundary. The runtime
selects exactly one `IOutputService` provider before Ready and retains that same
object through Stop and runtime destruction. C++ remains the default. An
explicit `SAKURA_OUTPUT_BACKEND_RUST` build selects the Rust authority and fails
closed when its ABI or initialization is unavailable; it never creates a C++
authority as a fallback. The observational accepted-commit candidate is attached
only when C++ is authoritative and is not part of the Rust production path.

The Rust provider owns channels, owner generations, operation replay, revisions,
and snapshots behind one opaque numeric token in the existing native Rust
static library. The C++ adapter owns only copied ABI conversion, diagnostics,
and the shared provider-neutral advisory notification dispatcher. The ABI is
callback-free, validates every fixed-width descriptor, contains panics at every
export, and never retains a foreign pointer. The provider selector is immutable
for a lifecycle: dual writes, per-call health probing, silent fallback,
snapshot-based state transfer, and in-place provider swaps after an accepted
mutation remain forbidden.

Issue #274 adds provider-neutral, payload-free health evidence without turning
health into a fallback selector. `IOutputService::Health()` reports the selected
kind, factory and lifecycle state, initialization and ABI boundaries, retained
terminal fault, last typed operation result, revision, and saturating counters.
Advisory listener exceptions and queue drops are counted separately and never
become authority faults. Test creator overrides are explicitly marked so their
runs cannot be mistaken for production evidence.

The Rust provider keeps one stopped object alive for existing runtime borrows.
A successful terminal Stop caches its stopped snapshot, destroys the opaque Rust
token exactly once, and continues to answer typed stopped operations without
state transfer. A destroy failure retains the original fault and ownership for
a bounded external Stop retry; it never selects or reconstructs the C++
provider. Runtime health overlays only immutable composition metadata and does
not infer provider state from RTTI, snapshots, or the observational candidate.

The Rust model keeps at most one canonical encoded snapshot per provider
revision. Measure and write still cross the frozen receipt boundary and still
reject stale or forged receipts; the cache only removes repeated serialization
of the same Rust-owned state. Copied mutation entry and terminal Stop invalidate
the cache before any fallible state change, so panic containment cannot expose
bytes from an earlier revision. The C++ adapter continues to allocate and decode
one caller-owned buffer and moves that decoded value to the consumer; no cached
foreign pointer or decoded C++ authority state crosses back into Rust.

Issue #274 remains a measurement gate, not an adoption decision. C++ is still
the default Output authority. `SAKURA_OUTPUT_PRODUCTION_PACKAGE` is an explicit
package-release gate independent from UTF-16 packaging and currently accepts
only the C++ Output backend; comparison builds may select Rust without enabling
that gate. The paired provider benchmark uses independently built and hashed
test executables, a fixed verified processor-affinity mask, interleaved runs,
provider health validation, semantic digests, and payload-free evidence. No
default or production-package flip is allowed until the remaining startup,
incremental-build, native-link closure, AMD/Intel, and required toolchain cells
are complete.

`SAKURA_OUTPUT_BACKEND_RUST` is deliberately independent of
`SAKURA_UTF16_BACKEND_RUST` and the SIMD ISA dispatcher. The Output model is a
stateful authority with lifecycle and revision semantics; SIMD remains an
internal stateless kernel concern. They share only the one-staticlib native Rust
transport and must be selected, tested, and rolled back independently.

`sakura_core/workbench/scm/GitOutputChannel.h`/`.cpp` (Issue #221) is this
service's first real production content producer: an HWND-free adapter that
mirrors `workbench::scm::RunGit` invocations into a "Git" `Log` channel,
reproducing upstream VS Code's own built-in Git extension Output channel
format. It is documented in `sakura_core/workbench/scm/CLAUDE.md`'s "Git Output
Channel" section rather than here, since its format contract and divergences
are SCM-specific; this file records only that the producer exists and that it
adds no new `OutputService` invariant. It is not yet wired into any production
`RunGit` call site (`CEditWnd.cpp`, `CScmWorkbenchTool.cpp`).
