from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.model import (  # noqa: E402
    Artifact,
    CompileProfiles,
    Component,
    Context,
    Edge,
    SemanticGraph,
)
from sakura_build_lib.package_restore import (  # noqa: E402
    package_gc,
    plan_package_restore,
    restore_package_closure,
    validate_package_restore,
)
from sakura_build_lib.runner import BuildError  # noqa: E402


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _package_graph(root: Path):
    _write(root / "vcpkg.json", json.dumps({"dependencies": ["fmt"]}) + "\n")
    context = Context("msvc-x64-debug", "x64", "x64", "Debug", "msvc", "msbuild", "development", ())
    product = Component(
        "product",
        "test",
        "executable",
        "candidate",
        "legacy",
        (context.id,),
        "app",
        (),
        (),
        (),
        (),
        (),
        (),
        "test state",
        {"msbuild": ("product.vcxproj",)},
        None,
    )
    vcpkg = root / "tools/vcpkg"
    _write(vcpkg / "vcpkg.exe", "test executable\n")
    _write(vcpkg / "scripts/vcpkg-tool-metadata.txt", "VCPKG_TOOL_RELEASE_TAG=test\n")
    _write(vcpkg / "scripts/buildsystems/vcpkg.cmake", "# test toolchain\n")
    _write(vcpkg / "triplets/x64-windows-static.cmake", "# target\n")
    _write(vcpkg / "triplets/x64-windows.cmake", "# host\n")
    package = Artifact(
        "vcpkg-root-package-set",
        "product",
        "package_set",
        ("vcpkg.json",),
        ("vcpkg:fmt",),
        "vcpkg",
        True,
    )
    edge = Edge(
        "product-to-vcpkg-root-package-set",
        "product",
        package.id,
        "build",
        ("compile", "link", "test"),
        "private",
        "none",
        None,
        True,
        True,
        (),
    )
    return SemanticGraph(
        root.resolve(),
        root / "src/main/modules/modules.json",
        3,
        "0.3.7",
        {context.id: context},
        {product.id: product},
        {},
        {package.id: package},
        CompileProfiles(1, {}, {}, {}, {context.id: {"project_profile": "test", "link_profile": "test"}}),
        (edge,),
        "sha256:test-package-graph",
    )


def _complete_fake_install(argv: list[str], **_kwargs) -> tuple[str, str]:
    install_root = Path(next(item.split("=", 1)[1] for item in argv if item.startswith("--x-install-root=")))
    triplet = next(item.split("=", 1)[1] for item in argv if item.startswith("--triplet="))
    (install_root / triplet / "share/fmt").mkdir(parents=True, exist_ok=True)
    return "", ""


class PackageRestoreTests(unittest.TestCase):
    def test_restore_reuse_validate_and_gc_are_content_addressed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph = _package_graph(root)
            with patch("sakura_build_lib.package_restore._run_vcpkg", side_effect=_complete_fake_install) as run_vcpkg:
                plan = plan_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertTrue(plan["required"])
                self.assertEqual("build/pkg/v/a/x64-windows-static.cmake", plan["active_cmake_relative"])

                first = restore_package_closure(graph, ("product",), "msvc-x64-debug")
                self.assertEqual("restored", first["status"])
                self.assertTrue(first["native_restore_execution_observed"])
                self.assertEqual(1, run_vcpkg.call_count)
                active_cmake = root / str(first["active_cmake_relative"])
                self.assertIn("VCPKG_MANIFEST_MODE OFF", active_cmake.read_text(encoding="utf-8"))

                validated = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertTrue(validated["valid"])
                self.assertEqual("validated", validated["status"])

                # bootstrap-vcpkg may replace this host-local executable after a
                # completed package receipt was published. The tracked vcpkg
                # tool metadata, toolchain, and triplets still identify the
                # same package tool source, so the active closure remains valid.
                _write(root / "tools/vcpkg/vcpkg.exe", "replacement bootstrap executable\n")
                after_bootstrap = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertTrue(after_bootstrap["valid"])

                reused = restore_package_closure(graph, ("product",), "msvc-x64-debug")
                self.assertEqual("reused", reused["status"])
                self.assertTrue(reused["native_restore_execution_observed"])
                self.assertFalse(reused["restore_performed_this_invocation"])
                self.assertEqual(1, run_vcpkg.call_count)

                _write(root / "vcpkg.json", json.dumps({"dependencies": ["fmt"], "builtin-baseline": "changed"}) + "\n")
                stale = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertFalse(stale["valid"])
                self.assertEqual("stale_or_missing", stale["status"])

                refreshed = restore_package_closure(graph, ("product",), "msvc-x64-debug")
                self.assertEqual("restored", refreshed["status"])
                self.assertEqual(2, run_vcpkg.call_count)
                preview = package_gc(root, keep=1, apply=False)
                self.assertEqual(1, len(preview["candidate_entries"]))
                applied = package_gc(root, keep=1, apply=True)
                self.assertEqual(preview["candidate_entries"], applied["removed_entries"])

    def test_declared_package_closure_must_match_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph = _package_graph(root)
            _write(root / "vcpkg.json", json.dumps({"dependencies": ["fmt", "gtest"]}) + "\n")
            with self.assertRaises(BuildError) as raised:
                plan_package_restore(graph, ("product",), "msvc-x64-debug")
            self.assertEqual("PACKAGE_CLOSURE_MISMATCH", raised.exception.code)

    def test_tracked_vcpkg_metadata_invalidates_a_completed_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph = _package_graph(root)
            with patch("sakura_build_lib.package_restore._run_vcpkg", side_effect=_complete_fake_install):
                restored = restore_package_closure(graph, ("product",), "msvc-x64-debug")
                self.assertEqual("restored", restored["status"])

                _write(
                    root / "tools/vcpkg/scripts/vcpkg-tool-metadata.txt",
                    "VCPKG_TOOL_RELEASE_TAG=changed\n",
                )
                stale = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertFalse(stale["valid"])
                self.assertEqual("stale_or_missing", stale["status"])

    def test_gc_uses_external_lru_metadata_and_enforces_capacity_when_safe(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "build/pkg/v"
            _write(cache / "e/older/payload.bin", "old")
            _write(cache / "e/newer/payload.bin", "newer")
            _write(
                cache / "u/older.json",
                json.dumps({"schema_version": 1, "entry_relative": "e/older", "last_used_unix_ns": 10}) + "\n",
            )
            _write(
                cache / "u/newer.json",
                json.dumps({"schema_version": 1, "entry_relative": "e/newer", "last_used_unix_ns": 20}) + "\n",
            )

            lru = package_gc(root, keep=1, max_bytes=None)
            self.assertEqual(["e/older"], lru["candidate_entries"])
            self.assertEqual(["retention_count"], lru["candidate_reasons"]["e/older"])

            capacity = package_gc(root, keep=2, max_bytes=0)
            self.assertEqual(["e/older", "e/newer"], capacity["candidate_entries"])
            self.assertEqual(["capacity"], capacity["candidate_reasons"]["e/older"])
            self.assertTrue(capacity["capacity_satisfied"])


if __name__ == "__main__":
    unittest.main()
