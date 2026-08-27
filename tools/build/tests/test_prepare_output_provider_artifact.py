"""Contract tests for the dedicated tests1 Output provider artifact producer."""

import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools" / "prepare-output-provider-artifact.ps1"
DOC = ROOT / "tools" / "prepare-output-provider-artifact.md"


def read_utf16le_script(path: Path) -> str:
    raw = path.read_bytes()
    if not raw.startswith(b"\xff\xfe"):
        raise AssertionError("producer script must use a UTF-16LE BOM")
    text = raw.decode("utf-16")
    if "\n" in text.replace("\r\n", ""):
        raise AssertionError("producer script contains a bare LF")
    if "\r\r\n" in text:
        raise AssertionError("producer script contains CRCRLF")
    return text


class PrepareOutputProviderArtifactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = read_utf16le_script(SCRIPT)
        cls.doc = DOC.read_text(encoding="utf-8")

    def test_frozen_selectors_and_build_path(self):
        for marker in (
            "build-sln.bat x64",
            "SAKURA_OUTPUT_BACKEND = $Backend",
            "SAKURA_UTF16_BACKEND = 'cpp'",
            "SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'",
            "SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'",
            "SAKURA_GENERATE_ASSEMBLY_LISTINGS = 'false'",
            "SKIP_CREATE_GITHASH = '1'",
            "MSBUILDDISABLENODEREUSE = '1'",
            "VSLANG = '1033'",
        ):
            self.assertIn(marker, self.text)

    def test_manifest_and_runtime_closure_contract(self):
        for marker in (
            "record = 'output-provider-build-manifest'",
            "payloadFree = $true",
            "status = 'committed'",
            "runtimeClosureMode = 'exe-only'",
            "exe-only|tests1={0}|size={1}",
            "output-provider-build-manifest.json",
            "atomic-directory-rename",
            "manifestGeneratedByProducer = $true",
        ):
            self.assertIn(marker, self.text)

    def test_selector_and_archive_proof_are_exact(self):
        for symbol in (
            "sakura_output_provider_create_v1",
            "sakura_output_provider_apply_v1",
            "sakura_output_provider_snapshot_measure_v1",
            "sakura_output_provider_snapshot_write_v1",
            "sakura_output_provider_active_channel_v1",
            "sakura_output_provider_stop_v1",
            "sakura_output_provider_destroy_v1",
        ):
            self.assertIn(symbol, self.text)
        for marker in (
            "dumpbin-unresolved-refs-verified",
            "dumpbin-defined-exports-verified",
            "selectorContractSha256",
            "archive-result=dumpbin-defined-exports-verified",
        ):
            self.assertIn(marker, self.text)

    def test_probe_and_copy_use_transaction_artifact(self):
        self.assertIn(
            "CWorkbenchRuntime.CompileSelectedOutputProviderOwnsTheRuntimeLifecycle",
            self.text,
        )
        copy_at = self.text.index("[IO.File]::Copy($testsSource, $copiedTests, $false)")
        probe_at = self.text.index("-FileName $copiedTests")
        publish_at = self.text.index("[IO.Directory]::Move($transaction, $finalRoot)")
        self.assertLess(copy_at, probe_at)
        self.assertLess(probe_at, publish_at)

    def test_configuration_wide_lock_and_clean_source(self):
        self.assertIn("('{0}.lock' -f $Configuration.ToLowerInvariant())", self.text)
        self.assertNotIn("$Backend, $Configuration.ToLowerInvariant()).lock", self.text)
        self.assertIn("$lockOwned = $false", self.text)
        self.assertIn("if ($lockOwned -and [IO.File]::Exists($lockPath))", self.text)
        self.assertIn("Qualified provider artifact production requires a clean checkout", self.text)
        self.assertGreaterEqual(self.text.count("Assert-SourceStateEqual $sourceBefore"), 2)

    def test_docs_keep_startup_and_provider_receipts_distinct(self):
        for marker in (
            "prepare-output-startup-artifact.ps1",
            "build-sln.bat",
            "runtimeClosureMode=exe-only",
            "not a GUI dependency-stage receipt",
            "HOLD",
        ):
            self.assertIn(marker, self.doc)

    def test_self_test_passes_on_both_powershell_hosts(self):
        shells = [name for name in ("powershell.exe", "pwsh") if shutil.which(name)]
        if not shells:
            self.skipTest("No PowerShell host is available")
        for shell in shells:
            completed = subprocess.run(
                [
                    shell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(SCRIPT),
                    "-SelfTest",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertIn(
                "PASS prepare-output-provider-artifact.ps1 self-tests",
                completed.stdout,
            )

    def test_uppercase_backend_fails_before_native_work(self):
        shell = next(
            (name for name in ("powershell.exe", "pwsh") if shutil.which(name)),
            None,
        )
        if shell is None:
            self.skipTest("No PowerShell host is available")
        completed = subprocess.run(
            [
                shell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(SCRIPT),
                "-SelfTest",
                "-Backend",
                "CPP",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(1, completed.returncode)
        compact_stderr = "".join(completed.stderr.split())
        self.assertIn("Selectorexactnessself-testfailed", compact_stderr)


if __name__ == "__main__":
    unittest.main()
