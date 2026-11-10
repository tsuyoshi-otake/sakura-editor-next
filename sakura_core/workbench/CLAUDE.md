# Phase 1/2/5 Workbench Guidance

## Scoped Guidance

- Editor inputs, Working Copies, operation coordination, backup, and sessions:
  [`editor/CLAUDE.md`](editor/CLAUDE.md)
- Contribution layout state and durable layout mementos:
  [`layout/CLAUDE.md`](layout/CLAUDE.md)
- Retained native ViewContainer pages and independent SENP Views:
  [`viewcontainer/CLAUDE.md`](viewcontainer/CLAUDE.md)
- Lazy Tree View state, SENP subscribers and native hierarchy projection:
  [`tree/CLAUDE.md`](tree/CLAUDE.md)
- Native projection of validated Workbench snapshots:
  [`win32/CLAUDE.md`](win32/CLAUDE.md)
- Markdown hover rendering (`HoverWidget` / `renderMarkdown` equivalents):
  [`hover/CLAUDE.md`](hover/CLAUDE.md)
- Stable commands, surface bindings, when clauses, and context keys:
  [`commands/CLAUDE.md`](commands/CLAUDE.md)
- Files Explorer view and its context-menu row model:
  [`explorer/CLAUDE.md`](explorer/CLAUDE.md)
- Source Control provider, Git execution, and the native SCM view:
  [`scm/CLAUDE.md`](scm/CLAUDE.md)
- SENP Extensions ViewContainer and compact extension-list projection:
  [`extensions/CLAUDE.md`](extensions/CLAUDE.md)
- Workspace artifact routing and file/watch ownership:
  [`workspace/CLAUDE.md`](workspace/CLAUDE.md)
- Problems/markers, Output channels, and Tasks:
  [`problems/CLAUDE.md`](problems/CLAUDE.md),
  [`output/CLAUDE.md`](output/CLAUDE.md),
  [`tasks/CLAUDE.md`](tasks/CLAUDE.md)

## Stable Boundary

The workbench consumes platform, configuration, document, and extension
contracts. Pure models do not depend on HWND, `CEditWnd`, `CEditDoc`, or the
extension transport. Legacy and Win32 behavior enters through adapters only.

## P1 Editor Vertical Slice

- `EditorGroupModel` always exists and owns zero or more `IEditorInput` values.
  Its active editor is optional; zero editors is a normal state.
- Keep resolved documents, visible editors, and active editor as separate sets.
- New/Open/Show/Save/Revert/Close return exactly one of succeeded, cancelled,
  failed, or not-applicable and preserve the last stable state on failure.
- `CEditDoc`/`CEditView` are backing objects behind a strangler adapter. Their
  mere existence does not expose an open or active editor.
- Commands, context keys, focus, native menus, keys, and watermark actions all
  enter through the same command IDs.

### Editor Core Checkpoint

The pure Editor Core model now supports a legitimate empty group with no active
input. It keeps resolved documents, open inputs, and the active input separate;
tracks resolver and input references independently; and disposes a document only
after its final reference is released. Operations use optimistic model revisions
and bounded exact-request replay. Committed notifications are published in
revision order and drained outside the model lock without recursively delivering
reentrant changes; revision exhaustion fails explicitly instead of wrapping.

The production adapter now exposes the legitimate empty-editor state: no tab,
ruler, editor view, or document status surface is fabricated when no input is
open. The empty surface and native commands enter the same stable VS Code command
IDs, and opening the first input transitions the core and legacy backing objects
in one visible commit. Working-copy integration and broader save/dirty migration
remain pending.

### Bootstrap and Runtime Checkpoint

- `WorkbenchBootstrapContext` is an immutable value assembled after the editor
  has verified the control-owned Profile identity. Profile resources, explicit
  workspace shape, initial document, terminal fallback, and window identity are
  separate fields; no consumer may infer one from another.
- `CWorkbenchRuntime` owns the process-local configuration and semantic workspace
  services. Empty, Folder, and Workspace are explicit states, and only explicit
  Folder/Workspace roots participate in `.vscode/settings.json` loading.
- `CEditApp` starts the runtime before constructing `CEditWnd` and destroys the
  window before stopping the runtime. Runtime start/stop and configuration reload
  paths expose explicit terminal states and dispose workspace callbacks on stop.
- The `.code-workspace` settings body is parsed through the existing workspace
  transaction. `CWorkbenchRuntime` owns the configuration watch lifecycle:
  callbacks are advisory, coalesced, and resnapshot through the file-source
  controller; `Stop` cancels and joins watchers before final state so no
  post-stop apply can occur. For a missing `.vscode`, retain the folder-level
  lifecycle watch and recreate the member watch after lifecycle/rescan events.
- `CWorkbenchRuntime` is the sole production-facing Settings writeback owner.
  `WriteSetting` delegates to the bounded coordinator and the existing file
  source controller, which serializes source revision bookkeeping with advisory
  watch reloads. Runtime Stop first gates versioned reads/replaces and stops
  writeback before joining watches, so no post-stop resnapshot applies a
  setting. Current writeback accepts resolved `settings.json` resources; UI and
  `.code-workspace` nested-settings editing are still separate work.
- `WriteSetting` is declared on `IWorkbenchRuntime`, not only on the concrete
  runtime, because a borrower that needs it must not have to name the
  implementation. `CEditWnd` holds the runtime as `IWorkbenchRuntime*`.
  `CWorkbenchRuntime` is
  still the only implementer and still the only writeback authority — putting
  the method on the interface widens who may *call* it, never who may own it.

## P2 Workbench MVP

Use stable part, view-container, view, menu, and command IDs. PROBLEMS and
OUTPUT must bind to diagnostics/problem matching and output channels, not a
visual placeholder. Settings, Explorer, and Search must use the same services
and lifecycle rules as the rest of the native workbench.

### Contribution and Layout-State Checkpoint

- `WorkbenchContributionRegistry` is the process-local catalog for built-in
  Part, ViewContainer, and View descriptors. The v1 native composition still
  accepts at most one SENP startup batch. G04 adds unpublished owner-generation
  candidates, revision-checked publication and allocation-free exact-owner
  disposal to the pure catalog. Native dynamic page composition is a separate
  boundary; a catalog candidate alone must never publish an unsupported page.
- `ProjectHostViewPages` resolves declarative View `provider` IDs through the
  product-owned factory table and returns an atomic native-page batch. Product
  composition registers factories, not extension ViewContainer/View IDs;
  unknown providers and duplicate providers fail closed. A singleton provider
  still rejects multiple Views targeting one container; a grouped provider may
  create exactly one native page for all matching Views in that ViewContainer.
