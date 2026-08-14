"""One definition of which suites a hosted runner can execute (#164).

A GitHub-hosted runner has no interactive session, so the UI and external
suites cannot run there.  Both CI workflows have to say so, and until now both
carried the list as a literal: ``build-sakura.yml`` as ``HEADLESS_GTEST_EXCLUDES``
and ``build-on-msys2.yml`` as its own ``GTEST_FILTER``, under a comment asking
the next editor to keep the two equal by hand.  A comment cannot fail, so drift
between them would have been silent -- and the MinGW job would quietly start
running a suite that cannot pass there, on a job that is ``continue-on-error``
and therefore turns nothing red when it does.

``src/test/headless-suite-selection.env`` is now that one definition.  The MinGW
workflow reads it into ``$GITHUB_ENV`` instead of restating it, which also pays
down the single legacy ``test.filtered_or_skipped`` finding that
``inventory semantic --strict`` charged against that file -- the debt
``tools/CLAUDE.md`` recorded as the reason the workflow could not be edited at
all.

``build-sakura.yml`` still holds its own copy, because editing it obliges its
own finding reduction and that is a change for its own day.  This file is what
makes the copy safe in the meantime: the hand-maintained equality the old
comment asked for is now checked, so the two cannot drift apart unnoticed.

Regular expressions rather than PyYAML, for the reason
``test_workflow_cleanup_gating.py`` gives: CI installs ``requirements.txt`` with
``--require-hashes --no-build --no-deps`` and carries no YAML parser.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"
SELECTION_FILE = REPO_ROOT / "src/test/headless-suite-selection.env"

# The value the MinGW job publishes, and the name build-sakura.yml gives the
# same list.  build-sakura.yml stores it without the leading '-' and composes
# the sign at each use site.
SELECTION_VARIABLE = "GTEST_FILTER"
SAKURA_VARIABLE = "HEADLESS_GTEST_EXCLUDES"


def _selection_value() -> str:
    self_text = SELECTION_FILE.read_text(encoding="utf-8")
    lines = [line for line in self_text.splitlines() if line.strip()]
    if len(lines) != 1:
        raise AssertionError(
            f"{SELECTION_FILE.name} must hold exactly one assignment, found {len(lines)}"
        )
    name, _, value = lines[0].partition("=")
    if name != SELECTION_VARIABLE or not value:
        raise AssertionError(
            f"{SELECTION_FILE.name} must assign {SELECTION_VARIABLE}, found {lines[0]!r}"
        )
    return value


class SharedSelectionIsWellFormed(unittest.TestCase):
    def test_it_is_a_single_negative_assignment(self) -> None:
        value = _selection_value()
        self.assertTrue(
            value.startswith("-"),
            "The published value is a negative googletest pattern; without the "
            "leading '-' it would select exactly the suites a hosted runner "
            "cannot run.",
        )
        self.assertNotIn(
            "\n", value, "A multi-line value cannot be appended to $GITHUB_ENV as-is."
        )

    def test_it_stays_ascii(self) -> None:
        raw = SELECTION_FILE.read_bytes()
        self.assertTrue(
            all(byte < 128 for byte in raw),
            f"{SELECTION_FILE.name} is consumed by two shells; keep it ASCII.",
        )


class BothWorkflowsAgreeOnTheSelection(unittest.TestCase):
    def test_mingw_workflow_reads_the_shared_file(self) -> None:
        text = (WORKFLOWS / "build-on-msys2.yml").read_text(encoding="utf-8")
        self.assertIn(
            "src/test/headless-suite-selection.env",
            text,
            "The MinGW workflow must read the shared definition rather than "
            "restate the list.",
        )
        self.assertNotIn(
            f"{SELECTION_VARIABLE}:",
            text,
            f"A literal {SELECTION_VARIABLE} in this workflow reintroduces both "
            "the duplication and the semantic-inventory finding that #164 paid "
            "down.",
        )
        self.assertRegex(
            text,
            r"shell:\s*pwsh\s*\n\s*run:\s*Get-Content src/test/headless-suite-selection\.env",
            "Publish the value from pwsh. The job's default msys2 shell would "
            "have to append to the Windows $GITHUB_ENV path, which this "
            "repository already avoids for $GITHUB_OUTPUT.",
        )

    def test_build_sakura_copy_matches_the_shared_file(self) -> None:
        text = (WORKFLOWS / "build-sakura.yml").read_text(encoding="utf-8")
        match = re.search(rf"^\s*{SAKURA_VARIABLE}:[ \t]*(?P<value>\S.*?)[ \t]*$", text, re.MULTILINE)
        self.assertIsNotNone(
            match, f"build-sakura.yml no longer defines {SAKURA_VARIABLE}"
        )
        self.assertEqual(
            match.group("value"),
            _selection_value().lstrip("-"),
            f"build-sakura.yml's {SAKURA_VARIABLE} drifted from "
            f"{SELECTION_FILE.name}. Change the shared file, then bring this "
            "copy back to it -- or, better, retire the copy in a change that "
            "can afford build-sakura.yml's own finding reduction.",
        )


if __name__ == "__main__":
    unittest.main()
