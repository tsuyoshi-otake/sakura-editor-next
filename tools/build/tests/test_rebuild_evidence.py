from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.rebuild_evidence import (
    _RunResult,
    _phase_result,
    run_rebuild_closure_rehearsal,
)
from sakura_build_lib.runner import EventWriter


class RebuildEvidenceTests(unittest.TestCase):
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
                    "sakura_uri_tests",
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
