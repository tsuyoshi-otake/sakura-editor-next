"""Repository-wide contract: every ``actions/cache*`` reference shares one major (#151).

``actions/cache/restore`` and ``actions/cache/save`` are the two halves of one
cache entry, and nothing guarantees that an entry written by one major version
can be read by another.  If a dependency update moves only one side, the
affected job becomes a permanent cold run and nothing turns red.

The per-feature cache contracts (``test_cppcheck_analyzer_cache.py``,
``test_package_closure_cache.py``, ``test_vcpkg_tool_cache.py``) deliberately
assert only which action runs, never which release, so that a routine
Dependabot bump does not fail tests that say nothing about versions.  This file
owns the one version invariant that does exist.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GITHUB_DIR = REPO_ROOT / ".github"

# Matches ``actions/cache``, ``actions/cache/restore`` and ``actions/cache/save``
# pinned by tag (``@v6``, ``@v6.1.0``) or by commit with a version comment
# (``@<sha> # v6.1.0``).
CACHE_USES_RE = re.compile(
    r"uses:\s*actions/cache(?:/restore|/save)?@(?P<ref>[^\s#]+)(?:\s*#\s*(?P<comment>\S+))?"
)
MAJOR_RE = re.compile(r"^v(?P<major>\d+)(?:\.\d+)*$")


def _cache_references() -> list[tuple[str, int, str]]:
    references: list[tuple[str, int, str]] = []
    for path in sorted(GITHUB_DIR.rglob("*")):
        if path.suffix not in (".yml", ".yaml") or not path.is_file():
            continue
        relative = path.relative_to(REPO_ROOT).as_posix()
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = CACHE_USES_RE.search(line)
            if match is None:
                continue
            version = match.group("ref")
            if MAJOR_RE.match(version) is None and match.group("comment"):
                version = match.group("comment")
            references.append((relative, number, version))
    return references


class ActionsCacheMajorVersionTests(unittest.TestCase):
    def test_every_reference_names_a_resolvable_major_version(self) -> None:
        references = _cache_references()
        self.assertTrue(references, "no actions/cache references found under .github")
        unresolved = [ref for ref in references if MAJOR_RE.match(ref[2]) is None]
        self.assertEqual(
            unresolved,
            [],
            "pin actions/cache by a vX tag, or by commit with a '# vX.Y.Z' comment",
        )

    def test_restore_and_save_share_one_major_version(self) -> None:
        references = _cache_references()
        majors = {
            MAJOR_RE.match(version).group("major")
            for _path, _line, version in references
            if MAJOR_RE.match(version) is not None
        }
        self.assertEqual(
            len(majors),
            1,
            "actions/cache references span several majors: "
            + ", ".join(f"{path}:{line} {version}" for path, line, version in references),
        )


if __name__ == "__main__":
    unittest.main()
