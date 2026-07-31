# Command Dispatch Guidance

## Command Definitions

- `EFunctionCode` is generated from `../Funccode_x.hsrc`. Edit the `.hsrc` source; never edit generated `Funccode_define.h` or `Funccode_enum.h` files.
- Treat the ranges and parameter notes in `Funccode_x.hsrc` as authoritative. Do not duplicate a range table here because additions can make it stale.
- Callers can encode source flags in the high 16 bits of a command value. Preserve those flags when decoding, forwarding, or redispatching a command.

## Dispatcher Lifecycle

- `CViewCommander::HandleCommand()` is the central view-command dispatcher. Put implementations in the matching `CViewCommander_*.cpp` category and keep declarations in sync.
- Preserve the dispatch lifecycle: preprocessing happens before an operation block is prepared; once undo/operation state is acquired, every return, error, skip, or delegated branch must retain its finalization contract.
- Do not introduce an early return that bypasses undo ownership, post-command notification, redraw/update work, or caller-visible completion.
- Commands that modify document state should enter through the document/editor abstractions described in `../doc/CLAUDE.md`; avoid direct storage mutation from UI dispatch code.

## P1 Workbench Command Adapter

- New/Open/Show/Save/Revert/Close/Focus and part toggles have stable workbench
  command IDs as the target contract. A native function code becomes an adapter
  only after its complete parameter and terminal-result behavior is routed; an
  aspirational ID is not evidence that the legacy implementation has migrated.
- Menus, accelerators, Activity Bar, Empty Group watermark, Command Palette,
  and extension RPC execute the same registered command and observe the same
  terminal result.
- Enablement and visibility come from Context Keys. With no active editor,
  text-editor-only commands return `NotApplicable`; general workbench commands
  remain available when their own context permits.
- Command completion updates model, context, focus, and extension deltas in the
  documented order. Do not publish success before the owning operation commits.

### Landed file-command seam

- After undo/operation-block preparation and before the legacy switch,
  `CViewCommander::HandleCommand` offers the complete command to
  `CEditWnd::TryExecuteWorkingCopyFileCommand`. Preserve `originalCommand`,
  redraw, and all four `lparam` values; the high 16-bit source flags and Save As
  target/encoding/EOL intent must survive translation.
- Once the seam marks Save, Quiet Save, Save As, Save-and-Close, or File Close
  as handled, every terminal result remains handled. Cancellation, conflict,
  failure, and unsupported capability must not fall through to a second,
  destructive legacy implementation. Common post-dispatch finalization still
  runs.
- `F_FILESAVEALL` remains a legacy process fan-out rather than one atomic
  transaction. Each receiver gets `F_FILESAVE_QUIET` and re-enters the same
  central seam; the sender's historical return does not claim aggregate save
  success.
- `F_FILE_REOPEN*` and `CAutoReloadAgent` remain legacy reload routes until a
  production backend can stage/apply/finalize or roll back native state. Do not
  route them to a typed Revert that can only return `Unsupported`, because that
  would silently remove an existing operation. When the backend lands, translate
  every encoding variant and no-confirm flag here and remove the direct reload
  route in the same verified change.

## Adding or Changing Commands

- Update `Funccode_x.hsrc`, the appropriate commander implementation, and any resource/menu/keybinding tables that expose the command.
- Add focused tests for the command behavior and for any generated code mapping it changes.
- If a new implementation file is added, update `sakura.vcxproj` and `sakura.vcxproj.filters` as required by the parent guidance.
