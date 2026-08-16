# Main Build and Resource Guidance

## Scope

This subtree owns shared C++ support code, generated-build orchestration, MSBuild imports, and common resources:

- `cpp/`: shared implementation outside the legacy `sakura_core/` tree
- `cmake/`: CMake target definitions, dependency acquisition, host tools, and generated files
- `msbuild/`: MSBuild properties/targets that invoke the CMake tool graph
- `resources/`: shared resource IDs, templates, icons, and debugger visualizers

Keep the hand-written MSBuild path and the CMake/MinGW path behaviorally aligned.

## Generated Artifacts

- `sakura.cmake` is the central CMake definition for the core object library, package discovery, and shared generator setup.
- `py/header_make.py` generates `Funccode_define.h` and `Funccode_enum.h` from `sakura_core/Funccode_x.hsrc`.
- `version.cmake` and `version.h.in` generate `version.h` from Git/CI inputs.
- Third-party libraries are resolved through the checked-in vcpkg manifest and `find_package`; do not reintroduce parallel ad-hoc dependency builds.
- Generated artifacts belong under the configured build directory. Never write them back over their source templates.

## Staged Runtime Tools

`ctags.cmake` and `diffutils.cmake` stage a third-party executable next to `sakura.exe`. Which provider supplies those bytes is a release decision, so each script selects it **by name** through a cache variable seeded from an environment variable of the same name — `SAKURA_CTAGS_SOURCE` (`archive` by default, plus `system` and `submodule`) and `SAKURA_DIFFUTILS_SOURCE` (`auto` by default, plus `system`, `archive`, and `none`). An unrecognized value is a `FATAL_ERROR`, not a silent fallback.

- **The provider must not be whatever the build machine happens to have on `PATH`.** `ctags.exe` used to come from `find_program(ctags)`, while `build-installer.bat` and `zipArtifacts.bat` both ship `license/` and `docs/` extracted from `installer/externals/universal-ctags/ctags-${CTAGS_VERSION}-<arch>.zip`. Three providers offered three different versions — a Chocolatey `universal-ctags 2022.6.5` in CI, the `externals/ctags` gitlink at 6.2.0-dev, and the committed v6.1.0 archive — so the released installer and ZIP shipped a 2022.6.5 binary under v6.1.0's license set. Defaulting to the committed archive makes the binary, its license, and its documentation one decision instead of three; `src/test/py/test_ctags_provenance.py` fails if they drift apart again.
- **Staging a tool into the output directory is a packaging act.** `zipArtifacts.bat` copies `%platform%\%configuration%\*.dll` wholesale, so the diffutils archive provider — which stages `libintl3.dll` and `libiconv2.dll` beside `diff.exe` — would inject two GPL DLLs into the release ZIP with no license text, purely because the build machine lacked `diff` on `PATH`. No released artifact carries `diff.exe` (neither `sakura-common.iss` nor `zipArtifacts.bat` copies it) and no test runs it, hence the `none` provider, which CI pins. Do not widen a `*.dll` copy or add an output-directory byproduct without checking what the packaging scripts sweep up.
- **Keep the fallback explicit and reported.** No committed ctags archive exists for every `ARCH`, so `archive` degrades to `system` and then to the submodule build with a `message(STATUS)` naming the provider actually used, and errors out only when none is available. A default that quietly changes what ships is the defect this section exists to prevent.
- `GenerateCTags`/`GenerateDiff` in `sakura_core/sakura.vcxproj` are gated on `!Exists('$(SakuraSharedOutDir)ctags.exe')`/`diff.exe`, so a stale binary already sitting in a local output directory is never replaced. Delete it once after changing a provider, or the old bytes survive the change.
- **`bregonig.dll` and `migemo.dll` have one provider: the vcpkg-built imported target.** `sakura.cmake` stages `$<TARGET_FILE:...>` so Debug copies `debug/bin` and Release copies `bin`. `build-installer.bat` and `zipArtifacts.bat` must consume that staged DLL and the license files under `externals/bregonig`. They must not extract `installer/externals/bregonig/bron420.zip` into the output directory. `src/test/py/test_runtime_artifact_providers.py` fails if a second provider reappears. Packaging scripts record SHA-256 of the staged DLLs against the installer payload and the executable ZIP.

