# P1/P2/P4 Workbench Test Guidance

Pure model tests are the first proof for editor and layout changes. Cover an
existing group with zero inputs and no active editor, then Empty -> New/Open ->
Show -> Dirty -> Save/Cancel -> Close -> Empty. Inject resolve/save failure and
stale completion, and assert the previous stable state and one terminal result.

For views/layout, test stable IDs, toggle/focus/move/restore, narrow clients,
supported DPI values, corrupt persisted state fallback, and capability-disabled
surfaces. UI tests supplement rather than replace the pure model assertions.

Registry/state/memento changes must additionally cover atomic invalid batches,
owner-generation disposal, exact operation replay, signed order values, unknown
stable-ID round trips, duplicate and malformed payload rejection, payload/count
limits, deterministic focus fallback, notification ordering, throwing listeners,
and runtime registration-to-disposal reconciliation. Keep the physical Auxiliary
Bar Part and the Outline View as separate assertions.

## Phase 1 Layout-Memento Persistence Gates

The production composition adapter has landed. Keep focused tests proving every
gate below. On 2026-07-31 the adapter/runtime/state/codec filter passed 42/42 and
the broader foundation filter passed 214/214; future changes must rerun the
relevant gates rather than relying on this historical checkpoint.

1. **Pure boundary:** layout state service and codec are exercised without HWND,
   editor-control, storage, or IPC dependencies; the adapter is the only storage
   caller.
2. **Restore-before-window:** a valid `workbench.layout` Machine-state memento
   is loaded and applied before native-window construction. Missing, invalid, and
   unavailable outcomes are independently typed and leave the process in an
   explicit usable/failure policy state.
3. **Invalid preservation:** corrupt JSON, checksum/schema failure, and
   unsupported-version mementos do not apply and are not overwritten by startup
   or later orderly shutdown merely to create defaults.
4. **Durable identity:** the key is profile-scoped `workbench.layout` at Machine
   target; changing PID/native-window identity does not change the durable key or
   payload. No PID is serialized.
5. **Orderly-save lifecycle:** native-window destruction precedes the one
   changed-layout save, and that save precedes editor-control runtime stop;
   unchanged mementos produce no Apply.
6. **CAS/replay:** save uses the captured global storage revision. A conflict
   preserves the remote value and has no hidden overwrite. An ambiguous result
   makes at most one retry with the identical operation ID; all other failures do
   not retry as a new operation.
7. **Multi-window interim policy:** multiple windows share the profile state;
   the last successful nonconflicting orderly closer wins. Tests must not encode
   PID identity as a substitute for the deferred stable logical
   window/workspace identity.
8. **Write-rate boundary:** layout mutations/events cause zero durable Apply
   calls before orderly shutdown, proving the O(N) full-file backend is not used
   as a per-event persistence channel.

## Phase 2 Working-Copy Persistence Gates

Keep the Phase 2 tests model-first and independent of HWND/control IPC. Cover
all of the following whenever lifecycle or adapter code changes:

1. **Core truth:** inactive recovery adoption, persisted active ID, legacy
   singleton-null migration, exact post-commit activation, identity replacement,
   and revision/version guards leave selection explicit and reject inconsistent
   or stale mutations.
2. **Coordinator terminals:** save, Save As, close, replay, cancellation,
   conflict, error, and the pure staged Revert prepare/apply/Core/finalize/
   rollback paths each finish once; the production native Revert capability is
   separately asserted as unsupported until full rollback exists. Failed close
   never clears the legacy document.
3. **Durable order:** dirty backup precedes session reference; clean session
   removal precedes exact-generation backup deletion; completion-fence replay,
   stale completion, partial-cleanup retry, and CAS guards preserve newer or
   ambiguous durable data.
4. **Lifecycle rate:** edit events capture no text; debounce/max-age and forced
   shutdown flush capture a stable snapshot exactly once per requested flush.
