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
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.output_final_image_evidence import (  # noqa: E402
    OutputFinalImageEvidenceError,
    bind_native_evidence_to_final_image,
    stage_output_final_image,
    validate_bound_native_evidence_for_final_image,
    validate_output_final_image_stage,
)
from sakura_build_lib.product_native_evidence import (  # noqa: E402
    validate_output_provider_evidence_for_final_image,
    product_native_evidence_hash,
    product_native_evidence_source_hash,
)


_EXPECTED_PROVIDER_SYMBOLS = [
    "sakura_output_provider_active_channel_v1",
    "sakura_output_provider_apply_v1",
    "sakura_output_provider_create_v1",
    "sakura_output_provider_destroy_v1",
    "sakura_output_provider_snapshot_measure_v1",
    "sakura_output_provider_snapshot_write_v1",
    "sakura_output_provider_stop_v1",
]


def _strict_native_fixture() -> dict[str, object]:
    executable = b"cpp-final-image"
    map_bytes = b"cpp-map\n"
    provider_contributions = [
        {"symbol": symbol, "archive": "sakura_native_ffi.lib", "member": "provider.obj"}
        for symbol in _EXPECTED_PROVIDER_SYMBOLS
    ]
    native: dict[str, object] = {
        "schema_version": 4,
        "collection_ok": True,
        "build_observed": True,
        "build_target": "Rebuild",
        "product_id": "sakura_app",
        "context_id": "msvc-x64-release",
        "backend": "msbuild",
        "link": {
            "output": "producer/sakura.exe",
            "product_hash": hashlib.sha256(executable).hexdigest(),
            "product_size_bytes": len(executable),
            "selected_archive_members_observed": True,
            "linkCommandSha256": "sha256:" + "c" * 64,
            "rustArchiveSha256": "sha256:" + "b" * 64,
            "rustArchiveSizeBytes": 1,
            "rustArchiveCount": 1,
            "selected_archive_member_evidence": {
                "map": "producer/sakura.map",
                "map_hash": hashlib.sha256(map_bytes).hexdigest(),
                "map_size_bytes": len(map_bytes),
            },
            "output_provider_member_evidence": {
                "observed": True,
                "provider": "output-provider",
                "method": "msvc_map_publics_by_value_provider_rows",
                "map": "producer/sakura.map",
                "map_hash": hashlib.sha256(map_bytes).hexdigest(),
                "map_size_bytes": len(map_bytes),
                "archive_name": "sakura_native_ffi.lib",
                "archive_input_count": 1,
                "contributing_archives": ["sakura_native_ffi.lib"],
                "contributing_archive_count": 1,
                "contributing_members": ["provider.obj"],
                "members": ["provider.obj"],
                "member_count": 1,
                "missing_symbols": [],
                "unexpected_symbols": [],
                "duplicate_count": 0,
                "contributions": provider_contributions,
                "selector_proof": "ltcg_compile_log_required",
            },
            "output_provider_symbol_evidence": {
                "observed": True,
                "provider": "output-provider",
                "scope": "output-provider",
                "method": "msvc_map_publics_by_value_provider_rows",
                "map": "producer/sakura.map",
                "map_hash": hashlib.sha256(map_bytes).hexdigest(),
                "map_size_bytes": len(map_bytes),
                "symbols": list(_EXPECTED_PROVIDER_SYMBOLS),
                "symbol_count": len(_EXPECTED_PROVIDER_SYMBOLS),
                "duplicate_count": 0,
                "missing_symbols": [],
                "unexpected_symbols": [],
                "contributing_archives": ["sakura_native_ffi.lib"],
                "contributing_members": ["provider.obj"],
                "contributions": provider_contributions,
                "selector_proof": "ltcg_compile_log_required",
            },
        },
    }
    native["hard_evidence_hash"] = product_native_evidence_hash(native)
    return native


