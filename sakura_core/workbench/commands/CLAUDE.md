# Phase 5 Command and Context Guidance

## Stable Authority

- `WorkbenchCommandRegistry` is the window-local authority for stable command
  descriptors, surface bindings, enablement, and executor selection. Menus,
  keybindings, Activity Bar items, the Command Palette, watermark actions, and
  extension requests must resolve to the same command ID before execution.
- `WorkbenchContextKeyService` owns the merged context snapshot. Core
  `workbench.*` keys are projected only from authoritative Workbench state;
  extension overlays are owner/generation scoped and cannot overwrite the core
  namespace.
- Keep this directory independent of `HWND`, `CEditWnd`, generated legacy
  function enums, and extension transports. The native composition root may
  bind executors and integer compatibility aliases, but the pure registry does
  not dispatch legacy commands itself.

## API Commands and Their Argument Lists

VS Code's *API commands* (`workbench/api/common/apiCommands.ts`) are a distinct
kind of command and must be registered as one.

- Upstream registers `vscode.diff` and `vscode.open` in `CommandsRegistry` only:
  no `MenuRegistry` contribution, no category, and no keybinding. They therefore
  carry **no surface bindings** here — no Command Palette slot, no menu, no
  Activity Bar entry — and their `when` clause is `workbenchReady`. A command an
  extension calls is not a command a user finds, and giving one a palette slot
  upstream does not have would invent a surface.
- They belong to the workbench, not to a feature, so they are registered in
  `RegisterBuiltinCommands` rather than in a feature batch such as
  `RegisterGitCommands`. A Git command that *calls* an API command
  (`git.openChange`) stays in the feature batch with the feature's own `when`
  clause.
- `ApiCommandArguments.h/.cpp` owns the argument lists. Upstream passes real
  `URI` objects because its caller and its handler share a process; here the
  argument list is a wire payload, so the string form is what crosses. That is
  the same string an extension would pass, which is the point of registering the
  API command at all.
- The builders and parsers are symmetric and the parsers fail closed on anything
  malformed, over-long, or carrying a member the command does not define. An
  absent trailing argument (`title`, `label`, `options`) is not malformed —
  upstream declares those optional. A bound on string length exists so a hostile
  payload cannot make a decoder allocate without limit; it is not a statement
  about how long a real path may be.
- An optional argument's *absence* is preserved rather than flattened to a
  default. `TextDocumentShowOptions.override` is `std::optional<bool>` because
  the built-in Git provider passes `false` for a both-modified conflict and
  leaves it undefined otherwise, and those are different requests even where
  this product currently routes both the same way.

## Explorer Resource Commands

`RegisterExplorerCommands` registers upstream's eight resource-scoped Explorer
file-operation commands (`explorer.newFile`, `explorer.newFolder`,
`renameFile`, `moveFileToTrash`, `deleteFile`, `copyFilePath`,
`copyRelativeFilePath`, `revealFileInOS`) as one atomic feature batch.

- **Surface shapes come from upstream, not from symmetry.** Upstream gives
  these commands three distinct shapes: `explorer.newFile`/`explorer.newFolder`
  are context-menu-only (no default keybinding, no Command Palette entry);
  `renameFile`/`moveFileToTrash`/`deleteFile` add a keybinding (F2, Delete,
  Shift+Delete) but still no palette entry; `copyFilePath`/
  `copyRelativeFilePath`/`revealFileInOS` have menu, keybinding, and palette.
  `EExplorerCommandSurfaces` encodes exactly those three shapes so a new
  command must pick one deliberately.
- **Recorded simplification — palette retitle.** The registry carries one
  title per command, so the palette slot reuses the context-menu title.
  Upstream retitles two of them in the Command Palette: `copyFilePath` appears
  there as "Copy Path of Active File" and `copyRelativeFilePath` as
  "Copy Relative Path of Active File" (category File), while their menu
  titles stay "Copy Path" / "Copy Relative Path". Until the descriptor model
  supports per-surface titles, the menu title is the one registered here. This
  is a known divergence, not an accident; remove it by adding per-surface
  titles, not by renaming the menu entries.
- **`workbenchReady` clauses.** Upstream gates these commands on Explorer
  focus/visibility context keys (`filesExplorerFocus` etc.) that this native
  provider does not publish. Following the `MakeGitAlwaysAvailableDescriptor`
  precedent, the batch uses `workbenchReady` alone rather than fabricating
  conjuncts over keys that never become true; add the real keys before adding
  the real clauses.
- **`moveFileToTrash` and `deleteFile` are two commands.** Upstream registers
  trash deletion and permanent deletion as separate command IDs, so this
  registry does too — never one executor reading a "permanent" flag.
- `ExplorerCommandArguments.h/.cpp` owns the wire payload: a one-element JSON
  array carrying the resource URI. Upstream's multi-select second argument is
  deliberately not part of the contract, and the parser rejects it rather than
  accepting-and-ignoring; the Explorer surface divergence record lives with
  the Explorer UI's own guidance.

## `isWorkspaceTrusted`

`isWorkspaceTrusted` is a reserved core context key: it is projected only from
`config::WorkspaceContextSnapshot::trust`, and `IsReservedCoreKey` rejects any
extension overlay that tries to write it. A `when` clause gated on trust must
not be forgeable by the very code the gate exists to restrain.

Upstream's key is a plain boolean while this product's trust is three-state, so
the projection collapses — and it collapses toward **withholding** trust.
`Unknown` means trust was never granted and `Untrusted` means it was denied;
both project `false`, because "we do not know" is not permission. Only
`EWorkspaceTrustState::Trusted` projects `true`.

## Stateful Operations

- Registration is atomic, revisioned, bounded, and conflict checked across
  command IDs and surface slots. Owner disposal removes exactly one matching
  generation.
- Evaluate `when` and enablement clauses against one immutable context snapshot.
  Malformed or unsupported expressions fail closed.
- Every execution terminates as `Succeeded`, `NotApplicable`, `Disabled`,
  `UnknownCommand`, `Unsupported`, or `Failed`. Once a stable command is found,
  its terminal failure must not fall through to a different legacy action.
- Native executor callbacks mutate the Workbench model first, project a
  validated current snapshot, and refresh the core context projection. Never
  pre-commit a second visibility, active-view, focus, or layout truth in an
  HWND-owning control.

## Verified Checkpoint (2026-07-31)

`workbench.action.toggleSidebarVisibility`, `workbench.view.explorer`,
`workbench.actions.view.problems`, and
`workbench.action.output.toggleOutput` are registered with real native
executors. Explorer Command Palette and Activity Bar reveal routes converge on
`workbench.view.explorer`. The title-bar control, View menu, and `Ctrl+B`
keybinding converge on `workbench.action.toggleSidebarVisibility`; Menu and
Keybinding retain `F_TOGGLE_LEFT_EXPLORER` only as the integer compatibility
alias after the source/high bits have been preserved by the native dispatcher.
Problems and Output are Command Palette/Menu/Keybinding commands, not Activity
Bar commands.

Output also has a distinct native show-only route for extension
`OutputChannel.show`; it must not toggle an already visible panel off.

The focused command/context/catalog cohort passes 19/19 and the integrated
profile/runtime/command cohort passes 79/79. The Command Palette UI and
extension-host command contribution bridge are not yet complete consumers of
this registry, so this checkpoint is a verified spine rather than complete
command compatibility.
