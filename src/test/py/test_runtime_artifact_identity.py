"""SHA-256 identity of staged runtime files versus packaged payloads.

``tools/verify_runtime_artifact_identity.py`` is what packaging scripts run
against real installer and ZIP outputs. These tests drive that same tool with
throwaway directories so a mismatch cannot pass, and a matching extract cannot
fail, without requiring a full ``build-all``.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
import zipfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL = REPO_ROOT / "tools/verify_runtime_artifact_identity.py"


def _load_tool():
    spec = importlib.util.spec_from_file_location("verify_runtime_artifact_identity", TOOL)
    if spec is None or spec.loader is None:
        raise AssertionError("tools/verify_runtime_artifact_identity.py is missing")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RuntimeArtifactIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tool = _load_tool()

    def _write_staged_files(self, staged: Path) -> dict[str, bytes]:
        payloads = {
            "sakura.exe": b"sakura-application-bytes",
            "bregonig.dll": b"bregonig-bytes",
            "migemo.dll": b"migemo-bytes",
            "sakura-senp-tool.exe": b"senp-tool-bytes",
            "sakura-senp-host.exe": b"senp-host-bytes",
            "sakura-tmux.exe": b"sakura-tmux-bytes",
            "tmux.exe": b"tmux-alias-bytes",
            "sakura-harness.exe": b"harness-client-bytes",
        }
        for name, payload in payloads.items():
            (staged / name).write_bytes(payload)
        return payloads

    def test_matching_zip_payload_agrees_with_the_staged_runtime_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            archive = root / "exe.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                for name, payload in payloads.items():
                    bundle.writestr(name, payload)
            report_path = root / "report.json"
            argv = [
                "--staged",
                str(staged),
                "--zip",
                str(archive),
                "--clean-extract",
                str(root / "extract"),
                "--report",
                str(report_path),
            ]
            self.assertEqual(0, self.tool.main(argv))
            report = self.tool.collect(self.tool.parse_args(argv))
            self.assertTrue(report["ok"], report)
            self.assertEqual(set(payloads), set(report["sources"]["staged"]))
            self.assertEqual(set(payloads), set(report["sources"]["zip"]))

    def test_a_different_zip_payload_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            archive = root / "exe.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                for name, payload in payloads.items():
                    bundle.writestr(
                        name,
                        b"shipped-other-bytes"
                        if name in ("sakura.exe", "bregonig.dll")
                        else payload,
                    )
            args = self.tool.parse_args(
                [
                    "--staged",
                    str(staged),
                    "--zip",
                    str(archive),
                    "--clean-extract",
                    str(root / "extract"),
                    "--report",
                    str(root / "report.json"),
                ]
            )
            report = self.tool.collect(args)
            self.assertFalse(report["ok"])
            self.assertTrue(any(line.startswith("sakura.exe:") for line in report["mismatches"]))
            self.assertTrue(any(line.startswith("bregonig.dll:") for line in report["mismatches"]))

    def test_missing_senp_runtime_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            archive = root / "exe.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                for name, payload in payloads.items():
                    if name != "sakura-senp-host.exe":
                        bundle.writestr(name, payload)
            args = self.tool.parse_args(
                [
                    "--staged",
                    str(staged),
                    "--zip",
                    str(archive),
                    "--clean-extract",
                    str(root / "extract"),
                    "--report",
                    str(root / "report.json"),
                ]
            )
            with self.assertRaisesRegex(self.tool.IdentityError, "sakura-senp-host.exe"):
                self.tool.collect(args)

    def _run_zip_check(
        self, root: Path, staged: Path, payloads: dict[str, bytes]
    ) -> tuple[int, dict[str, object]]:
        archive = root / "exe.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            for name, payload in payloads.items():
                bundle.writestr(name, payload)
        report_path = root / "report.json"
        result = self.tool.main(
            [
                "--staged",
                str(staged),
                "--zip",
                str(archive),
                "--clean-extract",
                str(root / "extract"),
                "--report",
                str(report_path),
            ]
        )
        return result, json.loads(report_path.read_text(encoding="utf-8"))

    def test_nested_staged_sakura_exe_copies_do_not_affect_the_top_level_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            nested = staged / "avx2"
            nested.mkdir()
            (nested / "sakura.exe").write_bytes(b"different-experiment-bytes")

            result, report = self._run_zip_check(root, staged, payloads)
            self.assertEqual(0, result, report)
            self.assertEqual(
                hashlib.sha256(payloads["sakura.exe"]).hexdigest(),
                report["sources"]["staged"]["sakura.exe"],
            )

    def test_staged_sakura_exe_must_be_the_top_level_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            nested = staged / "avx2"
            nested.mkdir()
            (staged / "sakura.exe").rename(nested / "sakura.exe")

            result, report = self._run_zip_check(root, staged, payloads)
            self.assertEqual(1, result, report)
            self.assertIn("directly under", report["error"])

    def test_a_second_packaged_sakura_exe_is_still_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            payloads = self._write_staged_files(staged)
            payloads["nested/sakura.exe"] = payloads["sakura.exe"]

            result, report = self._run_zip_check(root, staged, payloads)
            self.assertEqual(1, result, report)
            self.assertIn("more than one copy", report["error"])


if __name__ == "__main__":
    unittest.main()
