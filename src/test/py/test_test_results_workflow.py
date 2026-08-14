"""Contracts for the post-SonarCloud test-results publication path (#112).

SonarCloud analysis was removed after the evidence showed it had never produced
a result in this fork; the pieces that replaced it are pinned here so they do
not silently drift back apart:

* ``build-sakura.yml`` builds both matrix configurations with ``build-sln.bat``
  and uploads a per-configuration ``test-results-*`` artifact.
* ``test-results.yml`` is the ``workflow_run`` consumer that publishes the
  "Test Results" check from the Release artifact, guarded the same way the
  retired SonarCloud consumer was.
* ``CMakeLists.txt`` registers ``UnitTest`` for Debug as well when
  OpenCppCoverage is absent, because the instrumented Debug ``CoverageTest``
  existed only to feed Sonar coverage and the hosted build jobs no longer
  install OpenCppCoverage.

Like ``test_develop_issue_closure.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"
BUILD_WORKFLOW = WORKFLOWS / "build-sakura.yml"
TEST_RESULTS_WORKFLOW = WORKFLOWS / "test-results.yml"
CMAKE_LISTS = REPO_ROOT / "CMakeLists.txt"


class SonarRemovalTests(unittest.TestCase):
    def test_no_workflow_references_sonar_or_build_wrapper(self) -> None:
        sonar = re.compile("sonar", re.IGNORECASE)
        wrapper = re.compile("build-wrapper|bw-output", re.IGNORECASE)
        workflows = sorted(WORKFLOWS.glob("*.yml"))
        self.assertTrue(workflows)
        for workflow in workflows:
            text = workflow.read_text(encoding="utf-8-sig")
            with self.subTest(workflow=workflow.name):
                self.assertIsNone(sonar.search(text))
                self.assertIsNone(wrapper.search(text))

    def test_the_local_scanner_config_and_tooling_are_gone(self) -> None:
        # Retired with the pipeline: the scanner's project config and the
        # local helper scripts under tools/ would otherwise imply the
        # analysis still exists somewhere.
        self.assertFalse((REPO_ROOT / "sonar-project.properties").exists())
        self.assertFalse((REPO_ROOT / "tools/SonarQube").exists())

    def test_both_matrix_configurations_build_with_build_sln(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8-sig")
        self.assertIn("      if: ${{ inputs.release_promotion }}\n", text)
        self.assertIn("      if: ${{ !inputs.release_promotion }}\n", text)
        self.assertNotIn("MsBuild.exe", text)

    def test_build_jobs_upload_the_test_results_artifact(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8-sig")
        upload_start = text.index("    - name: Upload test-results artifact\n")
        upload = text[upload_start : text.index("    - name: ", upload_start + 1)]
        self.assertIn("name: test-results-${{ matrix.platform }}-${{ matrix.config }}", upload)
        self.assertIn("tests*-googletest.xml", upload)
        self.assertIn("github-event.json", upload)
        self.assertIn("if-no-files-found: error", upload)
        # The trigger event must still be captured for the consumer.
        self.assertIn("    - name: Copy github-event.json\n", text)


class TestResultsWorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = TEST_RESULTS_WORKFLOW.read_text(encoding="utf-8-sig")

    def test_the_workflow_consumes_completed_build_sakura_runs(self) -> None:
        self.assertIn(
            "on:\n  workflow_run:\n    workflows: ['build sakura', 'PR Gate']\n    types: [completed]\n",
            self.text,
        )

    def test_only_successful_same_repository_runs_are_published(self) -> None:
        self.assertIn("github.event.workflow_run.conclusion == 'success'", self.text)
        self.assertIn(
            "github.event.workflow_run.head_repository.full_name == github.repository",
            self.text,
        )

    def test_write_permissions_are_scoped_to_the_publishing_job(self) -> None:
        header, job = self.text.split("jobs:\n", 1)
        self.assertIn("permissions: {}\n", header)
        self.assertIn(
            "    permissions:\n      actions: read\n      checks: write\n      pull-requests: write\n",
            job,
        )

    def test_the_artifact_download_targets_the_triggering_run(self) -> None:
        self.assertIn("name: test-results-x64-Release", self.text)
        self.assertIn("github-token: ${{ secrets.GITHUB_TOKEN }}", self.text)
        self.assertIn("run-id: ${{ github.event.workflow_run.id }}", self.text)

    def test_documentation_only_runs_do_not_require_a_release_artifact(self) -> None:
        self.assertIn("id: artifact", self.text)
        self.assertIn("steps.artifact.outputs.exists == 'true'", self.text)
        self.assertIn(
            "No Release test-results artifact was expected for a documentation-only CI plan.",
            self.text,
        )
        self.assertEqual(self.text.count("if: ${{ steps.artifact.outputs.exists == 'true' }}"), 2)

    def test_the_check_is_published_from_the_downloaded_release_results(self) -> None:
        self.assertIn("uses: EnricoMi/publish-unit-test-result-action@v2", self.text)
        self.assertIn("check_name: Test Results", self.text)
        self.assertIn("commit: ${{ github.event.workflow_run.head_sha }}", self.text)
        self.assertIn("event_file: test-results/github-event.json", self.text)
        self.assertIn("event_name: ${{ github.event.workflow_run.event }}", self.text)
        self.assertIn("test-results/*-googletest.xml", self.text)


class DebugUnitTestRegistrationTests(unittest.TestCase):
    def test_unit_test_covers_debug_when_opencppcoverage_is_absent(self) -> None:
        text = CMAKE_LISTS.read_text(encoding="utf-8-sig")
        guarded_start = text.index("if(OpenCppCoverage_EXECUTABLE)\n")
        else_start = text.index("else()\n", guarded_start)
        end = text.index("endif()\n", else_start)
        self.assertIn("set(SAKURA_UNIT_TEST_CONFIGURATIONS Release)", text[guarded_start:else_start])
        self.assertIn("set(SAKURA_UNIT_TEST_CONFIGURATIONS Debug Release)", text[else_start:end])
        self.assertIn("CONFIGURATIONS ${SAKURA_UNIT_TEST_CONFIGURATIONS}", text[end:])
        # The XML the test-results artifact uploads must keep being produced.
        self.assertEqual(
            text.count("--gtest_output=xml:${CMAKE_SOURCE_DIR}/tests1-googletest.xml"), 2
        )


if __name__ == "__main__":
    unittest.main()
