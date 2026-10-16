"""Do not accept a broken tool or an unrelated counterexample as a model result."""
import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[2] / "verify-search-lifecycle.py"
SPEC = importlib.util.spec_from_file_location("search_lifecycle_gate", PATH)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class SearchLifecycleGateTest(unittest.TestCase):
    def test_positive_requires_success_and_exhaustion(self):
        success = "Model checking completed. No error has been found.\n0 states left on queue."
        self.assertTrue(GATE.accepted_result(0, success, False))
        for code, text in [(1, success), (0, ""), (0, success.replace("0 states", "3 states")),
                           (12, "Error: Invariant CurrentResults is violated.")]:
            self.assertFalse(GATE.accepted_result(code, text, False))

    def test_negative_requires_the_intended_invariant_and_exit_code(self):
        counterexample = "Error: Invariant CurrentResults is violated."
        self.assertTrue(GATE.accepted_result(12, counterexample, True))
        for code, text in [(0, counterexample), (1, counterexample), (12, "Parse error"),
                           (12, "Error: Invariant Unrelated is violated."), (None, "TimeoutExpired")]:
            self.assertFalse(GATE.accepted_result(code, text, True))

    def test_architecture_job_runs_gate_without_a_path_condition(self):
        workflow = PATH.parent.parent / ".github/workflows/architecture-gates.yml"
        text = workflow.read_text(encoding="utf-8")

        def step(name):
            body = text.split(f"- name: {name}\n", 1)[1]
            return body.split("\n      - name:", 1)[0]

        # The pinned jar is fetched once and shared with the SENP gate (#296).
        download = step("Download pinned TLC tool")
        gate = step("Verify Search lifecycle and intentional counterexamples")
        self.assertLess(text.index("- name: Download pinned TLC tool"),
                        text.index("- name: Verify Search lifecycle and intentional counterexamples"))
        self.assertIn(GATE.TOOL_URL, download)
        self.assertIn('--output "$RUNNER_TEMP/tla2tools.jar"', download)
        self.assertIn("python3 tools/verify-search-lifecycle.py", gate)
        self.assertIn('--jar "$RUNNER_TEMP/tla2tools.jar"', gate)
        for body in (download, gate):
            self.assertNotIn("if:", body)
            self.assertNotIn("continue-on-error", body)


if __name__ == "__main__":
    unittest.main()
