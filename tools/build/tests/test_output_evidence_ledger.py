from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = TOOLS_BUILD.parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.output_evidence_ledger import (  # noqa: E402
    OutputEvidenceLedgerError,
    append_output_evidence,
    canonical_json_sha256,
    extract_output_evidence,
    verify_output_evidence_ledger,
)


def _hash(digit: str) -> str:
    return digit * 64


def _fixture(backend: str, configuration: str, *, status: str = "passed", run: int = 1) -> dict[str, object]:
    """Return a producer-shaped, payload-free report used by the contract tests."""

    return {
        "schemaVersion": 1,
        "record": "output-startup-build-manifest",
        "payloadFree": True,
        "backend": backend,
        "outputBackend": backend,
        "utf16Backend": "cpp",
        "configuration": {
            "platform": "x64",
            "configuration": configuration,
            "environment": {
                "SAKURA_OUTPUT_BACKEND": backend,
                "SAKURA_UTF16_BACKEND": "cpp",
                "SAKURA_OUTPUT_PRODUCTION_PACKAGE": False,
                "SAKURA_UTF16_PRODUCTION_PACKAGE": False,
            },
        },
        "sourceCommit": "a" * 40,
        "sourceDirty": False,
        "sourceStatusSha256": _hash("b"),
        "host": {
            "osVersion": "Windows 11 10.0.26200.0",
            "architecture": "X64",
            "cpuManufacturer": "AuthenticAMD",
            "cpuModel": "AMD Ryzen 7 9700X",
            "physicalCores": 8,
            "logicalProcessors": 16,
        },
        "toolchain": {
            "msvc": "MSVC 19.40",
            "rust": "rustc 1.88.0",
            "rustLockSha256": _hash("c"),
            "packagePlanSha256": _hash("d"),
            "buildCommandSha256": _hash("e"),
            "packagePlanCommandSha256": _hash("f"),
            "runtimeStageCommandSha256": _hash("0"),
        },
        "packagePlanSha256": _hash("d"),
        "dependencyClosureSha256": _hash("1"),
        "packageRestore": {"status": "ok"},
        "sample": {"sha256": _hash("2"), "sizeBytes": 4096},
        "seed": 274001,
        "scripts": {"pairedRunnerSha256": _hash("3")},
        "selectorProof": {"result": "verified", "selectorProofSha256": _hash("5")},
        "artifacts": [
            {
                "backend": backend,
                "artifactSha256": _hash("4" if run == 1 else "6"),
                "sizeBytes": 1000 + run,
            }
        ],
        "status": status,
        "pass": status == "passed",
        "startupGatePass": status == "passed",
        "acceptance": {
            "qualified": status == "passed",
            "failedLaunches": 0 if status == "passed" else 1,
        },
        "performance": {"pass": status == "passed"},
        "cleanup": {"allCleanupVerified": status == "passed", "survivorCount": 0 if status == "passed" else 1},
        "termination": {
            "status": "completed" if status == "passed" else "terminated",
            "type": "none" if status == "passed" else "launch-failure",
            "exitCode": 0 if status == "passed" else 1,
        },
        "diagnostics": {
            "lineCount": 12,
            "errorCodes": {"C1083": 1 if status != "passed" else 0},
        },
        "actionCounts": {"cl": 1, "link": 1},
        "durationSeconds": 1.25,
        "runId": run,
    }


