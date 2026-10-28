from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from datetime import date
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.runner import BuildError  # noqa: E402
from sakura_build_lib.semantic_waivers import apply_semantic_waivers, semantic_waiver_ledger_path  # noqa: E402
import sakura_build  # noqa: E402


BASELINE_COMMIT = "a" * 40
TARGET_PATH = "sakura_core/io/CFileLoad.cpp"
TARGET_BYTES = b"int f() { return 1; }\n"
FINDING = {
    "rule_id": "global.get_dll_share_data",
    "path": TARGET_PATH,
    "line": 12,
    "column": 5,
    "evidence_sha256": "sha256:" + "b" * 64,
}
REDUCTION = {"path": TARGET_PATH, "baseline_finding_count": 1}


def _file_hash(content: bytes = TARGET_BYTES) -> str:
    normalized = content.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return "sha256:" + hashlib.sha256(normalized).hexdigest()


def _waiver(
    *,
    findings: list[dict[str, object]] | None = None,
    reductions: list[dict[str, object]] | None = None,
    **overrides: object,
) -> dict[str, object]:
    value: dict[str, object] = {
        "owner": "editor-core",
        "reason": "time-bounded issue acceptance",
        "tracking_issue": 389,
        "created": "2026-10-28",
        "expires": "2027-01-26",
        "baseline_source_commit": BASELINE_COMMIT,
        "files": {TARGET_PATH: _file_hash()},
        "new_findings": [FINDING] if findings is None else findings,
        "missing_touched_reductions": [REDUCTION] if reductions is None else reductions,
    }
    value.update(overrides)
    return value


class SemanticWaiverTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.target = self.root / TARGET_PATH
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(TARGET_BYTES)
        self.ledger_path = self.root / "tools/build/baselines/semantic-waivers.json"
        self.comparison: dict[str, object] = {
            "ok": False,
            "baseline_source_commit": BASELINE_COMMIT,
            "new_findings": [FINDING],
            "missing_touched_reductions": [REDUCTION],
            "removed_findings": [],
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_ledger(self, value: object) -> None:
        self.ledger_path.parent.mkdir(parents=True, exist_ok=True)
        self.ledger_path.write_text(json.dumps(value), encoding="utf-8")

    def apply_policy(self, *, today: date = date(2026, 10, 28)) -> dict[str, object]:
        return apply_semantic_waivers(
            self.comparison,
            repo_root=self.root,
            ledger_path=self.ledger_path,
            today=today,
        )

    def assert_policy_error(self, code: str) -> None:
        with self.assertRaises(BuildError) as raised:
            self.apply_policy()
        self.assertEqual(code, raised.exception.code)

    def test_alternative_baselines_have_independent_ledgers(self) -> None:
        default = self.root / "tools/build/baselines/editor-core-semantic.json"
        alternative = self.root / "tools/build/baselines/custom.json"
        self.assertEqual(self.ledger_path, semantic_waiver_ledger_path(self.root, default))
        self.assertEqual(alternative.with_name("custom-waivers.json"),
                         semantic_waiver_ledger_path(self.root, alternative))

    def test_missing_or_empty_ledger_preserves_raw_comparison(self) -> None:
        result = self.apply_policy()
        self.assertFalse(result["ok"])
        self.assertFalse(result["raw_ok"])
        self.assertFalse(result["effective_ok"])
        self.assertEqual(self.comparison["new_findings"], result["new_findings"])
        self.assertEqual(self.comparison["missing_touched_reductions"], result["missing_touched_reductions"])
        self.assertEqual([], result["waiver_review"]["waived"]["new_findings"])

        self.write_ledger({"schema_version": 1, "waivers": []})
        self.assertEqual(result, self.apply_policy())

    def test_exact_finding_and_reduction_can_be_waived_without_rewriting_raw_result(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        result = self.apply_policy()
        self.assertFalse(result["ok"])
        self.assertEqual([FINDING], result["new_findings"])
        self.assertEqual([REDUCTION], result["missing_touched_reductions"])
        self.assertTrue(result["effective_ok"])
        self.assertEqual([FINDING], result["waiver_review"]["waived"]["new_findings"])
        self.assertEqual([REDUCTION], result["waiver_review"]["waived"]["missing_touched_reductions"])

    def test_cli_strict_gate_uses_reviewed_exceptions_but_retains_raw_findings(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        baseline = self.root / "tools/build/baselines/editor-core-semantic.json"
        baseline.write_text("{}", encoding="utf-8")
        argv = ["--repo-root", str(self.root), "inventory", "semantic"]
        def fixed_date_policy(*args, **kwargs):
            return apply_semantic_waivers(*args, **kwargs, today=date(2026, 10, 28))

        with patch.object(sakura_build, "load_semantic_graph", return_value=SimpleNamespace(semantic_graph_hash="test")), \
             patch.object(sakura_build, "collect_semantic_inventory", return_value={}), \
             patch.object(sakura_build, "compare_semantic_inventory", side_effect=lambda *args, **kwargs: dict(self.comparison)), \
             patch.object(sakura_build, "apply_semantic_waivers", side_effect=fixed_date_policy) as policy, \
             patch.object(sakura_build, "write_semantic_inventory"), \
             patch.object(sakura_build, "semantic_inventory_summary", side_effect=lambda current, comparison, **kwargs: {"comparison": comparison}), \
             patch.object(sakura_build, "output") as output:
            self.assertEqual(0, sakura_build.main([*argv, "--strict"]))
            comparison = output.call_args.args[0]["comparison"]
            self.assertFalse(comparison["ok"])
            self.assertEqual([FINDING], comparison["new_findings"])
            self.assertTrue(comparison["effective_ok"])

            extra = {**FINDING, "line": 18, "evidence_sha256": "sha256:" + "c" * 64}
            self.comparison["new_findings"] = [FINDING, extra]
            self.assertEqual(sakura_build.EXIT_RATCHET, sakura_build.main([*argv, "--strict"]))
            self.assertEqual(0, sakura_build.main(argv))

            policy.reset_mock()
            self.assertEqual(0, sakura_build.main([*argv, "--collect-only"]))
            policy.assert_not_called()

    def test_unlisted_finding_stays_unwaived(self) -> None:
        extra = {**FINDING, "line": 18, "evidence_sha256": "sha256:" + "c" * 64}
        self.comparison["new_findings"] = [FINDING, extra]
        self.write_ledger({"schema_version": 1, "waivers": [_waiver(reductions=[])]})
        result = self.apply_policy()
        self.assertFalse(result["effective_ok"])
        self.assertEqual([extra], result["waiver_review"]["unwaived"]["new_findings"])
        self.assertEqual([FINDING, extra], result["new_findings"])

    def test_stale_or_non_exact_target_fails_closed(self) -> None:
        stale = {**FINDING, "column": 6}
        self.write_ledger({"schema_version": 1, "waivers": [_waiver(findings=[stale], reductions=[])]})
        self.assert_policy_error("SEMANTIC_WAIVER_STALE_TARGET")

    def test_file_content_change_invalidates_the_waiver(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        self.target.write_bytes(TARGET_BYTES + b"// changed\n")
        self.assert_policy_error("SEMANTIC_WAIVER_FINGERPRINT")

    def test_unlisted_file_edit_does_not_expand_waiver_scope(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        unrelated = self.root / "sakura_core/util/os.cpp"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_text("void unrelated() {}\n", encoding="utf-8")
        result = self.apply_policy()
        self.assertTrue(result["effective_ok"])
        self.assertEqual([FINDING], result["waiver_review"]["waived"]["new_findings"])

        extra = {**FINDING, "path": "sakura_core/util/os.cpp", "evidence_sha256": "sha256:" + "c" * 64}
        self.comparison["new_findings"] = [FINDING, extra]
        self.comparison["ok"] = False
        result = self.apply_policy()
        self.assertFalse(result["effective_ok"])
        self.assertEqual([extra], result["waiver_review"]["unwaived"]["new_findings"])

    def test_fingerprint_normalizes_lf_crlf_and_cr(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        self.target.write_bytes(TARGET_BYTES.replace(b"\n", b"\r\n"))
        self.assertTrue(self.apply_policy()["effective_ok"])
        self.target.write_bytes(TARGET_BYTES.replace(b"\n", b"\r"))
        self.assertTrue(self.apply_policy()["effective_ok"])

    def test_changed_baseline_is_not_covered(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver()]})
        self.comparison["baseline_source_commit"] = "d" * 40
        self.assert_policy_error("SEMANTIC_WAIVER_BASELINE")

    def test_expired_future_and_overlong_waivers_fail_closed(self) -> None:
        for waiver, code in (
            (_waiver(expires="2026-10-27"), "SEMANTIC_WAIVER_EXPIRED"),
            (_waiver(created="2026-10-29"), "SEMANTIC_WAIVER_FUTURE"),
            (_waiver(expires="2027-01-27"), "SEMANTIC_WAIVER_EXPIRY"),
        ):
            with self.subTest(code=code):
                self.write_ledger({"schema_version": 1, "waivers": [waiver]})
                self.assert_policy_error(code)

        # A valid creation/expiry interval still becomes invalid after expiry.
        self.write_ledger({"schema_version": 1, "waivers": [_waiver(expires="2026-10-30")]})
        with self.assertRaises(BuildError) as raised:
            self.apply_policy(today=date(2026, 10, 31))
        self.assertEqual("SEMANTIC_WAIVER_EXPIRED", raised.exception.code)

    def test_malformed_duplicate_and_path_escape_ledger_fail_closed(self) -> None:
        self.ledger_path.parent.mkdir(parents=True, exist_ok=True)
        self.ledger_path.write_bytes(b"\xff")
        self.assert_policy_error("SEMANTIC_WAIVER_READ")

        self.ledger_path.write_text('{"schema_version":1,"schema_version":1,"waivers":[]}', encoding="utf-8")
        self.assert_policy_error("SEMANTIC_WAIVER_DUPLICATE_KEY")

        self.write_ledger({"schema_version": 1, "waivers": [_waiver(tracking_issue=True)]})
        self.assert_policy_error("SEMANTIC_WAIVER_INVALID")

        escaped = _waiver(files={"../outside.cpp": _file_hash()})
        self.write_ledger({"schema_version": 1, "waivers": [escaped]})
        self.assert_policy_error("SEMANTIC_WAIVER_PATH")

    def test_duplicate_target_and_unknown_schema_fields_are_rejected(self) -> None:
        self.write_ledger({"schema_version": 1, "waivers": [_waiver(), _waiver()]})
        self.assert_policy_error("SEMANTIC_WAIVER_DUPLICATE")

        invalid = _waiver()
        invalid["allow_all_findings"] = True
        self.write_ledger({"schema_version": 1, "waivers": [invalid]})
        self.assert_policy_error("SEMANTIC_WAIVER_INVALID")

        self.write_ledger({"schema_version": 1.0, "waivers": []})
        self.assert_policy_error("SEMANTIC_WAIVER_SCHEMA")


if __name__ == "__main__":
    unittest.main()
