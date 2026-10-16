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

from sakura_build_lib.path_matrix import _MATRIX_FILES, _copy_minimal_repository, _run_child  # noqa: E402
from sakura_build_lib.runner import NATIVE_ALIAS_OWNER_ENVIRONMENT  # noqa: E402


class PathMatrixChildTests(unittest.TestCase):
    def test_minimal_repository_copy_includes_private_component_headers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            destination = root / "destination"
            for relative_text in _MATRIX_FILES:
                path = source / relative_text
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative_text, encoding="utf-8")

            private_header = Path("component/private.h")
            witness = Path("consumer/witness.cpp")
            legacy_project = Path("component/provider.vcxproj")
            for relative in (private_header, witness, legacy_project):
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative.as_posix(), encoding="utf-8")
            (source / "legacy-owner").mkdir(parents=True)
            manifest = {
                "components": [{
                    "sources": ["legacy-owner"],
                    "public_headers": [],
                    "private_headers": [private_header.as_posix()],
                    "backend_targets": {"msbuild": [legacy_project.as_posix()]},
                }],
                "edges": [{"witnesses": [{"probe": witness.as_posix()}]}],
                "artifacts": [],
            }
            (source / "src/main/modules/modules.json").write_text(json.dumps(manifest), encoding="utf-8")

            _copy_minimal_repository(source, destination)

            self.assertEqual(private_header.as_posix(), (destination / private_header).read_text(encoding="utf-8"))
            self.assertEqual(witness.as_posix(), (destination / witness).read_text(encoding="utf-8"))
            self.assertEqual(legacy_project.as_posix(), (destination / legacy_project).read_text(encoding="utf-8"))
            self.assertTrue((destination / "legacy-owner").is_dir())

    def test_minimal_repository_copy_rejects_a_missing_manifest_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            destination = root / "destination"
            for relative_text in _MATRIX_FILES:
                path = source / relative_text
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative_text, encoding="utf-8")
            manifest = {
                "components": [{
                    "sources": ["component/missing.cpp"],
                    "public_headers": [],
                    "private_headers": [],
                    "backend_targets": {},
                }],
                "edges": [],
                "artifacts": [],
            }
            (source / "src/main/modules/modules.json").write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(FileNotFoundError, r"components\[0\]\.sources\[0\]"):
                _copy_minimal_repository(source, destination)

            self.assertFalse((destination / "component/missing.cpp").exists())

    def test_child_terminal_states_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cwd = Path(temporary)
            success = _run_child([sys.executable, "-c", "print('ok')"], cwd, timeout_seconds=5)
            failure = _run_child([sys.executable, "-c", "raise SystemExit(7)"], cwd, timeout_seconds=5)
            timeout = _run_child(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                cwd,
                timeout_seconds=0.1,
            )

        self.assertEqual(("success", 0), (success.status, success.returncode))
        self.assertEqual(("failed", 7), (failure.status, failure.returncode))
        self.assertEqual(("timeout", 8), (timeout.status, timeout.returncode))
        self.assertNotIn("NATIVE_PATH_ALIAS_NONE_OWNED", timeout.stderr)

    @unittest.skipUnless(os.name == "nt", "native alias timeout cleanup is Windows-specific")
    def test_timeout_removes_the_run_owned_native_alias(self) -> None:
        # sys.executable is a venv launcher in the usual local setup, so the
        # interpreter that names the alias is not the process Popen.pid names.
        with tempfile.TemporaryDirectory(prefix="sakura-matrix-timeout-") as temporary:
            repo = Path(temporary) / "日本語" / "repo"
            repo.mkdir(parents=True)
            child = (
                "import os,sys,time; from contextlib import ExitStack; from pathlib import Path; "
                f"sys.path.insert(0, {str(TOOLS_BUILD)!r}); "
                "from sakura_build_lib.runner import native_execution_root; "
                "stack=ExitStack(); alias=stack.enter_context(native_execution_root(Path.cwd())); "
                f"print(os.environ[{NATIVE_ALIAS_OWNER_ENVIRONMENT!r}], alias, sep='\\n', flush=True); time.sleep(30)"
            )
            # The child needs about 0.2s to create its alias; the margin keeps
            # a slow hosted runner from killing it first.
            result = _run_child([sys.executable, "-c", child], repo, timeout_seconds=5)
            owner, alias_text = result.stdout.strip().splitlines()
            alias = Path(alias_text)

            self.assertEqual(("timeout", 8), (result.status, result.returncode))
            self.assertTrue(alias.name.startswith(f"{owner}-"))
            self.assertEqual((alias_text,), result.native_aliases_removed)
            self.assertNotIn("NATIVE_PATH_ALIAS_NONE_OWNED", result.stderr)
            self.assertFalse(alias.exists())

    @unittest.skipUnless(os.name == "nt", "native alias timeout cleanup is Windows-specific")
    def test_timeout_without_an_owned_alias_on_a_non_ascii_root_is_reported(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-matrix-unaliased-") as temporary:
            repo = Path(temporary) / "日本語" / "repo"
            repo.mkdir(parents=True)
            result = _run_child([sys.executable, "-c", "import time; time.sleep(30)"], repo, timeout_seconds=0.5)

        self.assertEqual(("timeout", 8), (result.status, result.returncode))
        self.assertEqual((), result.native_aliases_removed)
        self.assertIn("NATIVE_PATH_ALIAS_NONE_OWNED", result.stderr)


if __name__ == "__main__":
    unittest.main()