5. **Recovery compensation:** staged native commit follows inactive core
   adoption. Native rejection rolls back that precise inactive core input; a
   rollback failure is asserted as an explicit terminal status. Every prepared
   but uncommitted adoption failure/throw/shutdown aborts the native stage and a
   retry on the same lifecycle instance succeeds.
6. **Bridge policy:** explicit-file, multi-file, debug, and grep starts suppress
   restore; save/close completion only clears persistence after the native/core
   operation succeeds. Save As may replace identity without accepting a stale
   content version.
7. **Load barrier:** `LoadFinalization.*` verifies every read-terminal mapping,
   lossy-success semantics, all-listener invocation, and exception/failure
   aggregation. Native adapter fixtures initialize the character-width cache
   before constructing `CEditDoc` and do not rely on test-suite order.
8. **Command translation:** preserve high-bit source flags and all lparams,
   prove handled failures do not fall through, and keep Save All fan-out distinct.
   Add every reopen encoding/no-confirm and Auto Reload case only when production
   transactional Revert is available.

## Phase 5 Native Part Projection Gates

The 2026-07-31 Part/host/state/memento/runtime filter passes 68/68. Preserve
these gates when changing native Workbench layout:

1. Project Sidebar, Panel, and Auxiliary Bar in descriptor-order-independent
   fashion and reject unsupported schema, missing/duplicate required IDs,
   unsupported positions, and zero/oversized required extents without producing
   a partial projection.
2. Commit a splitter extent to the model before changing the host's committed
   extent. Rejection restores the previous visible state and extent; cancel and
   model-driven application perform no persistence callback.
3. Deliver committed layout revisions in order. A listener-triggered mutation
   queues a later notification instead of recursively entering delivery.
4. Keep the pinned defaults explicit: Sidebar visible, Panel and Auxiliary Bar
   hidden. Tests needing another state must hydrate or mutate it explicitly.
5. Do not use the pure `BuiltinPartProjection` suite as proof of UI-thread
   subscription, HWND application, focus, ViewContainer/View projection, or
   command routing. Add focused native integration coverage for each of those
   boundaries as it lands.
6. Active-surface projection must reject malformed or unsupported active/focus
   hierarchies atomically. Activation alone never assigns focus; editor fallback
   is a focus-only surface and every focused View must belong to the visible,
   active container for its visible Part.
7. Native user requests and committed projection are distinct paths. Outline
   expansion and bottom-panel tab requests invoke the owner before local state,
   preserve the prior state on veto/exception, and their projection setters are
   callback-free. These tests initialize `ShareDataTestSuite` when constructing
   the legacy outline dialog but create no HWND.

After the active-surface checkpoint, `BuiltinPartProjection.*:
WorkbenchLayoutStateService.*:WorkbenchLayoutMementoCodec.*` passed 45/45 and
`NativeWorkbenchToolRequest.*` passed 10/10. Both repository-scoped survivor
checks were clean.

## Phase 3/5 Profile and Command Gates

1. Construct `WorkbenchBootstrapContext` with distinct Control and user-data
   snapshots. Reject malformed selected descriptors/resources and mismatched
   authority ID/generation; prove a named profile's resources do not alias the
   Control root.
2. Runtime Settings tests must seed/read
   `Bootstrap().UserDataProfile().Resources().Settings()` and use the selected
   profile ID in every `ConfigurationTarget`. A transitional `Profile()` read is
   not acceptable in new profile-scoped tests.
3. Built-in command registration is atomic. Prove Command Palette, Menu,
   Activity Bar, and Keybinding slots resolve to one stable ID; only the legacy
   Menu/Keybinding aliases carry the integer function code.
4. Cover enabled success, when-not-applicable, disabled, unknown, unsupported,
   throwing executor, duplicate registration, and exact owner-generation
   disposal as distinct terminal outcomes.
5. Native Explorer entry points must preserve legacy source/high bits, avoid
   fallback after a recognized command fails, mutate model-first, and refresh
   context from the committed snapshot.

