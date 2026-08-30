"""Contract checks for the manifest-bound provider measurement consumer."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools" / "measure-output-provider.ps1"
DOC = ROOT / "tools" / "measure-output-provider.md"


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


class MeasureOutputProviderContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = read_utf16le_script(SCRIPT)
        cls.doc = DOC.read_text(encoding="utf-8")

    def test_script_encoding_and_manifest_parameters(self):
        self.assertTrue(SCRIPT.read_bytes().startswith(b"\xff\xfe"))
        for marker in (
            "[string]$CppBuildManifest",
            "[string]$RustBuildManifest",
            "output-provider-build-manifest",
            "Get-ProviderBuildManifest",
            "Test-ProviderAcceptanceQualified",
            "provenanceComplete",
            "Assert-ProviderManifestIdentity",
            "Get-ProviderSourceState",
            "CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle",
            "runtimeClosureSha256",
            "msvc-ltcg-compile-selector-verified",
            "msvc-ltcg-compile-selector",
            "compileLogSha256After",
            "compileCommandRustSelectorDefineCount",
            "Release selector proof must use the MSVC LTCG compile-selector contract.",
            "providerObjectFreshnessMethod",
            "providerObjectAbsentBeforeBuild",
            "exact-object-absence-v1",
        ):
            self.assertIn(marker, self.text)
        self.assertIn("([string]$Value).Length -gt 512", self.text)
        self.assertNotIn("[string]$Value.Length -gt 512", self.text)

    def test_qualified_requires_a_manifest_pair_before_launch(self):
        shell = next((name for name in ("pwsh", "powershell.exe") if shutil.which(name)), None)
        if shell is None:
            self.skipTest("Neither pwsh nor powershell.exe is available")
        # These are only regular-file inputs.  The consumer must reject the
        # missing qualified manifest pair before attempting to launch either.
        cpp_input = ROOT / "tools" / "measure-output-provider.md"
        rust_input = ROOT / "tools" / "build" / "tests" / "__init__.py"
        with tempfile.TemporaryDirectory(prefix="sakura-output-provider-contract-") as directory:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(SCRIPT),
                    "-CppTests1",
                    str(cpp_input),
                    "-RustTests1",
                    str(rust_input),
                    "-Pairs",
                    "1",
                    "-WarmupBlocks",
                    "1",
                    "-MeasuredBlocks",
                    "1",
                    "-OutputDirectory",
                    directory,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(1, completed.returncode, completed.stderr)
            combined = f"{completed.stdout}\n{completed.stderr}"
            self.assertIn("requires both CppBuildManifest and RustBuildManifest", combined)
            self.assertEqual([], list(Path(directory).glob("**/pair-*.jsonl")))

    def test_self_test_covers_valid_pair_and_rejection_cases(self):
        for marker in (
            "valid provider pair self-test",
            "tests1 identity self-test",
            "mixed source self-test",
            "dirty source self-test",
            "selector mismatch self-test",
            "collect-only evidence was incorrectly qualified",
            "complete provider evidence was incorrectly rejected",
            "runtime provider probe",
            "CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle",
            "tests1Sha256",
            "tests1SizeBytes",
            "LTCG compile log hash self-test",
            "LTCG selector macro self-test",
            "LTCG selector method self-test",
        ):
            self.assertIn(marker, self.text)

    def test_release_selector_uses_compile_tlog_contract(self):
        self.assertIn("(Get-ProviderString $manifestConfiguration 'configuration').ToLowerInvariant() -eq 'release'", self.text)
        self.assertIn("providerObjectFormat", self.text)
        self.assertIn("compileCommandHasGl", self.text)

    def test_selector_receipt_requires_exact_prebuild_absence_for_both_configs(self):
        for marker in (
            "Get-ProviderBoolean (Get-ProviderRequiredCandidate $proof @('providerObjectAbsentBeforeBuild')",
            "selector proof does not prove exact pre-build object absence",
            "selector proof object freshness method is unsupported",
            "object-freshness-method=exact-object-absence-v1",
            "object-absent-before-build=true",
            "legacy freshness receipt self-test",
            "wrong freshness method self-test",
            "false freshness receipt self-test",
            "non-boolean freshness receipt self-test",
        ):
            self.assertIn(marker, self.text)
        self.assertGreaterEqual(self.text.count("providerObjectFreshnessMethod = 'exact-object-absence-v1'"), 2)
        self.assertGreaterEqual(self.text.count("providerObjectAbsentBeforeBuild = $true"), 2)

    def test_provenance_propagates_freshness_receipt(self):
        for marker in (
            "providerObjectFreshnessMethod = [string]$Cpp.selectorProof.providerObjectFreshnessMethod",
            "providerObjectAbsentBeforeBuild = [bool]$Cpp.selectorProof.providerObjectAbsentBeforeBuild",
            "providerObjectFreshnessMethod = [string]$Rust.selectorProof.providerObjectFreshnessMethod",
            "providerObjectAbsentBeforeBuild = [bool]$Rust.selectorProof.providerObjectAbsentBeforeBuild",
        ):
            self.assertIn(marker, self.text)

    def test_collect_only_without_manifests_stays_unqualified(self):
        self.assertIn("$manifestsSupplied = $cppManifestSupplied -or $rustManifestSupplied", self.text)
        self.assertIn("$provenanceComplete = $null -ne $provenance", self.text)
        self.assertIn("if (-not $provenanceComplete)", self.text)
        self.assertIn("complete provider provenance is required", self.text)
        self.assertIn("collect-only mode is unqualified", self.text)
        self.assertRegex(
            self.text,
            r"\$pass\s*=\s*\$semanticPass\s*-and\s*\$acceptanceQualified\s*-and\s*\$performancePass\s*-and\s*\(-not \$CollectOnly\)",
        )

    def test_provenance_is_bounded_and_rechecked_around_campaign(self):
        for marker in (
            "Assert-ProviderPayloadFree $manifest",
            "manifest changed while it was being validated",
            "provider source state changed before pair",
            "provider build manifest changed during measurement",
            "provider source state changed after benchmark campaign",
            "outputBackend = 'paired(cpp,rust)'",
            "runtimeClosureMode = 'exe-only'",
            "exe-only|tests1={0}|size={1}",
            "paired-runtime-closure|cpp={0}|rust={1}",
            "artifacts = $analysisArtifacts",
        ):
            self.assertIn(marker, self.text)
        self.assertNotIn("closureSha256 = [string]$Cpp.tests1Sha256", self.text)
        for forbidden in ("command =", "commandLine =", "stdout =", "stderr =", "path =", "filePath ="):
            # The consumer may retain internal executable paths, but the
            # provenance/analysis object must never assign payload paths.
            self.assertNotIn(f"provenance {forbidden}", self.text)

    def test_docs_describe_canonical_producer_and_runner_contract(self):
        for marker in (
            "prepare-output-provider-artifact.ps1",
            "build-sln.bat",
            "-CppBuildManifest",
            "-RustBuildManifest",
            "output-provider-build-manifest",
            "provenanceComplete",
            "CollectOnly",
            "HOLD",
        ):
            self.assertIn(marker, self.doc)

    def test_self_test_output_on_both_powershell_hosts(self):
        available = [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]
        if not available:
            self.skipTest("Neither powershell.exe nor pwsh is available")
        for shell in available:
            completed = subprocess.run(
                [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT), "-SelfTest"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertIn("PASS measure-output-provider.ps1 self-tests", completed.stdout)


if __name__ == "__main__":
    unittest.main()
