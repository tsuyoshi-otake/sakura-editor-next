from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.rebuild_evidence import (
    _RunResult,
    _expected_actions,
    _environment_for_context,
    _mutation_applicability,
    _phase_result,
    run_rebuild_closure_rehearsal,
)
from sakura_build_lib.runner import EventWriter


class RebuildEvidenceTests(unittest.TestCase):
    def test_cmake_rebuild_environment_pins_english_showincludes_prefix(self):
        class Graph:
            @staticmethod
            def context(context_id):
                _ = context_id
                return SimpleNamespace(toolchain="msvc", backend="cmake")

            repo_root = Path(".")

        with patch(
            "sakura_build_lib.rebuild_evidence.msvc_environment",
            return_value={"VSLANG": "1041", "PATH": "compiler"},
        ):
            environment = _environment_for_context(Graph(), "cmake-msvc-x64-debug")
        self.assertEqual("1033", environment["VSLANG"])

    def test_mutation_applicability_does_not_invent_pch_resource_or_codegen_for_headless_leaf(self):
        class Graph:
            components = {
                "pilot_tests": SimpleNamespace(
                    id="pilot_tests",
                    build_definition="generated",
                    sources=("tests/pilot.cpp",),
                    public_headers=(),
                    private_headers=(),
                ),
                "provider": SimpleNamespace(
                    id="provider",
                    build_definition="generated",
                    sources=("provider/implementation.cpp",),
                    public_headers=("provider/public.h",),
                    private_headers=("provider/private.h",),
                ),
            }
            artifacts = {}

            @staticmethod
            def final_link_closure(root_ids, context_id):
                _ = (root_ids, context_id)
                return ("pilot_tests", "provider")

        applicability = _mutation_applicability(Graph(), "pilot_tests", "ctx")

        self.assertEqual("not_applicable", applicability["pch"]["status"])
        self.assertEqual("not_applicable", applicability["resource"]["status"])
        self.assertEqual("not_applicable", applicability["component_generated_input"]["status"])
        self.assertEqual("covered_by_staleness_gate", applicability["projection_input"]["status"])

    def test_mutation_applicability_finds_owned_resource_and_generated_inputs(self):
        class Graph:
            components = {
                "pilot_tests": SimpleNamespace(
                    id="pilot_tests",
                    build_definition="generated",
                    sources=("tests/pilot.cpp", "tests/pilot.rc"),
                    public_headers=(),
                    private_headers=(),
                ),
            }
            artifacts = {
                "generated-contract": SimpleNamespace(
                    id="generated-contract",
                    owner="pilot_tests",
                    artifact_kind="generated",
                    inputs=("tests/contract.in",),
                ),
            }

            @staticmethod
            def final_link_closure(root_ids, context_id):
                _ = (root_ids, context_id)
                return ("pilot_tests",)

        applicability = _mutation_applicability(Graph(), "pilot_tests", "ctx")

        self.assertEqual(["tests/pilot.rc"], applicability["resource"]["witnesses"])
        self.assertEqual(["tests/contract.in"], applicability["component_generated_input"]["witnesses"])

    def test_expected_actions_keep_private_mutations_inside_provider(self):
        class Graph:
            components = {
                "pilot_tests": SimpleNamespace(
                    kind="test",
                    sources=("tests/pilot.cpp",),
                    public_headers=(),
                    private_headers=(),
                ),
                "provider": SimpleNamespace(
                    kind="implementation",
                    sources=("provider/implementation.cpp",),
                    public_headers=("provider/public.h",),
                    private_headers=("provider/private.h",),
                ),
                "private_provider": SimpleNamespace(
                    kind="implementation",
                    sources=("private-provider/implementation.cpp",),
                    public_headers=(),
                    private_headers=(),
                ),
            }

            @staticmethod
            def final_link_closure(root_ids, context_id):
                _ = (root_ids, context_id)
                return ("pilot_tests", "provider", "private_provider")

        expected = _expected_actions(Graph(), "pilot_tests", "ctx")

        provider_compile = ["provider:provider/implementation.cpp"]
        self.assertEqual(expected["private_cpp"]["compile"], provider_compile)
        self.assertEqual(expected["private_cpp"]["archive"], ["provider"])
        self.assertEqual(expected["private_header"]["compile"], provider_compile)
        self.assertEqual(expected["private_header"]["archive"], ["provider"])
        self.assertEqual(expected["private_header"]["link"], ["pilot_tests"])
        self.assertEqual(expected["mutation_inputs"]["private_header"], ["provider/private.h"])

    def test_expected_actions_mark_private_header_mutation_not_applicable_when_absent(self):
        class Graph:
            components = {
                "pilot_tests": SimpleNamespace(
                    kind="test",
                    sources=("tests/pilot.cpp",),
                    public_headers=(),
                    private_headers=(),
                ),
                "provider": SimpleNamespace(
                    kind="implementation",
                    sources=("provider/implementation.cpp",),
                    public_headers=("provider/public.h",),
                    private_headers=(),
                ),
            }

            @staticmethod
            def final_link_closure(root_ids, context_id):
                _ = (root_ids, context_id)
                return ("pilot_tests", "provider")

        expected = _expected_actions(Graph(), "pilot_tests", "ctx")
        self.assertNotIn("private_header", expected)
        self.assertNotIn("private_header", expected["mutation_inputs"])

    def test_phase_rejects_unexpected_native_and_projection_work(self):
        expected = {"compile": [], "archive": [], "link": []}
        observed = {
            "compile": ["unrelated:unrelated.cpp"],
            "archive": [],
            "link": ["unrelated_tests"],
        }
        configure = _RunResult(
            ("cmake", "-S", "source", "-B", "build"),
            0,
            "",
            "",
            1.0,
        )

        report = _phase_result(
            "no_op",
            expected,
            observed,
            ["src/main/modules/generated/cmake/context.cmake"],
            [configure],
            test_exit_code=None,
        )

        self.assertFalse(report["ok"])
        self.assertEqual(report["explicit_configure_count"], 1)
        self.assertIn("unrelated:unrelated.cpp", report["unexpected"]["compile"])
        self.assertIn("unrelated_tests", report["unexpected"]["link"])
        self.assertTrue(any("generated projections changed" in failure for failure in report["failures"]))
        self.assertTrue(any("explicit configure count" in failure for failure in report["failures"]))

    def test_phase_rejects_package_restore_and_failed_component_test(self):
        expected = {"compile": [], "archive": [], "link": []}
        restore = _RunResult(
            ("msbuild", "pilot.vcxproj"),
            0,
            "VcpkgRestore invoked vcpkg.exe",
            "",
            1.0,
        )

        report = _phase_result(
            "private_cpp",
            expected,
            expected,
            [],
            [restore],
            test_exit_code=7,
        )

        self.assertFalse(report["ok"])
        self.assertTrue(report["package_restore_observed"])
        self.assertIn("package restore was observed", report["failures"])
        self.assertIn("component test exited 7", report["failures"])

    def test_native_setup_failure_preserves_typed_exit_and_cleans_workspace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            with patch(
                "sakura_build_lib.rebuild_evidence._copy_minimal_repository",
                side_effect=OSError("fixture copy failed"),
            ):
                report = run_rebuild_closure_rehearsal(
                    root,
                    "sakura_request_tests",
                    ("msvc-x64-debug",),
                    1,
                    1,
                    1,
                    EventWriter(),
                    workspace_root=workspace,
                )

            self.assertFalse(report["ok"])
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["failure_code"], "OSError")
            self.assertEqual(report["failure_exit_code"], 6)
            self.assertTrue(report["workspace_cleaned"])


if __name__ == "__main__":
    unittest.main()
