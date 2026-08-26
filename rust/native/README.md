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
  UTF-16 scan ABI delegates to the internal kernels in `sakura-simd`.

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
