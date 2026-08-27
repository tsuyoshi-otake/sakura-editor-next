# Paired C++ / Rust GUI startup evidence

`measure-output-startup.ps1` is the opt-in Issue #274 runner for comparing two
explicit `sakura.exe` artifacts. It measures the GUI startup boundary using the
same HWND, caption-readiness, input-idle, scrollbar-layout, process-identity,
and bounded cleanup mechanics as `measure-startup-performance.ps1`.

The Output provider `tests1.exe` benchmark is separate evidence. This runner
never starts `tests1.exe`, and a provider microbenchmark result must not be
reported as GUI startup evidence.

## Usage

For a bounded GUI smoke test, run from the repository root with two distinct
executable files and opt in to collect-only mode:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 `
  -CppSakuraExe .\build\evidence\output-startup\cpp\sakura.exe `
  -RustSakuraExe .\build\evidence\output-startup\rust\sakura.exe `
  -StartupSample .\tools\startup-benchmark-sample.md `
  -AffinityMask 1 `
  -CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1
```

The defaults are deliberately evidence-sized: five warmup launches and 30
measured launches per backend (70 total launches). Each slot runs both
backends; the first backend alternates deterministically, starting with C++
unless `-FirstBackend rust` is selected.

Before the first launch, the runner copies each supplied artifact into a fresh,
measurement-owned campaign bundle. Qualified evidence requires a canonical
runtime-stage directory and its `.sakura-runtime-stage.json` receipt for each
backend. Every receipt-declared file is copied and its size and SHA-256 are
verified before each launch and after the campaign. Collect-only mode may use a
self-contained executable without a receipt; that fallback bundle contains only
the copied `sakura.exe`. Every bundle also has an exact UTF-16LE-BOM/CRLF sibling
`sakura.exe.ini` whose
`[Settings]` section sets `MultiUser=0`. Sakura therefore resolves
`-PROF=<generated-name>` below that bundle, rather than falling back to
`%APPDATA%` or a user configuration. The sidecar is checked byte-for-byte
before every launch; a missing, reparse-point, or ambiguous sidecar fails
closed. The per-launch profile is hashed before deletion and deleted and
verified after each launch. The source and copied executable hashes are
checked before and after the campaign, and the bundle cleanup result is
recorded without serializing paths or profile contents.

The runner never discovers or silently copies adjacent DLLs. Qualified mode
copies only the dependency closure declared by the canonical runtime-stage
receipt. The exe-only fallback is limited to collect-only diagnostics and does
not qualify as dependency-closure evidence. A missing or altered receipt file
is an integrity failure, not a reason to broaden the copy set.

`-AffinityMask` must be nonzero. The editor is created suspended, placed in a
run-owned kill-on-close Job Object, assigned the requested affinity, and only
then resumed. The runner reads the mask back with `GetProcessAffinityMask` and
fails closed when the requested value is unavailable, cannot be applied, or
does not read back exactly. The editor's run-owned descendants are checked
against the same mask when they remain live. The bundle directory is the
process working directory. Startup waits are bounded at 30 seconds and
process/profile cleanup at three seconds per launch. Cleanup is identity-safe:
PID, creation identity, executable identity, and job membership are checked,
parents are stopped before children, and an exact bundle image-path sweep must
be empty after cleanup.

If any launch cannot verify process cleanup or profile cleanup, the campaign
stops immediately before scheduling the next backend. The report retains the
typed failed launch and a `termination` record with `type=cleanup-unverified`,
the completed count, and the suppressed count; `acceptance.qualified` and
`pass` are false. This prevents a survivor or contaminated profile from
multiplying later GUI launches.

Useful bounded overrides are available for local experiments:

| Parameter | Default | Meaning |
|---|---:|---|
| `-WarmupLaunches` | `5` | Warmup launches per backend; values below 5 are rejected. |
| `-MeasuredLaunches` | `30` | Timed launches per backend; values below 30 are rejected. |
| `-FirstBackend` | `cpp` | Backend that starts slot 1; slot order then alternates. |
| `-Platform` | `x64` | Manifest and runtime-stage platform; qualified mode accepts only x64. |
| `-Configuration` | `Debug` | Manifest and runtime-stage configuration (`Debug` or `Release`). |
| `-AffinityMask` | `1` | Required nonzero process-affinity mask. |
| `-CppBuildManifest`, `-RustBuildManifest` | none | Required qualified provenance manifests. |
| `-CppRuntimeStageDirectory`, `-RustRuntimeStageDirectory` | none | Required canonical runtime stages with receipts. |
| `-CollectOnly` | off | Explicit smoke/diagnostic mode; permits smaller counts but never qualifies evidence or passes performance gates. |
| `-ResultDirectory` | `build/output-startup-benchmarks` | Directory for the report (not serialized into it). |

Without `-CollectOnly`, fewer than five warmups or 30 measured launches per
backend is rejected. `-CollectOnly -WarmupLaunches 1 -MeasuredLaunches 1` is
available for a bounded GUI smoke test; its report always has
`acceptance.qualified=false` and `pass=false`.

