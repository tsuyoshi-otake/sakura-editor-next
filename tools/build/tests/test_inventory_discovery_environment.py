from __future__ import annotations

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

from sakura_build_lib import test_inventory


class InventoryDiscoveryEnvironmentTests(unittest.TestCase):
    def test_discovery_lists_all_tests_without_inheriting_gtest_variables(self) -> None:
        caller_environment = {
            "GTEST_FILTER": "FirstSuite.*",
            "gTeSt_OuTpUt": "xml:results.xml",
            "SAKURA_TEST_MARKER": "1",
        }
        listing = "FirstSuite.\n  FirstTest\nSecondSuite.\n  SecondTest\n"
        with tempfile.TemporaryDirectory() as directory:
            repo_root = Path(directory).resolve()
            executable = repo_root / "runner.exe"
            executable.write_bytes(b"runner fixture")
            arguments = [str(executable), "--gtest_list_tests"]
            completed = subprocess.CompletedProcess(arguments, 0, stdout=listing, stderr="")

            with (
                patch.dict(os.environ, caller_environment),
                patch.object(test_inventory.subprocess, "run", return_value=completed) as run,
            ):
                inventory = test_inventory.collect_gtest_inventory(
                    executable, "tests1", repo_root, "fixture-revision", False, 5
                )
                for name, value in caller_environment.items():
                    self.assertEqual(os.environ.get(name), value)

        run.assert_called_once()
        self.assertEqual(run.call_args.args, (arguments,))
        discovery_environment = run.call_args.kwargs["env"]
        self.assertEqual(discovery_environment["SAKURA_TEST_MARKER"], "1")
        self.assertFalse(any(name.upper().startswith("GTEST_") for name in discovery_environment))
        self.assertEqual(
            [entry["runtime"]["selector"] for entry in inventory["tests"]],
            ["FirstSuite.FirstTest", "SecondSuite.SecondTest"],
        )
