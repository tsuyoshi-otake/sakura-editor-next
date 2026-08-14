"""Executable contracts for the fail-closed main pull-request CI planner."""

from __future__ import annotations

import sys
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_BUILD = REPO_ROOT / "tools/build"

if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.ci_plan import (  # noqa: E402
    CiChangedFile,
    CiPlanError,
    changed_files_between,
    plan_ci,
)


BASE_SHA = "a" * 40
HEAD_SHA = "b" * 40
REPOSITORY = "tsuyoshi-otake/sakura-editor-next"


def decide(*changes: CiChangedFile | str, event_name: str = "pull_request") -> dict[str, object]:
    return plan_ci(
        changes,
        event_name=event_name,
        repository=REPOSITORY,
        head_repository=REPOSITORY,
        base_sha=BASE_SHA if event_name == "pull_request" else None,
        head_sha=HEAD_SHA if event_name == "pull_request" else None,
    )


class CiPlanDecisionTests(unittest.TestCase):
    def assertFull(self, result: dict[str, object], reason: str) -> None:
        self.assertEqual(result["mode"], "full_native")
        self.assertEqual(result["reason_codes"], [reason])
        self.assertEqual(result["jobs"], {"check_encoding": True, "native_build": True})

    def test_markdown_only_pull_request_omits_native_build(self) -> None:
        result = decide("README.md", "sakura_core/workbench/CLAUDE.md")
        self.assertEqual(result["mode"], "docs_only")
        self.assertEqual(result["reason_codes"], ["documentation_only"])
        self.assertEqual(result["jobs"], {"check_encoding": True, "native_build": False})

    def test_formal_documents_and_evidence_are_documentation(self) -> None:
        result = decide(
            "docs/formal/ControlStartupHandshake.tla",
            "docs/formal/ControlStartupHandshake_Current.cfg",
            "docs/evidence/結果 1.json",
        )
        self.assertEqual(result["mode"], "docs_only")

    def test_native_or_unknown_change_is_full(self) -> None:
        self.assertFull(decide("sakura_core/foo.cpp"), "native_or_unknown_change")
        self.assertFull(decide("unexpected.data"), "native_or_unknown_change")

    def test_adding_any_non_document_change_can_only_strengthen_the_plan(self) -> None:
        docs = decide("README.md")
        mixed = decide("README.md", "sakura_core/foo.cpp")
        strength = {"docs_only": 0, "full_native": 1}
        self.assertGreaterEqual(strength[str(mixed["mode"])], strength[str(docs["mode"])])

    def test_delete_rename_copy_and_type_change_are_full(self) -> None:
        for status in ("D", "R100", "C100", "T"):
            with self.subTest(status=status):
                self.assertFull(
                    decide(CiChangedFile("docs/README.md", status)),
                    "native_or_unknown_change",
                )

    def test_empty_diff_and_fork_are_full(self) -> None:
        self.assertFull(decide(), "no_changed_files")
        result = plan_ci(
            ["README.md"],
            event_name="pull_request",
            repository=REPOSITORY,
            head_repository="someone/fork",
            base_sha=BASE_SHA,
            head_sha=HEAD_SHA,
        )
        self.assertFull(result, "untrusted_or_unknown_head_repository")

    def test_main_push_and_other_non_pr_events_are_always_full(self) -> None:
        for event_name in ("push", "workflow_dispatch", "workflow_call"):
            with self.subTest(event_name=event_name):
                self.assertFull(decide("README.md", event_name=event_name), "non_pull_request_event")

    def test_invalid_sha_and_escaping_path_are_rejected(self) -> None:
        with self.assertRaisesRegex(CiPlanError, "base SHA"):
            plan_ci(
                ["README.md"],
                event_name="pull_request",
                repository=REPOSITORY,
                head_repository=REPOSITORY,
                base_sha="not-a-sha",
                head_sha=HEAD_SHA,
            )
        with self.assertRaisesRegex(CiPlanError, "repository-relative"):
            decide("../README.md")


class CiPlanGitDiffTests(unittest.TestCase):
    def run_git(self, repo: Path, *arguments: str) -> str:
        completed = subprocess.run(
            ("git", "-c", "user.name=CI Plan Test", "-c", "user.email=ci-plan@example.invalid", *arguments),
            cwd=repo,
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()

    def test_exact_base_head_diff_preserves_spaces_unicode_and_status(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            repo = Path(raw_directory)
            self.run_git(repo, "init", "--initial-branch=main")
            document = repo / "docs" / "設計 メモ.md"
            document.parent.mkdir()
            document.write_text("before\n", encoding="utf-8")
            self.run_git(repo, "add", "--", document.relative_to(repo).as_posix())
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "base")
            base = self.run_git(repo, "rev-parse", "HEAD")

            document.write_text("after\n", encoding="utf-8")
            self.run_git(repo, "add", "--", document.relative_to(repo).as_posix())
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "head")
            head = self.run_git(repo, "rev-parse", "HEAD")

            self.assertEqual(
                changed_files_between(repo, base, head),
                (CiChangedFile("docs/設計 メモ.md", "M"),),
            )

    def test_rename_status_cannot_be_misclassified_as_documentation_only(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            repo = Path(raw_directory)
            self.run_git(repo, "init", "--initial-branch=main")
            original = repo / "README.md"
            renamed = repo / "docs" / "README.md"
            original.write_text("documentation\n", encoding="utf-8")
            self.run_git(repo, "add", "README.md")
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "base")
            base = self.run_git(repo, "rev-parse", "HEAD")

            renamed.parent.mkdir()
            self.run_git(repo, "mv", "README.md", "docs/README.md")
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "head")
            head = self.run_git(repo, "rev-parse", "HEAD")

            changes = changed_files_between(repo, base, head)
            self.assertEqual(changes, (CiChangedFile("docs/README.md", "R100"),))
            decision = decide(*changes)
            self.assertEqual(decision["mode"], "full_native")
            self.assertEqual(decision["reason_codes"], ["native_or_unknown_change"])


if __name__ == "__main__":
    unittest.main()
