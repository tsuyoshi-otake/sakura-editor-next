from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.runner import BuildError  # noqa: E402
from sakura_build_lib.semantic_inventory import (  # noqa: E402
    _normalise_line_endings,
    _unchanged_line_map,
    accept_semantic_inventory,
    collect_semantic_inventory,
    compare_semantic_inventory,
    write_semantic_inventory,
)


def _git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        capture_output=True,
        check=False,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode:
        raise AssertionError(
            f"git {' '.join(arguments)} failed in {root}:\n{completed.stdout}\n{completed.stderr}"
        )
    return completed.stdout.strip()


def _write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _initialise_repo(root: Path, files: dict[str, str] | None = None) -> str:
    _git(root, "init", "--quiet")
    _git(root, "config", "user.email", "semantic-inventory@example.invalid")
    _git(root, "config", "user.name", "Semantic Inventory Test")
    for relative, text in (files or {"sakura_core/base.cpp": "void Base() {}\n"}).items():
        _write(root, relative, text)
    _git(root, "add", "--all")
    _git(root, "commit", "--quiet", "-m", "initial semantic fixture")
    return _git(root, "rev-parse", "HEAD")


def _commit(root: Path, message: str) -> str:
    _git(root, "add", "--all")
    _git(root, "commit", "--quiet", "-m", message)
    return _git(root, "rev-parse", "HEAD")


def _finding_paths(inventory: dict[str, object]) -> set[tuple[str, str, int]]:
    return {
        (str(item["rule_id"]), str(item["path"]), int(item["line"]))
        for item in inventory["findings"]  # type: ignore[index]
    }


