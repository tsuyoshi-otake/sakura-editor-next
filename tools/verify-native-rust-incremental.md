# Native Rust incremental-build verifier

[`verify-native-rust-incremental.ps1`](verify-native-rust-incremental.ps1) is
an opt-in MSVC evidence runner for the native Rust archive and its C++ product
consumer. It is deliberately payload-free: the output JSON contains artifact
metadata, action classes, and typed decisions, but never command lines, build
logs, source text, or mutation payloads.

Run it from the repository root in a Visual Studio developer PowerShell:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-native-rust-incremental.ps1 `
  -Platform x64 -Configuration Release -NoOpIterations 3 -TimeoutSeconds 900
```

The verifier requires at least three no-op iterations. It creates a detached
Git worktree under the deliberately short `build/tmp/nri/r-*` path, checks that the
worktree has the same `HEAD` as the invoking checkout, initializes submodules
only in that worktree, performs one bounded package-closure restore there, and
runs the product `sakura_core/sakura.vcxproj` directly through MSBuild with a
per-phase diagnostic file logger. The shared source checkout is not used as
the build working tree. On success or failure, cleanup unregisters and removes
only the exact owned worktree; `-KeepWorkspace` is available for post-failure
inspection. Evidence records only `cleanup.kept=true`; the raw workspace path
is deliberately omitted and remains discoverable through `git worktree list`.

Cleanup never runs `git submodule deinit` from the linked worktree because
superproject submodule configuration is shared with the invoking checkout. If
`git worktree remove` has already unregistered the owned worktree but cannot
delete a long path, cleanup removes only that validated, reparse-safe owned
directory. Empty or malformed porcelain output, or output that omits the
invoking checkout, is an error rather than proof that the owned registration
disappeared. If the registration remains, cleanup fails closed as `survivor`
and leaves the worktree available for explicit recovery instead of mutating a
shared submodule checkout.

The pinned `tools/vcpkg` submodule does not track its host-local `vcpkg.exe`.
Before package restore, the verifier reads `VCPKG_TOOL_RELEASE_TAG` from the
isolated submodule, runs the already-bootstrapped binary in the shared checkout
only for `version --disable-metrics`, and requires that exact release tag. It
then copies only the validated binary byte-for-byte into the owned worktree,
records matching size and SHA-256 metadata, and creates the isolated
`vcpkg.disable-metrics` marker. It never runs a network bootstrap. The child
`VCPKG_ROOT` is pinned to that isolated directory even if the caller supplied
an ambient value, and the original environment is restored during finalization.
The shared checkout therefore supplies a validated host tool but remains
read-only and is never the build working tree.

The phases are:

1. `baseline`: build `sakura_core/sakura.vcxproj` and require the Rust archive,
   Rust MSBuild stamp, provider object, and product executable. Its link must
   be attributed to the explicit `sakura_core/sakura.vcxproj` contract. A fresh
   baseline may configure/build CMake helper targets and run the declared SENP
   packaging tool. `vcpkg z-applocal` is recorded separately as the product
   link's runtime-dependency copy step.
2. `no_op_1` through `no_op_N`: rebuild without changing a source. These
   phases must observe no `cargo`, `rustc`, `cl`, `link`, `lib`, resource
   compiler (`rc`), manifest embedding (`mt`), CMake, SENP packaging,
   `vcpkg z-applocal`, or actual `delete` actions, no
   `cargo-preflight` action, no unknown executable, and
   the tracked artifact metadata must remain unchanged. The normal backend and
   package validation path is Cargo-free after the Cargo preflight refactor;
   this assertion is the explicit evidence that no-op builds benefit from that
   contract. If a toolchain check is desired, invoke the separate MSBuild
   `PreflightSakuraNativeFfiCargo` target; the verifier does not fold it into a
   no-op phase.
3. `rust_source`: append one trailing LF to the isolated
   `rust/native/sakura_native_ffi/src/lib.rs`. The phase must observe a Cargo
   build (and any Rust compiler work), no C++/archive/resource/delete/preflight
   or unknown tool, a Rust output change, and exactly the explicit
   `sakura_core/sakura.vcxproj` link consumer.
   The product relink may run `mt.exe` to embed its generated manifest and
   `vcpkg.exe z-applocal` to copy declared runtime dependencies. These are
   recorded as typed link companions rather than accepted as unknown
   executables. CMake and SENP generation remain forbidden in this phase.
4. `cpp_provider`: append one trailing LF to the isolated
   `sakura_core/workbench/output/OutputServiceRustProvider.cpp`. The phase must
   compile that provider translation unit exactly once, must not compile any
   other C++ translation unit, must not run Cargo/Rust/archive/resource/delete/
   preflight or an unknown tool, and must link exactly the same explicitly declared
   product consumer.

