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
            milestones = evidence["startupMilestones"]
            for field in (
                "processStarted",
                "topLevelWindowObserved",
                "visibleObserved",
                "captionObserved",
                "inputIdleObserved",
                "documentLayoutObserved",
            ):
                self.assertFalse(milestones[field], field)
            for field in (
                "processApiReturnMs",
                "topLevelHwndMs",
                "visibleMs",
                "captionReadyMs",
                "inputIdleMs",
                "documentReadyMs",
                "verticalScrollMaximum",
            ):
                self.assertIsNone(milestones[field], field)
            self.assertEqual("not-attempted", milestones["inputIdleObservationStatus"])
            self.assertEqual(
                ["process-start", "top-level-window", "visible", "caption", "document-layout"],
                milestones["missingMilestones"],
            )
            self.assertIsNone(milestones["timeoutStage"])
            self.assertEqual("not-attempted", milestones["descendantAffinityState"])

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
            milestones = evidence["startupMilestones"]
            self.assertFalse(milestones["processStarted"])
            self.assertFalse(milestones["topLevelWindowObserved"])
            self.assertEqual(5, len(milestones["missingMilestones"]))
            self.assertIsNone(milestones["timeoutStage"])
            self.assertEqual("not-attempted", milestones["descendantAffinityState"])

    def test_overlong_result_directory_is_rejected_before_gui_launch(self):
        shell = next((name for name in ("pwsh", "powershell.exe") if shutil.which(name)), None)
        if shell is None:
            self.skipTest("Neither pwsh nor powershell.exe is available")
        with tempfile.TemporaryDirectory(prefix="sakura-output-startup-path-budget-") as directory:
            target_root_length = 140
            base_length = len(str(Path(directory)))
            leaf_prefix = "path-budget-"
            leaf_length = target_root_length - base_length - 1
            self.assertGreaterEqual(leaf_length, len(leaf_prefix))
            overlong_root = Path(directory) / (
                leaf_prefix + "p" * (leaf_length - len(leaf_prefix))
            )
            overlong_root.mkdir()
            self.assertEqual(target_root_length, len(str(overlong_root)))
            missing_cpp = overlong_root / "missing-cpp.exe"
            missing_rust = overlong_root / "missing-rust.exe"
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PAIRED),
                    "-CppSakuraExe",
                    str(missing_cpp),
                    "-RustSakuraExe",
                    str(missing_rust),
                    "-CollectOnly",
                    "-WarmupLaunches",
                    "1",
                    "-MeasuredLaunches",
                    "1",
                    "-ResultDirectory",
                    str(overlong_root),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            reports = list(overlong_root.glob("paired-startup-*.json"))
            self.assertEqual(1, len(reports))
            self.assertLess(len(str(reports[0])), 260)
            evidence = json.loads(reports[0].read_text(encoding="utf-8"))
            self.assertEqual("path-budget", evidence["failure"]["stage"])
            self.assertEqual("path-budget", evidence["failure"]["type"])
            self.assertEqual("rejected", evidence["pathBudget"]["status"])
            self.assertGreater(evidence["pathBudget"]["maxPlannedLength"], 259)
            self.assertEqual(0, evidence["configuration"]["successfulLaunches"])
            self.assertEqual(0, evidence["termination"]["completedLaunches"])
            self.assertEqual(
                evidence["configuration"]["scheduledLaunches"],
                evidence["termination"]["suppressedLaunches"],
            )

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
        self.assertIn("startupInputIdleOptionalVerified", self.paired_text)
        self.assertIn("startupRequiredReadinessRejected", self.paired_text)
        self.assertIn("startupStrictSuccessFlagsRejected", self.paired_text)
        self.assertIn("startupInputIdleUnavailableVerified", self.paired_text)
        self.assertIn("startupInputIdleContradictionsRejected", self.paired_text)
        self.assertIn("Get-PairedRawPropertySlot", self.paired_text)
        self.assertIn("[object[]]@($false)", self.paired_text)
        self.assertIn("$metric -eq 'inputIdleMs'", self.paired_text)
        self.assertIn("readinessInputIdleOptionalSelfTestVerified", self.shared_text)
        for marker in (
            "startupMilestones",
            "processStarted",
            "topLevelWindowObserved",
            "visibleObserved",
            "captionObserved",
            "inputIdleObserved",
            "inputIdleObservationStatus",
            "documentLayoutObserved",
            "missingMilestones",
            "timeoutStage",
            "window-discovery",
            "descendantAffinityState",
            "not-attempted",
            "Convert-PairedElapsedMs",
            "New-PairedEmptyStartupMilestones",
        ):
            self.assertIn(marker, self.paired_text)
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
        for marker in (
            "function Get-PairedRunPathToken",
            "function Get-PairedPathRoleToken",
            "function New-PairedPathPlan",
            "function Complete-PairedPathPlan",
            "$script:PairedWin32PathTextLimit = 259",
            "$script:PairedWorstProfileAuthoritySuffix = '\\.sakura-platform\\profile-authority.v1.tmp.'",
            "profile-authority.v1.tmp.",
            "Assert-PairedPathBudget",
            "phase = 'generated'",
            "phase = 'finalized'",
            "closureEntries = $closureEntries.ToArray()",
            "closurePathsPlanned",
            "closureFileCountCpp",
            "closureDestinationMaxLength",
            "StartupProfileSidecarFileName",
            "sakura.exe.ini",
            "launchesBySequence",
            "$launchPlan = $pathPlan.launchesBySequence[[int]$row.sequence]",
            "profileName = [string]$launchPlan.profileName",
            "traceName",
            "[Parameter(Mandatory = $true)] [string]$PlannedTraceName",
            "$traceName = $PlannedTraceName",
            "New-PairedTraceDirectory $ExecutableDirectory $traceName",
            "pathBudget = Convert-PairedPathBudgetSummary",
            "FailureType 'path-budget'",
            "pathBudgetSubprocessNoGuiVerified",
            "pathBudgetClosureBoundaryVerified",
            "pathBudgetClosureFailureEnvelopeVerified",
            "pathBudgetClosureNoBundleVerified",
        ):
            self.assertIn(marker, self.paired_text)
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

    def test_diagnostic_substage_and_error_telemetry_is_bounded_and_fail_closed(self):
        for marker in (
            "failureSubstage = 'none'",
            "failureErrorCode = $null",
            "process-census-first",
            "process-identity-first",
            "process-census-second",
            "process-identity-second",
            "window-enumeration",
            "projection-finalize",
            "New-StartupDiagnosticProcessObservation",
            "DiagnosticPhase",
            "ProcessSnapshotFirst",
            "ProcessSnapshotSecond",
            "ProjectionFinalize",
            "diagnosticCheckpointSubstageSelfTestVerified",
            "Resolve-StartupProcessIdentity",
            "ProcessIdentity",
            "ProcessEntries",
            "Test-StartupDiagnosticFailureSubstage",
            "Set-StartupDiagnosticPhase",
            "diagnosticProductionIdentityStillPresentSelfTestVerified",
            "diagnosticMalformedIdentitySelfTestVerified",
            "diagnosticProductionDisappearanceWindowSelfTestVerified",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertNotIn("'process-identity',", self.shared_text)
        for marker in (
            "PairedDiagnosticFailureSubstages",
            "Convert-PairedDiagnosticFailureTelemetry",
            "Test-PairedDiagnosticFailureTelemetryState",
            "failureSubstage",
            "failureErrorCode",
            "startupDiagnosticsSubstageFailureVerified",
            "startupDiagnosticsSubstageMalformedRejected",
            "startupDiagnosticsCrossFieldWarningVerified",
            "startupDiagnosticsCrossFieldWarningMismatchRejected",
            "startupDiagnosticsLegacyCompatibilityVerified",
            "startupDiagnosticsMalformedCombinationsRejected",
            "mismatched-job-warning",
            "synthetic-unknown",
        ):
            self.assertIn(marker, self.paired_text)
        self.assertNotIn("'process-identity',", self.paired_text)
        for document in (PAIRED_DOC.read_text(encoding="utf-8"), STARTUP_DOC.read_text(encoding="utf-8")):
            self.assertIn("failureSubstage", document)
            self.assertIn("failureErrorCode", document)
            self.assertIn("ordinal", document.lower())

    def test_job_query_cleanup_telemetry_contract_is_bounded_and_diagnostic_only(self):
        for marker in (
            "function New-PairedEmptyJobQueryObservation",
            "function Convert-PairedJobQueryObservation",
            "function New-PairedEmptyCleanupObservation",
            "function Convert-PairedCleanupObservation",
            "$script:PairedJobQueryMaxCount = 4096",
            "$script:PairedJobQueryMaxBytes = [UInt64]1048576",
            "$script:PairedJobQueryMaxProcessCount = [UInt64]131072",
            "$script:PairedJobQueryMaxAttempts = 8",
            "launchJobQueryObservation",
            "cleanupObservation",
            "jobQueryObservation",
            "attemptsTruncated",
            "errorCode = 234",
            "status = 'not-attempted'",
            "status = 'partial'",
            "status = 'failed'",
            "status = 'unavailable'",
            "New-PairedUnavailableJobQueryObservation",
            "New-PairedUnavailableCleanupObservation",
        ):
            self.assertIn(marker, self.paired_text)
        # Producer JSON is lowercase; the paired converter must not invent a
        # second PascalCase schema.  Keep the old-v1 behavior as absence,
        # rather than accepting a guessed alternate field spelling.
        for marker in ("@('attempted', 'Attempted')", "@('succeeded', 'Succeeded')", "@('errorCode', 'ErrorCode')"):
            self.assertNotIn(marker, self.paired_text)
        self.assertNotIn("ERROR_MORE_DATA", self.paired_text)
        self.assertIn("if ($run.status -ne 'succeeded' -or -not (Test-PairedRunCleanupVerified $run))", self.paired_text)
        self.assertIn("$script:PairedSchemaVersion = 1", self.paired_text)
        self.assertIn("qualified = [bool]$accepted", self.paired_text)
        self.assertIn("startupGatePass = [bool]($accepted -and $performance.pass)", self.paired_text)

    def test_cleanup_telemetry_consumer_schema_is_additive_and_fail_closed(self):
        for marker in (
            "PairedCleanupTelemetryMaxCount = 4096",
            "PairedCleanupTelemetryProcessEnumerationFields",
            "PairedCleanupTelemetryTrackedSweepFields",
            "PairedCleanupTelemetryAffinityFields",
            "function New-PairedCleanupTelemetryFallback",
            "function Convert-PairedCleanupTelemetry",
            "function New-PairedAffinityTelemetryFallback",
            "function Convert-PairedAffinityTelemetry",
            "processEnumerationAttempted",
            "processEnumerationSucceeded",
            "processEnumerationComplete",
            "processEnumerationErrorCode",
            "processEnumerationRetryCount",
            "processEnumerationCallCount",
            "processEnumerationCompletedCount",
            "processEnumerationFailureCount",
            "trackedSweepFailureType",
            "trackedSweepFailureErrorCode",
            "trackedSweepIdentityAttemptCount",
            "trackedSweepIdentityFailureCount",
            "trackedSweepDisappearedAfterSnapshotCount",
            "trackedSweepStillPresentAfterFailureCount",
            "trackedSweepPassCount",
            "historicalOwnedCount",
            "currentLiveCount",
            "expiredHistoricalCount",
            "failureErrorCode",
            "liveSetSource",
            "$expired -ne ($historical - $current)",
            "processSum",
            "processFailures -eq 0",
            "-not $processSucceeded",
            "selfTestMalformedProcessFlags.processEnumerationSucceeded = $false",
            "selfTestMalformedProcessIncomplete.processEnumerationComplete = $false",
            "cleanupTelemetryProcessSuccessFlagsRejected",
            "first-cause",
            "cleanupTelemetryFirstCauseVerified",
            "cleanupTelemetryProcessCrossFieldRejected",
            "cleanupTelemetryPartialRejected",
            "cleanupTelemetryAffinityCrossFieldRejected",
            "cleanupTelemetryBoundedIntegerRejected",
            "cleanupTelemetryEnumRejected",
            "failureType = 'unknown'",
            "liveSetSource = 'not-observed'",
        ):
            self.assertIn(marker, self.paired_text)
        self.assertIn("current -gt $historical", self.paired_text)
        self.assertIn("$expired -ne ($historical - $current)", self.paired_text)
        self.assertIn("return New-PairedUnavailableCleanupObservation $outer", self.paired_text)
        self.assertIn("$telemetry = Convert-PairedCleanupTelemetry $Raw", self.paired_text)
        self.assertIn("foreach ($field in @($telemetry.Keys))", self.paired_text)
        self.assertIn("adoption = [ordered]@{", self.paired_text)

    def test_shared_direct_job_query_observation_count_matches_native_attempt(self):
        for marker in (
            "if ($Query -is [StartupProbeJobResult])",
            "$observation.queryCount = if ($observation.attempted) { 1 } else { 0 }",
            "$emptyJobObservation.queryCount -eq 1",
            "$syntheticFailedObservation.queryCount -eq 1",
            "$noJobMembers.queryObservation.queryCount -eq 0",
        ):
            self.assertIn(marker, self.shared_text)

    def test_tracked_identity_disappearance_recovery_is_typed_and_single_shot(self):
        # A process can exit between the complete Toolhelp snapshot and its
        # identity open.  The shared producer may recover only from a
        # coherent typed identity failure after exactly one complete fresh
        # census proves that exact PID is absent.  Keep the negative branches
        # visible in this cheap contract test so a future refactor cannot turn
        # a malformed or unavailable observation into a clean sweep.
        for marker in (
            "function Get-StartupTrackedSweepFailureType",
            "$ProcessId -le 0",
            "$attemptedValue -isnot [bool]",
            "$entriesValue",
            "PSObject.Properties['Entries']",
            "$entriesProperty.Value",
            "$entriesValue -isnot [Array]",
            "foreach ($entry in $entriesValue)",
            "$attemptCount -gt 3",
            "$retryCount -ge $attemptCount",
            "$retriedValue -isnot [bool]",
            "[bool]$retriedValue -ne ($retryCount -gt 0)",
            "$entriesValue.Length -gt 65536",
            "$retryCountValue",
            "$seen.ContainsKey($entryId)",
            "$imageNameValue.Length -ge 260",
            "function Invoke-StartupTrackedIdentityFailure",
            "Recovery is limited to a coherent typed QueryProcessIdentity failure.",
            "$identitySucceededValue -isnot [bool]",
            "$null -eq $IdentityProbe",
            "$null -eq $identityError -or $identityError -le 0",
            "[NativeStartupProbe]::GetProcessEntries()",
            "return $true",
            "if (Invoke-StartupTrackedIdentityFailure",
            "identityDisappearedAccepted",
            "identityNoObservationCalls",
            "identityNoObservationVerified",
            "freshAbsentCensusCalls.Value -eq 1",
            "identityStillPresentThrew",
            "identityUnavailableThrew",
            "identityNoFreshCalls",
            "identityMalformedProbeVerified",
            "freshMalformedCases",
            "freshMalformedVerified",
            "trackedSweepDisappearedAfterSnapshotCount",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertIn("Set-StartupTrackedSweepFailure $CleanupObservation 'identity-unavailable' 13", self.shared_text)
        self.assertIn("throw \"Could not verify the identity of tracked process", self.shared_text)

    def test_job_identity_disappearance_recovery_disables_tracked_telemetry(self):
        for marker in (
            "function Get-JobProcessRecords",
            "function Invoke-StartupTrackedIdentityFailure",
            "[bool]$RecordTrackedSweepTelemetry = $true",
            "$RecordTrackedSweepTelemetry -and $null -ne $CleanupObservation",
            "Invoke-StartupTrackedIdentityFailure $CleanupObservation ([int]$record.Id)",
            "-RecordTrackedSweepTelemetry:$false",
            "jobIdentityDisappearedTelemetryDisabledObservation",
            "jobIdentityDisappearedTelemetryDisabledAccepted",
            "jobIdentityDisappearedTelemetryDisabledCalls.Value -eq 1",
            "jobIdentityStillPresentTelemetryDisabledObservation",
            "jobIdentityCensusUnavailableTelemetryDisabledObservation",
            "jobIdentityMalformedFreshCensusTelemetryDisabledObservation",
            "jobIdentityDisappearedTelemetryDisabledVerified",
            "jobIdentityStillPresentTelemetryDisabledVerified",
            "jobIdentityCensusUnavailableTelemetryDisabledVerified",
            "jobIdentityMalformedFreshCensusTelemetryDisabledVerified",
            "jobIdentityTelemetryDisabledSelfTestVerified",
            "jobIdentityDisappearedTelemetryDisabledSelfTestVerified",
            "jobIdentityStillPresentTelemetryDisabledSelfTestVerified",
            "jobIdentityCensusUnavailableTelemetryDisabledSelfTestVerified",
            "jobIdentityMalformedFreshCensusTelemetryDisabledSelfTestVerified",
            "Job membership identity gaps use a fresh Job-membership query",
            "The fresh Job-membership query is the sole recovery proof",
            "add a second Toolhelp census here",
        ):
            self.assertIn(marker, self.shared_text)
        job_start = self.shared_text.index("function Get-JobProcessRecords")
        job_end = self.shared_text.index("function Test-ProcessIdentity", job_start)
        job_path = self.shared_text[job_start:job_end]
        self.assertNotIn("Add-StartupTrackedSweepCount $CleanupObservation", job_path)
        self.assertNotIn("Invoke-StartupTrackedIdentityFailure", job_path)
        self.assertIn("Convert-ProcessIdentity $identityProbe.Identity", job_path)

    def test_job_identity_observation_schema_and_production_path_wiring(self):
        for marker in (
            "startupJobIdentityCounterFields",
            "function New-StartupJobIdentityObservation",
            "function Add-StartupJobIdentityCount",
            "function Set-StartupJobIdentityFailure",
            "jobIdentityObservation = New-StartupJobIdentityObservation",
            "function Test-StartupCoherentIdentityFailure",
            "function Test-StartupJobIdentityShape",
            "$imagePathValue.Length -ge 32768",
            "[int]$processIdValue -eq $RequestedProcessId",
            "[int]$parentProcessIdValue -eq $ExpectedParentProcessId",
            "[AllowNull()] [object]$Invokers = $null",
            "candidateJobQueryInvoker",
            "candidateProcessCensusInvoker",
            "candidateIdentityQueryInvoker",
            "jobIdentityProductionPathSelfTestVerified",
            "jobIdentityAcceptedVerified",
            "jobIdentityStillPresentVerified",
            "jobIdentityUnavailableVerified",
            "jobIdentityMalformedVerified",
            "jobIdentityInvocationExceptionVerified",
            "jobIdentityFreshQueryExceptionVerified",
            "jobIdentityConversionFailureVerified",
            "jobIdentityContradictorySuccessVerified",
            "jobIdentityMalformedFreshQueryCasesVerified",
            "processEnumerationMalformedEnvelopeSelfTestVerified",
            "processEnumerationFailedEnvelopeSelfTestVerified",
            "function Test-StartupIntegralValue",
            "attemptedValue -is [bool]",
            "entriesValue -is [Array]",
            "entryIds.ContainsKey($processId)",
            "Test-StartupIdentitySuccessEnvelope",
            "identitySucceeded = Test-StartupIdentitySuccessEnvelope",
            "validateJobQuery",
            "processIdsValue -is [Array]",
            "Test-StartupIntegralValue $rawProcessId 1",
            "jobIdentityContradictorySuccessCases",
            "jobIdentityMalformedFreshQueryCases",
            "fresh Job-membership query",
            "JobIdentityObservation = $null",
        ):
            self.assertIn(marker, self.shared_text)
        for marker in (
            "PairedCleanupTelemetryJobIdentityFailureTypes",
            "PairedCleanupTelemetryJobIdentityFields",
            "function New-PairedEmptyJobIdentityObservation",
            "function New-PairedUnavailableJobIdentityObservation",
            "function Convert-PairedJobIdentityObservation",
            "function Test-PairedStructuredObject",
            "function New-PairedEmptyLaunchJobQueryObservation",
            "function Convert-PairedLaunchJobQueryObservation",
            "jobIdentityObservation = New-PairedEmptyJobIdentityObservation",
            "status = 'not-observed'",
            "status = 'observed'",
            "status = 'unavailable'",
            "jobIdentityObservation",
            "selfTestJobIdentityValidVerified",
            "selfTestJobIdentityPartialRejected",
            "selfTestJobIdentityCrossFieldRejected",
            "selfTestJobIdentityOneSidedRejected",
            "selfTestJobIdentityMismatchRejected",
            "selfTestJobIdentityMalformedOuterNullRejected",
            "selfTestJobIdentityMalformedOuterScalarRejected",
            "jobIdentityObservationMalformedOuterSuppressionVerified",
            "jobIdentityObservationFailureTerminationVerified",
            "jobIdentityObservationOldSchemaNotObservedVerified",
            "selfTestJobIdentityValidLaunchObservation",
            "Test-PairedJobIdentityObservationEqual",
            "Test-PairedJobIdentityObservationContract",
            "$rawLaunchQueryPresent = Test-PairedPropertyPresent $Raw 'launchJobQueryObservation'",
            "$rawCleanupPresent = Test-PairedPropertyPresent $Raw 'cleanupObservation'",
            "reason = 'malformed-outer'",
            "jobIdentityObservationContractValid",
            "reason = 'one-sided'",
            "reason = 'unavailable-or-mismatch'",
            "identityFailureCount",
            "recoveryAttemptCount",
            "disappearedAfterSnapshotCount",
            "stillPresentAfterFailureCount",
            "$failures -gt $attempts",
            "$recoveries -gt $failures",
            "$disappeared + $present -gt $recoveries",
            "Remove('jobIdentityObservation')",
        ):
            self.assertIn(marker, self.paired_text)
        job_start = self.shared_text.index("function Get-JobProcessRecords")
        job_end = self.shared_text.index("function Test-ProcessIdentity", job_start)
        job_path = self.shared_text[job_start:job_end]
        self.assertIn("$freshJobQuery = & $jobQueryInvoker $Job", job_path)
        self.assertIn("$freshJobCheck.processIds", job_path)
        self.assertIn("$JobIdentityObservation", job_path)
        self.assertIn("Test-StartupJobIdentityShape", job_path)
        self.assertIn("Set-StartupJobIdentityFailure $jobIdentityObservation 'identity-disappeared'", job_path)
        self.assertIn("Set-StartupJobIdentityFailure $jobIdentityObservation 'identity-still-present'", job_path)

    def test_paired_cleanup_gate_requires_explicit_boolean_identity_contract(self):
        for marker in (
            "function Test-PairedRunCleanupVerified",
            "Test-PairedPropertyPresent $Run 'jobIdentityObservationContractValid'",
            "$identityContract -is [bool]",
            "$identityContractValid = $identityContractPresent -and",
            "$selfTestMissingIdentityContractRejected",
            "$selfTestNonBooleanIdentityContractRejected",
            "$selfTestJobIdentityContractPresenceGateTerminationVerified",
            "jobIdentityObservationContractMissingRejected",
            "jobIdentityObservationContractNonBooleanRejected",
            "jobIdentityObservationContractPresenceGateTerminationVerified",
            "New-PairedCampaignTermination",
            "laterLaunchesSuppressed",
        ):
            self.assertIn(marker, self.paired_text)

    def test_graceful_identity_race_transfers_only_to_mandatory_job_containment(self):
        for marker in (
            "function Try-StartupGracefulJobIdentityFallback",
            "function Test-StartupProcessCleanupCompletion",
            "gracefulCloseAttempted",
            "gracefulCloseSucceeded",
            "gracefulCloseFallbackType",
            "identity-still-present",
            "Try-StartupGracefulJobIdentityFallback $cleanupObservation $stillPresentBefore",
            "$stillPresentAfter -ne ($StillPresentBefore + 1)",
            "$CleanupObservation.gracefulCloseFallbackType = 'identity-still-present'",
            "gracefulIdentityFallbackSelfTestVerified",
            "gracefulTerminalCompletionSelfTestVerified",
        ):
            self.assertIn(marker, self.shared_text)
        stop_start = self.shared_text.index("function Stop-OwnedProcesses")
        stop_end = self.shared_text.index("function Test-StartupProcessCleanupCompletion", stop_start)
        stop_path = self.shared_text[stop_start:stop_end]
        self.assertIn("CloseKillOnCloseJob", stop_path)
        self.assertNotIn("TerminateProcessHandle", stop_path)
        self.assertIn("trackedSweepVerified", self.shared_text)
        for marker in (
            "$script:PairedGracefulCloseFallbackTypes = @('none', 'identity-still-present')",
            "gracefulCloseFallbackType = 'not-observed'",
            "$gracefulPresentCount",
            "$gracefulCloseFallbackType -eq 'identity-still-present'",
            "jobIdentityObservation.stillPresentAfterFailureCount -eq [UInt64]0",
            "selfTestGracefulFallbackVerified",
            "selfTestGracefulFallbackPartialRejected",
            "selfTestGracefulFallbackContradictoryRejected",
            "gracefulIdentityFallbackValidVerified",
            "gracefulIdentityFallbackPartialRejected",
            "gracefulIdentityFallbackContradictoryRejected",
            "$cleanupObservationSucceeded",
            "Test-PairedStructuredObject $cleanupObservation",
            "$prelaunchCleanupNotAttempted",
            "$cleanupObservationStatus -eq 'not-attempted'",
            "$processStarted -is [bool] -and -not [bool]$processStarted",
            "selfTestOldSchemaMalformedGracefulRejected",
            "selfTestOldSchemaFailedCloseRejected",
            "cleanupObservationLegacyMalformedRejected",
            "cleanupObservationTerminalStatusRequired",
        ):
            self.assertIn(marker, self.paired_text)

    def test_shared_job_query_retry_contract_is_bounded_and_requires_full_membership(self):
        for marker in (
            "private const int ERROR_MORE_DATA = 234;",
            "private const uint MAX_JOB_QUERY_BYTES = 1024 * 1024;",
            "private const int MAX_JOB_QUERY_ATTEMPT_RECORDS = 8;",
            "private static bool CanAttemptJobQuery",
            "while (CanAttemptJobQuery(result.AttemptCount))",
            "RunJobQueryContractSelfTest",
            "JobQueryInvoker",
            "QueryJobProcessIdsCore",
            "BoundJobQueryByteCount",
            "nativeByteBoundsValid",
            "requiredBytes > capacity",
            "moreDataCalls != 3",
            "partialCalls != 3",
            "partialNextCapacity",
            "exhaustedCalls != MAX_JOB_QUERY_ATTEMPT_RECORDS",
            "CanAttemptJobQuery(exhaustedCalls)",
            "var idsSeen = new Hashtable();",
            "idsSeen.ContainsKey(id)",
            "if (listed != assigned)",
            "membershipFirstRetryCapacity",
            "membershipSecondRetryCapacity",
            "membershipFinalComplete",
            "invalidProcessIds",
            "jobQueryRetryBehaviorCorrected",
            "realMultiMemberJobQuerySelfTestVerified",
            "multiMemberIdsMatch",
            "multiMemberFinalComplete",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertNotIn("jobQueryRetryBehaviorUnchanged", self.shared_text)
        self.assertNotIn("for (int attempt = 0; attempt < 3", self.shared_text)

    def test_shared_process_enumeration_retry_contract_is_bounded_and_fail_closed(self):
        for marker in (
            "public sealed class StartupProbeProcessEntriesResult",
            "public bool Complete;",
            "public bool Succeeded;",
            "public int ErrorCode;",
            "public int AttemptCount;",
            "public int RetryCount;",
            "public bool Retried;",
            "public StartupProbeProcessEntry[] Entries;",
            "private const int ERROR_NO_MORE_FILES = 18;",
            "private const int ERROR_BAD_LENGTH = 24;",
            "private const int MAX_PROCESS_ENUMERATION_ATTEMPTS = 3;",
            "private const int MAX_PROCESS_ENTRY_COUNT = 65536;",
            "ProcessSnapshotInvoker",
            "ProcessEntryInvoker",
            "GetProcessEntriesCore",
            "TryAppendProcessEntry",
            "RunProcessEnumerationContractSelfTest",
            "emptyResult",
            "closeFailureResult",
            "oneEntryResult",
            "retryResult",
            "nextRetryResult",
            "partialResult",
            "exhaustedResult",
            "exhaustedSnapshotCalls != MAX_PROCESS_ENUMERATION_ATTEMPTS",
            "function Get-VerifiedProcessEntries",
            "-not [bool]$probe.Complete",
            "-not [bool]$probe.Succeeded",
            "processEnumerationPidZeroSelfTestVerified",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertNotIn("public static StartupProbeProcessEntry[] GetProcessEntries()", self.shared_text)
        self.assertIn("processEnumerationRetryContractSelfTestVerified", self.shared_text)
        self.assertIn("realProcessEnumerationSelfTestVerified", self.shared_text)

    def test_shared_affinity_verification_targets_only_fresh_current_live_records(self):
        for marker in (
            "function Get-StartupAffinityVerificationPlan",
            "$historicalById = @{}",
            "$currentById = @{}",
            "The current-live affinity process set is unavailable.",
            "The current-live affinity process set is empty.",
            "contains duplicate process",
            "contains unknown process",
            "does not match its historical identity",
            "Validate the complete current set before performing any native read-back.",
            "Get-TrackedOwnedProcesses $owned",
            "Get-StartupAffinityVerificationPlan $owned $currentRecords",
            "foreach ($record in $affinityVerificationPlan)",
            "Read-ProcessAffinityVerified -ProcessId ([int]$record.Id) -Mask $AffinityMask",
            "affinityCurrentSetSelfTestVerified",
            "affinityInvalidCurrentSetSelfTestVerified",
            "affinityExpiredHistoricalExcludedSelfTestVerified",
        ):
            self.assertIn(marker, self.shared_text)
        self.assertNotIn("function Get-StartupAffinityCurrentMatchCount", self.shared_text)

    def test_empty_source_status_hash_contract_is_explicit(self):
        self.assertIn("function New-PairedSourceState", self.paired_text)
        self.assertIn("[AllowEmptyString()] [object]$Value", self.paired_text)
        self.assertIn("[AllowEmptyString()] [object]$StatusText", self.paired_text)
        self.assertIn("Text SHA-256 input cannot be null.", self.paired_text)
        self.assertIn("Repository source status cannot be null.", self.paired_text)
        self.assertIn("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", self.paired_text)
        self.assertIn("sourcePreflightCaptured = $true", self.paired_text)
        self.assertIn("textSha256EmptyVerified", self.paired_text)
        self.assertIn("sourcePreflightSyntheticVerified", self.paired_text)

    def test_selector_proof_is_configuration_aware_and_archive_bound(self):
        for marker in (
            "msvc-ltcg-compile-selector-verified",
            "msvc-ltcg-compile-selector",
            "msvc-ltcg-anonymous",
            "dumpbin-object-undefined",
            "coff-symbols",
            "compileLogExistsBefore",
            "compileLogExistsAfter",
            "compileLogSha256Before",
            "compileLogSha256After",
            "compileLogSizeBytesBefore",
            "compileLogSizeBytesAfter",
            "compileCommandRustSelectorDefineCount",
            "rustArchiveResult",
            "dumpbin-defined-exports-verified",
            "definedProviderSymbols",
            "archive-result={1}|archive={2}|defined={3}",
            "selectorProofVerificationMethod",
            "manifestSelectorValidDebugCpp",
            "manifestSelectorValidReleaseRust",
            "manifestSelectorWrongConfigurationRejected",
            "manifestSelectorCompileLogRejected",
            "manifestSelectorArchiveHashRejected",
            "manifestSelectorSymbolRejected",
            "manifestSelectorProofHashRejected",
            "manifestSelectorMirrorRejected",
        ):
            self.assertIn(marker, self.paired_text)
        self.assertIn("The build manifest selector proof archive evidence is incomplete.", self.paired_text)
        self.assertIn("The build manifest selector proof compile log presence and size are inconsistent.", self.paired_text)
        self.assertIn("build manifest compile proof mirrors are stale.", self.paired_text)

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
        self.assertIn("two-phase", paired_doc)
        self.assertRegex(paired_doc, r"phase\s+2")
        self.assertIn("二段階", startup_doc)
        self.assertIn("phase 2", startup_doc)
        self.assertIn("prepare-output-startup-artifact.ps1", startup_doc)
        self.assertIn("checkout must be clean", paired_doc)
        self.assertIn("clean checkout", startup_doc)
        self.assertIn("dumpbin", paired_doc)
        self.assertIn("dumpbin", startup_doc)
        self.assertIn("raw error", paired_doc)
        self.assertIn("missingMilestones", paired_doc)

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
            self.assertTrue(payload["jobQueryRetryContractSelfTestVerified"])
            self.assertTrue(payload["jobQueryRetryBehaviorCorrected"])
            self.assertTrue(payload["processEnumerationRetryContractSelfTestVerified"])
            self.assertTrue(payload["processEnumerationRetryBehaviorCorrected"])
            self.assertTrue(payload["processEnumerationContractSelfTestVerified"])
            self.assertTrue(payload["processEnumerationMalformedEnvelopeSelfTestVerified"])
            self.assertTrue(payload["processEnumerationFailedEnvelopeSelfTestVerified"])
            self.assertTrue(payload["processEnumerationPidZeroSelfTestVerified"])
            self.assertTrue(payload["realProcessEnumerationSelfTestVerified"])
            self.assertTrue(payload["realMultiMemberJobQuerySelfTestVerified"])
            self.assertTrue(payload["affinityCurrentSetSelfTestVerified"])
            self.assertTrue(payload["affinityInvalidCurrentSetSelfTestVerified"])
            self.assertTrue(payload["affinityExpiredHistoricalExcludedSelfTestVerified"])
            self.assertTrue(payload["trackedIdentityNoObservationSelfTestVerified"])
            self.assertTrue(payload["trackedIdentityMalformedProbeSelfTestVerified"])
            self.assertTrue(payload["trackedIdentityMalformedFreshCensusSelfTestVerified"])
            self.assertTrue(payload["jobIdentityDisappearedTelemetryDisabledSelfTestVerified"])
            self.assertTrue(payload["jobIdentityStillPresentTelemetryDisabledSelfTestVerified"])
            self.assertTrue(payload["jobIdentityCensusUnavailableTelemetryDisabledSelfTestVerified"])
            self.assertTrue(payload["jobIdentityMalformedFreshCensusTelemetryDisabledSelfTestVerified"])
            self.assertTrue(payload["jobIdentityTelemetryDisabledSelfTestVerified"])
            self.assertTrue(payload["jobIdentityProductionPathSelfTestVerified"])
            self.assertTrue(payload["jobIdentityAcceptedDisappearanceSelfTestVerified"])
            self.assertTrue(payload["jobIdentityStillPresentSelfTestVerified"])
            self.assertTrue(payload["jobIdentityUnavailableSelfTestVerified"])
            self.assertTrue(payload["jobIdentityMalformedFreshQuerySelfTestVerified"])
            self.assertTrue(payload["jobIdentityInvocationExceptionSelfTestVerified"])
            self.assertTrue(payload["jobIdentityFreshQueryExceptionSelfTestVerified"])
            self.assertTrue(payload["jobIdentityConversionFailureSelfTestVerified"])
            self.assertTrue(payload["jobIdentityContradictorySuccessSelfTestVerified"])
            self.assertTrue(payload["jobIdentityMalformedFreshQueryCasesSelfTestVerified"])
            self.assertTrue(payload["readinessInputIdleOptionalSelfTestVerified"])
            self.assertTrue(payload["startupDiagnosticSubstageSelfTestVerified"])
            self.assertTrue(payload["startupDiagnosticProductionIdentityStillPresentSelfTestVerified"])
            self.assertTrue(payload["startupDiagnosticProductionMalformedIdentitySelfTestVerified"])
            self.assertTrue(payload["startupDiagnosticProductionDisappearanceWindowSelfTestVerified"])
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
            self.assertTrue(payload["textSha256EmptyVerified"])
            self.assertTrue(payload["textSha256NonEmptyVerified"])
            self.assertTrue(payload["textSha256NullRejected"])
            self.assertTrue(payload["sourcePreflightSyntheticVerified"])
            self.assertTrue(payload["manifestProducerContractVerified"])
            self.assertTrue(payload["manifestSelectorProofVerified"])
            for field in (
                "manifestSelectorValidDebugCpp",
                "manifestSelectorValidDebugRust",
                "manifestSelectorValidReleaseCpp",
                "manifestSelectorValidReleaseRust",
                "manifestSelectorWrongResultRejected",
                "manifestSelectorWrongConfigurationRejected",
                "manifestSelectorWrongCountRejected",
                "manifestSelectorCompileLogRejected",
                "manifestSelectorArchiveHashRejected",
                "manifestSelectorSymbolRejected",
                "manifestSelectorProofHashRejected",
                "manifestSelectorMirrorRejected",
                "manifestSelectorTopLevelHashRejected",
            ):
                self.assertTrue(payload[field], field)
            self.assertTrue(payload["manifestCleanSourceRequired"])
            self.assertTrue(payload["integrityRechecksVerified"])
            self.assertTrue(payload["postWriteReportRecheckVerified"])
            self.assertTrue(payload["pathPlanCompactNamesVerified"])
            self.assertTrue(payload["pathPlanDeterministicVerified"])
            self.assertTrue(payload["pathPlanEqualRoleTokenLengthVerified"])
            self.assertTrue(payload["pathBudgetBoundary259Accepted"])
            self.assertTrue(payload["pathBudgetBoundary260Rejected"])
            self.assertTrue(payload["pathBudgetNoGuiLaunchVerified"])
            self.assertTrue(payload["pathBudgetFailureEnvelopeVerified"])
            self.assertTrue(payload["pathBudgetExecutableSelfTestVerified"])
            self.assertTrue(payload["pathBudgetSubprocessNoGuiVerified"])
            self.assertTrue(payload["pathBudgetClosureBoundaryVerified"])
            self.assertTrue(payload["pathBudgetClosureFailureEnvelopeVerified"])
            self.assertTrue(payload["pathBudgetClosureNoBundleVerified"])
            self.assertTrue(payload["plannedTraceNamePropagationVerified"])
            self.assertEqual(259, payload["pathBudget"]["limit"])
            self.assertEqual(16, payload["pathBudget"]["tokenLength"])
            self.assertTrue(payload["pathBudget"]["roleTokensEqualLength"])
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
            self.assertTrue(payload["startupMilestonesPayloadFree"])
            self.assertTrue(payload["startupMilestonesWindowDiscoveryTimeoutVerified"])
            self.assertTrue(payload["startupMilestonesReadinessTimeoutVerified"])
            self.assertTrue(payload["startupMilestonesSuccessSchemaVerified"])
            self.assertTrue(payload["startupRequiredReadinessRejected"])
            self.assertTrue(payload["startupStrictSuccessFlagsRejected"])
            self.assertTrue(payload["startupInputIdleOptionalVerified"])
            self.assertTrue(payload["startupInputIdleUnavailableVerified"])
            self.assertTrue(payload["startupInputIdleContradictionsRejected"])
            self.assertTrue(payload["startupMilestonesDescendantNotAttemptedVerified"])
            self.assertTrue(payload["startupMilestonesDescendantFailureVerified"])
            self.assertTrue(payload["startupMilestonesFailureSchemaVerified"])
            self.assertTrue(payload["startupDiagnosticsSubstageFailureVerified"])
            self.assertTrue(payload["startupDiagnosticsSubstageMalformedRejected"])
            self.assertTrue(payload["startupDiagnosticsCrossFieldWarningVerified"])
            self.assertTrue(payload["startupDiagnosticsCrossFieldWarningMismatchRejected"])
            self.assertTrue(payload["startupDiagnosticsLegacyCompatibilityVerified"])
            self.assertTrue(payload["startupDiagnosticsMalformedCombinationsRejected"])
            for field in (
                "jobQueryObservationGoodVerified",
                "jobQueryObservationPartialVerified",
                "jobQueryObservationError234Verified",
                "jobQueryObservationMalformedLocalFallbackVerified",
                "jobQueryObservationOldSchemaNeutralVerified",
                "cleanupObservationGoodVerified",
                "cleanupObservationMalformedVerified",
                "jobIdentityObservationValidVerified",
                "jobIdentityObservationLaunchValidVerified",
                "jobIdentityObservationOldSchemaNotObservedVerified",
                "jobIdentityObservationPartialRejected",
                "jobIdentityObservationCrossFieldRejected",
                "jobIdentityObservationOneSidedRejected",
                "jobIdentityObservationMismatchRejected",
                "jobIdentityObservationMalformedOuterNullRejected",
                "jobIdentityObservationMalformedOuterScalarRejected",
                "jobIdentityObservationMalformedOuterSuppressionVerified",
                "jobIdentityObservationFailureTerminationVerified",
                "cleanupTelemetryFirstCauseVerified",
                "cleanupTelemetryProcessCrossFieldRejected",
                "cleanupTelemetryProcessSuccessFlagsRejected",
                "cleanupTelemetryPartialRejected",
                "cleanupTelemetryAffinityCrossFieldRejected",
                "cleanupTelemetryBoundedIntegerRejected",
                "cleanupTelemetryEnumRejected",
                "telemetryFailureNeutralVerified",
                "telemetryPayloadFreeVerified",
                "telemetrySuppressionGatesUnchangedVerified",
            ):
                self.assertTrue(payload[field], field)


if __name__ == "__main__":
    unittest.main()
