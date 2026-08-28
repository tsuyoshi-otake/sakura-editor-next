from __future__ import annotations

import copy
import hashlib
import json
import os
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
    validate_output_final_image_stage,
)
from sakura_build_lib.product_native_evidence import (  # noqa: E402
    product_native_evidence_hash,
    product_native_evidence_source_hash,
)


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

    def test_native_binding_preserves_source_identity_and_binds_all_known_map_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable_bytes = b"cpp-final-image"
            map_bytes = b"cpp-map\n"
            executable_hash = hashlib.sha256(executable_bytes).hexdigest()
            map_hash = hashlib.sha256(map_bytes).hexdigest()
            original = {
                "schema_version": 4,
                "collection_ok": True,
                "link": {
                    "output": "producer/sakura.exe",
                    "product_hash": executable_hash,
                    "product_size_bytes": len(executable_bytes),
                    "selected_archive_member_evidence": {"map": "producer/sakura.map", "map_hash": map_hash, "map_size_bytes": len(map_bytes)},
                    "output_provider_member_evidence": {"map": "producer/sakura.map", "map_hash": map_hash, "map_size_bytes": len(map_bytes)},
                    "output_provider_symbol_evidence": {
                        "observed": True,
                        "scope": "output-provider",
                        "map": "producer/sakura.map",
                        "map_hash": map_hash,
                        "map_size_bytes": len(map_bytes),
                        "symbols": ["sakura_output_provider_apply_v1"],
                        "symbol_count": 1,
                        "duplicate_count": 0,
                    },
                    "outputProviderSymbolEvidence": {
                        "map": "producer/sakura.map",
                        "mapHash": map_hash,
                        "mapSizeBytes": len(map_bytes),
                    },
                },
            }
            original["hard_evidence_hash"] = product_native_evidence_hash(original)
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
