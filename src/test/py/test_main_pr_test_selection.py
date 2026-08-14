"""Contracts for the main pull-request test selection gate (#112 Phase 3).

Coverage-map test selection used to be restricted to `feature/*` heads.  A
branch name never made a selection sound -- it only made the unsound cases
rarer -- so the restriction was replaced by the one thing that does: the
classifier in ``coverage_map.py``, which returns a full-suite decision for
every change class it cannot reason about, plus a workflow step that falls
back to the full filter on any error at all.

Widening the gate therefore moves load onto that classifier, and both halves
of the safety argument are pinned here:

* The workflow half.  All three steps read one job-level
  ``SAKURA_TEST_SELECTION_ELIGIBLE`` expression, so a fourth step or an edited
  condition cannot silently disagree with the other three, and promotion heads
  stay excluded.
* The classifier half.  ``select_tests`` must keep returning ``full`` for each
  fail-closed reason code.  ``tools/build/tests/test_coverage_map.py`` already
  asserts some of this, but pytest's default ``norecursedirs`` skips any
  directory named ``build``, so nothing under ``tools/build/tests`` is ever
  collected by the CTest ``pytest`` target or by CI.  Until that changes, the
  assertions below are the only ones CI actually runs against the classifier
  that now carries the whole gate.
"""

from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
TOOLS_BUILD = REPO_ROOT / "tools/build"

if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.coverage_map import (  # noqa: E402
    ChangedFile,
    build_coverage_map,
    select_tests,
)
from sakura_build_lib.test_inventory import guarantee_fingerprint  # noqa: E402


ELIGIBILITY = "SAKURA_TEST_SELECTION_ELIGIBLE"
GATE = f"env.{ELIGIBILITY} == 'true'"
GATED_STEPS = (
    "Restore main coverage map",
    "Select main PR tests",
    "Upload main test-selection evidence",
)

STEP_BOUNDARY_RE = re.compile(r"(?m)^(?=[ \t]*-[ \t]+(?:name|uses|if):)")

BASE_SHA = "a" * 40
BINARY_SHA = "b" * 64


def _workflow_text() -> str:
    return BUILD_WORKFLOW.read_text(encoding="utf-8-sig")


