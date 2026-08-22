# P3 Terminal and Task Backend Guidance

## Ownership

The terminal session service owns PTY/process creation, environment, input,
output, resize, exit, cancellation, and restore. A panel is a view of a session;
hiding or destroying the panel must not accidentally kill or orphan it.

Each launch reaches exactly one terminal outcome, including spawn failure,
normal exit, signal/forced exit, cancellation, host loss, and shutdown timeout.
Bound output queues and background work, and never block the UI thread on PTY
I/O. Tasks compose terminal sessions and problem matchers through stable
services, with deduplicated launch and bounded retry/cancellation.

No terminal here is reachable from an extension. `TerminalTabManager::Impl::Tab`
co-owns the input adapter, model, parser, and `CTerminalSession`, so the UI tab
is currently the authority for process lifetime and there is nothing an
extension-facing service could borrow. The VS Code Terminal API therefore fails
closed with `UnsupportedCapability`; the reasoning and the gating conditions are
recorded in [`../../src/exthost/CLAUDE.md`](../../src/exthost/CLAUDE.md).
Extracting a runtime-owned terminal-instance authority — shared with Task
execution, with this manager and the panel becoming projections of it — is the
prerequisite for changing that, and it is a larger change than wiring an RPC to
`CTerminalTool`.

## Verified Terminal Boundary (2026-07-31)

`CTerminalSession` already isolates the ConPTY backend behind
`ITerminalBackend`, bounds input/output queues and drain work, and exposes
explicit Idle/Starting/Running/Closing/Exited/Failed states. Keep ConPTY handles
private to the backend.

## MinGW vendor compatibility (Issue #83)

The experimental MinGW build compiles the same Sakura-selected Windows Terminal
parser/input/Unicode subset as MSVC. Its compiler boundary is owned by
`terminal/vendor/windows_terminal/sakura_compat`: MinGW-only `__assume`, ETW
declarations, and the minimal WIL surface (handle/string helpers and the flag
operations reached by the input files) are kept there while imported vendor
files remain textually unchanged. The ETW tracing implementation is still
excluded, so no telemetry capability is implied. Do not add a broad WIL shim or
silently disable terminal code to make a new MinGW error disappear; stop at the
closed subset and record a separate compatibility decision.

The Task configuration catalog is owned under
`workbench/tasks/CLAUDE.md`. A catalog entry is not an execution session.
Execution must inject its terminal/session factory, preserve argument
boundaries, assign stable run IDs, bound concurrently active runs, and own
cancel/Stop cleanup independently of panel visibility. Process tasks and shell
tasks require different launch-policy paths; do not concatenate a shell command
inside the catalog.

`CTerminalSession` now supplies the lifecycle shape required by
`ITaskExecutionSession`: `BeginClose()` is nonblocking and idempotent, and
`WaitForClose(absoluteDeadline)` returns `Closed` or `DeadlineExceeded` only
after the backend and all workers are quiescent. A callback/worker self-wait
returns `InProgress` and leaves finalization with an external owner. Start is
fenced by a concurrent close request, and callback-origin destruction retains
the shared implementation until the close worker completes. Never reintroduce
a live-worker detach or reinterpret a reporting deadline as permission to
release active backend work.

`ITerminalBackend::WaitForExit` now returns a typed `Exited`, `TimedOut`, or
`Failed` result. ConPTY caches the root process exit code and reports `Exited`
only after both the root and its job-owned descendants have terminated; output
EOF alone is never treated as process exit. `TerminalSessionCallbacks::completed`
fires exactly once after backend close and reader/writer quiescence and
distinguishes natural exit, requested close, and failure. Keep the real exit
code at this post-quiescence boundary.

`CTaskTerminalSessionFactory` is the production adapter. Process tasks preserve
the executable and argument vector. Shell tasks pass through the injected
PowerShell launch policy, which is the only owner allowed to serialize argument
tokens. Cancellation and close remain distinct, and an explicit service-owned
close does not race a second session-exit publication.

