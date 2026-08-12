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
import re
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = REPO_ROOT / ".github/scripts"
WORKFLOW = REPO_ROOT / ".github/workflows/develop-issue-closure.yml"
PULL_REQUEST_TEMPLATE = REPO_ROOT / ".github/PULL_REQUEST_TEMPLATE.md"
PR_TARGET_POLICY_WORKFLOW = REPO_ROOT / ".github/workflows/pr-target-policy.yml"
SEMANTIC_INVENTORY = REPO_ROOT / "tools/build/sakura_build_lib/semantic_inventory.py"

if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from close_referenced_issues import (  # noqa: E402
    CLOSING_KEYWORDS,
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
