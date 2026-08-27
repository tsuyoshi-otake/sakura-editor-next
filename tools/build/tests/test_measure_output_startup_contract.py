"""Contract checks for the payload-free paired GUI startup evidence runner."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PAIRED = ROOT / "tools" / "measure-output-startup.ps1"
SHARED = ROOT / "tools" / "measure-startup-performance.ps1"
PAIRED_DOC = ROOT / "tools" / "measure-output-startup.md"
STARTUP_DOC = ROOT / "tools" / "startup-performance.md"


def read_utf16le_script(path: Path) -> str:
    raw = path.read_bytes()
    if not raw.startswith(b"\xff\xfe"):
        raise AssertionError(f"{path} is missing a UTF-16LE BOM")
    text = raw.decode("utf-16")
    if "\n" in text.replace("\r\n", ""):
        raise AssertionError(f"{path} contains a bare LF")
    if "\r\r\n" in text:
        raise AssertionError(f"{path} contains CRCRLF")
    return text


class PairedStartupContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.paired_text = read_utf16le_script(PAIRED)
        cls.shared_text = read_utf16le_script(SHARED)

    def test_scripts_are_utf16le_crlf(self):
        self.assertTrue(PAIRED.read_bytes().startswith(b"\xff\xfe"))
        self.assertTrue(SHARED.read_bytes().startswith(b"\xff\xfe"))

    def test_collect_only_preflight_failure_is_typed_and_nonzero(self):
        shell = next((name for name in ("pwsh", "powershell.exe") if shutil.which(name)), None)
        if shell is None:
            self.skipTest("Neither pwsh nor powershell.exe is available")
        sample = ROOT / "tools" / "startup-benchmark-sample.md"
        with tempfile.TemporaryDirectory(prefix="sakura-output-startup-contract-") as directory:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PAIRED),
                    "-CppSakuraExe",
                    str(sample),
                    "-RustSakuraExe",
                    str(sample),
                    "-CollectOnly",
                    "-WarmupLaunches",
                    "1",
                    "-MeasuredLaunches",
                    "1",
                    "-ResultDirectory",
                    directory,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            reports = list(Path(directory).glob("paired-startup-*.json"))
            self.assertEqual(1, len(reports))
            evidence = json.loads(reports[0].read_text(encoding="utf-8"))
            self.assertEqual("failed", evidence["status"])
            self.assertEqual("preflight", evidence["failure"]["type"])
            self.assertEqual("collect-only", evidence["configuration"]["mode"])
            self.assertEqual(4, evidence["configuration"]["scheduledLaunches"])
            self.assertEqual(4, evidence["termination"]["suppressedLaunches"])
            self.assertFalse(evidence["acceptance"]["qualified"])
            self.assertFalse(evidence["startupGatePass"])
            self.assertEqual("HOLD", evidence["adoption"]["decision"])
            self.assertFalse(evidence["adoption"]["adoptionEligible"])

    def test_missing_artifacts_still_write_a_typed_envelope(self):
        shell = next((name for name in ("pwsh", "powershell.exe") if shutil.which(name)), None)
        if shell is None:
            self.skipTest("Neither pwsh nor powershell.exe is available")
        with tempfile.TemporaryDirectory(prefix="sakura-output-startup-missing-") as directory:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PAIRED),
                    "-CollectOnly",
                    "-WarmupLaunches",
                    "1",
                    "-MeasuredLaunches",
                    "1",
                    "-ResultDirectory",
                    directory,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            reports = list(Path(directory).glob("paired-startup-*.json"))
            self.assertEqual(1, len(reports))
            evidence = json.loads(reports[0].read_text(encoding="utf-8"))
            self.assertEqual("failed", evidence["status"])
            self.assertEqual("preflight", evidence["failure"]["type"])
            self.assertEqual("artifact-input", evidence["failure"]["stage"])
            self.assertEqual(4, evidence["configuration"]["scheduledLaunches"])
            self.assertEqual(4, evidence["termination"]["suppressedLaunches"])
            self.assertEqual("HOLD", evidence["adoption"]["decision"])

    def test_qualified_defaults_and_collect_only_escape_hatch(self):
        self.assertRegex(self.paired_text, r"\[ValidateRange\(1,\s*1000\)\]\s*\[int\]\$WarmupLaunches")
        self.assertRegex(self.paired_text, r"\[ValidateRange\(1,\s*1000\)\]\s*\[int\]\$MeasuredLaunches")
        self.assertIn("$WarmupLaunches = 5", self.paired_text)
        self.assertIn("$MeasuredLaunches = 30", self.paired_text)
        self.assertIn("$script:PairedMinimumWarmupLaunches = 5", self.paired_text)
        self.assertIn("$script:PairedMinimumMeasuredLaunches = 30", self.paired_text)
        self.assertIn("-CollectOnly", self.paired_text)
        self.assertIn("qualified = [bool]$accepted", self.paired_text)
        self.assertIn("mode = if ($CollectOnly) { 'collect-only' }", self.paired_text)

    def test_pair_regression_gates_and_payload_schema_are_present(self):
        self.assertIn("$script:PairedPrimaryMetric = 'documentReadyMs'", self.paired_text)
        self.assertIn("$script:PairedMedianRelativeLimitPercent = 2.0", self.paired_text)
        self.assertIn("$script:PairedMedianAbsoluteLimitMs = 1.0", self.paired_text)
        self.assertIn("$script:PairedP95RelativeLimitPercent = 5.0", self.paired_text)
        self.assertIn("New-PairedPerformanceSummary", self.paired_text)
        self.assertIn("pairedDeltaMs", self.paired_text)
        self.assertIn("Median relative regression boundary self-test", self.paired_text)
        self.assertIn("Median absolute regression boundary self-test", self.paired_text)
        self.assertIn("P95 relative regression boundary self-test", self.paired_text)
        self.assertIn("performance = $performance", self.paired_text)
        self.assertIn("pairedRunnerSha256", self.paired_text)
        self.assertIn("sharedStartupImplementationSha256", self.paired_text)
        self.assertIn("roleLabels = 'caller-supplied'", self.paired_text)
        self.assertIn("buildManifestVerified = $false", self.paired_text)
        self.assertIn("New-PairedCampaignTermination", self.paired_text)
        self.assertIn("if ($Object -is [Collections.IDictionary])", self.paired_text)
        self.assertIn("status = if ($Termination.status -eq 'completed')", self.paired_text)
        self.assertIn("Assert-PairedEqual 'timeout' $terminatedReport.termination.failureType", self.paired_text)
        self.assertIn("Get-PairedBuildManifest", self.paired_text)
        self.assertIn("Get-PairedRuntimeStageIdentity", self.paired_text)
        self.assertIn("Convert-PairedRuntimeReceiptPath", self.paired_text)
        self.assertIn("Assert-PairedRuntimeReceiptArtifactIdentity", self.paired_text)
        self.assertIn("nested\\sakura_lang_en_US.dll", self.paired_text)
        self.assertIn("COM[1-9]|LPT[1-9]", self.paired_text)
        self.assertIn("runtimeStageReceiptSha256", self.paired_text)
        self.assertIn("dependencyClosureSha256", self.paired_text)
        self.assertIn("Get-PairedScriptIdentity", self.paired_text)
        self.assertIn("Assert-PairedSourceStateUnchanged $sourceState 'Postflight'", self.paired_text)
        self.assertIn("Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Postflight'", self.paired_text)
        self.assertIn("Assert-PairedSourceStateUnchanged $sourceState 'Final report write'", self.paired_text)
        self.assertIn("Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Final report write'", self.paired_text)
        self.assertIn("manifestGeneratedByProducer", self.paired_text)
        self.assertIn("atomic-directory-rename", self.paired_text)
        self.assertIn("canonicalRuntimeStage", self.paired_text)
        self.assertIn("selectorProof", self.paired_text)
        self.assertIn("dumpbin-unresolved-refs-verified", self.paired_text)
        self.assertIn("sakura_output_provider_snapshot_measure_v1", self.paired_text)
        self.assertIn("Qualified paired evidence requires a clean checkout.", self.paired_text)
        self.assertIn("normalizedArguments", self.paired_text)
        self.assertIn("measurementArgumentsSchemaVersion", self.paired_text)
        self.assertIn("measurementCommandSha256", self.paired_text)
        self.assertIn("Get-PairedMeasurementCommandSha256 $MeasurementArguments", self.paired_text)
        self.assertIn("$bundlePlans", self.paired_text)
        self.assertIn("$sampleCopyPlan", self.paired_text)
        self.assertIn("$reportTempPath", self.paired_text)
        self.assertIn("[IO.File]::Move($reportTempPath, $reportPath)", self.paired_text)
        self.assertIn("Assert-PairedSourceStateUnchanged $sourceState 'Post-write report'", self.paired_text)
        self.assertIn("Assert-PairedScriptIdentityUnchanged $scriptIdentity 'Post-write report'", self.paired_text)
        self.assertIn("cleanup-unverified", self.paired_text)
        self.assertIn("decision = 'HOLD'", self.paired_text)
        self.assertIn("adoptionEligible = $false", self.paired_text)
        self.assertIn("if ($run.status -ne 'succeeded' -or -not (Test-PairedRunCleanupVerified $run))", self.paired_text)
        self.assertRegex(
            self.paired_text,
            r"if \(\$run\.status -ne 'succeeded' -or -not \(Test-PairedRunCleanupVerified \$run\)\)\s*\{\s*\$termination = New-PairedCampaignTermination[^\r\n]*\s*break\s*\}",
        )
        self.assertIn("exit [int]$measurement.exitCode", self.paired_text)
        self.assertIn("laterLaunchesSuppressed", self.paired_text)
        self.assertIn("reportEarlyTerminationVerified = $true", self.paired_text)
        self.assertIn("GetProcessAffinityMask", self.shared_text)
        self.assertIn("SetProcessAffinityMask", self.shared_text)
        self.assertNotRegex(self.paired_text, r"Get-ChildItem[^\r\n]*-Recurse")
        self.assertNotRegex(self.shared_text, r"Get-ChildItem[^\r\n]*-Recurse")

    def test_profile_isolation_bundle_and_sidecar_contract_are_mandatory(self):
        for marker in (
            "Convert-StartupReceiptPath",
            "Assert-StartupReceiptArtifactIdentity",
            "nested\\sakura_lang_en_US.dll",
            "COM[1-9]|LPT[1-9]",
            "$script:StartupProfileSidecarContract",
            "MultiUser=0",
            "Assert-StartupProfileSidecar",
            "New-StartupArtifactBundle",
            "Assert-StartupArtifactBundleUnchanged",
            "Remove-StartupArtifactBundle",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertRegex(
            self.shared_text,
            r"Assert-StartupProfileSidecar \$ExePath",
        )
        for marker in (
            "campaign-artifact-bundle",
            "artifactBundles",
            "allBundleCleanupVerified",
            "sourceHashBefore",
            "sourceHashAfter",
            "copiedHashBefore",
            "copiedHashAfter",
            "sidecarContract",
            "New-StartupArtifactBundle",
        ):
            self.assertIn(marker, self.paired_text)
        self.assertNotIn("artifact-local-profile", self.paired_text)

    def test_docs_state_portable_sidecar_runtime_closure_and_hold(self):
        paired_doc = PAIRED_DOC.read_text(encoding="utf-8")
        startup_doc = STARTUP_DOC.read_text(encoding="utf-8")
        for document in (paired_doc, startup_doc):
            self.assertIn("sakura.exe.ini", document)
            self.assertIn("MultiUser=0", document)
            self.assertRegex(document, r"(?i)(does not|never|no|しません|ありません)")
            self.assertRegex(document, r"(?i)(DLL|%APPDATA%|APPDATA)")
            self.assertIn(".sakura-runtime-stage.json", document)
            self.assertIn("HOLD", document)
        self.assertIn("SAKURA_OUTPUT_BACKEND", paired_doc)
        self.assertIn("SAKURA_UTF16_BACKEND", paired_doc)
        self.assertIn("build-dev.bat x64 Debug", paired_doc)
        self.assertIn("Get-FileHash", paired_doc)
        self.assertIn("-CppBuildManifest", paired_doc)
        self.assertIn("-RustBuildManifest", paired_doc)
        self.assertIn("-CppRuntimeStageDirectory", paired_doc)
        self.assertIn("-RustRuntimeStageDirectory", paired_doc)
        self.assertIn("prepare-output-startup-artifact.ps1", paired_doc)
        self.assertIn("prepare-output-startup-artifact.ps1", startup_doc)
        self.assertIn("checkout must be clean", paired_doc)
        self.assertIn("clean checkout", startup_doc)
        self.assertIn("dumpbin", paired_doc)
        self.assertIn("dumpbin", startup_doc)

    def test_shared_self_test_output_in_both_powershell_hosts(self):
        available = [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]
        if not available:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in available:
            completed = subprocess.run(
                [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SHARED), "-SelfTest"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            payload = json.loads(completed.stdout.strip().splitlines()[-1])
            self.assertTrue(payload["passed"])
            self.assertTrue(payload["noGuiLaunch"])
            self.assertTrue(payload["profileSidecarVerified"])
            self.assertTrue(payload["artifactBundleVerified"])
            self.assertTrue(payload["artifactBundleCleanupVerified"])
            self.assertTrue(payload["artifactClosureVerified"])
            self.assertTrue(payload["jobContainmentSelfTestVerified"])
            self.assertTrue(payload["workingDirectorySelfTestVerified"])

    def test_self_test_output_in_both_powershell_hosts(self):
        available = [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]
        if not available:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in available:
            completed = subprocess.run(
                [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PAIRED), "-SelfTest"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            payload = json.loads(completed.stdout.strip().splitlines()[-1])
            self.assertTrue(payload["passed"])
            self.assertTrue(payload["noGuiLaunch"])
            self.assertEqual(70, payload["scheduleEntries"])
            self.assertEqual(1, payload["measurementArgumentsSchemaVersion"])
            self.assertRegex(payload["measurementCommandSha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(payload["manifestProducerContractVerified"])
            self.assertTrue(payload["manifestSelectorProofVerified"])
            self.assertTrue(payload["manifestCleanSourceRequired"])
            self.assertTrue(payload["integrityRechecksVerified"])
            self.assertTrue(payload["postWriteReportRecheckVerified"])
            self.assertEqual("nearest-rank-ceiling", payload["p95Definition"])
            self.assertTrue(payload["affinityReadBackVerified"])
            self.assertTrue(payload["cleanupTreeVerified"])
            self.assertEqual("terminated", payload["campaignTermination"]["status"])
            self.assertEqual("cleanup-unverified", payload["campaignTermination"]["type"])
            self.assertTrue(payload["campaignTermination"]["laterLaunchesSuppressed"])
            self.assertTrue(payload["reportEarlyTerminationVerified"])
            self.assertEqual(2, payload["campaignTermination"]["suppressedLaunches"])
            self.assertTrue(payload["performanceGates"]["syntheticPass"])
            self.assertTrue(payload["performanceGates"]["syntheticRegressionRejected"])


if __name__ == "__main__":
    unittest.main()
