"""Executable contracts for the fail-closed main pull-request CI planner."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_BUILD = REPO_ROOT / "tools/build"
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
MINGW_WORKFLOW = REPO_ROOT / ".github/workflows/build-on-msys2.yml"
RUST_TOOLCHAIN = REPO_ROOT / "rust/rust-toolchain.toml"
CPU_DISPATCH_TEST = REPO_ROOT / "src/test/cpp/tests1/test-cpudispatch.cpp"

if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.ci_plan import (  # noqa: E402
    CiChangedFile,
    CiPlanError,
    changed_files_between,
    plan_ci,
)


BASE_SHA = "a" * 40
HEAD_SHA = "b" * 40
REPOSITORY = "tsuyoshi-otake/sakura-editor-next"


def decide(*changes: CiChangedFile | str, event_name: str = "pull_request") -> dict[str, object]:
    return plan_ci(
        changes,
        event_name=event_name,
        repository=REPOSITORY,
        head_repository=REPOSITORY,
        base_sha=BASE_SHA if event_name == "pull_request" else None,
        head_sha=HEAD_SHA if event_name == "pull_request" else None,
    )


class CiPlanDecisionTests(unittest.TestCase):
    def assertFull(self, result: dict[str, object], reason: str) -> None:
        self.assertEqual(result["mode"], "full_native")
        self.assertEqual(result["reason_codes"], [reason])
        self.assertEqual(result["jobs"], {"check_encoding": True, "native_build": True})

    def test_markdown_only_pull_request_omits_native_build(self) -> None:
        result = decide("README.md", "sakura_core/workbench/CLAUDE.md")
        self.assertEqual(result["mode"], "docs_only")
        self.assertEqual(result["reason_codes"], ["documentation_only"])
        self.assertEqual(result["jobs"], {"check_encoding": True, "native_build": False})

    def test_formal_documents_and_evidence_are_documentation(self) -> None:
        result = decide(
            "docs/formal/ControlStartupHandshake.tla",
            "docs/formal/ControlStartupHandshake_Current.cfg",
            "docs/evidence/結果 1.json",
        )
        self.assertEqual(result["mode"], "docs_only")

    def test_native_or_unknown_change_is_full(self) -> None:
        self.assertFull(decide("sakura_core/foo.cpp"), "native_or_unknown_change")
        self.assertFull(decide("unexpected.data"), "native_or_unknown_change")

    def test_issue239_rust_and_integration_paths_are_full_native(self) -> None:
        paths = (
            "rust/Cargo.toml",
            "rust/sakura_rust_core/Cargo.toml",
            "rust/Cargo.lock",
            "rust/rust-toolchain.toml",
            "rust/sakura_rust_core/src/lib.rs",
            "sakura_core/util/CpuDispatchRust.cpp",
            "sakura_core/util/RustUtf16Scan.h",
            "src/main/msbuild/sakura-rust-core.targets",
            "src/main/cmake/sakura-utf16-backend.cmake",
            "src/main/cmake/build-rust-sakura-core.cmake",
        )
        for path in paths:
            with self.subTest(path=path):
                self.assertFull(decide(path), "native_or_unknown_change")

    def test_adding_any_non_document_change_can_only_strengthen_the_plan(self) -> None:
        docs = decide("README.md")
        mixed = decide("README.md", "sakura_core/foo.cpp")
        strength = {"docs_only": 0, "full_native": 1}
        self.assertGreaterEqual(strength[str(mixed["mode"])], strength[str(docs["mode"])])

    def test_delete_rename_copy_and_type_change_are_full(self) -> None:
        for status in ("D", "R100", "C100", "T"):
            with self.subTest(status=status):
                self.assertFull(
                    decide(CiChangedFile("docs/README.md", status)),
                    "native_or_unknown_change",
                )

    def test_empty_diff_and_fork_are_full(self) -> None:
        self.assertFull(decide(), "no_changed_files")
        result = plan_ci(
            ["README.md"],
            event_name="pull_request",
            repository=REPOSITORY,
            head_repository="someone/fork",
            base_sha=BASE_SHA,
            head_sha=HEAD_SHA,
        )
        self.assertFull(result, "untrusted_or_unknown_head_repository")

    def test_main_push_and_other_non_pr_events_are_always_full(self) -> None:
        for event_name in ("push", "workflow_dispatch", "workflow_call"):
            with self.subTest(event_name=event_name):
                self.assertFull(decide("README.md", event_name=event_name), "non_pull_request_event")

    def test_invalid_sha_and_escaping_path_are_rejected(self) -> None:
        with self.assertRaisesRegex(CiPlanError, "base SHA"):
            plan_ci(
                ["README.md"],
                event_name="pull_request",
                repository=REPOSITORY,
                head_repository=REPOSITORY,
                base_sha="not-a-sha",
                head_sha=HEAD_SHA,
            )
        with self.assertRaisesRegex(CiPlanError, "repository-relative"):
            decide("../README.md")


class CiRustNativeWorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = BUILD_WORKFLOW.read_text(encoding="utf-8-sig")
        self.mingw_workflow = MINGW_WORKFLOW.read_text(encoding="utf-8-sig")
        with RUST_TOOLCHAIN.open("rb") as stream:
            self.toolchain = tomllib.load(stream)["toolchain"]["channel"]

    def step(self, name: str) -> str:
        marker = f"    - name: {name}\n"
        self.assertEqual(
            self.workflow.count(marker),
            1,
            f"expected exactly one workflow step named {name!r}",
        )
        start = self.workflow.index(marker)
        end = self.workflow.find("\n    - name:", start + len(marker))
        if end < 0:
            end = len(self.workflow)
        return self.workflow[start:end]

    def test_toolchain_toml_is_the_single_exact_ci_pin(self) -> None:
        self.assertRegex(self.toolchain, r"^[0-9]+\.[0-9]+\.[0-9]+$")
        prepare = self.step("Prepare pinned Rust toolchain")
        self.assertIn("Get-Content -LiteralPath 'rust-toolchain.toml' -Raw", prepare)
        self.assertIn("channel must be an exact numeric release", prepare)
        self.assertIn('"RUST_TOOLCHAIN_PIN=$pin"', prepare)
        self.assertIn("rustup toolchain install $pin", prepare)
        self.assertIn("$toolchainArgument = \"+$pin\"", prepare)
        self.assertNotIn(f"toolchain install {self.toolchain}", self.workflow)
        self.assertNotIn(f"cargo +{self.toolchain}", self.workflow)

    def test_static_gate_proves_dependency_free_single_staticlib_shape(self) -> None:
        static_gate = self.step("Rust static checks (Debug matrix cell)")
        self.assertIn(
            "if: ${{ !inputs.release_promotion && matrix.config == 'Debug' }}",
            static_gate,
        )
        self.assertIn("fmt --all -- --check", static_gate)
        self.assertIn("metadata --locked --no-deps --format-version 1", static_gate)
        self.assertIn("ConvertFrom-Json", static_gate)
        self.assertIn("$metadata.workspace_members", static_gate)
        self.assertIn("$package.dependencies", static_gate)
        self.assertIn("$targets[0].kind", static_gate)
        self.assertIn("$targets[0].crate_types", static_gate)
        self.assertIn(
            "clippy --workspace --all-targets --all-features --locked -- -D warnings",
            static_gate,
        )

    def test_debug_release_rust_and_msbuild_headless_gates_are_in_native_job(self) -> None:
        cargo_gate = self.step("Rust tests and product staticlib (${{ matrix.config }})")
        self.assertIn("test --workspace --locked --release", cargo_gate)
        self.assertIn("test --workspace --locked --no-fail-fast", cargo_gate)
        self.assertIn(
            "build --workspace --package sakura-rust-core --locked "
            "--target x86_64-pc-windows-msvc",
            cargo_gate,
        )

        rust_headless = self.step(
            "MSBuild Rust backend full headless gate (${{ matrix.config }})"
        )
        for block in (rust_headless,):
            self.assertIn("$env:HEADLESS_GTEST_EXCLUDES", block)
            self.assertIn("'--test-dir' 'build/${{ matrix.platform }}/CMakeTools'", block)
            self.assertIn("'--timeout' '600'", block)

        self.assertIn("SAKURA_UTF16_BACKEND: rust", self.workflow)
        self.assertIn("SAKURA_UTF16_PRODUCTION_PACKAGE: true", self.workflow)
        self.assertNotIn("SAKURA_UTF16_BACKEND: both", self.workflow)
        self.assertLess(
            self.workflow.index("MSBuild Rust integration (rust"),
            self.workflow.index("MSBuild Rust backend full headless gate"),
        )
        self.assertIn(
            "needs: [check-encoding, build-vcpkg-msvc, build]",
            self.workflow,
        )

    def test_rust_gates_preserve_historical_release_promotion(self) -> None:
        for name in (
            "Prepare pinned Rust toolchain",
            "Rust tests and product staticlib (${{ matrix.config }})",
            "MSBuild Rust integration (rust, ${{ matrix.config }})",
            "Rust integration initialization (rust, ${{ matrix.config }})",
            "Rust integration focused tests (rust, ${{ matrix.config }})",
            "MSBuild Rust backend full headless gate (${{ matrix.config }})",
        ):
            with self.subTest(step=name):
                self.assertIn(
                    "if: ${{ !inputs.release_promotion }}",
                    self.step(name),
                )

    def test_focused_filters_exist_and_fail_closed_on_inventory_or_count_drift(self) -> None:
        required_names = set(
            re.findall(r"'CpuDispatchTest\.([A-Za-z0-9_]+)'", self.workflow)
        )
        required_names.update(
            re.findall(
                r"--gtest_filter=CpuDispatchTest\.([A-Za-z0-9_]+)",
                self.workflow,
            )
        )
        source_names = set(
            re.findall(
                r"TEST\(CpuDispatchTest,\s*([A-Za-z0-9_]+)\)",
                CPU_DISPATCH_TEST.read_text(encoding="utf-8-sig"),
            )
        )
        self.assertTrue(required_names)
        self.assertEqual(required_names - source_names, set())
        self.assertNotIn(
            "RustUtf16ScannersRespectFfiAndThreePageGuardContracts",
            self.workflow,
        )
        for name in (
            "RustUtf16ScannersRejectInvalidFfiSpans",
            "RustUtf16ScannersMatchEveryLegalAlignment",
            "RustUtf16ScannersTouchBothGuardPagesAtEveryShortLength",
        ):
            self.assertIn(f"'CpuDispatchTest.{name}'", self.workflow)
        self.assertEqual(self.workflow.count("'--gtest_list_tests'"), 1)
        self.assertEqual(
            self.workflow.count("focused gtest run did not report all"),
            1,
        )
        self.assertEqual(
            self.workflow.count(
                "initialization diagnostics did not execute exactly one test"
            ),
            1,
        )

    def test_mingw_workflow_cannot_inherit_a_rust_backend(self) -> None:
        mingw_job = self.mingw_workflow[
            self.mingw_workflow.index("  mingw:\n") :
        ]
        self.assertRegex(
            mingw_job,
            r"(?m)^    env:\n(?:^      .+\n)*"
            r"^      SAKURA_UTF16_BACKEND: cpp$",
        )


class CiPlanGitDiffTests(unittest.TestCase):
    def run_git(self, repo: Path, *arguments: str) -> str:
        completed = subprocess.run(
            ("git", "-c", "user.name=CI Plan Test", "-c", "user.email=ci-plan@example.invalid", *arguments),
            cwd=repo,
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()

    def test_exact_base_head_diff_preserves_spaces_unicode_and_status(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            repo = Path(raw_directory)
            self.run_git(repo, "init", "--initial-branch=main")
            document = repo / "docs" / "設計 メモ.md"
            document.parent.mkdir()
            document.write_text("before\n", encoding="utf-8")
            self.run_git(repo, "add", "--", document.relative_to(repo).as_posix())
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "base")
            base = self.run_git(repo, "rev-parse", "HEAD")

            document.write_text("after\n", encoding="utf-8")
            self.run_git(repo, "add", "--", document.relative_to(repo).as_posix())
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "head")
            head = self.run_git(repo, "rev-parse", "HEAD")

            self.assertEqual(
                changed_files_between(repo, base, head),
                (CiChangedFile("docs/設計 メモ.md", "M"),),
            )

    def test_rename_status_cannot_be_misclassified_as_documentation_only(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            repo = Path(raw_directory)
            self.run_git(repo, "init", "--initial-branch=main")
            original = repo / "README.md"
            renamed = repo / "docs" / "README.md"
            original.write_text("documentation\n", encoding="utf-8")
            self.run_git(repo, "add", "README.md")
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "base")
            base = self.run_git(repo, "rev-parse", "HEAD")

            renamed.parent.mkdir()
            self.run_git(repo, "mv", "README.md", "docs/README.md")
            self.run_git(repo, "-c", "commit.gpgsign=false", "commit", "-m", "head")
            head = self.run_git(repo, "rev-parse", "HEAD")

            changes = changed_files_between(repo, base, head)
            self.assertEqual(changes, (CiChangedFile("docs/README.md", "R100"),))
            decision = decide(*changes)
            self.assertEqual(decision["mode"], "full_native")
            self.assertEqual(decision["reason_codes"], ["native_or_unknown_change"])


if __name__ == "__main__":
    unittest.main()
