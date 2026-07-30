# CI Workflow Guidance

## Windows Build Contract

- `workflows/build-sakura.yml` builds the x64 `{Debug, Release}` matrix. Debug compilation runs under Build Wrapper for analysis, while the regular MSBuild step builds Release with `build-sln.bat`.
- The workflow runs the test suite before creating the installer. Release jobs package and upload one AVX-baseline `sakura.exe`; AVX2 and AVX-512F/BW implementations are selected inside that binary at runtime.
- Release MSBuild must set `SAKURA_GENERATE_ASSEMBLY_LISTINGS=1` because the workflow calls `build-sln.bat` directly but later publishes the ASM ZIP. Keep the setting scoped to the MSBuild step; Debug does not need listing I/O.
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
