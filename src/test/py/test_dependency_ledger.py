"""Issue #184 dependency ledger: orthogonal enums, NOTICE, SPDX, provenance.

The committed ledger in ``src/main/dependencies/dependencies.json`` is the
source of truth. ``tools/dependency_ledger.py check`` must stay fail-closed:
unknown enums, a flat ``class`` field, missing gitmodule/vcpkg coverage,
stale NOTICE/SBOM, undeclared owned snapshots, and product includes outside
a row's roots all fail.

Like ``test_ctags_provenance.py``, this lives in ``src/test/py`` because the
CTest ``pytest`` target never collects ``tools/build/tests``.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
LEDGER = REPO_ROOT / "src/main/dependencies/dependencies.json"
SCHEMA = REPO_ROOT / "src/main/dependencies/schema.json"
NOTICE = REPO_ROOT / "src/main/dependencies/NOTICE"
SBOM = REPO_ROOT / "src/main/dependencies/sbom.spdx.json"
GITMODULES = REPO_ROOT / ".gitmodules"
WORKFLOW = REPO_ROOT / ".github/workflows/architecture-gates.yml"
OWNED_ROOT = REPO_ROOT / "third_party/owned"
TOOL = REPO_ROOT / "tools/dependency_ledger.py"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _ledger() -> dict:
    return json.loads(_read(LEDGER))


def _load_tool():
    spec = importlib.util.spec_from_file_location("dependency_ledger", TOOL)
    if spec is None or spec.loader is None:
        raise AssertionError("tools/dependency_ledger.py is missing")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DependencyLedgerTests(unittest.TestCase):
    def test_committed_ledger_check_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, str(TOOL), "check"],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)

    def test_architecture_gates_run_the_offline_check(self) -> None:
        text = _read(WORKFLOW)
        self.assertIn("python3 tools/dependency_ledger.py check", text)

    def test_schema_is_version_1_and_forbids_class(self) -> None:
        schema = json.loads(_read(SCHEMA))
        self.assertEqual(1, schema["properties"]["schemaVersion"]["const"])
        properties = schema["$defs"]["dependency"]["properties"]
        self.assertNotIn("class", properties)
        document = _ledger()
        self.assertEqual(1, document["schemaVersion"])
        for entry in document["dependencies"]:
            self.assertNotIn("class", entry)

    def test_class_field_is_rejected(self) -> None:
        tool = _load_tool()
        document = _ledger()
        document["dependencies"][0]["class"] = "absorb"
        with self.assertRaises(tool.LedgerError) as raised:
            tool.validate_document(document, json.loads(_read(SCHEMA)))
        self.assertIn("class", str(raised.exception))

    def test_unknown_enum_is_rejected(self) -> None:
        tool = _load_tool()
        document = _ledger()
        document["dependencies"][0]["ownership"] = "temporary"
        with self.assertRaises(tool.LedgerError) as raised:
            tool.validate_document(document, json.loads(_read(SCHEMA)))
        self.assertIn("ownership", str(raised.exception))

    def test_every_gitmodule_has_a_present_row(self) -> None:
        listed = {
            str(entry.get(key, "")).replace("\\", "/")
            for entry in _ledger()["dependencies"]
            if entry.get("status") == "present"
            for key in ("source", "maintainerCheckout")
        }
        paths = set()
        for line in _read(GITMODULES).splitlines():
            stripped = line.strip()
            if stripped.startswith("path"):
                paths.add(stripped.split("=", 1)[1].strip().replace("\\", "/"))
        missing = sorted(paths - listed)
        self.assertEqual(missing, [])

    def test_notice_and_sbom_are_generated_from_the_ledger(self) -> None:
        tool = _load_tool()
        document = _ledger()
        self.assertTrue(NOTICE.is_file())
        self.assertTrue(SBOM.is_file())
        self.assertEqual(
            NOTICE.read_text(encoding="utf-8").replace("\r\n", "\n"),
            tool.render_notice(document),
        )
        self.assertEqual(
            SBOM.read_text(encoding="utf-8").replace("\r\n", "\n"),
            tool.render_sbom(document),
        )
        self.assertIn("SPDX-2.3", SBOM.read_text(encoding="utf-8"))

    def test_ctags_and_diffutils_relate_unbundle_issues(self) -> None:
        by_id = {entry["id"]: entry for entry in _ledger()["dependencies"]}
        for entry_id in ("ctags", "diffutils"):
            related = set(by_id[entry_id].get("relatedIssues") or [])
            self.assertIn(125, related)
            self.assertIn(132, related)

    def test_owned_snapshot_tree_is_empty_or_declared(self) -> None:
        declared = {
            str(entry["snapshot"]).replace("\\", "/")
            for entry in _ledger()["dependencies"]
            if entry.get("status") == "present" and entry.get("snapshot")
        }
        if not OWNED_ROOT.is_dir():
            self.assertEqual(declared, set())
            return
        actual = {
            path.relative_to(REPO_ROOT).as_posix()
            for path in OWNED_ROOT.iterdir()
            if path.is_dir()
        }
        self.assertEqual(actual, declared)

    def test_runtime_dlls_have_one_product_provider(self) -> None:
        providers: dict[str, str] = {}
        for entry in _ledger()["dependencies"]:
            if entry.get("status") != "present":
                continue
            for artifact in entry.get("runtimeArtifacts") or []:
                if not artifact.get("productProvider"):
                    continue
                name = str(artifact["name"])
                self.assertNotIn(name, providers, name)
                providers[name] = str(entry["id"])
        self.assertEqual("bregonig-next", providers["bregonig.dll"])
        self.assertEqual("cmigemo-next", providers["migemo.dll"])


if __name__ == "__main__":
    unittest.main()
