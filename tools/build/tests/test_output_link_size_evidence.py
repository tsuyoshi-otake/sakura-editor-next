from __future__ import annotations

import copy
import hashlib
import json
import os
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
from sakura_build_lib.output_final_image_evidence import (  # noqa: E402
    bind_native_evidence_to_final_image,
    stage_output_final_image,
)
from sakura_build_lib.product_native_evidence import product_native_evidence_hash  # noqa: E402


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


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def _rehash_native(native: dict[str, object]) -> None:
    native["hard_evidence_hash"] = product_native_evidence_hash(native)


def _make_cpp_ltcg_negative_provider(native: dict[str, object]) -> None:
    """Replace provider rows with the complete Release/LTCG negative proof."""

    link = native["link"]
    member = link["output_provider_member_evidence"]
    symbols = link["output_provider_symbol_evidence"]
    member.update(
        {
            "observed": False,
            "archive_name": None,
            "contributing_archives": [],
            "contributing_archive_count": 0,
            "contributing_members": [],
            "members": [],
            "member_count": 0,
            "missing_symbols": list(EXPECTED_PROVIDER_SYMBOLS),
            "unexpected_symbols": [],
            "duplicate_count": 0,
            "contributions": [],
        }
    )
    symbols.update(
        {
            "observed": False,
            "symbols": [],
            "symbol_count": 0,
            "duplicate_count": 0,
            "missing_symbols": list(EXPECTED_PROVIDER_SYMBOLS),
            "unexpected_symbols": [],
            "contributing_archives": [],
            "contributing_members": [],
            "contributions": [],
        }
    )
    _rehash_native(native)


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
                "product_size_bytes": len(image_bytes),
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
            provider_contributions = [
                {"symbol": symbol, "archive": "sakura_native_ffi.lib", "member": member_name}
                for symbol in EXPECTED_PROVIDER_SYMBOLS
            ]
            native["link"]["output_provider_member_evidence"] = {
                "observed": True,
                "provider": "output-provider",
                "method": "msvc_map_publics_by_value_provider_rows",
                "map": map_path.relative_to(root).as_posix(),
                "map_hash": _hash(map_bytes),
                "map_size_bytes": len(map_bytes),
                "archive_name": "sakura_native_ffi.lib",
                "archive_input_count": 1,
                "contributing_archives": ["sakura_native_ffi.lib"],
                "contributing_archive_count": 1,
                "contributing_members": [member_name],
                "members": [member_name],
                "member_count": 1,
                "missing_symbols": [],
                "unexpected_symbols": [],
                "duplicate_count": 0,
                "contributions": provider_contributions,
                "selector_proof": "ltcg_compile_log_required",
            }
            native["link"]["output_provider_symbol_evidence"] = {
                "observed": True,
                "provider": "output-provider",
                "scope": "output-provider",
                "method": "msvc_map_publics_by_value_provider_rows",
                "map": map_path.relative_to(root).as_posix(),
                "map_hash": _hash(map_bytes),
                "map_size_bytes": len(map_bytes),
                "symbols": list(EXPECTED_PROVIDER_SYMBOLS),
                "symbol_count": len(EXPECTED_PROVIDER_SYMBOLS),
                "duplicate_count": 0,
                "missing_symbols": [],
                "unexpected_symbols": [],
                "contributing_archives": ["sakura_native_ffi.lib"],
                "contributing_members": [member_name],
                "contributions": provider_contributions,
                "selector_proof": "ltcg_compile_log_required",
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
                    "artifactSha256": _hash(image_bytes),
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
    def test_strict_final_image_mode_rejects_unstaged_native_inputs(self) -> None:
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
                require_immutable_stage=True,
            )
            self.assertEqual("incomplete", report["status"])
            self.assertIn("FINAL_IMAGE_STAGE_UNPROVEN", report["failures"])

    def test_immutable_stage_is_the_revalidated_image_binding(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, rust_value = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            for backend, native_path, native_value in (
                ("cpp", cpp_native, cpp_value),
                ("rust", rust_native, rust_value),
            ):
                source_native = copy.deepcopy(native_value)
                receipt = stage_output_final_image(
                    repo_root=root,
                    stage_root=root / "build" / "evidence" / "final-image",
                    backend=backend,
                    platform="x64",
                    configuration="Release",
                    source_native_evidence_sha256=native_value["hard_evidence_hash"],
                    executable_path=root / "images" / backend / "sakura.exe",
                    map_path=root / "maps" / f"{backend}.map",
                )
                bound = bind_native_evidence_to_final_image(native_value, receipt)
                self.assertEqual(source_native, native_value)
                _write_json(native_path, bound)
                if backend == "cpp":
                    cpp_bound = bound

            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
                require_immutable_stage=True,
            )
            self.assertEqual("complete", report["status"])
            (root / "images" / "cpp" / "sakura.exe").unlink()
            (root / "maps" / "cpp.map").unlink()
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
                require_immutable_stage=True,
            )
            self.assertEqual("complete", report["status"])
            staged_map = root / cpp_bound["link"]["final_image_stage"]["receipt"]
            staged_receipt = json.loads(staged_map.read_text(encoding="utf-8"))
            (root / staged_receipt["files"]["map"]["path"]).write_bytes(b"tampered-stage-map")
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
                require_immutable_stage=True,
            )
            self.assertIn("FINAL_IMAGE_STAGE_TAMPERED", report["failures"])

    def test_final_image_backend_label_cannot_override_manifest_selector(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, rust_value = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            # Deliberately stage the C++ observation in the Rust partition.  A
            # staging label alone is not selector proof; strict link-size
            # validation must compare it with the manifest's C++ selector.
            cpp_receipt = stage_output_final_image(
                repo_root=root,
                stage_root=root / "build" / "evidence" / "final-image",
                backend="rust",
                platform="x64",
                configuration="Release",
                source_native_evidence_sha256=cpp_value["hard_evidence_hash"],
                executable_path=root / "images" / "cpp" / "sakura.exe",
                map_path=root / "maps" / "cpp.map",
            )
            _write_json(cpp_native, bind_native_evidence_to_final_image(cpp_value, cpp_receipt))
            rust_receipt = stage_output_final_image(
                repo_root=root,
                stage_root=root / "build" / "evidence" / "final-image",
                backend="rust",
                platform="x64",
                configuration="Release",
                source_native_evidence_sha256=rust_value["hard_evidence_hash"],
                executable_path=root / "images" / "rust" / "sakura.exe",
                map_path=root / "maps" / "rust.map",
            )
            _write_json(rust_native, bind_native_evidence_to_final_image(rust_value, rust_receipt))
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
                require_immutable_stage=True,
            )
            self.assertIn("FINAL_IMAGE_STAGE_SELECTOR_MISMATCH", report["failures"])

    def test_reliable_native_output_selector_must_agree_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            cpp_value["outputBackend"] = "rust"
            _rehash_native(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertIn("SELECTOR_MISMATCH", report["failures"])

    def test_present_but_invalid_native_selector_is_unproven(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            cpp_value["outputBackend"] = "not-a-provider"
            _rehash_native(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("NATIVE_SELECTOR_UNPROVEN", report["failures"])

    def test_manifest_identity_aliases_must_agree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, _cpp, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            manifest = json.loads(cpp_manifest.read_text(encoding="utf-8"))
            manifest["artifactSha256"] = _hash("conflicting-image")
            _write_json(cpp_manifest, manifest)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("FINAL_IMAGE_MANIFEST_IDENTITY_MISMATCH", report["failures"])

    def test_map_reparse_ancestor_is_unproven(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root, provider_scope=True, startup_manifest=True
            )
            alias = root / "maps-alias"
            try:
                os.symlink(root / "maps", alias, target_is_directory=True)
            except OSError:
                self.skipTest("symbolic links are unavailable on this Windows host")
            member = cpp_value["link"]["output_provider_member_evidence"]
            member["map"] = "maps-alias/cpp.map"
            _rehash_native(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(cpp_native, rust_native, cpp_manifest, rust_manifest, repo_root=root)
            self.assertIn("MAP_PATH_UNSAFE", report["failures"])

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

    def test_release_cpp_ltcg_negative_projection_keeps_actual_zero_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root,
                provider_scope=True,
                startup_manifest=True,
                configuration="Release",
            )
            _make_cpp_ltcg_negative_provider(cpp_value)
            _write_json(cpp_native, cpp_value)
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("complete", report["status"])
            self.assertTrue(report["sizeGate"]["pass"])
            self.assertEqual(0, report["link"]["cpp"]["selectedMemberCount"])
            self.assertEqual(0, report["link"]["cpp"]["providerSymbolCount"])
            self.assertEqual(0, report["link"]["cpp"]["duplicateProviderSymbolCount"])
            self.assertEqual(1, report["link"]["cpp"]["staticRustArchiveCount"])
            self.assertTrue(validate_output_link_size_evidence(report)["ok"])

            malformed = copy.deepcopy(cpp_value)
            malformed["link"]["output_provider_member_evidence"]["members"] = ["partial.obj"]
            malformed["link"]["output_provider_member_evidence"]["member_count"] = 1
            _rehash_native(malformed)
            _write_json(cpp_native, malformed)
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("incomplete", report["status"])
            self.assertIn("PROVIDER_MEMBER_UNPROVEN", report["failures"])

    def test_malformed_positive_provider_projection_cannot_complete_link_size_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp_native, rust_native, cpp_manifest, rust_manifest, cpp_value, _rust = _fixture(
                root,
                provider_scope=True,
                startup_manifest=True,
                configuration="Release",
            )
            original = copy.deepcopy(cpp_value)
            member = original["link"]["output_provider_member_evidence"]
            symbols = original["link"]["output_provider_symbol_evidence"]
            first_member = member["members"][0]
            two_members = [first_member, "other-provider.obj"]
            contributions = [
                {
                    "symbol": symbol,
                    "archive": "sakura_native_ffi.lib",
                    "member": two_members[index % 2],
                }
                for index, symbol in enumerate(EXPECTED_PROVIDER_SYMBOLS)
            ]
            member["members"] = two_members
            member["member_count"] = 2
            member["contributing_members"] = two_members
            member["contributions"] = contributions
            symbols["contributing_members"] = two_members
            symbols["contributions"] = contributions
            _rehash_native(original)
            _write_json(cpp_native, original)
            report = build_output_link_size_evidence(
                cpp_native,
                rust_native,
                cpp_manifest,
                rust_manifest,
                repo_root=root,
            )
            self.assertEqual("incomplete", report["status"])
            self.assertIn("PROVIDER_MEMBER_UNPROVEN", report["failures"])

            for evidence_name in (
                "output_provider_member_evidence",
                "output_provider_symbol_evidence",
            ):
                for value in (True, 1.0):
                    malformed = copy.deepcopy(cpp_value)
                    malformed["link"][evidence_name]["contributing_archive_count"] = value
                    _rehash_native(malformed)
                    _write_json(cpp_native, malformed)
                    report = build_output_link_size_evidence(
                        cpp_native,
                        rust_native,
                        cpp_manifest,
                        rust_manifest,
                        repo_root=root,
                    )
                    self.assertEqual("incomplete", report["status"], (evidence_name, value))

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
            rust_value["link"]["product_size_bytes"] = len(rust_bytes)
            _rehash_native(rust_value)
            _write_json(rust_native, rust_value)
            rust_manifest_value = json.loads(rust_manifest.read_text(encoding="utf-8"))
            rust_manifest_value["exeSha256"] = _hash(rust_bytes)
            rust_manifest_value["artifactSha256"] = _hash(rust_bytes)
            rust_manifest_value["artifactSha256After"] = _hash(rust_bytes)
            rust_manifest_value["artifactHashAfter"] = _hash(rust_bytes)
            rust_manifest_value["artifactSizeBytesAfter"] = len(rust_bytes)
            rust_manifest_value["artifactSizeAfter"] = len(rust_bytes)
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
            for backend, native_path, native_value in (
                ("cpp", cpp_native, cpp_value),
                ("rust", rust_native, rust_value),
            ):
                receipt = stage_output_final_image(
                    repo_root=REPOSITORY_ROOT,
                    stage_root=root / "staged-final-images",
                    backend=backend,
                    platform="x64",
                    configuration="Release",
                    source_native_evidence_sha256=native_value["hard_evidence_hash"],
                    executable_path=root / "images" / backend / "sakura.exe",
                    map_path=root / "maps" / f"{backend}.map",
                )
                _write_json(native_path, bind_native_evidence_to_final_image(native_value, receipt))
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
