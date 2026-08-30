"""Fail-closed release-source check contracts for Issue #279.

The resolver's ``-SelfTest`` path evaluates synthetic check runs before any
Git, GitHub API, or filesystem output work.  These tests keep that executable
decision and the nine recorded source contexts visible to the CI-collected
Python suite.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
RESOLVER = REPO_ROOT / ".github/scripts/Resolve-ReleasePromotion.ps1"
EXPECTED_CHECKS = [
    "check-encoding",
    "MSBuild (Debug, x64)",
    "MSBuild (Release, x64)",
    "cppcheck (x64, Release)",
    "doxygen (x64, Release)",
    "architecture-gates",
    "mingw (Debug)",
    "mingw (Release)",
    "Audit locked Rust dependencies",
]
TIMESTAMP_GUARD = r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,7})?Z$"
VALID_FRACTIONAL_TIMESTAMPS = (
    "2026-08-30T00:00:00.1Z",
    "2026-08-30T00:00:00.1234567Z",
)
INVALID_TIMESTAMP_CASES = (
    "2026-08-30T00:00:00.Z",
    "2026-08-30T00:00:00.12345678Z",
    "2026-08-30T00:00:00+00:00",
    "2026-08-30T00:00:00z",
    "2026-08-30T00:00:00Z ",
    " 2026-08-30T00:00:00Z",
    "2026-02-30T00:00:00Z",
    "2026-08-30T24:00:00Z",
)


class ReleasePromotionSourceCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RESOLVER.read_text(encoding="utf-16")

    def test_required_trusted_main_checks_are_exact_and_ordered(self) -> None:
        start = self.text.index("function Get-RequiredSourceCheckNames")
        end = self.text.index("function Resolve-RequiredSourceChecks", start)
        declared = re.findall(r"^\s*'([^']+)'[,]?\s*$", self.text[start:end], re.MULTILINE)

        self.assertEqual(EXPECTED_CHECKS, declared)
        self.assertEqual(len(declared), len(set(declared)))
        self.assertIn("$githubActionsAppId = 15368", self.text)
        self.assertIn("required_checks = @($verifiedChecks)", self.text)

    def test_resolution_is_pure_and_fail_closed(self) -> None:
        start = self.text.index("function Resolve-RequiredSourceChecks")
        end = self.text.index("function Invoke-SourceCheckSelfTest", start)
        evaluator = self.text[start:end]

        self.assertNotIn("Invoke-RestMethod", evaluator)
        self.assertNotIn("Invoke-Git", evaluator)
        self.assertIn(
            "$validatedRuns = @(ConvertTo-ValidatedCheckRuns -CheckRuns $CheckRuns `",
            evaluator,
        )
        self.assertIn("-Repository $Repository -SourceSha $SourceSha", evaluator)
        self.assertIn("$_.name -ceq $requiredCheckName", evaluator)
        self.assertIn("$_.app.id -eq $TrustedAppId", evaluator)
        self.assertIn("$_.status -cne 'completed'", evaluator)
        self.assertIn("$_.status -ceq 'completed' -and $_.conclusion -ceq 'success'", evaluator)
        self.assertIn("$_.started_at_utc", evaluator)

        validation_start = self.text.index("function ConvertTo-ValidatedCheckRuns")
        validation_end = self.text.index("function Resolve-RequiredSourceChecks", validation_start)
        validation = self.text[validation_start:validation_end]
        for contract in (
            "ConvertFrom-PositiveNativeInteger",
            "ConvertFrom-StrictUtcTimestamp",
            "head_sha must exactly match",
            "check_suite id",
            "github-actions",
            "must be an absolute HTTPS URL",
            "canonical Actions job URL",
            "completed conclusion must be an exact supported string",
            "non-completed run must have null conclusion and completed_at",
            "external_id UUID",
            "started_at must not be later than completed_at",
        ):
            self.assertIn(contract, validation)

    def test_selftest_precedes_all_external_work(self) -> None:
        selftest_exit = self.text.index("if ($SelfTest)")
        self.assertLess(selftest_exit, self.text.index("$tagMatch = [regex]::Match"))
        self.assertLess(selftest_exit, self.text.index("Invoke-Git -Arguments"))
        self.assertLess(selftest_exit, self.text.index("Invoke-RestMethod"))

        for contract in (
            "all nine trusted checks must be recorded",
            "no run from trusted GitHub Actions app 15368",
            "non-completed run(s)",
            "has no completed success",
            "the newest successful trusted run must be selected deterministically",
            "unrelated checks must not change the evidence set",
            "native positive integer",
            "head_sha",
            "check_suite",
            "external_id",
            "Test Results",
            "completed_at",
            "must not be later than completed_at",
        ):
            self.assertIn(contract, self.text)

    def test_timestamp_grammar_is_ascii_bounded_and_selftested(self) -> None:
        guard = f"$Value -cnotmatch '{TIMESTAMP_GUARD}'"
        guard_index = self.text.index(guard)
        parse_index = self.text.index("[DateTimeOffset]::TryParseExact", guard_index)
        self.assertLess(guard_index, parse_index)

        for timestamp in VALID_FRACTIONAL_TIMESTAMPS + INVALID_TIMESTAMP_CASES:
            self.assertIn(
                f"'{timestamp}'",
                self.text,
                msg=f"timestamp case is missing from the executable self-test: {timestamp}",
            )
        self.assertIn("$missingStartedAt.PSObject.Properties.Remove('started_at')", self.text)
        self.assertIn("$null,", self.text)
        self.assertIn("'must not be later than completed_at'", self.text)

    def test_no_network_selftest_passes_on_every_available_powershell_host(self) -> None:
        hosts: list[str] = []
        for candidate in ("powershell.exe", "pwsh.exe", "powershell", "pwsh"):
            resolved = shutil.which(candidate)
            if resolved is not None and resolved.casefold() not in {item.casefold() for item in hosts}:
                hosts.append(resolved)
        if not hosts:
            self.skipTest("PowerShell is unavailable")

        for host in hosts:
            with self.subTest(host=host):
                completed = subprocess.run(
                    [
                        host,
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        str(RESOLVER),
                        "-SelfTest",
                    ],
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                self.assertEqual(
                    0,
                    completed.returncode,
                    msg=f"{host} self-test failed:\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
                )
                self.assertIn(
                    "Resolve-ReleasePromotion source-check self-test passed.",
                    completed.stdout,
                )


if __name__ == "__main__":
    unittest.main()
