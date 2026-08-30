"""Release-promotion build authority and smoke contracts for Issue #279."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
RELEASE_PROMOTION_WORKFLOW = REPO_ROOT / ".github/workflows/release-promotion.yml"
PROVENANCE_SCRIPT = REPO_ROOT / ".github/scripts/Write-ReleaseProvenance.ps1"
SMOKE_SCRIPT = REPO_ROOT / ".github/scripts/Test-ReleaseDistribution.ps1"
EXACT_BUILD_CONTRACT = {
    "platform": "x64",
    "configuration": "Release",
    "utf16_backend": "cpp",
    "output_backend": "cpp",
    "utf16_production_package": "true",
    "output_production_package": "true",
}


def _read_powershell(path: Path) -> str:
    raw = path.read_bytes()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig")


def _function_body(source: str, name: str) -> str:
    marker = f"function {name} {{"
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"PowerShell function {name!r} is missing")
    depth = 0
    for index in range(start + len(marker) - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"PowerShell function {name!r} has unbalanced braces")


def _powershell_hosts() -> tuple[str, str]:
    windows_powershell = shutil.which("powershell.exe")
    powershell = shutil.which("pwsh.exe")
    if windows_powershell is None or powershell is None:
        raise AssertionError("release smoke contracts require powershell.exe and pwsh.exe")
    return windows_powershell, powershell


def _run_build_contract_fixture(host: str, provenance: object) -> subprocess.CompletedProcess[str]:
    smoke_source = _read_powershell(SMOKE_SCRIPT)
    validator = _function_body(smoke_source, "Assert-ReleaseBuildContract")
    fixture = "\n".join(
        (
            "$ErrorActionPreference = 'Stop'",
            validator,
            "$provenance = $env:RELEASE_BUILD_CONTRACT | ConvertFrom-Json",
            "Assert-ReleaseBuildContract -Provenance $provenance",
        )
    )
    with tempfile.TemporaryDirectory() as temporary_directory:
        fixture_path = Path(temporary_directory) / "release-build-contract-fixture.ps1"
        fixture_path.write_text(fixture, encoding="utf-8")
        environment = os.environ.copy()
        environment["RELEASE_BUILD_CONTRACT"] = json.dumps(provenance, separators=(",", ":"))
        return subprocess.run(
            (
                host,
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(fixture_path),
            ),
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )


class ReleasePromotionWorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8-sig")
        promotion_start = cls.workflow.index("    - name: MSBuild release promotion\n")
        ordinary_start = cls.workflow.index("    - name: MSBuild\n", promotion_start)
        cls.promotion = cls.workflow[promotion_start:ordinary_start]
        ordinary_end = cls.workflow.index("    - name: Restore CHM artifacts\n", ordinary_start)
        cls.ordinary = cls.workflow[ordinary_start:ordinary_end]
        build_job_start = cls.workflow.index("  build:\n")
        cls.build_job_prelude = cls.workflow[build_job_start:promotion_start]

    def test_release_compile_sets_all_cpp_production_values_before_build(self) -> None:
        expected_assignments = (
            "        SAKURA_UTF16_BACKEND: cpp",
            "        SAKURA_OUTPUT_BACKEND: cpp",
            "        SAKURA_UTF16_PRODUCTION_PACKAGE: true",
            "        SAKURA_OUTPUT_PRODUCTION_PACKAGE: true",
        )
        build = self.promotion.index("call build-sln.bat")
        for assignment in expected_assignments:
            self.assertEqual(1, self.promotion.count(assignment))
            self.assertLess(self.promotion.index(assignment), build)
            self.assertNotIn(assignment, self.build_job_prelude)

    def test_legacy_tag_build_keeps_source_sha_child_only(self) -> None:
        self.assertIn("if: ${{ inputs.release_promotion }}", self.promotion)
        self.assertIn('set "GITHUB_SHA=${{ env.RELEASE_SOURCE_SHA }}"', self.promotion)
        self.assertNotIn("GITHUB_SHA:", self.promotion)
        self.assertIn(
            "call build-sln.bat ${{ matrix.platform }} ${{ matrix.config }}", self.promotion
        )

    def test_ordinary_build_branch_is_preserved(self) -> None:
        expected = """\
    - name: MSBuild
      if: ${{ !inputs.release_promotion }}
      env:
        # Rollback-first keeps C++ authoritative while the linked Rust
        # candidate remains available to the focused differential tests.
        SAKURA_UTF16_BACKEND: cpp
        SAKURA_UTF16_PRODUCTION_PACKAGE: true
      run: build-sln.bat ${{ matrix.platform }} ${{ matrix.config }}
      shell: cmd

