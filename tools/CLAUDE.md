# Build Tools and Documentation Guidance

## Build Interface

The public entry scripts live at the repository root; run them from there.

| Script | Responsibility |
|---|---|
| `build-dev.bat` | Builds only `sakura_core/sakura.vcxproj` for the fast edit loop |
| `build-sln.bat` | Builds `sakura.sln`, including `sakura` and `tests1` |
| `build-all.bat` | For x64 Release distribution work, builds the solution, tests, help, installer, ZIP, and assembly artifacts |
| `build-gnu.bat` | Configures, builds, and tests the experimental MinGW path |

- Keep platform validation limited to the values each script documents and propagate the exact child exit code.
- Quote tool and repository paths, use `setlocal` for script-local state, and keep MSBuild node reuse disabled with `/nr:false` for command-line wrapper builds.
- `build-all.bat` keeps assembly listings enabled for package collection. A listing-enabled `build-sln` or `build-all` first finishes solution/tests with listings explicitly off, then rebuilds only the product with listings on and `/m:1`; ordinary calls without the opt-in leave listings off.

## Environment Variables

`build-envvars.md` is the source of truth for public environment variables.

- `NUM_VSVERSION` selects the Visual Studio major version. `ARG_VSVERSION` is an internal temporary variable in `find-tools.bat` and must not be documented as the caller interface.
- `SAKURA_GENERATE_ASSEMBLY_LISTINGS=1` or `true` enables MSVC `/FAsu`; changing it can cause one recompilation because the compiler command line changed.
- `SAKURA_DEV_BUILD_TARGET` is a diagnostic escape hatch. Leave it unset for normal builds, and clear it after using a non-`Build` target.
- `SKIP_CREATE_GITHASH` and `FORCE_POWERSHELL_ZIP` should remain opt-in diagnostic/reproducibility controls.

## Hot-Path Script Rules

- Do not add fixed sleeps to build, delete, retry, or dependency checks. Use bounded retry with backoff only for transient executable replacement, and for the one documented network exception below.
- **Network exception (Issue #138).** Package restore may repeat a *fetch-shaped* vcpkg failure, because that failure says nothing about this repository. `package_restore._run_vcpkg` retries at most `_RESTORE_DOWNLOAD_ATTEMPTS` times with a 15s/45s/90s backoff, and only when `_transient_download_reason` recognizes an HTTP 5xx, a transient curl transport code, or a WinHTTP receive error in vcpkg's own output. Every attempt and backoff is drawn from the caller's existing timeout budget, so the worst case is unchanged. A port that fails to compile, a 404, and a checksum mismatch must keep failing on the first attempt: repeating them multiplies a 30-minute job and hides a real defect. Retries print `PACKAGE_RESTORE_RETRY` to stderr because an unattended `EventWriter` has no stream and would drop the evidence.
- Workflow-side vcpkg bootstrap lives in `.github/actions/bootstrap-vcpkg`, which every `pwsh` bootstrap site calls instead of repeating the same block. It prefers a cached copy of the pinned tool and only fetches when it has none (#142); the retry below is the fallback for that miss, not the first line of defence. Bootstrap downloads a fixed tool and compiles nothing, so its retry is deliberately unconditional, unlike the classified package-restore retry above. Both share the same horizon for the same measured reason: on 2026-08-12 the degradation defeated vcpkg's own three attempts inside ~5s, `msys2/setup-msys2`'s three across ~30s, and this action's first 5s/20s revision (21:24:25, 21:24:30, 21:24:51), while the same runner fetched the whole closure successfully from 21:27:04 to 21:29:03. **Tune the horizon, not the attempt count** — three attempts that all land inside half a minute are indistinguishable from one.
- `build-on-msys2.yml` deliberately keeps **no** bootstrap retry. It needs the `.sh` entry point under its `msys2 {0}` default shell, so it cannot call the composite action, and the job is `continue-on-error: true`, so a transient bootstrap failure there turns nothing red and ends the job sooner rather than later. That is the whole reason now. **The finding debt that used to block any edit to this file is paid (#164):** the workflow's one legacy `test.filtered_or_skipped` finding was its hand-copied `GTEST_FILTER` list, which `inventory semantic --strict` demanded be removed the moment the file was touched at all — and it could not be removed, because the hosted job cannot run without it. The list now lives once in `src/test/headless-suite-selection.env` and the job reads it into `$GITHUB_ENV` from a `pwsh` step, so the finding is gone and the file is ordinarily editable. Read it into `$GITHUB_ENV` from `pwsh`, never from the job's default `msys2` shell, which would have to append to a Windows path. `src/test/py/test_headless_suite_selection.py` pins the arrangement, including that `build-sakura.yml`'s remaining `HEADLESS_GTEST_EXCLUDES` copy still equals the shared value; retire that copy in a change that can afford `build-sakura.yml`'s own finding reduction.
- `Remove-FileWithRetry.ps1` must reject broad/empty targets, retry only the exact literal file, and fail after its bounded attempt count.
- Avoid unconditional child-tool builds. Make dependency checks incremental at the owning MSBuild/CMake target instead of probing repeatedly in wrapper scripts.
- Do not leave reusable MSBuild nodes, compiler/linker processes, or helper processes after a normal wrapper invocation.

## CI Preflight

- Before starting CI for any change, run `py -3 tools/build/sakura_build.py --format json lint checkout-invariance` from the repository root and require exit code 0. This is mandatory, not an optional diagnostic.
- After editing `src/main/dependencies/dependencies.json`, run `py -3 tools/dependency_ledger.py generate` then `py -3 tools/dependency_ledger.py check`. Architecture-gates runs the same check.
- The lint simulates LF and CRLF inputs for the semantic-graph schema and semantic-inventory scanner, and verifies that committed generated projections remain equivalent. Do not substitute `generate --check` for this preflight; run both checks.

## Documentation Contract

Keep these files synchronized with script behavior:

- `build.md`: task-oriented developer workflow and test warning
- `build-batchfiles.md`: script responsibilities and call graph
- `build-envvars.md`: public variables and defaults
- `find-tools.md`: tool discovery and Visual Studio selection

When changing a command, argument, default, environment variable, artifact, or call branch, update the corresponding documentation in the same change.
