"""Workflow contracts for one synchronous, fail-closed main PR gate."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"
PR_GATE = WORKFLOWS / "pr-gate.yml"
BUILD = WORKFLOWS / "build-sakura.yml"
CALLEES = (
    "architecture-gates.yml",
    "pr-target-policy.yml",
    "cppcheck.yml",
    "doxygen.yml",
    "build-sakura.yml",
)


def read_workflow(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def job(text: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n.*?(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow must contain the {name!r} job")
    return match.group(0)


def run_script(job_text: str) -> str:
    marker = "        run: |\n"
    remainder = job_text[job_text.index(marker) + len(marker) :]
    lines: list[str] = []
    for line in remainder.splitlines():
        if not line:
            lines.append("")
        elif line.startswith("          "):
            lines.append(line[10:])
        else:
            break
    return textwrap.dedent("\n".join(lines)) + "\n"


def execute_powershell(script_text: str, values: dict[str, str]) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as raw_directory:
        directory = Path(raw_directory)
        script = directory / "gate.ps1"
        summary = directory / "summary.md"
        script.write_text(script_text, encoding="utf-8")
        environment = {**os.environ, "GITHUB_STEP_SUMMARY": str(summary), **values}
        return subprocess.run(
            ("pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)),
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )


class PrGateTopologyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.parent = read_workflow(PR_GATE)
        self.build = read_workflow(BUILD)

    def test_only_the_parent_owns_the_pull_request_trigger(self) -> None:
        self.assertRegex(self.parent, r"(?m)^  pull_request:$")
        for name in CALLEES:
            with self.subTest(workflow=name):
                text = read_workflow(WORKFLOWS / name)
                self.assertRegex(text, r"(?m)^  workflow_call:$")
                self.assertNotRegex(text, r"(?m)^  pull_request:$")

    def test_parent_calls_every_required_workflow_in_one_dag(self) -> None:
        for name in CALLEES:
            with self.subTest(workflow=name):
                self.assertIn(f"uses: ./.github/workflows/{name}", self.parent)
        gate = job(self.parent, "gate")
        self.assertIn(
            "needs: [ci-plan, target-policy, architecture, cppcheck, doxygen, native]",
            gate,
        )
        self.assertIn("if: ${{ always() }}", gate)
        self.assertIn("name: PR Gate", gate)

    def test_planner_uses_the_exact_pull_request_diff_and_uploads_evidence(self) -> None:
        plan = job(self.parent, "ci-plan")
        self.assertIn("fetch-depth: 0", plan)
        self.assertIn("github.event.pull_request.base.sha", plan)
        self.assertIn("github.event.pull_request.head.sha", plan)
        self.assertIn("ci plan", plan)
        self.assertIn("run-native-build", plan)
        self.assertIn("Upload CI plan evidence", plan)

    def test_one_plan_controls_cppcheck_and_native_work(self) -> None:
        cppcheck = job(self.parent, "cppcheck")
        self.assertIn("needs.ci-plan.outputs.run-native-build == 'true'", cppcheck)
        native = job(self.parent, "native")
        self.assertIn("ci_plan_provided: true", native)
        self.assertIn("ci_mode: ${{ needs.ci-plan.outputs.mode }}", native)
        self.assertIn(
            "run_native_build: ${{ needs.ci-plan.outputs.run-native-build == 'true' }}",
            native,
        )

    def test_main_push_and_release_call_default_to_full_native_work(self) -> None:
        self.assertRegex(self.build, r"(?m)^  push:$")
        self.assertIn("ci_plan_provided:", self.build)
        self.assertIn("default: false", self.build)
        self.assertEqual(
            self.build.count("github.event_name != 'pull_request' || inputs.run_native_build"),
            4,
            "Rust audit, MSVC vcpkg, advisory MinGW vcpkg, and MSBuild must share the caller decision",
        )


class PrGateTerminalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.parent_script = run_script(job(read_workflow(PR_GATE), "gate"))
        self.native_script = run_script(job(read_workflow(BUILD), "pr-gate"))

    def run_parent(self, **values: str) -> subprocess.CompletedProcess[str]:
        environment = {
            "PLAN_RESULT": "success",
            "RUN_NATIVE_BUILD": "true",
            "TARGET_RESULT": "success",
            "ARCHITECTURE_RESULT": "success",
            "CPPCHECK_RESULT": "success",
            "DOXYGEN_RESULT": "success",
            "NATIVE_RESULT": "success",
            **values,
        }
        return execute_powershell(self.parent_script, environment)

    def run_native(self, **values: str) -> subprocess.CompletedProcess[str]:
        environment = {
            "CI_PLAN_PROVIDED": "true",
            "PLAN_MODE": "full_native",
            "RUN_NATIVE_BUILD": "true",
            "ENCODING_RESULT": "success",
            "AUDIT_RESULT": "success",
            "VCPKG_RESULT": "success",
            "BUILD_RESULT": "success",
            **values,
        }
        return execute_powershell(self.native_script, environment)

    def test_outer_gate_accepts_full_and_documentation_terminals(self) -> None:
        full = self.run_parent()
        self.assertEqual(full.returncode, 0, full.stderr)
        docs = self.run_parent(RUN_NATIVE_BUILD="false", CPPCHECK_RESULT="skipped")
        self.assertEqual(docs.returncode, 0, docs.stderr)

    def test_outer_gate_rejects_any_selected_failure_or_invalid_omission(self) -> None:
        for variable in (
            "PLAN_RESULT",
            "TARGET_RESULT",
            "ARCHITECTURE_RESULT",
            "DOXYGEN_RESULT",
            "NATIVE_RESULT",
        ):
            with self.subTest(variable=variable):
                self.assertNotEqual(self.run_parent(**{variable: "failure"}).returncode, 0)
        self.assertNotEqual(self.run_parent(CPPCHECK_RESULT="skipped").returncode, 0)
        self.assertNotEqual(
            self.run_parent(RUN_NATIVE_BUILD="false", CPPCHECK_RESULT="success").returncode,
            0,
        )

    def test_native_gate_accepts_both_supported_success_terminals(self) -> None:
        self.assertEqual(self.run_native().returncode, 0)
        docs = self.run_native(
            PLAN_MODE="docs_only",
            RUN_NATIVE_BUILD="false",
            AUDIT_RESULT="skipped",
            VCPKG_RESULT="skipped",
            BUILD_RESULT="skipped",
        )
        self.assertEqual(docs.returncode, 0, docs.stderr)

    def test_native_gate_rejects_missing_plan_or_selected_work(self) -> None:
        self.assertNotEqual(self.run_native(CI_PLAN_PROVIDED="false").returncode, 0)
        selected_but_absent = self.run_native(BUILD_RESULT="skipped")
        self.assertNotEqual(selected_but_absent.returncode, 0)
        self.assertIn("Selected MSBuild matrix did not succeed", selected_but_absent.stderr)
        audit_absent = self.run_native(AUDIT_RESULT="skipped")
        self.assertNotEqual(audit_absent.returncode, 0)
        self.assertIn("Selected Rust dependency audit did not succeed", audit_absent.stderr)
        unsupported = self.run_native(PLAN_MODE="full_native", RUN_NATIVE_BUILD="false")
        self.assertNotEqual(unsupported.returncode, 0)
        self.assertIn("unsupported terminal state", unsupported.stderr)


if __name__ == "__main__":
    unittest.main()