The consumer contract is intentionally fixed in the script:

```text
rust_source  -> sakura_core/sakura.vcxproj
cpp_provider -> sakura_core/sakura.vcxproj
```

It is not inferred by scanning project files at runtime. If MSBuild emits an
unattributed or additional link, the phase receives the typed result
`unexpected_consumer`.

The default evidence path is
`build/evidence/native-rust-incremental.json`; use `-Output` to select another
path below the repository's `build` directory. Each artifact record contains
`exists`, `sizeBytes`, `sha256`, and `lastWriteTimeUtc` for:

* `rust_archive` (`sakura_native_ffi.lib`)
* `rust_stamp` (`sakura_native_ffi.msbuild.stamp`)
* `provider_obj` (`OutputServiceRustProvider.obj`)
* `sakura_exe`

The JSON schema has `schemaVersion: 1`, `payloadFree: true`, a fixed phase
order, per-phase exact aggregate action counts/classes, and bounded retained
action records (at most 256 records per phase) with an explicit truncation
flag. A truncated record set is never used as if complete: no-op, closure,
and mutation matrix checks fail closed when the retained records cannot prove
the required scope. Unknown direct executables remain `unexpected_tool`; the
phase also records only their sanitized basename counts in
`unexpectedToolNames` (at most 32 distinct names) plus an explicit truncation
flag. Paths, arguments, and command lines are not retained. This bounded
identity is diagnostic evidence only and never authorizes a tool. The schema
also carries bounded diagnostic log metadata
(`byteCount`, `lineCount`, `sha256`, and capped compiler/MSBuild error-code
counts), typed closure results, package-restore result, shared-checkout
fingerprints, and cleanup state. A failed MSBuild phase retains its underlying
typed process result—even when that result is `survivor`—alongside the
payload-free diagnostics and action summary; parser status is a separate
boolean. Empty diagnostic lines are valid parser input and produce no action;
they do not discard the remaining classification evidence. Compiler and linker
actions require a direct executable command line; MSBuild task-loading prose and
assembly metadata are not work, and filename-extension matching cannot treat a
prefix such as `.Common.dll` as a C source. An executable artifact path followed
only by an MSBuild `TaskId` is output metadata rather than a child-process
command. A localized tracker status line is also metadata only when the same
absolute executable path and TaskId were listed as an output within the prior
four log lines; another TaskId or a direct invocation remains fail-closed.
Copy-task prose that starts with a source `.exe` and names a second destination
`.exe` is artifact metadata, not an executable invocation. Known
build companions are also verb-scoped: only CMake configure/build, SENP
`componentize`/`pack-builtin`, and `vcpkg z-applocal` receive typed action
kinds; another verb remains `unexpected_tool`. `rc.exe` and `mt.exe` are
explicit work-action kinds, so a manifest relink is distinguishable from an
unclassified executable while no-op phases still reject either tool. Baseline
also fails closed on any remaining
`unexpected_tool` or Cargo preflight action. Process identities used internally for exact cleanup are never
serialized: evidence contains only typed results, counts, and sanitized survivor
executable-name counts, never PIDs, creation dates, command lines, or paths.
Captured stdout/stderr are opened with read-sharing only while hashing and
parsing, so a concurrent writer or deletion attempt fails closed. The child environment pins
`SAKURA_OUTPUT_BACKEND=cpp`, `SAKURA_UTF16_BACKEND=cpp`, both production
package switches to `false`, `SKIP_CREATE_GITHASH=1`, disabled MSBuild node
reuse, one build job, and neutral Cargo/MSBuild locale settings.

Process arguments are passed as raw values on hosts that expose
`ProcessStartInfo.ArgumentList`. The Windows command-line quoting algorithm is
used only by the Windows PowerShell 5.1 fallback, including for project and
logger paths containing spaces. Cleanup inspects the owned tree with bounded,
non-recursive enumeration: directory reparse points are rejected before
deletion, while non-directory reparse leaves are not traversed.

Package-tool and package-restore failures follow the same rule. Their stdout
and stderr are streamed and reduced to byte/line counts, SHA-256 values, capped
sanitized `BuildError` symbols such as `TOOL_VCPKG_NOT_FOUND`, and capped
compiler/MSBuild error-code counts. Raw output is never serialized. Parser and
truncation flags are explicit, and the original typed process result and exit
code remain authoritative when diagnostic parsing also fails.
For a successful package-tool record, the verifier cross-checks the expected
release tag, the matched/proof flags, the nested probe type and exit code, the
stdout proof metadata, and the failure-record count equation before accepting
the result.

The main typed failure values are:

