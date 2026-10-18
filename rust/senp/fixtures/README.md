# SENP v2 event/effect protocol fixtures

`effect-protocol.jsonl` is shared by the native `SenpEffectProtocol` tests and
Rust host integration tests. Each line contains `name`, `valid` and the literal
wire `input`. Optional `fill` replaces one `~fill~` marker with that many ASCII
`x` bytes so boundary cases stay compact in source control. There are 78 cases:
27 accepted and 51 rejected. Missing fixtures or unexpected counts fail tests.

The v1 world and executable dispatch are unchanged. The v2 WIT world compiles
separately. `effect_bridge` exhaustively maps its records/variants to the wire
DTOs in both directions; accepted fixtures also exercise these conversions.
Runtime binding/dispatch and the session state machine are G03 work.
The stateless codec does not authorize a tool, select an account, retain a
request, acknowledge delivery, or publish a contribution.

## Wire contract

- Every envelope contains `protocol`, `sequence`, `sessionGeneration`, `body`.
  Protocol is exactly 2. Sequence and session generation are positive.
- Every record requires every declared member and rejects unknown members.
  Adjacent variants contain exactly `type` and `data`, with camelCase names.
  Records and variants are JSON objects only: positional arrays and the
  `{"variant":null}` spelling of a unit variant are rejected by both codecs.
- Strings are UTF-8 JSON, with valid Unicode scalar values. No BOM, comments,
  trailing commas, duplicate decoded keys or extra root values are accepted.
  Free text (labels, tooltips, messages, markdown, arguments) may carry an
  escaped U+0000; `nulInLabel` pins that codec-level parity. Native consumers
  must stay length-aware: the tree model still refuses such a page because
  Win32 TreeView text is NUL-terminated, so accepting it there would silently
  truncate the row.
- Counters are integers in `0..INT64_MAX`. Floats, exponent notation, negative
  zero and unsigned values above `INT64_MAX` do not become integer counters.
  A repository's `ahead` is WIT `u32`, so both codecs also reject it above
  `4294967295`.
- Operation context carries an ID and owner/workspace/account/request
  generations. Owner generation is positive. Account generation zero is
  bootstrap/unknown state and is never evidence of being signed out.
- IDs are 1-256 ASCII alphanumerics or `.-_:/`; operation IDs are at most 96
  bytes. Optional parent/command/icon IDs and cursors use empty strings. JSON
  null or an omitted field is not an alternative spelling.
- A frame is at most 1 MiB and 65,536 JSON nodes. Output serialization enforces
  the same aggregate budget before publishing bytes, without an intermediate
  JSON graph. The fixed schema has no recursive document or arbitrary JSON type.
- At most 64 effects, 256 tree items, 32 document sections, 16 table columns,
  256 table rows, 64 metadata fields, 32 repositories and 16 remotes are accepted.
  Tool/command arguments have at most 16 entries. A table row must match its
  column count. A text resource is at most 32 MiB; it is an opaque handle.
- Labels/titles/descriptions and tree-item context values are at most 1,024
  UTF-8 bytes; tooltip/argument/
  metadata values are at most 4,096; cursors 2,048; tool data 262,144; Markdown
  262,144. The frame budget still applies to their sum and JSON escaping.
- Partial tree pages require a next cursor; other terminal page states forbid
  it. Empty/failed pages contain no items. Repeated item IDs and a parent as its
  own child are rejected. Full cross-page ancestry checks belong to the provider.
- A batch cannot start the same read ID twice or complete a command twice.
  Reusing a tool argument name or workspace root ID is rejected. Failed tool
  completions cannot carry success data. Session-level replay and sequence/ack
  ordering are separate stateful checks in G03.

`tree-paging.jsonl` is shared by the guest crates and the native
`SenpTreeProvider` test. Each line is one scenario: `name`, `guest`, an ordered
`pages` list, and `children`, the final retained child ids of the scenario's
single parent after every page is applied in order. A page entry records
`requestCursor` (the cursor the host sends for that page; empty for the first),
`completion` (the `ToolCompleted` input the guest consumes: `readId`, `status`,
`data`, `message`), and `page`, the wire-shaped `publishTreePage` record the
guest is expected to publish (`viewId`, `parentId`, full `items`,
`nextCursor`, `revision`, `status`, `message`). Guests compare parentId, item
ids, nextCursor, revision and status; the host decodes the page through the
real provider conversion. `issues-offset-shift` exercises an offset page that
repeats the previous page's boundary item; `workflows-second-page` exercises a
plain second page. Every page of one scenario publishes the same `revision`
because a revision versions the parent's listing, not the page (#351).

## Verify

Run from the repository root after `build-sln.bat x64 Debug`. Rust dependencies
are already lockfile-pinned; these commands adopt no new package versions.

```powershell
cargo test --manifest-path rust/senp/Cargo.toml -p sakura-senp-host --locked --offline
./x64/Debug/tests1.exe --gtest_filter=SenpEffectProtocol.*:JsoncConfigurationSource.*
```

For an actual exchange, use a task-specific output directory under `~/tmp`.
Run each command under the normal bounded runner with process cleanup ownership.

```powershell
$protocolOutput = Join-Path $env:USERPROFILE 'tmp/senp-effect-protocol'
[System.IO.Directory]::CreateDirectory($protocolOutput) | Out-Null
$env:SENP_PROTOCOL_OUTPUT = Join-Path $protocolOutput 'cpp.jsonl'
./x64/Debug/tests1.exe --gtest_filter=SenpEffectProtocol.SharedFixturesAgreeOnGrammarBoundsAndTypedRoundTrips
$env:SENP_PROTOCOL_PEER_FILE = $env:SENP_PROTOCOL_OUTPUT
$env:SENP_PROTOCOL_OUTPUT = Join-Path $protocolOutput 'rust.jsonl'
cargo test --manifest-path rust/senp/Cargo.toml -p sakura-senp-host --test effect_protocol --locked --offline
$env:SENP_PROTOCOL_PEER_FILE = $env:SENP_PROTOCOL_OUTPUT
Remove-Item Env:SENP_PROTOCOL_OUTPUT
./x64/Debug/tests1.exe --gtest_filter=SenpEffectProtocol.AcceptsPeerSerializedFixturesWhenRequested
Remove-Item Env:SENP_PROTOCOL_PEER_FILE
Get-FileHash (Join-Path $protocolOutput 'cpp.jsonl'), (Join-Path $protocolOutput 'rust.jsonl')
```

Both peers must read 27 envelopes; both SHA-256 values must agree. Unicode,
control escaping, array order, field names and variant tags are included.
The 2026-10-18 Debug exchange produced identical
`7c9ba43064f311e4376c3d4d1980285c3632340db1ae840c89c323482769eb56`.
An aggregate-node regression failed before the bounded writer fix and passed
after it; the native Debug case fell from 18.3 s to 3.4 s in this local run.
These are machine-specific observations, not runtime performance guarantees.
