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

1. verifies that the clean source, Windows identity, and power identity stayed
   fixed;
2. immediately copies `tests1.exe` into a private transaction;
3. checks `OutputServiceRustProvider.obj` unresolved references (none for C++,
   exactly the frozen seven v1 exports for Rust);
4. checks that the one native Rust archive defines exactly those seven exports;
5. runs the copied executable with
   `CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle`;
6. publishes the directory by one atomic rename.

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
