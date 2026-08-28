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

The final descendant-affinity check uses only the fresh current-live records
returned by `Get-TrackedOwnedProcesses` after its complete typed process census
and exact PID/creation/image-path identity checks. Historical records missing
from that current set are expected exits and are not affinity read-back targets.
The current set itself must be nonempty and must contain only unique, known
records with matching creation and image-path identities; a null/empty set,
unknown or duplicate PID, identity mismatch, failed census/identity query, or
failed affinity read-back remains a fail-closed launch failure. The bounded
no-GUI self-test plans and reads back exactly four current records from a
five-record historical set, while proving that its one expired record is not
read back.

Each launch also creates a unique run-owned startup-trace directory below the
copied artifact bundle and passes it to the shared startup probe. The probe
reads the trace before returning; the paired runner then retains only an
allowlisted event name, total count, `editor`/`control`/`unknown` role counts,
and a bounded ordered event projection of at most 256 items. Each ordered item
contains only `ordinal`, the allowlisted `event`, `role`, `value1`, `value2`,
and QPC-relative `elapsedMs`; `orderedEventsTruncated` records that the cap
was reached. The elapsed-time conversion requires a compatible launch QPC
clock and accepts a separate bounded diagnostic window through cleanup, rather
than using the launch timeout as its upper bound (the current diagnostic cap is
120 seconds). An empty, malformed, or
clock-incompatible trace is `trace-unavailable`. Trace paths, directory names,
event details, payloads, and raw command data are not part of paired evidence.
These trace fields are diagnostics only: event order and counts do not prove
successful readiness, and RAII `editor_ready` events can be emitted on an
early-return path. The directory is checked as an owned non-reparse path and
removed in `finally`; a failed removal is
`traceCleanupVerified=false`, makes the launch fail closed, and is treated as
an unverified cleanup terminal state.

The same result contains four fixed `startupDiagnostics` checkpoints:
`0.5s`, `2s`, `10s`, and `timeout`. Each checkpoint is converted to bounded
process metadata and an identity-safe root exit state/code. `active` means the
root handle still reports `STILL_ACTIVE` (259), not that the process exited.
If a reached checkpoint is `unavailable`, or root exit-state conversion is
invalid, the run is typed `diagnostic-unavailable` and cannot be reported as a
successful launch; a fast success may leave later checkpoints as
`not-reached`. An enabled trace with empty records, or malformed trace
observations, is likewise typed `trace-unavailable`.

Primary failure classification follows the raw launch result. If the launch
itself is unsuccessful, its `timeout`, `startup`, `survivor`, `profileCleanup`,
or `affinity` status remains primary even when diagnostics or trace collection
is unavailable; those observations remain secondary evidence. The
`diagnostic-unavailable`, `trace-unavailable`, and `trace-cleanup` statuses are
primary only when the raw launch succeeded and the other process/profile
cleanup gates are verified. A trace cleanup failure still makes cleanup
unverified and terminates the campaign.

If any launch cannot verify process, profile, or trace cleanup, the campaign
stops immediately before scheduling the next backend. The report retains the
typed failed launch and a `termination` record with `type=cleanup-unverified`,
the completed count, and the suppressed count; `acceptance.qualified` and
`pass` are false. This prevents a survivor or contaminated profile from
multiplying later GUI launches.

### Legacy Win32 path budget and compact campaign names

The normal Win32 path-text budget is 259 characters (the terminating NUL is
not counted). The runner keeps the existing `runId`-based sample and report
filenames, but derives a deterministic 16-hex SHA-256 token from that same
`runId` for the campaign-owned bundle, profile, and trace names. The evidence
`runId` is unchanged. C++ and Rust use one-character path role tokens of equal
length, so the two backend layouts have the same path budget.

The runner uses a two-phase check. Phase 1 plans every schedule entry, tracks
the actual maximum ordinal for each backend, and checks the generated paths on
the ordinary Win32 path surface before artifact resolution. It includes the
full profile path plus `\.sakura-platform\profile-authority.v1.tmp.` and 32
hexadecimal characters. A maximum of 259 is accepted; 260 or more is rejected
fail-closed before artifact resolution, bundle creation, or GUI launch. After
the qualified manifests and runtime-stage receipts have been validated, phase
2 extends the same plan with every canonical receipt-relative destination that
the bundle copier will create under each backend bundle, plus the generated
`sakura.exe.ini` sidecar. Collect-only mode finalizes with `sakura.exe` and its
sidecar. The phase-2 assertion remains before sample copy, bundle creation, and
GUI launch, so a nested closure destination cannot bypass the early check.

