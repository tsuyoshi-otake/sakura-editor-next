from __future__ import annotations

import copy
import hashlib
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

from sakura_build_lib.output_link_size_evidence import (  # noqa: E402
    OutputLinkSizeEvidenceError,
    build_output_link_size_evidence,
    validate_output_link_size_evidence,
)


EXPECTED_PROVIDER_SYMBOLS = [
    "sakura_output_provider_active_channel_v1",
    "sakura_output_provider_apply_v1",
    "sakura_output_provider_create_v1",
    "sakura_output_provider_destroy_v1",
    "sakura_output_provider_snapshot_measure_v1",
    "sakura_output_provider_snapshot_write_v1",
    "sakura_output_provider_stop_v1",
]


def _hash(value: bytes | str) -> str:
    if isinstance(value, str):
        value = value.encode("utf-8")
    return hashlib.sha256(value).hexdigest()


def _canonical_hash(value: object) -> str:
    return _hash(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")))


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def _rehash_native(native: dict[str, object]) -> None:
    stable = {
        key: value
        for key, value in native.items()
        if key not in {"schema_version", "collection_ok", "hard_evidence_hash", "hardEvidenceSha256"}
    }
    native["hard_evidence_hash"] = _canonical_hash(stable)


def _fixture(
    root: Path,
    *,
    rust_size: int = 13,
    provider_scope: bool = False,
    startup_manifest: bool = False,
    configuration: str = "Release",
) -> tuple[Path, Path, Path, Path, dict[str, object], dict[str, object]]:
    source_commit = "a" * 40
    source_status = _hash("source-status")
    archive_hash = _hash("sakura-native-ffi")
    archive_size = 123456
    manifests: dict[str, dict[str, object]] = {}
    natives: dict[str, dict[str, object]] = {}

    for backend, image_bytes in (("cpp", b"cpp-output-16"), ("rust", b"r" * rust_size)):
        image_path = root / "images" / f"{backend}" / "sakura.exe"
        image_path.parent.mkdir(parents=True, exist_ok=True)
        image_path.write_bytes(image_bytes)
        map_path = root / "maps" / f"{backend}.map"
        map_path.parent.mkdir(parents=True, exist_ok=True)
        map_bytes = f"{backend}-map\n".encode("ascii")
        map_path.write_bytes(map_bytes)

        link_command_hash = _hash(f"{backend}-link-command")
        member_name = f"{backend}/output_provider.obj"
        native: dict[str, object] = {
            "schema_version": 4,
            "collection_ok": True,
            "build_observed": True,
            "product_id": "sakura_app",
            "context_id": f"msvc-x64-{configuration.lower()}",
            "backend": "msbuild",
            "link": {
                "output": image_path.relative_to(root).as_posix(),
                "product_hash": _hash(image_bytes),
                "linkCommandSha256": link_command_hash,
                "repository_libraries": ["build/lib/sakura_native_ffi.lib"],
                "rustArchiveSha256": archive_hash,
                "rustArchiveSizeBytes": archive_size,
                "rustArchiveCount": 1,
                "selected_archive_members_observed": True,
                "selected_archive_member_evidence": {
                    "method": "msvc_map_minus_direct_link_inputs",
                    "map": map_path.relative_to(root).as_posix(),
                    "map_hash": _hash(map_bytes),
                    "map_size_bytes": len(map_bytes),
                    "archive": "sakura_native_ffi.lib",
                    "members": [member_name],
                    "member_count": 1,
                },
            },
        }
        if provider_scope:
            native["link"]["output_provider_member_evidence"] = {
                "observed": True,
                "provider": "output-provider",
                "map": map_path.relative_to(root).as_posix(),
                "map_hash": _hash(map_bytes),
                "map_size_bytes": len(map_bytes),
                "members": [member_name],
                "member_count": 1,
            }
            native["link"]["output_provider_symbol_evidence"] = {
                "observed": True,
                "scope": "output-provider",
                "symbols": list(EXPECTED_PROVIDER_SYMBOLS),
                "symbol_count": len(EXPECTED_PROVIDER_SYMBOLS),
                "duplicate_count": 0,
            }
        _rehash_native(native)
        natives[backend] = native

        is_debug = configuration == "Debug"
        proof: dict[str, object] = {
            "result": "dumpbin-unresolved-refs-verified" if is_debug else "msvc-ltcg-compile-selector-verified",
            "outputBackend": backend,
            "utf16Backend": "cpp",
            "outputProductionPackage": False,
            "utf16ProductionPackage": False,
            "compileCommandHasGl": not is_debug,
            "compileLogProof": not is_debug,
            "compileCommandRustSelectorDefineCount": 0 if is_debug or backend == "cpp" else 1,
            "rustArchiveResult": "dumpbin-defined-exports-verified",
            "rustArchiveSha256": archive_hash,
            "rustArchiveSizeBytes": archive_size,
            "definedProviderSymbols": list(EXPECTED_PROVIDER_SYMBOLS),
            "definedProviderSymbolCount": len(EXPECTED_PROVIDER_SYMBOLS),
            "unresolvedProviderSymbols": [] if backend == "cpp" else list(EXPECTED_PROVIDER_SYMBOLS),
            "unresolvedProviderSymbolCount": 0 if backend == "cpp" else len(EXPECTED_PROVIDER_SYMBOLS),
            "selectorContractSha256": _hash(f"{backend}-selector-contract"),
        }
        manifest: dict[str, object] = {
            "schemaVersion": 1,
            "record": "output-startup-build-manifest" if startup_manifest else "output-provider-build-manifest",
            "payloadFree": True,
            "status": "committed",
            "backend": backend,
            "platform": "x64",
            "configuration": configuration,
            "sourceHead": source_commit,
            "sourceDirty": False,
            "sourceStatusSha256": source_status,
            "outputBackend": backend,
            "utf16Backend": "cpp",
            "outputProductionPackage": False,
            "utf16ProductionPackage": False,
            "selectorProof": proof,
            "selectorProofSha256": proof["selectorContractSha256"],
        }
        if startup_manifest:
            manifest.update(
                {
                    "exeSha256": _hash(image_bytes),
                    "artifactSha256After": _hash(image_bytes),
                    "artifactSizeBytesAfter": len(image_bytes),
                }
            )
        else:
            # This is deliberately a provider-shaped manifest: its artifact
            # is tests1.exe and must never be mistaken for sakura.exe.
            manifest["artifacts"] = [
                {
                    "artifact": "tests1.exe",
                    "artifactSha256": _hash(image_bytes),
                    "sizeBytes": len(image_bytes),
                }
            ]
        manifests[backend] = manifest

    paths: list[Path] = []
    for backend in ("cpp", "rust"):
        native_path = root / "evidence" / f"native-{backend}.json"
        manifest_path = root / "evidence" / f"manifest-{backend}.json"
        _write_json(native_path, natives[backend])
        _write_json(manifest_path, manifests[backend])
        paths.extend((native_path, manifest_path))
    return paths[0], paths[2], paths[1], paths[3], natives["cpp"], natives["rust"]


class OutputLinkSizeEvidenceTests(unittest.TestCase):
    def test_realistic_native_shape_does_not_overclaim_generic_archive_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(root)
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertTrue(report["source"]["complete"])
            self.assertEqual("incomplete", report["status"])
            self.assertIn("PROVIDER_MEMBER_UNPROVEN", report["failures"])
            self.assertIn("FINAL_PROVIDER_SYMBOL_UNPROVEN", report["failures"])
            self.assertIn("FINAL_IMAGE_UNPROVEN", report["failures"])
            self.assertIsNone(report["link"]["cpp"]["mapSha256"])
            self.assertIsNone(report["link"]["cpp"]["providerSymbolCount"])

    def test_complete_report_cross_checks_image_link_map_archive_and_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("complete", report["status"])
            self.assertTrue(report["sizeGate"]["pass"])
            self.assertEqual(0, report["sizeGate"]["deltaBytes"])
            self.assertEqual(1, report["link"]["cpp"]["staticRustArchiveCount"])
            self.assertEqual(1, report["link"]["rust"]["selectedMemberCount"])
            self.assertEqual(7, report["link"]["cpp"]["providerSymbolCount"])
            self.assertEqual(0, report["link"]["rust"]["duplicateProviderSymbolCount"])
            self.assertEqual("HOLD", report["decision"])
            self.assertFalse(report["adoptionEligible"])
            serialized = json.dumps(report, ensure_ascii=False, sort_keys=True)
            for secret in ("sakura.exe", ".map", "output_provider.obj", "images/cpp"):
                self.assertNotIn(secret, serialized)
            self.assertTrue(validate_output_link_size_evidence(report)["ok"])

    def test_debug_startup_manifest_uses_unresolved_selector_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True, configuration="Debug"
            )
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("complete", report["status"])
            self.assertEqual("dumpbin-unresolved-refs-verified", report["selectors"]["rust"]["proofResult"])
            self.assertTrue(validate_output_link_size_evidence(report)["ok"])

    def test_manifest_backend_and_output_selector_must_agree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["backend"] = "cpp"
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("SELECTOR_MISMATCH", report["failures"])

    def test_selector_proof_hash_must_match_its_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["selectorProofSha256"] = _hash("wrong-selector-contract")
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("SELECTOR_PROOF_HASH_MISMATCH", report["failures"])

    def test_native_archive_identity_must_match_selector_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["selectorProof"]["rustArchiveSha256"] = _hash("different-archive")
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("STATIC_RUST_ARCHIVE_MISMATCH", report["failures"])

    def test_source_and_configuration_mismatches_are_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["sourceHead"] = "b" * 40
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertEqual("incomplete", report["status"])
            self.assertIn("SOURCE_MISMATCH", report["failures"])
            self.assertFalse(validate_output_link_size_evidence(report)["ok"])

            rust_manifest_value["sourceHead"] = "a" * 40
            rust_manifest_value["configuration"] = "Debug"
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertEqual("incomplete", report["status"])
            self.assertIn("PLATFORM_CONFIGURATION_MISMATCH", report["failures"])

    def test_missing_map_or_members_never_becomes_proven(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            cpp_value["link"]["output_provider_member_evidence"] = None
            cpp_value["link"]["output_provider_symbol_evidence"] = None
            _rehash_native(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("MAP_UNPROVEN", report["failures"])
            self.assertIn("PROVIDER_MEMBER_UNPROVEN", report["failures"])
            self.assertIn("FINAL_PROVIDER_SYMBOL_UNPROVEN", report["failures"])
            self.assertFalse(report["sizeGate"]["pass"])

    def test_duplicate_archive_and_provider_symbols_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, rust_value = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            rust_value["link"]["repository_libraries"].append("other/sakura_native_ffi.lib")
            rust_value["link"]["output_provider_symbol_evidence"]["duplicate_count"] = 1
            _rehash_native(rust_value)
            _write_json(rust_native, rust_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("DUPLICATE_STATIC_RUST_ARCHIVE", report["failures"])
            self.assertIn("DUPLICATE_PROVIDER_SYMBOLS", report["failures"])

    def test_size_gate_failure_is_complete_but_not_eligible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, rust_value = _fixture(
                root, rust_size=128, provider_scope=True, startup_manifest=True
            )
            rust_image = root / "images" / "rust" / "sakura.exe"
            rust_bytes = rust_image.read_bytes()
            rust_value["link"]["product_hash"] = _hash(rust_bytes)
            _rehash_native(rust_value)
            _write_json(rust_native, rust_value)
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["exeSha256"] = _hash(rust_bytes)
            rust_manifest_value["artifactSizeBytesAfter"] = len(rust_bytes)
            _write_json(rust_manifest, rust_manifest_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertEqual("complete", report["status"])
            self.assertFalse(report["sizeGate"]["pass"])
            self.assertGreater(report["sizeGate"]["deltaPercent"], 5.0)
            self.assertFalse(validate_output_link_size_evidence(report)["ok"])

    def test_size_gate_accepts_a_rust_image_smaller_than_cpp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, rust_size=5, provider_scope=True, startup_manifest=True
            )
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("complete", report["status"])
            self.assertLess(report["sizeGate"]["deltaBytes"], 0)
            self.assertLess(report["sizeGate"]["deltaPercent"], 0.0)
            self.assertTrue(validate_output_link_size_evidence(report)["ok"])

    def test_map_tamper_and_unsafe_path_are_payload_free_failures(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            map_path = root / cpp_value["link"]["output_provider_member_evidence"]["map"]
            map_path.write_bytes(b"tampered-map\n")
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("MAP_HASH_MISMATCH", report["failures"])

            cpp_value["link"]["output_provider_member_evidence"]["map"] = "../outside.map"
            _rehash_native(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("MAP_PATH_UNSAFE", report["failures"])
            serialized = json.dumps(report, ensure_ascii=False, sort_keys=True)
            self.assertNotIn("outside.map", serialized)

    def test_validator_rejects_unknown_fields_and_tampered_report_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            unknown = copy.deepcopy(report)
            unknown["unknown"] = True
            with self.assertRaisesRegex(OutputLinkSizeEvidenceError, "schema"):
                validate_output_link_size_evidence(unknown)
            tampered = copy.deepcopy(report)
            tampered["images"]["cpp"]["sizeBytes"] += 1
            with self.assertRaisesRegex(OutputLinkSizeEvidenceError, "hash"):
                validate_output_link_size_evidence(tampered)

    def test_cli_writes_report_and_returns_success_for_a_qualified_gate(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPOSITORY_ROOT) as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, rust_value = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            relative_prefix = root.relative_to(REPOSITORY_ROOT).as_posix()
            for backend, native_path, native_value in (
                ("cpp", cpp_native, cpp_value),
                ("rust", rust_native, rust_value),
            ):
                native_value["link"]["output"] = f"{relative_prefix}/images/{backend}/sakura.exe"
                native_value["link"]["output_provider_member_evidence"]["map"] = (
                    f"{relative_prefix}/maps/{backend}.map"
                )
                _rehash_native(native_value)
                _write_json(native_path, native_value)
            output_path = root / "evidence" / "output-link-size.json"
            command = [
                sys.executable,
                str(REPOSITORY_ROOT / "tools/build/sakura_build.py"),
                "--repo-root",
                str(REPOSITORY_ROOT),
                "--format",
                "json",
                "evidence",
                "output-link-size",
                "--cpp-native-evidence",
                str(cpp_native),
                "--rust-native-evidence",
                str(rust_native),
                "--cpp-manifest",
                str(cpp_manifest),
                "--rust-manifest",
                str(rust_manifest),
                "--output",
                str(output_path),
            ]
            completed = subprocess.run(command, cwd=REPOSITORY_ROOT, capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertTrue(output_path.is_file())
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual("complete", report["status"])
            self.assertTrue(validate_output_link_size_evidence(report)["ok"])


if __name__ == "__main__":
    unittest.main()
