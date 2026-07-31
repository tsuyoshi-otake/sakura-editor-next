# P0 File-System Adapter Guidance

## Scope

Legacy streams and local Windows file operations live here. New workbench and
extension code consumes `IFileService` and `IFileSystemProvider` from the
platform layer instead of calling these classes or Win32 directly.

## Provider Rules

- Providers declare scheme and capabilities for read, write, stat, enumerate,
  watch, rename, copy, delete, and atomic replacement.
- Preserve URI identity across the boundary; convert to native paths only in a
  local provider adapter.
- Atomic operations either commit completely or return an explicit terminal
  failure. Cancellation must not leave staging files owned by nobody.
- Watch notifications are advisory and revisioned callers must tolerate
  coalescing, duplication, overflow, and rescan.
- Never let virtual/remote schemes silently fall back to local file APIs.
