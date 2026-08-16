"""Contracts that each shipped runtime artifact has exactly one provider.

Issue #182 Phase 0: installer generation used to extract ``bregonig.dll`` from
``installer/externals/bregonig/bron420.zip`` into the MSBuild output directory
after CMake had already staged the vcpkg-built DLL. Tests therefore exercised
one binary and the installer shipped another.

The invariants pinned here are:

* packaging scripts consume the staged product DLL and source-tree licenses;
* they never extract ``bron420.zip`` into an output directory;
* CMake stages Debug from ``debug/bin`` and Release from ``bin`` via the
  imported target's ``TARGET_FILE``;
* ``generate_miniz`` is a ``tests1`` dependency, not an ``ALL`` product target;
* GoogleTest is not also present as ``externals/googletest``.

Like ``test_ctags_provenance.py``, this lives in ``src/test/py`` because the
CTest ``pytest`` target runs from the repository root under pytest's default
``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import hashlib
import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
BUILD_INSTALLER = REPO_ROOT / "build-installer.bat"
ZIP_ARTIFACTS = REPO_ROOT / "zipArtifacts.bat"
SAKURA_CMAKE = REPO_ROOT / "src/main/cmake/sakura.cmake"
TESTS1_CMAKE = REPO_ROOT / "src/test/cmake/tests1.cmake"
GITMODULES = REPO_ROOT / ".gitmodules"
BREGONIG_LICENSE_DIR = REPO_ROOT / "externals/bregonig"
BREGONIG_ORACLE = REPO_ROOT / "installer/externals/bregonig/ORACLE.json"
BRON_ZIP = REPO_ROOT / "installer/externals/bregonig/bron420.zip"
SAKURA_ISS = REPO_ROOT / "installer/sakura-common.iss"
IDENTITY_TOOL = REPO_ROOT / "tools/verify_runtime_artifact_identity.py"
INNOUNP_EXE = REPO_ROOT / "tools/innounp/innounp.exe"
INNOUNP_PIN = REPO_ROOT / "tools/innounp/PIN.json"

BRON_ZIP_EXTRACT_RE = re.compile(
    r'7z[^\n]*bron420\.zip|bron420\.zip[^\n]*-o"%platform%\\%configuration%"',
    re.IGNORECASE,
)
STAGED_DLL_RE = re.compile(r"was not staged by the product build")
MINIZ_ALL_RE = re.compile(r"add_custom_target\s*\(\s*generate_miniz\s+ALL\b")
MINIZ_DEP_RE = re.compile(
    r"add_dependencies\s*\(\s*tests1\b[^)]*\bgenerate_miniz\b",
    re.DOTALL,
)
GITMODULE_GTEST_RE = re.compile(r"externals/googletest")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


class BregonigProviderTests(unittest.TestCase):
    def test_packaging_scripts_do_not_extract_bron420(self) -> None:
        for script in (BUILD_INSTALLER, ZIP_ARTIFACTS):
            with self.subTest(script=script.name):
                text = _read(script)
                self.assertIsNone(
                    BRON_ZIP_EXTRACT_RE.search(text),
                    f"{script.name} extracts bron420.zip into the output "
                    f"directory, so the shipped DLL would not be the DLL the "
                    f"product build staged",
                )

    def test_packaging_scripts_require_the_staged_dll(self) -> None:
        for script in (BUILD_INSTALLER, ZIP_ARTIFACTS):
            with self.subTest(script=script.name):
                text = _read(script)
                self.assertIsNotNone(
                    STAGED_DLL_RE.search(text),
                    f"{script.name} must fail closed when the vcpkg-staged "
                    f"DLL is missing instead of substituting another provider",
                )

    def test_licenses_come_from_the_source_tree(self) -> None:
        for name in ("bsd_license.txt", "perl_license.txt", "perl_license_jp.txt"):
            with self.subTest(license=name):
                self.assertTrue(
                    (BREGONIG_LICENSE_DIR / name).is_file(),
                    f"{name} is missing from externals/bregonig",
                )
        for script in (BUILD_INSTALLER, ZIP_ARTIFACTS):
            with self.subTest(script=script.name):
                text = _read(script)
                self.assertIn(
                    r"externals\bregonig",
                    text,
                    f"{script.name} must copy bregonig licenses from the "
                    f"source tree, not from a binary archive",
                )
                for name in ("bsd_license.txt", "perl_license.txt", "perl_license_jp.txt"):
                    self.assertIn(name, text)

    def test_cmake_stages_the_imported_configuration_dll(self) -> None:
        text = _read(SAKURA_CMAKE)
        self.assertIn("$<TARGET_FILE:bregonig::bregonig>", text)
        self.assertIn("$<TARGET_FILE:cmigemo::cmigemo>", text)
        self.assertNotRegex(
            text,
            r'set\(\s*BREGONIG_RUNTIME\s+"\$\{VCPKG_INSTALLED_DIR\}/[^"]*/bin/bregonig\.dll"\)',
            "hard-coding bin/ stages the Release DLL into Debug output",
        )
        self.assertNotRegex(
            text,
            r'set\(\s*CMIGEMO_RUNTIME\s+"\$\{VCPKG_INSTALLED_DIR\}/[^"]*/bin/migemo\.dll"\)',
            "hard-coding bin/ stages the Release DLL into Debug output",
        )


class BronOracleTests(unittest.TestCase):
    def test_oracle_records_the_frozen_archive_hash(self) -> None:
        oracle = json.loads(_read(BREGONIG_ORACLE))
        self.assertFalse(oracle["productProvider"])
        self.assertEqual(185, oracle["removedByIssue"])
        self.assertEqual(
            hashlib.sha256(BRON_ZIP.read_bytes()).hexdigest(),
            oracle["sha256"],
            "bron420.zip changed without updating ORACLE.json",
        )

    def test_committed_innounp_matches_its_pin(self) -> None:
        pin = json.loads(_read(INNOUNP_PIN))
        self.assertTrue(INNOUNP_EXE.is_file())
        self.assertFalse(pin["productProvider"])
        self.assertEqual(
            hashlib.sha256(INNOUNP_EXE.read_bytes()).hexdigest(),
            pin["sha256"],
            "tools/innounp/innounp.exe changed without updating PIN.json",
        )

    def test_packaging_scripts_record_sha256_identity(self) -> None:
        self.assertTrue(IDENTITY_TOOL.is_file())
        for script in (BUILD_INSTALLER, ZIP_ARTIFACTS):
            with self.subTest(script=script.name):
                text = _read(script)
                self.assertIn(
                    "verify_runtime_artifact_identity.py",
                    text,
                    f"{script.name} must hash staged DLLs against packaged "
                    f"payloads instead of trusting the copy step",
                )

    def test_installer_script_ships_both_staged_runtime_dlls(self) -> None:
        text = _read(SAKURA_ISS)
        self.assertIn('Source: "sakura\\bregonig.dll"', text)
        self.assertIn('Source: "sakura\\migemo.dll"', text)


class MinizProductGraphTests(unittest.TestCase):
    def test_generate_miniz_is_tests1_only(self) -> None:
        text = _read(TESTS1_CMAKE)
        self.assertIsNone(
            MINIZ_ALL_RE.search(text),
            "generate_miniz ALL puts miniz-cpp on the product default graph",
        )
        self.assertIsNotNone(
            MINIZ_DEP_RE.search(text),
            "tests1 must still depend on generate_miniz so the header exists "
            "when the test graph builds",
        )


class GoogletestProviderTests(unittest.TestCase):
    def test_googletest_is_not_a_product_submodule(self) -> None:
        text = _read(GITMODULES)
        self.assertIsNone(
            GITMODULE_GTEST_RE.search(text),
            "externals/googletest duplicates vcpkg GTest; keep one test-only "
            "provider",
        )
        self.assertFalse(
            (REPO_ROOT / "externals/googletest").exists(),
            "externals/googletest must be removed once the submodule is gone",
        )


if __name__ == "__main__":
    unittest.main()
