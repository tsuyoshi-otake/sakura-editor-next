# P0 Process Composition Guidance

## Scope

This directory is the composition root for process selection and lifecycle.
It binds platform contracts to the hidden control process or an editor process.

## Ownership

- `CProcessFactory` remains the only process/profile bootstrap entry point.
- `CControlProcess` is authoritative for stable profile registration,
  workspace/window profile association, durable storage writes, migrations,
  cleanup, and shared extension-host ownership.
- `CNormalProcess` receives a resolved profile/workspace/window descriptor. It
  must not reopen the profile registry or durable state database directly.
- Keep `CShareData` limited to its legacy shared-memory ABI. New revisioned
  state uses a dedicated IPC contract.

## Lifecycle

Use explicit phases: profile resolution, platform services, storage migration,
workspace/configuration, workbench creation, restore, extension session, ready,
and reverse shutdown. Each failure declares rollback ownership and one terminal
outcome. Shutdown joins bounded flush/close operations and records timeout or
forced termination instead of silently abandoning them.

For the P0 control-platform path, bind the phases in this order: resolve and load
the final legacy profile directory; acquire `ProfileAuthorityStore`; open and
lock `CAtomicFileStorageService`; start `CControlPlatformServiceHost` through its
`Accepting` endpoint state; then publish the existing control-ready handoff.
Shutdown is the reverse: stop/join the host and withdraw the endpoint before
closing storage and releasing authority ownership. A failed or corrupt durable
authority/store must have an explicit terminal policy and must never silently
mint a new identity or expose an empty replacement store under the old endpoint.

After the control-ready handoff, `CNormalProcess` may read and freeze one trusted
`ControlPlatformEndpoint` descriptor and create `CControlPlatformClient`. It must
not reopen `ProfileAuthorityStore`, the storage database, or infer identity from
the profile display name, command line, or `CShareData`; Hello independently
confirms the endpoint's immutable `profileId` and generation.

Once Hello has confirmed that identity, `CNormalProcess` resolves an immutable
`ProfileBootstrapSnapshot` and `WorkbenchBootstrapContext` before creating the
workbench. Only an explicit folder argument establishes a Folder workspace; an
initial document remains a loose document and cannot become an Explorer root or
authorize `.vscode` discovery. Terminal fallback is frozen independently. Invalid
or incomplete bootstrap input fails closed before any editor window is created.

An explicit `-WORKSPACE=<path>` launch resolves only to the typed workspace
configuration URI before the runtime starts. It is mutually exclusive with
`-FOLDER=<path>` through the bootstrap shape contract. The runtime owns reading
and parsing the `.code-workspace` contents: unreadable or malformed documents
remain Workspace launches and surface typed `ReadFailed` or `ParseFailed`
diagnostics; they must never fall back to Folder.

## Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

`CNormalProcess`/`CEditApp` composition owns the adapter between the pure
workbench-layout service and the control-process storage writer. For the verified
profile, it reads only the profile-scoped `workbench.layout` Machine-state key
before creating a native editor/workbench window. The adapter exposes `Loaded`,
`NotFound`, `InvalidStoredMemento`, `Unavailable`, and `Failed` restore outcomes: only `Loaded` restores;
corrupt or unsupported bytes remain durable and are not replaced during startup.

At orderly editor shutdown, composition destroys the native window first, then
performs at most one changed-layout save before `CEditorControlPlatformRuntime`
stops. It obtains the current global storage revision for CAS, keeps one stable
operation ID across one bounded ambiguous-result retry, and surfaces conflicts
without overwriting the remote state. Editors and HWND-owning views do not own
durable writes. The interim multi-window rule is profile-shared, last successful
nonconflicting orderly closer wins; stable logical window/workspace identities are
deferred and PID identity is never persisted. The production adapter is now
composed on this lifecycle edge and the focused Phase 1 run passes 42/42 tests.

## Implementation Status (2026-07-31)

The UI-independent authority, atomic storage, host, endpoint ABI v2, discovery
reader, editor client/cache primitives, and the long-lived editor runtime exist
and are unit-tested.
`CControlProcess` now owns `CControlPlatformRuntime`: it starts authority,
storage, and the host before publishing the legacy control-ready event and
stops them in reverse order before relinquishing control ownership. Failures
before `Running` fail closed and never publish readiness. `CNormalProcess` owns
`CEditorControlPlatformRuntime` only for processes that create a real editor:
it freezes the immutable descriptor and completes Hello/full snapshot before
plugins and the workbench start, then stops the client after those consumers are
destroyed. Forward-only helper processes do not acquire a session. Both halves
fail closed; production never treats an empty generation-zero cache as defaults.
The verified editor identity now also feeds the immutable profile/workspace
bootstrap, and `CEditApp` owns the resulting workbench runtime for the complete
window lifetime.

