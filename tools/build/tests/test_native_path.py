from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.runner import (  # noqa: E402
    NATIVE_ALIAS_OWNER_ENVIRONMENT,
    NATIVE_PATH_IDENTITY_FILE,
    BuildError,
    cleanup_native_aliases_for_owner,
    native_execution_root,
    new_native_alias_owner,
    write_native_path_identity,
)
from sakura_build_lib import runner  # noqa: E402


@contextmanager
def isolated_alias_root(testcase: unittest.TestCase):
    """Place aliases under a private ASCII directory instead of the shared one.

    The real candidate is a per-user directory such as ``AppData/.snp`` that
    other runs share, so asserting on its contents would test the machine.
    """
    with tempfile.TemporaryDirectory(prefix="sakura-native-base-") as directory:
        base_root = Path(directory).resolve()
        if not runner._is_ascii_path(base_root):
            testcase.skipTest(f"temporary directory is not ASCII: {base_root}")
        with mock.patch.object(runner, "_native_alias_candidates", return_value=(base_root,)):
            yield base_root / ".snp"


def make_junction(link: Path, target: Path) -> None:
    subprocess.run(
        [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "mklink", "/J", str(link), str(target)],
        check=True,
        capture_output=True,
    )


def remove_links(base: Path) -> None:
    """Remove junctions left by a test without following them."""
    if not base.is_dir():
        return
    for entry in base.iterdir():
        if runner._is_junction(entry):
            os.rmdir(entry)


