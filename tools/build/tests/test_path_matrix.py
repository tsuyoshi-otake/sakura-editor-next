from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.path_matrix import _run_child  # noqa: E402


class PathMatrixChildTests(unittest.TestCase):
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

    @unittest.skipUnless(os.name == "nt", "native alias timeout cleanup is Windows-specific")
    def test_timeout_removes_the_run_owned_native_alias(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-matrix-timeout-") as temporary:
            repo = Path(temporary) / "日本語" / "repo"
            repo.mkdir(parents=True)
            child = (
                "import sys,time; from contextlib import ExitStack; from pathlib import Path; "
                f"sys.path.insert(0, {str(TOOLS_BUILD)!r}); "
                "from sakura_build_lib.runner import native_execution_root; "
                "stack=ExitStack(); alias=stack.enter_context(native_execution_root(Path.cwd())); "
                "print(alias, flush=True); time.sleep(30)"
            )
            result = _run_child([sys.executable, "-c", child], repo, timeout_seconds=0.5)
            alias = Path(result.stdout.strip())

            self.assertEqual(("timeout", 8), (result.status, result.returncode))
            self.assertTrue(str(alias))
            self.assertFalse(alias.exists())


if __name__ == "__main__":
    unittest.main()
