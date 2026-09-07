"""The model gate must reject unrelated TLC failures and incomplete searches."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("senp_model_gate", ROOT / "tools/verify-senp-github-models.py")
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)

GOOD = """Finished checking temporal properties in 00s.
Model checking completed. No error has been found.
42 states generated, 20 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 8.
"""
BAD = """Error: Invariant NoStaleConnection is violated.
The behavior up to this point is:
State 1: <Initial predicate>
"""


class SenpGithubModelGateTests(unittest.TestCase):
    def test_complete_safety_and_liveness_required(self):
        self.assertTrue(gate.accepted_result(0, GOOD, None))
        for code, output in ((1, GOOD), (None, GOOD), (0, GOOD.replace("0 states left", "1 states left")),
                             (0, GOOD.replace("20 distinct", "0 distinct")),
                             (0, GOOD.replace("Finished checking temporal properties", "No properties")),
                             (0, "Model checking completed. No error has been found.")):
            with self.subTest(code=code, output=output):
                self.assertFalse(gate.accepted_result(code, output, None))

    def test_counterexample_requires_exact_invariant_exit_and_trace(self):
        self.assertTrue(gate.accepted_result(12, BAD, "NoStaleConnection"))
        for code, output in ((0, BAD), (1, BAD), (12, BAD.replace("NoStaleConnection", "TypeOK")),
                             (12, BAD.replace("State 1:", "missing trace")), (12, "Syntax error")):
            with self.subTest(code=code, output=output):
                self.assertFalse(gate.accepted_result(code, output, "NoStaleConnection"))

    def test_timeout_kills_and_reaps_child(self):
        with tempfile.TemporaryDirectory(prefix="senp-tlc-gate-") as temporary:
            code, _, lifecycle = gate.run_case(
                [sys.executable, "-c", "import time; time.sleep(30)"], Path(temporary), .05)
        self.assertIsNone(code)
        self.assertTrue(lifecycle["timed_out"])
        self.assertTrue(lifecycle["exited"])

    def test_temporal_counterexample_requires_property_config_and_cycle(self):
        output = ("Error: Temporal properties were violated.\n"
                  "The following behavior constitutes a counter-example:\n"
                  "State 1: <Initial predicate>\nState 2: Stuttering\n")
        self.assertTrue(gate.accepted_result(13, output, None, "EveryAcceptedTerminates"))
        for code, log in ((12, output), (None, output), (13, output.replace("Stuttering", "stopped")),
                          (13, BAD), (13, "Syntax error")):
            self.assertFalse(gate.accepted_result(code, log, None, "EveryAcceptedTerminates"))
        self.assertTrue(gate.sole_temporal_property("PROPERTIES EveryAcceptedTerminates\n",
                                                   "EveryAcceptedTerminates"))
        for config in ("PROPERTIES Other\n", "PROPERTIES EveryAcceptedTerminates Other\n",
                       "PROPERTIES EveryAcceptedTerminates\nPROPERTIES Other\n",
                       "PROPERTIES EveryAcceptedTerminates\n Other\nCHECK_DEADLOCK FALSE\n",
                       " PROPERTY Other\nPROPERTIES EveryAcceptedTerminates\n", ""):
            self.assertFalse(gate.sole_temporal_property(config, "EveryAcceptedTerminates"))

    def test_missing_tool_is_terminal(self):
        code, log, lifecycle = gate.run_case([str(ROOT / "no-such-java.exe")], ROOT, 1)
        self.assertIsNone(code)
        self.assertTrue(lifecycle["exited"])
        self.assertIn("TLC_LAUNCH_FAILED", log)

    def test_wrong_jar_rejected_before_any_process(self):
        with tempfile.TemporaryDirectory(prefix="senp-tlc-hash-") as temporary:
            jar = Path(temporary) / "wrong.jar"
            jar.write_bytes(b"not the pinned tool")
            with mock.patch.object(sys, "argv", ["gate", "--jar", str(jar), "--output", temporary]), \
                    mock.patch.object(gate.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(SystemExit, "HASH_MISMATCH"):
                    gate.main()
                launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