* `timeout`: a bounded MSBuild invocation exceeded `-TimeoutSeconds`.
* `missing_output`: a diagnostic log or required artifact was not produced.
* `unexpected_consumer`: actual link consumers differ from the fixed contract.
* `survivor`: a child process remained alive after the bounded invocation; an
  observed unexpected Job Object member is still a failure even if job cleanup
  removes it before the post-cleanup query.

Other closed failure values (`build_failed`, `unexpected_action`, and
`artifact_changed`) identify ordinary build/closure mismatches. The script
does not retain raw output in the evidence file.

Process ownership is kernel-bound and fail-closed. The verifier creates the
exact target with `CREATE_SUSPENDED` and atomically supplies a private Windows
Job Object through `PROC_THREAD_ATTRIBUTE_JOB_LIST`; the Job is configured with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. A second creation attribute,
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, limits inheritance to the target's exact
stdin, stdout, and stderr handles. The primary thread is resumed only after
parent-side setup completes. The target therefore cannot execute an instruction
or create a descendant outside the job. Timeout, post-exit helper
inspection, and cleanup query or terminate that job; they never stop a process
by a numeric PID obtained from a user-mode ancestry census. PID reuse, a stale
`ParentProcessId`, or an unrelated WSL/container process cannot enter the kill
set. Closing the verifier's job handle is also the final crash-safe lifetime
fence.

The launcher checks Job, process, thread, and parent pipe handle closure as
well as redirected-output drain. If verified close or drain fails, an otherwise
successful invocation is replaced with typed `process_error`; cleanup failure
is never hidden by a successful build result.

Every executable is resolved to an existing absolute application path before
`CreateProcessW` is called. This keeps a bare tool name such as `git.exe` from
being mistaken for an `lpApplicationName` that Windows cannot start, while the
quoted command line still begins with that same resolved executable. If PATH
contains multiple applications with the same name, the normal first-match
command resolution is used explicitly; candidate paths are never concatenated.

MSVC can intentionally leave its exact descendant `mspdbsrv.exe` alive after
MSBuild exits because the project uses `/FS`. It is accepted only while it is
still an active member of the invocation's private Job Object. The verifier
records that helper by sanitized executable name, terminates the job-owned
process set, and requires a zero post-cleanup count. No other
post-exit helper is accepted;
an unlisted descendant is still a `survivor` even when cleanup succeeds. The
short worktree root also keeps legacy tool paths comfortably below the Windows
path-length boundary without moving verifier state outside `build/tmp`.

A shared-checkout fingerprint mismatch is retained as `artifact_changed` at
the `shared_checkout_audit` phase. It is a normal typed failure record rather
than an evidence-schema exception, so completed phase, cleanup, and diagnostic
metadata remain available when isolation itself fails.

Final evidence validation also fails closed without discarding earlier phase
results. A validation failure is reduced to a payload-free envelope with
`failure.type=process_error` plus `schemaValidation.code` and
`schemaValidation.stage`. Stable codes distinguish core shape, process
metadata, package-tool cross-fields, raw process identity/payload, and shared
checkout validation. The envelope retains sanitized completed phase evidence,
but removes raw command, process identity, and stdout/stderr string properties;
exception messages, stacks, paths, and commands are never serialized. If the
normal sanitizer or its final validation throws, a fixed emergency envelope
still retains each completed phase name and typed result together with bounded
exit, action, diagnostic-code, and survivor counts. It discards variable text,
paths, commands, raw process identities, and process names.

Worktree setup is evidence-bearing as well. Before an owned worktree is removed
after failure, the verifier copies the `submodule_update` typed result and its
bounded stdout/stderr count, hash, and failure-code metadata into
`workspaceSetup`. A setup failure therefore remains diagnosable even though its
temporary logs and worktree are correctly deleted.

The shared fingerprint covers more than the superproject status. It hashes the
fixed `.gitmodules` set, each gitlink and initialization state, each submodule
HEAD, tracked/untracked status, binary diff, and local Git configuration. This
detects both dirty-submodule content changes and clean submodules that were
silently deinitialized, while keeping file contents and configuration values
out of the evidence payload.

Before relying on a host toolchain, run the deterministic self-test:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-native-rust-incremental.ps1 -SelfTest
```

The self-test covers action classification, explicit-consumer closure,
diagnostic log metadata and combined survivor/build-failure evidence,
bounded package failure parsing, pinned vcpkg version/file evidence, ambient
`VCPKG_ROOT` rejection, immutable output-sharing, artifact/missing-output
records, atomic suspended Job ownership, timeout termination of a root plus
child with a zero post-cleanup job count, preservation and exact cleanup of an
external sentinel process, kernel job membership,
reparse-safe cleanup, package-tool
cross-field mutation rejection, process-identity payload rejection, and
cross-host argument quoting with a spaced argv value, and schema/payload checks
without creating a worktree or running MSBuild.
