from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.runner import NATIVE_PATH_IDENTITY_FILE, native_execution_root, write_native_path_identity


class NativeExecutionRootTests(unittest.TestCase):
    def test_ascii_root_is_used_directly(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-ascii-") as directory:
            root = Path(directory).resolve()
            with native_execution_root(root) as execution_root:
                self.assertEqual(root, execution_root)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_non_ascii_root_uses_and_cleans_ascii_junction(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-junction-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            marker = root / "marker.txt"
            marker.write_text("same file", encoding="utf-8")

            with native_execution_root(root) as execution_root:
                alias = execution_root
                str(alias).encode("ascii")
                self.assertNotEqual(root.resolve(), alias)
                self.assertEqual(root.resolve(), alias.resolve())
                self.assertEqual("same file", (alias / "marker.txt").read_text(encoding="utf-8"))
                build_dir = alias / "build" / "probe"
                write_native_path_identity(build_dir, root, alias)
                recorded = json.loads((build_dir / NATIVE_PATH_IDENTITY_FILE).read_text(encoding="utf-8"))

            self.assertFalse(alias.exists())
            self.assertFalse(alias.parent.exists())
            with native_execution_root(root, preferred_alias=Path(recorded["execution_root"])) as recreated:
                self.assertEqual(alias, recreated)
                self.assertEqual(root.resolve(), recreated.resolve())
            self.assertFalse(alias.exists())

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_non_ascii_alias_is_cleaned_when_the_child_scope_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-failure-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            alias: Path | None = None

            with self.assertRaisesRegex(RuntimeError, "synthetic child failure"):
                with native_execution_root(root) as execution_root:
                    alias = execution_root
                    raise RuntimeError("synthetic child failure")

            self.assertIsNotNone(alias)
            self.assertFalse(alias.exists())
            self.assertFalse(alias.parent.exists())


if __name__ == "__main__":
    unittest.main()