def _incremental_verifier_fixture() -> dict[str, object]:
    """Return a passed six-phase verifier report with bounded nested status data."""

    phase_names = ("baseline", "no_op_1", "no_op_2", "no_op_3", "rust_source", "cpp_provider")
    action_counts = {
        "cargo": 1,
        "cargo-preflight": 0,
        "rustc": 0,
        "cl": 1,
        "link": 1,
        "lib": 0,
        "rc": 0,
        "mt": 0,
        "cmake": 0,
        "senp-tool": 0,
        "vcpkg-applocal": 1,
        "delete": 0,
        "unexpected_tool": 0,
    }
    phases = []
    for index, name in enumerate(phase_names):
        phases.append(
            {
                "name": name,
                "result": {
                    "type": "ok",
                    "phase": name,
                    "exitCode": 0,
                    "durationSeconds": 1.0 + index,
                    "observedExpectedHelperCount": 0,
                    "postCleanupSurvivorCount": 0,
                },
                "diagnostics": {
                    "available": True,
                    "byteCount": 1000 + index,
                    "lineCount": 10 + index,
                    "sha256": _hash(str(index + 1)),
                    "errorCodes": {"C180": 1},
                    "errorCodesTruncated": False,
                },
                "diagnosticsParseFailed": False,
                "actionCounts": action_counts,
                "actions": [
                    {
                        "phase": name,
                        "kind": "cl",
                        "operation": "invoke",
                        "project": "sakura_core/sakura.vcxproj",
                        "sourcePaths": "sakura_core/workbench/output/OutputServiceRustProvider.cpp",
                    }
                ],
                "actionRecordCount": 1,
                "retainedActionCount": 1,
                "unretainedActionCount": 0,
                "workActionCount": 1,
                "closure": {"type": "ok", "phase": name},
                "closureProofAvailable": True,
                "artifactChanges": [],
                "artifactsBefore": [],
                "artifactsAfter": [],
                "logAvailable": True,
                "actionsTruncated": False,
                "unexpectedToolNames": [],
                "unexpectedToolNamesTruncated": False,
            }
        )
    return {
        "schemaVersion": 1,
        "verifier": "verify-native-rust-incremental.ps1",
        "payloadFree": True,
        "status": "passed",
        "phaseOrder": list(phase_names),
        "phases": phases,
        "configuration": {
            "platform": "x64",
            "configuration": "Debug",
            "environment": {
                "SAKURA_OUTPUT_BACKEND": "cpp",
                "SAKURA_UTF16_BACKEND": "cpp",
                "SAKURA_OUTPUT_PRODUCTION_PACKAGE": "false",
                "SAKURA_UTF16_PRODUCTION_PACKAGE": "false",
            },
        },
        "sourceCommit": "a" * 40,
        "sourceDirty": False,
        "sourceStatusSha256": _hash("b"),
        "host": {
            "osVersion": "Windows 11 10.0.26200.0",
            "architecture": "X64",
            "cpuManufacturer": "AuthenticAMD",
            "cpuModel": "AMD Ryzen 7 9700X",
            "physicalCores": 8,
            "logicalProcessors": 16,
        },
        "toolchain": {
            "msvc": "MSVC 19.40",
            "rust": "rustc 1.88.0",
            "rustLockSha256": _hash("c"),
            "packagePlanSha256": _hash("d"),
        },
        "packagePlanSha256": _hash("d"),
        "dependencyClosureSha256": _hash("e"),
        "packageRestore": {"status": "ok"},
        "sample": {"sha256": _hash("f"), "sizeBytes": 4096},
        "seed": 274001,
        "scripts": {"pairedRunnerSha256": _hash("0")},
    }