## Dependency ledger

`src/main/dependencies/dependencies.json` is the source of truth for third-party
kind, ownership, lifecycle, status, and scope. Do not add a flat `class` field.
`NOTICE` and `sbom.spdx.json` are generated; after editing the ledger run
`py -3 tools/dependency_ledger.py generate` and `py -3 tools/dependency_ledger.py check`.
Architecture-gates runs that check offline. Owned snapshots under
`third_party/owned/` are verified when a row declares them; they are not imported
by this guardrail.

## Incremental and Nested-Build Rules

- Every expensive generator or child build must declare the real source/configuration inputs and stable outputs that invalidate it. A no-op parent build must skip unchanged generated headers, package staging, PPA, and nested build work.
- `build/<platform>/CMakeTools` is a shared generator workspace, not a leaf-project intermediate directory. Ordinary project and language-DLL Clean/Rebuild operations must preserve it; only an explicit full-generator observation or repository-wide clean owner may request its removal.
- A CMake `add_custom_target` emitted for Visual Studio can be requested on every parent build because its generated project contains a phony output. Keep such observers lightweight and content-aware: hash or compare the real inputs and outputs, skip expensive extraction/build/copy work when unchanged, and publish a state file only after every output succeeds. Never use an unconditional `touch` as the no-op contract.
- Resolve runtime-asset symlinks to their real provider before comparing content. `cmake -E copy_if_different` alone is not a stable timestamp contract for every Windows symlink provider.
- For a Git submodule-backed tool, the parent repository gitlink is the authoritative source version. Build and archive that exact commit under a lock; do not let dirty or untracked submodule worktree state enter the generated artifact implicitly.
- If a CMake-generated Visual Studio child build is added, keep its MSBuild-only arguments behind the Visual Studio generator check. Disable node reuse and child FileTracker injection without changing non-MSBuild generators.
- Never pass `/nr:false` or `/p:TrackFileAccess=false` to Ninja, Makefiles, or other non-Visual-Studio generators.
- `CMakeBuildEnvironmentVariables` in `msbuild/cmake.props` is deliberately scoped to nested CMake/MSBuild commands. Do not disable FileTracker for the parent `sakura` or `tests1` projects.
- When an MSBuild wrapper target owns invalidation, give it complete `Inputs` and `Outputs`. Use `Touch` only after the nested command succeeds and only to close a deliberate timestamp contract.
- Do not make package or submodule materialization an unbounded parallel startup path; concurrent targets must not race while initializing the same dependency.

## Compiler and Generator Compatibility

- `SAKURA_GENERATE_ASSEMBLY_LISTINGS` defaults to `OFF`. Keep `/Fa` directory paths terminated so listings use source-derived names. Its opt-in Release product MSBuild branch deletes `cl.exe`'s provisional listings immediately before Link, then disables `/MP` and uses `/CGTHREADS:1` while LTCG writes the final listings. Because tests1 relinks the product `/GL` archive and replays its `/Fa` destination during LTCG, the canonical listing build completes the solution/tests with listings explicitly off before a product-only `/m:1` listing pass.
- Preserve configuration-aware paths: Debug libraries can live below a `Debug` subdirectory while Release libraries live at the base install path.
- CMake currently discovers C++ files recursively, whereas `.vcxproj` files enumerate them explicitly. Adding a source still requires the MSBuild project/filter updates described in `sakura_core/CLAUDE.md`.

## Verification

- For shared CMake changes, verify MSVC Debug and Release plus MinGW when the altered logic is generator-independent.
- For nested-build changes, run one build that performs the child work and a second no-op build that proves the child is skipped.
- Confirm compiler, linker, and nested MSBuild processes exit after verification; simultaneous builds must not share the same platform/configuration intermediate directory.