On 2026-07-31 the integrated
`UserDataProfileBootstrap.*:WorkbenchBootstrapContext.*:CWorkbenchRuntime.*:
WorkbenchContextKeyService.*:WorkbenchWhenClauseEvaluator.*:
WorkbenchCommandRegistry.*:ControlPlatformClient.*:
EditorControlPlatformRuntime.*` cohort passed 79/79. This is not proof of
profile-management UI, Command Palette integration, extension contributions, or
all native command surfaces.

## Phase 6 Problems, Output, and Tasks Gates

1. Marker tests cover owner-generation replacement/disposal, atomic
   collection/resource replace and empty delete, stale revision, invalid
   ranges/payload limits, deterministic filtering/ordering, reentrant and
   throwing listeners, and terminal Stop.
2. Output tests cover multiple channels per owner generation, atomic newer-owner
   replacement, exact operation replay/conflict, expected revision, UTF-8-safe
   truncation, Output/Log separation, active-channel fallback, preserve-focus
   metadata, reentrant/throwing listeners, and terminal Stop.
3. Task catalog tests cover folder and workspace artifact input, supported
   Shell/Process tasks, explicit unsupported Custom/dependency/background/
   problem-matcher flags, duplicates/limits, last-good retention, stale source
   fences, Clear, and Stop.
4. Runtime artifact tests must prove `.vscode` and `.code-workspace` artifacts
   never enter effective Settings, folder documents override workspace fallback
   only for their folder, watch reload is bounded, topology generations clear
   old sources, and Stop joins all artifact watchers.
5. Task execution tests use fake session factories. They must prove bounded
   active runs, exact operation replay, argument-boundary preservation,
   spawn/exit/cancel/close/Stop terminals, shared absolute close deadlines,
   in-flight startup fencing, listener fault containment, callback-originated
   deferred Stop from Start/Cancel/Close/completion notifications, external
   callback-drain waits, and that panel hide does not own a run.
6. Marker, Output, and Ports service tests bound owner/tombstone identity,
   saturate advisory-drop counters, wait for external callback drain, and return
   Deferred for callback-originated Stop. They must not destroy a borrowed
   service from inside its callback.
7. Terminal lifecycle tests prove close-before-start, start/close races,
   nonblocking close initiation, absolute-deadline reporting only after
   quiescence, callback self-wait deferral, and destruction from Starting and
   Closing callbacks. Observations that outlive a fake backend must use
   separately owned state; never dereference the backend after the session may
   have destroyed it.
8. Native Problems/Output tests preserve full marker URI/ranges and Output
   owner/generation metadata, keep projection setters callback-free, and prove
   model-first user selection. Dispatcher tests cover service-first mutation,
   exact-generation disposal, desired-absence diagnostics, Output operation
   replay/conflict, and repeated `show` reveal intent.
9. Folder Task registry tests use the artifact service's atomic batch API and
   prove explicit Empty/single/multi-root topology, per-folder override/fallback,
   reorder identity stability, last-good retention, and no first-folder guess.
   Production adapter tests preserve argv/cwd/terminal size, cover PowerShell
   token quoting and bounded drain, and observe one post-quiescence result.
   Include a real ConPTY process with a deterministic nonzero exit code.
10. Runtime Task tests expose the registry/execution service only while Ready,
    reconcile known-empty/add/remove/reorder slots, and prove that Task-callback
    Stop returns Busy until the Task service has quiesced. An external runtime
    Stop must own final `Stopped` publication. These lifecycle tests are not
    proof of Task Terminal UI: production output remains unprojected until a
    shared runtime-owned terminal presentation authority is added.

The integrated runtime/pure-backend cohort covers workspace artifacts, runtime,
Tasks, DAP, Debug Console, ports, markers, output, terminals, and native panel
projection. A later focused checkpoint adds the production Task terminal
adapter, real exit-code smoke, folder-scoped
Task catalog composition, and runtime Task shutdown; record its current count
in the goal-loop journal after each verification. Neither checkpoint proves
Task Terminal presentation, production Debug/forwarding processes, problem
matching, exact Problems range navigation, or complete Debug Console/Ports/Run
and Debug projection.
