# Output provider artifact producer

`prepare-output-provider-artifact.ps1` produces the only qualified input accepted
by `measure-output-provider.ps1`. It is intentionally separate from
`prepare-output-startup-artifact.ps1`: the provider campaign owns a copied
`tests1.exe` and its provider-lifecycle proof, while the startup campaign owns a
staged `sakura.exe` runtime closure.

Run it from the repository root on a clean checkout:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/prepare-output-provider-artifact.ps1 `
  -Backend cpp -Platform x64 -Configuration Release

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/prepare-output-provider-artifact.ps1 `
  -Backend rust -Platform x64 -Configuration Release
```

The producer invokes `build-sln.bat x64 <Configuration>` with an exact Output
selector, `SAKURA_UTF16_BACKEND=cpp`, both production-package flags disabled,
telemetry and assembly listings disabled, stable generated Git metadata, bounded
parallelism, English MSVC diagnostics, and MSBuild node reuse disabled. Output and
UTF-16/SIMD selection therefore remain independent.

## Transaction and authority proof

Only one producer may own a Debug or Release build at a time. The lock is scoped
to the configuration rather than the backend because both builds overwrite
`x64/<Configuration>/tests1.exe`. After a successful build, the producer:

1. records the exact provider object and compile-log identities, then enters a
   provider-compile fence under that configuration lock. The fence validates
   the fixed `build/x64/<Configuration>/sakura_core/OutputServiceRustProvider.obj`
   path (including regular-file and no-reparse checks), deletes only that exact
   object when present, and proves it is absent. It never deletes a tlog,
   directory, or unrelated build output. The normal build starts only after
   this absence proof, so an interrupted prior build cannot satisfy the
   producer from a stale provider object. The receipt records
   `providerObjectFreshnessMethod=exact-object-absence-v1` and the raw boolean
   `providerObjectAbsentBeforeBuild=true`; both are part of
   `selectorContractSha256` for Debug and Release.
2. verifies that the clean source, Windows identity, and power identity stayed
   fixed;
3. immediately copies `tests1.exe` into a private transaction;
4. proves the selected provider at the object/compile boundary. Debug and
   non-LTCG objects use `dumpbin /symbols` directly and require the frozen
   seven provider imports for Rust (and none for C++). Release MSVC `/GL`
   objects intentionally report `File Type: ANONYMOUS OBJECT`; the producer
   therefore fails closed unless the fresh `sakura.tlog/CL.command.1.tlog`
   contains exactly one `OutputServiceRustProvider.cpp` compile command with
   `/GL`, exactly one standalone `/D SAKURA_OUTPUT_BACKEND_RUST` for Rust, or
   no such selector definition for C++. The tlog and provider object hashes,
   sizes, and object format are recorded in the payload-free selector receipt.
   This is a compile-selector proof, not an unresolved-reference claim;
5. checks that the one native Rust archive defines exactly those seven exports;
6. runs the copied executable with
   `CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle`;
7. publishes the directory by one atomic rename.

The object-absence fence is an interrupted-build recovery contract. A failed or
interrupted producer may leave build outputs in the shared build directory, but
the next producer invalidates only the expected provider object before invoking
the ordinary build. A separate manual build running outside this producer lock
is unsupported; it can race the proof and must not be used for qualified
evidence. The consumer rejects manifests from the old v1 shape that do not carry
the freshness fields and their hash-bound selector proof.

The Release compile-selector receipt, exact seven archive definitions, and
successful runtime ownership probe are separate required proofs. A compile
selector or archive receipt alone cannot establish semantic provider ownership;
the runner validates all three before accepting a qualified pair.

The manifest record is `output-provider-build-manifest` schema v1. It contains
only bounded identities, hashes, counts, statuses, selectors, and transaction
booleans. It contains no paths, commands, console output, exception messages, or
editor data. `runtimeClosureMode=exe-only` means the copied `tests1.exe` passed the
standalone lifecycle probe; `runtimeClosureSha256` is a receipt over its hash and
size, not a renamed executable hash and not a GUI dependency-stage receipt.

Qualified paired measurement requires both manifests:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/measure-output-provider.ps1 `
  -CppTests1 build/output-provider-artifacts/release/cpp/tests1.exe `
  -RustTests1 build/output-provider-artifacts/release/rust/tests1.exe `
  -CppBuildManifest build/output-provider-artifacts/release/cpp/output-provider-build-manifest.json `
  -RustBuildManifest build/output-provider-artifacts/release/rust/output-provider-build-manifest.json `
  -Configuration Release
```

The producer and runner do not authorize adoption. The Issue #274 decision remains
HOLD until all required Debug/Release, startup, MinGW, incremental, size, host, and
ledger gates pass.

## Cheap verification

The self-test never builds native code:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/prepare-output-provider-artifact.ps1 -SelfTest
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/prepare-output-provider-artifact.ps1 -SelfTest
py -3 -m unittest tools.build.tests.test_prepare_output_provider_artifact -q
```
