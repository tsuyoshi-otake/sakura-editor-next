"""Contract checks for the explicit Output startup-artifact producer.

The producer owns a potentially expensive MSVC build.  These tests therefore
exercise its static contract and the opt-in PowerShell self-test only; they do
not invoke a native build, Cargo, or the runtime-stage command.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PRODUCER = ROOT / "tools" / "prepare-output-startup-artifact.ps1"


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


def powershell_hosts() -> list[str]:
    return [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]


class PrepareOutputStartupArtifactContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = read_utf16le_script(PRODUCER)

    def test_script_is_utf16le_with_crlf(self) -> None:
        raw = PRODUCER.read_bytes()
        self.assertTrue(raw.startswith(b"\xff\xfe"))
        text = raw.decode("utf-16")
        self.assertNotIn("\n", text.replace("\r\n", ""))
        self.assertNotIn("\r\r\n", text)

    def test_selector_and_reproducibility_contract_is_explicit(self) -> None:
        for marker in (
            "Assert-BackendSelector",
            "SAKURA_OUTPUT_BACKEND = $Backend",
            "SAKURA_UTF16_BACKEND = 'cpp'",
            "SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'",
            "SAKURA_GENERATE_ASSEMBLY_LISTINGS = 'false'",
            "SKIP_CREATE_GITHASH = '1'",
            "MSBUILDDISABLENODEREUSE = '1'",
            "SAKURA_BUILD_JOBS = [string]$BuildParallelism",
            "sourceHead",
            "sourceDirty",
            "sourceStatusSha256",
            "$statusLines = @()",
            "Get-MsvcIdentity",
            "Resolve-RustcExecutable",
            "Invoke-NativeOutputCapture $rustup @('which', 'rustc')",
            "Get-RustToolchainIdentity",
            "rust/native/Cargo.lock",
            "packagePlanSha256",
            "buildCommandSha256",
            "windowsImageSha256",
            "powerModeSha256",
            "selectorProof",
            "providerObjectSha256After",
            "Resolve-Dumpbin",
            "Microsoft Visual Studio/Installer/vswhere.exe",
            "-requires Microsoft.Component.MSBuild -find",
            "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\dumpbin.exe",
            "$dumpbin = Resolve-Dumpbin $comspec",
            "Get-SelectorProof $providerObjectBefore $providerObjectAfter",
            "Get-NormalizedProviderSymbols",
            "Get-ProviderCompileSelector",
            "Assert-ProviderCompileSelector",
            "Assert-ProviderObjectFormat",
            "providerObjectFormat",
            "compileLogProof",
            "compileCommandHasGl",
            "compileLogExistsBefore",
            "compileLogExistsAfter",
            "$compileLogAfter = Get-OptionalFileIdentity $compileLogSource",
            "$Configuration -eq 'Release'",
            "Assert-ProviderCompileSelector $compileLogText",
            "ANONYMOUS OBJECT",
            "msvc-ltcg-compile-selector-verified",
            "compileLogSha256Before",
            "compileLogSizeBytesBefore",
            "compileLogSha256After",
            "compileLogSizeBytesAfter",
            "compileCommandRustSelectorDefineCount",
            "compile-log-before",
            "compile-log-after",
            "dumpbin-unresolved-refs-verified",
            "Get-ProviderArchiveProof",
            "Add-ProviderArchiveProof",
            "dumpbin-defined-exports-verified",
            "definedProviderSymbols",
            "archiveExportsVerified",
            "$expected.Sort([StringComparer]::Ordinal)",
            "$unorderedRustSymbols",
            "rustSelectorProofVerified",
            "cleanup-unverified",
            "finalRootExists",
            "remainingCount",
            "cleanupEnvelopeVerified",
            "Acquire-ExclusiveLock",
            "FileStream]::new",
            "FileMode]::CreateNew",
            "FileShare]::None",
            "FileOptions]::DeleteOnClose",
            "Release-ExclusiveLock",
            "exclusiveLockVerified",
        ):
            self.assertIn(marker, self.text)
        self.assertNotIn("CreateDirectory($script:LockPath)", self.text)
        self.assertNotIn("$compileLogAfter = Get-FileIdentity $compileLogSource", self.text)
        self.assertRegex(self.text, r"\$script:SchemaVersion\s*=\s*1")
        self.assertIn("[ValidateSet('x64')]", self.text)
        self.assertIn("[ValidateSet('Debug', 'Release')]", self.text)
        self.assertNotRegex(self.text, r"Get-ChildItem[^\r\n]*-Recurse")
        self.assertNotRegex(self.text, r"Remove-Item[^\r\n]*-Recurse")

    def test_transaction_and_fail_closed_identity_contract_is_explicit(self) -> None:
        for marker in (
            "Get-OptionalFileIdentity $artifactSource",
            "Get-OptionalFileIdentity $compileLogSource",
            "$artifactAfter = Get-FileIdentity $artifactSource",
            "$compileLogAfter = Get-OptionalFileIdentity $compileLogSource",
            "CL.command.1.tlog",
            "Release provider object and compile log were not produced by this build.",
            "artifactHashBefore",
            "artifactHashAfter",
            "artifactSha256Before",
            "artifactSha256After",
            "Assert-SourceStateEqual $sourceBefore $sourceAfterBuild",
            "Assert-SourceStateEqual $sourceBefore $sourceAfterStage",
            "Get-RuntimeStageSnapshot $canonicalStage",
            "Copy-RuntimeStage $canonicalStage $transactionStage",
            "Convert-RuntimeReceiptPath",
            "Assert-RuntimeReceiptArtifactIdentity",
            "nested\\sakura_lang_en_US.dll",
            "COM[1-9]|LPT[1-9]",
            "dependencyClosureSha256",
            "runtimeStageReceiptSha256",
            "Write-JsonAtomic $manifestPath $manifest",
            "[IO.Directory]::Move($transactionRoot, $finalRoot)",
            "manifestGeneratedByProducer = $true",
            "publication = 'atomic-directory-rename'",
        ):
            self.assertIn(marker, self.text)
        self.assertIn("if (Test-Path -LiteralPath $finalRoot) { throw", self.text)
        self.assertIn("refusing overwrite", self.text)
        self.assertIn("Assert-PayloadFreeManifest", self.text)

    def test_native_exit_code_capture_and_typed_failure_contract_is_explicit(self) -> None:
        for marker in (
            "function Invoke-NativeOutputCapture",
            "$rawOutput = & $Executable @Arguments 2>$null",
            "$exitCode = $LASTEXITCODE",
            "function Convert-RustToolchainCaptureToIdentity",
            "RUST_TOOLCHAIN_IDENTITY_COMMAND_FAILED",
            "RUST_TOOLCHAIN_IDENTITY_MALFORMED",
            "rustToolchainNonzeroRejected",
            "rustToolchainMalformedRejected",
            "rustToolchainFailureCodeVerified",
            "rustToolchainExitZeroVerified",
            "primaryCode",
            "FailureSubstage",
        ):
            self.assertIn(marker, self.text)
        self.assertRegex(
            self.text,
            r"\$rawOutput = & \$Executable @Arguments 2>\$null\r?\n\s+\$exitCode = \$LASTEXITCODE",
        )
        self.assertNotRegex(
            self.text,
            r"\$rawOutput\s*=\s*& \$Executable @Arguments[^\r\n]*\|",
        )

    def test_self_test_is_bounded_and_does_not_launch_build(self) -> None:
        hosts = powershell_hosts()
        if not hosts:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in hosts:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PRODUCER),
                    "-SelfTest",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            lines = [line for line in completed.stdout.splitlines() if line.strip()]
            self.assertEqual(1, len(lines), completed.stdout)
            self.assertTrue(lines[0].startswith("SELFTEST_JSON "), completed.stdout)
            payload = json.loads(lines[0][len("SELFTEST_JSON ") :])
            self.assertTrue(payload["passed"])
            self.assertTrue(payload["noBuildLaunched"])
            self.assertTrue(payload["selectorVerified"])
            self.assertTrue(payload["selectorProofVerified"])
            self.assertTrue(payload["rustLtcgSelectorVerified"])
            self.assertTrue(payload["cppLtcgSelectorVerified"])
            self.assertTrue(payload["missingGlRejected"])
            self.assertTrue(payload["duplicateSelectorRejected"])
            self.assertTrue(payload["ambiguousSourceRejected"])
            self.assertTrue(payload["wrongConfigObjectFormatRejected"])
            self.assertTrue(payload["archiveExportsVerified"])
            self.assertTrue(payload["archiveExactSetRejected"])
            self.assertTrue(payload["runtimeStageVerified"])
            self.assertTrue(payload["manifestPayloadFreeVerified"])
            self.assertTrue(payload["boundedProcessOwnershipVerified"])
            self.assertTrue(payload["canonicalClosureVerified"])
            self.assertTrue(payload["nativeExitCodeCaptureVerified"])
            self.assertTrue(payload["rustToolchainExitZeroVerified"])
            self.assertTrue(payload["rustToolchainNonzeroRejected"])
            self.assertTrue(payload["rustToolchainMalformedRejected"])
            self.assertTrue(payload["rustToolchainFailureCodeVerified"])

    def test_invalid_selector_is_a_typed_payload_free_failure(self) -> None:
        hosts = powershell_hosts()
        if not hosts:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        shell = hosts[0]
        with tempfile.TemporaryDirectory(prefix="sakura-output-startup-producer-") as directory:
            # The producer only permits evidence below the repository build
            # tree.  The unique path is used to prove that preflight fails
            # before it creates a transaction or starts a build.
            relative_output = f"build/tmp/producer-contract-{Path(directory).name}"
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(PRODUCER),
                    "-Backend",
                    "auto",
                    "-OutputDirectory",
                    relative_output,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            lines = [line for line in completed.stdout.splitlines() if line.strip()]
            self.assertEqual(1, len(lines), completed.stdout)
            failure = json.loads(lines[0])
            self.assertEqual(1, failure["schemaVersion"])
            self.assertEqual("failed", failure["status"])
            self.assertEqual("preflight", failure["failure"]["stage"])
            self.assertEqual("preflight", failure["failure"]["type"])
            self.assertEqual("PRODUCER_PREFLIGHT", failure["failure"]["code"])
            self.assertEqual("preflight", failure["failure"]["substage"])
            self.assertTrue(failure["payloadFree"])
            self.assertNotIn(str(ROOT), completed.stdout)
            self.assertNotIn(str(ROOT), completed.stderr)
            self.assertFalse((ROOT / relative_output).exists())


if __name__ == "__main__":
    unittest.main()
