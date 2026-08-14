"""Contracts for the declared-package closure cache and the cache save policy.

Restoring the declared package closure took 123s of the 20m05s Release leg in
#123 and repeated the same work in both matrix jobs on every run.  It is now
wrapped in an ``actions/cache`` entry keyed on the restore's own ``plan_hash``,
which already covers the declared package set, every declared input digest,
both triplets, and the vcpkg tool/toolchain hashes.

Every invariant below fails silently rather than loudly, which is why it is
pinned here:

* If the cached path stops matching the entry directory ``package_restore.py``
  computes, every run is a cold run and nothing turns red.
* If the ``Restore declared Sakura packages`` step is ever gated on
  ``cache-hit``, a cache hit stops producing the explicit restore that #116 and
  #118 require, and the structural validation of the restored tree is skipped
  with it.
* If a save step stops excluding pull requests, every pull-request run adds an
  entry scoped to its own ``refs/pull/N/merge`` ref that no other run may read.
  By #123 the repository held 133 entries using 9.88 GiB of a 10 GiB quota,
  7.17 GiB of it in 82 such entries belonging to already merged pull requests.
  Above that fill level a newly saved entry is evicted almost immediately, so
  every cache in the repository becomes a permanent miss without any signal.

Like ``test_cppcheck_analyzer_cache.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW_DIR = REPO_ROOT / ".github/workflows"
BUILD_WORKFLOW = WORKFLOW_DIR / "build-sakura.yml"
CLEANUP_WORKFLOW = WORKFLOW_DIR / "pr-cache-cleanup.yml"
PACKAGE_RESTORE = REPO_ROOT / "tools/build/sakura_build_lib/package_restore.py"

PATH_EXPRESSION = "${{ steps.package-plan.outputs.path }}"
KEY_EXPRESSION = "sakura-packages-${{ runner.os }}-${{ steps.package-plan.outputs.hash }}"

# `coverage-map.yml` has no step-level pull-request exclusion because it cannot
# be reached by a pull request at all: it is workflow_call-only and its single
# caller is build-sakura's trusted develop-push path.  The cache write has to
# retain that caller's scope so feature PRs can restore the map read-only,
# which is exactly what a `workflow_run` indirection would destroy.  The test
# below re-checks that reason instead of trusting the entry.
CALLER_GATED_WORKFLOWS = {"coverage-map.yml"}

STEP_BOUNDARY_RE = re.compile(r"(?m)^(?=[ \t]*-[ \t]+(?:name|uses|if):)")
# Anything that can write an entry: the combined action writes from its own
# post-step, so it counts too.
CACHE_SAVE_RE = re.compile(r"uses:\s*actions/cache(?:/save)?@")
# The save sub-action specifically.  Neither pattern spells a major version:
# which one is current is dependabot's to bump, and pinning it into an
# assertion only turned a routine bump into a red build (#149).
CACHE_SAVE_ACTION_RE = re.compile(r"uses:\s*actions/cache/save@")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _workflows() -> list[Path]:
    return sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))


def _steps(text: str) -> list[str]:
    """Split a workflow into step-sized blocks without a YAML parser.

    Only ``- name:``/``- uses:``/``- if:`` begin a step, so matrix entries,
    trigger branch lists, and block scalars such as ``restore-keys: |`` stay
    inside the block that owns them.
    """

    return STEP_BOUNDARY_RE.split(text)


class PackageClosureCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _read(BUILD_WORKFLOW)
        self.steps = _steps(self.text)

    def _step(self, name: str) -> str:
        matches = [step for step in self.steps if step.lstrip().startswith(f"- name: {name}\n")]
        self.assertEqual(
            len(matches), 1, f"build-sakura.yml must have exactly one step named {name!r}"
        )
        return matches[0]

    def test_the_cached_path_is_the_entry_directory_the_restore_computes(self) -> None:
        source = PACKAGE_RESTORE.read_text(encoding="utf-8-sig")
        root = re.search(
            r'_PACKAGE_ROOT_RELATIVE\s*=\s*Path\("(?P<value>[^"]+)"\)', source
        )
        self.assertIsNotNone(root, "package_restore.py no longer declares a cache root")
        assert root is not None
        # The entry name is the plan hash with its `sha256:` prefix removed.
        self.assertIn('entry_name = plan_hash.split(":", 1)[1]', source)
        self.assertIn('entry_root = cache_root / "e" / entry_name', source)

        plan = self._step("Plan the declared package restore")
        self.assertIn(
            f'"path={root.group("value")}/e/$hash" >> $env:GITHUB_OUTPUT',
            plan,
            "the cached path no longer matches the entry directory the restore "
            "publishes, so every run would be a cold run",
        )

    def test_the_plan_step_publishes_a_validated_hash(self) -> None:
        plan = self._step("Plan the declared package restore")
        self.assertIn("id: package-plan", plan)
        self.assertIn("package plan sakura_app --context $context", plan)
        # A malformed hash must skip the cache, not key an entry on garbage and
        # not fail a build the uncached restore would have completed.
        self.assertIn("$hash -notmatch '^[0-9a-f]{64}$'", plan)
        self.assertIn('"hash=" >> $env:GITHUB_OUTPUT', plan)
        self.assertIn("::warning::", plan)

    def test_restore_and_save_agree_on_the_path_and_key(self) -> None:
        for name in ("Restore cached package closure", "Save cached package closure"):
            with self.subTest(step=name):
                step = self._step(name)
                self.assertIn(f"path: {PATH_EXPRESSION}", step)
                self.assertIn(f"key: {KEY_EXPRESSION}", step)
                self.assertIn("steps.package-plan.outputs.hash != ''", step)

    def test_the_declared_restore_stays_unconditional(self) -> None:
        # `reused` still validates the restored tree and still reports
        # native_restore_execution_observed, so the explicit-restore evidence
        # contract survives a cache hit only while this step always runs.
        restore = self._step("Restore declared Sakura packages")
        self.assertNotRegex(
            restore,
            r"(?m)^\s+if:",
            "gating the declared restore on the cache would skip both the "
            "structural validation of the restored tree and the explicit "
            "restore #116/#118 require",
        )
        self.assertIn("package restore sakura_app --context $context", restore)

    def test_only_non_pull_request_events_write_to_the_cache(self) -> None:
        save = self._step("Save cached package closure")
        self.assertRegex(save, CACHE_SAVE_ACTION_RE)
        self.assertIn("github.event_name != 'pull_request'", save)
        self.assertIn("steps.package-cache.outputs.cache-hit != 'true'", save)

    def test_the_steps_run_in_the_only_useful_order(self) -> None:
        order = [
            self.text.index("    - name: Plan the declared package restore\n"),
            self.text.index("    - name: Restore cached package closure\n"),
            self.text.index("    - name: Restore declared Sakura packages\n"),
            self.text.index("    - name: Save cached package closure\n"),
            self.text.index("    - name: MSBuild\n"),
        ]
        self.assertEqual(order, sorted(order))


class RepositoryCacheSavePolicyTests(unittest.TestCase):
    def test_every_cache_write_excludes_pull_requests(self) -> None:
        for workflow in _workflows():
            if workflow.name in CALLER_GATED_WORKFLOWS:
                continue
            text = _read(workflow)
            for step in _steps(text):
                if not CACHE_SAVE_RE.search(step):
                    continue
                name = step.lstrip().splitlines()[0]
                with self.subTest(workflow=workflow.name, step=name):
                    self.assertIn(
                        "github.event_name != 'pull_request'",
                        step,
                        f"{workflow.name} saves a cache on pull requests; that "
                        f"entry is scoped to refs/pull/N/merge, no other run "
                        f"can read it, and it consumes the repository-wide "
                        f"10 GiB quota until GitHub evicts it",
                    )

    def test_the_caller_gated_workflow_is_unreachable_from_a_pull_request(self) -> None:
        for name in CALLER_GATED_WORKFLOWS:
            with self.subTest(workflow=name):
                text = _read(WORKFLOW_DIR / name)
                # No DOTALL here: `.` matching newlines would let the body run
                # past `on:` and collect every later top-level block's keys.
                triggers = re.search(r"(?m)^on:\n(?P<body>(?:[ \t]+.*\n|\n)*)", text)
                self.assertIsNotNone(triggers, f"{name} declares no triggers")
                assert triggers is not None
                declared = re.findall(r"(?m)^  (\w+):", triggers.group("body"))
                self.assertEqual(
                    declared,
                    ["workflow_call"],
                    f"{name} is exempt from the pull-request save exclusion only "
                    f"because its caller gates it; a direct trigger removes that "
                    f"guarantee",
                )


class PullRequestCacheCleanupTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _read(CLEANUP_WORKFLOW)

    def test_it_runs_when_a_pull_request_closes(self) -> None:
        self.assertIn("on:\n  pull_request:\n    types: [closed]\n", self.text)
        # Merged and abandoned pull requests both leave unreadable entries, so
        # unlike develop-issue-closure.yml this must not require `merged`.
        self.assertNotIn("github.event.pull_request.merged", self.text)

    def test_it_can_delete_caches(self) -> None:
        self.assertIn("actions: write", self.text)
        self.assertIn(
            'gh api --method DELETE "repos/$REPOSITORY/actions/caches/$id"', self.text
        )
        self.assertIn('ref="refs/pull/${PR_NUMBER}/merge"', self.text)

    def test_it_stands_down_where_the_token_is_read_only(self) -> None:
        # A fork or Dependabot pull_request event receives a read-only token
        # whatever the permissions block says; the delete would fail rather
        # than be denied cleanly.
        self.assertIn('if [[ "$HEAD_REPOSITORY" != "$REPOSITORY" ]]; then', self.text)
        self.assertIn('if [[ "$ACTOR" == "dependabot[bot]" ]]; then', self.text)
        self.assertIn("eligible=false", self.text)
        self.assertIn("steps.eligibility.outputs.eligible == 'true'", self.text)

    def test_it_tolerates_an_entry_that_disappeared(self) -> None:
        # Eviction and a concurrent run of this job are both normal; neither is
        # a reason to fail a cleanup job.
        self.assertIn("::warning::Cache $id could not be deleted", self.text)


if __name__ == "__main__":
    unittest.main()
