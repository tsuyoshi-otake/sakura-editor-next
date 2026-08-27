from __future__ import annotations

import json
import shutil
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools/verify-native-rust-incremental.ps1"


def _powershell_hosts() -> list[str]:
    return [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]


class NativeRustIncrementalVerifierTests(unittest.TestCase):
    def test_script_is_utf16le_bom_crlf(self) -> None:
        raw = SCRIPT.read_bytes()
        self.assertGreaterEqual(len(raw), 2)
        self.assertEqual(b"\xff\xfe", raw[:2])
        text = raw[2:].decode("utf-16le")
        self.assertNotIn("\r\r\n", text)
        self.assertNotIn("\n", text.replace("\r\n", ""))
        self.assertIn("BuildSakuraNativeFfi", text)
        self.assertIn("cargo-preflight", text)
        self.assertIn("unexpected_consumer", text)
        self.assertIn("missing_output", text)
        self.assertIn("survivor", text)
        self.assertIn("/verbosity:diagnostic", text)
        self.assertIn("/flp:logfile=", text)
        self.assertIn("Get-DiagnosticLogSummary", text)
        self.assertIn("diagnosticsParseFailed", text)
        self.assertIn("errorCodesTruncated", text)
        self.assertIn("MaxRetainedActions = 256", text)
        self.assertIn("MaxProcessFailureRecords = 64", text)
        self.assertIn("MaxProcessFailureCodes = 32", text)
        self.assertIn("actionsTruncated", text)
        self.assertIn("type = 'build_failed'", text)
        self.assertIn("StreamReader", text)
        self.assertNotIn("[IO.File]::ReadAllLines($LogPath)", text)
        self.assertIn("Get-ProcessOutputMetadata", text)
        self.assertIn("Get-OwnedJobMemberDisposition", text)
        self.assertIn("CREATE_SUSPENDED", text)
        self.assertIn("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE", text)
        self.assertIn("PROC_THREAD_ATTRIBUTE_JOB_LIST", text)
        self.assertIn("PROC_THREAD_ATTRIBUTE_HANDLE_LIST", text)
        self.assertIn("UpdateProcThreadAttribute", text)
        self.assertIn("IsProcessInJob", text)
        self.assertIn("QueryFullProcessImageNameW", text)
        self.assertIn("DisposeVerified", text)
        self.assertNotIn("AssignProcessToJobObject", text)
        self.assertIn("TerminateJobObject", text)
        self.assertIn("externalSentinelPreserved", text)
        self.assertIn("Get-OwnedJobMemberRecords", text)
        self.assertNotIn("Stop-Process -Id", text)
        self.assertIn("-AllowedPostExitHelperNames @('mspdbsrv.exe')", text)
        self.assertIn("[string] $WorkspaceRoot = 'build/tmp/nri'", text)
        self.assertIn("[AllowEmptyString()][string] $Line", text)
        self.assertIn("$share = [IO.FileShare]::Read", text)
        self.assertIn("Assert-NoDirectoryReparsePointsBelow", text)
        self.assertIn("$isReparsePoint -and $isDirectory", text)
        self.assertIn("if ($isReparsePoint) { continue }", text)
        self.assertNotIn("@('submodule', 'deinit'", text)
        self.assertIn("shared_checkout_audit", text)
        self.assertIn("shared checkout fingerprint mismatch is not typed", text)
        self.assertIn("submoduleSha256", text)
        self.assertIn("initializedSubmoduleCount", text)
        self.assertIn("Get-StringSequenceSha256 $submoduleDiff", text)
        self.assertIn("Get-StringSequenceSha256 $submoduleLocalConfig", text)
        self.assertIn("Get-WorktreeRegistrationState", text)
        self.assertIn("worktree registration output omitted the invoking checkout", text)
        self.assertIn("New-SchemaFailureEvidence", text)
        self.assertIn("New-EmergencyEvidenceEnvelope", text)
        self.assertIn("EVIDENCE_SCHEMA_SHARED_CHECKOUT", text)
        self.assertIn("schemaValidation", text)
        self.assertIn("workspaceSetup", text)
        self.assertIn("bounded_phase_metadata", text)
        self.assertIn("underlyingResultType", text)
        self.assertIn("cleanupVerified", text)
        self.assertIn("MaxCleanupInspectionEntries = 1000000", text)
        self.assertIn("ConvertTo-WindowsCommandLineArgument", text)
        self.assertIn("Resolve-ApplicationPath", text)
        self.assertIn(
            "ConvertTo-WindowsCommandLine -Arguments (@($applicationPath) + @($ArgumentList))",
            text,
        )
        self.assertIn("$argumentValue = 'argv value with spaces'", text)
        self.assertIn("$project,", text)
        self.assertNotIn("('" + '"' + "' + $project + '" + '"' + "')", text)
        self.assertNotIn("$rowsAfter = Get-ProcessRows", text)
        self.assertNotIn("identities = $identities.ToArray()", text)
        self.assertIn("raw process identity was emitted", text)
        self.assertIn("failureRecordCount -ne", text)
        self.assertIn("Prepare-IsolatedVcpkgTool", text)
        self.assertIn("VCPKG_TOOL_RELEASE_TAG", text)
        self.assertIn("$env:VCPKG_ROOT = Get-IsolatedVcpkgRoot", text)
        self.assertIn("[IO.File]::Copy($sourcePath, $isolatedToolPath, $true)", text)
        self.assertIn("vcpkg.disable-metrics", text)
        self.assertNotIn("& .\\bootstrap-vcpkg.bat", text)
        self.assertIn("SAKURA_OUTPUT_BACKEND = 'cpp'", text)
        self.assertIn("SAKURA_UTF16_BACKEND = 'cpp'", text)
        self.assertIn("SKIP_CREATE_GITHASH = '1'", text)

    def test_document_pins_explicit_payload_free_contract(self) -> None:
        document = (REPO_ROOT / "tools/verify-native-rust-incremental.md").read_text(
            encoding="utf-8"
        )
        for required in (
            "baseline",
            "no_op_1",
            "rust_source",
            "cpp_provider",
            "rust_archive",
            "rust_stamp",
            "provider_obj",
            "unexpected_consumer",
            "payload-free",
            "sakura_core/sakura.vcxproj",
            "Cargo-free",
            "PreflightSakuraNativeFfiCargo",
            "diagnostic log metadata",
            "sanitized survivor",
            "immutable",
            "reparse",
            "CREATE_SUSPENDED",
            "VCPKG_TOOL_RELEASE_TAG",
            "vcpkg.exe",
            "VCPKG_ROOT",
            "BuildError",
            "package restore",
        ):
            self.assertIn(required, document)

    @unittest.skipUnless(_powershell_hosts(), "PowerShell is required")
    def test_self_test_exercises_classification_closure_and_schema(self) -> None:
        for powershell in _powershell_hosts():
            with self.subTest(shell=powershell):
                completed = subprocess.run(
                    [
                        powershell,
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        str(SCRIPT),
                        "-SelfTest",
                    ],
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                self.assertEqual(0, completed.returncode, completed.stdout + completed.stderr)
                self.assertIn("PASS verify-native-rust-incremental.ps1 self-tests", completed.stdout)
                summary_lines = [
                    line for line in completed.stdout.splitlines() if line.startswith("SELFTEST_JSON ")
                ]
                self.assertEqual(1, len(summary_lines), completed.stdout)
                summary = json.loads(summary_lines[0][len("SELFTEST_JSON ") :])
                self.assertEqual(1, summary["schemaVersion"])
                self.assertTrue(summary["payloadFree"])
                self.assertEqual(11, summary["workActionCount"])
                self.assertEqual("ok", summary["closureType"])
                self.assertEqual("unexpected_consumer", summary["unexpectedClosureType"])
                self.assertEqual(1, summary["actionCounts"]["cargo-preflight"])
                self.assertEqual(1, summary["actionCounts"]["cargo"])
                self.assertEqual(2, summary["actionCounts"]["unexpected_tool"])
                self.assertEqual(
                    {"signcode.exe": 1, "vcpkg.exe": 1}, summary["unexpectedToolNames"]
                )
                self.assertFalse(summary["unexpectedToolNamesTruncated"])
                for kind in ("cl", "link", "lib", "rc", "mt", "delete"):
                    self.assertEqual(1, summary["actionCounts"][kind])
                self.assertEqual(2, summary["actionCounts"]["cmake"])
                self.assertEqual(1, summary["actionCounts"]["senp-tool"])
                self.assertEqual(1, summary["actionCounts"]["vcpkg-applocal"])
                self.assertEqual("survivor", summary["observedSurvivorResultType"])
                self.assertEqual(1, summary["observedSurvivorCount"])
                self.assertEqual(0, summary["postCleanupSurvivorCount"])
                self.assertTrue(summary["expectedCompilerHelperPolicyVerified"])
                self.assertGreater(summary["diagnosticByteCount"], 0)
                self.assertEqual(18, summary["diagnosticLineCount"])
                self.assertEqual(2, summary["diagnosticErrorCodes"]["MSB4019"])
                self.assertEqual(1, summary["diagnosticErrorCodes"]["C1083"])
                self.assertEqual(1, summary["diagnosticErrorCodes"]["LNK1104"])
                self.assertEqual(1, summary["diagnosticErrorCodes"]["E0425"])
                self.assertEqual("survivor", summary["combinedFailureResultType"])
                self.assertEqual(1, summary["combinedFailureExitCode"])
                self.assertEqual(2, summary["combinedFailureDiagnosticErrorCodeCount"])
                self.assertEqual(14, summary["actionRecordCount"])
                self.assertEqual(14, summary["retainedActionCount"])
                self.assertTrue(summary["actionsTruncated"])
                self.assertEqual("unexpected_consumer", summary["truncatedClosureType"])
                self.assertEqual("unexpected_action", summary["truncatedNoOpType"])
                self.assertEqual("build_failed", summary["nonzeroResultType"])
                self.assertEqual(7, summary["nonzeroExitCode"])
                self.assertEqual("timeout", summary["jobBoundTimeoutResultType"])
                self.assertEqual(0, summary["jobBoundTimeoutPostCleanupCount"])
                self.assertTrue(summary["suspendedJobOwnershipVerified"])
                self.assertTrue(summary["externalSentinelPreserved"])
                self.assertEqual("ok", summary["fastProcessResultType"])
                self.assertTrue(summary["fastProcessStdoutShaAvailable"])
                self.assertEqual("build_failed", summary["packageFailureResultType"])
                self.assertEqual(3, summary["packageFailureExitCode"])
                self.assertGreater(summary["packageFailureStdoutBytes"], 0)
                self.assertEqual(1, summary["packageFailureStdoutLines"])
                self.assertEqual(2, summary["packageFailureStderrLines"])
                self.assertEqual(1, summary["packageFailureBuildErrorCount"])
                self.assertEqual(1, summary["packageFailureErrorCodeCount"])
                self.assertFalse(summary["packageFailureParserFailed"])
                self.assertTrue(summary["packageFailureRecordsTruncated"])
                self.assertTrue(summary["versionValidationMatched"])
                self.assertEqual(3, summary["versionOutputLineCount"])
                self.assertTrue(summary["immutableSnapshotShareVerified"])
                self.assertTrue(summary["cleanupInspectionVerified"])
                self.assertTrue(summary["argumentQuotingVerified"])
                self.assertTrue(summary["resolvedApplicationPathVerified"])
                self.assertTrue(summary["sharedFingerprintFailureTyped"])
                self.assertTrue(summary["schemaFailureEnvelopeVerified"])
                self.assertTrue(summary["emergencyEnvelopeVerified"])
                self.assertTrue(summary["worktreeRegistrationFailClosedVerified"])
                self.assertEqual(4, summary["worktreeRegistrationRejectedCount"])
                self.assertTrue(summary["submoduleFingerprintComparisonVerified"])
                self.assertEqual(6, summary["packageToolMutationCount"])
                self.assertEqual(6, summary["packageToolMutationRejectedCount"])
                self.assertTrue(summary["processIdentityPayloadRejected"])
                self.assertTrue(summary["parserFailureObserved"])
                self.assertTrue(summary["blankLineActionClassificationVerified"])
                self.assertTrue(summary["directToolInvocationBoundaryVerified"])
                self.assertTrue(summary["trackedArtifactStatusBoundaryVerified"])
                self.assertTrue(summary["sourceExtensionBoundaryVerified"])
                self.assertTrue(summary["resourceAndManifestActionClassificationVerified"])
                self.assertTrue(summary["incrementalCompanionActionClassificationVerified"])
                self.assertTrue(summary["unexpectedToolNameSchemaVerified"])
                self.assertTrue(summary["vcpkgRootPinVerified"])
                self.assertTrue(summary["packageToolSchemaVerified"])

    def test_phase_order_and_expected_consumer_contract_are_literal(self) -> None:
        text = SCRIPT.read_bytes()[2:].decode("utf-16le")
        self.assertIn(
            "phaseOrder = @('baseline') + @(1..$NoOpIterations | ForEach-Object { \"no_op_$($_)\" }) + @('rust_source', 'cpp_provider')",
            text,
        )
        self.assertIn("rust_source = @('sakura_core/sakura.vcxproj')", text)
        self.assertIn("cpp_provider = @('sakura_core/sakura.vcxproj')", text)
        self.assertIn("Get-NoOpViolation", text)
        self.assertIn("$script:UnexpectedActionKinds", text)
        self.assertIn('Write-Output "PASS ${script:VerifierName}: $outputPath"', text)
        self.assertNotIn("$evidence.cleanup.path", text)
        self.assertNotIn("Get-ChildItem -LiteralPath $RepositoryRoot -Recurse -Filter *.vcxproj", text)


if __name__ == "__main__":
    unittest.main()
