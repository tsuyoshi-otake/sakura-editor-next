from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "build"))

from sakura_build_lib.component_evidence import (  # noqa: E402
    _declared_inputs,
    _declared_system_libraries,
    _expected_map_members,
    _hard_evidence_hash,
    _link_libraries,
    _missing_map_providers,
    collect_component_evidence,
)
from sakura_build_lib.model import load_semantic_graph  # noqa: E402


class ComponentEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.graph = load_semantic_graph(REPO_ROOT)

    def test_link_libraries_excludes_output_import_library(self) -> None:
        command = (
            'link.exe /OUT:"root.exe" /IMPLIB:"root.lib" '
            '"C:\\repo\\build\\sakura_uri.lib" kernel32.lib'
        )

        self.assertEqual(
            {"sakura_uri.lib", "kernel32.lib"},
            _link_libraries(command),
        )

    def test_declared_system_libraries_follow_final_link_closure(self) -> None:
        self.assertEqual(
            {"advapi32.lib"},
            _declared_system_libraries(self.graph, ("sakura_security", "sakura_security_tests")),
        )

    def test_win32_editor_frame_declares_its_native_window_library(self) -> None:
        """CMake leaf links must not rely on the legacy app's implicit user32 link."""

        self.assertEqual(
            {"user32.lib"},
            _declared_system_libraries(
                self.graph,
                ("sakura_editor_frame", "sakura_win32_editor_frame", "sakura_win32_editor_frame_tests"),
            ),
        )

    def test_win32_editor_frame_uses_editor_frame_as_a_compile_only_contract(self) -> None:
        """The adapter consumes value types from the public header, not its implementation archive."""

        self.assertEqual(
            (
                "sakura_editor_frame",
                "sakura_win32_editor_frame",
                "sakura_win32_editor_frame_tests",
            ),
            self.graph.closure(
                ("sakura_win32_editor_frame_tests",),
                "cmake-msvc-x64-debug",
                ("compile", "link", "test"),
            ),
        )
        self.assertEqual(
            (
                "sakura_win32_editor_frame",
                "sakura_win32_editor_frame_tests",
            ),
            self.graph.final_link_closure(
                ("sakura_win32_editor_frame_tests",),
                "cmake-msvc-x64-debug",
            ),
        )

    def test_expected_map_members_cover_msbuild_and_cmake_object_names(self) -> None:
        members = _expected_map_members(self.graph, {"sakura_uri.lib"})

        self.assertEqual(
            (
                "sakura_uri:uriidentity.cpp.obj",
                "sakura_uri:uriidentity.obj",
            ),
            members["sakura_uri.lib"],
        )

    def test_generated_abi_headers_are_declared_component_inputs(self) -> None:
        declared = _declared_inputs(
            self.graph,
            ("sakura_uri", "sakura_uri_tests"),
            "msvc-x64-debug",
        )

        self.assertIn(
            "src/main/modules/generated/abi/msvc-x64-debug/sakura_uri.h",
            declared,
        )
        self.assertIn(
            "src/main/modules/generated/abi/msvc-x64-debug/sakura_uri_tests.h",
            declared,
        )

    def test_compile_only_dependency_declares_its_public_header_not_its_unbuilt_source(self) -> None:
        declared = _declared_inputs(
            self.graph,
            (
                "sakura_editor_frame",
                "sakura_win32_editor_frame",
                "sakura_win32_editor_frame_tests",
            ),
            "cmake-msvc-x64-debug",
            compiled_component_ids=(
                "sakura_win32_editor_frame",
                "sakura_win32_editor_frame_tests",
            ),
        )

        self.assertIn("sakura_core/include/sakura/editor/EditorFrameEvents.h", declared)
        self.assertNotIn("sakura_core/workbench/editor/frame/EditorFrameEvents.cpp", declared)
        self.assertNotIn(
            "src/main/modules/generated/abi/cmake-msvc-x64-debug/sakura_editor_frame.h",
            declared,
        )

    def test_msbuild_evidence_scans_only_the_final_link_closure_for_compile_only_edges(self) -> None:
        """Old provider tlogs cannot leak through a public compile-only header edge."""

        context_id = "msvc-x64-debug"
        closure = self.graph.closure(
            ("sakura_win32_editor_frame_tests",),
            context_id,
            ("compile", "link", "test"),
        )
        final_link_closure = self.graph.final_link_closure(
            ("sakura_win32_editor_frame_tests",),
            context_id,
        )
        declared = _declared_inputs(
            self.graph,
            closure,
            context_id,
            compiled_component_ids=final_link_closure,
        )
        native_evidence = {
            "observed_repo_inputs": sorted(declared),
            "external_input_count": 0,
            "link_command": "sakura_win32_editor_frame.lib user32.lib",
            "map_path": "build/component.map",
            "map_text": "sakura_win32_editor_frame:win32editorframeadapter.obj",
            "build_command_text": "",
            "root_build_imports_suppressed": True,
            "package_metadata_mentions": [],
        }

        with patch(
            "sakura_build_lib.component_evidence._msbuild_evidence",
            return_value=native_evidence,
        ) as collect_native:
            result = collect_component_evidence(
                self.graph,
                "sakura_win32_editor_frame_tests",
                context_id,
            )

        self.assertTrue(result["ok"])
        collect_native.assert_called_once_with(
            self.graph,
            "sakura_win32_editor_frame_tests",
            context_id,
            final_link_closure,
        )

    def test_missing_map_provider_requires_an_exact_selected_archive_member(self) -> None:
        self.assertEqual(
            ["sakura_uri.lib"],
            _missing_map_providers({"sakura_uri.lib": []}),
        )
        self.assertEqual(
            [],
            _missing_map_providers({"sakura_uri.lib": ["sakura_uri:uriidentity.obj"]}),
        )

    def test_hard_evidence_hash_excludes_checkout_and_toolchain_paths(self) -> None:
        evidence = {
            "semantic_graph_hash": "sha256:graph",
            "component_id": "sakura_uri_tests",
            "context_id": "msvc-x64-debug",
            "backend": "msbuild",
            "closure": ["sakura_uri", "sakura_uri_tests"],
            "final_link_closure": ["sakura_uri", "sakura_uri_tests"],
            "declared_repo_inputs": ["sakura_core/platform/uri/UriIdentity.cpp"],
            "observed_repo_inputs": ["sakura_core/platform/uri/UriIdentity.cpp"],
            "expected_link_providers": ["sakura_uri.lib"],
            "declared_system_libraries": [],
            "implicit_system_libraries": ["kernel32.lib"],
            "observed_link_libraries": ["kernel32.lib", "sakura_uri.lib"],
            "expected_link_map_members": {"sakura_uri.lib": ["sakura_uri:uriidentity.obj"]},
            "observed_link_map_members": {"sakura_uri.lib": ["sakura_uri:uriidentity.obj"]},
            "package_restore_observed": False,
            "root_build_imports_suppressed": True,
            "failures": [],
            "link_map": "C:/first checkout/build/output.map",
            "external_input_count": 123,
        }
        first = _hard_evidence_hash(evidence)
        evidence["link_map"] = "D:/日本語 path/build/output.map"
        evidence["external_input_count"] = 999
        self.assertEqual(first, _hard_evidence_hash(evidence))
        evidence["observed_repo_inputs"] = []
        self.assertNotEqual(first, _hard_evidence_hash(evidence))


if __name__ == "__main__":
    unittest.main()
