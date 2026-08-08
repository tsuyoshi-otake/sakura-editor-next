# CI Workflow Guidance

## Windows Build Contract

- `workflows/build-sakura.yml` builds the x64 `{Debug, Release}` matrix. Debug compilation runs under Build Wrapper for analysis, while the regular MSBuild step builds Release with `build-sln.bat`.
- The workflow runs the test suite before creating the installer. Release jobs package and upload one AVX-baseline `sakura.exe`; AVX2 and AVX-512F/BW implementations are selected inside that binary at runtime.
- Pull requests build and test both MSVC configurations, but skip CHM, BmpTools, the installer, ZIP creation, and production uploads. The Debug `sonar-input` artifact remains available for the separate Sonar workflow.
- The MSBuild job uses `if: ${{ !cancelled() }}` and starts with an explicit prerequisite validator. `build-vcpkg-msvc` is the strict dependency; `build-vcpkg-mingw` is experimental and advisory, matching the MinGW workflow contract.
- `SAKURA_GENERATE_ASSEMBLY_LISTINGS` is a job-level env on `build`, not scoped to the MSBuild step alone: `zipArtifacts.bat` and the Upload Asm step both need to see the same value the MSBuild step used. It resolves to `1` only for `matrix.config == 'Release'` on non-`pull_request` triggers (`push` to `master`, `workflow_dispatch`); Debug never needs listing I/O, and `pull_request` Release builds leave it `0` since they never publish the Asm artifact.
- This setting does more than emit `.asm` files: it also serializes LTCG codegen (`/CGTHREADS:1`). Issue #43 Phase 2b's verified two-pair comparison found that turning Listings off reduced the Release MSBuild step by 34.9-40.6% (pooled 37.8%), roughly 9-11 minutes. Link was the only reproducible driver; CL and Exec deltas changed sign between pairs. `zipArtifacts.bat` treats the Asm archive as optional (skips the `.asm` copy and zip instead of failing `copyRequired`'s missing-file check) when the value isn't `1`/`true`, and the workflow's Upload Asm step mirrors that with `env.SAKURA_GENERATE_ASSEMBLY_LISTINGS == '1'`.
- Do not replace CI's solution build with `build-dev.bat`, and do not apply the local headless test filter to the required full CI suite solely for speed.
- The Sonar job has its own rebuild/coverage flow; do not assume the main matrix's incremental settings apply to it.

## Other Workflows

- `workflows/build-on-msys2.yml` owns the experimental MinGW build and repeated test run.
- Encoding, Python, cppcheck, and Doxygen workflows are separate checks. Update their path filters only when the affected-file contract changes.
- The main build currently ignores Markdown-only, `.gitignore`, and `.editorconfig` changes for push and pull-request triggers. Commit-message CI skips do not apply to the merge commit itself.

## Workflow Changes

- Preserve the distinction between local speed optimizations and release/CI completeness.
- Keep action permissions and downloaded tooling narrowly scoped. Pin or update actions deliberately and retain any existing commit pin where the workflow uses one.
- Verify YAML structure and inspect every changed matrix branch; a condition that works for Release must not accidentally add work to Debug or Sonar jobs.
