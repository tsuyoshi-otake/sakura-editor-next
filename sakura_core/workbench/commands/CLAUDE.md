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
executors. Explorer Activity Bar, Menu, and Keybinding routes converge on
`workbench.view.explorer`; Menu and Keybinding retain
`F_TOGGLE_LEFT_EXPLORER` only as the integer compatibility alias after the
source/high bits have been preserved by the native dispatcher. Problems and
Output are Command Palette/Menu/Keybinding commands, not Activity Bar commands.

Output also has a distinct native show-only route for extension
`OutputChannel.show`; it must not toggle an already visible panel off.

The focused command/context/catalog cohort passes 19/19 and the integrated
profile/runtime/command cohort passes 79/79. The Command Palette UI and
extension-host command contribution bridge are not yet complete consumers of
this registry, so this checkpoint is a verified spine rather than complete
command compatibility.
