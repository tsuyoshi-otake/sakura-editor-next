# Filesystem Platform Guidance

## Scope

This directory owns UI-independent filesystem capabilities and native provider
adapters. Workbench, Settings, extensions, and Working Copy code depend on these
contracts; this layer must not depend on their models, dialogs, HWNDs, proxy
policy, credentials, or control-process UI.

## Resource and Version Semantics

- Normalize resource identity through the shared URI identity service. Display
  paths, casing guesses, PID, and HWND are not canonical resource keys.
- A successful read returns content together with a version token derived from
  the strongest native identity available, such as file ID/volume plus size and
  modification time. Callers must not synthesize a token from content length
  alone.
- Missing files, directories, permission failures, cancellation, conflicts, and
  provider unavailability are distinct terminal outcomes. Deletion of a loaded
  file maps to an orphaned working copy rather than an empty successful read.
- Conditional write/replace compares the expected version token and fails with
  conflict when the source changed. Durable replacement uses a same-directory
  temporary file, flushes required data/metadata, atomically replaces the target
  where supported, and has explicit cleanup ownership on every failure branch.
- Watch events are hints. Consumers resnapshot identity/version after overflow,
  coalesce duplicate notifications, and never treat a watcher callback alone as
  proof of current contents.
- A non-recursive directory watch only proves changes beneath the current
  directory. Consumers that need an optional child directory (for example
  `.vscode`) retain a parent lifecycle watch while the child is absent, then
  recreate the child/member watch after lifecycle, disposal, overflow, or
  rescan events. The caller that obtained a watch owns `Cancel` and must join
  any `Next` worker before releasing the callback owner.

## Phase 2 Boundary

Working-copy backup blobs and editor-session manifests are not ordinary user
files and are not stored through a UI/model direct write. Their durable owner is
the control-platform storage adapter. This filesystem layer supplies staged and
conditional file operations for stored-file Working Copies; it does not own
backup generations, session policy, save prompts, or restore focus.

The current first-slice read/watch API does not yet prove atomic conditional
write/replace support. Keep this limitation explicit until native-provider and
failure-path tests pass; do not emulate it with an unchecked overwrite.

## Durable Working-Copy Record Boundary

The filesystem layer must not reinterpret control-platform working-copy backup
or session payloads as ordinary files. Its role in Phase 2 is limited to stable
resource identity, version-aware stored-file operations, and explicit failure
outcomes. Backup/session keys are scope-qualified control storage records;
their hashing, CAS/replay semantics, corruption preservation, and generation
cleanup stay with the control-platform persistence adapter.
