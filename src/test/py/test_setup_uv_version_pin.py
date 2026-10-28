"""Keep CI from selecting an unreviewed uv release at runtime (#390)."""

from __future__ import annotations

import re
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path


WORKFLOWS = Path(__file__).resolve().parents[3] / ".github/workflows"
UV_VERSION = "0.13.5"
# GitHub release 0.13.5, published 2026-10-22T01:35:54Z.
UV_PUBLISHED = datetime(2026, 10, 22, 1, 35, 54, tzinfo=timezone.utc)
STEP = re.compile(r"(?m)^(?P<indent> *)- (?:name|uses|run):")
SETUP_UV = re.compile(r"(?m)^ *-? *uses: *astral-sh/setup-uv@[^\s#]+")


def setup_uv_versions(workflow: str) -> list[str | None]:
    """Read versions only from each setup-uv step, never a neighboring step."""
    boundaries = list(STEP.finditer(workflow))
    versions: list[str | None] = []
    for usage in SETUP_UV.finditer(workflow):
        preceding = [i for i, match in enumerate(boundaries) if match.start() <= usage.start()]
        if not preceding:
            versions.append(None)
            continue
        index = preceding[-1]
        indent = boundaries[index].group("indent")
        following = next(
            (match.start() for match in boundaries[index + 1:] if match.group("indent") == indent),
            len(workflow),
        )
        step = workflow[boundaries[index].start():following]
        with_indent = len(indent) + 2
        with_block = re.search(
            rf"(?ms)^ {{{with_indent}}}with:[ \t]*$(.*?)(?=^ {{{with_indent}}}[^ \n]|\Z)", step
        )
        found = [] if with_block is None else re.findall(
            rf"(?m)^ {{{with_indent + 2}}}version: *['\"]?([^'\"\s#]+)['\"]? *$",
            with_block.group(1),
        )
        versions.append(found[0] if len(found) == 1 else None)
    return versions


class SetupUvVersionPinTests(unittest.TestCase):
    def test_every_setup_uses_the_same_reviewed_version(self) -> None:
        versions: list[tuple[str, str | None]] = []
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            versions.extend((path.name, version) for version in setup_uv_versions(path.read_text(encoding="utf-8-sig")))
        self.assertTrue(versions, "no setup-uv steps were inspected")
        for filename, version in versions:
            with self.subTest(workflow=filename):
                self.assertEqual(UV_VERSION, version)

    def test_reviewed_version_is_at_least_three_days_old(self) -> None:
        self.assertGreaterEqual(datetime.now(timezone.utc) - UV_PUBLISHED, timedelta(days=3))

    def test_missing_or_neighboring_version_is_not_a_pin(self) -> None:
        workflow = """\
    - name: Install uv
      uses: astral-sh/setup-uv@abc
      with:
        activate-environment: true
      env:
        version: '0.13.5'
    - name: Another action
      with:
        version: '0.13.5'
"""
        self.assertEqual([None], setup_uv_versions(workflow))


if __name__ == "__main__":
    unittest.main()
