# Output provider measurement harness

`measure-output-provider.ps1` is an opt-in measurement runner for Issue #274.
It starts two explicitly supplied `tests1.exe` binaries: one compiled with the
C++ Output provider and one compiled with the Rust Output provider. The test
itself is a disabled GoogleTest, so ordinary `tests1.exe` runs do not perform
timing work.

Run the script from the repository root and supply distinct executable paths:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-provider.ps1 `
  -CppTests1 .\x64\Release\tests1-cpp.exe `
  -RustTests1 .\x64\Release\tests1-rust.exe `
  -Configuration Release `
  -AffinityMask 1
```

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

`analysis-v1.json` records only provider labels, executable SHA-256/size,
the applied affinity mask, payload-free OS/CPU identity, pair result metadata,
aggregate counts, and statistics. It deliberately omits executable paths,
output paths, command lines, captured stdout/stderr paths, and user data. The
console may print the analysis path for retrieval. Each C++/Rust executable is
hashed before the first pair, checked before every launch, and checked again
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

The script rejects malformed, incomplete, payload-bearing, failed, timed-out,
or surviving-process runs. It alternates provider order between pairs and
owns bounded cleanup of only the exact process tree it launched.

`-CollectOnly` is an explicit raw-collection mode. It still validates schema,
semantics, and process cleanup, but always writes `pass=false` and never emits
acceptance evidence. Use it only for exploratory collection or debugging.

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
