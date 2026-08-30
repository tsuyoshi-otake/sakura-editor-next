"""Executable release-note formatting contracts for Issue #279."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
FORMATTER = REPO_ROOT / ".github" / "scripts" / "Format-ReleasePromotionNotes.ps1"
PUBLISHER = REPO_ROOT / ".github" / "scripts" / "Publish-ReleasePromotion.ps1"
SOURCE_TAG = "v3.1.0-build.8001"
SOURCE_SHA = "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
FILE_VERSION = "3.1.0.8001"
CHECK_NAMES = (
    "zeta source gate",
    "alpha source gate",
    "middle source gate",
)
EXPECTED_BODY = "\n".join(
    (
        "## Verified release promotion",
        "",
        f"- Source tag: `{SOURCE_TAG}`",
        f"- Source SHA: `{SOURCE_SHA.lower()}`",
        f"- File version: `{FILE_VERSION}`",
        "",
        "The source SHA passed the required main-branch checks before packaging. "
        "The packaged installer was provenance-checked, installed into an isolated "
        "directory, opened with a real document, and uninstalled before this release "
        "was published.",
        "",
        "Required source checks:",
        *(f"- {name}" for name in CHECK_NAMES),
    )
)


def _read_powershell(path: Path) -> str:
    raw = path.read_bytes()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig")


def _powershell_hosts() -> tuple[str, str]:
    hosts = tuple(filter(None, (shutil.which("powershell.exe"), shutil.which("pwsh.exe"))))
    if len(hosts) != 2:
        raise AssertionError("release-note fixtures require both powershell.exe and pwsh.exe")
    return hosts  # type: ignore[return-value]


def _run_formatter(host: str, checks: object) -> subprocess.CompletedProcess[str]:
    fixture = """\
$ErrorActionPreference = 'Stop'
. $env:RELEASE_NOTES_FORMATTER
$checks = $env:RELEASE_NOTES_CHECKS | ConvertFrom-Json
$body = Format-ReleasePromotionNotes `
    -SourceTag $env:RELEASE_NOTES_TAG `
    -SourceSha $env:RELEASE_NOTES_SHA `
    -FileVersion $env:RELEASE_NOTES_VERSION `
    -RequiredChecks $checks
[IO.File]::WriteAllText(
    $env:RELEASE_NOTES_OUTPUT,
    $body,
    [Text.UTF8Encoding]::new($false)
)
"""
    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary = Path(temporary_directory)
        fixture_path = temporary / "format-release-notes-fixture.ps1"
        output_path = temporary / "release-notes.md"
        fixture_path.write_text(fixture, encoding="utf-8")
        environment = os.environ.copy()
        environment.update(
            {
                "RELEASE_NOTES_FORMATTER": str(FORMATTER),
                "RELEASE_NOTES_CHECKS": json.dumps(checks, separators=(",", ":")),
                "RELEASE_NOTES_TAG": SOURCE_TAG,
                "RELEASE_NOTES_SHA": SOURCE_SHA,
                "RELEASE_NOTES_VERSION": FILE_VERSION,
                "RELEASE_NOTES_OUTPUT": str(output_path),
            }
        )
        completed = subprocess.run(
            (host, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(fixture_path)),
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if output_path.exists():
            completed.formatted_body = output_path.read_text(encoding="utf-8").replace("\r\n", "\n")  # type: ignore[attr-defined]
        return completed


class ReleasePromotionNotesExecutableTests(unittest.TestCase):
    def test_both_powershell_hosts_emit_the_exact_flat_body(self) -> None:
        checks = [{"name": name, "status": "completed"} for name in CHECK_NAMES]
        for host in _powershell_hosts():
            with self.subTest(host=host):
                completed = _run_formatter(host, checks)
                self.assertEqual(0, completed.returncode, completed.stderr)
                body = completed.formatted_body  # type: ignore[attr-defined]
                self.assertEqual(EXPECTED_BODY, body)
                self.assertNotIn("System.Object[]", body)
                for name in CHECK_NAMES:
                    self.assertEqual(1, body.splitlines().count(f"- {name}"))
                offsets = [body.index(f"- {name}") for name in CHECK_NAMES]
                self.assertEqual(sorted(offsets), offsets, "recorded check order must be preserved")

    def test_both_powershell_hosts_reject_empty_and_malformed_evidence(self) -> None:
        malformed = (
            [],
            {"name": "not a collection"},
            [{}],
            [{"name": ""}],
            [{"name": ["nested"]}],
            [{"name": "line\nbreak"}],
            [{"name": "duplicate"}, {"name": "duplicate"}],
        )
        for host in _powershell_hosts():
            for checks in malformed:
                with self.subTest(host=host, checks=checks):
                    completed = _run_formatter(host, checks)
                    self.assertNotEqual(0, completed.returncode)


class ReleasePromotionNotesStaticContracts(unittest.TestCase):
    def test_formatter_has_no_network_or_release_side_effects(self) -> None:
        source = _read_powershell(FORMATTER)
        for forbidden in (" gh ", "Invoke-RestMethod", "Invoke-WebRequest", "release create"):
            self.assertNotIn(forbidden, source)

    def test_publisher_finishes_notes_before_any_draft_creation(self) -> None:
        source = _read_powershell(PUBLISHER)
        formatting = source.find("$notes = Format-ReleasePromotionNotes")
        draft_creation = source.find("gh api --method POST")
        self.assertGreaterEqual(formatting, 0)
        self.assertGreaterEqual(draft_creation, 0)
        self.assertLess(formatting, draft_creation)
        self.assertIn("body = $notes", source)
        self.assertNotIn("($requiredCheckNames | ForEach-Object", source)


if __name__ == "__main__":
    unittest.main()
