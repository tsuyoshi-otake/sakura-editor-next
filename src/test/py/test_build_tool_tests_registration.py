"""Contract: ``tools/build/tests`` runs in CI through the CTest ``BuildToolTests``.

The CTest ``pytest`` target never collects ``tools/build/tests``: pytest's
default ``norecursedirs`` skips any directory named ``build``, and two modules
there import siblings by name.  Those tests went unrun until #136 found a
junction leak hidden behind three of their failures.  ``CMakeLists.txt`` now
registers them under unittest discovery, and both hosted ctest invocations
must keep running every registered test.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CMAKE_LISTS = REPO_ROOT / "CMakeLists.txt"
BUILD_TOOL_TESTS = REPO_ROOT / "tools/build/tests"
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
# build-on-msys2.yml reaches ctest through build-gnu.bat and the canonical CLI.
RUNNER = REPO_ROOT / "tools/build/sakura_build_lib/runner.py"
_CTEST_SELECTION = re.compile(
    r"""['"\s](-R|-E|-L|-LE|-U|--tests-regex|--exclude-regex|--label-regex|--label-exclude"""
    r"""|--tests-from-file|--exclude-from-file)['"\s]"""
)


def _add_test_block(text: str, name: str) -> str:
    start = text.index(f"add_test(NAME {name}\n")
    end = text.index("\n)\n", start)
    return text[start:end]


class BuildToolTestsRegistrationTests(unittest.TestCase):
    def test_cmake_registers_unittest_discovery_of_tools_build_tests(self) -> None:
        block = _add_test_block(CMAKE_LISTS.read_text(encoding="utf-8-sig"), "BuildToolTests")
        arguments = block.split()
        self.assertEqual(
            ["-m", "unittest", "discover", "-s", "tools/build/tests", "-p", '"test_*.py"'],
            arguments[arguments.index("-m"):arguments.index("WORKING_DIRECTORY")],
        )
        self.assertIn('"${UV_EXECUTABLE}"', arguments)
        self.assertIn("WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}", block)
        # Both MSVC configurations and the MinGW leg run it.
        self.assertNotIn("CONFIGURATIONS", block)

    def test_the_discovery_start_directory_holds_tests(self) -> None:
        # A rename that left the -s argument behind would discover nothing.
        self.assertTrue(BUILD_TOOL_TESTS.is_dir())
        self.assertGreaterEqual(len(list(BUILD_TOOL_TESTS.glob("test_*.py"))), 19)

    def test_hosted_ctest_runs_select_every_registered_test(self) -> None:
        workflow = BUILD_WORKFLOW.read_text(encoding="utf-8-sig")
        start = workflow.index("    - name: Run headless tests\n")
        step = workflow[start:workflow.index("\n    - name:", start)]
        self.assertIn("& ctest", step)
        self.assertIsNone(_CTEST_SELECTION.search(step), step)

        runner = RUNNER.read_text(encoding="utf-8-sig")
        gnu_ctest = [
            line for line in runner.splitlines()
            if "commands.append(" in line and 'find_cmake_tool("ctest")' in line
        ]
        self.assertEqual(1, len(gnu_ctest))
        self.assertIsNone(_CTEST_SELECTION.search(gnu_ctest[0]), gnu_ctest[0])


if __name__ == "__main__":
    unittest.main()