def _cpp_ltcg_negative_native_fixture() -> dict[str, object]:
    """Build the complete C++/LTCG proof that unused Rust exports are absent."""

    native = _strict_native_fixture()
    member = native["link"]["output_provider_member_evidence"]
    symbols = native["link"]["output_provider_symbol_evidence"]
    expected_missing = list(_EXPECTED_PROVIDER_SYMBOLS)
    member.update(
        {
            "observed": False,
            "archive_name": None,
            "contributing_archives": [],
            "contributing_archive_count": 0,
            "contributing_members": [],
            "members": [],
            "member_count": 0,
            "missing_symbols": expected_missing,
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
            "missing_symbols": expected_missing,
            "unexpected_symbols": [],
            "contributing_archives": [],
            "contributing_members": [],
            "contributions": [],
        }
    )
    native["hard_evidence_hash"] = product_native_evidence_hash(native)
    return native


class OutputFinalImageEvidenceTests(unittest.TestCase):
    def _source_files(self, root: Path) -> tuple[Path, Path]:
        executable = root / "producer" / "sakura.exe"
        map_file = root / "producer" / "sakura.map"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b"cpp-final-image")
        map_file.write_bytes(b"cpp-map\n")
        return executable, map_file

    def _stage(
        self,
        root: Path,
        backend: str = "cpp",
        source_native_evidence_sha256: str | None = None,
        native_evidence: dict[str, object] | None = None,
    ) -> dict[str, object]:
        executable, map_file = self._source_files(root)
        return stage_output_final_image(
            repo_root=root,
            stage_root=root / "staged-final-images",
            backend=backend,
            platform="x64",
            configuration="Release",
            source_native_evidence_sha256=source_native_evidence_sha256 or "sha256:" + "a" * 64,
            executable_path=executable,
            map_path=map_file,
            native_evidence=native_evidence,
        )

    def test_stage_is_create_new_backend_scoped_and_revalidates_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cpp = self._stage(root, "cpp")
            cpp_path = root / cpp["receiptPath"]
            self.assertTrue(cpp_path.is_file())
            self.assertEqual(cpp["receiptSha256"], validate_output_final_image_stage(cpp_path, repo_root=root)["receiptSha256"])
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "exists"):
                self._stage(root, "cpp")

            rust = self._stage(root, "rust")
            self.assertNotEqual(cpp["receiptPath"], rust["receiptPath"])
            self.assertEqual("rust", rust["backend"])

            staged_executable = root / cpp["files"]["exe"]["path"]
            staged_executable.write_bytes(b"tampered")
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "match"):
                validate_output_final_image_stage(cpp_path, repo_root=root)

    def test_receipt_hash_tamper_is_rejected_before_file_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = self._stage(root)
            receipt_path = root / receipt["receiptPath"]
            tampered = json.loads(receipt_path.read_text(encoding="utf-8"))
            tampered["files"]["exe"]["sizeBytes"] += 1
            receipt_path.write_text(json.dumps(tampered, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "hash"):
                validate_output_final_image_stage(receipt_path, repo_root=root)

    def test_failed_copy_and_publish_leave_no_partial_transaction(self) -> None:
        for injected, expected_code in (
            ("copy", "OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED"),
            ("publish", "OUTPUT_FINAL_IMAGE_PUBLISH_FAILED"),
        ):
            with self.subTest(injected=injected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                kwargs = {}
                if injected == "copy":
                    target = "sakura_build_lib.output_final_image_evidence._copy_stable"
                    kwargs = {"side_effect": OutputFinalImageEvidenceError(expected_code, "injected failure")}
                else:
                    target = "sakura_build_lib.output_final_image_evidence.os.rename"
                    kwargs = {"side_effect": OSError("injected failure")}
                with patch(target, **kwargs):
                    with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                        self._stage(root)
                self.assertEqual(expected_code, raised.exception.code)
                transaction_parent = root / "staged-final-images" / "x64" / "Release" / "cpp"
                self.assertEqual([], list(transaction_parent.glob(".txn-*")))

    def test_cleanup_failure_preserves_primary_and_reports_typed_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch(
                "sakura_build_lib.output_final_image_evidence._copy_stable",
                side_effect=OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", "injected failure"),
            ), patch(
                "sakura_build_lib.output_final_image_evidence.shutil.rmtree",
                side_effect=OSError("injected cleanup failure"),
            ):
                with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                    self._stage(root)
            self.assertEqual("OUTPUT_FINAL_IMAGE_TRANSACTION_CLEANUP_FAILED", raised.exception.code)
            self.assertEqual("OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", getattr(raised.exception.__cause__, "code", None))
            self.assertIn("primary=OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", str(raised.exception))
            transaction_parent = root / "staged-final-images" / "x64" / "Release" / "cpp"
            self.assertTrue(any(transaction_parent.glob(".txn-*")))

    def test_cleanup_noop_is_rejected_when_transaction_remains(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch(
                "sakura_build_lib.output_final_image_evidence._copy_stable",
                side_effect=OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", "injected failure"),
            ), patch("sakura_build_lib.output_final_image_evidence.shutil.rmtree", return_value=None):
                with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                    self._stage(root)
            self.assertEqual("OUTPUT_FINAL_IMAGE_TRANSACTION_CLEANUP_FAILED", raised.exception.code)
            self.assertEqual("OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", getattr(raised.exception.__cause__, "code", None))
            transaction_parent = root / "staged-final-images" / "x64" / "Release" / "cpp"
            self.assertTrue(any(transaction_parent.glob(".txn-*")))

    def test_post_publish_validation_failure_removes_invalid_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch(
                "sakura_build_lib.output_final_image_evidence.validate_output_final_image_stage",
                side_effect=OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_TAMPERED", "injected validation failure"),
            ):
                with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                    self._stage(root)
            self.assertEqual("OUTPUT_FINAL_IMAGE_TAMPERED", raised.exception.code)
            transaction_parent = root / "staged-final-images" / "x64" / "Release" / "cpp"
            self.assertEqual([], list(transaction_parent.glob(".txn-*")))
            self.assertEqual([], [path for path in transaction_parent.iterdir() if path.name.startswith("cpp-")])

    def test_directory_fsync_failure_is_typed_and_cleans_transaction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original_open = os.open
            original_fsync = os.fsync
            original_close = os.close

            def open_for_directory(path: object, flags: int, *args: object) -> int:
                return 17 if Path(path).is_dir() else original_open(path, flags, *args)

            def fsync_directory_only(descriptor: int) -> None:
                if descriptor == 17:
                    raise OSError("injected fsync failure")
                original_fsync(descriptor)

            def close_directory_only(descriptor: int) -> None:
                if descriptor != 17:
                    original_close(descriptor)

            with patch("sakura_build_lib.output_final_image_evidence.os.open", side_effect=open_for_directory), patch(
                "sakura_build_lib.output_final_image_evidence.os.fsync", side_effect=fsync_directory_only
            ), patch("sakura_build_lib.output_final_image_evidence.os.close", side_effect=close_directory_only):
                with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                    self._stage(root)
            self.assertEqual("OUTPUT_FINAL_IMAGE_FSYNC_FAILED", raised.exception.code)
            transaction_parent = root / "staged-final-images" / "x64" / "Release" / "cpp"
            self.assertEqual([], list(transaction_parent.glob(".txn-*")))

    def test_stage_rejects_escape_and_invalid_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable, map_file = self._source_files(root)
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "escapes"):
                stage_output_final_image(
                    repo_root=root,
                    stage_root=root / "staged-final-images",
                    backend="cpp",
                    platform="x64",
                    configuration="Release",
                    source_native_evidence_sha256="sha256:" + "a" * 64,
                    executable_path=Path(temporary).parent / "outside.exe",
                    map_path=map_file,
                )
            receipt = self._stage(root)
            unknown = copy.deepcopy(receipt)
            unknown["unexpected"] = True
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "canonical"):
                validate_output_final_image_stage(unknown, repo_root=root)

    def test_stage_rejects_reparse_source_ancestors(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable, map_file = self._source_files(root)
            alias = root / "producer-alias"
            try:
                os.symlink(executable.parent, alias, target_is_directory=True)
            except OSError:
                self.skipTest("symbolic links are unavailable on this Windows host")
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "reparse"):
                stage_output_final_image(
                    repo_root=root,
                    stage_root=root / "staged-final-images",
                    backend="cpp",
                    platform="x64",
                    configuration="Release",
                    source_native_evidence_sha256="sha256:" + "a" * 64,
                    executable_path=alias / executable.name,
                    map_path=map_file,
                )

    def test_strict_native_stage_and_binding_are_backend_neutral(self) -> None:
        for backend in ("cpp", "rust"):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                native = _strict_native_fixture()
                self.assertEqual(
                    {"valid": True, "failures": []},
                    validate_output_provider_evidence_for_final_image(native["link"]),
                )
                receipt = self._stage(
                    root,
                    backend=backend,
                    source_native_evidence_sha256=native["hard_evidence_hash"],
                    native_evidence=native,
                )
                bound = bind_native_evidence_to_final_image(native, receipt)
                self.assertEqual(
                    receipt["sourceNativeEvidenceSha256"],
                    product_native_evidence_source_hash(bound),
                )
                self.assertEqual(bound["hard_evidence_hash"], product_native_evidence_hash(bound))

    def test_cpp_ltcg_negative_provider_projection_preserves_zero_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            native = _cpp_ltcg_negative_native_fixture()
            link = native["link"]
            self.assertFalse(validate_output_provider_evidence_for_final_image(link)["valid"])
            self.assertTrue(
                validate_output_provider_evidence_for_final_image(
                    link,
                    expected_backend="cpp",
                    configuration="Release",
                )["valid"]
            )
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    link,
                    expected_backend="cpp",
                    configuration="Debug",
                )["valid"]
            )
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    link,
                    expected_backend="rust",
                )["valid"]
            )
            receipt = self._stage(
                root,
                backend="cpp",
                source_native_evidence_sha256=native["hard_evidence_hash"],
                native_evidence=native,
            )
            bound = bind_native_evidence_to_final_image(native, receipt)
            executable = root / "producer" / "sakura.exe"
            result = validate_bound_native_evidence_for_final_image(
                bound,
                repo_root=root,
                expected_stage_root=root / "staged-final-images",
                expected_backend="cpp",
                expected_platform="x64",
                expected_configuration="Release",
                expected_artifact_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                expected_artifact_size_bytes=executable.stat().st_size,
            )
            self.assertEqual(0, result["provider"]["memberCount"])
            self.assertEqual(0, result["provider"]["symbolCount"])
            self.assertEqual(hashlib.sha256(b"cpp-map\n").hexdigest(), result["provider"]["mapSha256"][7:])
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                self._stage(
                    root,
                    backend="rust",
                    source_native_evidence_sha256=native["hard_evidence_hash"],
                    native_evidence=native,
                )
            self.assertEqual("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", raised.exception.code)

    def test_positive_provider_projection_rejects_multiple_members_and_noninteger_archive_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = _strict_native_fixture()
            member = original["link"]["output_provider_member_evidence"]
            symbols = original["link"]["output_provider_symbol_evidence"]
            two_members = [member["members"][0], "other-provider.obj"]
            contributions = [
                {
                    "symbol": symbol,
                    "archive": "sakura_native_ffi.lib",
                    "member": two_members[index % 2],
                }
                for index, symbol in enumerate(_EXPECTED_PROVIDER_SYMBOLS)
            ]
            member["members"] = two_members
            member["member_count"] = 2
            member["contributing_members"] = two_members
            member["contributions"] = contributions
            symbols["contributing_members"] = two_members
            symbols["contributions"] = contributions
            original["hard_evidence_hash"] = product_native_evidence_hash(original)
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                self._stage(
                    root,
                    backend="cpp",
                    source_native_evidence_sha256=original["hard_evidence_hash"],
                    native_evidence=original,
                )
            self.assertEqual("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", raised.exception.code)

            for evidence_name in (
                "output_provider_member_evidence",
                "output_provider_symbol_evidence",
            ):
                for value in (True, 1.0):
                    malformed = _strict_native_fixture()
                    malformed["link"][evidence_name]["contributing_archive_count"] = value
                    malformed["hard_evidence_hash"] = product_native_evidence_hash(malformed)
                    with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                        self._stage(
                            root,
                            backend="cpp",
                            source_native_evidence_sha256=malformed["hard_evidence_hash"],
                            native_evidence=malformed,
                        )
                    self.assertEqual(
                        "OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA",
                        raised.exception.code,
                        (evidence_name, value),
                    )

    def test_strict_native_stage_rejects_unproven_provider_without_publishing(self) -> None:
        for case in ("missing", "false", "malformed"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                native = _strict_native_fixture()
                link = native["link"]
                self.assertIsInstance(link, dict)
                if case == "missing":
                    del link["output_provider_member_evidence"]
                elif case == "false":
                    link["output_provider_member_evidence"]["observed"] = False
                    link["output_provider_symbol_evidence"]["observed"] = False
                else:
                    link["output_provider_symbol_evidence"]["map_size_bytes"] = "not-an-integer"
                native["hard_evidence_hash"] = product_native_evidence_hash(native)
                with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                    self._stage(
                        root,
                        source_native_evidence_sha256=native["hard_evidence_hash"],
                        native_evidence=native,
                    )
                self.assertEqual("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", raised.exception.code)
                self.assertFalse((root / "staged-final-images").exists())

    def test_native_binding_preserves_source_identity_and_binds_all_known_map_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable_bytes = b"cpp-final-image"
            map_bytes = b"cpp-map\n"
            executable_hash = hashlib.sha256(executable_bytes).hexdigest()
            map_hash = hashlib.sha256(map_bytes).hexdigest()
            provider_symbols = [
                "sakura_output_provider_active_channel_v1",
                "sakura_output_provider_apply_v1",
                "sakura_output_provider_create_v1",
                "sakura_output_provider_destroy_v1",
                "sakura_output_provider_snapshot_measure_v1",
                "sakura_output_provider_snapshot_write_v1",
                "sakura_output_provider_stop_v1",
            ]
            provider_contributions = [
                {"symbol": symbol, "archive": "sakura_native_ffi.lib", "member": "provider.obj"}
                for symbol in provider_symbols
            ]
            original = {
                "schema_version": 4,
                "collection_ok": True,
                "link": {
                    "output": "producer/sakura.exe",
                    "product_hash": executable_hash,
                    "product_size_bytes": len(executable_bytes),
                    "selected_archive_members_observed": True,
                    "linkCommandSha256": "sha256:" + "c" * 64,
                    "rustArchiveSha256": "sha256:" + "b" * 64,
                    "rustArchiveSizeBytes": 1,
                    "rustArchiveCount": 1,
                    "selected_archive_member_evidence": {"map": "producer/sakura.map", "map_hash": map_hash, "map_size_bytes": len(map_bytes)},
                    "output_provider_member_evidence": {
                        "observed": True,
                        "provider": "output-provider",
                        "method": "msvc_map_publics_by_value_provider_rows",
                        "map": "producer/sakura.map",
                        "map_hash": map_hash,
                        "map_size_bytes": len(map_bytes),
                        "archive_name": "sakura_native_ffi.lib",
                        "archive_input_count": 1,
                        "contributing_archives": ["sakura_native_ffi.lib"],
                        "contributing_archive_count": 1,
                        "contributing_members": ["provider.obj"],
                        "members": ["provider.obj"],
                        "member_count": 1,
                        "missing_symbols": [],
                        "unexpected_symbols": [],
                        "duplicate_count": 0,
                        "contributions": provider_contributions,
                        "selector_proof": "ltcg_compile_log_required",
                    },
                    "output_provider_symbol_evidence": {
                        "observed": True,
                        "provider": "output-provider",
                        "scope": "output-provider",
                        "method": "msvc_map_publics_by_value_provider_rows",
                        "map": "producer/sakura.map",
                        "map_hash": map_hash,
                        "map_size_bytes": len(map_bytes),
                        "symbols": provider_symbols,
                        "symbol_count": len(provider_symbols),
                        "duplicate_count": 0,
                        "missing_symbols": [],
                        "unexpected_symbols": [],
                        "contributing_archives": ["sakura_native_ffi.lib"],
                        "contributing_members": ["provider.obj"],
                        "contributions": provider_contributions,
                        "selector_proof": "ltcg_compile_log_required",
                    },
                    "outputProviderSymbolEvidence": {
                        "map": "producer/sakura.map",
                        "mapHash": map_hash,
                        "mapSizeBytes": len(map_bytes),
                    },
                },
            }
            original["hard_evidence_hash"] = product_native_evidence_hash(original)
            self.assertTrue(validate_output_provider_evidence_for_final_image(original["link"])["valid"])
            receipt = self._stage(root, source_native_evidence_sha256=original["hard_evidence_hash"])
            before = copy.deepcopy(original)
            bound = bind_native_evidence_to_final_image(original, receipt)
            self.assertEqual(before, original)
            self.assertEqual("producer/sakura.exe", bound["link"]["output"])
            for key in (
                "selected_archive_member_evidence",
                "output_provider_member_evidence",
                "output_provider_symbol_evidence",
                "outputProviderSymbolEvidence",
            ):
                self.assertEqual("producer/sakura.map", bound["link"][key]["map"])
            self.assertEqual(map_hash, bound["link"]["output_provider_symbol_evidence"]["map_hash"])
            self.assertEqual(map_hash, bound["link"]["outputProviderSymbolEvidence"]["mapHash"])
            self.assertEqual(len(map_bytes), bound["link"]["output_provider_symbol_evidence"]["map_size_bytes"])
            self.assertEqual(len(map_bytes), bound["link"]["outputProviderSymbolEvidence"]["mapSizeBytes"])
            self.assertEqual(receipt["receiptPath"], bound["link"]["final_image_stage"]["receipt"])
            self.assertEqual(bound["hard_evidence_hash"], product_native_evidence_hash(bound))
            self.assertEqual(receipt["sourceNativeEvidenceSha256"], product_native_evidence_source_hash(bound))
            self.assertEqual(4, bound["schema_version"])
            self.assertTrue(bound["collection_ok"])
            self.assertTrue(validate_output_final_image_stage(root / receipt["receiptPath"], repo_root=root)["ok"])
            (root / "producer" / "sakura.map").unlink()
            self.assertTrue(validate_output_final_image_stage(root / receipt["receiptPath"], repo_root=root)["ok"])
            json.dumps(bound, ensure_ascii=False, sort_keys=True)

    def _qualified_bound_fixture(self, root: Path) -> tuple[dict[str, object], dict[str, object], Path]:
        native = _strict_native_fixture()
        executable, _map_file = self._source_files(root)
        receipt = self._stage(
            root,
            source_native_evidence_sha256=native["hard_evidence_hash"],
            native_evidence=native,
        )
        bound = bind_native_evidence_to_final_image(native, receipt)
        native_path = root / "build" / "evidence" / "native-bound.json"
        native_path.parent.mkdir(parents=True, exist_ok=True)
        native_path.write_text(json.dumps(bound, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        return bound, receipt, executable

    def test_bound_native_validation_returns_only_bounded_payload_free_identities(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bound, receipt, executable = self._qualified_bound_fixture(root)
            executable_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
            result = validate_bound_native_evidence_for_final_image(
                bound,
                repo_root=root,
                expected_stage_root=root / "staged-final-images",
                expected_backend="cpp",
                expected_platform="x64",
                expected_configuration="Release",
                expected_artifact_sha256=executable_hash,
                expected_artifact_size_bytes=executable.stat().st_size,
            )
            self.assertEqual(
                {
                    "ok",
                    "payloadFree",
                    "record",
                    "backend",
                    "platform",
                    "configuration",
                    "boundNativeEvidenceSha256",
                    "sourceNativeEvidenceSha256",
                    "stageId",
                    "receiptPath",
                    "receiptSha256",
                    "files",
                    "provider",
                },
                set(result),
            )
            self.assertTrue(result["ok"])
            self.assertTrue(result["payloadFree"])
            self.assertEqual(bound["hard_evidence_hash"], result["boundNativeEvidenceSha256"])
            self.assertEqual(receipt["receiptPath"], result["receiptPath"])
            self.assertEqual(7, result["provider"]["symbolCount"])
            self.assertEqual(hashlib.sha256(b"cpp-map\n").hexdigest(), result["provider"]["mapSha256"][7:])

            # Receipt derivation and explicit receipt paths/mappings must lead
            # to the same on-disk canonical receipt.
            receipt_path = root / receipt["receiptPath"]
            self.assertEqual(
                result,
                validate_bound_native_evidence_for_final_image(
                    bound,
                    repo_root=root,
                    expected_stage_root=root / "staged-final-images",
                    expected_backend="cpp",
                    expected_platform="x64",
                    expected_configuration="Release",
                    expected_artifact_sha256="sha256:" + executable_hash,
                    expected_artifact_size_bytes=int(executable.stat().st_size),
                    receipt=receipt_path,
                ),
            )

    def test_bound_native_validation_rejects_tampering_and_bool_artifact_size(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bound, _receipt, executable = self._qualified_bound_fixture(root)
            executable_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
            common = {
                "repo_root": root,
                "expected_stage_root": root / "staged-final-images",
                "expected_backend": "cpp",
                "expected_platform": "x64",
                "expected_configuration": "Release",
                "expected_artifact_sha256": executable_hash,
                "expected_artifact_size_bytes": executable.stat().st_size,
            }
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                validate_bound_native_evidence_for_final_image(bound, **common | {"expected_artifact_size_bytes": True})
            self.assertEqual("OUTPUT_FINAL_IMAGE_ARTIFACT_ARGUMENT", raised.exception.code)
            self.assertEqual(2, raised.exception.exit_code)

            tampered = copy.deepcopy(bound)
            tampered["link"]["final_image_stage"]["receiptSha256"] = "0" * 64
            tampered["hard_evidence_hash"] = product_native_evidence_hash(tampered)
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                validate_bound_native_evidence_for_final_image(tampered, **common)
            self.assertEqual("OUTPUT_FINAL_IMAGE_BINDING_MISMATCH", raised.exception.code)
            self.assertNotIn("receipt", str(raised.exception).lower())

    def test_bound_native_validation_rejects_unowned_root_aliases_and_existing_unsafe_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bound, _receipt, executable = self._qualified_bound_fixture(root)
            common = {
                "repo_root": root,
                "expected_stage_root": root / "staged-final-images",
                "expected_backend": "cpp",
                "expected_platform": "x64",
                "expected_configuration": "Release",
                "expected_artifact_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "expected_artifact_size_bytes": executable.stat().st_size,
            }
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                validate_bound_native_evidence_for_final_image(bound, **common | {"expected_stage_root": root})
            self.assertEqual("OUTPUT_FINAL_IMAGE_STAGE_ROOT_UNSAFE", raised.exception.code)

            aliased = copy.deepcopy(bound)
            aliased["link"]["outputPath"] = aliased["link"]["output"]
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                validate_bound_native_evidence_for_final_image(aliased, **common)
            self.assertEqual("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", raised.exception.code)

            executable.unlink()
            executable.mkdir()
            with self.assertRaises(OutputFinalImageEvidenceError) as raised:
                validate_bound_native_evidence_for_final_image(bound, **common)
            self.assertEqual("OUTPUT_FINAL_IMAGE_ARTIFACT_UNPROVEN", raised.exception.code)

    def test_output_final_image_verify_cli_is_no_build_and_payload_free(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _bound, receipt, executable = self._qualified_bound_fixture(root)
            native_path = root / "build" / "evidence" / "native-bound.json"
            cli = TOOLS_BUILD / "sakura_build.py"
            command = [
                sys.executable,
                str(cli),
                "--repo-root",
                str(root),
                "--format",
                "json",
                "evidence",
                "output-final-image-verify",
                "--native-evidence",
                str(native_path),
                "--stage-root",
                str(root / "staged-final-images"),
                "--backend",
                "cpp",
                "--platform",
                "x64",
                "--configuration",
                "Release",
                "--artifact-sha256",
                hashlib.sha256(executable.read_bytes()).hexdigest(),
                "--artifact-size-bytes",
                str(executable.stat().st_size),
            ]
            completed = subprocess.run(command, cwd=TOOLS_BUILD, capture_output=True, text=True, check=False)
            self.assertEqual(0, completed.returncode, completed.stderr)
            result = json.loads(completed.stdout)
            self.assertEqual(
                {
                    "ok",
                    "payloadFree",
                    "record",
                    "backend",
                    "platform",
                    "configuration",
                    "boundNativeEvidenceSha256",
                    "sourceNativeEvidenceSha256",
                    "stageId",
                    "receiptPath",
                    "receiptSha256",
                    "files",
                    "provider",
                },
                set(result),
            )
            self.assertTrue(result["ok"])
            self.assertTrue(result["payloadFree"])
            self.assertEqual(receipt["receiptPath"], result["receiptPath"])
            self.assertNotIn("cpp-final-image", completed.stdout)
            self.assertNotIn("provider.obj", completed.stdout)

            command[command.index(hashlib.sha256(executable.read_bytes()).hexdigest())] = "f" * 64
            failed = subprocess.run(command, cwd=TOOLS_BUILD, capture_output=True, text=True, check=False)
            self.assertEqual(5, failed.returncode)
            failure = json.loads(failed.stdout)
            self.assertEqual(
                {
                    "ok": False,
                    "payloadFree": True,
                    "record": "output-final-image-binding-validation",
                    "failureCode": "OUTPUT_FINAL_IMAGE_ARTIFACT_MISMATCH",
                },
                failure,
            )

    def test_binding_rejects_receipt_and_native_records_from_different_attempts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = {
                "schema_version": 4,
                "collection_ok": True,
                "link": {
                    "output": "producer/sakura.exe",
                    "product_hash": hashlib.sha256(b"cpp-final-image").hexdigest(),
                    "product_size_bytes": len(b"cpp-final-image"),
                    "selected_archive_member_evidence": {
                        "map": "producer/sakura.map",
                        "map_hash": hashlib.sha256(b"cpp-map\n").hexdigest(),
                        "map_size_bytes": len(b"cpp-map\n"),
                    },
                },
            }
            original["hard_evidence_hash"] = product_native_evidence_hash(original)
            receipt = self._stage(root, source_native_evidence_sha256=original["hard_evidence_hash"])
            other = copy.deepcopy(original)
            other["link"]["product_size_bytes"] += 1
            other["hard_evidence_hash"] = product_native_evidence_hash(other)
            with self.assertRaisesRegex(OutputFinalImageEvidenceError, "match"):
                bind_native_evidence_to_final_image(other, receipt)


if __name__ == "__main__":
    unittest.main()
