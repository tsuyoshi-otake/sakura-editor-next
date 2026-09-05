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

Prepared readers lease the mapped file independently of the parent loader.
The private MappedFile owner holds the file handle, mapping handle, and mapped
view through ResourceHolder; the final lease releases the view first, then
mapping and file handles. CFileLoad retains only a non-owning data pointer
backed by its lease plus its own slice offsets and line state. Parent close,
destruction, or reopen cannot invalidate an existing reader's mapping.
Prepare validates source ownership and range before replacing the destination;
self-Prepare and invalid slices are rejected without changing the old reader.
Open/Prepare failure guards close partial reader state before propagating.
The lease does not provide a stable file-content snapshot against external
writers. Converter lifetime is still shared; this does not establish immutable
or thread-safe conversion. Independent stateful converters remain unfinished.
Prepared partitions copy the encoded NEL/LS/PS byte sequences along with the
EOL flags. They start with an empty decoded-line cache and a zero decoded
UTF-7 offset, including when a previously used reader is prepared again.
Parent progress must not seed a new partition. The two `FileLoadOptionsTest`
Prepared regressions cover these state transitions through real file reads.
This state initialization is independent of the mapping lease.

The extended-EOL policy is captured on FileOpen and inherited by Prepare. UTF-7 decoded splitting and UTF-16 scanning use the same captured state as the byte scanner. A settings change takes effect on reopen, not halfway through a partition.
