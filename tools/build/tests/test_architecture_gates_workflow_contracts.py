from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/architecture-gates.yml"


class ArchitectureGatesWorkflowContractTests(unittest.TestCase):
    def _workflow_text(self) -> str:
        return WORKFLOW.read_text(encoding="utf-8-sig")

    def _job_text(self) -> str:
        text = self._workflow_text()
        return text[text.index("  architecture-gates:\n"):]

    def test_pull_request_trigger_is_unfiltered_and_job_is_unconditional(self) -> None:
        text = self._workflow_text()
        trigger = text[text.index("on:\n"):text.index("\npermissions:")]
        job = self._job_text()

        self.assertIn("  pull_request:\n", trigger)
        self.assertNotIn("paths:", trigger)
        self.assertNotIn("paths-ignore:", trigger)
        self.assertNotIn("\n    if:", job)
        self.assertNotIn("continue-on-error:", job)

    def test_job_fails_closed_on_all_architecture_contracts(self) -> None:
        job = self._job_text()

        self.assertIn("    name: architecture-gates\n", job)
        self.assertIn("    runs-on: ubuntu-latest\n", job)
        self.assertIn("fetch-depth: 0", job)
        self.assertIn(
            "python3 tools/build/sakura_build.py --format json inventory semantic --strict",
            job,
        )
        self.assertIn("python3 tools/build/sakura_build.py generate --check", job)
        self.assertIn("python3 tools/build/sakura_build.py graph check --all-contexts", job)


if __name__ == "__main__":
    unittest.main()
