# Output provider measurement harness

`measure-output-provider.ps1` is an opt-in measurement runner for Issue #274.
It starts two explicitly supplied `tests1.exe` binaries: one compiled with the
C++ Output provider and one compiled with the Rust Output provider. A qualified
run also consumes the matching producer-owned
`output-provider-build-manifest` for each binary. The test itself is a disabled
GoogleTest, so ordinary `tests1.exe` runs do not perform timing work.

Run the script from the repository root and supply distinct executable paths:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-provider.ps1 `
  -CppTests1 .\x64\Release\tests1-cpp.exe `
  -RustTests1 .\x64\Release\tests1-rust.exe `
  -CppBuildManifest .\build\output-provider-artifacts\release\cpp\output-provider-build-manifest.json `
  -RustBuildManifest .\build\output-provider-artifacts\release\rust\output-provider-build-manifest.json `
  -Configuration Release `
  -AffinityMask 1
```

The canonical artifact flow is producer first, runner second. The provider
producer (`tools/prepare-output-provider-artifact.ps1`) builds `build-sln.bat`
for the requested x64/configuration/provider, freezes and verifies
`tests1.exe`, and publishes one transaction directory per provider. Use the
published executable and its adjacent manifest as the two runner inputs; do
not pair an executable from another build or source checkout:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-provider-artifact.ps1 `
  -Backend cpp -Platform x64 -Configuration Release -OutputDirectory .\build\output-provider-artifacts
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-provider-artifact.ps1 `
  -Backend rust -Platform x64 -Configuration Release -OutputDirectory .\build\output-provider-artifacts
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-provider.ps1 `
  -CppTests1 .\build\output-provider-artifacts\release\cpp\tests1.exe `
  -RustTests1 .\build\output-provider-artifacts\release\rust\tests1.exe `
  -CppBuildManifest .\build\output-provider-artifacts\release\cpp\output-provider-build-manifest.json `
  -RustBuildManifest .\build\output-provider-artifacts\release\rust\output-provider-build-manifest.json `
  -Configuration Release
```

For a non-collection run both `-CppBuildManifest` and `-RustBuildManifest` are
required. Supplying only one is rejected before any benchmark process starts.
The runner validates the producer v1 record, committed transaction, clean and
identical source state, x64/configuration/backend selectors, UTF-16 C++
selector, non-production flags, tests1 SHA-256/size, selector object/archive
proof (Release `/GL` uses the source-specific compile-selector receipt rather
than claiming unresolved references from an anonymous object), the exact
pre-build provider-object absence receipt (`exact-object-absence-v1`, raw
boolean `providerObjectAbsentBeforeBuild=true`), host/power/
toolchain/package/command proof, and the standalone provider
probe (`CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle`)
and explicit `runtimeClosureMode=exe-only`/`runtimeClosureSha256`. It rechecks both manifest identities and the current source state before
each launch and after the campaign. A pair mismatch (including source, host,
power, MSVC/Rust toolchain, Cargo lock, package plan, or build parallelism) is
fail-closed.

The freshness receipt is mandatory for both Debug and Release manifests. It is
bound into each provider's `selectorContractSha256`; missing, non-Boolean,
false, or differently named/valued fields are rejected. This prevents a stale
provider object left by an interrupted build from being relabeled as a current
measurement input. The runner does not accept a manual build outside the
producer's configuration lock as evidence.

The default run uses seven interleaved C++/Rust process pairs, two warmup
blocks, ten measured blocks, 256 Snapshot calls per snapshot block, and 512
fresh provider lifecycles per mutation block. Each benchmark process is pinned
to the nonzero `-AffinityMask` (default `1`); the workload applies and reads
back the mask before constructing a provider or reading timing counters, and
rejects a mask that is unavailable on the host. Thus the default emits at least
100,000 timed mutation operations for each provider. `-Pairs`,
`-WarmupBlocks`, `-MeasuredBlocks`, `-SnapshotIterations`, and
`-LifecycleIterations` are bounded overrides for smoke runs; `-AffinityMask`
must be nonzero and supported by both explicit executables. An override below
the acceptance thresholds is recorded as rejected evidence.

The default output directory is `build/output-provider-benchmarks`. Each run
gets a unique directory containing payload-free JSONL raw records, captured
stdout/stderr, and `analysis-v1.json`. The raw records include schema version,
provider/configuration/seed attribution, trace and result digests, status
counts, callback/drop counters, and QPC ranges. Text, channel IDs, owner IDs,
operation IDs, and other request payloads are never emitted.

`analysis-v1.json` records only provider selectors, executable SHA-256/size,
the applied affinity mask, payload-free OS/CPU identity, bounded validated
provenance, pair result metadata, aggregate counts, and statistics. Its
`provenanceComplete` flag is required for qualification. Provenance carries
only hashes, bounded identities, booleans, counts, selector proof, transaction
proof, and artifact rows; it deliberately omits executable paths, output paths,
command lines, captured stdout/stderr paths, labels/text/IDs, and user data.
The console may print the analysis path for retrieval. Each C++/Rust executable
is hashed before the first pair, checked before every launch, and checked again
after all pairs. Every raw metadata/sample/summary record carries the same
nonzero mask, and the analyzer validates it against the requested mask and
between providers.

An analysis is accepted only when all of the following hold:

- every pair completes with the expected provider and configuration;
- semantic digests, status counts, snapshots, callback counts, and drop counts
  match between providers;
- there are at least seven pairs, ten measured blocks, and 100,000 timed
  mutation operations per provider;
- the Rust/C++ median regression is at most 2 percent and p95 regression is at
  most 5 percent for every measured block; and
- no measured operation class has a Rust/C++ median or p95 ratio above 2.0.
- both producer manifests validate as a complete, matching provenance pair.

The script rejects malformed, incomplete, payload-bearing, failed, timed-out,
or surviving-process runs. It alternates provider order between pairs and
owns bounded cleanup of only the exact process tree it launched.

`-CollectOnly` is an explicit raw-collection mode. It may omit both build
manifests, but omission leaves `provenanceComplete=false`, records an
acceptance failure, always writes `pass=false`, and remains a `HOLD` for the
evidence ledger. If either manifest is
supplied, both are still validated even in collection mode. The mode still
validates schema, semantics, and process cleanup, and is intended only for
exploratory collection or debugging.

Use `-SelfTest` to exercise deterministic statistics, payload rejection,
acceptance thresholds, and the performance gate without starting test
processes:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-provider.ps1 -SelfTest
```

The workload is selected only through `OutputProviderFactory` and invokes the
common `IOutputService` interface. It covers all mutation kinds, Output/Log
channels, bounded and UTF-8-boundary text, replay/conflict/stale/rejection
cases, owner-generation replacement/fencing, snapshots, advisory drops and
unsubscribe, callback-originated Stop, external retry, repeated Stop, and
post-Stop typed results. Setup and listener registration are outside the timed
QPC regions; Stop is terminal, so each mutation iteration uses a fresh service
lifecycle. The compiled test derives its own Debug/Release configuration and
rejects a conflicting environment label; it also rejects factory/provider
health that is unavailable, not Ready, compiled for another kind, or marked as
test-overridden. The process affinity is applied and read back through
`GetProcessAffinityMask`/`SetProcessAffinityMask` before this workload starts;
the emitted mask is the verified read-back value.
