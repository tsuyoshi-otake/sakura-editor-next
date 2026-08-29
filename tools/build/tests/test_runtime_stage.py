from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.model import Artifact, CompileProfiles, Component, Context, Edge, SemanticGraph, load_semantic_graph  # noqa: E402
from sakura_build_lib.runner import BuildError  # noqa: E402
from sakura_build_lib.runtime_stage import _stage_specification, observe_runtime_stage, stage_runtime_artifacts  # noqa: E402


def _write(path: Path, content: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(content, encoding="utf-8", newline="\n")


def _graph(
    root: Path,
    *,
    include_resource_edge: bool = True,
    destination_paths: tuple[str, str] | None = None,
) -> SemanticGraph:
    context = Context("msvc-x64-debug", "x64", "x64", "Debug", "msvc", "msbuild", "development", ())
    product = Component(
        "sakura_app",
        "app",
        "executable",
        "candidate",
        "legacy",
        (context.id,),
        "application",
        (),
        (),
        (),
        (),
        (),
        (),
        "application state",
        {"msbuild": ("sakura.vcxproj",)},
        None,
    )
    _write(root / "out/sakura.exe", b"product")
    _write(root / "out/sakura_lang_en_US.dll", b"language")
    if destination_paths is None:
        destination_paths = (
            "build/staging/debug/sakura.exe",
            "build/staging/debug/sakura_lang_en_US.dll",
        )
    product_destination, language_destination = destination_paths
    receipt_destination = "build/staging/debug/.sakura-runtime-stage.json"
    stage_document = {
        "schema_version": 1,
        "staging_sets": [
            {
                "id": "sakura-debug-stage",
                "context_id": context.id,
                "entries": [
                    {
                        "artifact_id": "sakura-debug-product",
                        "role": "product",
                        "source": "out/sakura.exe",
                        "destination": product_destination,
                    },
                    {
                        "artifact_id": "sakura-debug-language-en",
                        "role": "language-en",
                        "source": "out/sakura_lang_en_US.dll",
                        "destination": language_destination,
                    },
                ],
            }
        ],
    }
    _write(root / "runtime-stage.json", json.dumps(stage_document) + "\n")
    product_artifact = Artifact(
        "sakura-debug-product", "sakura_app", "product", (), ("out/sakura.exe",), "msbuild", True
    )
    language_artifact = Artifact(
        "sakura-debug-language-en", "sakura_app", "resource", (), ("out/sakura_lang_en_US.dll",), "rc", True
    )
    stage_artifact = Artifact(
        "sakura-debug-stage",
        "sakura_app",
        "staging_set",
        ("runtime-stage.json",),
        (
            product_destination,
            language_destination,
            receipt_destination,
        ),
        "copy",
        True,
    )
    edges = [
        Edge("app-stage", "sakura_app", stage_artifact.id, "asset", ("stage", "runtime"), "private", "none", None, True, True, ()),
        Edge("stage-product", stage_artifact.id, product_artifact.id, "asset", ("stage", "runtime"), "private", "none", None, True, True, ()),
    ]
    if include_resource_edge:
        edges.append(
            Edge("stage-language", stage_artifact.id, language_artifact.id, "asset", ("stage", "runtime"), "private", "none", None, True, True, ())
        )
    return SemanticGraph(
        root.resolve(),
        root / "src/main/modules/modules.json",
        3,
        "0.3.7",
        {context.id: context},
        {product.id: product},
        {},
        {
            product_artifact.id: product_artifact,
            language_artifact.id: language_artifact,
            stage_artifact.id: stage_artifact,
        },
        CompileProfiles(1, {}, {}, {}, {context.id: {"project_profile": "test", "link_profile": "test"}}),
        tuple(edges),
        "sha256:test-runtime-stage",
    )


class RuntimeStageTests(unittest.TestCase):
    def test_stage_reuses_exact_receipt_and_detects_content_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph = _graph(root)

            first = stage_runtime_artifacts(graph, "sakura_app", "msvc-x64-debug")
            self.assertEqual("staged", first["status"])
            self.assertEqual(2, first["staging_sets"][0]["copied_file_count"])
            self.assertTrue(observe_runtime_stage(graph, "sakura_app", "msvc-x64-debug")["valid"])

            second = stage_runtime_artifacts(graph, "sakura_app", "msvc-x64-debug")
            self.assertEqual("reused", second["status"])
            self.assertEqual(0, second["staging_sets"][0]["copied_file_count"])

            _write(root / "build/staging/debug/sakura.exe", b"drift")
            observed = observe_runtime_stage(graph, "sakura_app", "msvc-x64-debug")
            self.assertFalse(observed["valid"])
            self.assertIn("RUNTIME_STAGE_CONTENT_MISMATCH", {item["code"] for item in observed["failures"]})

    def test_stage_accepts_payloads_nested_below_receipt_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _graph(
                Path(temporary),
                destination_paths=(
                    "build/staging/debug/sakura.exe",
                    "build/staging/debug/terminal-tools/sakura_lang_en_US.dll",
                ),
            )

            staged = stage_runtime_artifacts(graph, "sakura_app", "msvc-x64-debug")

            self.assertEqual("staged", staged["status"])
            self.assertTrue(observe_runtime_stage(graph, "sakura_app", "msvc-x64-debug")["valid"])

    def test_stage_rejects_sibling_or_outside_receipt_directory(self) -> None:
        invalid_destinations = (
            ("sibling", "build/staging/other/sakura.exe"),
            ("outside", "build/other/sakura.exe"),
            ("receipt-parent", "build/staging/debug"),
            ("sibling-prefix", "build/staging/debug-evil/sakura.exe"),
        )
        for label, product_destination in invalid_destinations:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                graph = _graph(
                    Path(temporary),
                    destination_paths=(
                        product_destination,
                        "build/staging/debug/sakura_lang_en_US.dll",
                    ),
                )

                with self.assertRaises(BuildError) as raised:
                    stage_runtime_artifacts(graph, "sakura_app", "msvc-x64-debug")
                self.assertEqual("RUNTIME_STAGE_LAYOUT", raised.exception.code)

    def test_canonical_runtime_stage_accepts_terminal_tools_layout(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        graph = load_semantic_graph(repository_root)

        specifications = _stage_specification(graph, "sakura_app", "msvc-x64-debug")

        self.assertEqual(1, len(specifications))
        destinations = {str(entry["destination"]) for entry in specifications[0]["entries"]}
        self.assertIn("build/staging/msvc-x64-debug/sakura-editor/sakura.exe", destinations)
        self.assertIn(
            "build/staging/msvc-x64-debug/sakura-editor/terminal-tools/sakura-tmux.exe",
            destinations,
        )

    def test_stage_rejects_undeclared_provider_edge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _graph(Path(temporary), include_resource_edge=False)
            with self.assertRaises(BuildError) as raised:
                stage_runtime_artifacts(graph, "sakura_app", "msvc-x64-debug")
            self.assertEqual("RUNTIME_STAGE_EDGE", raised.exception.code)


if __name__ == "__main__":
    unittest.main()
