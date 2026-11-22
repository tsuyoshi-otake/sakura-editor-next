"""Guard native PowerShell exit codes in GitHub Actions (#429)."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GITHUB = REPO_ROOT / ".github"


class WorkflowNativeExitCodeTests(unittest.TestCase):
    def test_native_calls_are_not_piped_into_select_object_first(self) -> None:
        early_pipeline_stop = re.compile(
            r"&\s*\S+[^|\n]*\|\s*Select-Object\s+-First\b", re.IGNORECASE
        )
        workflows = (GITHUB / "workflows").glob("*.yml")
        actions = (GITHUB / "actions").rglob("action.yml")
        violations = []
        for path in sorted([*workflows, *actions]):
            for line_number, line in enumerate(
                path.read_text(encoding="utf-8-sig").splitlines(), start=1
            ):
                if early_pipeline_stop.search(line):
                    violations.append(f"{path.relative_to(REPO_ROOT).as_posix()}:{line_number}")

        self.assertFalse(
            violations,
            "Native calls piped into Select-Object -First:\n" + "\n".join(violations),
        )
