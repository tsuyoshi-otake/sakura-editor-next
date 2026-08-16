from __future__ import annotations

import contextlib
import io
import json
import shutil
import subprocess
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
    _RESTORE_DOWNLOAD_ATTEMPTS,
    _RESTORE_DOWNLOAD_BACKOFF_SECONDS,
    _run_vcpkg,
    _transient_download_reason,
    package_gc,
    plan_package_restore,
    restore_package_closure,
    validate_package_restore,
)
from sakura_build_lib.runner import BuildError, EventWriter  # noqa: E402


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _package_graph(root: Path, *, package_inputs: tuple[str, ...] = ("vcpkg.json",)):
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
        package_inputs,
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

    def test_run_local_output_beside_explicit_package_sources_does_not_stale_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write(root / "package-source/CMakeLists.txt", "add_library(helper MODULE helper.cpp)\n")
            _write(root / "package-source/helper.cpp", "int helper() { return 1; }\n")
            graph = _package_graph(
                root,
                package_inputs=(
                    "vcpkg.json",
                    "package-source/CMakeLists.txt",
                    "package-source/helper.cpp",
                ),
            )

            def complete_with_run_local_output(argv: list[str], **kwargs) -> tuple[str, str]:
                result = _complete_fake_install(argv, **kwargs)
                _write(root / "package-source/out/Release/helper.pdb", "host-local build output\n")
                return result

            with patch(
                "sakura_build_lib.package_restore._run_vcpkg",
                side_effect=complete_with_run_local_output,
            ):
                restored = restore_package_closure(graph, ("product",), "msvc-x64-debug")
                self.assertEqual("restored", restored["status"])
                after_output = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertTrue(after_output["valid"])
                self.assertEqual(restored["plan_hash"], after_output["plan_hash"])

                _write(root / "package-source/helper.cpp", "int helper() { return 2; }\n")
                after_source_change = validate_package_restore(graph, ("product",), "msvc-x64-debug")
                self.assertFalse(after_source_change["valid"])
                self.assertNotEqual(restored["plan_hash"], after_source_change["plan_hash"])

    @unittest.skipUnless(shutil.which("git"), "git is required")
    def test_source_revision_change_changes_plan_hash_without_tree_bytes_changing(self) -> None:
        git = shutil.which("git")
        assert git is not None
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "package-source"
            source.mkdir(parents=True)
            _write(source / "helper.cpp", "int helper() { return 1; }\n")
            self._git(git, source, "init")
            self._git(git, source, "add", "helper.cpp")
            self._git(
                git,
                source,
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "commit",
                "-m",
                "source",
            )
            graph = _package_graph(root, package_inputs=("vcpkg.json", "package-source"))
            first = plan_package_restore(graph, ("product",), "msvc-x64-debug")
            self._git(
                git,
                source,
                "-c",
                "user.name=Test",
                "-c",
                "user.email=test@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--allow-empty",
                "-m",
                "revision only",
            )
            second = plan_package_restore(graph, ("product",), "msvc-x64-debug")
            self.assertNotEqual(
                first["plan_hash"],
                second["plan_hash"],
                "an empty source-revision bump must change plan_hash even when "
                "the walked tree bytes are unchanged",
            )

    def _git(self, git: str, cwd: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [git, *arguments],
            cwd=cwd,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        return result

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


# Observed verbatim in CI while github.com release/archive delivery degraded for
# runners on 2026-08-12. The wording is the classifier's only input, so keep
# these samples as vcpkg actually prints them.
_TRANSIENT_503 = (
    "Downloading https://github.com/Microsoft/GSL/archive/a353456.tar.gz\n"
    "Attempt 1 of 3, retrying download.\n"
    "error: curl operation failed with response code 503.\n"
    "error: Reached maximum number of attempts, won't retry download from "
    "https://github.com/Microsoft/GSL/archive/a353456.tar.gz.\n"
    "error: building ms-gsl:x64-windows-static failed with: BUILD_FAILED\n"
)
_TRANSIENT_CURL_56 = "error: curl operation failed with error code 56 (Failure when receiving data from the peer).\n"
_PERMANENT_404 = "error: curl operation failed with response code 404.\n"
_PERMANENT_HASH = (
    "error: File does not have the expected hash:\n"
    "error: building fmt:x64-windows-static failed with: BUILD_FAILED\n"
)
_PORT_BUILD_FAILURE = (
    "error: building fmt:x64-windows-static failed with: BUILD_FAILED\n"
    "fmt/core.h(120): fatal error C1083: Cannot open include file\n"
)


class PackageRestoreDownloadRetryTests(unittest.TestCase):
    """A fetch outage must not discard a restore; a real defect must not repeat."""

    def _run(self, outputs, *, timeout_seconds: int = 600, events: EventWriter | None = None):
        pending = list(outputs)
        with patch("sakura_build_lib.package_restore._spawn_vcpkg", side_effect=lambda *_a, **_k: pending.pop(0)) as spawn:
            with patch("sakura_build_lib.package_restore.time.sleep") as sleep:
                captured = io.StringIO()
                with contextlib.redirect_stderr(captured):
                    try:
                        result = _run_vcpkg(
                            ["vcpkg.exe", "install"],
                            repo_root=Path("."),
                            environment={},
                            timeout_seconds=timeout_seconds,
                            events=events,
                        )
                        error = None
                    except BuildError as raised:
                        result, error = None, raised
        return result, error, spawn, sleep, captured.getvalue()

    def test_transient_fetch_failure_is_retried_and_then_succeeds(self) -> None:
        events = EventWriter(io.StringIO())
        result, error, spawn, sleep, stderr = self._run(
            [(_TRANSIENT_503, "", 1), ("installed\n", "", 0)],
            events=events,
        )
        self.assertIsNone(error)
        self.assertEqual(("installed\n", ""), result)
        self.assertEqual(2, spawn.call_count)
        sleep.assert_called_once_with(_RESTORE_DOWNLOAD_BACKOFF_SECONDS[0])
        # An unattended EventWriter has no stream, so the build log is the only
        # place a human can see that an attempt was repeated.
        self.assertIn(f"PACKAGE_RESTORE_RETRY: attempt 1 of {_RESTORE_DOWNLOAD_ATTEMPTS}", stderr)
        self.assertIn("HTTP 503", stderr)

    def test_curl_transport_failure_is_retried(self) -> None:
        result, error, spawn, sleep, stderr = self._run([(_TRANSIENT_CURL_56, "", 1), ("", "", 0)])
        self.assertIsNone(error)
        self.assertEqual(2, spawn.call_count)
        self.assertIn("curl transport error 56", stderr)
        sleep.assert_called_once_with(_RESTORE_DOWNLOAD_BACKOFF_SECONDS[0])

    def test_exhausted_attempts_report_the_original_cause(self) -> None:
        _result, error, spawn, sleep, _stderr = self._run([(_TRANSIENT_503, "", 1)] * _RESTORE_DOWNLOAD_ATTEMPTS)
        self.assertIsNotNone(error)
        self.assertEqual("PACKAGE_RESTORE_FAILED", error.code)
        self.assertIn("curl operation failed with response code 503", str(error))
        self.assertEqual(_RESTORE_DOWNLOAD_ATTEMPTS, spawn.call_count)
        self.assertEqual(_RESTORE_DOWNLOAD_ATTEMPTS - 1, sleep.call_count)

    def test_a_port_that_does_not_compile_fails_on_the_first_attempt(self) -> None:
        _result, error, spawn, sleep, stderr = self._run([(_PORT_BUILD_FAILURE, "", 1)])
        self.assertEqual("PACKAGE_RESTORE_FAILED", error.code)
        self.assertEqual(1, spawn.call_count)
        sleep.assert_not_called()
        self.assertEqual("", stderr)

    def test_permanent_fetch_failures_are_not_retried(self) -> None:
        for sample in (_PERMANENT_404, _PERMANENT_HASH):
            with self.subTest(sample=sample.splitlines()[0]):
                _result, error, spawn, sleep, _stderr = self._run([(sample, "", 1)])
                self.assertEqual("PACKAGE_RESTORE_FAILED", error.code)
                self.assertEqual(1, spawn.call_count)
                sleep.assert_not_called()

    def test_retry_never_extends_the_caller_timeout_budget(self) -> None:
        # One second of budget cannot absorb the first backoff, so the failure
        # must surface instead of overrunning the wait the caller chose.
        _result, error, spawn, sleep, _stderr = self._run([(_TRANSIENT_503, "", 1)], timeout_seconds=1)
        self.assertEqual("PACKAGE_RESTORE_FAILED", error.code)
        self.assertEqual(1, spawn.call_count)
        sleep.assert_not_called()

    def test_the_retry_horizon_covers_a_minutes_long_outage(self) -> None:
        # The attempt count is not the property that matters; the horizon is.
        # On 2026-08-12 three independent retries lost to the same github.com
        # degradation because each gave up inside half a minute, while the path
        # recovered about two and a quarter minutes later.  Pin the horizon so a
        # future tidy-up cannot quietly shrink it back to a second-scale retry.
        self.assertGreaterEqual(len(_RESTORE_DOWNLOAD_BACKOFF_SECONDS), _RESTORE_DOWNLOAD_ATTEMPTS - 1)
        self.assertGreaterEqual(sum(_RESTORE_DOWNLOAD_BACKOFF_SECONDS[: _RESTORE_DOWNLOAD_ATTEMPTS - 1]), 135.0)

    def test_classifier_names_the_failure_shape(self) -> None:
        self.assertIn("HTTP 503", _transient_download_reason(_TRANSIENT_503, ""))
        self.assertIn("curl transport error 56", _transient_download_reason("", _TRANSIENT_CURL_56))
        self.assertIn(
            "WinHttpReceiveResponse got error",
            _transient_download_reason("WinHttpReceiveResponse got error 0x00002F78", ""),
        )
        self.assertIsNone(_transient_download_reason(_PERMANENT_404, ""))
        self.assertIsNone(_transient_download_reason(_PERMANENT_HASH, ""))
        self.assertIsNone(_transient_download_reason(_PORT_BUILD_FAILURE, ""))


if __name__ == "__main__":
    unittest.main()
