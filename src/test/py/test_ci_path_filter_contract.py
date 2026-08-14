"""Contracts for where CI path filters may and may not appear (#164).

Two rules pull in opposite directions, and each one breaks a different way when
it drifts.

A **required** workflow must carry no path filter at all.  A required status
check that never starts is not reported as skipped -- it stays ``expected``
forever, and the pull request can never merge.  The required ``pr-gate.yml``
therefore starts for every pull request and synchronously calls the reusable
build, analysis, documentation, architecture, and target-policy workflows.
Those callees also carry no event-level path filters; the parent planner makes
the one explicit documentation-only decision inside the job graph.

An **advisory** workflow is the opposite case.  Nothing waits on it, so a skip
costs nothing, and ``build-on-msys2.yml`` is the most expensive workflow in the
repository: its two MinGW legs were 38 of the roughly 74 runner-minutes a pull
request spent, and not one of those minutes was a required check.  It therefore
keeps path filters, and a pull request builds only the Release leg.

This file pins both halves so a future edit has to choose deliberately.  It
parses with regular expressions rather than PyYAML because CI installs
``requirements.txt`` with ``--require-hashes --no-build --no-deps``, which does
not carry a YAML parser, and because ``test_workflow_cleanup_gating.py`` already
established that a plain scan is enough for these files.

It lives in ``src/test/py`` for the same reason its neighbours do: the CTest
``pytest`` target runs from the repository root under pytest's default
``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"

# The one required parent and every reusable workflow whose conclusion it
# synchronously consumes. Adding another selected workflow means adding it here.
REQUIRED_CHECK_WORKFLOWS = (
    "pr-gate.yml",
    "build-sakura.yml",
    "cppcheck.yml",
    "doxygen.yml",
    "architecture-gates.yml",
    "pr-target-policy.yml",
)

ADVISORY_MINGW_WORKFLOW = "build-on-msys2.yml"

# Trees that cannot change what a MinGW build produces.  `docs/**` covers the
# TLA+ models, TLC configurations, and evidence JSON that live beside the
# Markdown there and would otherwise slip past a `**/*.md` filter.
REQUIRED_MINGW_IGNORES = ("'**/*.md'", "'docs/**'")

_TOP_LEVEL_KEY_RE = re.compile(r"^(?P<name>[A-Za-z_][A-Za-z0-9_-]*):", re.MULTILINE)
_PATH_FILTER_RE = re.compile(r"^[ ]+paths(-ignore)?:[ \t]*$", re.MULTILINE)
_TRIGGER_RE = re.compile(r"^  (?P<name>[a-z_]+):[ \t]*$", re.MULTILINE)


def _read(name: str) -> str:
    path = WORKFLOWS / name
    if not path.is_file():
        raise AssertionError(f"{name} is missing from {WORKFLOWS}")
    return path.read_text(encoding="utf-8")


def _trigger_block(text: str) -> str:
    """Return the body of the workflow's top-level ``on:`` key.

    ``on`` is a YAML 1.1 boolean, so a workflow may spell it ``on:`` or
    ``'on':``; both forms appear in the wild and the scan accepts either.
    """
    start = None
    for match in _TOP_LEVEL_KEY_RE.finditer(text):
        name = match.group("name")
        if start is None:
            if name == "on":
                start = match.end()
            continue
        return text[start : match.start()]
    if start is None:
        raise AssertionError("workflow has no top-level 'on:' key")
    return text[start:]


def _trigger_sections(block: str) -> dict[str, str]:
    """Split an ``on:`` block into ``{trigger name: body}``."""
    sections: dict[str, str] = {}
    matches = list(_TRIGGER_RE.finditer(block))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(block)
        sections[match.group("name")] = block[match.end() : end]
    return sections


class RequiredWorkflowsHaveNoPathFilter(unittest.TestCase):
    def test_no_required_workflow_filters_by_path(self) -> None:
        for name in REQUIRED_CHECK_WORKFLOWS:
            with self.subTest(workflow=name):
                block = _trigger_block(_read(name))
                found = _PATH_FILTER_RE.findall(block)
                self.assertEqual(
                    found,
                    [],
                    f"{name} owns a required status check, so a path filter "
                    "would leave that check 'expected' forever on any pull "
                    "request the filter excludes. Gate the expensive steps "
                    "instead, so the job still reports a conclusion.",
                )


class AdvisoryMinGWWorkflowFiltersByPath(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _read(ADVISORY_MINGW_WORKFLOW)
        self.sections = _trigger_sections(_trigger_block(self.text))

    def test_push_and_pull_request_both_filter(self) -> None:
        for trigger in ("push", "pull_request"):
            with self.subTest(trigger=trigger):
                body = self.sections.get(trigger)
                self.assertIsNotNone(
                    body, f"{ADVISORY_MINGW_WORKFLOW} lost its '{trigger}' trigger"
                )
                self.assertIn(
                    "paths-ignore:",
                    body,
                    f"{ADVISORY_MINGW_WORKFLOW} is advisory and is the most "
                    "expensive workflow here; nothing waits on it, so its "
                    f"'{trigger}' trigger must keep its path filter.",
                )
                for pattern in REQUIRED_MINGW_IGNORES:
                    self.assertIn(
                        pattern,
                        body,
                        f"{ADVISORY_MINGW_WORKFLOW}'s '{trigger}' filter must "
                        f"ignore {pattern}; a change confined to that tree "
                        "cannot alter a MinGW build.",
                    )

    def test_pull_requests_build_one_configuration(self) -> None:
        match = re.search(r"^\s*configuration:[ \t]*(?P<value>.+)$", self.text, re.MULTILINE)
        self.assertIsNotNone(
            match, f"{ADVISORY_MINGW_WORKFLOW} has no matrix 'configuration:' key"
        )
        value = match.group("value")
        self.assertIn(
            "github.event_name == 'pull_request'",
            value,
            "The MinGW matrix must narrow on pull requests; both legs cost 38 "
            "runner-minutes of advisory work per pull request.",
        )
        self.assertIn(
            "'[\"Release\"]'",
            value,
            "A pull request must build the Release leg only.",
        )
        self.assertIn(
            "'[\"Debug\", \"Release\"]'",
            value,
            "A trusted push to main and a manual dispatch must keep both "
            "configurations, so the advisory signal survives at the "
            "integration point.",
        )


if __name__ == "__main__":
    unittest.main()