The remaining Terminal compatibility seam is presentation ownership. Production
Task sessions currently drain their bounded output queue, but the normal-process
composition supplies no view sink, so Task output is not yet attached to the
existing Terminal tab/model authority. Do not route raw ANSI/UTF-8 chunks into
an unrelated view-local buffer or claim native Task Terminal compatibility
until a runtime-owned terminal presentation service is shared by tasks and the
panel. Variable resolution, dependency/background scheduling, problem matching,
terminal restoration, and full native task UI are also open.

## Workspace Transitions and Native IME

An in-place folder transition is a terminal ownership boundary. Compare the
canonical `workspaceIdentityKey`; aliases of the same folder do not cross the
boundary. When the identity changes, close every old tab/session and discard
split and queued-input state. Recreate exactly one session in the new folder
only when the Terminal View was visible before the transition. A hidden
Terminal, or a Panel showing Problems/Output, stays process-free until Terminal
is explicitly revealed. Never let a close or replacement-launch failure roll
back an already committed workspace context.

This is an explicit interim divergence from VS Code's workspace-scoped terminal
persistence. Sakura Editor NEXT does not yet serialize/reconnect terminal
processes, history, tabs, or splits per stable workspace/window identity. Do not
retain a live session from the previous workspace or describe the replacement
behavior as restoration; full per-workspace restoration remains open.

The native terminal owns Unicode IME commit delivery. Handle
`WM_IME_COMPOSITION`/`GCS_RESULTSTR` explicitly and encode the complete UTF-16
result once as UTF-8. Keep `WM_IME_CHAR` as the fallback for IMEs that use it,
and do not depend on `DefWindowProc` synthesizing `WM_CHAR`: that behavior varies
between IMEs and native full-screen TUI states. Session rebinding must cancel
composition and discard partial surrogate/backpressured input so old text can
never enter a replacement session.

## The terminal owns its own text keys (2026-08-07)

`CEditWnd::MessageLoop` runs `TranslateAccelerator` against the legacy
key-assignment table *after* the workbench `PreTranslateMessage` chain, and it
does so regardless of which child window has focus. A translated accelerator
consumes the `WM_KEYDOWN`, so `TranslateMessage` never runs and the `WM_CHAR`
this terminal converts into shell input is never produced. The default
assignment in `func/CKeyBind.cpp` binds bare `Space` to `F_INDENT_SPACE` and
`Shift+Space` to `F_UNINDENT_SPACE`, which is exactly why Space could not be
typed into the terminal while every other printable key worked: only Space has
an unmodified default binding, and the keys that do have one (`Tab`, `Enter`,
`BkSp`, `Esc`, arrows, `F1`–`F12`) are already claimed by `EncodeTerminalKey`.

`CTerminalWnd::PreTranslateMessage` therefore finishes the text-input path
itself: when `TerminalKeyNeedsTextDelivery` accepts the event it calls
`TranslateMessage`/`DispatchMessageW` and returns `true`, so the accelerator
never sees the key. The predicate is pure and lives beside the other encoders;
the Win32 half is only "does this virtual key map to a character", answered by
`MapVirtualKey(vk, MAPVK_VK_TO_CHAR)`. Modifier keys, `VK_APPS`, and the
`VK_PROCESSKEY` an IME sends while composing all map to zero and keep their
existing routing.

This matches VS Code, where terminal input goes to the shell unless the binding
opts out through `terminal.integrated.commandsToSkipShell`. Do not widen the
claim to `Ctrl`/`Alt` combinations: those belong to `EncodeTerminalKey` and the
Alt-printable path, and widening it would swallow the host commands that are
still reached through the accelerator.

Verified on 2026-08-07 against x64 Debug with a throwaway `-PROF=` profile. The
same probe typed `echo a b c` into the terminal and pressed Enter, disabling the
claim in place for the control run: with the claim enabled the shell received
`echo a b c` and printed `a b c`; with it disabled the shell received `echoabc`
and reported an unknown command. Every space, and only the spaces, disappeared.