- `CSenpEffectCoordinator` adapts one current SENP owner to Tree, document,
  command, workspace, and derived-tool event admission. Owner publication only
  queues validated terminals. `Drain` runs after owner `Poll`, calls native
  targets at most 16 times, releases ordinary lineages (notifying the target
  once through `Released`), and retains a lineage only when the target
  explicitly owns later derived work. A rejected target is a terminal result
  whose caller owns owner revocation. Which failures are invocation terminals
  and which revoke the owner is the shared policy in
  `../senp/CLAUDE.md` (invocation < owner < package < window snapshot): the
  projection records each admitted root's kind and `Failed` dispatches on it.
- Use the canonical VS Code IDs in `WorkbenchIds`. A physical Part ID is never a
  View ID: in particular `workbench.parts.auxiliarybar` and `outline` describe
  different layers and must remain independently movable and visible.
- `WorkbenchLayoutStateService` owns HWND-free visible, active, focused,
  position, alignment, order, and committed-extent state. Reveal, activation,
  focus, and movement are separate operations. Contribution changes reconcile
  automatically through `CWorkbenchRuntime`, with deterministic active-view and
  focus fallback when an owner is disposed.
- `WorkbenchLayoutMementoCodec` is bounded machine-owned JSON, not user-authored
  Settings JSONC. It persists only stable IDs and committed model state, keeps
  signed order values and structurally valid unknown IDs, and omits HWND,
  generation/revision, maximized state, drag state, and other transient geometry.
- Persisting a memento through the control-owned profile/workspace storage writer
  is a separate adapter responsibility. Do not add direct file writes or make the
  process-local layout model a storage authority.

### Sakura Projects contribution

- `sakura.view.projects` / `sakura.projects` is an intentional Sakura-specific
  contribution because VS Code has no built-in Project catalog equivalent. It
  remains a real declarative ViewContainer/View hosted in the Primary Side Bar;
  it must not be modeled as a new physical Workbench Part.
- A Project is a profile-scoped navigation entry pointing to an existing Folder
  or `.code-workspace` URI. `WorkspaceContext` is still the only active workspace
  authority, and SCM remains the only repository-state authority.
