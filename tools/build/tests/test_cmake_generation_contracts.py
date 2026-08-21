from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOLS_BUILD.parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

GIT_EXECUTABLE = shutil.which("git")
CMAKE_EXECUTABLE = shutil.which("cmake")
if CMAKE_EXECUTABLE is None:
    bundled_cmake = Path(
        "C:/Program Files/Microsoft Visual Studio/2022/Community/"
        "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    )
    if bundled_cmake.is_file():
        CMAKE_EXECUTABLE = str(bundled_cmake)
MSBUILD_EXECUTABLE = shutil.which("MSBuild.exe")
if MSBUILD_EXECUTABLE is None:
    bundled_msbuild = Path(
        "C:/Program Files/Microsoft Visual Studio/2022/Community/"
        "MSBuild/Current/Bin/MSBuild.exe"
    )
    if bundled_msbuild.is_file():
        MSBUILD_EXECUTABLE = str(bundled_msbuild)
SEVEN_ZIP_EXECUTABLE = shutil.which("7z")
if SEVEN_ZIP_EXECUTABLE is None:
    installed_7zip = Path("C:/Program Files/7-Zip/7z.exe")
    if installed_7zip.is_file():
        SEVEN_ZIP_EXECUTABLE = str(installed_7zip)

from sakura_build_lib.repository_inventory import (  # noqa: E402
    _cmake_command_blocks,
    _cmake_section,
)


def _generation_records(path: Path) -> list[dict[str, object]]:
    text = path.read_text(encoding="utf-8-sig")
    records: list[dict[str, object]] = []
    for ordinal, block in enumerate(_cmake_command_blocks(text, "add_custom_command"), 1):
        records.append(
            {
                "ordinal": ordinal,
                "kind": "custom-command",
                "outputs": _cmake_section(block, "OUTPUT"),
                "byproducts": _cmake_section(block, "BYPRODUCTS"),
                "inputs": _cmake_section(block, "DEPENDS"),
            }
        )
    for ordinal, block in enumerate(_cmake_command_blocks(text, "add_custom_target"), 1):
        tokens = shlex.split(block, posix=True)
        records.append(
            {
                "ordinal": ordinal,
                "kind": "custom-target",
                "target": tokens[0] if tokens else "",
                "outputs": [],
                "byproducts": _cmake_section(block, "BYPRODUCTS"),
                "inputs": _cmake_section(block, "DEPENDS"),
            }
        )
    return records


def _record_for_output(records: list[dict[str, object]], output: str) -> dict[str, object]:
    matches = [
        record
        for record in records
        if output in record["outputs"] or output in record["byproducts"]
    ]
    if len(matches) != 1:
        raise AssertionError(f"expected one command for {output}, found {len(matches)}")
    return matches[0]


class CMakeGenerationContractTests(unittest.TestCase):
    def test_single_config_msvc_architecture_and_rust_byproducts_are_explicit(self) -> None:
        sakura_path = REPO_ROOT / "src/main/cmake/sakura.cmake"
        sakura_cmake = sakura_path.read_text(encoding="utf-8-sig")

        # Ninja with the MSVC compiler may leave CMAKE_SYSTEM_PROCESSOR empty.
        # The compiler architecture is then the authoritative x64/x86/arm64
        # input used by the regular single-config CMake path.
        self.assertIn("CMAKE_CXX_COMPILER_ARCHITECTURE_ID", sakura_cmake)
        self.assertIn("MSVC_CXX_ARCHITECTURE_ID", sakura_cmake)
        self.assertIn(
            'string(TOLOWER "${_sakura_build_target_processor}"',
            sakura_cmake,
        )

        records = _generation_records(sakura_path)
        rust_targets = [
            record
            for record in records
            if record.get("kind") == "custom-target"
            and record.get("target") == "sakura_rust_core_build"
        ]
        self.assertEqual(1, len(rust_targets))
        self.assertTrue(
            {
                "${SAKURA_RUST_CORE_DEBUG_LIBRARY}",
                "${SAKURA_RUST_CORE_RELEASE_LIBRARY}",
            }.issubset(set(rust_targets[0]["byproducts"]))
        )

    def test_rust_utf16_backend_contract_is_explicit(self) -> None:
        backend_cmake = (REPO_ROOT / "src/main/cmake/sakura-utf16-backend.cmake").read_text(
            encoding="utf-8-sig"
        )
        sakura_cmake = (REPO_ROOT / "src/main/cmake/sakura.cmake").read_text(encoding="utf-8-sig")
        rust_helper = (REPO_ROOT / "src/main/cmake/build-rust-sakura-core.cmake").read_text(
            encoding="utf-8-sig"
        )
        msbuild_target = (REPO_ROOT / "src/main/msbuild/sakura-rust-core.targets").read_text(
            encoding="utf-8-sig"
        )

        self.assertIn("PROPERTY STRINGS cpp rust", backend_cmake)
        self.assertNotIn("PROPERTY STRINGS cpp rust both", backend_cmake)
        self.assertIn("SAKURA_UTF16_BACKEND must be exactly cpp or rust", backend_cmake)
        self.assertIn("SAKURA_UTF16_PRODUCTION_PACKAGE", backend_cmake)
        self.assertIn('set(_sakura_utf16_backend_value "rust")', backend_cmake)
        self.assertIn("canonical MinGW runner passes", backend_cmake)
        self.assertIn("The C++ UTF-16 backend cannot package production", backend_cmake)
        self.assertIn("string(TOUPPER", backend_cmake)
        self.assertIn('include("${CMAKE_SOURCE_DIR}/src/main/cmake/sakura-utf16-backend.cmake")', (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertLess(
            (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8").index(
                'include("${CMAKE_SOURCE_DIR}/src/main/cmake/sakura-utf16-backend.cmake")'
            ),
            (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8").index(
                "if(NOT SAKURA_SKIP_MODULES_CHECK)"
            ),
        )
        self.assertIn("SAKURA_RUST_CORE_MEMBER_MANIFEST", sakura_cmake)
        self.assertIn("${SAKURA_RUST_CORE_MEMBER_MANIFEST}", sakura_cmake)
        self.assertNotIn("utf16_scan", sakura_cmake.lower())
        self.assertIn('WORKING_DIRECTORY "${SAKURA_RUST_CORE_WORKING_DIR}"', rust_helper)
        self.assertIn("--locked", rust_helper)
        self.assertIn("Cargo.toml", msbuild_target)
        self.assertIn("SakuraRustCoreCargo", msbuild_target)
        self.assertIn("WorkingDirectory=\"$(SakuraRustCoreWorkspace)\"", msbuild_target)
        self.assertIn("ToUpperInvariant()", msbuild_target)
        self.assertIn(
            "<SAKURA_UTF16_BACKEND Condition=\"'$(SAKURA_UTF16_BACKEND)'==''\">rust</SAKURA_UTF16_BACKEND>",
            msbuild_target,
        )
        self.assertIn(">rust</SAKURA_UTF16_BACKEND>", msbuild_target)
        self.assertIn("--version", msbuild_target)
        self.assertIn('DependsOnTargets="ValidateSakuraRustCoreBackend"', msbuild_target)
        self.assertIn("<SakuraRustCoreMemberManifest>", msbuild_target)
        self.assertIn("<SakuraRustCoreStamp>", msbuild_target)
        self.assertIn('Include="$(MSBuildThisFileFullPath)"', msbuild_target)
        self.assertIn(
            r'Include="$(SakuraRustCoreWorkspace)\sakura_rust_core\src\**\*.rs"',
            msbuild_target,
        )
        self.assertIn('Outputs="$(SakuraRustCoreStamp)"', msbuild_target)
        self.assertIn("<Touch Files=\"$(SakuraRustCoreStamp)\"", msbuild_target)
        self.assertIn("!Exists('$(SakuraRustCoreLibrary)')", msbuild_target)
        self.assertIn("--locked", msbuild_target)

    def test_rust_avx512_public_abi_matches_global_dispatch_contract(self) -> None:
        header = (REPO_ROOT / "sakura_core/util/RustUtf16Scan.h").read_text(
            encoding="utf-8-sig"
        )
        rust_source = (REPO_ROOT / "rust/sakura_rust_core/src/lib.rs").read_text(
            encoding="utf-8"
        )
        dispatch_source = (REPO_ROOT / "sakura_core/util/CpuDispatch.cpp").read_text(
            encoding="utf-8-sig"
        )

        contract_match = re.search(
            r"(?P<contract>(?://[^\n]*\n)+)"
            r"std::size_t sakura_utf16_find_cr_or_lf_avx512bw_v1\s*\(",
            header,
        )
        self.assertIsNotNone(contract_match)
        assert contract_match is not None
        contract_comment = re.sub(
            r"\s+",
            " ",
            re.sub(r"(?m)^// ?", "", contract_match.group("contract")),
        ).strip()
        for required in (
            "process-wide AVX-512 tier",
            "AVX2",
            "AVX512F",
            "AVX512BW",
            "OSXSAVE/XCR0 XMM/YMM/opmask/ZMM",
            "tail delegates to `FindCrOrLfAvx2`",
        ):
            self.assertIn(required, contract_comment)

        declaration_group = header[
            contract_match.start() : header.index("\n}", contract_match.end())
        ]
        self.assertEqual(
            3,
            len(
                re.findall(
                    r"\bsakura_utf16_find_[a-z_]+_avx512bw_v1\s*\(",
                    declaration_group,
                )
            ),
        )

        overview_start = header.index("// The caller must also prove")
        overview = re.sub(
            r"\s+",
            " ",
            header[overview_start : header.index('extern "C"', overview_start)],
        )
        avx512_overview = overview[overview.index("AVX512BW requires") :]
        for required in ("AVX2", "AVX512F", "AVX512BW", "OSXSAVE/XCR0"):
            self.assertIn(required, avx512_overview)

        self.assertEqual(
            3,
            rust_source.count('#[target_feature(enable = "avx2,avx512f,avx512bw")]'),
        )
        self.assertIn("capabilities.avx2 && cpuAvx512", dispatch_source)

    @unittest.skipUnless(CMAKE_EXECUTABLE, "cmake is required")
    def test_invalid_backend_configures_fail_before_project(self) -> None:
        cmake = CMAKE_EXECUTABLE
        assert cmake is not None
        cases = (
            (
                "auto",
                ["-DSAKURA_UTF16_BACKEND=auto"],
                "SAKURA_UTF16_BACKEND must be exactly cpp or rust",
            ),
            (
                "unknown",
                ["-DSAKURA_UTF16_BACKEND=not-a-backend"],
                "SAKURA_UTF16_BACKEND must be exactly cpp or rust",
            ),
            (
                "both",
                [
                    "-DSAKURA_UTF16_BACKEND=both",
                ],
                "SAKURA_UTF16_BACKEND must be exactly cpp or rust",
            ),
            (
                "cpp-production",
                [
                    "-DSAKURA_UTF16_BACKEND=cpp",
                    "-DSAKURA_UTF16_PRODUCTION_PACKAGE=ON",
                ],
                "The C++ UTF-16 backend cannot package production",
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, options, expected in cases:
                with self.subTest(case=name):
                    started = time.monotonic()
                    result = subprocess.run(
                        [
                            cmake,
                            "-S",
                            str(REPO_ROOT),
                            "-B",
                            str(root / name),
                            *options,
                        ],
                        capture_output=True,
                        check=False,
                        text=True,
                        timeout=10,
                    )
                    elapsed = time.monotonic() - started
                    output = result.stdout + result.stderr
                    normalized_output = re.sub(r"\s+", " ", output)
                    self.assertNotEqual(0, result.returncode, output)
                    self.assertIn(expected, normalized_output)
                    self.assertLess(elapsed, 8.0, output)

    @unittest.skipUnless(CMAKE_EXECUTABLE, "cmake is required")
    def test_lowercase_production_environment_rejects_removed_backend(self) -> None:
        cmake = CMAKE_EXECUTABLE
        assert cmake is not None
        with tempfile.TemporaryDirectory() as temporary:
            environment = os.environ.copy()
            environment["SAKURA_UTF16_PRODUCTION_PACKAGE"] = "true"
            started = time.monotonic()
            result = subprocess.run(
                [
                    cmake,
                    "-S",
                    str(REPO_ROOT),
                    "-B",
                    str(Path(temporary) / "both-production-lowercase-env"),
                    "-DSAKURA_UTF16_BACKEND=both",
                ],
                capture_output=True,
                check=False,
                env=environment,
                text=True,
                timeout=10,
            )
            elapsed = time.monotonic() - started
            output = result.stdout + result.stderr
            normalized_output = re.sub(r"\s+", " ", output)
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn(
                "SAKURA_UTF16_BACKEND must be exactly cpp or rust",
                normalized_output,
            )
            self.assertLess(elapsed, 8.0, output)

    @unittest.skipUnless(CMAKE_EXECUTABLE, "cmake is required")
    def test_cmake_backend_module_accepts_rust_and_rejects_cpp_production(self) -> None:
        cmake = CMAKE_EXECUTABLE
        assert cmake is not None
        module = (REPO_ROOT / "src/main/cmake/sakura-utf16-backend.cmake").as_posix()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            probe = root / "probe.cmake"
            probe.write_text(
                f'include("{module}")\n'
                'message(STATUS "contract backend=${SAKURA_UTF16_BACKEND}")\n'
                'message(STATUS "contract production=${SAKURA_UTF16_PRODUCTION_PACKAGE}")\n',
                encoding="utf-8",
            )

            def run_case(
                name: str,
                options: list[str],
                backend: str | None,
                production: str | None,
            ) -> subprocess.CompletedProcess[str]:
                environment = os.environ.copy()
                environment.pop("SAKURA_UTF16_BACKEND", None)
                environment.pop("SAKURA_UTF16_PRODUCTION_PACKAGE", None)
                if backend is not None:
                    environment["SAKURA_UTF16_BACKEND"] = backend
                if production is not None:
                    environment["SAKURA_UTF16_PRODUCTION_PACKAGE"] = production
                return subprocess.run(
                    [cmake, *options, "-P", str(probe)],
                    capture_output=True,
                    check=False,
                    env=environment,
                    text=True,
                    timeout=10,
                )

            accepted = (
                (
                    "explicit-rust-production",
                    [
                        "-DSAKURA_UTF16_BACKEND=rust",
                        "-DSAKURA_UTF16_PRODUCTION_PACKAGE=ON",
                    ],
                    None,
                    None,
                    "rust",
                ),
                (
                    "implicit-rust-production",
                    ["-DSAKURA_UTF16_PRODUCTION_PACKAGE=ON"],
                    None,
                    None,
                    "rust",
                ),
                (
                    "ambient-rust-production",
                    [],
                    "rust",
                    "true",
                    "rust",
                ),
                (
                    "explicit-rust-nonproduction",
                    ["-DSAKURA_UTF16_BACKEND=rust"],
                    None,
                    None,
                    "rust",
                ),
                ("ambient-rust-nonproduction", [], "rust", None, "rust"),
            )
            for name, options, backend, production, expected_backend in accepted:
                with self.subTest(case=name):
                    result = run_case(name, options, backend, production)
                    output = result.stdout + result.stderr
                    self.assertEqual(0, result.returncode, output)
                    self.assertIn(f"contract backend={expected_backend}", output)

            rejected = (
                (
                    "ambient-both",
                    "both",
                    None,
                    "SAKURA_UTF16_BACKEND must be exactly cpp or rust",
                ),
            )
            for name, backend, production, expected in rejected:
                with self.subTest(case=name):
                    result = run_case(name, [], backend, production)
                    output = re.sub(r"\s+", " ", result.stdout + result.stderr)
                    self.assertNotEqual(0, result.returncode, output)
                    self.assertIn(expected, output)

    @unittest.skipUnless(MSBUILD_EXECUTABLE, "MSBuild is required")
    def test_msbuild_production_backend_contract_fails_closed(self) -> None:
        msbuild = MSBUILD_EXECUTABLE
        assert msbuild is not None
        cases = (
            (
                "both",
                "both",
                "false",
                "MSBuild requires SAKURA_UTF16_BACKEND=rust",
            ),
            (
                "rust-production-telemetry",
                "rust",
                "true",
                "SAKURA_UTF16_BENCHMARK_TELEMETRY is test-only and cannot package production",
            ),
            (
                "cpp-production",
                "cpp",
                "false",
                "MSBuild requires SAKURA_UTF16_BACKEND=rust",
            ),
        )
        for name, backend, telemetry, expected in cases:
            with self.subTest(case=name):
                started = time.monotonic()
                result = subprocess.run(
                    [
                        msbuild,
                        str(REPO_ROOT / "sakura_core/sakura.vcxproj"),
                        "/t:ValidateSakuraRustCoreBackend",
                        "/p:Platform=x64",
                        "/p:Configuration=Debug",
                        f"/p:SAKURA_UTF16_BACKEND={backend}",
                        "/p:SAKURA_UTF16_PRODUCTION_PACKAGE=true",
                        f"/p:SAKURA_UTF16_BENCHMARK_TELEMETRY={telemetry}",
                        "/nr:false",
                        "/nologo",
                    ],
                    capture_output=True,
                    check=False,
                    encoding="utf-8",
                    errors="replace",
                    text=True,
                    timeout=10,
                )
                elapsed = time.monotonic() - started
                output = result.stdout + result.stderr
                normalized_output = re.sub(r"\s+", " ", output)
                self.assertNotEqual(0, result.returncode, output)
                self.assertIn(expected, normalized_output)
                self.assertLess(elapsed, 8.0, output)

    @unittest.skipUnless(MSBUILD_EXECUTABLE, "MSBuild is required")
    def test_msbuild_accepts_rust_for_all_configurations(self) -> None:
        msbuild = MSBUILD_EXECUTABLE
        assert msbuild is not None
        project = str(REPO_ROOT / "sakura_core/sakura.vcxproj")
        cases = (
            (
                "explicit-rust-nonproduction",
                [
                    "/p:SAKURA_UTF16_BACKEND=rust",
                    "/p:SAKURA_UTF16_PRODUCTION_PACKAGE=false",
                    "/p:SAKURA_UTF16_BENCHMARK_TELEMETRY=false",
                ],
                {},
            ),
            (
                "explicit-rust-production",
                [
                    "/p:SAKURA_UTF16_BACKEND=rust",
                    "/p:SAKURA_UTF16_PRODUCTION_PACKAGE=true",
                    "/p:SAKURA_UTF16_BENCHMARK_TELEMETRY=false",
                ],
                {},
            ),
            (
                "implicit-rust-production",
                [
                    "/p:SAKURA_UTF16_PRODUCTION_PACKAGE=true",
                    "/p:SAKURA_UTF16_BENCHMARK_TELEMETRY=false",
                ],
                {},
            ),
            (
                "ambient-rust-nonproduction",
                [
                    "/p:SAKURA_UTF16_PRODUCTION_PACKAGE=false",
                    "/p:SAKURA_UTF16_BENCHMARK_TELEMETRY=false",
                ],
                {"SAKURA_UTF16_BACKEND": "rust"},
            ),
        )
        for name, properties, overrides in cases:
            with self.subTest(case=name):
                environment = os.environ.copy()
                environment.pop("SAKURA_UTF16_BACKEND", None)
                environment.pop("SAKURA_UTF16_PRODUCTION_PACKAGE", None)
                environment.update(overrides)
                started = time.monotonic()
                result = subprocess.run(
                    [
                        msbuild,
                        project,
                        "/t:ValidateSakuraRustCoreBackend",
                        "/p:Platform=x64",
                        "/p:Configuration=Debug",
                        *properties,
                        "/nr:false",
                        "/nologo",
                    ],
                    capture_output=True,
                    check=False,
                    env=environment,
                    encoding="utf-8",
                    errors="replace",
                    text=True,
                    timeout=10,
                )
                elapsed = time.monotonic() - started
                output = result.stdout + result.stderr
                self.assertEqual(0, result.returncode, output)
                self.assertLess(elapsed, 8.0, output)

    @unittest.skipUnless(MSBUILD_EXECUTABLE, "MSBuild is required")
    def test_msbuild_missing_cargo_fails_with_current_output(self) -> None:
        msbuild = MSBUILD_EXECUTABLE
        assert msbuild is not None
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary) / "sakura_rust_core.lib"
            library.write_bytes(b"up-to-date output placeholder")
            original = library.read_bytes()
            started = time.monotonic()
            result = subprocess.run(
                [
                    msbuild,
                    str(REPO_ROOT / "sakura_core/sakura.vcxproj"),
                    "/t:BuildSakuraRustCore",
                    "/p:Platform=x64",
                    "/p:Configuration=Debug",
                    "/p:SAKURA_UTF16_BACKEND=rust",
                    "/p:SakuraRustCoreCargo=issue239-cargo-missing",
                    f"/p:SakuraRustCoreLibrary={library}",
                    "/m:1",
                    "/nr:false",
                    "/nologo",
                ],
                capture_output=True,
                check=False,
                encoding="utf-8",
                errors="replace",
                text=True,
                timeout=10,
            )
            elapsed = time.monotonic() - started
            output = result.stdout + result.stderr
            normalized_output = re.sub(r"\s+", " ", output)
            self.assertNotEqual(0, result.returncode, output)
            self.assertIn("--version", normalized_output)
            self.assertIn("MSB3073", normalized_output)
            self.assertEqual(original, library.read_bytes())
            self.assertLess(elapsed, 8.0, output)

    def test_production_custom_commands_declare_real_inputs_and_outputs(self) -> None:
        cmake_root = REPO_ROOT / "src/main/cmake"
        ctags_text = (cmake_root / "ctags.cmake").read_text(encoding="utf-8-sig")
        self.assertNotIn('${CMAKE_SOURCE_DIR}/externals/ctags/.git', ctags_text)
        self.assertNotIn("observe_ctags_gitlink", ctags_text)
        self.assertIn("ctags_build_if_needed.cmake", ctags_text)

        ctags = _generation_records(cmake_root / "ctags.cmake")
        archive = _record_for_output(ctags, "${CTAGS_SOURCE_ARCHIVE}")
        self.assertEqual(
            {
                "${CMAKE_SOURCE_DIR}/.gitmodules",
                "${CTAGS_BUILD_SCRIPT}",
                "${CTAGS_GITLINK_SCRIPT}",
                "${CTAGS_ARCHIVE_SCRIPT}",
            },
            set(archive["inputs"]),
        )
        self.assertEqual("custom-target", archive["kind"])
        self.assertEqual("generate_ctags", archive["target"])
        self.assertTrue(
            {
                "${CTAGS_EXPECTED_COMMIT}",
                "${CTAGS_SOURCE_ARCHIVE}",
                "${CTAGS_GENERATED}",
                "${CTAGS_EXECUTABLE}",
                "${CTAGS_BUILD_STATE}",
            }.issubset(set(archive["byproducts"]))
        )
        ctags_outputs = [
            record for record in ctags if "${CTAGS_EXECUTABLE}" in record["byproducts"]
        ]
        self.assertTrue(ctags_outputs)
        self.assertTrue(all(record["inputs"] for record in ctags_outputs))
        self.assertTrue(
            any("${CTAGS_ZIP_FILE}" in record["inputs"] for record in ctags_outputs)
        )

        diffutils = _generation_records(cmake_root / "diffutils.cmake")
        archive_diff = next(
            record
            for record in diffutils
            if "${OUTPUT_DIRECTORY}/libintl3.dll" in record["byproducts"]
        )
        self.assertEqual(
            {
                "${DIFF_EXECUTABLE}",
                "${OUTPUT_DIRECTORY}/libintl3.dll",
                "${OUTPUT_DIRECTORY}/libiconv2.dll",
                "${DIFFUTILS_STATE}",
            },
            set(archive_diff["byproducts"]),
        )
        self.assertEqual(
            {
                "${DIFF_ZIP_FILE1}",
                "${DIFF_ZIP_FILE2}",
                "${ARCHIVE_RUNTIME_SCRIPT}",
            },
            set(archive_diff["inputs"]),
        )

        sakura = _generation_records(cmake_root / "sakura.cmake")
        expected = {
            "${CMAKE_BINARY_DIR}/version.h": {
                "${CMAKE_SOURCE_DIR}/src/main/cmake/version.cmake",
                "${CMAKE_SOURCE_DIR}/src/main/cmake/version.h.in",
            },
            "${CMAKE_BINARY_DIR}/sakura.exe.manifest": {
                "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.cmake",
                "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.in",
            },
            "${OUTPUT_DIRECTORY}/bregonig.dll": {
                "${BREGONIG_RUNTIME}",
                "${COPY_RUNTIME_ASSET_SCRIPT}",
            },
            "${OUTPUT_DIRECTORY}/migemo.dll": {
                "${CMIGEMO_RUNTIME}",
                "${COPY_RUNTIME_ASSET_SCRIPT}",
            },
            "${OUTPUT_DIRECTORY}/ppa_stub.dll": {
                "${PPA_STUB_RUNTIME}",
                "${COPY_RUNTIME_ASSET_SCRIPT}",
            },
            "${OUTPUT_DIRECTORY}/dll_plugin1.dll": {
                "${DLL_PLUGIN1_RUNTIME}",
                "${COPY_RUNTIME_ASSET_SCRIPT}",
            },
            "${SAKURA_MANIFEST_RC}": {
                "${SAKURA_EXE_MANIFEST}",
                "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.cmake",
                "${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.in",
            },
        }
        for output, inputs in expected.items():
            with self.subTest(output=output):
                self.assertEqual(inputs, set(_record_for_output(sakura, output)["inputs"]))

    @unittest.skipUnless(GIT_EXECUTABLE and CMAKE_EXECUTABLE, "git and cmake are required")
    def test_version_header_prefers_explicit_build_source_sha(self) -> None:
        git = GIT_EXECUTABLE
        cmake = CMAKE_EXECUTABLE
        assert git is not None and cmake is not None
        script = REPO_ROOT / "src/main/cmake/version.cmake"
        source_sha = "0123456789abcdef0123456789abcdef01234567"
        workflow_sha = "fedcba9876543210fedcba9876543210fedcba98"
        env = {
            **os.environ,
            "GITHUB_ACTIONS": "true",
            "GITHUB_REPOSITORY": "owner/repository",
            "GITHUB_SERVER_URL": "https://github.example.invalid",
            "GITHUB_SHA": workflow_sha,
            "SAKURA_BUILD_SOURCE_SHA": source_sha,
        }
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "version.h"
            result = subprocess.run(
                [
                    cmake,
                    f"-DSOURCE_DIR:PATH={REPO_ROOT}",
                    f"-DGIT_EXECUTABLE:FILEPATH={git}",
                    f"-DOUTPUT_FILE:FILEPATH={output}",
                    "-DQUIET=ON",
                    "-P",
                    str(script),
                ],
                capture_output=True,
                check=False,
                env=env,
                text=True,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            rendered = output.read_text(encoding="utf-8")

        self.assertIn(f'#define GIT_COMMIT_HASH "{source_sha}"', rendered)
        self.assertIn(f'#define GIT_SHORT_COMMIT_HASH "{source_sha[:8]}"', rendered)
        self.assertNotIn(workflow_sha, rendered)

    @unittest.skipUnless(CMAKE_EXECUTABLE, "cmake is required")
    def test_copy_runtime_asset_preserves_noop_timestamp(self) -> None:
        cmake = CMAKE_EXECUTABLE
        assert cmake is not None
        script = REPO_ROOT / "src/main/cmake/copy_runtime_asset.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.dll"
            output = root / "stage/runtime.dll"
            source.write_bytes(b"runtime")
            command = [
                cmake,
                f"-DINPUT_FILE:FILEPATH={source}",
                f"-DOUTPUT_FILE:FILEPATH={output}",
                "-P",
                str(script),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            forced_mtime = 1_700_000_000_000_000_000
            os.utime(output, ns=(forced_mtime, forced_mtime))
            time.sleep(0.02)
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(forced_mtime, output.stat().st_mtime_ns)

    @unittest.skipUnless(CMAKE_EXECUTABLE and SEVEN_ZIP_EXECUTABLE, "cmake and 7z are required")
    def test_archive_runtime_preserves_noop_timestamps(self) -> None:
        cmake = CMAKE_EXECUTABLE
        seven_zip = SEVEN_ZIP_EXECUTABLE
        assert cmake is not None and seven_zip is not None
        script = REPO_ROOT / "src/main/cmake/archive_runtime_if_needed.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "ctags.zip"
            with zipfile.ZipFile(archive, "w") as target:
                target.writestr("ctags.exe", b"ctags")
            output = root / "stage/ctags.exe"
            state = root / "state/ctags.txt"
            command = [
                cmake,
                "-DMODE:STRING=CTAGS",
                f"-DSEVEN_ZIP_EXECUTABLE:FILEPATH={seven_zip}",
                f"-DOUTPUT_DIRECTORY:PATH={output.parent}",
                f"-DSTATE_FILE:FILEPATH={state}",
                "-DBUILD_SIGNATURE:STRING=test-signature",
                f"-DARCHIVE_FILE:FILEPATH={archive}",
                f"-DOUTPUT_FILE:FILEPATH={output}",
                "-P",
                str(script),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            first_times = (output.stat().st_mtime_ns, state.stat().st_mtime_ns)
            time.sleep(0.02)
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(first_times, (output.stat().st_mtime_ns, state.stat().st_mtime_ns))
            output.write_bytes(b"corrupt")
            corrupted_mtime = output.stat().st_mtime_ns
            time.sleep(0.02)
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(b"ctags", output.read_bytes())
            repaired_mtime = output.stat().st_mtime_ns
            self.assertNotEqual(corrupted_mtime, repaired_mtime)
            repaired_state_mtime = state.stat().st_mtime_ns
            time.sleep(0.02)
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(repaired_mtime, output.stat().st_mtime_ns)
            self.assertEqual(repaired_state_mtime, state.stat().st_mtime_ns)

    @unittest.skipUnless(GIT_EXECUTABLE and CMAKE_EXECUTABLE, "git and cmake are required")
    def test_ctags_cached_build_path_skips_toolchain_and_is_content_stable(self) -> None:
        git = GIT_EXECUTABLE
        cmake = CMAKE_EXECUTABLE
        assert git is not None and cmake is not None
        script = REPO_ROOT / "src/main/cmake/ctags_build_if_needed.cmake"
        gitlink_script = REPO_ROOT / "src/main/cmake/gitlink_state.cmake"
        archive_script = REPO_ROOT / "src/main/cmake/git_submodule_update_locked.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            parent = root / "parent"
            child.mkdir()
            parent.mkdir()
            self._git(git, child, "init")
            (child / "payload.txt").write_text("payload\n", encoding="utf-8")
            self._git(git, child, "add", "payload.txt")
            self._git(git, child, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "payload")
            commit = self._git(git, child, "rev-parse", "HEAD").stdout.strip()
            self._git(git, parent, "init")
            self._git(git, parent, "update-index", "--add", "--cacheinfo", f"160000,{commit},externals/ctags")

            generated = root / "build/ctags/ctags.exe"
            generated.parent.mkdir(parents=True)
            generated.write_bytes(b"cached ctags")
            state = root / "state/built-state.txt"
            state.parent.mkdir(parents=True)
            state.write_text(
                f"{commit}\ntest-signature\n"
                f"{self._sha256(script)}\n"
                f"{self._sha256(gitlink_script)}\n"
                f"{self._sha256(archive_script)}\n"
                f"generated={self._sha256(generated)}\n",
                encoding="utf-8",
            )
            expected = root / "state/expected-commit.txt"
            output = root / "stage/ctags.exe"
            command = [
                cmake,
                f"-DGIT_EXECUTABLE:FILEPATH={git}",
                f"-DREPO_ROOT:PATH={parent}",
                "-DSUBMODULE_PATH:STRING=externals/ctags",
                f"-DLOCK_PATH:FILEPATH={root / 'state/ctags.lock'}",
                f"-DGITLINK_SCRIPT:FILEPATH={gitlink_script}",
                f"-DARCHIVE_SCRIPT:FILEPATH={archive_script}",
                f"-DEXPECTED_COMMIT_FILE:FILEPATH={expected}",
                f"-DARCHIVE_FILE:FILEPATH={root / 'state/source.zip'}",
                f"-DBUILD_DIR:PATH={generated.parent}",
                f"-DGENERATED_FILE:FILEPATH={generated}",
                f"-DOUTPUT_FILE:FILEPATH={output}",
                f"-DSTATE_FILE:FILEPATH={state}",
                "-DSEVEN_ZIP_EXECUTABLE:FILEPATH=must-not-run-7z",
                "-DCMD_VS_DEV:FILEPATH=must-not-run-vsdevcmd",
                "-DHOST_ARCH:STRING=x64",
                "-DBUILD_SIGNATURE:STRING=test-signature",
                "-P",
                str(script),
            ]
            cached = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(0, cached.returncode, cached.stdout + cached.stderr)
            first_times = (
                expected.stat().st_mtime_ns,
                state.stat().st_mtime_ns,
                generated.stat().st_mtime_ns,
                output.stat().st_mtime_ns,
            )
            time.sleep(0.02)
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(
                first_times,
                (
                    expected.stat().st_mtime_ns,
                    state.stat().st_mtime_ns,
                    generated.stat().st_mtime_ns,
                    output.stat().st_mtime_ns,
                ),
            )
            generated.write_bytes(b"corrupt")
            invalidated = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertNotEqual(0, invalidated.returncode)
            self.assertIn("Could not materialize the committed ctags source", invalidated.stderr)

    @unittest.skipUnless(GIT_EXECUTABLE and CMAKE_EXECUTABLE, "git and cmake are required")
    def test_gitlink_state_is_content_stable_and_tracks_index_commit(self) -> None:
        git = GIT_EXECUTABLE
        cmake = CMAKE_EXECUTABLE
        assert git is not None and cmake is not None
        script = REPO_ROOT / "src/main/cmake/gitlink_state.cmake"

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            parent = root / "parent"
            child.mkdir()
            parent.mkdir()
            self._git(git, child, "init")
            (child / "payload.txt").write_text("first\n", encoding="utf-8")
            self._git(git, child, "add", "payload.txt")
            self._git(git, child, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "first")
            first_commit = self._git(git, child, "rev-parse", "HEAD").stdout.strip()
            (child / "payload.txt").write_text("second\n", encoding="utf-8")
            self._git(git, child, "add", "payload.txt")
            self._git(git, child, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "second")
            second_commit = self._git(git, child, "rev-parse", "HEAD").stdout.strip()

            self._git(git, parent, "init")
            self._git(git, parent, "update-index", "--add", "--cacheinfo", f"160000,{first_commit},externals/ctags")
            output = root / "state/expected-commit.txt"
            self._run_cmake_script(cmake, script, git, parent, output)
            self.assertEqual(first_commit, output.read_text(encoding="utf-8").strip())
            first_mtime = output.stat().st_mtime_ns
            time.sleep(0.02)
            self._run_cmake_script(cmake, script, git, parent, output)
            self.assertEqual(first_mtime, output.stat().st_mtime_ns)

            self._git(git, parent, "update-index", "--cacheinfo", f"160000,{second_commit},externals/ctags")
            time.sleep(0.02)
            self._run_cmake_script(cmake, script, git, parent, output)
            self.assertEqual(second_commit, output.read_text(encoding="utf-8").strip())
            self.assertGreater(output.stat().st_mtime_ns, first_mtime)

    @unittest.skipUnless(GIT_EXECUTABLE and CMAKE_EXECUTABLE, "git and cmake are required")
    def test_locked_submodule_archive_matches_expected_commit_and_fails_closed(self) -> None:
        git = GIT_EXECUTABLE
        cmake = CMAKE_EXECUTABLE
        assert git is not None and cmake is not None
        state_script = REPO_ROOT / "src/main/cmake/gitlink_state.cmake"
        archive_script = REPO_ROOT / "src/main/cmake/git_submodule_update_locked.cmake"

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            parent = root / "parent"
            child.mkdir()
            parent.mkdir()
            self._git(git, child, "init")
            (child / "payload.txt").write_text("committed payload\n", encoding="utf-8")
            self._git(git, child, "add", "payload.txt")
            self._git(git, child, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "payload")

            self._git(git, parent, "init")
            env = {**os.environ, "GIT_ALLOW_PROTOCOL": "file"}
            self._git(git, parent, "-c", "protocol.file.allow=always", "submodule", "add", str(child), "externals/ctags", env=env)
            self._git(git, parent, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-am", "submodule")

            expected = root / "state/expected-commit.txt"
            archive = root / "state/ctags-source.zip"
            self._run_cmake_script(cmake, state_script, git, parent, expected)
            result = subprocess.run(
                [
                    cmake,
                    f"-DGIT_EXECUTABLE:FILEPATH={git}",
                    f"-DREPO_ROOT:PATH={parent}",
                    "-DSUBMODULE_PATH:STRING=externals/ctags",
                    f"-DLOCK_PATH:FILEPATH={root / 'state/submodule.lock'}",
                    f"-DEXPECTED_COMMIT_FILE:FILEPATH={expected}",
                    f"-DARCHIVE_FILE:FILEPATH={archive}",
                    "-P",
                    str(archive_script),
                ],
                text=True,
                capture_output=True,
                env=env,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            with zipfile.ZipFile(archive) as source_archive:
                self.assertEqual(
                    ["committed payload"],
                    source_archive.read("payload.txt").decode("utf-8").splitlines(),
                )

            expected.write_text("0" * 40 + "\n", encoding="utf-8")
            mismatch = subprocess.run(
                [
                    cmake,
                    f"-DGIT_EXECUTABLE:FILEPATH={git}",
                    f"-DREPO_ROOT:PATH={parent}",
                    "-DSUBMODULE_PATH:STRING=externals/ctags",
                    f"-DLOCK_PATH:FILEPATH={root / 'state/submodule.lock'}",
                    f"-DEXPECTED_COMMIT_FILE:FILEPATH={expected}",
                    f"-DARCHIVE_FILE:FILEPATH={archive}",
                    "-P",
                    str(archive_script),
                ],
                text=True,
                capture_output=True,
                env=env,
                check=False,
            )
            self.assertNotEqual(0, mismatch.returncode)
            self.assertIn("not at expected gitlink", mismatch.stdout + mismatch.stderr)

    @staticmethod
    def _git(
        git: str,
        cwd: Path,
        *args: str,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [git, *args],
            cwd=cwd,
            text=True,
            capture_output=True,
            env=env,
            check=True,
        )

    @staticmethod
    def _sha256(path: Path) -> str:
        import hashlib

        return hashlib.sha256(path.read_bytes()).hexdigest()

    @staticmethod
    def _run_cmake_script(
        cmake: str,
        script: Path,
        git: str,
        parent: Path,
        output: Path,
    ) -> None:
        result = subprocess.run(
            [
                cmake,
                f"-DGIT_EXECUTABLE:FILEPATH={git}",
                f"-DREPO_ROOT:PATH={parent}",
                "-DSUBMODULE_PATH:STRING=externals/ctags",
                f"-DOUTPUT_FILE:FILEPATH={output}",
                "-P",
                str(script),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