class SemanticInventoryTests(unittest.TestCase):
    def _temporary_repo(self, files: dict[str, str] | None = None) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        _initialise_repo(root, files)
        return temporary, root

    def test_git_index_scope_is_identical_for_uninitialised_and_populated_gitlink(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "parent"
            root.mkdir()
            _initialise_repo(root, {"sakura_core/window/CEditWnd.cpp": "void f() { GetDllShareData(); }\n"})

            vendor = Path(temporary) / "vcpkg-source"
            vendor.mkdir()
            vendor_commit = _initialise_repo(vendor, {"noisy.cpp": "void f() { GetEditWnd(); new Widget; }\n"})
            _git(root, "update-index", "--add", "--cacheinfo", f"160000,{vendor_commit},tools/vcpkg")
            _git(root, "commit", "--quiet", "-m", "record vcpkg gitlink")

            uninitialised = collect_semantic_inventory(root)
            self.assertNotIn("tools/vcpkg/noisy.cpp", uninitialised["tracked_source_paths"])

            subprocess.run(["git", "clone", "--quiet", str(vendor), str(root / "tools/vcpkg")], check=True)
            initialised = collect_semantic_inventory(root)
            _write(root, "tools/vcpkg/buildtrees/generated/noisy.cpp", "void f() { GetEditDoc(); delete item; }\n")
            _write(root, "sakura_core/untracked.cpp", "void f() { GetEditDoc(); }\n")
            populated = collect_semantic_inventory(root)

            self.assertEqual(uninitialised, initialised)
            self.assertEqual(uninitialised, populated)
            self.assertEqual(1, uninitialised["metrics"]["direct_global_getter_calls"]["GetDllShareData"])

    def test_comments_and_string_literals_do_not_count_as_code(self) -> None:
        files = {
            "sakura_core/lex.cpp": """
void f() {
    // GetDllShareData(); new Widget; catch (...) { } HWND hwnd;
    const char* ordinary = \"GetEditWnd(); new Widget; catch (...) LPARAM\";
    const char* raw = R\"tag(GetEditDoc(); delete item; catch (...) WPARAM)tag\";
    /* GetDllShareData(); new Widget; catch (...) HWND hwnd; */
}
""",
        }
        temporary, root = self._temporary_repo(files)
        with temporary:
            inventory = collect_semantic_inventory(root)
            self.assertEqual(set(), _finding_paths(inventory))

    def test_line_mapping_is_independent_of_windows_line_endings(self) -> None:
        self.assertEqual(
            {1: 1, 2: 2, 3: 3},
            _unchanged_line_map("first\nsecond\nthird\n", "first\r\nsecond\r\nthird\r\n"),
        )

    def test_scanner_version_input_is_independent_of_checkout_line_endings(self) -> None:
        self.assertEqual(
            b"first\nsecond\nthird\n",
            _normalise_line_endings(b"first\r\nsecond\nthird\r"),
        )

    def test_file_a_minus_one_and_file_b_plus_one_still_fails(self) -> None:
        temporary, root = self._temporary_repo(
            {"sakura_core/a.cpp": "void A() { GetDllShareData(); }\n"}
        )
        with temporary:
            baseline = collect_semantic_inventory(root)
            _write(root, "sakura_core/a.cpp", "void A() {}\n")
            _write(root, "sakura_core/b.cpp", "void B() { GetDllShareData(); }\n")
            _commit(root, "move semantic debt")
            comparison = compare_semantic_inventory(collect_semantic_inventory(root), baseline, repo_root=root)
            self.assertFalse(comparison["ok"])
            self.assertEqual(
                [{"rule_id": "global.get_dll_share_data", "path": "sakura_core/b.cpp", "line": 1, "column": 12, "evidence_sha256": comparison["new_findings"][0]["evidence_sha256"]}],
                comparison["new_findings"],
            )
            self.assertEqual("sakura_core/a.cpp", comparison["removed_findings"][0]["path"])

    def test_new_source_file_with_one_violation_fails(self) -> None:
        temporary, root = self._temporary_repo()
        with temporary:
            baseline = collect_semantic_inventory(root)
            _write(root, "sakura_core/new.cpp", "void NewFile() { GetEditDoc(); }\n")
            _commit(root, "add violating source")
            comparison = compare_semantic_inventory(collect_semantic_inventory(root), baseline, repo_root=root)
            self.assertFalse(comparison["ok"])
            self.assertEqual("sakura_core/new.cpp", comparison["new_findings"][0]["path"])
            self.assertEqual("global.get_edit_doc", comparison["new_findings"][0]["rule_id"])

    def test_new_source_violation_succeeds_after_removal(self) -> None:
        temporary, root = self._temporary_repo()
        with temporary:
            baseline = collect_semantic_inventory(root)
            _write(root, "sakura_core/new.cpp", "void NewFile() { GetEditDoc(); }\n")
            _commit(root, "add violating source")
            self.assertFalse(compare_semantic_inventory(collect_semantic_inventory(root), baseline, repo_root=root)["ok"])

            _write(root, "sakura_core/new.cpp", "void NewFile() {}\n")
            _commit(root, "remove violating source")
            comparison = compare_semantic_inventory(collect_semantic_inventory(root), baseline, repo_root=root)
            self.assertTrue(comparison["ok"])
            self.assertEqual([], comparison["new_findings"])

    def test_renaming_a_file_does_not_erase_existing_debt(self) -> None:
        temporary, root = self._temporary_repo(
            {"sakura_core/a.cpp": "void A() { GetEditWnd(); }\n"}
        )
        with temporary:
            baseline = collect_semantic_inventory(root)
            _git(root, "mv", "sakura_core/a.cpp", "sakura_core/renamed.cpp")
            _commit(root, "rename semantic debt")
            current = collect_semantic_inventory(root)
            comparison = compare_semantic_inventory(current, baseline, repo_root=root)
            self.assertTrue(comparison["ok"])
            self.assertEqual({"sakura_core/a.cpp": "sakura_core/renamed.cpp"}, comparison["renames"])
            self.assertEqual([], comparison["new_findings"])
            self.assertEqual([], comparison["removed_findings"])
            self.assertIn(("global.get_edit_wnd", "sakura_core/renamed.cpp", 1), _finding_paths(current))

    def test_current_only_rule_key_fails_closed(self) -> None:
        temporary, root = self._temporary_repo()
        with temporary:
            baseline = collect_semantic_inventory(root)
            current = copy.deepcopy(baseline)
            current["rule_catalog"] = [*current["rule_catalog"], {"id": "new.current.only", "category": "code", "version": 1}]
            current["rule_catalog_sha256"] = "sha256:current-only-rule"
            with self.assertRaises(BuildError) as raised:
                compare_semantic_inventory(current, baseline, repo_root=root)
            self.assertEqual("SEMANTIC_BASELINE_SCHEMA", raised.exception.code)

    def test_changed_file_with_existing_debt_requires_a_reduction(self) -> None:
        temporary, root = self._temporary_repo(
            {"sakura_core/a.cpp": "void A() { GetDllShareData(); }\n"}
        )
        with temporary:
            baseline = collect_semantic_inventory(root)
            _write(root, "sakura_core/a.cpp", "// keep the pre-existing debt\nvoid A() { GetDllShareData(); }\n")
            _commit(root, "touch without reducing debt")
            comparison = compare_semantic_inventory(collect_semantic_inventory(root), baseline, repo_root=root)
            self.assertFalse(comparison["ok"])
            self.assertEqual([], comparison["new_findings"])
            self.assertEqual(["sakura_core/a.cpp"], [item["path"] for item in comparison["missing_touched_reductions"]])

    def test_accept_current_requires_clean_exact_sha_and_writes_ledger(self) -> None:
        temporary, root = self._temporary_repo()
        with temporary:
            inventory = collect_semantic_inventory(root)
            source_commit = _git(root, "rev-parse", "HEAD")
            baseline_path = root / "tools/build/baselines/editor-core-semantic.json"
            history_directory = root / "tools/build/baselines/editor-core-semantic-history"
            accepted = accept_semantic_inventory(
                root,
                inventory,
                baseline_path,
                history_directory=history_directory,
                source_commit=source_commit,
                accepted_reason="Establish reviewed semantic v2 fixture baseline.",
                tracking_issue=18,
                environment={},
            )
            self.assertTrue(accepted["baseline_changed"])
            self.assertEqual(inventory, json.loads(baseline_path.read_text(encoding="utf-8")))
            record = json.loads(Path(accepted["history_record"]).read_text(encoding="utf-8"))
            self.assertEqual(source_commit, record["source_commit"])
            self.assertEqual(18, record["tracking_issue"])
            self.assertEqual("sha256:absent", record["previous_baseline_sha256"])
            self.assertEqual(inventory["scanner_version"], record["scanner_version"])
            self.assertEqual(inventory["tracked_source_set_sha256"], record["tracked_source_set_sha256"])

    def test_accept_current_rejects_ci_dirty_commit_and_path_set_mismatches(self) -> None:
        temporary, root = self._temporary_repo()
        with temporary:
            inventory = collect_semantic_inventory(root)
            source_commit = _git(root, "rev-parse", "HEAD")
            kwargs = {
                "history_directory": root / "history",
                "source_commit": source_commit,
                "accepted_reason": "Fixture guard test.",
                "tracking_issue": 18,
            }
            with self.assertRaises(BuildError) as raised:
                accept_semantic_inventory(root, inventory, root / "baseline.json", environment={"CI": "true"}, **kwargs)
            self.assertEqual("SEMANTIC_ACCEPT_CI_FORBIDDEN", raised.exception.code)

            with self.assertRaises(BuildError) as raised:
                accept_semantic_inventory(root, inventory, root / "baseline.json", environment={}, source_commit="0" * 40, **{key: value for key, value in kwargs.items() if key != "source_commit"})
            self.assertEqual("SEMANTIC_ACCEPT_COMMIT", raised.exception.code)

            wrong_path_set = copy.deepcopy(inventory)
            wrong_path_set["tracked_source_set_sha256"] = "sha256:not-the-tree"
            with self.assertRaises(BuildError) as raised:
                accept_semantic_inventory(root, wrong_path_set, root / "baseline.json", environment={}, **kwargs)
            self.assertEqual("SEMANTIC_ACCEPT_PATH_SET", raised.exception.code)

            wrong_scope = copy.deepcopy(inventory)
            wrong_scope["scope_definition_sha256"] = "sha256:not-this-scanner"
            with self.assertRaises(BuildError) as raised:
                accept_semantic_inventory(root, wrong_scope, root / "baseline.json", environment={}, **kwargs)
            self.assertEqual("SEMANTIC_ACCEPT_SCOPE", raised.exception.code)

            _write(root, "sakura_core/base.cpp", "void Changed() {}\n")
            with self.assertRaises(BuildError) as raised:
                accept_semantic_inventory(root, inventory, root / "baseline.json", environment={}, **kwargs)
            self.assertEqual("SEMANTIC_ACCEPT_DIRTY", raised.exception.code)

    def test_writer_is_idempotent_and_json_is_stable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "semantic.json"
            inventory = {"schema_version": 2, "inventory_kind": "editor-core-semantic", "metrics": {"x": 1}}
            self.assertTrue(write_semantic_inventory(path, inventory))
            self.assertFalse(write_semantic_inventory(path, inventory))
            self.assertEqual(inventory, json.loads(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
