from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib import repository_graduation_evidence as graduation  # noqa: E402
from sakura_build_lib.model import Artifact, CompileProfiles, Component, Context, Edge, SemanticGraph  # noqa: E402


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _graph(root: Path, *, fixture_edge: bool) -> SemanticGraph:
    context = Context("msvc-x64-debug", "x64", "x64", "Debug", "msvc", "msbuild", "development", ())
    product = Component(
        "sakura_app", "app", "executable", "candidate", "legacy", (context.id,), "application", (), (), (), (), (), (),
        "application state", {"msbuild": ("sakura.vcxproj",)}, None,
    )
    test = Component(
        "protocol_tests", "test", "test", "candidate", "generated", (context.id,), "tests", (), (), (), (), (), (),
        "test state", {"msbuild": ("protocol_tests.vcxproj",)}, None,
    )
    _write(root / "fixtures/protocol.json", '{"version":1}\n')
    fixture = Artifact("protocol-fixture", test.id, "test_fixture", ("fixtures/protocol.json",), (), None, True)
    edges: tuple[Edge, ...] = ()
    if fixture_edge:
        edges = (
            Edge("test-fixture", test.id, fixture.id, "test", ("test",), "private", "none", None, True, True, ()),
        )
    return SemanticGraph(
        root.resolve(),
        root / "src/main/modules/modules.json",
        3,
        "0.3.7",
        {context.id: context},
        {product.id: product, test.id: test},
        {},
        {fixture.id: fixture},
        CompileProfiles(1, {}, {}, {}, {context.id: {"project_profile": "test", "link_profile": "test"}}),
        edges,
        "sha256:test-graduation",
    )


class RepositoryGraduationEvidenceTests(unittest.TestCase):
    def test_fixture_requires_an_explicit_test_owner_edge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _graph(Path(temporary), fixture_edge=False)
            observation = graduation.collect_test_fixture_observation(graph, "msvc-x64-debug")
            self.assertFalse(observation["valid"])
            self.assertIn("GRADUATION_FIXTURE_EDGE_MISSING", {item["code"] for item in observation["failures"]})

    def test_malformed_runtime_record_fails_closed_without_attribute_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph = _graph(root, fixture_edge=True)
            evidence = {
                "schema_version": 1,
                "collection_ok": True,
                "semantic_graph_hash": graph.semantic_graph_hash,
                "product_id": "sakura_app",
                "context_id": "msvc-x64-debug",
                "native_evidence": {"path": "e/native.json", "hard_evidence_hash": "sha256:native"},
                "resource_evidence": {"path": "e/resource.json", "hard_evidence_hash": "sha256:resource"},
                "runtime_asset": [],
                "test_fixture": {},
                "state": {},
                "protocol": {},
                "test_inventory": {},
                "coverage": {"runtime_asset": True, "test_fixture": True, "state": True, "protocol": True},
            }
            stable = graduation._stable_payload(evidence)
            evidence["hard_evidence_hash"] = "sha256:" + hashlib.sha256(
                graduation._canonical_json(stable).encode("utf-8")
            ).hexdigest()
            path = root / "e/graduation.json"
            _write(path, json.dumps(evidence) + "\n")
            with patch.object(graduation, "validate_product_native_evidence", return_value={"valid": True, "hard_evidence_hash": "sha256:native"}), patch.object(
                graduation, "validate_resource_native_evidence", return_value={"valid": True, "hard_evidence_hash": "sha256:resource"}
            ), patch.object(
                graduation, "observe_runtime_stage", return_value={"valid": True, "hard_evidence_hash": "sha256:runtime"}
            ), patch.object(graduation, "_validate_recorded_fixture_observation", return_value=True), patch.object(
                graduation, "_validate_recorded_state_observation", return_value=True
            ), patch.object(graduation, "_validate_recorded_test_inventory", return_value=True), patch.object(
                graduation, "_validate_recorded_protocol", return_value=True
            ):
                observed = graduation.validate_repository_graduation_evidence(
                    graph,
                    path,
                    product_id="sakura_app",
                    context_id="msvc-x64-debug",
                )
            self.assertFalse(observed["valid"])
            self.assertIn("GRADUATION_EVIDENCE_RUNTIME_STAGE_STALE", {item["code"] for item in observed["failures"]})
            self.assertFalse(observed["coverage"]["runtime_asset"])
            self.assertTrue(observed["coverage"]["test_fixture"])


if __name__ == "__main__":
    unittest.main()
