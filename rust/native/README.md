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
- `sakura-native-ffi` is the only `staticlib`. It uses `std` to own the C ABI
  wrappers and catches a panic at every export, returning the typed
  `InternalError` status rather than unwinding into C++. Its fixed-width V2
  UTF-16 scan ABI delegates to the internal kernels in `sakura-simd`. The same
  archive also owns the stateless URI candidate, the replay-only Output state
  candidate, and the callback-free Output authority provider. All three copy
  caller input and retain no foreign pointer. Candidates have no callback or
  external side-effect authority. In an explicit
  `SAKURA_OUTPUT_BACKEND_RUST` build, the provider owns the Output model behind
  a separate opaque-token family while its C++ adapter owns advisory listener
  dispatch; C++ remains the default authority and there is no runtime fallback
  between providers.

`sakura_native_ffi.lib` is therefore the only Rust library added to each
native product/test link. The old `SakuraRustCore*` MSBuild property and target
names remain compatibility shims, but resolve to this archive and to the
`rust/native` workspace; they do not identify a second static library.

The workspace keeps its own `Cargo.lock` and `rust-toolchain.toml`. Native
MSBuild inputs include only these files and the native source closure, so a
SENP-only change cannot invalidate the native Cargo stamp.

Both native Cargo profiles use unwind semantics so the FFI crate can contain
panics at the ABI boundary with `catch_unwind`; the no_std SIMD rlib does not
install a panic handler.

Output provider selection is independent of UTF-16/SIMD backend selection.
Both paths use this same static archive, but the stateful Output lifecycle must
not be coupled to CPU feature detection or the stateless SIMD dispatch table.

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