The typed `path-budget` envelope retains one payload-free summary with a
`phase` (`generated` or `finalized`), `maxPlannedLength`, `limit`, `margin`,
launch/ordinal counts, token lengths, and closure counts/maximums; it never
contains a raw path. The runner does not silently relocate to `%TEMP%` or
change the product manifest/LongPaths settings. Legacy names that embedded the
full timestamp/GUID remain the compatibility mapping for sample/report files;
only the bundle/profile/trace components are compacted.

If the caller's result root is so long that even the preserved report filename
cannot be opened, the fail-closed exit is still safe but on-disk envelope
retention cannot be guaranteed. The runner does not invent a second evidence
location; use a result root whose report path is writable when retaining the
typed envelope is required.

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
shared environment identities. The checkout must be clean. An empty clean
`git status --porcelain` is hashed as the UTF-8 empty string
(`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`);
null or non-string source-status input fails closed. During the campaign,
the runner rechecks the source state and both measurement-script hashes after
the launches, immediately before report serialization, and after atomic report
publication. A drift produces typed integrity evidence and keeps the decision
at HOLD.

Do not hand-author manifest fields after a build. Use
`prepare-output-startup-artifact.ps1`, which owns build, selector proof,
canonical staging, manifest generation, and atomic publication as one bounded
transaction. The paired verifier consumes the producer's configuration-aware
selector contract: Debug uses the `dumpbin-unresolved-refs-verified` object
proof (`coff-symbols`) with no unresolved C++ references and the complete fixed
v1 Rust reference set; Release uses the
`msvc-ltcg-compile-selector-verified` proof (`msvc-ltcg-anonymous`) and binds
the `/GL` compile-log identity and backend selector count. Both configurations
also require the Rust archive's `dumpbin-defined-exports-verified` result,
exactly seven defined v1 entry points, and the producer's archive-wrapped
contract hash. The receipt parser binds every `artifact_id`, role, source, and
destination to the canonical `build/staging/<context>/sakura-editor` and
`x64/<Configuration>` layout; a basename-only, absolute, traversing, or
otherwise ambiguous path is rejected.

Create both Debug artifacts below one new run-specific output root, then pass
the producer outputs to the paired runner:

```powershell
$artifactRoot = ".\build\evidence\output-startup-qualified\$(Get-Date -Format 'yyyyMMdd-HHmmss')"

pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-startup-artifact.ps1 `
  -Backend cpp -Platform x64 -Configuration Debug -BuildParallelism 1 `
  -OutputDirectory $artifactRoot
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\prepare-output-startup-artifact.ps1 `
  -Backend rust -Platform x64 -Configuration Debug -BuildParallelism 1 `
  -OutputDirectory $artifactRoot

pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 `
  -CppSakuraExe "$artifactRoot\Debug\cpp\runtime-stage\sakura.exe" `
  -RustSakuraExe "$artifactRoot\Debug\rust\runtime-stage\sakura.exe" `
  -CppBuildManifest "$artifactRoot\Debug\cpp\build-manifest.json" `
  -RustBuildManifest "$artifactRoot\Debug\rust\build-manifest.json" `
  -CppRuntimeStageDirectory "$artifactRoot\Debug\cpp\runtime-stage" `
  -RustRuntimeStageDirectory "$artifactRoot\Debug\rust\runtime-stage" `
  -Platform x64 -Configuration Debug `
  -StartupSample .\tools\startup-benchmark-sample.md -AffinityMask 1
```

Repeat with a different output root and `-Configuration Release` for the
Release cell. The producer refuses to overwrite an existing backend/configuration
transaction. A producer manifest created from a dirty checkout remains useful
for diagnostics, but the paired runner rejects it in qualified mode.

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
- the normalized measurement-argument schema and measurement-command SHA-256;
- the deterministic launch order and its SHA-256;
- per-launch startup milestone timings (`processApiReturnMs`, `topLevelHwndMs`,
  `visibleMs`, `captionReadyMs`, `inputIdleMs`, and `documentReadyMs`);
- per-launch `startupMilestones` presence booleans for process start, top-level
  window, visibility, caption, input idle, and document layout, together with
  bounded nullable timings, the nullable scrollbar maximum, and a fixed
  `missingMilestones` allowlist;
