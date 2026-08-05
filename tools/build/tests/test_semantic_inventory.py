from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.semantic_inventory import (  # noqa: E402
    collect_semantic_inventory,
    compare_semantic_inventory,
    write_semantic_inventory,
)


def _write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


class SemanticInventoryTests(unittest.TestCase):
    def test_collects_hotspots_and_ratchet_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write(
                root,
                "sakura_core/window/CEditWnd.cpp",
                '#include "../doc/CEditDoc.h"\n'
                "void CEditWnd::f(HWND hwnd) { GetDllShareData(); GetEditWnd(); new Widget; delete widget; }\n",
            )
            _write(root, "sakura_core/doc/CEditDoc.cpp", "void f() { GetEditDoc(); }\n")
            _write(root, "sakura_core/window/CEditWnd.h", "class CEditWnd {};\n")
            _write(root, "src/test/cpp/tests1/test-window.cpp", "catch (...) {}\n")
            _write(root, "sakura_core/tests1.vcxproj", "CollectSakuraObjectsForTests1\n")
            _write(root, ".github/workflows/build.yml", "gtest_filter=-EditWndTest\n")

            inventory = collect_semantic_inventory(root)
            metrics = inventory["metrics"]
            self.assertEqual(1, metrics["direct_global_getter_calls"]["GetDllShareData"])
            self.assertEqual(1, metrics["direct_global_getter_calls"]["GetEditWnd"])
            self.assertEqual(1, metrics["direct_global_getter_calls"]["GetEditDoc"])
            self.assertEqual(1, metrics["raw_new_expression_count"])
            self.assertEqual(1, metrics["raw_delete_expression_count"])
            self.assertEqual(1, metrics["catch_all_count"])
            self.assertEqual(1, metrics["monolith_link_hint_count"])
            self.assertEqual(1, metrics["filtered_test_hint_count"])
            self.assertIn("sakura_core/window/CEditWnd.cpp", inventory["hotspots"])

    def test_ratchet_reports_only_gated_increases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write(root, "sakura_core/a.cpp", "void f() { GetDllShareData(); }\n")
            baseline = collect_semantic_inventory(root)
            _write(root, "sakura_core/b.cpp", "void g() { GetDllShareData(); }\n")
            current = collect_semantic_inventory(root)
            comparison = compare_semantic_inventory(current, baseline)
            self.assertFalse(comparison["ok"])
            self.assertIn(
                {"metric": "direct_global_getter_calls.GetDllShareData", "baseline": 1, "current": 2, "delta": 1},
                comparison["increases"],
            )
            self.assertNotIn("source_file_count", {item["metric"] for item in comparison["increases"]})

    def test_writer_is_idempotent_and_json_is_stable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "semantic.json"
            inventory = {"schema_version": 1, "inventory_kind": "editor-core-semantic", "metrics": {"x": 1}}
            self.assertTrue(write_semantic_inventory(path, inventory))
            self.assertFalse(write_semantic_inventory(path, inventory))
            self.assertEqual(inventory, json.loads(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