Drive that probe with `PostMessage` to the `SakuraNativeTerminalWindow` child
and capture with `PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)`. Do **not** use
`SendInput`: it types into whatever currently holds the foreground, so the
moment the editor loses it the keystrokes land in an unrelated application, and
a lost-foreground run is indistinguishable from a key that did nothing. Posting
to the window exercises the same `GetMessage` → `PreTranslateMessage` →
`TranslateAccelerator` path this fix lives on, without touching global input
state or requiring the window to be unoccluded.

Known remaining divergences at this boundary, none of them faked: `Ctrl+/`
(`^_`), `Ctrl+,`, `Ctrl+.`, `Ctrl+;`, `Ctrl+:`, `Ctrl+-`, and `Ctrl+1`–`Ctrl+7`
are still claimed by the accelerator instead of reaching the shell, because
`EncodeTerminalKey` covers only `Ctrl`+letter, `Ctrl+Space`, and
`Ctrl+VK_OEM_4..7`. Conversely `Ctrl+B` and `Ctrl+J` reach the shell as `^B` /
`^J` and therefore do **not** toggle the Side Bar or Panel from a focused
terminal, although VS Code lists both commands in its default
`commandsToSkipShell`. Closing either gap requires a real
`commandsToSkipShell`-shaped policy rather than more special cases here.

## DirectWrite Damage Rendering

Terminal render-plan rectangles use absolute client coordinates. Bind the
`ID2D1DCRenderTarget` to the stable full client/back-buffer rectangle even when
Win32 reports a one-row `PAINTSTRUCT::rcPaint`; keep the render plan, HDC clip,
and final `BitBlt` limited to the dirty rectangle. Binding DirectWrite only to a
non-zero dirty row changes the target geometry and can clip shaped fallback
glyphs (notably Japanese) until a later click, move, or full repaint. Preserve
the pixel regression that draws Japanese below row zero and immediately copies
only that damaged row.

## VS Code terminal padding (Issue #173)

Upstream VS Code reserves a fixed 20 CSS-pixel left gutter on `.terminal.xterm`
for command decorations; top, right, and bottom stay flush with the client.
Sakura keeps that same 20 DIP budget but apportions it across all four sides
(`TerminalViewportGeometry::kPaddingDip = 5`), so the character grid is inset
uniformly. This is an intentional divergence: Sakura does not yet own a
decoration gutter, and uniform inset matches the requested native padding
behavior. Scale the per-side padding with the window DPI (5/8/10 physical
pixels at 96/144/192 DPI) and use the same shared geometry for PTY column and
row sizing, render-plan rectangles, caret placement, native IME composition
placement, pointer/selection/mouse-report coordinates, and dirty-row
invalidation. A pointer in the padding clamps to the nearest grid edge.

Keep the full client rectangle filled with the terminal default background and
bound to DirectWrite; only the terminal grid starts inside the padding. The
overlay scrollbar remains pinned to the full client's right edge. A client
narrower or shorter than twice the padding can have a zero-extent grid, but the
existing terminal size contract still reports at least one row and one column.
Do not introduce a separate coordinate calculation at one of these consumers, or
a resize/input path will diverge from what was painted.

Verified on 2026-08-15 with x64 Debug and a throwaway `-PROF=` profile against
the earlier left-only gutter: opening `F_NEW_TERMINAL` produced its first prompt
pixel at x=20 on a 96-DPI terminal; the first 20 pixels were background-only.
Two unoccluded captures each compared `Graphics.CopyFromScreen` against
`PrintWindow(PW_RENDERFULLCONTENT)` with zero different pixels, and the profile
plus every run-owned Sakura process was gone afterward. Re-verify the
apportioned four-side inset the same way after this change.

Interactive terminal output uses event-driven leading-edge delivery. The
notification gate keeps at most one queued wake per tab; the UI thread consumes
that wake before draining the latest available output once, so output arriving
during the drain can own the next wake without flooding the message queue. A
key echo or short response is therefore never held behind low-priority
`WM_TIMER` dispatch, and sustained output does not create an unbounded backlog.
Do not reintroduce a fixed 16 ms application frame timer: presentation cadence
belongs to the workbench frame coordinator, while terminal timers remain
limited to protocol retry and synchronized-output expiry work.