class NativeExecutionRootTests(unittest.TestCase):
    def test_ascii_root_is_used_directly(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-ascii-") as directory:
            root = Path(directory).resolve()
            with native_execution_root(root) as execution_root:
                self.assertEqual(root, execution_root)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_non_ascii_root_uses_and_cleans_ascii_junction(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-junction-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            marker = root / "marker.txt"
            marker.write_text("same file", encoding="utf-8")

            with isolated_alias_root(self) as base:
                with native_execution_root(root) as execution_root:
                    alias = execution_root
                    self.assertEqual(base, alias.parent)
                    str(alias).encode("ascii")
                    self.assertNotEqual(root.resolve(), alias)
                    self.assertEqual(root.resolve(), alias.resolve())
                    self.assertEqual("same file", (alias / "marker.txt").read_text(encoding="utf-8"))
                    build_dir = alias / "build" / "probe"
                    write_native_path_identity(build_dir, root, alias)
                    recorded = json.loads((build_dir / NATIVE_PATH_IDENTITY_FILE).read_text(encoding="utf-8"))

                self.assertFalse(os.path.lexists(alias))
                self.assertFalse(base.exists())
                with native_execution_root(root, preferred_alias=Path(recorded["execution_root"])) as recreated:
                    self.assertEqual(alias, recreated)
                    self.assertEqual(root.resolve(), recreated.resolve())
                self.assertFalse(os.path.lexists(alias))

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_non_ascii_alias_is_cleaned_when_the_child_scope_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-failure-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            alias: Path | None = None

            with isolated_alias_root(self) as base:
                with self.assertRaisesRegex(RuntimeError, "synthetic child failure"):
                    with native_execution_root(root) as execution_root:
                        alias = execution_root
                        raise RuntimeError("synthetic child failure")

                self.assertIsNotNone(alias)
                self.assertFalse(os.path.lexists(alias))
                self.assertFalse(base.exists())

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_dead_aliases_are_reclaimed_and_everything_else_is_kept(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-reclaim-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            gone = Path(directory) / "deleted-checkout"
            elsewhere = Path(directory) / "live-checkout"
            gone.mkdir()
            elsewhere.mkdir()

            with isolated_alias_root(self) as base:
                base.mkdir()
                try:
                    # The shape a killed creator leaves behind: its checkout was
                    # deleted afterwards, so the junction can never work again.
                    dead = base / "9b34-20258945"
                    make_junction(dead, gone)
                    live = base / "0123abcd-89abcdef"
                    make_junction(live, elsewhere)
                    foreign_name = base / "not-an-alias"
                    make_junction(foreign_name, gone)
                    plain = base / "4567-0123abcd"
                    plain.mkdir()
                    gone.rmdir()

                    with native_execution_root(root) as alias:
                        self.assertFalse(os.path.lexists(dead))
                        for kept in (live, foreign_name, plain):
                            self.assertTrue(os.path.lexists(kept), kept)

                    self.assertFalse(os.path.lexists(alias))
                    self.assertEqual(
                        {live.name, foreign_name.name, plain.name},
                        {entry.name for entry in base.iterdir()},
                    )
                    self.assertEqual(elsewhere.resolve(), live.resolve())
                finally:
                    remove_links(base)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_a_recorded_alias_in_use_is_not_taken_over(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-in-use-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)

            with isolated_alias_root(self) as base:
                base.mkdir()
                in_use = base / "0123abcd-89abcdef"
                make_junction(in_use, root.resolve())
                try:
                    with self.assertRaises(BuildError) as raised:
                        with native_execution_root(root, preferred_alias=in_use):
                            self.fail("an alias another process holds must not be reused")
                    self.assertEqual("NATIVE_PATH_ALIAS_UNAVAILABLE", raised.exception.code)
                    self.assertTrue(in_use.is_dir())
                    self.assertEqual(root.resolve(), in_use.resolve())
                finally:
                    remove_links(base)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_a_failed_mklink_reports_a_readable_message(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-mklink-") as directory:
            missing_parent = Path(directory) / "missing" / "alias"
            with self.assertRaises(OSError) as raised:
                runner._create_junction(missing_parent, Path(directory))
            message = str(raised.exception)
            self.assertTrue(message.startswith("mklink exit 1: "), message)
            # cmd writes its built-ins' messages in the console code page
            # unless asked for UTF-16; decoding that as UTF-8 yields U+FFFD.
            self.assertNotIn("�", message)
            self.assertGreater(len(message), len("mklink exit 1: "))

    def test_a_wedged_mklink_is_an_error_instead_of_a_wait(self) -> None:
        wedged = subprocess.TimeoutExpired(["mklink"], runner._NATIVE_ALIAS_MKLINK_TIMEOUT_SECONDS)
        with mock.patch.object(runner.subprocess, "run", side_effect=wedged) as run:
            with self.assertRaisesRegex(OSError, "mklink did not finish within"):
                runner._create_junction(Path("link"), Path("target"))
        self.assertEqual(runner._NATIVE_ALIAS_MKLINK_TIMEOUT_SECONDS, run.call_args.kwargs["timeout"])


def in_thread(work):
    """Run ``work`` on its own thread; the alias lock is owned per thread."""
    finished = threading.Event()
    outcome: list[object] = []

    def run() -> None:
        try:
            outcome.append(work())
        except BaseException as error:  # handed to the asserting thread
            outcome.append(error)
        finally:
            finished.set()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return finished, outcome


@unittest.skipUnless(os.name == "nt", "the alias parent lock is a Windows named mutex")
class NativeAliasParentLockTests(unittest.TestCase):
    """Every change to a shared ``.snp`` parent's existence holds one lock.

    Without it, the last process leaving removes the parent between another
    creator's mkdir and mklink; a stress run of six parallel creator processes
    lost 3 to 6 of 240 cycles that way.  Mocked interleavings could not show
    it, so these tests use real threads and a real process on the lock.
    """

    def test_a_creator_waits_for_the_holder_before_creating_the_parent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-lock-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)

            def create_and_leave() -> Path:
                with native_execution_root(root) as alias:
                    return alias

            with isolated_alias_root(self) as base:
                with runner._native_alias_parent_lock(base):
                    finished, outcome = in_thread(create_and_leave)
                    self.assertFalse(finished.wait(0.5), outcome)
                    self.assertFalse(base.exists())
                self.assertTrue(finished.wait(30))
                (alias,) = outcome
                self.assertIsInstance(alias, Path)
                self.assertEqual(base, alias.parent)
                self.assertFalse(os.path.lexists(alias))
                self.assertFalse(base.exists())

    def test_a_leaver_waits_for_the_holder_before_removing_the_parent(self) -> None:
        with isolated_alias_root(self) as base:
            base.mkdir()
            with runner._native_alias_parent_lock(base):
                finished, outcome = in_thread(lambda: runner._remove_empty_native_alias_parent(base))
                self.assertFalse(finished.wait(0.5), outcome)
                self.assertTrue(base.is_dir())
            self.assertTrue(finished.wait(30))
            self.assertEqual([None], outcome)
            self.assertFalse(base.exists())

    def test_a_creator_fails_closed_when_the_lock_stays_held(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-lock-timeout-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)

            def create() -> None:
                with native_execution_root(root):
                    pass

            with isolated_alias_root(self) as base:
                with mock.patch.object(runner, "_NATIVE_ALIAS_LOCK_TIMEOUT_MILLISECONDS", 200):
                    with runner._native_alias_parent_lock(base):
                        finished, outcome = in_thread(create)
                        self.assertTrue(finished.wait(30))
                (error,) = outcome
                self.assertIsInstance(error, BuildError)
                self.assertEqual("NATIVE_PATH_ALIAS_UNAVAILABLE", error.code)
                self.assertIn("timed out after 200 ms", str(error))
                self.assertFalse(base.exists())

    def test_a_lock_whose_holder_died_is_taken_over(self) -> None:
        with isolated_alias_root(self) as base:
            holder = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    "import os, sys\n"
                    "sys.path.insert(0, sys.argv[1])\n"
                    "from pathlib import Path\n"
                    "from sakura_build_lib import runner\n"
                    # Keep a reference: collecting the context manager would
                    # close its generator and release the lock right away.
                    "held = runner._native_alias_parent_lock(Path(sys.argv[2]))\n"
                    "held.__enter__()\n"
                    "print('held', flush=True)\n"
                    "sys.stdin.readline()\n"
                    "os._exit(0)\n",
                    str(TOOLS_BUILD),
                    str(base),
                ],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            try:
                self.assertEqual("held", holder.stdout.readline().strip())

                def lock_and_report() -> str:
                    with runner._native_alias_parent_lock(base):
                        return "entered"

                finished, outcome = in_thread(lock_and_report)
                self.assertFalse(finished.wait(0.5), outcome)
                # The holder exits without releasing, which abandons the mutex
                # to the waiter instead of leaving it waiting for the bound.
                holder.stdin.write("\n")
                holder.stdin.flush()
                self.assertEqual(0, holder.wait(30))
                self.assertTrue(finished.wait(30))
                self.assertEqual(["entered"], outcome)
            finally:
                if holder.poll() is None:
                    holder.kill()
                    holder.wait(30)
                holder.stdin.close()
                holder.stdout.close()


class NativeAliasCandidateTests(unittest.TestCase):
    def test_the_shortest_ascii_ancestor_outside_home_comes_first(self) -> None:
        home = Path("C:/Users/someone")
        temporary = home / "AppData" / "Local" / "Temp"
        repo = temporary / "work" / "日本語" / "repo"
        with mock.patch.object(Path, "home", return_value=home), mock.patch.object(
            tempfile, "gettempdir", return_value=str(temporary)
        ):
            candidates = runner._native_alias_candidates(repo)

        # Home and its ancestors are never used, so aliases of a checkout under
        # TEMP land in the shared, per-user AppData directory first.
        self.assertEqual(
            (home / "AppData", home / "AppData" / "Local", temporary, temporary / "work"),
            candidates,
        )


class NativeAliasOwnerTests(unittest.TestCase):
    def test_cleanup_rejects_a_malformed_owner(self) -> None:
        for owner in ("", "12345678-", "ABCDEF12", "1234567", "123456789", "*"):
            with self.subTest(owner=owner):
                with self.assertRaises(BuildError) as raised:
                    cleanup_native_aliases_for_owner(Path.cwd(), owner)
                self.assertEqual("NATIVE_PATH_ALIAS_OWNER_INVALID", raised.exception.code)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_inherited_owner_names_the_alias_and_selects_it_for_cleanup(self) -> None:
        owner = new_native_alias_owner()
        with tempfile.TemporaryDirectory(prefix="sakura-native-owner-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            with isolated_alias_root(self):
                with mock.patch.dict(os.environ, {NATIVE_ALIAS_OWNER_ENVIRONMENT: owner}):
                    scope = native_execution_root(root)
                    alias = scope.__enter__()
                try:
                    self.assertTrue(alias.name.startswith(f"{owner}-"))
                    self.assertEqual((), cleanup_native_aliases_for_owner(root, new_native_alias_owner()))
                    self.assertTrue(alias.exists())
                    self.assertEqual((str(alias),), cleanup_native_aliases_for_owner(root, owner))
                    self.assertFalse(os.path.lexists(alias))
                finally:
                    # The owner already removed the junction, which is the
                    # outcome the scope's own cleanup exists to reach.
                    scope.__exit__(None, None, None)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_cleanup_removes_an_owned_alias_whose_checkout_is_gone(self) -> None:
        owner = new_native_alias_owner()
        with tempfile.TemporaryDirectory(prefix="sakura-native-owner-dead-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            gone = Path(directory) / "deleted-checkout"
            gone.mkdir()
            with isolated_alias_root(self) as base:
                base.mkdir()
                dead = base / f"{owner}-0123abcd"
                make_junction(dead, gone)
                gone.rmdir()
                try:
                    self.assertEqual((str(dead),), cleanup_native_aliases_for_owner(root, owner))
                    self.assertFalse(os.path.lexists(dead))
                finally:
                    remove_links(base)

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_an_unreadable_parent_is_a_cleanup_failure(self) -> None:
        owner = new_native_alias_owner()
        with tempfile.TemporaryDirectory(prefix="sakura-native-owner-denied-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            with isolated_alias_root(self) as base:
                base.mkdir()
                iterdir = Path.iterdir

                def denied(path: Path):
                    if path == base:
                        raise PermissionError(13, "Access is denied", str(path))
                    return iterdir(path)

                with mock.patch.object(Path, "iterdir", denied):
                    with self.assertRaises(BuildError) as raised:
                        cleanup_native_aliases_for_owner(root, owner)
                # path_matrix maps this code to a cleanup_failed child; a raw
                # OSError would instead abort the whole matrix run.
                self.assertEqual("NATIVE_PATH_ALIAS_CLEANUP_FAILED", raised.exception.code)
                self.assertIn(str(base), str(raised.exception))

    @unittest.skipUnless(os.name == "nt", "directory junction fallback is Windows-specific")
    def test_malformed_inherited_owner_fails_before_creating_an_alias(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sakura-native-bad-owner-") as directory:
            root = Path(directory) / "日本語" / "repo"
            root.mkdir(parents=True)
            with isolated_alias_root(self) as base:
                with mock.patch.dict(os.environ, {NATIVE_ALIAS_OWNER_ENVIRONMENT: "not-hex!"}):
                    with self.assertRaises(BuildError) as raised:
                        with native_execution_root(root):
                            self.fail("a malformed owner must not yield an execution root")
                self.assertEqual("NATIVE_PATH_ALIAS_OWNER_INVALID", raised.exception.code)
                self.assertFalse(base.exists())


if __name__ == "__main__":
    unittest.main()
