from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.coverage_map import (  # noqa: E402
    ChangedFile,
    CoverageMapError,
    build_coverage_map,
    coverage_cache_key,
    load_module_index,
    parse_cobertura_fragment,
    select_tests,
    validate_coverage_map,
)
from sakura_build_lib.test_inventory import guarantee_fingerprint  # noqa: E402


BASE_SHA = "a" * 40
BINARY_SHA = "b" * 64


def inventory() -> dict:
    tests = [
        {
            "test_id": "tests1:SmokeSuite.AlwaysPass",
            "runtime": {"runner_id": "tests1", "selector": "SmokeSuite.AlwaysPass"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:FooTest.CoversSource",
            "runtime": {"runner_id": "tests1", "selector": "FooTest.CoversSource"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:FooTest.Other",
            "runtime": {"runner_id": "tests1", "selector": "FooTest.Other"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:BarTest.Unrelated",
            "runtime": {"runner_id": "tests1", "selector": "BarTest.Unrelated"},
            "status": "enabled",
        },
    ]
    return {
        "schema_version": 1,
        "inventory_id": "fixture",
        "source_revision": BASE_SHA,
        "source_dirty": False,
        "discovery": {
            "framework": "googletest",
            "executable": "x64/Debug/tests1.exe",
            "arguments": ["--gtest_list_tests"],
            "executable_sha256": BINARY_SHA,
        },
        "test_count": len(tests),
        "disabled_count": 0,
        "guarantee_fingerprint": guarantee_fingerprint(tests),
        "tests": tests,
    }


def cobertura(repo: Path, filename: str = "sakura_core/foo.cpp", hits: int = 1) -> str:
    return f"""<?xml version=\"1.0\"?>
<coverage>
  <sources><source>{repo.as_posix()}</source></sources>
  <packages><package><classes><class filename=\"{filename}\">
    <lines><line number=\"1\" hits=\"{hits}\"/></lines>
  </class></classes></package></packages>
</coverage>
"""


class CoverageMapTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "sakura_core").mkdir()
        (self.root / "sakura_core/foo.cpp").write_text("// fixture\n", encoding="utf-8")
        (self.root / "sakura_core/foo.hpp").write_text("// fixture\n", encoding="utf-8")
        self.xml = self.root / "FooTest.xml"
        self.xml.write_text(cobertura(self.root), encoding="utf-8")
        self.inventory = inventory()
        self.map = build_coverage_map(
            base_sha=BASE_SHA,
            test_binary_sha256=BINARY_SHA,
            inventory=self.inventory,
            fragments=(("FooTest.*", self.xml),),
            repo_root=self.root,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def decision(self, changed, **kwargs):
        expected_base_sha = kwargs.pop("expected_base_sha", BASE_SHA)
        smoke_selectors = kwargs.pop("smoke_selectors", ("SmokeSuite.*",))
        module_index = kwargs.pop(
            "module_index",
            {"foo": ("sakura_core/foo.cpp", "sakura_core/foo.hpp")},
        )
        return select_tests(
            changed_files=changed,
            coverage_map=self.map,
            inventory=self.inventory,
            repo_root=self.root,
            expected_base_sha=expected_base_sha,
            smoke_selectors=smoke_selectors,
            module_index=module_index,
            **kwargs,
        )

    def test_merges_cobertura_to_repo_relative_source_mapping(self):
        self.assertEqual({"sakura_core/foo.cpp": ["FooTest.*"]}, self.map["source_to_tests"])
        self.assertEqual("FooTest.*", self.map["fragments"][0]["selector"])
        self.assertEqual({"sakura_core/foo.cpp"}, parse_cobertura_fragment(self.xml, self.root, "FooTest.*"))

    def test_cache_key_contains_platform_base_sha_and_schema(self):
        self.assertEqual(
            f"tia-map-windows-x64-{BASE_SHA}-1",
            coverage_cache_key(BASE_SHA),
        )

    def test_canonical_cli_merges_and_selects_from_the_same_map(self):
        repository = Path(__file__).resolve().parents[3]
        inventory_path = self.root / "inventory.json"
        inventory_path.write_text(json.dumps(self.inventory), encoding="utf-8")
        binary = self.root / "tests1.exe"
        binary.write_bytes(b"fixture-binary")
        output_map = self.root / "coverage-map.json"
        command = [
            sys.executable,
            str(repository / "tools/build/sakura_build.py"),
            "--repo-root",
            str(repository),
            "--format",
            "json",
            "test",
            "coverage-map",
            "merge",
            "--base-sha",
            BASE_SHA,
            "--test-binary",
            str(binary),
            "--inventory",
            str(inventory_path),
            "--fragment",
            f"FooTest.*::{self.xml}",
            "--output",
            str(output_map),
        ]
        merged = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(0, merged.returncode, merged.stderr)
        self.assertEqual(True, json.loads(merged.stdout)["ok"])

        selected = subprocess.run(
            [
                sys.executable,
                str(repository / "tools/build/sakura_build.py"),
                "--repo-root",
                str(repository),
                "--format",
                "json",
                "test",
                "coverage-map",
                "select",
                "--map",
                str(output_map),
                "--inventory",
                str(inventory_path),
                "--base-sha",
                BASE_SHA,
                "--changed-file",
                "M::sakura_core/foo.cpp",
                "--smoke-selector",
                "SmokeSuite.*",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(0, selected.returncode, selected.stderr)
        selected_value = json.loads(selected.stdout)
        self.assertEqual("selected", selected_value["mode"])
        self.assertIn("FooTest.*", selected_value["gtest_filter"])

    def test_selects_impacted_suite_and_always_adds_smoke(self):
        result = self.decision((ChangedFile("sakura_core/foo.cpp"),))
        self.assertEqual("selected", result["mode"])
        self.assertFalse(result["full_fallback"])
        self.assertEqual(2, result["impacted_test_count"])
        self.assertIn("FooTest.*", result["gtest_filter"])
        self.assertIn("SmokeSuite.*", result["gtest_filter"])

    def test_module_header_change_expands_to_owned_covered_sources(self):
        result = self.decision((ChangedFile("sakura_core/foo.hpp"),))
        self.assertEqual("selected", result["mode"])
        self.assertEqual(["sakura_core/foo.cpp"], result["impacted_paths"])

    def test_documentation_only_change_runs_smoke_instead_of_zero_tests(self):
        result = self.decision((ChangedFile("README.md"),))
        self.assertEqual("smoke", result["mode"])
        self.assertEqual(0, result["impacted_test_count"])
        self.assertEqual(1, result["run_test_count"])
        self.assertEqual("SmokeSuite.*", result["gtest_filter"])

    def test_missing_map_falls_back_to_full(self):
        result = select_tests(
            changed_files=(ChangedFile("sakura_core/foo.cpp"),),
            coverage_map=None,
            inventory=self.inventory,
            repo_root=self.root,
            expected_base_sha=BASE_SHA,
            smoke_selectors=("SmokeSuite.*",),
        )
        self.assertTrue(result["full_fallback"])
        self.assertIn("coverage_map_missing", result["reason_codes"])

    def test_stale_base_sha_falls_back_to_full(self):
        result = self.decision((ChangedFile("sakura_core/foo.cpp"),), expected_base_sha="c" * 40)
        self.assertTrue(result["full_fallback"])
        self.assertIn("coverage_map_base_sha", result["reason_codes"])

    def test_missing_expected_base_sha_falls_back_to_full(self):
        result = self.decision((ChangedFile("sakura_core/foo.cpp"),), expected_base_sha=None)
        self.assertTrue(result["full_fallback"])
        self.assertIn("coverage_map_base_sha_missing", result["reason_codes"])

    def test_unmapped_production_file_falls_back_to_full(self):
        result = self.decision((ChangedFile("sakura_core/unknown.cpp"),))
        self.assertTrue(result["full_fallback"])
        self.assertIn("coverage_path_unmapped", result["reason_codes"])

    def test_build_topology_and_deletion_force_full(self):
        topology = self.decision((ChangedFile("src/main/modules/modules.json"),))
        deleted = self.decision((ChangedFile("sakura_core/foo.cpp", "D"),))
        self.assertTrue(topology["full_fallback"])
        self.assertIn("build_or_test_topology", topology["reason_codes"])
        self.assertTrue(deleted["full_fallback"])
        self.assertIn("deletion_or_rename", deleted["reason_codes"])

    def test_threshold_forces_full_before_smoke_is_added(self):
        result = self.decision((ChangedFile("sakura_core/foo.cpp"),), threshold=0.4)
        self.assertTrue(result["full_fallback"])
        self.assertIn("selected_test_threshold", result["reason_codes"])

    def test_invalid_smoke_selector_falls_back_to_full(self):
        result = self.decision((ChangedFile("sakura_core/foo.cpp"),), smoke_selectors=("Missing.*",))
        self.assertTrue(result["full_fallback"])
        self.assertIn("coverage_selector_unknown", result["reason_codes"])

    def test_excluding_the_smoke_selector_falls_back_to_full(self):
        result = self.decision(
            (ChangedFile("README.md"),),
            excluded_selectors=("SmokeSuite.*",),
        )
        self.assertTrue(result["full_fallback"])
        self.assertIn("smoke_selector_excluded", result["reason_codes"])

    def test_coverage_map_rejects_inventory_fingerprint_drift(self):
        changed = dict(self.map)
        changed["inventory_guarantee_fingerprint"] = "sha256:" + "c" * 64
        with self.assertRaisesRegex(CoverageMapError, "fingerprint"):
            validate_coverage_map(changed, self.inventory)

    def test_modules_json_builds_path_index(self):
        modules = self.root / "modules.json"
        modules.write_text(
            json.dumps(
                {
                    "components": [
                        {
                            "id": "foo",
                            "sources": ["sakura_core/foo.cpp"],
                            "public_headers": ["sakura_core/foo.hpp"],
                            "private_headers": [],
                            "public_include_roots": [],
                            "private_include_roots": [],
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        self.assertEqual(
            {"foo": ("sakura_core/foo.cpp", "sakura_core/foo.hpp")},
            load_module_index(modules, self.root),
        )


if __name__ == "__main__":
    unittest.main()