`CTerminalSession` destruction is also nonblocking. A live session pre-admits a
fixed slot in `TerminalWorkerRetirementService`; the lifecycle worker joins the
ConPTY reader/writer workers, and callback-origin destruction transfers its own
join handle to the four-thread bounded reaper. If lifecycle-thread creation
fails, the same close body runs as a fixed-slot reaper task. No terminal worker
is detached, and a UI destructor never waits on a backend. Explicit
`Close()`/`WaitForClose()` remains the external quiescence API for callers that
need a completion result.

## VS Code terminal groups and terminal list (Issue #174)

`TerminalTabManager` owns the independent terminal sessions. `CTerminalTool`
owns the selected terminal group as an ordered leaf projection of a recursive
split tree and creates one native `CTerminalWnd` per leaf in that group. Do not
reintroduce a primary/secondary terminal model or a pane-count cap. Splitting
inserts a new session immediately after the focused pane, and closing a
focused split removes only that pane until its final pane is gone.
`Alt+Left` / `Alt+Right` (and `Alt+Up` / `Alt+Down`) move focus between adjacent
panes; `Ctrl+PageUp` and `Ctrl+PageDown` move between terminal groups.

The active group is a recursive split layout. Upstream VS Code keeps bottom-
Panel splits on one axis (side-by-side); nested panes and vertical splits are
an explicit Sakura multiplexer extension. A split on the same axis as the
focused pane's immediate parent joins that sibling row/column and equalizes
the whole row/column. An orthogonal split replaces only the focused leaf with
a two-child local split, so a left full-height pane can sit beside two stacked
panes on the right:

- `workbench.action.terminal.split` / `Ctrl+Shift+5` / the Split header action
  add a horizontal (side-by-side) pane.
- `SplitTerminalDown` / `Ctrl+Alt+5` / Alt+Split header action / the context
  menu "Split Terminal Down" entry add a vertical (stacked) pane.

`TerminalPaneLayout` is the one geometry authority: it uses a 4-DIP inter-pane
divider, honors the 80-DIP minimum along each split axis when the available
extent permits it, and distributes any remaining extent from each internal
node's positive weights. Newly-created same-axis siblings start with equal
weights; divider dragging changes only the local split node. The typed
`terminal.integrated.tabs.location` policy selects the left or right list
boundary; the resulting pane/list rectangles still come only from
`TerminalPaneLayout`. When a group must be rebuilt, suppress parent-focus
handling until every child has been detached. Closing a focused native child
synchronously transfers focus and may otherwise re-enter layout against a
pane that is being destroyed.

The terminal list is a right-side projection of all `TerminalTabManager`
sessions. It is hidden for zero or one session and, for two or more sessions,
uses the VS Code normal-list policy of 120 DIP preferred width, 80 DIP minimum
width, and a 1-DIP separator. A list row is selected by single click and focused
by double click (`Ctrl+Shift+\\` focuses the list). When the list has more rows
than its height can display, it scrolls by mouse wheel and keeps the selected
row visible. It must never be modeled as
another terminal group or as the authority for process lifetime.

