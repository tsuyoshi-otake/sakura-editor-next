"""Contracts for the age-verified, offline MSYS2 bootstrap (#390)."""

from __future__ import annotations

import copy
import importlib.util
import itertools
import json
import re
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
ACTION = ROOT / ".github/actions/setup-locked-msys2"
SPEC = importlib.util.spec_from_file_location("sakura_locked_msys2", ACTION / "setup.py")
assert SPEC and SPEC.loader
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)
NOW = datetime(2026, 10, 28, 10, 0, 0, tzinfo=timezone.utc)


class LockedMsys2Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.lock = json.loads((ACTION / "packages.lock.json").read_text(encoding="utf-8"))

    def read_candidate(self, data: object) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            return setup.read_lock(path, NOW)

    def test_reviewed_lock_has_aged_evidence_and_exact_unique_packages(self) -> None:
        lock = self.read_candidate(self.lock)
        self.assertEqual(74, len(lock["packages"]))
        self.assertEqual({"mingw": 71, "cppcheck": 10, "doxygen": 1},
                         {name: len(packages) for name, packages in lock["profiles"].items()})
        names = {entry["filename"] for entry in lock["packages"]}
        self.assertIn("mingw-w64-x86_64-cmake-4.4.4-3-any.pkg.tar.zst", names)
        self.assertIn("mingw-w64-x86_64-crt-14.0.0.r495.g63ebe6cdb-1-any.pkg.tar.zst", names)
        self.assertIn("mingw-w64-x86_64-uv-0.13.5-1-any.pkg.tar.zst", names)
        self.assertIn("mingw-w64-x86_64-cppcheck-2.21.0-1-any.pkg.tar.zst", names)
        self.assertIn("doxygen-1.18.0-1-x86_64.pkg.tar.zst", names)

    def test_age_boundary_and_future_evidence_fail_closed(self) -> None:
        setup.require_old_timestamp("2026-10-25T10:00:00Z", NOW)
        for value in ("2026-10-25T10:00:01Z", "2026-10-29T00:00:00Z", "bad", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                setup.require_old_timestamp(value, NOW)

    def test_unreviewed_or_unsafe_lock_fails_before_download(self) -> None:
        mutations = []
        young = copy.deepcopy(self.lock)
        young["packages_observed_at"] = "2026-10-27T10:00:00Z"
        mutations.append(young)
        installer = copy.deepcopy(self.lock)
        installer["installer"]["published_at"] = "2026-10-27T10:00:00Z"
        mutations.append(installer)
        escaped = copy.deepcopy(self.lock)
        escaped["packages"][0]["filename"] = "../escape.pkg.tar.zst"
        mutations.append(escaped)
        duplicate = copy.deepcopy(self.lock)
        duplicate["packages"].append(copy.deepcopy(duplicate["packages"][0]))
        mutations.append(duplicate)
        changed_hash = copy.deepcopy(self.lock)
        changed_hash["packages"][0]["sha256"] = "not-a-sha256"
        mutations.append(changed_hash)
        unobserved = copy.deepcopy(self.lock)
        previous = unobserved["packages"][0]["filename"]
        replacement = "mingw-w64-x86_64-7zip-99.99-1-any.pkg.tar.zst"
        unobserved["packages"][0]["filename"] = replacement
        for profile in unobserved["profiles"].values():
            for index, name in enumerate(profile):
                if name == previous:
                    profile[index] = replacement
        mutations.append(unobserved)
        before_observation = copy.deepcopy(self.lock)
        before_observation["packages_observed_at"] = "2026-10-24T08:43:00Z"
        mutations.append(before_observation)
        unknown_profile_package = copy.deepcopy(self.lock)
        unknown_profile_package["profiles"]["doxygen"].append("unknown.pkg.tar.zst")
        mutations.append(unknown_profile_package)
        duplicate_profile_package = copy.deepcopy(self.lock)
        duplicate_profile_package["profiles"]["doxygen"].append(duplicate_profile_package["profiles"]["doxygen"][0])
        mutations.append(duplicate_profile_package)
        for candidate in mutations:
            with self.subTest(candidate=candidate), self.assertRaises(ValueError):
                self.read_candidate(candidate)

    def test_archive_paths_cannot_escape_the_installer_directory(self) -> None:
        setup.validate_archive_listing("msys64/\nmsys64/usr/bin/bash.exe\n")
        for name in ("../escape", "/absolute", "msys64/../../escape", "C:/escape", "msys64\\escape"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                setup.validate_archive_listing(name)

    def test_hash_mismatch_is_not_retried_or_written(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value.read1.side_effect = [b"changed bytes", b""]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "package"
            with mock.patch.object(setup.urllib.request, "urlopen", return_value=response) as fetch:
                with self.assertRaises(ValueError):
                    setup.download("https://example.invalid/package", destination, "0" * 64)
            self.assertEqual(1, fetch.call_count)
            self.assertFalse(destination.exists())

    def test_historical_observation_cannot_be_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            observation = Path(directory) / "observation.txt"
            original = setup.OBSERVATION.read_bytes().replace(b"\r\n", b"\n")
            observation.write_bytes(original.replace(b"\n", b"\r\n"))
            setup.read_lock(ACTION / "packages.lock.json", NOW, observation)
            observation.write_bytes(original + b"changed")
            with self.assertRaisesRegex(ValueError, "fingerprint"):
                setup.read_lock(ACTION / "packages.lock.json", NOW, observation)

    def test_unknown_profile_stops_before_any_download(self) -> None:
        with mock.patch.dict(setup.os.environ, {"SAKURA_MSYS2_PROFILE": "unknown"}), \
                mock.patch.object(setup, "download") as fetch:
            with self.assertRaisesRegex(ValueError, "unknown MSYS2 profile"):
                setup.main()
        fetch.assert_not_called()

    def test_crt_pin_uses_the_same_verified_bytes_and_offline_config(self) -> None:
        action = (ROOT / ".github/actions/pin-mingw-crt/action.yml").read_text(encoding="utf-8-sig")
        entries = re.findall(r"(?m)^\s*([0-9a-f]{64})  (mingw-w64-x86_64-[^\s]+\.pkg\.tar\.zst)$", action)
        locked = {item["filename"]: item["sha256"] for item in self.lock["packages"]}
        self.assertEqual(4, len(entries))
        for digest, filename in entries:
            self.assertEqual(digest, locked[filename])
        self.assertIn("pacman --upgrade --noconfirm --config /etc/sakura-offline.conf", action)

    def test_all_msys_workflows_use_their_locked_profiles(self) -> None:
        workflows = ROOT / ".github/workflows"
        for workflow in workflows.glob("*.yml"):
            self.assertNotIn("msys2/setup-msys2@", workflow.read_text(encoding="utf-8-sig"), workflow.name)
        for name, profile in (("cppcheck.yml", "cppcheck"), ("doxygen.yml", "doxygen")):
            text = (workflows / name).read_text(encoding="utf-8-sig")
            self.assertIn("uses: ./.github/actions/setup-locked-msys2", text)
            self.assertIn(f"profile: {profile}", text)

    def test_download_state_machine_for_all_three_attempt_sequences(self) -> None:
        # Exhaust the finite response alphabet, including retry, rate-limit,
        # corruption and success in every position; the oracle is independent
        # of the implementation's control flow.
        good = b"verified package"
        digest = setup.hashlib.sha256(good).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "package"
            for states in itertools.product(("ok", "corrupt", "error", "rate"), repeat=3):
                with self.subTest(states=states):
                    destination.unlink(missing_ok=True)
                    outcomes = []
                    for state in states:
                        if state in ("ok", "corrupt"):
                            response = mock.MagicMock()
                            response.__enter__.return_value.read1.side_effect = [good if state == "ok" else b"corrupted", b""]
                            outcomes.append(response)
                        else:
                            headers = {"Retry-After": "10"} if state == "rate" else {}
                            outcomes.append(setup.urllib.error.HTTPError("https://example.invalid", 429 if state == "rate" else 500, "failure", headers, None))
                    terminal = next((index for index, state in enumerate(states) if state in ("ok", "corrupt")), 2)
                    expected = states[terminal]
                    delays = [10.0 if state == "rate" else float(2 ** index)
                              for index, state in enumerate(states[:terminal])]
                    with mock.patch.object(setup.urllib.request, "urlopen", side_effect=outcomes) as fetch, \
                            mock.patch.object(setup.random, "random", return_value=0.0), \
                            mock.patch.object(setup.time, "sleep") as sleep:
                        if expected == "ok":
                            setup.download("https://example.invalid/package", destination, digest)
                        else:
                            with self.assertRaises(ValueError if expected == "corrupt" else RuntimeError):
                                setup.download("https://example.invalid/package", destination, digest)
                    self.assertEqual(terminal + 1, fetch.call_count)
                    self.assertEqual(delays, [call.args[0] for call in sleep.call_args_list])
                    self.assertEqual(expected == "ok", destination.exists())
                    if expected == "ok":
                        self.assertEqual(good, destination.read_bytes())

    def test_bad_or_long_retry_after_never_causes_an_early_retry(self) -> None:
        for header in ("invalid", "NaN", "31"):
            with self.subTest(header=header), tempfile.TemporaryDirectory() as directory:
                error = setup.urllib.error.HTTPError("https://example.invalid", 429, "rate", {"Retry-After": header}, None)
                with mock.patch.object(setup.urllib.request, "urlopen", side_effect=error) as fetch, \
                        mock.patch.object(setup.time, "sleep") as sleep:
                    with self.assertRaises((ValueError, RuntimeError)):
                        setup.download("https://example.invalid/package", Path(directory) / "package")
                self.assertEqual(1, fetch.call_count)
                sleep.assert_not_called()

    def test_oversized_download_never_writes_a_partial_file(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value.read1.side_effect = [b"12345", b""]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "package"
            with mock.patch.object(setup, "MAX_DOWNLOAD_BYTES", 4), \
                    mock.patch.object(setup.urllib.request, "urlopen", return_value=response):
                with self.assertRaisesRegex(ValueError, "exceeds"):
                    setup.download("https://example.invalid/package", destination)
            self.assertFalse(destination.exists())

    def test_slow_download_stops_after_its_deadline(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value.read1.return_value = b"x"
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "package"
            with mock.patch.object(setup.urllib.request, "urlopen", return_value=response), \
                    mock.patch.object(setup.time, "monotonic", side_effect=[0, 181] * 3), \
                    mock.patch.object(setup.time, "sleep"):
                with self.assertRaisesRegex(RuntimeError, "could not download"):
                    setup.download("https://example.invalid/package", destination)
            self.assertFalse(destination.exists())

    def test_offline_install_requires_signatures_and_rejects_young_signatures(self) -> None:
        self.assertIn("TRUST_(FULLY|ULTIMATE)", setup.BOOTSTRAP)
        self.assertIn("now - stamp >= 259200", setup.BOOTSTRAP)
        self.assertIn("pacman --config /etc/sakura-offline.conf", setup.BOOTSTRAP)
        self.assertNotIn("pacman -S", setup.BOOTSTRAP)


if __name__ == "__main__":
    unittest.main()
