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
        step = text.split("- name: Verify Search lifecycle and intentional counterexamples", 1)[1]
        step = step.split("\n      - name:", 1)[0]
        self.assertIn("python3 tools/verify-search-lifecycle.py", step)
        self.assertIn(GATE.TOOL_URL, step)
        self.assertNotIn("if:", step)
        self.assertNotIn("continue-on-error", step)


if __name__ == "__main__":
    unittest.main()