The typed visibility predicate implements VS Code's `tabs.enabled` and
`tabs.hideCondition` values (`never`, `singleTerminal`, `singleGroup`); its
`singleGroup` branch uses the number of terminal groups, never the number of
terminal sessions in one split group. The pure `ShouldShowTerminalTabPolicy`
helper applies `showActions` to the split/kill/more terminal chrome actions
using the resolved single-terminal-or-narrow condition and the current number
of terminal groups, not the number of split panes. The header uses the
separate `ShouldShowActiveTerminalHeader` projection: when the active group is
split, the focused pane's identity remains visible under
`singleTerminalOrNarrow`, as in the observed VS Code split-terminal chrome.
Explicit `always`, `singleTerminal`, and `never` settings remain authoritative.
This is an intentional fork-side presentation choice because the split panes
otherwise lose their compact active-pane identity while the terminal list is
present. The row projection now resolves the
default `terminal` codicon through the existing ThemeIcon/Codicon infrastructure
and paints it in the geometry-owned icon slot; `defaultIcon`/`defaultColor`
customization remains deferred because Sakura has no terminal-profile icon/color
source yet.
All nine tab policy keys are read in one coherent configuration snapshot by
`CEditWnd` and pushed as plain data through `CTerminalTool`. Configuration
notifications marshal to the UI thread and invalidate layout/paint without
restarting a PTY or entering `TerminalTabManager`. Nested/grid terminal
arrangements are part of the recursive terminal-group contract above; they
remain a fork extension rather than a claim of upstream VS Code bottom-Panel
parity.

## Clipboard and Selection Interaction

On Windows, the native terminal follows the VS Code/Windows Terminal host
selection convention: with terminal mouse reporting disabled, right-click copies
and clears a non-empty selection, while right-click with no selection pastes
through the existing bracketed-paste encoder. Paste preference is:

1. Non-empty Unicode text (unchanged VS Code/Windows Terminal behaviour)
2. `CF_HDROP` file paths from Explorer (quoted when needed)
3. Clipboard image (`PNG` / `CF_DIB` / `CF_DIBV5`) saved as a temp PNG under
   `%TEMP%\sakura-editor\terminal-paste\` with the absolute path pasted. After
   each successful save the folder keeps only the newest
   `kMaxRetainedTerminalPasteImages` (32) PNGs so a long session cannot fill
   Temp unbounded. Files still live long enough for multi-image Claude Code /
   Codex turns within that window.

Step 3 is a Sakura-native helper for Claude Code, Codex, Cursor CLI, and similar
tools that accept image *paths* rather than raw bitmaps. Stock VS Code does not
do this without an extension; do not claim upstream parity for it. When a TUI
has enabled mouse reporting, preserve the application's right-click event;
`Shift` explicitly opts into host selection/clipboard handling. Selection ranges
are stored as half-open cell intervals, but mouse endpoints must be normalized to
include both drag endpoints and the complete continuation cells of wide graphemes
before painting or extracting clipboard text.

## Multiplexer Keybinding Presets (fork extension, documented divergence)

The terminal menu offers a `ショートカット` submenu with `なし` / `tmux (Ctrl+B)` /
`GNU Screen (Ctrl+A)`. Stock VS Code has no such feature and no such setting, so
this is an explicit fork extension, not upstream parity. Never present it as a
VS Code capability.

- **`screen` is the default (owner decision, 2026-08-20).** The registered
  default of `sakura.terminal.shortcutPreset` is `screen`, and
  `CTerminalTool`'s own initial value and `CEditWnd`'s read fallbacks all say
  `Screen` so a window with no readable setting behaves like one that read the
  default. The three sites must stay in agreement.
  Consequences the user accepted: `Ctrl+A` is claimed by the panel, so readline's
  beginning-of-line no longer reaches the shell on a bare press — `Ctrl+A`
  `Ctrl+A` sends the literal byte — and a real `screen` running inside the shell
  needs that doubled prefix too. `なし` in the submenu restores an unclaimed
  `Ctrl+A`, and the selection persists to the profile's `settings.json`.
- The chord table lives in `terminal/input/TerminalShortcutPreset.{h,cpp}` as
  pure logic with no Win32 windowing: `ResolveTerminalPresetKey` maps
  `(preset, prefix-armed, key)` onto a `TerminalPresetAction`. It is unit-tested
  in `src/test/cpp/tests1/terminal/TerminalShortcutPresetTest.cpp`.
- Every action maps onto a capability the panel already owns (new terminal,
  split right/down, close pane, close terminal, focus pane/group, select group,
  focus the terminal list). A preset must never invent a capability, and must
  never approximate a missing one with unrelated state.
- Pressing the prefix twice is deliberately reported as *not consumed*, so the
  ordinary encoder delivers the literal control byte. That is tmux's
  `send-prefix` and Screen's `C-a a`, and it is what a nested multiplexer needs.
- An armed prefix followed by an unbound key is swallowed rather than forwarded.
  Leaking `%` or `"` into the shell after a prefix is worse than doing nothing.
