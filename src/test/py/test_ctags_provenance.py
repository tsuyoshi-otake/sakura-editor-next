"""Contracts for the ctags build the release artifacts actually ship.

``build-installer.bat`` and ``zipArtifacts.bat`` both extract ``license/`` and
``docs/`` from ``installer/externals/universal-ctags/ctags-<version>-<arch>.zip``
and ship them beside ``ctags.exe``. ``ctags.exe`` itself used to come from
whatever ``find_program(ctags)`` resolved first -- in CI, a Chocolatey
``universal-ctags 2022.6.5`` installed a few steps earlier -- so the released
installer and ZIP carried a 2022.6.5 binary under a different version's license
set and documentation, and a required job sat behind a third-party package feed
that answered 503 during #114 and 504 during #123.

``ctags.cmake`` now stages that same committed archive by default, which makes
the three references to it one decision instead of three. The invariants pinned
here are the ones that keep it that way:

* the version ``ctags.cmake`` extracts is the version whose license and
  documentation the packaging scripts ship, and an archive exists for it;
* ``SAKURA_CTAGS_SOURCE`` still defaults to the archive, so that agreement is
  what a build does rather than what it could be asked to do;
* no workflow installs the tool from a package feed again, which is the
  one-line change that would silently restore both problems at once.

Like ``test_develop_issue_closure.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CTAGS_CMAKE = REPO_ROOT / "src/main/cmake/ctags.cmake"
BUILD_INSTALLER = REPO_ROOT / "build-installer.bat"
ZIP_ARTIFACTS = REPO_ROOT / "zipArtifacts.bat"
ARCHIVE_DIR = REPO_ROOT / "installer/externals/universal-ctags"
WORKFLOW_DIR = REPO_ROOT / ".github/workflows"

# The architectures the committed archive set has to cover. Anything else falls
# back to a system or source-built ctags, which ctags.cmake reports.
PACKAGED_ARCHITECTURES = ("x86", "x64")

CMAKE_VERSION_RE = re.compile(r'set\(CTAGS_VERSION\s+"(?P<version>[^"]+)"\)')
# Both batch files spell the archive out; read the version back from the name
# they use rather than from a separate declaration that could drift from it.
BATCH_ARCHIVE_RE = re.compile(
    r"universal-ctags\\ctags-(?P<version>v[0-9][0-9.]*)-", re.IGNORECASE
)

# Either spelling puts a required job back behind community.chocolatey.org.
# Anchor the comment exclusion at the start of the line: `^\s*(?!#)` backtracks
# to one fewer leading space and matches the commented-out line it excludes.
CHOCOLATEY_RE = re.compile(
    r"^(?!\s*#).*(\bchoco\s+install\b|Install-ChocolateyPackage\.ps1)", re.MULTILINE
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _workflows() -> list[Path]:
    return sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))


class CtagsVersionAgreementTests(unittest.TestCase):
    def setUp(self) -> None:
        match = CMAKE_VERSION_RE.search(_read(CTAGS_CMAKE))
        self.assertIsNotNone(match, "ctags.cmake must declare CTAGS_VERSION")
        assert match is not None
        self.version = match.group("version")

    def test_the_packaging_scripts_ship_the_staged_version(self) -> None:
        for script in (BUILD_INSTALLER, ZIP_ARTIFACTS):
            with self.subTest(script=script.name):
                found = {
                    match.group("version")
                    for match in BATCH_ARCHIVE_RE.finditer(_read(script))
                }
                self.assertEqual(
                    found,
                    {self.version},
                    f"{script.name} ships license/documentation from a ctags "
                    f"archive that ctags.cmake does not stage the binary from; "
                    f"the installed ctags.exe would then run under another "
                    f"version's license set",
                )

    def test_an_archive_exists_for_every_packaged_architecture(self) -> None:
        for arch in PACKAGED_ARCHITECTURES:
            with self.subTest(arch=arch):
                archive = ARCHIVE_DIR / f"ctags-{self.version}-{arch}.zip"
                self.assertTrue(
                    archive.is_file(),
                    f"{archive.name} is missing, so a {arch} build silently "
                    f"falls back to a system or source-built ctags",
                )


class CtagsProviderDefaultTests(unittest.TestCase):
    def test_the_committed_archive_is_the_default_provider(self) -> None:
        self.assertRegex(
            _read(CTAGS_CMAKE),
            r'set\(SAKURA_CTAGS_SOURCE_DEFAULT\s+"archive"\)',
            "a different default makes the version agreement above optional",
        )


class WorkflowPackageFeedTests(unittest.TestCase):
    def test_no_workflow_installs_a_tool_from_chocolatey(self) -> None:
        for workflow in _workflows():
            with self.subTest(workflow=workflow.name):
                self.assertEqual(
                    CHOCOLATEY_RE.findall(_read(workflow)),
                    [],
                    f"{workflow.name} installs a build tool from "
                    f"community.chocolatey.org; ctags comes from the committed "
                    f"archive and diff.exe is not staged in CI, so a feed "
                    f"outage should not be able to fail a required job",
                )


if __name__ == "__main__":
    unittest.main()
