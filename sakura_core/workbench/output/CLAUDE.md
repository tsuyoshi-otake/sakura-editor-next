# Phase 6 Output Service Guidance

## Ownership

`OutputService` is the process-local, HWND-free authority for Output and
structured Log channels. Native panel tabs are projections/adapters; they own
neither channel content, active-channel selection, nor owner lifetime. The
extension output-channel RPC that used to sit beside them was removed in #169.

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

`CWorkbenchRuntime` owns the selected `IOutputService` provider; `CEditWnd`
renders authoritative channel snapshots through a coalesced UI-thread
projection. The Git `Log` channel below is the only production content
producer. Native channel selection is model-first with exact owner/generation,
expected revision, a stable non-wrapping operation ID, and `preserveFocus`.

The canonical `workbench.action.output.toggleOutput` command remains the user
toggle, while a service `Show` mutation is a non-toggle reveal: its
`ChannelShown` notification reveals the Output view without stealing focus
when `preserveFocus` is set. The integrated native cohort passes 210/210.

Issue #383 adds the upstream Output view action surface
(`workbench.output.action.clearOutput`,
`workbench.output.action.toggleAutoScroll`,
`workbench.action.openActiveLogOutputFile`,
`workbench.action.saveActiveLogOutputAs`,
`workbench.action.showOutputChannels`, and the keybinding-only
`workbench.actions.workbench.panel.output.clearFilterText`) plus the native
channel selector, text filter, find, and smart-scroll chrome. Two recorded
approximations: upstream groups the channel selector by extension/core/user
origin, but this service has no separate user-channel metadata, so
`GroupOutputViewChannels` treats `core.*` owner IDs as core and every other
owner as extension-like. `workbench.action.openActiveLogOutputFile` was a
typed unsupported terminal until #421 gave Log channels a session log file;
it now opens that file, and stays `NotApplicable` for plain Output channels
(see [`../logs/CLAUDE.md`](../logs/CLAUDE.md)).

## The Output body keeps its colors across any WM_SETFONT (#420)

- **Symptom:** the Output body's text was black on the dark panel right after
  the user switched to Output. It turned light only when the next line
  arrived.
- **Cause:** the body is a plain-text Rich Edit (`CBottomPanelTool`). On every
  `WM_SETFONT`, a plain-text Rich Edit resets all characters, and its default
  format, to `CFE_AUTOCOLOR`, which is black.
- **Two senders reset it:**
  - The tool itself, on its first layout and on a DPI change. The first fix
    (868dbccdd, 7919) covered only this sender.
  - `CWorkbenchPanelHost::LayoutTool`, which sends the host's chrome font to
    every descendant before each tool layout. This one kept the defect alive
    in 7921.
- **Fix:** the body owns its colors. A window subclass
  (`OutputTextSubclassProc`) calls `RestoreOutputTextFormat` after every
  `WM_SETFONT`, whoever sent it. It reapplies the palette colors (SCF_DEFAULT
  and SCF_ALL) and the filter-match highlights. The selection and the first
  visible line are kept.
- **Rule:** do not fix this by repainting from one known sender. Any parent
  may broadcast a font.
- **Test:** `BottomPanelComposite.OutputBodyTextKeepsThePaletteColorAcrossAFontChange`
  reads the colors back with `EM_GETCHARFORMAT` after each of these:
  - the tool's own layout;
  - a DPI change;
  - a tab switch;
  - a `WM_SETFONT` sent straight to the body.

  The last step fails without the subclass.

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

The adapter admits each mutation through one serialized transaction: count and
invalidate under the model lock, convert the admitted input outside that lock,
call Rust, and finalize either the result or the retained fault before another
mutation enters. All advisory callbacks run outside both the model and mutation
locks. Notification IDs may be borrowed through this synchronous path, but the
bounded dispatcher must own their values before QueueLocked returns; no view of
caller storage may be retained in the queue.

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
bytes from an earlier revision. The C++ adapter keeps at most one bounded,
immutable decoded observation cache for a ready live snapshot. Its valid bit is
cleared before every fallible mutation and terminal Stop, while revision and
the C++ dispatcher drop count are checked on every hit; only validated
non-accepted results may make the retained value eligible again. Every public
Snapshot still returns a caller-owned value copy, and no cached foreign pointer
or decoded C++ authority state crosses back into Rust.