- Digit chords select terminal *groups*, because a tmux/Screen window is a group
  here rather than a pane, and both are treated as 0-based.
- Punctuation chords accept the US and JIS spellings of the same character
  (`"` is `Shift+VK_OEM_7` or `Shift+2`), and letter chords ignore Shift, so a
  layout or Caps Lock state cannot silently drop a binding.
- The selection persists as `sakura.terminal.shortcutPreset`
  (`none` / `tmux` / `screen`). The key is deliberately outside the
  `terminal.integrated.*` namespace: those identifiers must keep upstream
  semantics, and this concept has no upstream identifier to reuse.
- The panel never persists the value itself. `CEditWnd` reads the effective
  setting through `ApplyTerminalShortcutPresetSetting()` and writes a menu
  selection back through `PersistTerminalShortcutPresetSelection()`, the same
  split the color theme selection already uses.

## Tab presentation is resolved, not stored (Issue #232, PR 1A)

The tab list used to render whatever the process put in its OSC 0/2 title:
`DrainOutput()` overwrote `Tab::label` with `TerminalModel::Title()` and the
painter drew that field. pwsh reports its working directory that way, so every
row read `C:\Program Files\...`. VS Code's default is
`terminal.integrated.tabs.title` = `${process}`.

The manager now stores only raw data — `processName`, `profileLabel`,
`sequenceTitle`, `initialWorkingDirectory` — and `TerminalDrainResult` reports
`sequenceChanged`, not `titleChanged`, because an OSC title change no longer
implies the displayed title changed. `TerminalTabPresentation` is the pure
resolver that turns settings plus a context into a title and a description; the
tab list, the session dropdown, and the compact active-terminal label in the
panel header must all go through it, and nothing below `CTerminalTool` may hold
a display string. This keeps a recognized Agent CLI title visible even when the
single-terminal policy hides the tab list.

Rules for that resolver:

- It tokenizes into Text / Value / Separator. `${separator}` is conditional, so
  a chain of `wstring::replace` calls cannot implement it: an empty variable
  between two separators has to collapse the pair into one, and a separator at
  either end has to disappear.
- A **known but unavailable** variable resolves to empty and collapses. An
  **unknown** variable is omitted, matching VS Code's `labels.template`
  tokenizer; an all-unknown title therefore reaches the normal process-name
  fallback.
- Never fabricate a value from a neighbouring one. `${cwdFolder}`,
  `${task}`, `${shellCommand}`, `${shellPromptInput}`, and `${progress}` stay
  empty until the subsystem that really owns them exists; guessing them from the
  OSC title becomes a compatibility break once shell integration lands.
- Titles and templates are attacker-influenced (the process writes the OSC title,
  the user writes the template), so both resolved fields strip C0 controls and
  are bounded by `kMaximumTerminalTabTextLength`.

`terminal.integrated.tabs.allowAgentCliTitle` (default `true`) is why the
existing `Claude Code` contract survives the change: for a recognized Agent CLI
the title template is replaced by `${sequence}` exactly once, before expansion,
so a configured title can never be half-merged with the sequence title. The
recognition list is deliberately tiny and rejects any path-shaped title, which
is what keeps pwsh's directory title out. Widening it needs evidence that the CLI
in question really announces itself that way.

The defaults live in `TerminalTabPresentationSettings`. `CEditWnd` owns the
configuration service boundary and pushes a coherent snapshot through
`CTerminalTool` as plain data — `TerminalTabManagerDependencies` owns session
lifetime and must not gain a configuration dependency. Paint reads only the
already-projected settings; it never calls the configuration service.
