"""Local-port source trees must participate in the package plan hash.

A vcpkg overlay portfile that builds ``${REPO_ROOT}/externals/<name>``
can change the restored DLL without touching ``vcpkg.json``. If that tree
is not a declared ``package_set`` input, ``plan_hash`` stays put and CI
reuses a stale closure.

Like ``test_ctags_provenance.py``, this lives in ``src/test/py`` because the
CTest ``pytest`` target never collects ``tools/build/tests``.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULES = REPO_ROOT / "src/main/modules/modules.json"
LOCAL_PORTS = REPO_ROOT / "tools/vcpkg-local-registry/ports"

REPO_ROOT_PATH_RE = re.compile(r"\$\{REPO_ROOT\}/([A-Za-z0-9_./-]+)")
SKIP_SUFFIXES = (
    "/LICENSE",
    "/LICENSE.md",
    "/COPYING",
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _package_set_inputs() -> list[str]:
    document = json.loads(_read(MODULES))
    for artifact in document["artifacts"]:
        if artifact.get("id") == "root-vcpkg-package-set":
            return [str(item).replace("\\", "/") for item in artifact["inputs"]]
    raise AssertionError("root-vcpkg-package-set is missing from modules.json")


def _covered(path: str, inputs: list[str]) -> bool:
    normalized = path.replace("\\", "/").rstrip("/")
    for declared in inputs:
        candidate = declared.replace("\\", "/").rstrip("/")
        if normalized == candidate or normalized.startswith(candidate + "/"):
            return True
        if candidate.startswith(normalized + "/"):
            return True
    return False


def _port_source_paths() -> set[str]:
    paths: set[str] = set()
    for portfile in sorted(LOCAL_PORTS.glob("*/portfile.cmake")):
        for match in REPO_ROOT_PATH_RE.finditer(_read(portfile)):
            relative = match.group(1).rstrip("/")
            if relative.endswith(SKIP_SUFFIXES) or relative in {"LICENSE", "LICENSE.md", "COPYING"}:
                continue
            paths.add(relative)
    return paths


class PackageSourceRevisionContractTests(unittest.TestCase):
    def test_every_local_port_source_tree_is_a_package_set_input(self) -> None:
        inputs = _package_set_inputs()
        missing = sorted(
            path for path in _port_source_paths() if not _covered(path, inputs)
        )
        self.assertEqual(
            missing,
            [],
            "local vcpkg ports build these trees, but root-vcpkg-package-set "
            "does not declare them, so a source revision change would keep "
            "the same plan_hash",
        )

    def test_local_port_source_trees_are_declared_explicitly(self) -> None:
        inputs = set(_package_set_inputs())
        for path in (
            "third_party/owned/bregonig-next",
            "externals/cmigemo",
            "externals/cmigemo-dict",
            "third_party/owned/onigmo-next",
            "externals/darkmodelib",
        ):
            with self.subTest(path=path):
                self.assertIn(path, inputs)


if __name__ == "__main__":
    unittest.main()
