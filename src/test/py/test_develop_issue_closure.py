"""Parser behaviour and workflow contract for ``develop-issue-closure``.

These tests live here rather than beside the other workflow-contract tests in
``tools/build/tests`` on purpose.  The CTest ``pytest`` target runs from the
repository root with pytest's default ``norecursedirs``, which skips any
directory named ``build`` - so ``tools/build/tests`` is never collected by CI,
and a contract test placed there would silently never run.  ``src/test/py`` is
the Python suite CI actually executes.
"""

from __future__ import annotations

import ast
import contextlib
import io
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = REPO_ROOT / ".github/scripts"
CLOSING_SCRIPT = SCRIPTS / "close_referenced_issues.py"
WORKFLOW = REPO_ROOT / ".github/workflows/develop-issue-closure.yml"
PULL_REQUEST_TEMPLATE = REPO_ROOT / ".github/PULL_REQUEST_TEMPLATE.md"
PR_TARGET_POLICY_WORKFLOW = REPO_ROOT / ".github/workflows/pr-target-policy.yml"
SEMANTIC_INVENTORY = REPO_ROOT / "tools/build/sakura_build_lib/semantic_inventory.py"

if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from close_referenced_issues import (  # noqa: E402
    CLOSING_KEYWORDS,
    main,
    resolve_closing_references,
    strip_ignored_regions,
)


REPOSITORY = "tsuyoshi-otake/sakura-editor-next"
UPSTREAM = "sakura-editor/sakura"


def filter_rule_pattern():
    """Read the semantic ratchet's test-filtering pattern from its own source.

    The module is parsed rather than imported: importing it would pull the whole
    semantic inventory into this suite's coverage measurement and drop the run
    below the configured coverage gate.
    """

    source = SEMANTIC_INVENTORY.read_text(encoding="utf-8-sig")
    for node in ast.parse(source).body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == "_FILTER_RE" for target in node.targets):
            continue
        return node.value.args[0].value
    raise AssertionError("semantic_inventory.py no longer defines _FILTER_RE")


class ClosingReferenceResolutionTests(unittest.TestCase):
    def resolve(self, text, repository=REPOSITORY):
        return resolve_closing_references(text, repository)

    def test_every_documented_keyword_resolves_a_bare_reference(self) -> None:
        for keyword in CLOSING_KEYWORDS:
            with self.subTest(keyword=keyword):
                self.assertEqual(self.resolve(keyword + " #15").numbers, (15,))
                self.assertEqual(self.resolve(keyword.capitalize() + ": #15").numbers, (15,))

    def test_gh_prefixed_and_qualified_same_repository_references_resolve(self) -> None:
        self.assertEqual(self.resolve("Fixes GH-7").numbers, (7,))
        self.assertEqual(self.resolve("Closes " + REPOSITORY + "#15").numbers, (15,))
        self.assertEqual(self.resolve("Closes " + REPOSITORY.upper() + "#15").numbers, (15,))
        self.assertEqual(
            self.resolve("Resolves https://github.com/" + REPOSITORY + "/issues/15").numbers,
            (15,),
        )

    def test_references_outside_this_repository_are_reported_but_never_closed(self) -> None:
        qualified = self.resolve("Closes " + UPSTREAM + "#1234")
        self.assertEqual(qualified.numbers, ())
        self.assertEqual(qualified.foreign, ("Closes " + UPSTREAM + "#1234",))

        url = self.resolve("Fixes https://github.com/" + UPSTREAM + "/issues/1234")
        self.assertEqual(url.numbers, ())
        self.assertEqual(url.foreign, ("Fixes https://github.com/" + UPSTREAM + "/issues/1234",))

    def test_a_foreign_reference_does_not_suppress_a_local_one(self) -> None:
        references = self.resolve("Closes " + UPSTREAM + "#1234 and closes #15")
        self.assertEqual(references.numbers, (15,))
        self.assertEqual(len(references.foreign), 1)

    def test_numbers_are_deduplicated_and_sorted(self) -> None:
        self.assertEqual(self.resolve("Closes #9\nFixes #3\nresolved #9").numbers, (3, 9))

    def test_a_word_that_merely_contains_a_keyword_does_not_resolve(self) -> None:
        self.assertEqual(self.resolve("unclose #15").numbers, ())
        self.assertEqual(self.resolve("closing #15").numbers, ())
        self.assertEqual(self.resolve("non-fixed #15").numbers, ())

    def test_a_bare_reference_without_a_keyword_does_not_resolve(self) -> None:
        self.assertEqual(self.resolve("See #15 for the background.").numbers, ())

    def test_a_keyword_and_a_reference_on_separate_lines_do_not_resolve(self) -> None:
        self.assertEqual(self.resolve("This does not close\n#15 is unrelated.").numbers, ())

    def test_html_comments_and_code_regions_are_ignored(self) -> None:
        self.assertEqual(self.resolve("<!-- Closes #15 -->").numbers, ())
        self.assertEqual(self.resolve("```\nCloses #15\n```").numbers, ())
        self.assertEqual(self.resolve("~~~text\nCloses #15\n~~~").numbers, ())
        self.assertEqual(self.resolve("`Closes #15`").numbers, ())
        self.assertEqual(
            self.resolve("<!-- Closes #9 -->\nCloses #15\n```\nCloses #3\n```").numbers,
            (15,),
        )

    def test_strip_ignored_regions_keeps_ordinary_prose(self) -> None:
        self.assertIn("Closes #15", strip_ignored_regions("Closes #15\n\n```\nnoise\n```"))

    def test_an_empty_body_resolves_nothing(self) -> None:
        self.assertEqual(self.resolve("").numbers, ())
        self.assertEqual(self.resolve(None).numbers, ())

    def test_an_empty_repository_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            resolve_closing_references("Closes #15", "   ")

    def test_the_pull_request_template_itself_closes_nothing(self) -> None:
        template = PULL_REQUEST_TEMPLATE.read_text(encoding="utf-8-sig")
        references = resolve_closing_references(template, REPOSITORY)
        self.assertEqual(references.numbers, ())
        self.assertEqual(references.foreign, ())