A cached Snapshot read linearizes when its revision, validity and advisory drop
count are checked under the provider locks. Pin that immutable observation with
shared ownership, then release both locks before its O(N) caller-owned copy.
Likewise, after publishing a newly decoded observation, retire the previous
cache and copy the result outside the mutation fence. Concurrent mutations or
Stop may retire the provider's reference but must not alter an in-flight read's
captured value; subsequent reads still revalidate against current state.

Issue #274's adoption decision was explicitly approved by the user on
2026-09-08: Rust is the default MSVC Output authority; MinGW remains C++.
That names the production authority, not what is linked. Since #303 the MinGW
build links the same `sakura_native_ffi` archive and compiles the same candidate
paths, so this service's differential corpus runs under both toolchains while
only MSVC's Output provider is Rust; `src/main/CLAUDE.md` records that boundary.
`SAKURA_OUTPUT_PRODUCTION_PACKAGE` remains independent from UTF-16 and accepts
both Rust and an explicitly selected C++ rollback build. The canonical release
provenance requires Rust Output and C++ UTF-16; a rollback release must explicitly
update that exact provenance policy, never silently relabel a C++ artifact.

This decision accepts the recorded performance-gate misses and defers Intel
hardware coverage and long-duration canary validation. Those cells remain
unverified, not passed. The local-release exception requires successful local
regression, installer/payload identity, and startup/shutdown checks before
publication; it does not represent hosted CI as passed. Preserve all paired
benchmark results and failed gates. Selection remains immutable per lifecycle;
rollback requires a new build and never means runtime state transfer or fallback.

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
adds no new `OutputService` invariant. `CEditWnd` wires it into every Git
operation it invokes: `MakeGitOutputSink(m_outputService)` feeds `RunGitLogged`
behind each command context's `run` callback (branch, init, clone, stage,
discard, commit, sync) and the diff/line-staging readers. Since Issue #419 the
channel exists as soon as the window's Source Control provider starts —
matching upstream's `createOutputChannel` at Git extension activation — and
the two background paths log through the same sink: `CScmWorkbenchTool`'s
periodic status/history refresh (`GitStatusRefresh.cpp`, snapshotted with the
worker's root and generation) and `CGitHubRepositoryStatusService`'s
`git remote get-url origin` probe. `senp/github/GhRepositorySelection.cpp`
still calls `RunGit` directly and does not log: it is the SENP GitHub
extension's own provider surface, not the built-in Git integration this
channel mirrors.

## Output view pixel parity (#422)

Checked 2026-11-17 against upstream `outputView.ts` and `views.css`.

- **Body font.** The Output body is a read-only text editor upstream, so it
  uses `ThemeFontKind::Editor`, not the chrome font. The panel host sends its
  chrome font to every descendant before each tool layout, so the body's
  subclass replaces any `WM_SETFONT` font with the editor font, then restores
  the margin and the colors (#420).
- **Wrap and margin.** Upstream sets `wordWrap: 'on'` and
  `lineDecorationsWidth: 20` with line numbers, glyph margin and folding off.
  The Rich Edit uses `EM_SETTARGETDEVICE(0, 0)` (wrap at the window edge; `1`
  turns wrapping off) and a 20 DIP left margin through `EM_SETMARGINS`.
- **View title.** Upstream right-aligns the title actions and
  `FilterViewPane` puts the filter first: filter, Switch Output, Clear Output,
  the scroll lock, `...`. `.panel > .title .viewpane-filter-container` is
  150-200px wide, 400px while focused, with a 10px right margin.
  `CalculateOutputViewTitleLayout` is that rule; the region grows to
  `OutputViewTitlePreferredWidth` instead of the terminal toolbar's 260 DIP,
  which had left the filter about 70 DIP. The placeholder is cut at 200px as
  upstream's is.
- **Open: log line timestamps.** Upstream's view of a Log channel shows the
  log file's `YYYY-MM-DD HH:MM:SS.mmm [info]` lines; this view still projects
  `[Info] message` without a time. The stored log entry has no timestamp, and
  the Rust authority (`output_shadow.rs`), its C ABI and snapshot codec, and
  the C++ provider must all gain one together. Tracked in #422.
- Tests: `BottomPanelComposite.OutputViewTitleKeepsUpstreamOrderAndFilterWidth`
  and `BottomPanelComposite.OutputBodyIsAWrappedEditorSurfaceWithTheUpstreamMargin`.
