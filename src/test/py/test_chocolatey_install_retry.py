"""Contracts for the bounded Chocolatey install retry (#114).

``community.chocolatey.org`` answered 503 for one job during PR #114 and failed
a required check in under a minute. Nothing was wrong with the repository, the
package, or the pinned version; the feed was briefly unavailable. Because the
tool install sits ahead of MSBuild in the required ``build`` job, that single
bad answer cost a full re-run of the most expensive job in the matrix.

Every Chocolatey install therefore goes through
``.github/scripts/Install-ChocolateyPackage.ps1``, which retries a bounded
number of times with exponential backoff and reports each attempt to the run
log. The invariants pinned here are the ones that make the retry honest rather
than a way to hide a broken package:

* the attempt count is bounded and validated, so a feed that is genuinely down
  cannot spin the job for its whole timeout;
* the final attempt raises, so a package that has stopped resolving still turns
  the job red;
* each retry is announced, so a feed that needs retrying is visible in the run
  summary instead of silently absorbed;
* no workflow calls ``choco install`` directly, which is the one-line change
  that would quietly reintroduce the original fragility.

Like ``test_develop_issue_closure.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
HELPER = REPO_ROOT / ".github/scripts/Install-ChocolateyPackage.ps1"
WORKFLOW_DIR = REPO_ROOT / ".github/workflows"

HELPER_INVOCATION = r".\.github\scripts\Install-ChocolateyPackage.ps1"

# Each call site and the version it pins. The versions are deliberate: they keep
# ctags.cmake and diffutils.cmake on the "copy the system binary" path.
EXPECTED_CALL_SITES = {
    "build-sakura.yml": {"universal-ctags": "2022.6.5", "diffutils": "2.8.7"},
    "listings-ab-benchmark.yml": {"universal-ctags": "2022.6.5", "diffutils": "2.8.7"},
}

CALL_SITE_RE = re.compile(
    re.escape(HELPER_INVOCATION) + r" -PackageId (?P<id>\S+) -Version (?P<version>\S+)"
)

# A direct install in a workflow bypasses the retry entirely.
DIRECT_INSTALL_RE = re.compile(r"^\s*(?!#).*\bchoco\s+install\b", re.MULTILINE)


def _helper_bytes() -> bytes:
    return HELPER.read_bytes()


def _helper_text() -> str:
    # .gitattributes pins *.ps1 to UTF-16LE in the working tree; Git stores UTF-8.
    # Decode the bytes rather than read_text: universal newlines would rewrite
    # the very line endings this file exists to pin.
    return _helper_bytes().decode("utf-16")


def _workflows() -> list[Path]:
    return sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))


class HelperEncodingTests(unittest.TestCase):
    def test_matches_the_repository_powershell_encoding(self) -> None:
        raw = _helper_bytes()
        self.assertEqual(
            raw[:2],
            b"\xff\xfe",
            "repository PowerShell is UTF-16LE with BOM in the working tree",
        )
        text = _helper_text()
        self.assertEqual(
            text.count("\n"),
            text.count("\r\n"),
            "every line ending must be CRLF, per .gitattributes",
        )


class HelperContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _helper_text()

    def test_attempts_are_bounded_and_validated(self) -> None:
        self.assertRegex(
            self.text,
            r"\[ValidateRange\(1, 10\)\]\s*\r?\n\s*\[int\]\$MaxAttempts = 3",
            "an unbounded or unvalidated attempt count lets a dead feed hold "
            "the runner until the job times out",
        )
        self.assertIn(
            "for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++)",
            self.text,
            "the retry must be a bounded loop, not an open-ended wait",
        )

    def test_the_last_attempt_fails_the_job(self) -> None:
        self.assertRegex(
            self.text,
            r"if \(\$attempt -eq \$MaxAttempts\) \{\s*\r?\n\s*throw ",
            "a package that has genuinely stopped resolving must still turn "
            "the job red rather than being absorbed by the retry",
        )

    def test_every_retry_is_announced(self) -> None:
        self.assertIn(
            "::warning::choco install",
            self.text,
            "a silent retry hides a feed that is degrading",
        )
        self.assertIn(
            "::notice::Installed",
            self.text,
            "a job that only succeeded on a later attempt should say so",
        )

    def test_backoff_grows_between_attempts(self) -> None:
        self.assertIn("$delaySeconds = $delaySeconds * 2", self.text)
        self.assertIn("Start-Sleep -Seconds ($delaySeconds + $jitterSeconds)", self.text)

    def test_a_completed_install_awaiting_reboot_counts_as_success(self) -> None:
        # Chocolatey returns 1641/3010 when the install finished but Windows
        # wants a reboot. The runner is discarded with the job, so both are done.
        self.assertIn("$succeeded = @(0, 1641, 3010)", self.text)
        self.assertIn("if ($succeeded -contains $exitCode)", self.text)

    def test_the_exit_code_drives_the_decision(self) -> None:
        # PowerShell 7.3+ can raise on a non-zero native exit code, which would
        # pre-empt the loop before it ever inspects the code.
        self.assertIn("$PSNativeCommandUseErrorActionPreference = $false", self.text)
        self.assertIn("$exitCode = $LASTEXITCODE", self.text)


class WorkflowCallSiteTests(unittest.TestCase):
    def test_every_workflow_install_uses_the_helper(self) -> None:
        for workflow in _workflows():
            with self.subTest(workflow=workflow.name):
                direct = DIRECT_INSTALL_RE.findall(
                    workflow.read_text(encoding="utf-8-sig")
                )
                self.assertEqual(
                    direct,
                    [],
                    f"{workflow.name} calls choco install directly, which "
                    "bypasses the bounded retry",
                )

    def test_the_known_call_sites_still_pin_their_versions(self) -> None:
        for name, expected in EXPECTED_CALL_SITES.items():
            with self.subTest(workflow=name):
                text = (WORKFLOW_DIR / name).read_text(encoding="utf-8-sig")
                found = {
                    match.group("id"): match.group("version")
                    for match in CALL_SITE_RE.finditer(text)
                }
                self.assertEqual(found, expected)


if __name__ == "__main__":
    unittest.main()
