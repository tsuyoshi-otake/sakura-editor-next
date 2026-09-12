"""Contracts for the one native Rust staticlib both toolchains link (#303).

A native core that moves to Rust must have one implementation to verify, not
one per toolchain.  MinGW therefore builds and links the same
``sakura_native_ffi`` archive MSVC links, from the same pinned release, while
its production providers stay C++.  That agreement is expressed in exactly
three places, and these tests keep them from drifting apart: the pinned
toolchain file declares both Windows targets, the CMake provider block selects
a triple per toolchain instead of gating the archive on MSVC, and the MinGW
workflow installs that same pinned release with the GNU target.
"""

from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
NATIVE_RUST_TOOLCHAIN = REPO_ROOT / "rust/native/rust-toolchain.toml"
SAKURA_CMAKE = REPO_ROOT / "src/main/cmake/sakura.cmake"
MINGW_WORKFLOW = REPO_ROOT / ".github/workflows/build-on-msys2.yml"

MSVC_TRIPLE = "x86_64-pc-windows-msvc"
GNU_TRIPLE = "x86_64-pc-windows-gnu"

_IF = re.compile(r"^if\s*\(")
_ENDIF = re.compile(r"^endif\s*\(")


def _condition_depths(text: str) -> dict[str, int]:
    """Map each CMake line to the ``if()`` nesting depth it executes at.

    Only ``if``/``endif`` are counted.  A line that appears once is keyed by
    its stripped text, so a contract can state that a command is reached
    unconditionally rather than merely present somewhere in the file.
    """

    depth = 0
    depths: dict[str, int] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if _ENDIF.match(stripped):
            depth -= 1
        depths.setdefault(stripped, depth)
        if _IF.match(stripped):
            depth += 1
    depths["\0final"] = depth
    return depths


class NativeRustToolchainParityTests(unittest.TestCase):
    def setUp(self) -> None:
        with NATIVE_RUST_TOOLCHAIN.open("rb") as stream:
            self.toolchain = tomllib.load(stream)["toolchain"]
        self.cmake = SAKURA_CMAKE.read_text(encoding="utf-8-sig")
        self.workflow = MINGW_WORKFLOW.read_text(encoding="utf-8-sig")
        self.depths = _condition_depths(self.cmake)

    def test_declared_targets_match_the_triples_cmake_selects(self) -> None:
        self.assertRegex(self.toolchain["channel"], r"^[0-9]+\.[0-9]+\.[0-9]+$")
        selected = set(re.findall(r"set\(SAKURA_NATIVE_FFI_TARGET (\S+)\)", self.cmake))
        self.assertEqual(selected, {MSVC_TRIPLE, GNU_TRIPLE})
        self.assertEqual(set(self.toolchain["targets"]), selected)

    def test_cmake_reaches_the_archive_without_an_msvc_gate(self) -> None:
        self.assertEqual(self.depths["\0final"], 0, "unbalanced if()/endif()")
        for command in (
            "find_program(SAKURA_CARGO_EXECUTABLE cargo)",
            "add_dependencies(sakura_native_ffi sakura_native_ffi_build)",
            "target_compile_definitions(sakura_core PUBLIC SAKURA_UTF16_RUST_CANDIDATE)",
            "target_link_libraries(sakura_core PUBLIC sakura_native_ffi)",
        ):
            self.assertIn(command, self.depths, f"sakura.cmake lost: {command}")
            self.assertEqual(
                self.depths[command],
                0,
                f"{command} must not be reached conditionally",
            )

    def test_only_the_triple_and_the_archive_name_differ_per_toolchain(self) -> None:
        self.assertIn(f"set(SAKURA_NATIVE_FFI_TARGET {MSVC_TRIPLE})", self.cmake)
        self.assertIn(f"set(SAKURA_NATIVE_FFI_TARGET {GNU_TRIPLE})", self.cmake)
        self.assertIn(
            "set(SAKURA_NATIVE_FFI_LIBRARY_NAME sakura_native_ffi.lib)",
            self.cmake,
        )
        self.assertIn(
            "set(SAKURA_NATIVE_FFI_LIBRARY_NAME libsakura_native_ffi.a)",
            self.cmake,
        )
        self.assertEqual(
            self.cmake.count("add_library(sakura_native_ffi STATIC IMPORTED GLOBAL)"),
            1,
            "exactly one Rust staticlib may reach the final link",
        )

    def test_mingw_production_providers_remain_cpp(self) -> None:
        # Linking the archive is not adopting a backend.  The provider guard
        # stays fatal for MinGW, and the workflow pins the selector so the
        # decision cannot arrive from a caller or the runner environment.
        self.assertIn(
            "MinGW currently retains only the legacy C++ UTF-16 compatibility backend",
            self.cmake,
        )
        self.assertIn(
            "MinGW currently retains only the C++ Output authority backend",
            self.cmake,
        )
        self.assertIn("SAKURA_UTF16_BACKEND: cpp", self.workflow)
        self.assertNotIn("SAKURA_UTF16_BACKEND: rust", self.workflow)

    def test_mingw_installs_the_pinned_release_for_the_gnu_target(self) -> None:
        self.assertIn(
            "Get-Content -LiteralPath 'rust-toolchain.toml' -Raw",
            self.workflow,
        )
        self.assertIn("channel must be an exact numeric release", self.workflow)
        self.assertIn(
            "rustup toolchain install $pin --profile minimal "
            f"--target {GNU_TRIPLE} --no-self-update",
            self.workflow,
        )
        self.assertIn(f"rustc \"+$pin\" --target {GNU_TRIPLE}", self.workflow)
        # The release is resolved from the toolchain file, never restated here,
        # and never taken from a package whose version this repository does not
        # control.
        self.assertNotIn(
            f"toolchain install {self.toolchain['channel']}",
            self.workflow,
        )
        self.assertNotIn("mingw-w64-x86_64-rust", self.workflow)

    def test_every_mingw_build_names_the_pinned_cargo_proxy(self) -> None:
        # The msys2 shell rebuilds PATH from a minimal set, so a build step that
        # does not name Cargo's proxy directory cannot resolve cargo at all, and
        # one that inherits the whole Windows PATH could resolve a different
        # cargo than the step above pinned.
        self.assertEqual(
            self.workflow.count('"SAKURA_CARGO_BIN=$(Split-Path -Parent $cargo)"'),
            1,
        )
        self.assertEqual(
            self.workflow.count(
                'export PATH="$(cygpath --unix "${SAKURA_CARGO_BIN}"):${PATH}"'
            ),
            self.workflow.count("./build-gnu.bat "),
        )


if __name__ == "__main__":
    unittest.main()
