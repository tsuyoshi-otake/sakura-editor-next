# Native Rust workspace

This workspace owns Rust code linked into the native Sakura product and test
executables. It is intentionally separate from [`../senp`](../senp), which
owns SENP package validation, host/tool binaries, and extension guests.

The native link boundary is deliberately one archive:

- `sakura-simd` is a `no_std` `rlib` containing scalar and audited x86-64 ISA
  kernels. It has no C ABI exports and no panic handler.
- `sakura-unicode-core` is an allocation-free `no_std` `rlib` containing only
  strict Unicode scalar-value UTF-8 primitives. CESU-8 and subsystem-specific
  replacement or escaping policies remain outside this crate.
- `sakura-document-codec` is a safe, allocation-free `no_std` `rlib`
  (`forbid(unsafe_code)`) that owns the comparison-only encoded-EOL scan
  for UTF-16LE/BE and UTF-32LE/BE and the comparison-only one-shot UTF-8 and
  CESU-8 conversion with loss reporting, which builds on the strict scalar
  primitives of `sakura-unicode-core`. It has no C ABI and no panic handler.
- `sakura-native-ffi` is the only `staticlib`. It uses `std` to own the C ABI
  wrappers and catches a panic at every export, returning the typed
  `InternalError` status rather than unwinding into C++. Its fixed-width V2
  UTF-16 scan ABI delegates to the internal kernels in `sakura-simd`. #239
  keeps all nine production UTF-16 cells in C++. The Rust scan ABI and kernels
  remain available for tests and explicit non-production MSVC comparisons. The
  archive also owns the stateless encoded-EOL scan and UTF-8/CESU-8 conversion
  candidates alongside the stateless URI candidate, the replay-only Output state
  candidate, and the callback-free Output authority provider. The last three
  copy caller input and retain no foreign pointer; the two document-codec
  candidates read caller input in place and retain none. Candidates have no
  callback or external side-effect authority. In a
  `SAKURA_OUTPUT_BACKEND_RUST` build, the provider owns the Output model behind
  a separate opaque-token family while its C++ adapter owns advisory listener
  dispatch. Rust Output is the MSVC default (#274). MinGW uses C++ Output.
  An explicit C++ build is the rollback path. Each build uses a single Output
  provider.

The worker-thread ABI (`sakura_worker_thread_*_v2`) is also production on
MinGW, where it links from `libsakura_native_ffi.a` (#435).

The only Rust library added to each native product/test link is
`sakura_native_ffi.lib` on MSVC or `libsakura_native_ffi.a` on MinGW.
The old `SakuraRustCore*` MSBuild property and target
names remain compatibility shims, but resolve to this archive and to the
`rust/native` workspace; they do not identify a second static library.

[`exported-symbols.txt`](sakura_native_ffi/exported-symbols.txt) lists the
native Rust C ABI exports checked against the built archive by
`tools/verify-native-rust-exports.py`. For an intentional ABI change, inspect
the rebuilt archive and update the sorted list with the reviewed names.

The workspace keeps its own `Cargo.lock` and `rust-toolchain.toml`. Native
MSBuild inputs include only these files and the native source closure, so a
SENP-only change cannot invalidate the native Cargo stamp.

Both native Cargo profiles use unwind semantics so the FFI crate can contain
panics at the ABI boundary with `catch_unwind`; the no_std SIMD rlib does not
install a panic handler.

Output provider selection is independent of UTF-16/SIMD backend selection.
Both paths use this same static archive, but the stateful Output lifecycle must
not be coupled to CPU feature detection or the stateless SIMD dispatch table.

## C ABI threading, reentry, and failure

Concurrent callers must keep input storage valid for each call and use
independent writable outputs, including slots passed to destroy or join.

| Export family | Threading and reentry | Failure |
| --- | --- | --- |
| Output provider (7) | Callable from any thread. The provider registry lock serializes calls, including measure/write individually. No callbacks. | A caught panic returns provider InternalError. Once poisoned, a token returns InternalError on every call except destroy, without accessing the model. |
| Output shadow (6) | Callable from any thread. The model registry lock serializes model access. No callbacks. Measure and write are separate calls and can see an intervening mutation. | A caught panic returns shadow InternalError. Once poisoned, a token returns InternalError on every call except destroy, without accessing the model. |
| URI candidate (4) | Stateless, with independent measure/write calls and no callbacks. | Typed validation or capacity errors. A caught panic returns URI InternalError. Consume output only on Ok. |
| Document codec EOL scan (2) | Stateless, with independent measure/write calls and no callbacks. | Typed validation or capacity errors. A caught panic returns InternalError. Every failure leaves all caller bytes unchanged. Consume output only on Ok. |
| Document codec UTF-8/CESU-8 conversion (4) | Stateless, with independent decode and encode measure/write calls and no callbacks. | Typed validation or capacity errors. A caught panic or a failed staging reservation returns InternalError. Every failure leaves all caller bytes unchanged. Consume output only on Ok. |
| UTF-16 scan (9) | Concurrent readers share the immutable CPU snapshot. No callbacks. | NotInitialized before snapshot publication, Unsupported for missing CPU/OS support, and typed span errors. A caught panic returns InternalError. The result slot is initialized to the input length before scanning. |
| Byte scan (3) | Concurrent readers share the immutable CPU snapshot. No callbacks. Accepted G2 exception: comparison-only through the CPU/OS capability gate. Before adding any production caller, add a versioned byte operation policy with an operation ID and minimum length. | NotInitialized, Unsupported, and typed span errors. A caught panic returns InternalError. The result slot is initialized to the input length before scanning. See [#433](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/433). |
| Worker thread (2, ABI v2) | One-shot launch: one successful start runs one body on a new thread and yields exactly one join terminal. The registry lock is released before waiting and is never held across the callback. The opaque context must stay valid for the body, including after a self-join detaches. | Spawn failure returns SpawnFailed. Join consumes the token before waiting. Ok writes the redacted body outcome (Completed, ThrewException, or ThrewUnknown). A self-join detaches and returns WouldDeadlock. Competing joins return NotFound after the token is claimed. Rust panics return InternalError. An undefined callback outcome also returns InternalError. |
| Initialize (1) | Copies the inputs and publishes one immutable snapshot with OnceLock. Concurrent identical calls succeed. Conflicting calls receive ConflictingInitialization. No callbacks. | Invalid inputs receive typed errors. A caught panic returns InternalError. See [#433](https://github.com/tsuyoshi-otake/sakura-editor-next/issues/433). |

An Output export that panics after looking up a token poisons that token
unless it can prove that no change occurred. Poison is recorded under the
registry lock before another caller can observe the token. A poisoned token
still accepts destroy, which releases its model and any provider receipts,
zeros the caller's slot, and returns Ok. Other tokens remain usable.

| Output export | Provider | Shadow |
| --- | --- | --- |
| Create | Publishes a new token only on success. | Publishes a new token only on success. |
| Apply | A panic in request preparation preserves the token. A panic from the start of model mutation poisons both the provider token and its backing model entry. | A panic in request preparation preserves the token. A panic during model mutation poisons it. |
| Stop | A panic before model mutation preserves the token. A panic during model mutation poisons the token and its backing entry. | A panic during terminal-state mutation poisons the token. |
| Snapshot measure | Model encoding stages bytes before commit. A panic during receipt registration poisons the token and removes the tentative receipt before any eviction. | Encoding works on local bytes and commits the cache once after encoding. A panic before commit preserves the token. |
| Snapshot write | A panic before modifying a receipt preserves the token. A panic during removal of a rejected receipt poisons it. A successful write leaves the receipt available. | Encoding uses the same staged cache commit as measure. Destination copy and length publication follow validation with no fallible model work. |
| Active channel | Reads model metadata only. A panic preserves the token. | No exported active-channel call. |
| Destroy | A panic before removing receipts or the model preserves the token. If receipts have been removed and destruction panics before model removal, the token is poisoned and destroy can be retried. | Removal of the occupied entry is the single commit. A panic before removal preserves the token and destroy can be retried. |

## Output provider test isolation

The Output provider retains at most 64 snapshot measurements per process,
across all provider tokens. A new measurement can evict an older receipt from
another provider. A write with an evicted receipt fails closed; the caller must
measure again. This budget is separate from forged-receipt rejection, which
must not consume a still-retained valid receipt.

Tests using that registry hold the test-only lifecycle guard for their whole
case. Otherwise a capacity test can evict a contract test's receipt between
measure and write, even though they use different provider tokens. Keep the
normal Cargo test scheduler and the worker threads inside concurrency tests;
do not replace this isolation with a global `--test-threads=1` CI setting.
The cross-provider eviction regression explicitly orders a foreign worker's
measurements between a caller's measure and write, checks rejection without
destination writes, and verifies recovery through a fresh measurement.
