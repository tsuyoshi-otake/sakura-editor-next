"""Contracts for the pinned mingw-w64 CRT (#378).

``msys2/setup-msys2`` with ``update: true`` installs the newest CRT MSYS2 has
published. The r532 CRT (2026-10-23) left ``__mingw_fe_dfl_env`` undefined in
``libmingwex.a``, so every static MinGW link that pulls in ``fesetenv`` failed
and both release-required MinGW checks turned red with no source change.
``.github/actions/setup-locked-msys2`` installs an aged, verified toolchain and
``.github/actions/pin-mingw-crt`` rechecks the known-good revision. These tests
keep the pin complete and keep every MinGW toolchain setup behind it.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
ACTION = REPO_ROOT / ".github/actions/pin-mingw-crt/action.yml"
WORKFLOWS = REPO_ROOT / ".github/workflows"
PIN_USES = "uses: ./.github/actions/pin-mingw-crt"

# One mingw-w64 source revision produces all four packages; mixing revisions
# pairs headers with a CRT built from different sources.
PINNED_PACKAGES = (
    "crt",
    "headers",
    "libwinpthread",
    "winpthreads",
)

CHECKSUM_LINE = re.compile(
    r"(?m)^\s*([0-9a-f]{64})  "
    r"mingw-w64-x86_64-([a-z]+)-(14\.0\.0\.r\d+\.g[0-9a-f]+)-(\d+)-any\.pkg\.tar\.zst$"
)


def _steps(workflow: str) -> list[str]:
    starts = [m.start() for m in re.finditer(r"(?m)^\s*- (?:name:|uses:|run:)", workflow)]
    return [workflow[a:b] for a, b in zip(starts, starts[1:] + [len(workflow)])]


class MinGWCrtPinTests(unittest.TestCase):
    def setUp(self) -> None:
        self.action = ACTION.read_text(encoding="utf-8-sig")

    def test_pin_lists_each_package_once_at_one_revision(self) -> None:
        entries = CHECKSUM_LINE.findall(self.action)
        self.assertEqual(
            sorted(name for _, name, _, _ in entries),
            sorted(PINNED_PACKAGES),
        )
        self.assertEqual(
            len({revision for _, _, revision, _ in entries}),
            1,
            "the CRT, headers, and winpthreads must come from one source revision",
        )

    def test_pin_verifies_bytes_and_signature_before_installing(self) -> None:
        verify = self.action.index("sha256sum --check --strict SHA256SUMS")
        install = self.action.index("pacman --upgrade --noconfirm")
        self.assertLess(verify, install, "hashes must be checked before pacman runs")
        self.assertIn('"$base/$file.sig"', self.action)
        self.assertIn("https://repo.msys2.org/mingw/mingw64", self.action)
        self.assertIn("set -euo pipefail", self.action)

    def test_every_locked_mingw_toolchain_setup_is_followed_by_the_pin(self) -> None:
        guarded = 0
        for workflow_path in sorted(WORKFLOWS.glob("*.yml")):
            steps = _steps(workflow_path.read_text(encoding="utf-8-sig"))
            for index, step in enumerate(steps):
                self.assertFalse(
                    "msys2/setup-msys2@" in step and "mingw-w64-x86_64-gcc" in step,
                    "a rolling package installation bypasses the age-verified lock",
                )
                if "uses: ./.github/actions/setup-locked-msys2" not in step:
                    continue
                if "profile: cppcheck" in step or "profile: doxygen" in step:
                    continue
                with self.subTest(workflow=workflow_path.name, step=index):
                    self.assertLess(index + 1, len(steps))
                    self.assertIn(
                        PIN_USES,
                        steps[index + 1],
                        "the pin must run before anything compiles with the CRT",
                    )
                    guarded += 1
        # build-on-msys2.yml's mingw job and build-sakura.yml's vcpkg MinGW job.
        self.assertEqual(guarded, 2)


if __name__ == "__main__":
    unittest.main()