- **Documented divergence (owner decision, 2026-11-09, #405): a switch started
  from the Projects view keeps the active Side Bar container.** Activating a
  Folder, `.code-workspace`, or worktree row in the current window commits the
  workspace and leaves the Projects view showing. It does not call
  `RevealExplorerAfterWorkspaceCommit`.
  - Upstream: VS Code reloads the window to open a folder, and a built
    product's fresh (non-reload) startup falls back to the default Side Bar
    container, Explorer (`src/vs/workbench/browser/layout.ts`, "Sidebar View
    Container To Restore", checked 2026-11-09).
  - Every other folder-open route keeps that upstream result and reveals
    Explorer: File > Open Folder, Open Recent, Git clone/init, and the command
    line (`ApplyFolderWorkspace(root, /*revealExplorer=*/true)`).
  - Why the Projects view differs: it has no VS Code counterpart, and its
    switch is in place, with no window reload. Someone moving between Projects
    is still using this view, and forcing Explorer made them reopen it after
    every switch.
  - Verified 2026-11-09 with a real-window probe (throwaway profile, two Folder
    Projects, double-click on the other row): before, the Projects page was
    hidden and Explorer shown; after, the workspace switched and the Projects
    page stayed visible.
- Git worktree discovery is a bounded, read-only child provider. The current
  Project is scheduled first, then every catalog Project receives one fair
  discovery slot before additional multi-root folders. One single-flight queue
  serializes Git processes, deduplicates roots per Project, caps both roots per
  Project and total work per refresh, and reaches an explicit terminal summary
  on failure, supersession, or teardown. Linked worktrees remain actionable only
  beneath the active Project and start collapsed behind a truthful count.
- Agent rows (#402 Phase A) are projections of the window's terminal agent
  presence supplied by composition; the page never inspects processes or
  titles itself. Phase A shows presence only (no state text), rows exist only
  under the current Project, and a change notification rebuilds the projection
  without restarting Git discovery. An agent row's text is the product name
  alone; `ProjectsRow::tooltip` carries the agent's session title. The page
  owns one tooltip window; each list adds one TTF_IDISHWND tool to it, and
  the tool's text follows the hovered row's painted rectangle. The accessible
  label stays the visible text. The session-title rules live in
  [`../terminal/CLAUDE.md`](../terminal/CLAUDE.md).
- Single click (#412) follows VS Code's `singleClick` defaults of
  `workbench.list.openMode` and `workbench.tree.expandMode`.
  - An agent row opens on a single click: the Terminal view is revealed and
    that terminal is focused, the row's Enter action.
  - The worktrees group row toggles on a single click. `LBN_DBLCLK` does
    not toggle it back, because upstream's double-click expansion is enabled
    only in `doubleClick` mode.
  - The press and the release must land on the same row's painted
    rectangle. The list's own selection runs first. The second press of a
    double click belongs to `LBN_DBLCLK`, so its release does not open the
    row again. A rebuild between press and release drops the press.
  - Documented divergence: Project, worktree, and session rows keep
    Enter/double-click. A Project or worktree row switches the window's
    workspace in place, and a session row starts a new CLI process, so a
    stray click would do something hard to undo. Upstream's single-click
    open only opens an editor.
- Projects rows nest 12 DIP per level, the Explorer TreeView's own
  `TreeView_SetIndent` (#409). A row's dot or chevron sits in the level column
  just before its label.
- **Two Views in one container (#411).** `sakura.view.projects` holds two
  declarative Views, `sakura.projects` (Projects) and `sakura.agentSessions`
  (Sessions), the way Explorer holds Explorer and Outline. Both name the
  provider `sakura.projects`, which supplies `factoryForContainer`, so
  `ProjectHostViewPages` creates one page for the pair.
  - The page renders VS Code's paneview: a 22 DIP header per View
    (`PaintViewPaneHeader`; every header below the first draws the section
    line), bodies split by `BuildViewPaneStackLayout` with a 120 DIP minimum,
    and a 4 DIP sash between two expanded bodies. A header click collapses or
    expands its View, and the press and the release must land on the same
    header. A collapsed body hands its keyboard focus to the other list.
  - Without session history (`ProjectsPageOptions::agentSessions` unset),
    the page has only the Projects View. Its header merges into the container
    title, as `mergeViewWithContainerWhenSingleView` does, and the list fills
    the content.
  - `CProjectsListPane` owns one View's list mechanics: the owner-drawn
    LISTBOX, the overlay scrollbar (#407), its tooltip tool, the single-click
    gesture (#412), and the empty message painted in the list's own body.
    `CProjectsPage` owns the rows, the pane stack, and what each row does.
  - Session discovery rebuilds only the Sessions list, so it never resets
    the Projects list's selection or scroll position.
  - Documented divergences:
    - VS Code's Agent Sessions is a Chat sub-viewer with no View ID. The
      Sessions View and its ID `sakura.agentSessions` are Sakura's own.
    - Collapse state and the sash position live with the page only. VS Code
      keeps them in its view-state memento, which this container lacks.
    - Pane headers take no keyboard focus, so Space/Enter on a header and
      the header's Up/Down navigation are not available.
- **Agent session history (#406, fork extension).** The Sessions View lists
  the current folder's resumable agent CLI sessions, newest first, as
  top-level rows. Double-click or Enter on a row resumes it. While discovery
  is loading with nothing accepted, the View's body says so; Ready with none
  says "No sessions", and Unavailable says "Unavailable".
  - Ownership: `AgentSessionHistory` is the pure contract (records, parsers,
    resume commands, merge, age text). `AgentSessionDiscovery` is the bounded,
    read-only disk and process boundary. `ProjectAgentSessions` in
    `ProjectsModel` projects the rows and the empty message. `CProjectsPage`
    schedules discovery. `CEditWnd` composes the provider and the resume
    action. Discovery only reads agent files and never writes them; the
    only writers are the rename and delete operations of #408 below.
  - Storage formats, verified 2026-11-09 against the installed CLIs:

    | Agent | Location | Session id and title | Resume |
    |---|---|---|---|
    | Claude Code | `%CLAUDE_CONFIG_DIR%` or `~\.claude`, then `projects\<slug>\<id>.jsonl`. The slug replaces every non-alphanumeric character with `-`. | id: the file stem. Title, first match wins: latest `custom-title`, latest `ai-title`, latest `summary`, first real user prompt. An all-`isSidechain` transcript is a sub-agent and is skipped. | `claude --resume <id>` |
    | Codex | `%CODEX_HOME%` or `~\.codex`, then `sessions\YYYY\MM\DD\rollout-*-<id>.jsonl`. `session_index.jsonl` maps id to `thread_name`. | The first line must be `session_meta` for the folder. A `thread_source` other than `user`, or a `source.subagent`, is a sub-agent and is rejected. | `codex resume <id>` |
    | Pi | `%PI_CODING_AGENT_DIR%`, `%TAU_CODING_AGENT_DIR%`, or `~\.pi\agent`, then `sessions\--<cwd, with \ / : as ->--\*.jsonl`. | The `session` header gives the id and cwd. Title: the latest `session_info.name`; an empty name clears it and the first user prompt is used. | `pi --session <id>` |
    | Devin | `devin list --format json`, run in the folder. The whole pretty-printed array ends with a newline. | `id`, `title`, `working_directory`, and `last_activity_at` in unix seconds: a JSON number in 3000.12.17, a string in earlier builds. Both are accepted. | `devin -r <id>` |

  - Overrides: an override variable that is set but holds a relative path
    disables its provider. It is never resolved against the editor's CWD.
  - Not supported, on purpose:
    - Gemini CLI resumes only by list index or file, not by a stable id.
    - The OpenCode schema has not been verified.
    - Copilot is only VS Code's shim.
    - Command Code is not installed here.

    Add one of these only after its format and resume command have been
    verified.
  - Bounds: the display count (20 by default, #408) per provider, and four
    times that plus the marked sessions in total. Large Claude Code and Pi
    files are read through a 256 KiB head and a 256 KiB tail; Codex reads a
    64 KiB head. At most 256 file reads per provider and 20000 directory
    entries are visited. The Codex index is capped at 4 MiB and keeps its tail.
    Codex rollouts named in the index are read first, because sub-agent
    rollouts outnumber them. Reparse points are skipped.
  - Devin trust: `devin.exe` is resolved through PATH, never through the CWD,
    and its signature is not checked, unlike `gh.exe`. It runs through
    `RunBoundedProcess` with a 5 s timeout, a 1 MiB stdout cap, a 64 KiB
    stderr cap, and the page's stop event.
  - Ids: every id must match `[A-Za-z0-9][A-Za-z0-9_-]{0,127}`, so a resume
    command never needs shell quoting.
  - Discovery runs single-flight on a worker thread. The page polls it every
    50 ms with timer `0x7073`.
    - It reruns on Refresh (activation or F5) and when the set of agent
      identities changes. A title change alone does not rerun it.
    - Catch-up (#410): an agent CLI writes its session file only after the
      first message, so its row appears before its session exists. The page
      records when it first saw each agent row. While a present agent still
      has no session of its own, the page reruns discovery on timer
      `0x7074`. A session is the agent's own when its provider's display name
      matches the agent's product name and its activity is at or after that
      first sighting. Each session covers one agent.
      - Schedule: 5 s, doubling to a 60 s cap.
      - Bound: at most 12 attempts, about 9 minutes, per activation or
        agent-set change.
      - It stops as soon as every agent is covered, and on deactivation.
        `CountAgentsAwaitingSessionHistory` is the pure rule.
      - Codex and Devin set no OSC title, so a title-change trigger could
        never have caught them.
    - Whole-document JSON, Devin's list, ends with a newline. `ParseJson`
      treats all four RFC 8259 whitespace characters as insignificant at the
      end. Before #410 a trailing `\n` rejected every Devin list, so no Devin
      session was ever shown.
    - While a rerun for the same folder is loading, the last accepted list
      stays visible. A folder change cancels the running job and clears the
      list.
    - Close signals the stop event and waits for the worker.
  - Resume works like VS Code's `createTerminal({cwd}) + sendText(command,
    true)`:
    1. `CTerminalTool::AddTerminalWithInput` creates the terminal in the
       session's folder and queues the command followed by CR.
    2. The Terminal view is revealed. Creating the terminal first means the
       reveal does not start a second, default terminal.
    3. The new terminal is selected.
  - Tests: `AgentSessionHistoryTest.cpp` covers the parsers, the resume
    commands, and discovery against temporary agent homes. `ProjectsTest.cpp`
    covers the rows, the page's worker scheduling and cancellation, resume
    from a row, and the two-View pane stack (geometry, header collapse, sash
    drag and its minimum).
- **Agent session restore (#406 Phase 2, fork extension).** When a window
  leaves a Project — close, or a Folder/Workspace switch — it records which
  agent sessions were live there, and the next open of that Project resumes
  them in terminals through the same path a Sessions row's double-click uses.
  - Ownership: `AgentSessionRestore` is the pure model — recording
    (`RecordLiveAgentSessions`), the persisted record
    (`AgentSessionRestoreState`), its codec, and the per-open plan
    (`PlanAgentSessionRestore` / `DispatchAgentSessionRestore`, which takes
    the resume operation as a seam). `AgentSessionRestoreService` is the
    profile-scoped owner behind `IWorkbenchRuntime::AgentSessionRestore()`,
    with the same `IProjectCatalogStore` contract as the catalog and the
    view state: coherent load-before-save, a conflict reloads and applies
    once, and an undecodable record is kept, never overwritten. It lives
    under owner `workbench.projects`, key `agentSessionRestore` — its own
    record beside `state` and `agentSessions`. `CEditWnd` adapts it to the
    terminal and the workspace lifecycle.
  - Recording: `PersistLiveAgentSessions` runs inside `CloseWorkbench` and
    before a workspace switch commits the outgoing identity — the last
    points where the outgoing terminal projection is still attached. The
    presence record's `startedUnixSeconds` anchors each agent: the earliest
    agent-process creation time, or the first instant a title-only presence
    resolved (`TerminalAgentPresenceTracker::StartedUnixSeconds`). A session
    Sakura resumed is remembered per tab (`m_resumedAgentSessions`) as a
    known identity, pruned to live tabs; a hand-started agent is inferred
    only when the newest unclaimed session of its provider has activity at
    or after its anchor — the same arbitration the catch-up rule uses —
    otherwise nothing is recorded for it. The model never guesses.
  - Inference needs the folder's session list, so a bounded worker
    discovery runs inside the record path: a 1 s Devin cap and a 1.5 s
    deadline, abandoned rather than allowed to stall a close or switch.
  - Restoring: `BeginAgentSessionRestore` runs once per workspace identity
    — startup and folder opens through `RecordCurrentWorkspaceAfterReady`,
    a `.code-workspace` switch at its commit point. It loads the record for
    the single semantic root (empty and multi-root contexts restore
    nothing), starts `requiredIdentities`-marked discovery on a worker, and
    `IDT_AGENT_SESSION_RESTORE` (id 461, 50 ms) polls the future without
    blocking. A repeated open is a no-op; a real switch moves the marker,
    so A -> B -> A attempts A again; a stale result after a second switch
    is discarded. Teardown signals the stop event and joins the worker.
  - No session is resumed twice. A workspace switch keeps the outgoing
    terminals alive in the terminal registry, so an open whose reattached
    projection already hosts an agent restores nothing. The open that
    dispatches a record also removes it, so a second window on the same
    folder finds nothing; the first window records again when it leaves
    the Project or closes. An editor that dies without closing therefore
    loses that record, which is the chosen cost of never double-resuming.
  - The plan distinguishes `Disabled`, `NothingRecorded`, `DiscoveryFailed`
    and `Ready`. Recorded sessions that no longer exist are skipped and
    debug-reported, never retried; the result caps at
    `kMaximumAgentSessionRestoreSessions` (8) per Project and
    `kMaximumAgentSessionRestoreProjects` (64) folders per record.
  - Setting: `sakura.projects.restoreAgentSessions` (default true) turns
    the whole feature off. It is Application/Profile scoped like `http.*`
    and `update.*`, so a repository cannot re-enable process spawning on
    open that the user disabled; an unreadable value takes the default.
    While it is off nothing is recorded either, so re-enabling cannot fire
    a stale record.
  - Tests: `AgentSessionRestoreTest.cpp` covers the record model (known,
    inferred, uninferable), the entry model and cap, the codec, the plan's
    every branch, dispatch through the resume seam, the service's
    load/save/conflict/fail-closed paths, and the presence anchor.
- **Session management menu (#408, fork extension).** A right click on a
  Sessions row selects it and opens its menu; Shift+F10 opens the focused
  row's. Each entry acts on the session the menu was opened for, even when
  the list is rebuilt while the menu is up.
  - Entries and command IDs. VS Code's Agent Sessions ID is used where VS
    Code has the same action (`agentSessionsActions.ts`), a `sakura.` ID
    otherwise:

    | Entry | Command ID |
    |---|---|
    | Resume | `sakura.agentSession.resume` |
    | Rename... | `agentSession.rename` |
    | Pin / Unpin | `agentSession.pin` / `agentSession.unpin` |
    | Archive / Unarchive | `agentSession.archive` / `agentSession.unarchive` |
    | Copy Session ID | `sakura.agentSession.copyId` |
    | Copy Resume Command | `sakura.agentSession.copyResumeCommand` |
    | Delete | `agentSession.delete` |
    | Show 10 / 20 / 30 / 50 | `sakura.agentSessions.setDisplayCount` |

    The IDs name the entries in `AgentSessionCommandId`. They are not
    registered in the command registry yet, so they have no Command Palette
    entry or keybinding.
  - Ownership: `AgentSessionManagement` is the pure model (menu, view state,
    codec). `AgentSessionStoreOperations` writes the agents' stores.
    `AgentSessionViewStateService` owns the profile's view state.
    `CProjectsPage` opens the menu and runs one rename or delete at a time on
    a worker (timer `0x7075`). `CEditWnd` composes the prompt, the
    confirmation, the clipboard, and the error dialog.
  - Rename writes the agent's own store, so the agent's CLI shows the name
    too. Formats verified 2026-11-10 against the installed CLIs:

    | Agent | Rename |
    |---|---|
    | Claude Code 2.1.321 | Appends `{"type":"custom-title","customTitle":..,"sessionId":..}` to the transcript and rewrites `projects\<slug>\<id>\custom-title.json` (`{"customTitle":..}`). The CLI's session list reads the sidecar first. |
    | Codex 0.167.0 | `codex app-server` over stdio: `initialize` (id 1), `initialized`, `thread/name/set` (id 2). The name lives in Codex's state database, so no file is written here. Standard input stays open until the id-2 reply arrives. |
    | Pi 1.3.0 | Appends a `session_info` entry with a new 8-hex `id` whose `parentId` is the file's last entry. |
    | Devin | No rename command exists, so the menu has no Rename entry. |

  - Delete:

    | Agent | Delete |
    |---|---|
    | Claude Code | The transcript, `projects\<slug>\<id>\`, `file-history\<id>\`, and `session-env\<id>\` go to the Recycle Bin in one operation. |
    | Pi | The session file goes to the Recycle Bin. |
    | Codex | `codex delete --force <id>`. Permanent. |
    | Devin | `devin rm <id> --force`. Permanent. |

    Codex and Devin keep sessions in their own databases, so only their CLIs
    can delete one, and that delete cannot be undone. The confirmation says
    so with a warning; the other two say the files go to the Recycle Bin. A
    Recycle Bin move that fails is a failure. Nothing falls back to a
    permanent delete.
  - A Claude Code session whose CLI is running is refused for rename and
    delete. `sessions\<pid>.json` names the session and its process, and the
    process must still exist.
  - `codex.cmd` (the npm shim) runs through the absolute `%ComSpec% /d /c`.
    A shim path that contains any of `"&|<>^%!()@` or a control character is
    refused, because cmd.exe parses its command line again. Session ids need
    no escaping (see Ids above).
  - Pin, archive, and the display count belong to Sakura, not to an agent.
    They are stored per profile under the storage owner `workbench.projects`,
    key `agentSessions`, through the same adapter and the same
    coherent-load-before-save rule as the Project catalog (key `state`). A
    stored record that cannot be decoded is never overwritten. When another
    window saved first, its state is loaded and the change is applied to it.
  - Rows: pinned sessions first, then up to the display count of unmarked
    sessions, then an `Archived (n)` group row that a single click opens.
    Pinned rows are extra to the display count and carry the `pinned`
    codicon. Each list holds at most 256 marks; one more is refused.
  - Discovery reads pinned and archived sessions first and does not count
    them against the per-provider limit (`requiredIdentities`), so a marked
    session stays listed however many newer sessions exist.
  - Renaming or deleting reruns discovery. Deleting also drops the session's
    pin or archive mark.
  - Documented divergences:
    - VS Code's `agentSession.delete` belongs to its own chat sessions. Here
      Delete changes another program's store.
    - Whether the Archived group is open lives with the page only.
    - There is no keyboard shortcut for an entry (VS Code binds F2 to
      rename), only the menu.
  - Tests: `AgentSessionManagementTest.cpp` covers the menu model, the view
    state, its codec, and the view state service.
    `AgentSessionStoreOperationsTest.cpp` covers every rename and delete
    against temporary agent homes with fake process, Recycle Bin, and
    process-table services. `AgentSessionHistoryTest.cpp` covers the record
    builders and required identities. `ProjectsTest.cpp` covers the row
    order and the menu on a real page through the `chooseMenuItem` seam.
- Folder Projects expose their branch or detached state before activation.
  Multi-root workspace Projects inspect the workspace document without mutating
  `WorkspaceContext`, then show a common branch, bounded mixed-branch summary,
  no-repository state, or explicit unavailable state. Never infer repository
  state from the `.code-workspace` file's parent directory.
- The catalog must finish one coherent load before it can write. Invalid or
  unsupported stored data is retained for diagnosis and may never be silently
  replaced by startup auto-registration.
- Projects presentation uses `ProjectsTexts` values in the pure model and the
  existing Japanese/English/Chinese resource tables at `CProjectsPage`. These
  values own their strings because changing language unloads the old DLL.
  `RefreshProjectionStrings` reprojects cached Git observations and preserves
  selection; it must not restart discovery or mutate the workspace/catalog.
  `ProjectsTest.cpp` covers model labels and live native language switching;
  `ActivityBarEntryProjectionTest.cpp` covers the stable title resource mapping.

### Phase 1 Layout-Memento Persistence Checkpoint (2026-07-31)

- `WorkbenchLayoutStateService` and `WorkbenchLayoutMementoCodec` remain pure:
  they must not depend on HWNDs, editor controls, storage interfaces, IPC, or
  profile-path details. A composition adapter is the only bridge between the
  layout service and control-owned storage.
- The durable record is the profile-scoped `workbench.layout` key, targeted at
  `Machine` state. PID-derived window identity is runtime-only and must never be
  a durable key or part of the memento payload.
- Restore must happen before creating the native workbench window. Its typed
  outcomes are `Loaded`, `NotFound`, `InvalidStoredMemento`, `Unavailable`, and
  `Failed`; only `Loaded` applies a decoded memento. Invalid data includes corrupt or unsupported durable
  payloads and must leave that payload untouched for diagnosis/recovery.
- On orderly shutdown, capture the global storage revision for the CAS write,
  destroy the native window, then save once before editor-control runtime stop.
  Skip an unchanged memento. Storage writes are full-file O(N), so layout events
  must update process-local state only and must not each write durable state.
- An ambiguous transport failure may retry the same mutation at most once, with
  the identical operation ID. A CAS conflict preserves the remotely committed
  value and reports a typed conflict; it is never hidden by a retry or overwrite.
- No editor, view, or layout-model class may write durable state directly. The
  current multi-window policy is profile-shared: the last successful,
  nonconflicting orderly closer wins. Stable logical window/workspace identities
  are deliberately deferred; do not approximate them with a PID.

The production control-platform adapter is composed through `CEditApp`, and the
focused layout run passes 42/42 tests. This checkpoint does not complete the
working-copy/editor-operation work above, native shell projection, or deferred
multi-window/workspace memento identity.

## P4 Layout and Interaction

Layout is model-first: part visibility, position, size constraints, persisted
extent, focus target, and DPI-independent tokens produce a snapshot applied by
Win32 adapters. Multiple groups/windows, drag/drop, panel movement,
accessibility, keyboard navigation, and restore extend the model rather than
adding one-off HWND branches. Unsupported capabilities are explicit.

### Native sash and Explorer scrolling compatibility (2026-08-04)

- Workbench Part boundaries reserve one DPI-independent pixel, matching VS
  Code's visible sash border. A sibling, input-only HWND overlays that line as a
  four-DIP interaction target without changing layout geometry or painting a
  thick bar. Parent-only hit-test math is insufficient because the adjacent
  child HWNDs consume three of those four pixels before `CEditWnd` can see the
  initial press; keep the overlay above siblings after every layout pass.
- Explorer never exposes a horizontal scrollbar. Its TreeView keeps
  `TVS_NOHSCROLL`, native scrollbars are suppressed, and the owned vertical
  scrollbar overlays the content instead of reserving a permanent gutter.
- Single-line input geometry is one shared control,
  `workbench/controls/CInputBoxGeometry`. Upstream never writes this code: an
  `InputBox`'s `<input>` and the SCM widget's Monaco line box are both centred
  by CSS `line-height`, and Win32 has no equivalent, so the fork must supply
  that missing layer once rather than per view. `MeasureTextLineHeight` reads
  the control font's `TEXTMETRICW::tmHeight` and `CenterSingleLineEditor`
  centres a one-line EDIT inside the painted frame; Search and Quick Input both
  route through it. A view that recomputes its own inset instead will drift,
  which is exactly how #263 reached two views at once.
- The overlay scrollbar is one shared control, `workbench/controls/COverlayScrollbar`.
  Every workbench list that scrolls uses it -- the Explorer TreeView, the
  Source Control lists, and the Projects list (#407) today -- so no view can drift back to the platform
  scrollbar while its neighbours draw the VS Code one. The control reads the
  target's own `SB_VERT` `SCROLLINFO` and asks it to scroll through one
  callback, which is the only view-specific part (a TreeView walks visible
  items; a LISTBOX takes `LB_SETTOPINDEX`).
- An owner-drawn LISTBOX binds to the overlay through
  `workbench/controls/ListBoxOverlayScrolling` (#407).
  - It uses `ExplicitModel` with `SetHideNativeBar(true)`.
  - Its model is in rows: `LB_GETCOUNT`, whole visible rows, and
    `LB_GETTOPINDEX`. It is read from the row contract, not `SCROLLINFO`,
    which can lag a resize by one paint.
  - A list box whose bar is hidden drops `WM_MOUSEWHEEL`, so the owner calls
    `ScrollListBoxByWheel` and then refreshes the overlay.
  - The list needs `WS_CLIPSIBLINGS`.
  - The owner refreshes the overlay after every top-index change: the
    rebuild, layout, DPI, palette, visibility, `WM_VSCROLL`, `WM_KEYDOWN`, and
    `WM_SIZE`.
  - The `setTopRow` callback runs before the overlay repaints itself, so it
    must set the new model first.

  Source Control and Projects are the two users.
- The same control now also serves the Markdown preview pane and the editor
  text body, and it has a horizontal orientation
  (`OverlayScrollbarOrientation::Horizontal`) for the editor's horizontal bar.
  The Markdown preview is different from native-control consumers: it has no
  `WS_VSCROLL` and supplies one pixel-based `ExplicitModel`. The preview's
  `m_scrollY`, content extent, and viewport extent are the sole authority; the
  overlay must not call `GetScrollInfo`, `SetScrollInfo`, or `ShowScrollBar` for
  that source. This prevents native non-client geometry from competing with the
  sibling overlay during width changes.
  The editor owns real `WC_SCROLLBAR` children; they stay alive as the scroll
  model (`OverlayScrollbarSource::ScrollbarControl`), are hidden, and the
  overlay is given their entire reserved rectangle through `SetBounds`. The
  overlay must cover that full rectangle even when no thumb is required; using
  only the narrower visual thumb width leaves stale native pixels in the
  reserved gutter. Create the model controls without `WS_VISIBLE`, destroy the
  matching overlay before recreating a model control, and do not re-show those
  controls: `ShowScrollBar(..., TRUE)` would put the platform bar back on top of
  the overlay.
- The overlay scrollbar uses TreeView scroll metadata for ordinary layout and
  paint work. Walking visible items is confined to an explicit thumb drag or
  track click, so refresh, resize, and hover do not add an O(N) tree traversal.
- Issue #311 records two open native scrolling behaviors. The shared overlay
  remains visible whenever its source scrolls, whereas VS Code's
  `ScrollableElement` defaults to automatic visibility and fades after hover or
  scroll ends. The native list/tree views also do not autoscroll when the middle
  button is held and the pointer moves; wheel rotation, track clicks, and thumb
  drags are separate working gestures. The overlay currently has no visibility
  state or middle-button gesture owner. Any parity change belongs in the shared
  control and must preserve its source callbacks and sibling clipping; the
  current behavior is recorded rather than simulated by a visual-only fade.

## Phase 5 Native Part Projection Checkpoint (2026-07-31)

- The process-local layout service is the authority for Primary Sidebar, Panel,
  and Auxiliary Bar visibility and committed extent. Native hosts subscribe to
  committed revisions and project a newly read snapshot on the UI thread.
- The pinned VS Code default is Primary Sidebar visible, Panel hidden, and
  Auxiliary Bar hidden. Outline remains a View inside the Sidebar; it is not the
  physical right-side Part.
- Native visibility commands and splitter commits mutate the model first with a
  bounded operation ID and expected revision. A rejected mutation restores the
  prior host state; a native host must not pre-commit a second layout truth.
- `CShareData` physical left/bottom/right fields are compatibility mirrors for a
  runtime-backed window. They are not inputs that can overwrite the current
  layout model.
- The bounded active-surface adapter also projects committed
  ViewContainer/View selection for Explorer, Outline, Source Control, Terminal,
  Problems, and Output. It validates the complete
  active/focus hierarchy before native mutation and applies an explicit focus
  only after native bounds exist. Activity Bar selection, Outline expansion,
  and bottom-panel tab selection are therefore projections of model truth
  rather than an independent `m_eActiveTool` authority.
- Every supported Activity Bar ViewContainer is registered into the Primary Side
  Bar, and the Auxiliary Bar registers none. That is VS Code's **default**, not a capability limit: the
  Secondary Side Bar starts empty and gains a container only when the user moves
  one there, and its toggle remains a pure Part-visibility change that never
  changes Activity Bar selection. Do not reintroduce a placeholder auxiliary
  container to make the right edge look populated.
- Relocation is a model operation. `MoveContainer` is the only authority for a
  ViewContainer's location; the native side bars project that fact and never
  hold a second one. Because a container has exactly one location, a container
  in the Auxiliary Bar has no Activity Bar entry, matching VS Code, and the
  Primary Side Bar must stop claiming it.
- The active side-bar ViewContainer and a container's active View are separate
  model facts. Activating Outline keeps `activeContainers.sideBar` at
  `workbench.view.explorer` while the Explorer container's `activeViewId` becomes
  `outline`. VS Code's Activity Bar toggle compares containers, so any code
  answering "is this Activity Bar entry already active" reads
  `activeContainers.sideBar`, not the container's active view.
- Recorded divergence (omit, don't fake): the bottom Panel header renders only
  Problems, Output, and Terminal. VS Code also contributes Ports
  (`~remote.forwardedPortsContainer`) and Debug Console (`workbench.panel.repl`)
  to this Part, and both stay in the contribution catalog, but neither gets a tab.
  Ports forwards through a Remote Development authority this fork already refuses,
  and a Debug Console reads a DAP adapter process and transport that do not exist
  here, so a tab for either could never resolve to a view. Both previously shipped
  as `WS_DISABLED` buttons; a permanently inert tab is chrome that promises a
  surface, so the container is omitted from the header instead. `PortForwardingService`,
  `DebugConsoleModel`, `DapSession`, `DapProtocolCodec`, and
  `LaunchConfigurationCatalog` are untouched and still pass their suites; only the
  header buttons and their `BottomPanelTab` selection paths were removed. Restoring
  a tab means implementing its view projection, not re-adding the button.
- Run and Debug, Ports, Debug Console, moving the whole Panel
  (`workbench.action.movePanelToSecondarySideBar`), and panel position/alignment
  remain typed unsupported boundaries. A unified command/context route is still
  required. Moving a built-in Activity
  Bar ViewContainer between the Primary and the Secondary Side Bar is now
  supported and projected; see [`win32/CLAUDE.md`](win32/CLAUDE.md) for the
  location-set mapping and [`../window/CLAUDE.md`](../window/CLAUDE.md) for the
  drag gesture and its documented edge-strip divergence.

- Reordering a ViewContainer within its current Activity Bar is a model-first
  layout transaction. The bar resolves a gap among visible ViewContainers only,
  excluding GlobalCompositeBar actions, and the native strip immediately
  reprojects the committed signed order values. The existing orderly-shutdown
  layout memento persists that order; the Activity Bar does not write storage.

The original focused Part/host/state/memento/runtime filter passed 68/68. After
the active-surface integration, the state/memento/projection filter passed
45/45 and the callback/projection-separation filter passed 10/10; the x64 Debug
solution build completed with zero errors and the repository process audits
were clean.

## ViewContainer Pool/Host Boundary (2026-08-01)

- `CViewContainerPages` owns every window a page renders, not just the page's
  primary control. `Close()` destroys page-owned windows before host teardown.
  A host may render a page; it may never own one.
- A ViewContainer has exactly one location, and that location now carries every
  window the page owns, not only its primary control. Nested views such as
  Outline move with their owning Explorer container; nothing may leave a stray
  reparented copy behind in the vacated host.
- A host offers dialog-message translation only to the page content it actually
  owns. A message belonging to another surface is never offered to it, so
  Tab/arrow navigation inside one control cannot swallow input meant for another.
- I06 adds a bounded native companion registration seam beside the frozen
  `IViewContainerPage` and `ViewContainerPagePool` contracts. A contributed page
  declares its own supported location set and must implement native projection
  callbacks for layout, visibility, activation, and message routing; registration
  closes all retained products and fails before publication when it does not. The Primary
  Side Bar, Panel, and Auxiliary Bar query that same descriptor/pool authority;
  no host invents a second capability list. The established built-in descriptors
  remain SideBar/AuxiliaryBar-only unless upstream behavior is verified separately.
- Contributed layout is one atomic native callback carrying host-relative root
  bounds and wrapper-local child bounds. The Panel wrapper begins below its own
  header; side-bar root and child bounds are the same complete client rectangle.

## Phase 3/5 Dual-Profile and Command Checkpoint (2026-07-31)

- `WorkbenchBootstrapContext` carries the pinned Control authority snapshot and
  the selected user-data profile snapshot as different values. It validates
  both, their shared authority ID/generation, the selected descriptor, and all
  URI-only profile resources before a runtime can become observable.
- Settings source identity and Settings file loading/watching/writeback use
  `UserDataProfile()`. Control endpoint and storage adapters continue to use
  `ControlProfile()` until each
  durable state key has an explicit selected-profile scope.
- The Default profile temporarily uses the explicit legacy-control-root bridge
  so existing settings are not silently abandoned. Named/transient profiles use
  opaque-ID namespaces. A durable migration marker and a control-owned stable
  empty-window identity store remain required before removing this bridge or
  supporting independently associated empty windows.
- The native Explorer entry points now route through the command/context spine
  described in `commands/CLAUDE.md`. This does not yet prove complete palette,
  menu/keybinding, or arbitrary View routing.

The integrated dual-profile/runtime/command/control-client cohort passes 79/79.
The complete Phase 3 profile-management UI and the complete Phase 5 command
surface remain open.

## SENP Owner Projection Boundary

`CSenpOwnerProjection` is the UI-thread owner of one committed SENP generation's
Tree providers and document request lineages. It shares one inertable runtime
port across those providers, routes only exact View and operation contexts, and
queues `OpenDocument` effects until the contribution-owner poll and effect drain
have both returned. A document admission is reported to the native target before
its terminal can be accepted. Rejection or revocation clears the runtime port
before providers and document surfaces are released.

Keep owner publication and physical ViewContainer/editor construction above this
class. It does not create HWNDs, invent containers, or perform I/O. A
`StartToolRead` effect retains its request lineage while the native target owns
the nonblocking broker operation. The target exposes terminals only through
`TakeToolRead` on a later UI-thread pump; it must cancel matching reads on
request cancellation and own final cleanup on cancellation or revoke. The
projection bounds outstanding reads to the runtime pending-operation limit and
never accepts a broker callback into the coordinator. Reentrant close from a
native target is deferred until the active drain/admission callback reaches a
terminal state.

A tool completion is a derived operation and therefore can have a different
operation ID from its originating Tree or Document request. Match a derived
document publication or failure by the preserved owner and request generations,
then call the native document target with the original admitted context. Never
publish it using the derived context or match it by resource ID alone.

`CSenpOwnerPublicationHub` is that UI-thread publication boundary. It prepares
the owner projection, hidden grouped ViewContainer HWNDs, the immutable
workbench catalog candidate, and the matching native page candidate before the
runtime starts. Commit first preflights both revision fences and then performs
only allocation-free swaps. `ISenpOwnerPublication::Apply` validates and queues
activation invalidations or runtime results while the owner service is entered;
`CSenpOwnerComposition` is the sole window-local scheduler and always completes
owner `Poll` before it calls `CSenpOwnerPublicationHub::Pump`. Close stops every
runtime before it closes the publication hub and its borrowed native resources.
Legacy native publications remove their exact owner's contributed page factories
and retained pages on revocation. Runtime replacement uses the separate
`ISenpDeclaredTreePublication` transaction: window-owned declarations retain their
catalog and bodies while a prepared runtime generation binds new tree providers.
Commit must change only prepared binding authority; native visibility and provider
requests belong to Pump after owner Poll. A failed commit preserves the previous
generation. Close must fence its exact runtime identity, so a retiring publication
cannot clear its successor or dispose the declaration catalog, even when their
extension ID and generation happen to match. Native declaration composition is a
separate integration responsibility; the binding port alone does not publish UI.

`CSenpDeclaredTreeViews` supplies persistent native bodies for one declaration
cohort. The outer HWND survives runtime replacement and revocation. Preparation
creates hidden real TreeView children; Commit only transfers binding authority,
and Pump replaces native children and starts visible provider requests after
owner Poll. Every prepared candidate has a predecessor fence, so a losing
candidate can close only its own hidden children. Closing an old publication
cannot revoke its successor. Declaration Close owns final native cleanup.
Visibility posts a deduplicated activation request; painting and layout never
activate extensions. Failed and capacity-busy states offer an explicit Retry;
disabled, unsupported and stopped states stay terminal without automatic retry.
The window must Pump the actual activation state even after runtime revocation.
The opt-in `DeclaredTreeViews` rendering probe verifies activation status, the
Unsupported details page, the verified missing-GitHub-CLI diagnostic, runtime
binding, resizing and visibility with the dual-capture protocol. This
component does not itself register declarations in the workbench catalog.

`CSenpViewDeclarations` owns those native declaration cohorts together with their
catalog and page registrations. Registration prepares real hidden bodies, both
revision-fenced candidates, and the retaining map node before either authority
commits. Unchanged descriptors retain their cohort; conflicting structural edits
preserve the old registration and report Conflict. That result is an incomplete
live structural-update boundary, not permission to remove the old cohort first.
Runtime Bind checks the independently current declaration owner. The window must
stop the extension runtime before removing a declaration, and stop all runtime
owners before closing this registry and the borrowed page service. Exact-owner
cleanup preserves foreign pages and registrations. Its injected native callbacks
must not pump messages or destroy the owning window/registry while entered.

`CSenpWindowExtensions` composes package snapshots, persistent declarations and
lazy activation for one native window. Its target factory receives the actual
runtime owner generation. Unchanged declarations survive package replacement;
new hidden cohorts roll back if a later registration fails. One package this
window cannot compose is excluded per package (#352): an unsupported/invalid
`Prepare`, a duplicate ID, the 64-package bound, a refused declaration
registration, or an incompatible runtime removes only that entry from the
cohort, records a `SenpWindowExtensionExclusion` reason the Extensions View
paints, and never reaches the activation `accepted` snapshot. An ID that
several enabled packages claim excludes every claimant, because the row status
and the activation path are keyed by ID and must not pick one by snapshot
order. A refused registration of a package that is already live leaves the
cohort like any other departing package: activation revokes its runtime first,
and only then is its declaration removed. Structural
changes remain a typed conflict boundary: the conflicted package keeps its
previous registration and runtime only while still enabled in the same
workspace/account scope; otherwise it is removed, and the rest of the snapshot
applies either way. Activation failure, exceptions and a registration rollback
that cannot complete are still whole-window failures. Removal revokes
publication authority and signals runtime Stop before removing native pages;
retained runtime slots own physical exit/cleanup independently. Close disables
admission and native callbacks even when physical cleanup fails; the window
must retain the composition and explicitly retry Close. Ordinary polling never
retries a failed join. The window still owns timer scheduling, layout
reconciliation, editor/tool target construction and CEditWnd integration.

A package's `view/title` actions are part of its declaration (#297). The
composition maps each to a `SenpViewTitleAction` on the named View when the
cohort is prepared, so they are compared like any other structural field and a
changed set is a Conflict. The button outlives runtime generations: its callback
holds the declared bodies weakly and asks them for the provider of the binding
that is current now, which submits `CommandInvoked` with no arguments. Without a
bound runtime the click is refused; it never activates the extension (see the
`onCommand:` divergence in `../senp/CLAUDE.md`).

A package's `view/item/context` inline actions are different: they are drawn
by the Tree provider, which exists per runtime generation, so they travel with
`SenpOwnerTreeContribution` into each generation's provider rather than being a
structural field. A changed set of item actions is therefore not a Conflict; it
applies with the next runtime generation.

## Phase 6 Service Foundations Checkpoint (2026-07-31)

- `CWorkbenchRuntime` owns the process-local Marker and Output authorities.
  Their ready-only accessors return null before runtime publication and after
  stop begins; the service objects remain alive in their typed stopped state
  until runtime destruction so native borrowers cannot observe a dangling
  pointer.
- Marker and Output keep bounded state, owner-generation fencing,
  deterministic copied snapshots, committed notifications delivered outside
  locks, and callback-draining terminal Stop behavior. Runtime shutdown stops
  Output before Marker and does not publish `Stopped` while a callback-origin
  service stop is still deferred.
- `.vscode/tasks.json` and the `tasks` workspace member feed a pure bounded
  Task catalog; `.vscode/launch.json` and its workspace member feed the Debug
  launch catalog. Workspace/folder artifacts remain separate from Settings.
- Multi-root ownership stays explicit: do not select the first folder
  implicitly or put independent folder revisions into one global task/launch
  catalog.
- Canonical Problems and Output command IDs reach service-backed native panel
  projections. `CEditWnd` subscribes through an HWND-only coalescing gate,
  re-snapshots on the UI thread, and unsubscribes before panel/runtime teardown.
  The extension bridge mutates Marker/Output first and mirrors accepted state
  into legacy decoration/view caches only as a best-effort compatibility
  projection.
- Pure Task execution owns bounded run/session lifecycle through a split
  begin-close/deadline-wait contract. The injected DAP session serializes
  physical sends and owns request/transport shutdown. Debug Console and Ports
  have generation-fenced pure state authorities.
- `CTerminalSession` now provides nonblocking, idempotent `BeginClose` and an
  absolute-deadline `WaitForClose`; every externally returned terminal result
  owns quiesced backend/workers. Callback/self-destruction paths retain the
  implementation until the close worker completes and do not detach live work.
- Runtime now owns `TaskExecutionService` and an explicit per-folder Task
  catalog registry. Production injects `CTaskTerminalSessionFactory`; its
  process and shell paths preserve argument tokens and publish the real root
  exit code from the post-quiescence Terminal completion boundary. Artifact
  batching makes multi-root selection atomic and never guesses the first folder.
- This does not yet make Task output a native Terminal tab. The production
  adapter is currently composed without a presentation sink. A later
  runtime-owned terminal presentation authority must be shared by Task
  execution and the native panel; raw Task bytes must not become a second
  HWND-local or Output-channel truth.

The earlier integrated runtime/service/native-bridge cohort passed 210/210.
The later Task/Terminal checkpoint adds the production Task adapter,
folder-scoped Task catalogs, real ConPTY exit-code integration, and runtime
Task-stop fencing; exact evidence belongs in the goal-loop journal. Folder-
scoped Launch catalogs, DAP adapter processes/controllers, Debug Console and
Ports production ownership, extension Task/Debug/Ports RPC, Task Terminal
presentation, and exact Problems UTF-16 range navigation remain open gates.

## Activity Bar Location (2026-08-23)

- `workbench.activityBar.location` supports the upstream `default`, `top`, and
  `bottom` values. The matching stable commands are
  `workbench.action.activityBarLocation.default`, `.top`, and `.bottom`.
- `default` keeps the Primary Side Bar composite bar vertical. `top` and
  `bottom` project independent horizontal composite bars inside the Primary
  and Secondary Side Bars, each containing only the ViewContainers owned by
  that Part.
- The horizontal bars are the Side Bar-owned compact `PaneCompositeBar`
  presentation used upstream, not a rotated full-size `ActivitybarPart`: the
  bar is 35 DIP high, uses 16 DIP icons and compact horizontal action targets.
  Accounts and Manage remain in the custom title bar for both horizontal
  locations.
- Intentional owner-approved divergence: upstream also supports `hidden`, but
  Sakura does not. The value is rejected by the configuration descriptor and
  no `.hide` command or menu entry is registered. This avoids advertising an
  unsupported state or silently mapping it to a different layout.

## Account discovery process boundary (2026-10-19, #364)

`workbench/account/GhCommandRunner.cpp` runs the fixed `gh auth status
--json hosts` query through the shared `platform::process::RunBoundedProcess`
boundary rather than a private pipe/job/attribute-list stack: the child gets
only its three standard handles, an empty stdin payload (gh never reads
stdin), a kill-on-close job, and the GH_DEBUG-removal / fixed non-interactive
override environment (`detail::BuildGhAuthStatusEnvironmentBlock`,
`GhAuthStatusEnvironment.h`). gh's stderr is discarded inside a 64 KiB bound
(`kMaximumGhAuthStatusStandardErrorBytes`); a larger diagnostic stream ends as
`OutputLimitExceeded` instead of being drained unbounded. Before any launch
the resolved `gh.exe` must pass the shared GitHub-publisher gate: the
injected `detail::GhExecutableGate` seam (`AccountDiscovery.h`) supplies the
resolver and verifier, and production binds
`platform::ResolveWindowsExecutable` +
`platform::security::VerifyGitHubCliTrust` (leaf signer organization exactly
`GitHub, Inc.`). A resolution failure or a refused signature returns
`Unavailable`, which the account source reports as its existing
not-installed state; the untrusted binary receives no invocation at all.
