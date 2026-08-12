"""Contracts for the pinned vcpkg tool cache (#142).

``Bootstrap vcpkg`` runs ahead of every later step, so a failure there discards
the whole job before the package closure cache is even consulted.  Measured on
2026-08-12 during a github.com delivery degradation: the closure cache had just
been saved (77,332,767 bytes at 21:29:08Z), a rerun of ``main`` started at
21:36:09Z, and it died at 21:37:59Z in ``Bootstrap vcpkg`` -- two seconds in,
with the closure it needed already sitting in the cache it never reached.

The tool does not need to come from the network at all.  ``tools/vcpkg`` is a
pinned submodule and ``scripts/vcpkg-tool-metadata.txt`` names one release tag
that ``bootstrap.ps1`` turns into one download URL, so a given commit always
fetches the same bytes.

Each contract below is pinned because breaking it fails silently rather than
loudly:

* If the restore and save paths or keys drift apart, every run is a miss and
  nothing turns red.
* If the key stops carrying the release tag, a submodule bump can serve the
  previous tool from the cache forever.
* If the skip path stops validating the binary it found, a truncated or stale
  entry poisons every later step instead of falling back to a real bootstrap.
* If the skip path stops writing ``vcpkg.disable-metrics``, skipping the
  bootstrap silently re-enables the telemetry the bootstrap call opts out of.
* If the save step stops excluding pull requests, each pull-request run adds an
  entry that no other run may read, against the shared 10 GiB quota.
* If the retry horizon shrinks back to seconds, it stops covering the outage it
  was measured against (#138).

Like ``test_cppcheck_analyzer_cache.py``, this lives in ``src/test/py`` because
the CTest ``pytest`` target runs from the repository root under pytest's
default ``norecursedirs``, which never collects ``tools/build/tests``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
ACTION = REPO_ROOT / ".github/actions/bootstrap-vcpkg/action.yml"
TOOL_METADATA = REPO_ROOT / "tools/vcpkg/scripts/vcpkg-tool-metadata.txt"
WORKFLOWS = REPO_ROOT / ".github/workflows"

# The 2026-08-12 degradation recovered about two and a quarter minutes after the
# last of three attempts spent inside twenty-five seconds.  See #138.
MEASURED_RECOVERY_SECONDS = 135


def _action_text() -> str:
    return ACTION.read_text(encoding="utf-8")


class VcpkgToolCacheContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _action_text()

    def test_the_restore_and_save_agree_on_one_path_and_one_key(self) -> None:
        paths = set(re.findall(r"^\s*path:\s*(.+)$", self.text, re.MULTILINE))
        keys = set(re.findall(r"^\s*key:\s*(.+)$", self.text, re.MULTILINE))
        self.assertEqual(1, len(paths), f"restore and save must cache one path, got {paths}")
        self.assertEqual(1, len(keys), f"restore and save must share one key, got {keys}")
        self.assertIn("vcpkg.exe", paths.pop())

    def test_the_key_is_derived_from_the_pinned_release_tag(self) -> None:
        # The tag is what fixes the download URL, so it is what must invalidate
        # the entry.  Read it out of the checkout to prove the name still exists.
        self.assertIn("VCPKG_TOOL_RELEASE_TAG", TOOL_METADATA.read_text(encoding="ascii"))
        self.assertIn("$config.VCPKG_TOOL_RELEASE_TAG", self.text)
        self.assertRegex(self.text, r'"cache-key=vcpkg-tool-\$env:RUNNER_OS-\$architecture-\$tag-\$digest"')

    def test_the_cache_is_restored_on_every_event_and_saved_only_on_a_trusted_push(self) -> None:
        restore = re.search(r"- name: Restore cached vcpkg tool\n(?:.*\n)*?\s*uses: (\S+)", self.text)
        self.assertIsNotNone(restore)
        self.assertEqual("actions/cache/restore@v4", restore.group(1))

        save = re.search(
            r"- name: Save cached vcpkg tool\n\s*if: (.+)\n\s*uses: (\S+)",
            self.text,
        )
        self.assertIsNotNone(save)
        condition, uses = save.group(1), save.group(2)
        self.assertEqual("actions/cache/save@v4", uses)
        self.assertIn("github.event_name != 'pull_request'", condition)
        self.assertIn("steps.restore.outputs.cache-hit != 'true'", condition)

        # The restore itself must carry no event condition; a pull request that
        # cannot read the default branch's entry gains nothing from the cache.
        self.assertNotRegex(
            self.text,
            r"- name: Restore cached vcpkg tool\n\s*id: restore\n\s*if:",
        )

    def test_a_restored_binary_is_validated_before_it_is_trusted(self) -> None:
        self.assertIn("& '.\\vcpkg.exe' version --disable-metrics", self.text)
        self.assertIn("[regex]::Escape($expected)", self.text)
        # A binary that does not answer, or answers with another release, must
        # reach the bootstrap rather than being used.
        self.assertIn("bootstrapping instead.", self.text)

    def test_skipping_the_bootstrap_still_opts_out_of_metrics(self) -> None:
        self.assertIn("Set-Content -Value '' -Path '.\\vcpkg.disable-metrics' -Force", self.text)

    def test_the_retry_horizon_still_covers_the_measured_outage(self) -> None:
        backoff = re.search(r"\$backoff = @\(([^)]+)\)", self.text)
        self.assertIsNotNone(backoff)
        waits = [int(value.strip()) for value in backoff.group(1).split(",")]

        attempts = re.search(r"default: \"(\d+)\"", self.text)
        self.assertIsNotNone(attempts)
        allowed = int(attempts.group(1))

        self.assertGreaterEqual(len(waits), allowed - 1, "every retry needs its own backoff entry")
        self.assertGreaterEqual(sum(waits[: allowed - 1]), MEASURED_RECOVERY_SECONDS)

    def test_every_pwsh_bootstrap_site_calls_this_action(self) -> None:
        # build-on-msys2.yml is the deliberate exception: its msys2 default
        # shell needs the .sh entry point, so it cannot call a pwsh action.
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text(encoding="utf-8")
            with self.subTest(workflow=workflow.name):
                self.assertNotIn(
                    "bootstrap-vcpkg.bat",
                    text,
                    "bootstrap the Windows tool through .github/actions/bootstrap-vcpkg",
                )


if __name__ == "__main__":
    unittest.main()
