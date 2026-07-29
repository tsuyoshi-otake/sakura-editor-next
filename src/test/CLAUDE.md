# Test Infrastructure Guidance

## Scope

- `cpp/tests1/` contains GoogleTest sources and test fixtures.
- `resources/` contains the test resource script, generated-code checks, and embedded test data.
- `cmake/` defines `tests1`, Miniz, test resource ZIPs, and related dependencies; GoogleTest is resolved from the vcpkg toolchain.
- `sakura_core/tests1.vcxproj` imports the shared MSBuild/CMake orchestration used to stage generated test inputs.

`build-dev.bat` deliberately does not evaluate or build this subtree. Use `build-sln.bat <Platform> <Configuration>` before running MSVC tests.

## Adding Tests

- Add the source to `sakura_core/tests1.vcxproj` and `sakura_core/tests1.vcxproj.filters`; keep the CMake test source discovery path working as well.
- Reuse the existing `ShareDataTestSuite`, `EditorTestSuite`, and `UiaTestSuite` fixtures when a test needs those lifecycles. Do not create a competing global editor/control-process setup.
- Keep live network or machine-specific integration checks disabled by default and clearly named. Routine verification must be deterministic.
- When a test needs a generated library, DLL, ZIP, or header, update both its CMake dependency and the MSBuild staging/invalidation contract.

## Running Tests

Full suite for a built configuration:

```cmd
x64\Debug\tests1.exe
```

The full binary includes UI and integration suites. `WinMain/WinMainTest.*` launches a real editor using an isolated/default test profile, so its visible UI can look different from the user's normal profile. `EditWndTest`, dialog, profile, tray, and macro suites can also require UI or external integration.

For unattended local smoke verification, use the currently verified exclusion set:

```cmd
x64\Debug\tests1.exe --gtest_filter=-MacroMgrTest.*:CPpaTest.*:SelectFileTest.*:FileDialog/FileDialogTest.*:CDlgProfileMgrTest.*:TrayWndTest.*:EditWndTest.*:WinMainFuncTest.*:WinMain/WinMainTest.*
```

- This filter is not a substitute for the full suite. Run the affected UI/integration tests separately when the change touches them.
- Use `--gtest_list_tests` before changing automation filters; suite names can change.
- After automated execution, verify that `tests1.exe`, test-launched `sakura.exe`, and their parent runners have exited. Terminate parent processes first if a failed test can respawn a child.

## Build-Dependency Invariants

- GoogleTest package resolution and staging must remain conditional on their declared inputs/outputs; do not add a second source-build path that runs on every `tests1` compile.
- PPA stub, Miniz, test ZIP, and plugin assets must be produced before the consuming compile/link/resource step, but skipped on an unchanged no-op build.
- Keep nested CMake/MSBuild node reuse and FileTracker workarounds scoped to the nested child. Parent test compilation still relies on normal MSBuild tracking.