def _inventory() -> dict:
    tests = [
        {
            "test_id": "tests1:SmokeSuite.AlwaysPass",
            "runtime": {"runner_id": "tests1", "selector": "SmokeSuite.AlwaysPass"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:FooTest.CoversSource",
            "runtime": {"runner_id": "tests1", "selector": "FooTest.CoversSource"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:BarTest.Unrelated",
            "runtime": {"runner_id": "tests1", "selector": "BarTest.Unrelated"},
            "status": "enabled",
        },
        {
            "test_id": "tests1:BazTest.Unrelated",
            "runtime": {"runner_id": "tests1", "selector": "BazTest.Unrelated"},
            "status": "enabled",
        },
    ]
    return {
        "schema_version": 1,
        "inventory_id": "main-pr-gate-fixture",
        "source_revision": BASE_SHA,
        "source_dirty": False,
        "discovery": {
            "framework": "googletest",
            "executable": "x64/Debug/tests1.exe",
            "arguments": ["--gtest_list_tests"],
            "executable_sha256": BINARY_SHA,
        },
        "test_count": len(tests),
        "disabled_count": 0,
        "guarantee_fingerprint": guarantee_fingerprint(tests),
        "tests": tests,
    }


class EligibilityGateTests(unittest.TestCase):
    """The workflow half: one expression, read identically by every gate."""

    def setUp(self) -> None:
        self.text = _workflow_text()
        self.steps = STEP_BOUNDARY_RE.split(self.text)

    def _step(self, name: str) -> str:
        matches = [step for step in self.steps if step.lstrip().startswith(f"- name: {name}\n")]
        self.assertEqual(
            len(matches), 1, f"build-sakura.yml must have exactly one step named {name!r}"
        )
        return matches[0]

    def _eligibility_expression(self) -> str:
        matches = re.findall(rf"(?m)^\s+{ELIGIBILITY}:\s*(?P<value>.+)$", self.text)
        self.assertEqual(
            len(matches),
            1,
            f"{ELIGIBILITY} must be defined exactly once; two definitions would "
            f"let the gates disagree again by a different route",
        )
        return matches[0]

    def test_eligibility_is_decided_once_for_the_whole_job(self) -> None:
        expression = self._eligibility_expression()
        # Job-level `env` sits inside the `build:` job and above its `steps:`.
        # A step-level definition would be visible to that step alone, which
        # is the drift this replaced.
        job = self.text.index("\n  build:\n")
        self.assertLess(job, self.text.index(f"      {ELIGIBILITY}:"))
        self.assertLess(
            self.text.index(f"      {ELIGIBILITY}:"), self.text.index("\n    steps:\n", job)
        )
        self.assertIn("github.event_name == 'pull_request'", expression)
        self.assertIn("github.event.pull_request.base.ref == 'main'", expression)

    def test_only_same_repository_pull_requests_are_eligible(self) -> None:
        # A fork cannot read the base branch's map cache, so a fork PR would
        # take the cache-miss path anyway -- but it would also be the one
        # place where an untrusted head chooses its own test filter.
        self.assertIn(
            "github.event.pull_request.head.repo.full_name == github.repository",
            self._eligibility_expression(),
        )

    def test_main_promotion_head_stays_excluded(self) -> None:
        # A `main` head is the integration state itself: the last place to
        # accept reduced coverage, and a diff that is not the work of one change.
        expression = self._eligibility_expression()
        self.assertIn("github.event.pull_request.head.ref != 'main'", expression)

    def test_every_gate_reads_the_shared_flag(self) -> None:
        for name in GATED_STEPS:
            with self.subTest(step=name):
                self.assertIn(GATE, self._step(name))

    def test_the_evidence_upload_still_runs_after_a_failure(self) -> None:
        # The selection decision is the only record of which tests ran; a
        # failing suite is exactly when it is needed.
        self.assertIn(f"if: ${{{{ always() && {GATE} }}}}", self._step(GATED_STEPS[2]))

    def test_no_gate_reintroduces_a_branch_name_condition(self) -> None:
        # A branch-name condition here would be dead weight at best: it cannot
        # make an unsound selection sound, and a second condition beside the
        # shared flag is how the three gates drifted apart in the first place.
        self.assertNotIn("head.ref, 'feature/'", self.text)
        self.assertNotIn("startsWith(github.event.pull_request.head.ref", self.text)
        for name in GATED_STEPS:
            with self.subTest(step=name):
                conditions = re.findall(r"(?m)^\s+if:\s*(?P<value>.+)$", self._step(name))
                self.assertEqual(len(conditions), 1, f"{name} must have exactly one condition")
                self.assertNotIn("head.ref", conditions[0].replace(GATE, ""))

    def test_the_selection_step_falls_back_to_the_full_filter_on_any_error(self) -> None:
        step = self._step("Select main PR tests")
        self.assertIn("GetEnvironmentVariable($omissionVariable)", step)
        catch = step[step.index("} catch {") :]
        self.assertIn("$runSelection = $completeSelection", catch)
        self.assertIn("selection_workflow_error", catch)
        # An empty or full-fallback decision must reach the same filter as a
        # thrown error, or the classifier's fail-closed codes would only be
        # honoured when the tooling also happened to crash.
        self.assertIn(
            "if ($decision.full_fallback -or [string]::IsNullOrWhiteSpace($runSelection)) {", step
        )


class FailClosedClassifierTests(unittest.TestCase):
    """The classifier half: every change class it cannot reason about is full.

    ``tools/build/tests/test_coverage_map.py`` is never collected (see the
    module docstring), so these run in CI in its place now that no branch name
    stands between an arbitrary pull request and this decision.
    """

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "sakura_core").mkdir()
        for name in ("foo.cpp", "foo.hpp", "bar.cpp"):
            (self.root / "sakura_core" / name).write_text("// fixture\n", encoding="utf-8")
        fragment = self.root / "FooTest.xml"
        fragment.write_text(
            '<?xml version="1.0"?>\n'
            "<coverage>\n"
            f"  <sources><source>{self.root.as_posix()}</source></sources>\n"
            '  <packages><package><classes><class filename="sakura_core/foo.cpp">\n'
            '    <lines><line number="1" hits="1"/></lines>\n'
            "  </class></classes></package></packages>\n"
            "</coverage>\n",
            encoding="utf-8",
        )
        self.inventory = _inventory()
        self.map = build_coverage_map(
            base_sha=BASE_SHA,
            test_binary_sha256=BINARY_SHA,
            inventory=self.inventory,
            fragments=(("FooTest.*", fragment),),
            repo_root=self.root,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def decide(self, *changed: ChangedFile, **kwargs) -> dict:
        return select_tests(
            changed_files=changed,
            coverage_map=kwargs.pop("coverage_map", self.map),
            inventory=self.inventory,
            repo_root=self.root,
            expected_base_sha=kwargs.pop("expected_base_sha", BASE_SHA),
            smoke_selectors=("SmokeSuite.*",),
            module_index=kwargs.pop(
                "module_index",
                {
                    "foo": ("sakura_core/foo.cpp", "sakura_core/foo.hpp"),
                    "bar": ("sakura_core/bar.cpp",),
                },
            ),
            **kwargs,
        )

    def assertFullSuite(self, decision: dict, reason: str) -> None:
        self.assertTrue(decision["full_fallback"], f"{reason} did not fall back to the full suite")
        self.assertEqual("full", decision["mode"])
        self.assertIn(reason, decision["reason_codes"])
        # The workflow substitutes its own full filter whenever the decision
        # is a fallback; an empty filter here keeps a dropped substitution
        # from quietly running zero tests instead of all of them.
        self.assertEqual("", decision["gtest_filter"])

    def test_the_selection_this_gate_exists_for_still_narrows(self) -> None:
        # Without this, every assertion below would pass on a classifier that
        # had simply stopped selecting anything.
        decision = self.decide(ChangedFile("sakura_core/foo.cpp"))
        self.assertEqual("selected", decision["mode"])
        self.assertFalse(decision["full_fallback"])
        self.assertIn("FooTest.*", decision["gtest_filter"])
        self.assertIn("SmokeSuite.*", decision["gtest_filter"])

    def test_each_unreasonable_change_class_runs_the_full_suite(self) -> None:
        cases = {
            # A deleted or renamed file's map entry describes a path that no
            # longer exists, so nothing it says about coverage is usable.
            "deletion_or_rename": (ChangedFile("sakura_core/foo.cpp", "D"),),
            # .gitignore/.editorconfig change what the build and the tools see
            # without appearing in any coverage relation.
            "repository_configuration": (ChangedFile(".gitignore"),),
            # Project, CMake, workflow, and module-manifest edits change which
            # sources build into which binary, invalidating the map wholesale.
            "build_or_test_topology": (ChangedFile("sakura_core/sakura.vcxproj"),),
            # Coverage runs from tests, so a test edit cannot be resolved by
            # the coverage those same tests produced.
            "test_or_external_source": (ChangedFile("src/test/cpp/tests1/test-foo.cpp"),),
            # A path outside every production prefix is a path the classifier
            # has no rule for.
            "unknown_change_class": (ChangedFile("sakura.sln"),),
            # A production source with no map entry has unknown impact.
            "coverage_path_unmapped": (ChangedFile("sakura_core/unknown.cpp"),),
            # A known module whose sources are absent from the map is the same
            # unknown impact, one level up.
            "module_has_no_coverage": (ChangedFile("sakura_core/bar.cpp"),),
        }
        for reason, changed in cases.items():
            with self.subTest(reason=reason):
                self.assertFullSuite(self.decide(*changed), reason)

    def test_externals_changes_run_the_full_suite(self) -> None:
        # `externals/` is a production prefix, so this is only fail-closed
        # because the classifier names it before the production check.
        self.assertFullSuite(
            self.decide(ChangedFile("externals/miniz-cpp/zip_file.hpp")),
            "test_or_external_source",
        )

    def test_a_missing_or_stale_map_runs_the_full_suite(self) -> None:
        # The cache restore is best-effort, and a base SHA moves whenever
        # main advances under an open pull request.
        self.assertFullSuite(
            self.decide(ChangedFile("sakura_core/foo.cpp"), coverage_map=None),
            "coverage_map_missing",
        )
        self.assertFullSuite(
            self.decide(ChangedFile("sakura_core/foo.cpp"), expected_base_sha="c" * 40),
            "coverage_map_base_sha",
        )

    def test_a_selection_that_excludes_an_impacted_test_runs_the_full_suite(self) -> None:
        # The headless excludes are passed in as excluded selectors; if the
        # impacted set lands inside them, running the narrowed filter would
        # skip the very tests the change impacts.
        self.assertFullSuite(
            self.decide(ChangedFile("sakura_core/foo.cpp"), excluded_selectors=("FooTest.*",)),
            "impacted_tests_excluded",
        )

    def test_a_selection_near_the_whole_suite_runs_the_full_suite(self) -> None:
        # Narrowing stops paying for itself well before 100%, and a wide
        # selection is a sign the map no longer discriminates.
        self.assertFullSuite(
            self.decide(ChangedFile("sakura_core/foo.cpp"), threshold=0.2),
            "selected_test_threshold",
        )

    def test_documentation_only_changes_still_run_smoke(self) -> None:
        # Fail-closed must not mean "always full": a docs-only pull request
        # running the entire headless suite is the cost this gate removes.
        decision = self.decide(ChangedFile("README.md"))
        self.assertEqual("smoke", decision["mode"])
        self.assertFalse(decision["full_fallback"])
        self.assertEqual("SmokeSuite.*", decision["gtest_filter"])


if __name__ == "__main__":
    unittest.main()
