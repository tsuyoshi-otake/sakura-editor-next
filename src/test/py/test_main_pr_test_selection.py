"""Contracts for the full main pull-request headless gate (#166).

The hosted exact-base coverage map was retired after its producer consumed
94.43 runner-minutes while the observed reuse population was at most two pull
requests per base. At 4.77 runner-minutes saved per consumer, break-even would
require 20 distinct consumers. The required native matrix therefore runs the
complete shared headless suite in both configurations and has no selection
cache, selector, or evidence-artifact path.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
COVERAGE_WORKFLOW = REPO_ROOT / ".github/workflows/coverage-map.yml"

RETIRED_VOCABULARY = (
    "SAKURA_TEST_SELECTION_ELIGIBLE",
    "TIA_SELECTION",
    ".coverage-map-cache",
    "coverage-map-input",
    "test-selection-",
    "Restore main coverage map",
    "Select main PR tests",
)


def _workflow_text() -> str:
    return BUILD_WORKFLOW.read_text(encoding="utf-8-sig")


def _named_step(text: str, name: str) -> str:
    boundaries = list(re.finditer(r"(?m)^    - name: ", text))
    matches = []
    for index, boundary in enumerate(boundaries):
        end = boundaries[index + 1].start() if index + 1 < len(boundaries) else len(text)
        step = text[boundary.start() : end]
        if step.startswith(f"    - name: {name}\n"):
            matches.append(step)
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one step named {name!r}, found {len(matches)}")
    return matches[0]


class FullHeadlessGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _workflow_text()

    def test_hosted_coverage_map_workflow_is_retired(self) -> None:
        self.assertFalse(
            COVERAGE_WORKFLOW.exists(),
            "restoring the producer requires a new measured break-even case",
        )
        for token in RETIRED_VOCABULARY:
            with self.subTest(token=token):
                self.assertNotIn(token, self.text)

    def test_each_native_configuration_reaches_the_same_full_ctest_step(self) -> None:
        self.assertRegex(
            self.text,
            r"(?m)^        config:\s*\n          - Debug\s*\n          - Release\s*$",
        )
        step = _named_step(self.text, "Run headless tests")
        self.assertIn("& ctest", step)
        self.assertIn("${{ matrix.config }}", step)
        self.assertNotIn("--tests-regex", step)
        self.assertNotIn("--exclude-regex", step)
        self.assertNotIn("GTEST_FILTER=", step)

    def test_shared_negative_filter_is_published_before_the_full_suite(self) -> None:
        publish = self.text.index("    - name: Set default headless test filter\n")
        execute = self.text.index("    - name: Run headless tests\n")
        self.assertLess(publish, execute)
        self.assertIn(
            "Get-Content src/test/headless-suite-selection.env >> $env:GITHUB_ENV",
            self.text[publish:execute],
        )


if __name__ == "__main__":
    unittest.main()