class OutputEvidenceLedgerTests(unittest.TestCase):
    def test_paired_provider_analysis_requires_both_observed_artifacts(self) -> None:
        source = _fixture("cpp", "Debug")
        source["record"] = "analysis"
        source["backend"] = "paired(cpp,rust)"
        source["outputBackend"] = "paired(cpp,rust)"
        source["provenance"] = {"outputBackend": "paired(cpp,rust)", "utf16Backend": "cpp"}
        source["artifacts"] = [
            {"backend": "cpp", "artifactSha256": _hash("4"), "sizeBytes": 1001},
            {"backend": "rust", "artifactSha256": _hash("6"), "sizeBytes": 1002},
        ]
        extracted = extract_output_evidence(source)
        self.assertEqual("paired", extracted["backend"])
        self.assertEqual(["cpp", "rust"], extracted["backends"])

        incomplete = copy.deepcopy(source)
        incomplete["artifacts"] = incomplete["artifacts"][:1]
        with self.assertRaisesRegex(OutputEvidenceLedgerError, "paired selector"):
            extract_output_evidence(incomplete)

    def test_cpp_and_rust_debug_and_release_cells_are_derived(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            expected = [(backend, configuration) for backend in ("cpp", "rust") for configuration in ("Debug", "Release")]
            for run, (backend, configuration) in enumerate(expected, start=1):
                result = append_output_evidence(ledger, _fixture(backend, configuration, run=run))
                self.assertEqual(result["sequence"], run)

            verification = verify_output_evidence_ledger(ledger)
            self.assertTrue(verification["ok"], verification)
            self.assertEqual(
                [(item["backend"], item["configuration"]) for item in verification["records"]],
                expected,
            )

    def test_duplicate_source_is_rejected_without_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            source = _fixture("cpp", "Debug")
            append_output_evidence(ledger, source)
            record = next((ledger / "records").iterdir())
            before = record.read_bytes()
            with self.assertRaisesRegex(OutputEvidenceLedgerError, "already"):
                append_output_evidence(ledger, copy.deepcopy(source))
            self.assertEqual(record.read_bytes(), before)
            self.assertEqual(len(list((ledger / "records").iterdir())), 1)

    def test_tamper_and_temporary_artifact_are_detected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            append_output_evidence(ledger, _fixture("cpp", "Debug"))
            record = next((ledger / "records").iterdir())
            value = json.loads(record.read_text(encoding="utf-8"))
            value["results"]["pass"] = False
            record.write_text(json.dumps(value), encoding="utf-8", newline="\n")
            verification = verify_output_evidence_ledger(ledger)
            self.assertFalse(verification["ok"])
            self.assertIn("OUTPUT_LEDGER_RECORD_HASH", {item["code"] for item in verification["failures"]})

            record.write_bytes(b"{}")
            (ledger / "records" / "record-00000001-corrupt.json.tmp").write_bytes(b"partial")
            verification = verify_output_evidence_ledger(ledger)
            self.assertFalse(verification["ok"])
            self.assertIn("OUTPUT_LEDGER_TEMP_ARTIFACT", {item["code"] for item in verification["failures"]})

    def test_append_lock_is_fail_closed_and_is_not_a_record(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            ledger.mkdir()
            lock = ledger / ".append.lock"
            lock.write_bytes(b"")
            verification = verify_output_evidence_ledger(ledger)
            self.assertFalse(verification["ok"])
            self.assertEqual(verification["failures"], [{"code": "OUTPUT_LEDGER_APPEND_LOCK"}])
            with self.assertRaisesRegex(OutputEvidenceLedgerError, "append lock"):
                append_output_evidence(ledger, _fixture("cpp", "Debug"))

    def test_failed_result_status_is_retained_and_adoption_stays_hold(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            append_output_evidence(ledger, _fixture("rust", "Release", status="failed"))
            record_path = next((ledger / "records").iterdir())
            record = json.loads(record_path.read_text(encoding="utf-8"))
            self.assertEqual(record["decision"], "HOLD")
            self.assertFalse(record["adoptionEligible"])
            self.assertEqual(record["results"]["sourceStatus"], "failed")
            self.assertEqual(record["results"]["failureType"], "launch-failure")
            self.assertEqual(record["results"]["survivorCount"], 1)
            values = {item["field"]: item["value"] for item in record["results"]["statuses"]}
            self.assertEqual(values["termination.exitCode"], 1)
            self.assertEqual(values["diagnostics.lineCount"], 12)
            self.assertEqual(values["diagnostics.errorCodes.C1083"], 1)
            self.assertEqual(values["actionCounts.cl"], 1)
            self.assertEqual(values["durationSeconds"], 1.25)
            self.assertTrue(any(item["value"] == "failed" for item in record["results"]["statuses"]))

    def test_payload_fields_are_rejected(self) -> None:
        for key in ("text", "path", "commandLine", "content"):
            source = _fixture("cpp", "Debug")
            source[key] = "sensitive payload"
            with self.subTest(key=key), self.assertRaisesRegex(OutputEvidenceLedgerError, "forbidden"):
                extract_output_evidence(source)

    def test_role_label_cannot_override_provider_observation(self) -> None:
        source = _fixture("cpp", "Debug")
        source["roleLabels"] = "rust"
        self.assertEqual(extract_output_evidence(source)["backend"], "cpp")

        conflicting = _fixture("cpp", "Debug")
        conflicting["outputBackend"] = "rust"
        with self.assertRaisesRegex(OutputEvidenceLedgerError, "disagree"):
            extract_output_evidence(conflicting)

    def test_canonical_hash_is_order_independent(self) -> None:
        self.assertEqual(
            canonical_json_sha256({"b": 2, "a": [True, "x"]}),
            canonical_json_sha256({"a": [True, "x"], "b": 2}),
        )

    def test_multiple_attempts_form_a_hash_chain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            first = append_output_evidence(ledger, _fixture("cpp", "Debug", run=1))
            second = append_output_evidence(ledger, _fixture("cpp", "Debug", run=2))
            records = sorted((ledger / "records").iterdir())
            second_record = json.loads(records[1].read_text(encoding="utf-8"))
            self.assertEqual(second_record["previousRecordSha256"], first["recordSha256"])
            self.assertEqual(second_record["recordSha256"], second["recordSha256"])
            self.assertTrue(verify_output_evidence_ledger(ledger)["ok"])

    def test_existing_incremental_evidence_is_accepted_as_incomplete_history(self) -> None:
        source = REPOSITORY_ROOT / "build" / "evidence" / "native-rust-incremental.json"
        if not source.is_file():
            self.skipTest("repository evidence is not present")
        with tempfile.TemporaryDirectory() as temporary:
            result = append_output_evidence(Path(temporary) / "ledger", source)
            self.assertEqual(result["backend"], "cpp")
            record = json.loads(next((Path(temporary) / "ledger" / "records").iterdir()).read_text(encoding="utf-8"))
            self.assertEqual(record["sourceEvidenceKind"], "native-rust-incremental")
            self.assertFalse(record["source"]["complete"])
            self.assertFalse(record["host"]["complete"])

    def test_passed_incremental_verifier_six_phases_respects_status_cap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            result = append_output_evidence(ledger, _incremental_verifier_fixture())
            record = json.loads(next((ledger / "records").iterdir()).read_text(encoding="utf-8"))
            self.assertEqual(result["backend"], "cpp")
            self.assertEqual(record["sourceEvidenceKind"], "native-rust-incremental")
            self.assertEqual(len(record["results"]["statuses"]), 128)
            self.assertTrue(record["results"]["statusesTruncated"])
            verification = verify_output_evidence_ledger(ledger)
            self.assertTrue(verification["ok"], verification)
            self.assertEqual(verification["recordCount"], 1)

    def test_existing_paired_and_provider_evidence_are_accepted(self) -> None:
        paired_files = sorted((REPOSITORY_ROOT / "build" / "evidence").glob("output-startup-smoke-*/results/paired-startup-*.json"))
        provider_files = sorted((REPOSITORY_ROOT / "build" / "output-provider-benchmarks").glob("output-provider-v1-*/analysis-v1.json"))
        if not paired_files or not provider_files:
            self.skipTest("repository producer evidence is not present")
        with tempfile.TemporaryDirectory() as temporary:
            ledger = Path(temporary) / "ledger"
            paired = append_output_evidence(ledger, paired_files[-1])
            provider = append_output_evidence(ledger, provider_files[-1])
            self.assertEqual(paired["backend"], "paired")
            self.assertEqual(provider["backend"], "paired")
            self.assertTrue(verify_output_evidence_ledger(ledger)["ok"])

    def test_cli_append_and_verify(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPOSITORY_ROOT) as temporary:
            root = Path(temporary)
            source = root / "source.json"
            ledger = root / "ledger"
            source.write_text(json.dumps(_fixture("rust", "Release")), encoding="utf-8", newline="\n")
            command = [
                sys.executable,
                str(TOOLS_BUILD / "sakura_build.py"),
                "--format",
                "json",
                "evidence",
                "output-append",
                "--source",
                str(source.relative_to(REPOSITORY_ROOT)),
                "--ledger-dir",
                str(ledger.relative_to(REPOSITORY_ROOT)),
            ]
            appended = subprocess.run(command, cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
            self.assertEqual(appended.returncode, 0, appended.stderr)
            command[command.index("output-append")] = "output-verify"
            command.remove("--source")
            command.remove(str(source.relative_to(REPOSITORY_ROOT)))
            verified = subprocess.run(command, cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
            self.assertEqual(verified.returncode, 0, verified.stderr)
            self.assertTrue(json.loads(verified.stdout)["ok"])


if __name__ == "__main__":
    unittest.main()