## Phase 3 Selected-Profile Composition Checkpoint (2026-07-31)

After the editor runtime reaches `Ready`, `CNormalProcess` obtains one immutable
registry document through `CEditorControlPlatformRuntime::ExecuteProfile`.
Startup never opens the registry file or constructs a second profile authority.
It resolves command-line Folder/Workspace and loose-document URIs first, selects
the user-data profile through the pinned registry snapshot, then creates a
`WorkbenchBootstrapContext` containing both:

- the Control profile used for endpoint, storage, Vault, and other
  control-owned adapter authority; and
- the selected user-data profile used by Settings resource composition.

Any missing/multiple/malformed Profile RPC terminal, authority/generation
mismatch, invalid registry bytes, invalid stable selector, or invalid resource
URI fails before creating the editor window. The process-local window identity
may contain the PID, but the empty-window profile selector and all durable keys
must not.

The current empty-window selector is the fixed
`empty-window:default-window` compatibility key, and the Default profile retains
an explicit legacy-root bridge. Multiple independently associated empty windows,
durable namespace migration, native profile management commands/UI, and moving
layout/working-copy durable scopes to selected-profile resource URIs remain
future composition gates.

## Phase 2 Working-Copy Composition Contract

`CNormalProcess` resolves the canonical control-verified profile identity and
frozen workspace bootstrap before it composes one
`CControlPlatformWorkingCopyPersistenceStore`. Empty workspace scope is absent,
not a PID-derived substitute; folder/workspace scope uses the canonical bounded
workspace identity. `CEditApp` then owns, in dependency order, the store, core
snapshot source, native persistence adapter, recovered-input adopter, lifecycle,
and lifecycle bridge. `CEditWnd` borrows the bridge only.

After native workbench/group creation and before ordinary initial document,
debug, or grep loading, composition asks the bridge to restore only when the
launch policy permits it. A successful native recovery requires a post-restore
layout refresh outside the persistence adapter. During normal operation the
window reports post-core edit commits and periodically requests debounced
flushes; it captures save/close completion tokens before native work and
acknowledges them only after a successful commit.

Shutdown is ordered: begin lifecycle shutdown, force its final flush while the
document exists, destroy the native window, stop the lifecycle/bridge, destroy
native/core dependencies, stop the workbench runtime, then stop the editor
control runtime. Each unavailable, conflict, cancellation, or recovery failure
has an observable terminal result and must not be converted into destructive
cleanup.

## Phase 6 Task Process Composition Checkpoint (2026-07-31)

`CNormalProcess` injects the self-contained default
`ITaskExecutionSessionFactory` into `CWorkbenchRuntime`; it does not select a
workspace folder, read `tasks.json`, or launch a task itself. The factory owns
the native PowerShell locator/provider/policy lifetime and creates one ConPTY
Terminal session per accepted Task execution. Process tasks keep executable and
argv separate; shell tasks are serialized only by the shell-policy boundary.

Runtime starts the folder Task registry from the final semantic workspace
topology and owns Task execution for the entire Ready lifetime. During shutdown
it first disconnects artifact notifications, then stops and joins every Task
session before stopping catalogs/artifact sources and before publishing
Workbench `Stopped`. A Task-listener-originated stop remains `Busy` until the
Task service's safe outer boundary has completed its deferred close.

The current factory is constructed with no native presentation sink. It drains
bounded output so a process cannot deadlock, but does not yet attach Task bytes
to `CTerminalTool`. Keep that limitation explicit: the next Terminal slice must
introduce a runtime-owned presentation/session authority that the HWND panel
borrows. Do not compose Task output directly into a window-local cache or claim
VS Code-compatible Task Terminal behavior before that owner exists.

## Phase 3 User Data Profile Registry Composition Checkpoint (2026-07-31)

`CControlPlatformRuntime` creates and starts `ControlUserDataProfileRegistry`
after control-owned atomic storage opens and before it publishes the accepting
endpoint. The coordinator loads the one durable profile document, then exposes
only typed control-process operations for named/transient profile creation,
rename/delete, workspace or empty-window association, import/export, and pure
selection. `ControlProfileRpc` exposes the command model to Hello-pinned editor
connections through a bounded payload-v1 request/response envelope;
snapshot/list/current are immutable projections, workspace identity is a URI,
and profile IDs remain opaque. Every durable mutation carries an externally retained bounded
operation ID plus a storage-revision CAS precondition; a failed write rolls
back its in-memory mutation. Runtime shutdown stops the host first, then saves
the registry before closing storage. UI rendering remains a separate concern;
no HWND, PID, raw path, or raw pointer is used as durable identity.
