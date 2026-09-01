"""Issue #185: owned bregonig snapshot, provider ABI, and bron420 removal."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CBREGEXP = REPO_ROOT / "sakura_core/extmodule/CBregexpDll2.h"
PORTFILE = REPO_ROOT / "tools/vcpkg-local-registry/ports/bregonig/portfile.cmake"
SNAPSHOT = REPO_ROOT / "third_party/owned/bregonig-next"
CONTRACT = REPO_ROOT / "src/test/cpp/tests1/extmodule/BregexpProviderContractTest.cpp"
CORPUS = REPO_ROOT / "src/test/resources/tests1/bregonig/oracle-corpus.json"
BRON_ZIP = REPO_ROOT / "installer/externals/bregonig/bron420.zip"
ORACLE = REPO_ROOT / "installer/externals/bregonig/ORACLE.json"
VCXPROJ = REPO_ROOT / "sakura_core/tests1.vcxproj"
FILTERS = REPO_ROOT / "sakura_core/tests1.vcxproj.filters"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


class BregonigOwnedContractTests(unittest.TestCase):
    def test_provider_header_is_the_only_bregexp_layout(self) -> None:
        text = _read(CBREGEXP)
        self.assertIn("#include <bregexp.h>", text)
        self.assertNotIn("typedef struct bregexp", text)
        self.assertNotIn("int rsv1", text)
        self.assertIn("using BREGEXP_W = BREGEXP", text)

    def test_port_builds_the_owned_cmake_tree(self) -> None:
        text = _read(PORTFILE)
        self.assertIn("third_party/owned/bregonig-next", text)
        self.assertIn("vcpkg_cmake_configure", text)
        self.assertIn("vcpkg_cmake_install", text)
        self.assertNotIn("nmake", text.lower())
        self.assertTrue((SNAPSHOT / "CMakeLists.txt").is_file())
        self.assertTrue((SNAPSHOT / "src/bregexp.h").is_file())
        self.assertTrue((SNAPSHOT / ".github/workflows/ci.yml").is_file())

    def test_mingw_provider_keeps_gcc_runtimes_out_of_the_package_boundary(self) -> None:
        port = _read(PORTFILE)
        contract = _read(CONTRACT)
        self.assertIn("VCPKG_TARGET_IS_MINGW", port)
        self.assertIn("-static -static-libgcc -static-libstdc++", port)
        self.assertIn('#if defined(__MINGW32__)', contract)
        self.assertIn('"msvcrt.dll"', contract)
        self.assertNotIn('"libgcc_s_seh-1.dll"', contract)
        self.assertNotIn('"libstdc++-6.dll"', contract)
        self.assertNotIn('"libwinpthread-1.dll"', contract)

    def test_snapshot_forbids_local_edits(self) -> None:
        manifest = json.loads(_read(SNAPSHOT / "SNAPSHOT.json"))
        self.assertFalse(manifest["localModificationAllowed"])
        header = _read(SNAPSHOT / "src/bregexp.h")
        self.assertIn("INT_PTR rsv1", header)

    def test_corpus_ids_are_compiled_into_tests1(self) -> None:
        cpp = _read(CONTRACT)
        rows = json.loads(_read(CORPUS))
        for row in rows:
            self.assertIn(row["id"], cpp, row["id"])
        self.assertIn("BregexpProviderContractTest.cpp", _read(VCXPROJ))
        self.assertIn("BregexpProviderContractTest.cpp", _read(FILTERS))

    def test_bron420_archive_and_readers_are_gone(self) -> None:
        self.assertFalse(BRON_ZIP.is_file())
        self.assertFalse(ORACLE.is_file())


if __name__ == "__main__":
    unittest.main()
