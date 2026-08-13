"""Contracts for the cppcheck analyzer-information cache (#112).

``--cppcheck-build-dir`` is not a compiler cache.  cppcheck stores one analysis
result per translation unit and re-emits the cached findings, so a hit reports
exactly what a miss would; it additionally restores the whole-program checks
(``unusedFunction``, ``ctunullpointer``, ``ctuuninitvar``) that ``-j`` disables.
Measured against ``sakura_core/sakura.vcxproj`` with cppcheck 2.21.0, the same
build MSYS2 installs in CI: 609s uncached, 599s cold, 18s warm, with the cold
and warm XML byte-identical over all 7558 findings.

The pieces below are pinned because each one fails silently rather than loudly:

* If the workflow's cached path and the directory ``run-cppcheck.bat`` computes
  ever drift apart, every run is a cold run and nothing turns red.
* If the key stops carrying the resolved cppcheck version, a toolchain upgrade
  can resurrect analyzer information produced by a different analyzer.
* If the save step stops excluding pull requests, every pull-request run adds a
  cache entry that no other run is allowed to read, against a repository-wide
  10 GiB quota that was already 8.46 GiB full when this was introduced.

Like ``test_test_results_workflow.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
RUN_CPPCHECK = REPO_ROOT / "run-cppcheck.bat"
WORKFLOW = REPO_ROOT / ".github/workflows/cppcheck.yml"
GITIGNORE = REPO_ROOT / ".gitignore"

WORKFLOW_PATH_EXPRESSION = (
    ".cppcheck-build-${{ env.BuildPlatform }}-${{ env.Configuration }}"
)

# What is pinned here is that restore and save are the *separate* sub-actions,
# never the combined ``actions/cache@``: the combined action writes an entry
# from its own post-step, which would put a cache write back on the
# pull-request path this file exists to keep clear.  The major version is not
# part of that contract -- it is dependabot's to bump -- and spelling it into
# the assertion only turned a routine bump into a red build (#149).
CACHE_RESTORE_RE = re.compile(r"uses:\s*actions/cache/restore@")
CACHE_SAVE_RE = re.compile(r"uses:\s*actions/cache/save@")


def _batch_build_dir_as_workflow_expression() -> str:
    """Render the batch file's build directory the way the workflow spells it."""
    text = RUN_CPPCHECK.read_text(encoding="ascii")
    match = re.search(r"^set CPPCHECK_BUILD_DIR=%~dp0(.+)$", text, re.MULTILINE)
    assert match is not None, "run-cppcheck.bat no longer defines CPPCHECK_BUILD_DIR"
    return (
        match.group(1)
        .strip()
        .replace("%platform%", "${{ env.BuildPlatform }}")
        .replace("%configuration%", "${{ env.Configuration }}")
    )


class RunCppcheckBatchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = RUN_CPPCHECK.read_text(encoding="ascii")

    def test_the_build_directory_is_created_and_passed_to_cppcheck(self) -> None:
        self.assertIn(
            'if not exist "%CPPCHECK_BUILD_DIR%" (\n\tmkdir "%CPPCHECK_BUILD_DIR%"\n)',
            self.text.replace("\r\n", "\n"),
        )
        self.assertIn(
            'set CPPCHECK_PARAMS=%CPPCHECK_PARAMS% '
            '"--cppcheck-build-dir=%CPPCHECK_BUILD_DIR%"',
            self.text,
        )

    def test_the_build_directory_is_separate_per_platform_and_configuration(self) -> None:
        # One shared directory across configurations would let a Debug run's
        # analyzer information answer for a Release run, whose preprocessor
        # macros differ (-D_DEBUG vs -U_DEBUG).
        definition = re.search(
            r"^set CPPCHECK_BUILD_DIR=(.+)$", self.text, re.MULTILINE
        )
        assert definition is not None
        self.assertIn("%platform%", definition.group(1))
        self.assertIn("%configuration%", definition.group(1))

    def test_the_report_files_are_still_deleted_before_each_run(self) -> None:
        # The cache must not turn a failed run into a stale-but-plausible
        # report: the XML and log are still rebuilt from scratch every time.
        normalized = self.text.replace("\r\n", "\n")
        self.assertIn('if exist "%CPPCHECK_OUT%" (\n\tdel %CPPCHECK_OUT%\n)', normalized)
        self.assertIn('if exist "%CPPCHECK_LOG%" (\n\tdel %CPPCHECK_LOG%\n)', normalized)

    def test_the_script_stays_ascii_with_crlf_endings(self) -> None:
        raw = RUN_CPPCHECK.read_bytes()
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))
        raw.decode("ascii")


class CppcheckWorkflowCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8-sig")
        self.steps = self.text.split("      - name: ")

    def _step(self, name: str) -> str:
        for step in self.steps:
            if step.startswith(name + "\n"):
                return step
        raise AssertionError(f"cppcheck.yml has no step named {name!r}")

    def test_the_cached_path_is_the_directory_the_batch_file_uses(self) -> None:
        self.assertEqual(
            _batch_build_dir_as_workflow_expression(), WORKFLOW_PATH_EXPRESSION
        )
        for name in (
            "Restore cppcheck analyzer information",
            "Save cppcheck analyzer information",
        ):
            with self.subTest(step=name):
                self.assertIn(f"path: {WORKFLOW_PATH_EXPRESSION}", self._step(name))

    def test_the_key_carries_the_resolved_cppcheck_version(self) -> None:
        self.assertIn(
            "run: cppcheck --version > cppcheck-installed-version.txt",
            self._step("Report cppcheck version"),
        )
        resolve = self._step("Resolve cppcheck cache key component")
        self.assertIn("id: cppcheck-version", resolve)
        self.assertIn('"value=$component" >> $env:GITHUB_OUTPUT', resolve)
        # An unreadable version must fail the step rather than silently produce
        # one shared key for every cppcheck build.
        self.assertIn("throw", resolve)
        for name in (
            "Restore cppcheck analyzer information",
            "Save cppcheck analyzer information",
        ):
            with self.subTest(step=name):
                self.assertIn(
                    "key: cppcheck-analyzer-${{ env.BuildPlatform }}-"
                    "${{ env.Configuration }}-"
                    "${{ steps.cppcheck-version.outputs.value }}-${{ github.sha }}",
                    self._step(name),
                )

    def test_restore_falls_back_to_an_older_entry_of_the_same_analyzer(self) -> None:
        restore = self._step("Restore cppcheck analyzer information")
        self.assertRegex(restore, CACHE_RESTORE_RE)
        self.assertIn(
            "restore-keys: |\n"
            "            cppcheck-analyzer-${{ env.BuildPlatform }}-"
            "${{ env.Configuration }}-"
            "${{ steps.cppcheck-version.outputs.value }}-\n",
            restore,
        )

    def test_only_non_pull_request_events_write_to_the_cache(self) -> None:
        save = self._step("Save cppcheck analyzer information")
        self.assertRegex(save, CACHE_SAVE_RE)
        self.assertIn("if: ${{ github.event_name != 'pull_request' }}", save)

    def test_restore_precedes_the_analysis_and_save_follows_it(self) -> None:
        order = [
            self.text.index("      - name: Restore cppcheck analyzer information\n"),
            self.text.index("      - name: Run cppcheck\n"),
            self.text.index("      - name: Save cppcheck analyzer information\n"),
        ]
        self.assertEqual(order, sorted(order))

    def test_the_published_artifact_is_still_only_the_report(self) -> None:
        upload = self._step("Upload cppcheck log artifact")
        self.assertNotIn(".cppcheck-build-", upload)
        self.assertIn("if-no-files-found: error", upload)


class CppcheckBuildDirectoryIgnoreTests(unittest.TestCase):
    def test_the_build_directory_is_ignored(self) -> None:
        entries = GITIGNORE.read_text(encoding="utf-8-sig").splitlines()
        self.assertIn("/.cppcheck-build-*/", entries)


if __name__ == "__main__":
    unittest.main()
