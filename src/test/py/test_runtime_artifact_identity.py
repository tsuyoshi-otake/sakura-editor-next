"""SHA-256 identity of staged runtime DLLs versus packaged payloads.

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

    def test_matching_zip_payload_agrees_with_the_staged_dlls(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged"
            staged.mkdir()
            (staged / "bregonig.dll").write_bytes(b"bregonig-bytes")
            (staged / "migemo.dll").write_bytes(b"migemo-bytes")
            archive = root / "exe.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("bregonig.dll", b"bregonig-bytes")
                bundle.writestr("migemo.dll", b"migemo-bytes")
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
            (staged / "bregonig.dll").write_bytes(b"tested")
            (staged / "migemo.dll").write_bytes(b"migemo-bytes")
            archive = root / "exe.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("bregonig.dll", b"shipped-other-bytes")
                bundle.writestr("migemo.dll", b"migemo-bytes")
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


if __name__ == "__main__":
    unittest.main()