- timeout runs retain a typed `timeoutStage` (`window-discovery` or
  `readiness`) when the observed milestones localize the wait. The runner does
  not serialize the raw error; diagnosis comes from milestone presence only.
- per-launch `startupDiagnostics` with the fixed `0.5s`, `2s`, `10s`, and
  `timeout` checkpoints, bounded process metadata, and root exit state/code;
  `active` is the identity-safe `STILL_ACTIVE` state and is not an exit;
- per-launch `startupTrace` reduced to allowlisted event names, total and role
  counts, a maximum-256-item `orderedEvents` projection, and the
  `orderedEventsTruncated` flag, plus `traceCleanupVerified`; and
- median, nearest-rank ceiling p95, minimum, maximum, mean, successful count,
  and excluded count for each backend and warmup/measured phase; and
- a measured C++/Rust `documentReadyMs` paired-delta and regression summary,
  with a median gate of at most 2% and 1 ms absolute Rust regression and a
  p95 gate of at most 5% relative regression;
- typed launch status (`succeeded`, `timeout`, `affinity`, `survivor`,
  `profileCleanup`, `startup`, `diagnostic-unavailable`,
  `trace-unavailable`, or `trace-cleanup`) plus `failureStage`, cleanup, and
  survivor counts; and
- `startupGatePass` plus an explicit `adoption.decision=HOLD` and
  `adoptionEligible=false`.

`payloadFree` is true only for this fixed schema. The report intentionally does
not contain executable/sample/profile/output/trace paths, trace directory names,
document text, captions, command lines, raw exception messages, event `detail`,
or raw trace records. Ordered trace items are limited to the fixed fields
`ordinal`, allowlisted `event`, `role`, `value1`, `value2`, and `elapsedMs`.
Failed or surviving launches remain typed
records with `excluded=true`, explicit `startupMilestones` false/null fields
when no launch evidence exists, and null timings; they never enter the
statistics. Acceptance requires every scheduled warmup and measured launch to
succeed, every process/profile/trace cleanup to verify, and the requested
affinity read-back to pass. `acceptance.qualified` reports this
collection qualification separately from `performance.pass`; the top-level
`pass` is true only when both are true. This is only the Issue #274 startup gate.
It never changes the adoption decision: correctness, provider workload,
build/package, Debug/Release, MinGW, and hardware evidence remain separate hard
gates. A collect-only report therefore cannot be mistaken for adoption evidence
even if its smoke timings happen to satisfy the synthetic thresholds.

The console prints the report path. The path is a retrieval aid and is not part
of the JSON payload.

## Job-query and cleanup telemetry

The paired runner accepts the optional v1 `launchJobQueryObservation`,
`startupDiagnostics.*.jobQueryObservation`, and `cleanupObservation` fields
when a producer supplies them. These observations are deliberately
payload-free and numeric: bounded attempt/count/byte fields, booleans, and
the numeric Win32 error code are retained; PIDs, paths, handles, messages,
commands, captions, document data, and raw objects are never copied into the
paired report. The hard bounds include at most eight retained attempt records,
bounded counts, and bounded `capacityBytes`, `requiredBytes`, and
`returnLengthBytes`.

The producer's `JobObjectBasicProcessIdList` enumeration is bounded to at most
eight native query calls per enumeration, including the zero-buffer sizing call.
Only Win32 errors `122`, `24`, and `234` are retryable, and only when checked
arithmetic produces a strictly larger target no bigger than 1 MiB. The retained
`ERROR_MORE_DATA (234)` case grows from 16 to 40 bytes. `capacityBytes`,
`requiredBytes`, `returnLengthBytes`, `assignedProcessCount`, and
`listedProcessCount` remain separate bounded diagnostics; every attempt records
its resize decision and a terminal error, and none of this telemetry qualifies
or weakens cleanup.

The no-GUI producer self-test drives the same retry loop through an injected
query invoker: the 16-to-40 `234` correction and a successful partial-list
correction each use exactly three calls, while a retry-exhaustion script uses
exactly eight calls and preserves the final native error. The shared attempt
predicate rejects a ninth call; the same self-test also drives
architecture-sized growth while membership changes between partial responses
and rejects zero, negative, out-of-range, or duplicate PIDs. An identity-gap
requery remains a separate enumeration with its own budget. When the budget ends after a failed native
attempt, the top-level error and final attempt retain that actual retryable
error; `122` is used for successful-partial exhaustion only.

