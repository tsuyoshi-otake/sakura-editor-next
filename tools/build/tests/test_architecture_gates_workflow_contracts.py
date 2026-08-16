from __future__ import annotations

import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/architecture-gates.yml"
RULESET_BEFORE = REPO_ROOT / "docs/evidence/issue18-architecture-gates-ruleset-before.json"
RULESET_AFTER = REPO_ROOT / "docs/evidence/issue18-architecture-gates-ruleset-after.json"


class ArchitectureGatesWorkflowContractTests(unittest.TestCase):
    def _workflow_text(self) -> str:
        return WORKFLOW.read_text(encoding="utf-8-sig")

    def _job_text(self) -> str:
        text = self._workflow_text()
        return text[text.index("  architecture-gates:\n"):]

    @staticmethod
    def _read_json(path: Path) -> dict[str, object]:
        return json.loads(path.read_text(encoding="utf-8"))

    @staticmethod
    def _rule_by_type(rules: list[object], rule_type: str) -> dict[str, object]:
        for rule in rules:
            if isinstance(rule, dict) and rule.get("type") == rule_type:
                return rule
        raise AssertionError(f"ruleset does not contain {rule_type}")

    def test_pull_request_trigger_is_unfiltered_and_job_is_unconditional(self) -> None:
        text = self._workflow_text()
        trigger = text[text.index("on:\n"):text.index("\npermissions:")]
        job = self._job_text()

        self.assertIn("  workflow_call:\n", trigger)
        self.assertNotIn("  pull_request:\n", trigger)
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
            "python3 tools/build/sakura_build.py --format json lint checkout-invariance",
            job,
        )
        self.assertLess(
            job.index("python3 tools/build/sakura_build.py --format json lint checkout-invariance"),
            job.index("python3 tools/build/sakura_build.py --format json inventory semantic --strict"),
        )
        self.assertIn(
            "python3 tools/build/sakura_build.py --format json inventory semantic --strict",
            job,
        )
        self.assertIn("python3 tools/build/sakura_build.py generate --check", job)
        self.assertIn("python3 tools/build/sakura_build.py graph check --all-contexts", job)
        self.assertIn("python3 tools/dependency_ledger.py check", job)

    def test_ruleset_snapshot_appends_only_architecture_gates_requirement(self) -> None:
        before = self._read_json(RULESET_BEFORE)
        after = self._read_json(RULESET_AFTER)

        before_metadata = dict(before)
        after_metadata = dict(after)
        before_metadata.pop("rules")
        before_metadata.pop("updated_at")
        after_metadata.pop("rules")
        after_metadata.pop("updated_at")
        self.assertEqual(before_metadata, after_metadata)

        before_rules = before["rules"]
        after_rules = after["rules"]
        self.assertIsInstance(before_rules, list)
        self.assertIsInstance(after_rules, list)
        self.assertEqual(
            [rule for rule in before_rules if rule.get("type") != "required_status_checks"],
            [rule for rule in after_rules if rule.get("type") != "required_status_checks"],
        )

        before_status = self._rule_by_type(before_rules, "required_status_checks")
        after_status = self._rule_by_type(after_rules, "required_status_checks")
        before_parameters = before_status["parameters"]
        after_parameters = after_status["parameters"]
        self.assertIsInstance(before_parameters, dict)
        self.assertIsInstance(after_parameters, dict)
        before_status_metadata = dict(before_parameters)
        after_status_metadata = dict(after_parameters)
        before_status_metadata.pop("required_status_checks")
        after_status_metadata.pop("required_status_checks")
        self.assertEqual(before_status_metadata, after_status_metadata)
        before_required = before_parameters["required_status_checks"]
        after_required = after_parameters["required_status_checks"]
        self.assertIsInstance(before_required, list)
        self.assertIsInstance(after_required, list)
        self.assertEqual(after_required[:-1], before_required)
        self.assertEqual(
            after_required[-1],
            {"context": "architecture-gates", "integration_id": 15368},
        )


if __name__ == "__main__":
    unittest.main()
