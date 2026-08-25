"""SHA-256 identity of staged runtime files versus packaged payloads.

``tools/verify_runtime_artifact_identity.py`` is what packaging scripts run
against real installer and ZIP outputs. These tests drive that same tool with
throwaway directories so a mismatch cannot pass, and a matching extract cannot
fail, without requiring a full ``build-all``.
"""

from __future__ import annotations

import importlib.util
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
            "bregonig.dll": b"bregonig-bytes",
            "migemo.dll": b"migemo-bytes",
            "sakura-senp-tool.exe": b"senp-tool-bytes",
            "sakura-senp-host.exe": b"senp-host-bytes",
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
                        b"shipped-other-bytes" if name == "bregonig.dll" else payload,
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
            self.assertTrue(report["mismatches"])

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


if __name__ == "__main__":
    unittest.main()
