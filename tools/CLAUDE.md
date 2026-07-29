# Build Tools and Documentation Guidance

## Build Interface

The public entry scripts live at the repository root; run them from there.

| Script | Responsibility |
|---|---|
| `build-dev.bat` | Builds only `sakura_core/sakura.vcxproj` for the fast edit loop |
| `build-sln.bat` | Builds `sakura.sln`, including `sakura` and `tests1` |
| `build-all.bat` | For Win32/x64, builds the solution, help, installer, and ZIP artifacts; for MinGW, delegates to `build-gnu.bat` and exits |
| `build-gnu.bat` | Configures, builds, and tests the experimental MinGW path |

- Keep platform validation limited to the values each script documents and propagate the exact child exit code.
- Quote tool and repository paths, use `setlocal` for script-local state, and keep MSBuild node reuse disabled with `/nr:false` for command-line wrapper builds.
- `build-all.bat` enables assembly listings inside its local environment. Distribution CI enables them on its Release MSBuild step; ordinary `build-dev` and `build-sln` calls leave them off.

## Environment Variables

`build-envvars.md` is the source of truth for public environment variables.

- `NUM_VSVERSION` selects the Visual Studio major version. `ARG_VSVERSION` is an internal temporary variable in `find-tools.bat` and must not be documented as the caller interface.
- `SAKURA_GENERATE_ASSEMBLY_LISTINGS=1` or `true` enables MSVC `/FAsu`; changing it can cause one recompilation because the compiler command line changed.
- `SAKURA_DEV_BUILD_TARGET` is a diagnostic escape hatch. Leave it unset for normal builds, and clear it after using a non-`Build` target.
- `SKIP_CREATE_GITHASH` and `FORCE_POWERSHELL_ZIP` should remain opt-in diagnostic/reproducibility controls.

## Hot-Path Script Rules

- Do not add fixed sleeps to build, delete, retry, or dependency checks. Use bounded retry with backoff only for transient executable replacement.
- `Remove-FileWithRetry.ps1` must reject broad/empty targets, retry only the exact literal file, and fail after its bounded attempt count.
- Avoid unconditional child-tool builds. Make dependency checks incremental at the owning MSBuild/CMake target instead of probing repeatedly in wrapper scripts.
- Do not leave reusable MSBuild nodes, compiler/linker processes, or helper processes after a normal wrapper invocation.

## Documentation Contract

Keep these files synchronized with script behavior:

- `build.md`: task-oriented developer workflow and test warning
- `build-batchfiles.md`: script responsibilities and call graph
- `build-envvars.md`: public variables and defaults
- `find-tools.md`: tool discovery and Visual Studio selection

When changing a command, argument, default, environment variable, artifact, or call branch, update the corresponding documentation in the same change.

