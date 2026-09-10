"""The live GitHub gate must refuse a run that proved nothing.

``--live-github`` is opt-in, but once asked for it is a gate and not a report.
The failure that matters is the quiet one: a filter matching no case at all
still writes a well-formed JUnit report, and a gate that counts passes instead
of naming the cases it required reads that clean zero as a success. These cases
pin the refusal so the gate cannot regress into a reporter.

Like ``test_senp_github_models.py``, this lives in ``src/test/py`` because the
CTest ``pytest`` target never collects ``tools/build/tests``.
"""
import importlib.util
from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("senp_runtime_gate", ROOT / "tools/verify-senp-runtime.py")
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)


def report(*cases: str) -> ET.Element:
    """A JUnit report carrying exactly the given ``<testcase>`` fragments."""
    return ET.fromstring('<testsuites>' + "".join(cases) + '</testsuites>')


def case(name: str, classname: str = "SenpLiveGitHub", inner: str = "") -> str:
    return f'<testcase name="{name}" classname="{classname}">{inner}</testcase>'


BOTH = tuple(sorted(gate.LIVE_GITHUB_CASES))


class LiveGitHubGateTests(unittest.TestCase):
    def test_both_named_cases_passing_is_the_only_success(self):
        self.assertIsNone(gate.live_gate_failure(report(*(case(name) for name in BOTH))))

    def test_an_empty_report_is_refused_rather_than_read_as_zero_failures(self):
        refusal = gate.live_gate_failure(report())
        self.assertIsNotNone(refusal)
        for name in BOTH:
            self.assertIn(name, refusal)

    def test_a_report_of_only_other_suites_is_refused(self):
        # The live filter matched nothing, so what landed in the report is
        # whatever else the binary listed. Passes are still not this gate's.
        other = report(case("RealSamplePublishesTwoTreesAndStructuredDocument",
                            classname="SenpOwnerComposition"))
        self.assertIsNotNone(gate.live_gate_failure(other))

    def test_one_case_out_of_two_is_refused(self):
        for present, absent in ((BOTH[0], BOTH[1]), (BOTH[1], BOTH[0])):
            with self.subTest(present=present):
                refusal = gate.live_gate_failure(report(case(present)))
                self.assertIsNotNone(refusal)
                self.assertIn(absent, refusal)

    def test_a_skipped_or_failed_case_is_refused_although_present(self):
        for inner in ('<skipped message="gh is not signed in"/>',
                      '<failure message="the issue detail read never produced a document"/>'):
            with self.subTest(inner=inner):
                both = report(case(BOTH[0], inner=inner), case(BOTH[1]))
                refusal = gate.live_gate_failure(both)
                self.assertIsNotNone(refusal)
                self.assertIn(BOTH[0], refusal)


if __name__ == "__main__":
    unittest.main()