The producer reports success only when `listedProcessCount` equals
`assignedProcessCount`, the header/count/capacity shape is valid, and every PID
is positive, representable, and unique. A successful partial list is never
reported as success: the producer performs a checked growth and requires a
complete requery. Overflow, stagnation, malformed counts, duplicate PIDs, the
1 MiB cap, or exhaustion of the eight-call budget fail closed. An identity-gap
requery is a separate enumeration with its own eight-call budget. The paired
runner remains a compatibility consumer: if an older producer supplies a
`partial` observation, it retains that diagnostic and does not retry or adjust
it here. Zero survivors still do not prove cleanup when the job query failed.

The paired consumer also retains the shared cleanup aggregate fields
`processEnumerationAttempted`, `processEnumerationSucceeded`,
`processEnumerationComplete`, `processEnumerationErrorCode`,
`processEnumerationRetryCount`, `processEnumerationCallCount`,
`processEnumerationCompletedCount`, `processEnumerationFailureCount`,
`trackedSweepFailureType`, `trackedSweepFailureErrorCode`,
`trackedSweepIdentityAttemptCount`, `trackedSweepIdentityFailureCount`,
`trackedSweepDisappearedAfterSnapshotCount`,
`trackedSweepStillPresentAfterFailureCount`, and `trackedSweepPassCount`.
Affinity records likewise retain `historicalOwnedCount`, `currentLiveCount`,
`expiredHistoricalCount`, `failureType`, `failureErrorCode`, and
`liveSetSource`. All integer fields are bounded and payload-free; the consumer
accepts only the producer enum allowlists, preserves the first nonzero failure
code, requires the process-enumeration call/completed/failed count equations,
requires zero process-enumeration failures to imply `succeeded=true`,
`complete=true`, and `completedCount=callCount`; nonzero failures require
`succeeded=false` (with either complete state), and requires
`currentLiveCount <= historicalOwnedCount` with
`expiredHistoricalCount = historicalOwnedCount - currentLiveCount`.

These fields are additive to schema version 1. An older report with all of the
new fields absent is readable through explicit local `unknown`,
`not-observed`, and zero fallbacks. A report that contains only part of the new
field set, an out-of-range integer, an unknown enum, or an inconsistent
cross-field value is represented as local `unavailable` evidence. Neither the
fallback nor local rejection changes the paired report's existing acceptance,
suppression, performance, or `adoption.decision=HOLD` behavior.

Query and cleanup telemetry never participates in the success expression,
`Test-PairedRunCleanupVerified`, termination/suppression, acceptance,
qualification, or adoption gates. In particular, zero survivors do not prove
cleanup when the job query failed. A failed or preflight launch receives
neutral `not-attempted` telemetry. Reports from an older v1 producer with the
optional fields absent also receive neutral `not-attempted` telemetry, while a
present malformed query or cleanup subobject becomes only local
`unavailable` evidence and leaves valid process-tree, root-exit, window, and
legacy status/gate fields intact.

## Self-test

Run the helper self-test before a real campaign:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\measure-output-startup.ps1 -SelfTest
```

It exercises median/p95 statistics, deterministic interleaving and ordering,
the payload-free schema rejection, affinity metadata conversion and nonzero
mask validation, exact portable-sidecar and artifact-bundle identity/cleanup,
profile hashing/cleanup, parent-first PID identity helpers, all four
Debug/Release × C++/Rust selector-proof cells, archive/hash reconstruction,
top-level mirror checks, the four startup-diagnostic checkpoints, payload-free
trace allowlisting, ordered-event ordering/truncation and malformed-clock
rejection, trace-cleanup terminal states, and the paired performance gates. It
also mutates selector result, configuration, compile-log, archive,
symbol, and contract hash fields to verify rejection. It does not launch
`sakura.exe` or any other GUI process. Run it with both `powershell.exe` and
`pwsh` when validating the two supported PowerShell hosts.
The self-test also exercises the additive cleanup telemetry schema: old-report
fallbacks, bounded integer and enum rejection, first-cause retention, process
enumeration equations, and affinity historical/current/expired count checks. Its
affinity plan uses five historical records and four exact current-live records,
performs four read-backs only, rejects null/empty, unknown, duplicate, and
creation/image-path-mismatched current sets, and confirms that the one expired
historical record is not a failure target.
Do not use it as a substitute for the full 35-launch-per-backend campaign.