The script rejects the same executable path or SHA-256 for both roles, refuses
to start while either exact artifact already has a running process, and checks
both artifact hashes again after the campaign. It copies the fixed sample into
campaign-owned storage and verifies both the source and copy before every
launch and after the campaign.

Qualified mode fails closed unless both build manifests match the current
checkout, artifact hashes, explicit Output selectors, UTF-16 C++ selector,
Debug/Release configuration, runtime-stage receipt and dependency closure,
Windows image, power mode, parallelism, MSVC/Rust toolchains, Cargo lock,
package plan, and build-command identities. The two manifests must agree on the
shared environment identities. Do not hand-author these fields after a build;
the build orchestration that creates the artifacts must emit the manifests.
Until that reproducible producer exists and its outputs are supplied, use
collect-only mode and treat the result as diagnostic evidence only.

For a collect-only Debug smoke pair, build and stage one artifact at a time
because both configurations produce `x64\Debug\sakura.exe`:

```powershell
$env:SAKURA_OUTPUT_BACKEND = 'cpp'
$env:SAKURA_UTF16_BACKEND = 'cpp'
$env:SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'
$env:SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'
$env:SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'
$env:SKIP_CREATE_GITHASH = '1'
.\build-dev.bat x64 Debug
py -3 .\tools\build\sakura_build.py stage runtime --context msvc-x64-debug --product sakura_app
New-Item -ItemType Directory -Force .\build\evidence\output-startup-artifacts\Debug | Out-Null
Copy-Item -LiteralPath .\build\staging\msvc-x64-debug\sakura-editor `
  -Destination .\build\evidence\output-startup-artifacts\Debug\cpp -Recurse
Get-FileHash .\build\evidence\output-startup-artifacts\Debug\cpp\sakura.exe -Algorithm SHA256

$env:SAKURA_OUTPUT_BACKEND = 'rust'
$env:SAKURA_UTF16_BACKEND = 'cpp'
.\build-dev.bat x64 Debug
py -3 .\tools\build\sakura_build.py stage runtime --context msvc-x64-debug --product sakura_app
Copy-Item -LiteralPath .\build\staging\msvc-x64-debug\sakura-editor `
  -Destination .\build\evidence\output-startup-artifacts\Debug\rust -Recurse
Get-FileHash .\build\evidence\output-startup-artifacts\Debug\rust\sakura.exe -Algorithm SHA256
```

Restore or clear these measurement-only environment variables before unrelated
builds. This sequence is still collect-only unless the same controlled build
operation also emits the required provenance manifests. The canonical stage
receipt proves the copied runtime closure; it does not by itself prove the
backend selectors or build environment.

## Payload-free report

Each run writes one `paired-startup-<run-id>.json` report. The report contains:

- the repository source state, scripts, C++ artifact, Rust artifact, verified or
  explicitly unverified provenance, runtime closure, host, sample,
  profile-policy, and per-launch profile SHA-256 identities;
- the deterministic launch order and its SHA-256;
- per-launch startup milestone timings (`processApiReturnMs`, `topLevelHwndMs`,
  `visibleMs`, `captionReadyMs`, `inputIdleMs`, and `documentReadyMs`);
- median, nearest-rank ceiling p95, minimum, maximum, mean, successful count,
  and excluded count for each backend and warmup/measured phase; and
- a measured C++/Rust `documentReadyMs` paired-delta and regression summary,
  with a median gate of at most 2% and 1 ms absolute Rust regression and a
  p95 gate of at most 5% relative regression;
- typed launch status (`succeeded`, `timeout`, `affinity`, `survivor`,
  `profileCleanup`, or `startup`) plus cleanup and survivor counts; and
- `startupGatePass` plus an explicit `adoption.decision=HOLD` and
  `adoptionEligible=false`.

`payloadFree` is true only for this fixed schema. The report intentionally does
not contain executable/sample/profile/output paths, document text, captions,
command lines, raw exception messages, or environment variable values.
Failed or surviving launches remain typed records with `excluded=true` and
null timings; they never enter the statistics. Acceptance requires every
scheduled warmup and measured launch to succeed, every cleanup to verify, and
the requested affinity read-back to pass. `acceptance.qualified` reports this
collection qualification separately from `performance.pass`; the top-level
`pass` is true only when both are true. This is only the Issue #274 startup gate.
It never changes the adoption decision: correctness, provider workload,
build/package, Debug/Release, MinGW, and hardware evidence remain separate hard
gates. A collect-only report therefore cannot be mistaken for adoption evidence
even if its smoke timings happen to satisfy the synthetic thresholds.

The console prints the report path. The path is a retrieval aid and is not part
of the JSON payload.

## Self-test

Run the helper self-test before a real campaign:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 -SelfTest
```

It exercises median/p95 statistics, deterministic interleaving and ordering,
the payload-free schema rejection, affinity metadata conversion and nonzero
mask validation, exact portable-sidecar and artifact-bundle identity/cleanup,
profile hashing/cleanup, parent-first PID identity helpers, and the paired
performance gates. It does not launch `sakura.exe` or any other GUI process.
Do not use it as a substitute for the full 35-launch-per-backend campaign.
