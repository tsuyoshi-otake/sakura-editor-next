"""Contracts for observable failures in the advisory MinGW job (#257).

The MinGW workflow is intentionally non-blocking, but that policy belongs to
the job.  A second ``continue-on-error`` on the unit-test step erases the
Release failure before the job can expose it: the historical marker step ran
only for Debug.  Keep the original failed step for both configurations, allow
bounded Debug-only diagnostics, and always retain cleanup ownership.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/build-on-msys2.yml"
CMAKE_LISTS = REPO_ROOT / "CMakeLists.txt"


def _workflow() -> str:
    return WORKFLOW.read_text(encoding="utf-8-sig")


def _job(text: str, name: str) -> str:
    jobs = text.index("\njobs:\n")
    body = text[jobs + 1 :]
    match = re.search(rf"(?m)^  {re.escape(name)}:\s*$", body)
    if match is None:
        raise AssertionError(f"workflow has no {name!r} job")
    following = re.search(r"(?m)^  [A-Za-z0-9_-]+:\s*$", body[match.end() :])
    end = match.end() + following.start() if following is not None else len(body)
    return body[match.end() : end]


def _step(job: str, name: str) -> str:
    match = re.search(rf"(?m)^      - name: {re.escape(name)}\s*$", job)
    if match is None:
        raise AssertionError(f"job has no {name!r} step")
    following = re.search(r"(?m)^      - (?:name:|uses:|run:)", job[match.end() :])
    end = match.end() + following.start() if following is not None else len(job)
    return job[match.start() : end]


class MinGWAdvisoryFailureContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _workflow()
        self.job = _job(self.text, "mingw")

    def test_only_the_job_is_advisory(self) -> None:
        header = self.job[: self.job.index("    steps:\n")]
        self.assertIn("    continue-on-error: true\n", header)

        unit_tests = _step(self.job, "Run unit tests")
        self.assertNotIn(
            "continue-on-error:",
            unit_tests,
            "a step-level advisory converts Release test failure into success",
        )
        self.assertIn("id: run_unit_tests", unit_tests)
        self.assertIn("timeout-minutes: 15", unit_tests)
        self.assertIn("MSYS2_PATH_TYPE: inherit", unit_tests)

        build = _step(self.job, "Build with MinGW-w64-gcc")
        self.assertNotIn(
            "MSYS2_PATH_TYPE: inherit",
            build,
            "the broad host PATH belongs only to tests that invoke host tools",
        )

    def test_debug_diagnostics_follow_failure_without_repairing_it(self) -> None:
        diagnostics = _step(self.job, "Try gdb backtrace on test failure")
        self.assertIn("if: failure() && env.Configuration == 'Debug'", diagnostics)
        self.assertIn("continue-on-error: true", diagnostics)
        self.assertIn("timeout-minutes: 5", diagnostics)

        self.assertNotIn(
            "Mark workflow failed when unit tests fail",
            self.job,
            "the original test step must own failure for both configurations",
        )
        self.assertNotIn("steps.run_unit_tests.outcome", self.job)

    def test_cleanup_owns_every_terminal_path(self) -> None:
        cleanup = _step(self.job, "Clean up test process survivors")
        self.assertIn("if: ${{ always() }}", cleanup)
        self.assertIn("Cleanup-TestProcessSurvivors.ps1", cleanup)

    def test_ctest_ends_a_hung_test_before_the_step_limit_does(self) -> None:
        # Only a test that ctest itself times out has its captured output, and
        # so the googletest line naming the hung test, printed (#338).
        unit_tests = _step(self.job, "Run unit tests")
        step = re.search(r"timeout-minutes: (\d+)", unit_tests)
        self.assertIsNotNone(step)
        step_seconds = int(step.group(1)) * 60

        cmake = CMAKE_LISTS.read_text(encoding="utf-8-sig")
        bound = re.search(
            r"get_property\((\w+) DIRECTORY PROPERTY TESTS\)\s*\n"
            r"set_tests_properties\(\$\{\1\} PROPERTIES TIMEOUT (\d+)\)",
            cmake,
        )
        self.assertIsNotNone(bound, "every hosted ctest test needs its own TIMEOUT")
        # The step also reconfigures and rebuilds before ctest starts.
        self.assertLessEqual(int(bound.group(2)), step_seconds - 120)

        # A name list missed the CoverageTest that the Debug leg registers only
        # with OpenCppCoverage installed; the directory's list cannot, as long
        # as no test is registered after it is read.
        registrations = [m.start() for m in re.finditer(r"(?m)^\s*add_test\(", cmake)]
        self.assertIn(
            "add_test(NAME CoverageTest", cmake, "the conditional test this guards"
        )
        self.assertLess(
            max(registrations), bound.start(), "an add_test after the bound escapes it"
        )


if __name__ == "__main__":
    unittest.main()
