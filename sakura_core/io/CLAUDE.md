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

## UTF-8/CESU-8 Rust candidate (#443)

`DocumentUtf8CodecCandidate.{h,cpp}` is a comparison-only adapter over the Rust
`sakura_document_codec_utf8_{decode,encode}_{measure,write}_v1` exports. `CUtf8`, `CCesu8`
and their callers (`CFileLoad`, `CIoBridge`) stay legacy-production: nothing outside tests
calls the adapter, and linking the archive does not select a backend. The ABI version and the
typed status are shared by every document-codec candidate in `DocumentCodecAbi.h`; a
capability adds only its own descriptors, bounds and result.

One call converts one whole buffer. A decode reads UTF-8 or CESU-8 bytes and produces UTF-16
units; an encode reads units and produces bytes plus a loss flag. `variant` is the `ECodeType`
number (`CODE_UTF8` 4, `CODE_CESU8` 7) and any other value is refused. The byte rules are the
frozen behavior of `CUtf8::Utf8ToUni` and `_UnicodeToUTF8`, not a new specification: a decode
never loses input (a byte it cannot read becomes the unit `0xDC00 + byte`), CESU-8 refuses every
four-byte form and reads a surrogate pair as two three-byte forms, and an encode maps a lone
`U+DC00..U+DCFF` back to its byte and any other lone surrogate to `?` with the loss flag set.
An input is bounded to 2,147,483,647 elements, the `int` length domain of the converters, and a
longer one is refused before it is read.

Measure and write are independent calls with no receipt between them. A failed call leaves every
caller byte unchanged, a write with too little capacity writes nothing, and a successful write
leaves the unused suffix of the array intact. An empty input and an empty array ignore their
addresses. The result and the output array are written, so the result may share bytes with
nothing else the call touches and the array with nothing else at all; the request, the buffer
descriptor and the input are read-only and may share bytes with one another. The check order is
documented in `rust/native/sakura_native_ffi/src/utf8_codec.rs`; the C++ structs pin the same
sizes, offsets and wire numbers with `static_assert`.

`Utf8CodecContract` freezes the production converters over
`rust/native/fixtures/utf8-codec-corpus.tsv`. `Utf8CodecRustCandidate` compares the candidate
with them and with the corpus: every corpus row at the end of a guard page and at an odd
address, the recorded golden digests, every byte and byte pair, short sequences and truncated
forms over boundary alphabets, every BMP unit, the whole 1,048,576-pair high/low surrogate grid,
seeded random buffers of up to 4,096 elements with ASCII-prefix handoffs, and a replay through
`CFileLoad::ReadLine` and `CIoBridge::ImplToFile`. The ABI rules (headers, pointers, capacity,
check order, overlaps) run once per direction, and conversions run with input and output ending
at a guard page. The injected-panic paths and the acceptance of an input at the 2 GiB bound are
covered by the Rust unit tests only, because C++ can neither inject a panic nor supply that much
input.

Known production limit, outside this candidate: `_UnicodeToUTF8` sizes its worst case as three
bytes per unit in an `int`, which overflows above 715,827,882 units. The candidate has no such
limit below its own bound, and the suites compare the two only far below it.
