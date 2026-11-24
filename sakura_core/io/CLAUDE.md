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
writers. Each reader owns a separate converter constructed from the inherited
encoding and MIME options. Prepare never shares mutable converter instances.
This isolates instance state, not any process-wide state inside encoding backends.
Prepared partitions copy the encoded NEL/LS/PS byte sequences along with the
EOL flags. They start with an empty decoded-line cache and a zero decoded
UTF-7 offset, including when a previously used reader is prepared again.
Parent progress must not seed a new partition. The two `FileLoadOptionsTest`
Prepared regressions cover these state transitions through real file reads.
This state initialization is independent of the mapping lease.

The extended-EOL policy is captured on FileOpen and inherited by Prepare. UTF-7 decoded splitting and UTF-16 scanning use the same captured state as the byte scanner. A settings change takes effect on reopen, not halfway through a partition.

## Encoded EOL Rust candidate (#441)

`DocumentEncodedEolCandidate.{h,cpp}` is a comparison-only adapter over the Rust
`sakura_document_codec_scan_eol_measure_v1` and `..._write_v1` exports. `CFileLoad`
and the `CReadManager` partition arithmetic stay legacy-production: nothing outside
tests calls the adapter, and linking the archive does not select a backend.

The scan range is `[start, length)` over a byte image, and the finish offset is the
supplied end, trailing partial units included. Records are byte offset, byte width and
`EEolType`; a CR LF pair is one record of two units. Measure and write are independent
calls with no receipt between them. A failed call leaves every caller byte unchanged,
and a write with too little capacity writes nothing. The check order and the
1,048,576-record ceiling are documented in
`rust/native/sakura_native_ffi/src/encoded_eol.rs`; the C++ structs pin the same sizes,
offsets and wire numbers with `static_assert`.

`EncodedEolContract` freezes the production scan over
`rust/native/fixtures/encoded-eol-corpus.tsv`. `EncodedEolRustCandidate` compares the
candidate with the production scan and the corpus: every aligned start and every byte end
of each corpus case under a guard page, and the generated partition-threshold images. The
injected-panic paths are covered by the Rust unit tests only.

The module model declares the scan in the `document-file-codec` domain
(`src/main/modules/modules.json`): `sakura-encoded-eol-cpp-provider` is `legacy-production` in
all six contexts, and `sakura-encoded-eol-rust-candidate` (MSVC) and
`sakura-encoded-eol-rust-gnu-candidate` (MinGW) are side-effect-free `candidate-shadow` records.
The MinGW leg proves that both suites ran with `tools/verify-gtest-suite-executed.py`
(`.github/CLAUDE.md`).
