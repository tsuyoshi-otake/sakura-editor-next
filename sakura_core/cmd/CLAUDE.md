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

## Adding or Changing Commands

- Update `Funccode_x.hsrc`, the appropriate commander implementation, and any resource/menu/keybinding tables that expose the command.
- Add focused tests for the command behavior and for any generated code mapping it changes.
- If a new implementation file is added, update `sakura.vcxproj` and `sakura.vcxproj.filters` as required by the parent guidance.
