from __future__ import annotations

import os
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