"""
        self.assertEqual(expected, self.ordinary)

    def test_provenance_step_passes_the_exact_build_contract(self) -> None:
        start = self.workflow.index("    - name: Record release provenance\n")
        end = self.workflow.index("    - name: Upload release provenance\n", start)
        step = self.workflow[start:end]
        for variable, value in (
            ("SAKURA_UTF16_BACKEND", "cpp"),
            ("SAKURA_OUTPUT_BACKEND", "cpp"),
            ("SAKURA_UTF16_PRODUCTION_PACKAGE", "true"),
            ("SAKURA_OUTPUT_PRODUCTION_PACKAGE", "true"),
        ):
            self.assertIn(f"        {variable}: {value}", step)
        for argument in (
            '-Platform "${{ matrix.platform }}"',
            '-Configuration "${{ matrix.config }}"',
            '-Utf16Backend "$env:SAKURA_UTF16_BACKEND"',
            '-OutputBackend "$env:SAKURA_OUTPUT_BACKEND"',
            '-Utf16ProductionPackage "$env:SAKURA_UTF16_PRODUCTION_PACKAGE"',
            '-OutputProductionPackage "$env:SAKURA_OUTPUT_PRODUCTION_PACKAGE"',
        ):
            self.assertIn(argument, step)

    def test_provenance_serializes_all_exact_contract_fields(self) -> None:
        source = _read_powershell(PROVENANCE_SCRIPT)
        self.assertIn("build_contract = $buildContract", source)
        for name, value in EXACT_BUILD_CONTRACT.items():
            self.assertIn(f"{name} = '{value}'", source)

    def test_distribution_smoke_uses_a_runner_compatible_with_the_installer(self) -> None:
        text = RELEASE_PROMOTION_WORKFLOW.read_text(encoding="utf-8-sig")
        smoke_start = text.index("  smoke:\n")
        publish_start = text.index("  publish:\n", smoke_start)
        smoke = text[smoke_start:publish_start]
        self.assertIn("    runs-on: windows-2025\n", smoke)
        self.assertNotIn("runs-on: windows-2022", smoke)
        self.assertIn("    - name: Verify smoke runner compatibility\n", smoke)
        self.assertIn("$minimum = [Version]::new(10, 0, 22000)", smoke)

    def test_smoke_validates_contract_before_archive_or_process_execution(self) -> None:
        source = _read_powershell(SMOKE_SCRIPT)
        validation = source.index("Assert-ReleaseBuildContract -Provenance $provenance")
        archive = source.index("$installerArchive = Get-ArchiveRecord", validation)
        process = source.index("$installerProcess = Start-Process", validation)
        self.assertLess(validation, archive)
        self.assertLess(validation, process)


class ReleaseBuildContractExecutableTests(unittest.TestCase):
    def test_both_powershell_hosts_accept_only_the_exact_contract(self) -> None:
        for host in _powershell_hosts():
            with self.subTest(host=host):
                completed = _run_build_contract_fixture(
                    host, {"build_contract": EXACT_BUILD_CONTRACT}
                )
                self.assertEqual(0, completed.returncode, completed.stderr)

    def test_both_powershell_hosts_reject_missing_wrong_and_non_string_values(self) -> None:
        tampered: list[object] = [
            {},
            {"build_contract": {}},
            {
                "build_contract": {
                    key: value
                    for key, value in EXACT_BUILD_CONTRACT.items()
                    if key != "platform"
                }
            },
            {"build_contract": {**EXACT_BUILD_CONTRACT, "unexpected": "value"}},
        ]
        wrong_values = {
            "platform": "arm64",
            "configuration": "Debug",
            "utf16_backend": "rust",
            "output_backend": "rust",
            "utf16_production_package": "false",
            "output_production_package": "false",
        }
        for key in EXACT_BUILD_CONTRACT:
            wrong = dict(EXACT_BUILD_CONTRACT)
            wrong[key] = wrong_values[key]
            tampered.append({"build_contract": wrong})
            non_string = dict(EXACT_BUILD_CONTRACT)
            non_string[key] = True
            tampered.append({"build_contract": non_string})

        for host in _powershell_hosts():
            for provenance in tampered:
                with self.subTest(host=host, provenance=provenance):
                    completed = _run_build_contract_fixture(host, provenance)
                    self.assertNotEqual(0, completed.returncode)


if __name__ == "__main__":
    unittest.main()
