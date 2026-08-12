"""Contracts for the test-process cleanup steps in CI workflows (#121).

``tools/Cleanup-TestProcessSurvivors.ps1`` runs under ``always()`` so a failed
test run still cannot leave a survivor process behind.  That condition also
fires when the job never checked the repository out, and then the step's only
possible outcome is ``The term '.\\tools\\Cleanup-TestProcessSurvivors.ps1' is
not recognized`` -- a second, meaningless error written after the real one.

Issue #116's diagnosis hit exactly that.  ``build-sakura.yml``'s ``build`` job
validates its prerequisites *before* the checkout, so a prerequisite failure
produced a run whose failed-step log showed only the missing script, and the
actual cause had to be dug out by hand.

Both call sites in ``build-sakura.yml`` are therefore gated on their own job's
checkout having succeeded, with one condition shared between them so neither
can drift into its own dialect.  This file walks the workflows and proves the
property, rather than pinning hand-written line numbers that rot on the next
edit.  ``KNOWN_UNGATED_CALL_SITES`` records the two sites deliberately left
alone and why; a fifth site anywhere forces a decision instead of appearing
silently.

Like ``test_develop_issue_closure.py``, it lives in ``src/test/py`` because the
CTest ``pytest`` target runs from the repository root under pytest's default
``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"

CLEANUP_SCRIPT = "Cleanup-TestProcessSurvivors.ps1"
REQUIRED_CONDITION = "${{ always() && steps.checkout.outcome == 'success' }}"

GATED_CALL_SITES = {
    ("build-sakura.yml", "build"),
    ("build-sakura.yml", "test-exthost"),
}

# Both of these run their checkout as the very first step, so the useless
# error can only appear when the checkout itself failed and is red directly
# above it -- far less costly than the ``build`` job's case, where any of the
# pre-checkout prerequisite steps produces it.
#
# Gating them means editing two files whose semantic-inventory baseline
# findings are all load-bearing GoogleTest selector names: the negative
# selector environment variable, the planner's negative-selector command-line
# flag, and the selector argument passed to tests1.exe.  ``inventory semantic
# --strict`` requires every touched file to give up one of its own baseline
# findings, so those edits would demand renaming a live test-selection
# interface to satisfy a vocabulary rule.  That is real test-selection debt
# and not something a two-line robustness fix should trade against; pay it
# when those files change for a reason of their own.
KNOWN_UNGATED_CALL_SITES = {
    ("build-on-msys2.yml", "mingw"),
    ("coverage-map.yml", "collect"),
}

_JOBS_RE = re.compile(r"^jobs:[ \t]*$", re.MULTILINE)
_JOB_KEY_RE = re.compile(r"^  (?P<name>[A-Za-z0-9_-]+):[ \t]*$", re.MULTILINE)
_STEPS_RE = re.compile(r"^(?P<indent>[ ]*)steps:[ \t]*$", re.MULTILINE)
_IF_RE = re.compile(r"^[ ]*if:[ \t]*(?P<value>.*?)[ \t]*$", re.MULTILINE)


def _jobs(text: str) -> list[tuple[str, str]]:
    """Return ``(job name, job body)`` for every job in a workflow.

    Job names are the only two-space keys under ``jobs:``; job properties sit
    at four spaces and block scalars deeper still, so a plain scan is enough
    here and keeps the test free of a PyYAML dependency that CI's hash-locked
    ``requirements.txt`` does not carry.
    """
    header = _JOBS_RE.search(text)
    if header is None:
        return []

    body = text[header.end() :]
    keys = list(_JOB_KEY_RE.finditer(body))
    return [
        (
            key.group("name"),
            body[key.end() : (keys[index + 1].start() if index + 1 < len(keys) else len(body))],
        )
        for index, key in enumerate(keys)
    ]


def _steps(job_body: str) -> list[str]:
    """Return each step of a job as raw text."""
    header = _STEPS_RE.search(job_body)
    if header is None:
        return []

    body = job_body[header.end() :]
    first = re.search(r"^(?P<indent>[ ]*)- ", body, re.MULTILINE)
    if first is None:
        return []

    bounds = [
        match.start()
        for match in re.finditer(rf"^{first.group('indent')}- ", body, re.MULTILINE)
    ]
    bounds.append(len(body))
    return [body[bounds[i] : bounds[i + 1]] for i in range(len(bounds) - 1)]


def _condition(step: str) -> str | None:
    match = _IF_RE.search(step)
    return None if match is None else match.group("value")


class CleanupStepGatingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sites: list[tuple[str, str, str, str]] = []
        for workflow in sorted(WORKFLOWS.glob("*.y*ml")):
            text = workflow.read_text(encoding="utf-8-sig")
            if CLEANUP_SCRIPT not in text:
                continue
            for name, job_body in _jobs(text):
                for step in _steps(job_body):
                    if CLEANUP_SCRIPT in step:
                        self.sites.append((workflow.name, name, job_body, step))

    def test_every_call_site_is_accounted_for(self) -> None:
        found = {(workflow, job) for workflow, job, _, _ in self.sites}
        # Equality on both sides at once: a walker that silently matches
        # nothing fails, and so does a new call site that was never weighed
        # against the rule.
        self.assertEqual(GATED_CALL_SITES | KNOWN_UNGATED_CALL_SITES, found)

    def test_every_gated_call_site_requires_a_successful_checkout(self) -> None:
        for workflow, job, _, step in self.sites:
            if (workflow, job) not in GATED_CALL_SITES:
                continue
            with self.subTest(workflow=workflow, job=job):
                # A bare always() is the defect this contract exists to stop:
                # it lets a repository-less job append a missing-script error
                # on top of whatever actually failed.
                self.assertEqual(REQUIRED_CONDITION, _condition(step))

    def test_every_gated_job_identifies_exactly_one_checkout(self) -> None:
        for workflow, job, job_body, _ in self.sites:
            if (workflow, job) not in GATED_CALL_SITES:
                continue
            with self.subTest(workflow=workflow, job=job):
                identified = [step for step in _steps(job_body) if "id: checkout" in step]
                self.assertEqual(1, len(identified))
                # The referenced outcome has to be the repository checkout's,
                # not some other step that happens to carry the same id.
                self.assertIn("actions/checkout@", identified[0])


class BuildJobOrderingTests(unittest.TestCase):
    def test_the_build_job_validates_prerequisites_before_its_checkout(self) -> None:
        # The ordering that makes the gate necessary rather than decorative.
        # If this ever reverses, the cleanup condition stops being load-bearing
        # and the comment above it becomes wrong.
        text = (WORKFLOWS / "build-sakura.yml").read_text(encoding="utf-8-sig")
        jobs = dict((name, body) for name, body in _jobs(text))
        body = jobs["build"]
        validate = body.index("- name: Validate build prerequisites")
        checkout = body.index("- uses: actions/checkout@")
        self.assertLess(validate, checkout)


if __name__ == "__main__":
    unittest.main()
