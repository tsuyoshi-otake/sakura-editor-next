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

## FileLoad options and reader ownership (#290)

`CFileLoad` captures `SEncodingConfig` as an immutable owned snapshot. The caller
may change or destroy its settings after construction. `FileOpen` applies the
per-open MIME option before constructing the converter; `CIoBridge` delegates
to that converter and does not decode a second time. Closing and reopening a
loader must use the new option, not the preceding open's flag.

Prepared readers retain the configuration and converter lifetime. This does
not make a converter immutable or thread-safe and does not extend the mapped
view lifetime: the parent loader must still outlive all readers that use its
mapping. Mapping leases and independent stateful converters remain a separate
unfinished ownership change; do not describe the snapshot fix as completing it.
