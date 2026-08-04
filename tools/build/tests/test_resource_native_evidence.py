from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.product_native_evidence import (  # noqa: E402
    collect_product_native_evidence,
    write_product_native_evidence,
)
from sakura_build_lib.repository_inventory import collect_repository_inventory  # noqa: E402
from sakura_build_lib.resource_native_evidence import (  # noqa: E402
    collect_resource_native_evidence,
    validate_resource_native_evidence,
    write_resource_native_evidence,
)
from sakura_build_lib.runner import BuildError  # noqa: E402
from test_product_native_evidence import _native_fixture  # noqa: E402


RESOURCE_ENTRIES = [
    {
        "type": {"kind": "name", "value": "CUSTOM"},
        "name": {"kind": "id", "value": 7},
        "language_id": 1041,
        "size": 4,
        "content_hash": "sha256:custom",
    },
    {
        "type": {"kind": "id", "value": 6},
        "name": {"kind": "id", "value": 1},
        "language_id": 0,
        "size": 8,
        "content_hash": "sha256:string-table",
    },
]


def _evidence_fixture(root: Path):
    graph, _tlog = _native_fixture(root)
    native = collect_product_native_evidence(
        graph,
        "product",
        "msvc-x64-debug",
        build_observed=True,
    )
    native_path = graph.repo_root / "build/evidence/native-product.json"
    write_product_native_evidence(native_path, native)
    return graph, native_path


class ResourceNativeEvidenceTests(unittest.TestCase):
    def test_collect_validate_and_merge_preserve_id_compatibility_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, native_path = _evidence_fixture(Path(temporary))
            observed_paths: list[Path] = []

            def table_reader(path: Path) -> list[dict[str, object]]:
                observed_paths.append(path)
                return list(RESOURCE_ENTRIES)

            evidence = collect_resource_native_evidence(
                graph,
                native_path,
                "product",
                "msvc-x64-debug",
                table_reader=table_reader,
            )
            evidence_path = graph.repo_root / "build/evidence/native-resources.json"
            write_resource_native_evidence(evidence_path, evidence)
            old_time = 1_700_000_000_000_000_000
            os.utime(evidence_path, ns=(old_time, old_time))
            write_resource_native_evidence(evidence_path, evidence)
            evidence_mtime = evidence_path.stat().st_mtime_ns

            validation = validate_resource_native_evidence(
                graph,
                evidence_path,
                native_path,
                "product",
                "msvc-x64-debug",
            )
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
                native_evidence_path=native_path,
                resource_evidence_path=evidence_path,
            )

        self.assertEqual([graph.repo_root / "x64/Debug/app.exe"], observed_paths)
        self.assertEqual(old_time, evidence_mtime)
        self.assertEqual(2, evidence["resource_table"]["entry_count"])
        self.assertEqual("id", evidence["resource_table"]["entries"][0]["type"]["kind"])
        self.assertTrue(validation["valid"])
        self.assertTrue(validation["coverage"]["resource_table_observed"])
        self.assertFalse(validation["coverage"]["resource_id_compatibility_observed"])
        self.assertTrue(inventory["resource_provenance"]["native_resource_table_observed"])
        codes = {item["code"] for item in inventory["findings"]}
        self.assertNotIn("NATIVE_RESOURCE_TABLE_UNOBSERVED", codes)
        self.assertIn("RESOURCE_ID_COMPATIBILITY_UNOBSERVED", codes)

    def test_tampered_table_breaks_content_and_hard_evidence_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, native_path = _evidence_fixture(Path(temporary))
            evidence = collect_resource_native_evidence(
                graph,
                native_path,
                "product",
                "msvc-x64-debug",
                table_reader=lambda _path: list(RESOURCE_ENTRIES),
            )
            evidence["resource_table"]["entries"][0]["content_hash"] = "sha256:tampered"
            evidence_path = graph.repo_root / "build/evidence/native-resources.json"
            evidence_path.parent.mkdir(parents=True, exist_ok=True)
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")

            validation = validate_resource_native_evidence(
                graph,
                evidence_path,
                native_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        codes = {item["code"] for item in validation["failures"]}
        self.assertIn("RESOURCE_TABLE_CONTENT_HASH_MISMATCH", codes)
        self.assertIn("RESOURCE_TABLE_EVIDENCE_HASH_MISMATCH", codes)

    def test_changed_product_invalidates_resource_and_native_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, native_path = _evidence_fixture(Path(temporary))
            evidence = collect_resource_native_evidence(
                graph,
                native_path,
                "product",
                "msvc-x64-debug",
                table_reader=lambda _path: list(RESOURCE_ENTRIES),
            )
            evidence_path = graph.repo_root / "build/evidence/native-resources.json"
            write_resource_native_evidence(evidence_path, evidence)
            (graph.repo_root / "x64/Debug/app.exe").write_bytes(b"changed-product")

            validation = validate_resource_native_evidence(
                graph,
                evidence_path,
                native_path,
                "product",
                "msvc-x64-debug",
            )
            with self.assertRaises(BuildError) as caught:
                collect_resource_native_evidence(
                    graph,
                    native_path,
                    "product",
                    "msvc-x64-debug",
                    table_reader=lambda _path: list(RESOURCE_ENTRIES),
                )

        self.assertFalse(validation["valid"])
        self.assertIn(
            "RESOURCE_TABLE_NATIVE_EVIDENCE_INVALID",
            {item["code"] for item in validation["failures"]},
        )
        self.assertEqual("RESOURCE_TABLE_NATIVE_EVIDENCE_INVALID", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)

    def test_hard_evidence_hash_is_checkout_independent(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            first_graph, first_native = _evidence_fixture(Path(first))
            second_graph, second_native = _evidence_fixture(Path(second))
            first_evidence = collect_resource_native_evidence(
                first_graph,
                first_native,
                "product",
                "msvc-x64-debug",
                table_reader=lambda _path: list(RESOURCE_ENTRIES),
            )
            second_evidence = collect_resource_native_evidence(
                second_graph,
                second_native,
                "product",
                "msvc-x64-debug",
                table_reader=lambda _path: list(RESOURCE_ENTRIES),
            )

        self.assertEqual(first_evidence["hard_evidence_hash"], second_evidence["hard_evidence_hash"])


if __name__ == "__main__":
    unittest.main()
