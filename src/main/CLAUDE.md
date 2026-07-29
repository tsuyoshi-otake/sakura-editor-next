# Main Build and Resource Guidance

## Scope

This subtree owns shared C++ support code, generated-build orchestration, MSBuild imports, and common resources:

- `cpp/`: shared implementation outside the legacy `sakura_core/` tree
- `cmake/`: CMake target definitions, dependency acquisition, host tools, and generated files
- `msbuild/`: MSBuild properties/targets that invoke the CMake tool graph
- `resources/`: shared resource IDs, templates, icons, and debugger visualizers

Keep the hand-written MSBuild path and the CMake/MinGW path behaviorally aligned.

## Generated Artifacts

- `sakura.cmake` is the central CMake definition for the core object library and shared generator setup.
- `HeaderMake.cmake` builds the host `HeaderMake` tool used to generate `Funccode_define.h` and `Funccode_enum.h` from `sakura_core/Funccode_x.hsrc`.
- `version.cmake` and `version.h.in` generate `version.h` from Git/CI inputs.
- `darkmodelib.cmake` produces the configuration-specific `darkmode.lib` consumed by the MSBuild projects.
- Generated artifacts belong under the configured build directory. Never write them back over their source templates.

## Incremental and Nested-Build Rules

- Every expensive generator or child build must declare the real source/configuration inputs and stable outputs that invalidate it. A no-op parent build must not invoke unchanged DarkModeLib, HeaderMake, GoogleTest, or PPA builds.
- Use `SAKURA_NESTED_BUILD_TOOL_ARGS` for Visual Studio-generated CMake child builds. It disables MSBuild node reuse and child FileTracker injection without changing non-MSBuild generators.
- Never pass `/nr:false` or `/p:TrackFileAccess=false` to Ninja, Makefiles, or other non-Visual-Studio generators.
- `CMakeBuildEnvironmentVariables` in `msbuild/cmake.props` is deliberately scoped to nested CMake/MSBuild commands. Do not disable FileTracker for the parent `sakura` or `tests1` projects.
- When an MSBuild wrapper target owns invalidation, give it complete `Inputs` and `Outputs`. Use `Touch` only after the nested command succeeds and only to close a deliberate timestamp contract.
- Serialize submodule materialization through `git_submodule_update_locked.cmake`; parallel targets must not race while initializing the same submodule.

## Compiler and Generator Compatibility

- `SAKURA_GENERATE_ASSEMBLY_LISTINGS` defaults to `OFF`. Keep `/Fa` directory paths terminated so parallel translation units receive distinct listing files.
- Preserve configuration-aware paths: Debug libraries can live below a `Debug` subdirectory while Release libraries live at the base install path.
- CMake currently discovers C++ files recursively, whereas `.vcxproj` files enumerate them explicitly. Adding a source still requires the MSBuild project/filter updates described in `sakura_core/CLAUDE.md`.

## Verification

- For shared CMake changes, verify MSVC Debug and Release plus MinGW when the altered logic is generator-independent.
- For nested-build changes, run one build that performs the child work and a second no-op build that proves the child is skipped.
- Confirm compiler, linker, and nested MSBuild processes exit after verification; simultaneous builds must not share the same platform/configuration intermediate directory.