class ClosingReferenceReportingTests(unittest.TestCase):
    """Closing nothing must leave a trace, not a green run and a bare "(none)".

    This is the workflow's only failure mode that is silent by construction:
    the job succeeds either way, so a reader cannot tell a pull request that
    deliberately closed no issue from one whose closing keyword was never
    written down.  PR #134 carried ``Fixes #137`` in a commit message while its
    body opened with ``Refs #112``; the run was green and #137 stayed open.
    """

    def invoke(self, title, body):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "step-output.txt"
            environment = {
                "REPOSITORY": REPOSITORY,
                "PR_TITLE": title,
                "PR_BODY": body,
                "GITHUB_OUTPUT": str(output),
            }
            # ``clear=True`` because a real GITHUB_OUTPUT in the ambient
            # environment would otherwise make this test append to the running
            # job's own step output.
            stream = io.StringIO()
            with mock.patch.dict(os.environ, environment, clear=True):
                with contextlib.redirect_stdout(stream):
                    status = main()
            return status, stream.getvalue(), output.read_text(encoding="utf-8")

    def test_resolving_nothing_reports_a_notice_naming_what_is_read(self) -> None:
        status, printed, emitted = self.invoke("Speed up the develop gate", "Refs #112")
        self.assertEqual(status, 0)
        self.assertIn("::notice::", printed)
        self.assertIn("no issue was closed", printed)
        # The trap is specifically that a commit message does not count, so the
        # notice has to say which text was actually read.
        self.assertIn("title or body", printed)
        self.assertIn("Commit messages are not read", printed)
        self.assertEqual(emitted, "numbers=\n")

    def test_resolving_a_reference_reports_the_numbers_without_the_notice(self) -> None:
        status, printed, emitted = self.invoke("Fix the restore path", "Fixes #137")
        self.assertEqual(status, 0)
        self.assertIn("Resolved closing references: 137", printed)
        self.assertNotIn("::notice::No closing keyword", printed)
        self.assertEqual(emitted, "numbers=137\n")

    def test_a_reference_outside_this_repository_still_closes_nothing(self) -> None:
        status, printed, emitted = self.invoke("Sync", "Fixes " + UPSTREAM + "#1")
        self.assertEqual(status, 0)
        self.assertIn("::notice::Ignored a closing reference outside", printed)
        self.assertIn("::notice::", printed)
        self.assertIn("no issue was closed", printed)
        self.assertEqual(emitted, "numbers=\n")

    def test_the_report_does_not_inflate_the_test_filtering_ratchet(self) -> None:
        # The script is a .github file too, so its own wording is counted by the
        # same ratchet the workflow's messages are held to.
        rule = re.compile(filter_rule_pattern(), re.IGNORECASE)
        self.assertTrue(rule.findall('print("::notice::skipping issue closure.")'))
        self.assertEqual(rule.findall(CLOSING_SCRIPT.read_text(encoding="utf-8-sig")), [])


class DevelopIssueClosureWorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8-sig")

    def test_the_workflow_only_acts_on_a_merged_develop_pull_request(self) -> None:
        self.assertIn("on:\n  pull_request:\n    types: [closed]\n", self.text)
        self.assertIn(
            "    if: ${{ github.event.pull_request.merged == true"
            " && github.event.pull_request.base.ref == 'develop' }}\n",
            self.text,
        )

    def test_issue_write_permission_is_scoped_to_the_closing_job(self) -> None:
        header, job = self.text.split("jobs:\n", 1)
        self.assertIn("permissions:\n  contents: read\n", header)
        self.assertNotIn("issues: write", header)
        self.assertIn("    permissions:\n      contents: read\n      issues: write\n", job)

    def test_read_only_token_events_are_skipped_rather_than_failed(self) -> None:
        self.assertIn('if [[ "$HEAD_REPOSITORY" != "$REPOSITORY" ]]; then', self.text)
        self.assertIn('if [[ "$ACTOR" == "dependabot[bot]" ]]; then', self.text)
        self.assertEqual(self.text.count('echo "eligible=false" >> "$GITHUB_OUTPUT"'), 2)
        self.assertIn('echo "eligible=true" >> "$GITHUB_OUTPUT"', self.text)

    def test_untrusted_pull_request_text_reaches_the_script_only_through_env(self) -> None:
        body = "${{ github.event.pull_request.body }}"
        title = "${{ github.event.pull_request.title }}"
        self.assertEqual(self.text.count(body), 1)
        self.assertEqual(self.text.count(title), 1)
        self.assertIn("          PR_BODY: " + body + "\n", self.text)
        self.assertIn("          PR_TITLE: " + title + "\n", self.text)

    def test_the_workflow_delegates_resolution_to_the_tested_script(self) -> None:
        self.assertIn("        run: python3 .github/scripts/close_referenced_issues.py\n", self.text)
        self.assertIn(
            "uses: actions/checkout@9c091bb21b7c1c1d1991bb908d89e4e9dddfe3e0 # v7.0.0\n",
            self.text,
        )
        self.assertIn("          persist-credentials: false\n", self.text)

    def test_closing_skips_pull_requests_and_already_closed_issues(self) -> None:
        self.assertIn("if .pull_request then \"pull\" else \"issue\" end", self.text)
        self.assertIn('if [[ "$state" != "open" ]]; then', self.text)
        self.assertIn('gh issue comment "$number" --repo "$REPOSITORY"', self.text)
        self.assertIn('gh issue close "$number" --repo "$REPOSITORY" --reason completed', self.text)

    def test_the_workflow_does_not_inflate_the_test_filtering_ratchet(self) -> None:
        # semantic_inventory counts filter/skip vocabulary in every .github file
        # as test-filtering debt.  This job never filters a test, so its status
        # messages must stay clear of that vocabulary; otherwise the ratchet
        # baseline would have to be raised and would stop guarding real test
        # exclusion.  Match against the real rule rather than a copy of it.
        rule = re.compile(filter_rule_pattern(), re.IGNORECASE)
        # Prove the rule is live before trusting an empty result from it.
        self.assertTrue(rule.findall('echo "::notice::skipping issue closure."'))
        self.assertEqual(rule.findall(self.text), [])

    def test_the_branch_policy_that_makes_this_workflow_necessary_still_holds(self) -> None:
        policy = PR_TARGET_POLICY_WORKFLOW.read_text(encoding="utf-8-sig")
        self.assertIn(
            "Feature, fix, and dependabot PRs must target develop;"
            " only develop or hotfix/* may target main.",
            policy,
        )


if __name__ == "__main__":
    unittest.main()
