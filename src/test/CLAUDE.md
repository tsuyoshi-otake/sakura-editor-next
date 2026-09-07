# Test Infrastructure Guidance

## Scope

- `cpp/tests1/` contains GoogleTest sources and test fixtures.
- `resources/` contains the test resource script, generated-code checks, and embedded test data.
- `cmake/` defines `tests1`, Miniz, test resource ZIPs, and related dependencies; GoogleTest is resolved from the vcpkg toolchain. `generate_miniz` is a `tests1` dependency only — it must not be an `ALL` target.
- `sakura_core/tests1.vcxproj` imports the shared MSBuild/CMake orchestration used to stage generated test inputs.

`build-dev.bat` deliberately does not evaluate or build this subtree. Use `build-sln.bat <Platform> <Configuration>` before running MSVC tests.

## Adding Tests

- Add the source to `sakura_core/tests1.vcxproj` and `sakura_core/tests1.vcxproj.filters`; keep the CMake test source discovery path working as well.
- Reuse the existing `ShareDataTestSuite`, `EditorTestSuite`, and `UiaTestSuite` fixtures when a test needs those lifecycles. Do not create a competing global editor/control-process setup.
- Keep live network or machine-specific integration checks disabled by default and clearly named. Routine verification must be deterministic.
- When a test needs a generated library, DLL, ZIP, or header, update both its CMake dependency and the MSBuild staging/invalidation contract.

### Shared single-owner fixtures

Some third-party headers may be included by exactly one translation unit in
`tests1`. Use the existing fixture instead of including such a header again;
add an API to the fixture when it does not cover your case.

- **ZIP and zlib: use `ZipArchiveFixture.h`.** `externals/miniz-cpp/zip_file.hpp`
  emits the miniz C API bodies (`mz_*`) and non-inline
  `miniz_cpp::detail::*` helpers. `ZipArchiveFixture.cpp` is therefore the sole
  includer and implementation owner in `tests1`; other tests call its narrow
  archive-reading API. `externals/` is upstream code and must not be edited to
  work around this.

### COM-dependent tests

A GoogleTest body runs on a thread with no COM apartment. Production UI code
runs on an OLE-initialized thread, so a component that decodes or renders
through COM works there and returns nothing under test. When the code under
test fails closed (as `DecodeExtensionIconBitmap` does, being `noexcept`), the
missing apartment is indistinguishable from bad input and reads as a defect in
the code under test. Initialize the apartment in a fixture for the cases that
expect success, and leave the rejection cases — which return before reaching
COM — as plain `TEST`s.

**Never rely on an apartment another suite happened to leave behind.** A test
that needs COM must own its own apartment, in its own scope, with a matching
uninitialize. The ambient apartment is not a shared fixture: it is an artifact
of link order, because gtest runs suites in the order their translation units
were linked, and `tests1.vcxproj` is an explicit source list. Adding one
unrelated `.cpp` to that list reorders the suites and can move the leak away
from the test that was silently consuming it.

This is not hypothetical. Verified 2026-08-07: `CSakuraEnvironmentTest.ResolvePath001`
had depended on an ambient COM apartment created by an earlier test. Adding five test
files for #35–#38 changed the link order, and the test began failing with
`0x800401F0 CoInitialize has not been called` on `CLSID_ShellLink`. CI is the
harsher environment here: its headless `GTEST_FILTER` excludes the GUI suites
(`EditWndTest`, `WinMainTest`, `TrayWndTest`, and the dialog/macro suites) that
carry `cxx::COleInit` via `UiaTestSuite`, so fewer apartments exist to borrow.
Reproduce this class of failure by running the single test alone
(`tests1.exe --gtest_filter=<Suite>.<Test>`); an isolated run has no ambient
state to hide the dependency.

Existing patterns to reuse rather than reinvent: `cxx::COleInit` in
`cpp/tests1/window/UiaTestSuite.hpp` (RAII `OleInitialize`/`OleUninitialize`),
and the scoped `CoInitializeEx`/conditional-`CoUninitialize` guard in
`test-csakuraenvironment.cpp`, which also tolerates `RPC_E_CHANGED_MODE` by
riding an already-initialized apartment without releasing one it does not own.

## Phase-Scoped Tests

| Priority | Additional guidance |
|---|---|
| P0 platform contracts | [`cpp/tests1/platform/CLAUDE.md`](cpp/tests1/platform/CLAUDE.md) |
| P1/P2/P4 workbench models and layout | [`cpp/tests1/workbench/CLAUDE.md`](cpp/tests1/workbench/CLAUDE.md) |
| Cross-process and real backends | [`integration/CLAUDE.md`](integration/CLAUDE.md) |

When a new scoped test directory is introduced, add its own `CLAUDE.md` with
only the subsystem-specific invariants; keep build registration and runner
cleanup rules centralized here.

## Running Tests

`src/test/test-inventory.json` is the source-controlled pre-split guarantee
baseline.  Refresh it only from a successfully rebuilt Debug `tests1.exe` via
the canonical `test inventory collect` command.  Preserve each `test_id` when
moving a test to a new executable; change only its runtime runner/selector
mapping.  A discovery failure or an unexpected zero-test result must never
replace the baseline.

For an existing baseline, use `test inventory refresh-runtime` with every
declared `--runner runner-id=executable` after Debug discovery. Confirm renames
against source/history and pass each as an explicit
`--remap stable-test-id=runner-id::selector`; do not delete the old stable ID and
create a replacement. This also adds newly discovered tests and refreshes binary
provenance. Then run `test inventory verify-runtime` with the same runner set:
both `missing_selectors` and `unexpected_selectors` must be empty. A test-only
rename or addition still requires this inventory update (#294).

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

### SENP v2 process acceptance

After rebuilding Debug, run `py -3 tools/verify-senp-runtime.py --offline` for
the real SENP v2 process boundary. The runner builds an explicit test-only
native peer and WIT component under the existing Cargo lock, sets
`SAKURA_SENP_RUNTIME_FIXTURES`, runs the native lifecycle/process/codec suites,
and verifies both directions of the C++/Rust codec exchange. It rejects skipped
or missing process cases and records child exits and surviving processes.
Without that fixture directory, ordinary `SenpRuntimeProcess` execution skips
explicitly; this skip is never acceptance evidence. The fixtures belong in a
temporary work directory and must not be staged into product output or installed
as extensions. Details and limits are in `docs/senp-v2-runtime.md`.

## Build-Dependency Invariants

- GoogleTest package resolution and staging must remain conditional on their declared inputs/outputs; do not add a second source-build path that runs on every `tests1` compile.
- PPA stub, Miniz, test ZIP, and plugin assets must be produced before the consuming compile/link/resource step, but skipped on an unchanged no-op build.
- Keep nested CMake/MSBuild node reuse and FileTracker workarounds scoped to the nested child. Parent test compilation still relies on normal MSBuild tracking.
