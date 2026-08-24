"""Contracts for the main-only CI event path (#166)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = REPO_ROOT / ".github/workflows"
BUILD_WORKFLOW = WORKFLOWS / "build-sakura.yml"
TARGET_POLICY = WORKFLOWS / "pr-target-policy.yml"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _trigger_block(text: str) -> str:
    match = re.search(r"(?m)^on:\n(?P<body>(?:[ \t]+.*\n|\n)*)", text)
    if match is None:
        raise AssertionError("workflow has no top-level on: block")
    return match.group("body")


def _trigger_section(text: str, name: str) -> str:
    block = _trigger_block(text)
    sections = list(re.finditer(r"(?m)^  (?P<name>[a-z_]+):[ \t]*\n", block))
    for index, section in enumerate(sections):
        if section.group("name") != name:
            continue
        end = sections[index + 1].start() if index + 1 < len(sections) else len(block)
        return block[section.end() : end]
    raise AssertionError(f"workflow has no {name!r} trigger")


class MainOnlyTriggerTests(unittest.TestCase):
    def test_build_workflow_pushes_only_main(self) -> None:
        push = _trigger_section(_read(BUILD_WORKFLOW), "push")
        self.assertRegex(push, r"(?m)^    branches:\n      - main\n")
        self.assertNotRegex(push, r"(?m)^      - develop\s*$")

    def test_develop_does_not_reappear_as_a_push_path(self) -> None:
        text = _read(BUILD_WORKFLOW)
        self.assertNotIn("github.ref_name == 'develop'", text)


class MainOnlyTargetPolicyTests(unittest.TestCase):
    def test_all_pull_requests_target_main_and_same_repository_branches(self) -> None:
        text = _read(TARGET_POLICY)
        self.assertIn('echo "::error::All pull requests must target main."', text)
        self.assertIn(
            'echo "::error::Only same-repository branches may target main."',
            text,
        )
        self.assertNotIn("must target develop", text)
        self.assertNotIn("hotfix/*", text)


if __name__ == "__main__":
    unittest.main()
