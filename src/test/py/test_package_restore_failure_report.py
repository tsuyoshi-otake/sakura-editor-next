"""Contracts for the vcpkg restore failure report (#137).

When a port build fails, vcpkg prints a ready-to-paste GitHub issue report
after the real error. That report is longer than thirty lines, so the original
``"\\n".join(lines[-30:])`` window filled up with boilerplate and discarded the
cause. CI observed exactly that: three consecutive ``Restore declared Sakura
packages`` failures whose entire message was ``**Additional context**`` plus
the manifest this module already knows, with no indication of which port failed
or why, and no way to tell a transient network fault from a real build break.

The rule pinned here is that the cause must survive regardless of how much
boilerplate follows it, and that the vcpkg exit code is always reported - the
old ``or f"vcpkg failed with {returncode}"`` fallback could never fire, because
a template-filled window is not empty.

Like ``test_package_restore_gate_scope.py``, this lives in ``src/test/py``
because the CTest ``pytest`` target runs from the repository root under
pytest's default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_BUILD = REPO_ROOT / "tools/build"
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.package_restore import _summarize_vcpkg_failure  # noqa: E402


def _issue_report_head() -> list[str]:
    """The part of vcpkg's issue report that still carries diagnostic value."""
    return [
        "",
        "**Host Environment**",
        "",
        "- Host: x64-windows",
        "- Compiler: MSVC 19.44.35217.0",
        "-    vcpkg-tool version: 2026-05-27",
        "    vcpkg-scripts version: 1a2b3c4d 2026-05-20",
        "",
        "**To Reproduce**",
        "",
        "`vcpkg install `",
        "",
        "**Failure logs**",
        "",
        "```",
        "-- Downloading https://example.invalid/cmigemo-1.3.tar.gz",
        "```",
    ]


def _issue_report_tail() -> list[str]:
    """The trailing section, transcribed from the CI failure that motivated #137.

    This alone is long enough to fill the old thirty-line window, which is why
    the cause never reached the log.
    """
    return [
        "",
        "**Additional context**",
        "",
        "<details><summary>vcpkg.json</summary>",
        "",
        "```",
        "{",
        '  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",',
        '  "dependencies": [',
        '    "bregonig",',
        '    "cmigemo",',
        "    {",
        '      "name": "darkmodelib",',
        '      "platform": "static"',
        "    },",
        '    "dll-plugin1",',
        '    "fmt",',
        "    {",
        '      "name": "gtest",',
        '      "platform": "static"',
        "    },",
        '    "ms-gsl",',
        '    "ppa-stub",',
        '    "wil"',
        "  ]",
        "}",
        "",
        "```",
        "</details>",
    ]


def _issue_report_template() -> list[str]:
    """The whole report vcpkg appends to a failed port build."""
    return [*_issue_report_head(), *_issue_report_tail()]


class SummarizeVcpkgFailureTests(unittest.TestCase):
    def test_the_cause_survives_the_issue_report_template(self) -> None:
        cause = "error: building cmigemo:x64-windows-static failed with: BUILD_FAILED"
        stdout = "\n".join(["-- Restoring 9 packages", cause, *_issue_report_template()])

        message = _summarize_vcpkg_failure(stdout, "", 1)

        self.assertIn(cause, message)
        self.assertIn("vcpkg exited with 1", message)

    def test_the_cause_survives_even_when_far_from_the_end(self) -> None:
        # The defect this pins: the dropped section alone nearly fills the old
        # 30-line window, so the cause must be lifted out, not sliced for.
        cause = "error: building cmigemo:x64-windows-static failed with: BUILD_FAILED"
        self.assertGreaterEqual(len(_issue_report_tail()), 28)
        stdout = "\n".join([cause, *(["filler"] * 400), *_issue_report_template()])

        message = _summarize_vcpkg_failure(stdout, "", 1)

        self.assertIn(cause, message)

    def test_the_report_template_tail_is_dropped(self) -> None:
        stdout = "\n".join(["error: something broke", *_issue_report_template()])

        message = _summarize_vcpkg_failure(stdout, "", 1)

        self.assertNotIn("**Additional context**", message)
        self.assertNotIn("<details><summary>vcpkg.json</summary>", message)
        self.assertIn("issue-report template removed", message)
        # Everything before the dropped section is still available.
        self.assertIn("**Failure logs**", message)

    def test_output_without_a_template_keeps_its_tail_intact(self) -> None:
        stdout = "\n".join(["line one", "line two", "error: no template here"])

        message = _summarize_vcpkg_failure(stdout, "", 2)

        self.assertIn("line one", message)
        self.assertIn("error: no template here", message)
        self.assertNotIn("issue-report template removed", message)

    def test_stderr_is_searched_for_the_cause_too(self) -> None:
        message = _summarize_vcpkg_failure("-- Restoring", "CMake Error at ports/fmt/portfile.cmake:3", 1)

        self.assertIn("CMake Error at ports/fmt/portfile.cmake:3", message)

    def test_empty_output_still_reports_the_exit_code(self) -> None:
        # The old `or` fallback could not fire once the window held boilerplate.
        # Reporting the code unconditionally is what makes it reliable.
        message = _summarize_vcpkg_failure("", "", 6)

        self.assertEqual(message, "vcpkg exited with 6")

    def test_diagnostic_lines_are_deduplicated_and_capped(self) -> None:
        repeated = ["error: repeated failure"] * 5
        distinct = [f"error: distinct failure {index}" for index in range(20)]
        stdout = "\n".join([*repeated, *distinct])

        message = _summarize_vcpkg_failure(stdout, "", 1)
        header = message.split("--- vcpkg output", 1)[0]

        self.assertEqual(header.count("error: repeated failure"), 1)
        self.assertIn("further diagnostic line(s) omitted", message)

    def test_a_long_run_is_bounded(self) -> None:
        stdout = "\n".join(f"line {index}" for index in range(5000))

        message = _summarize_vcpkg_failure(stdout, "", 1)

        self.assertLess(len(message.splitlines()), 100)
        self.assertIn("line 4999", message)


if __name__ == "__main__":
    unittest.main()
