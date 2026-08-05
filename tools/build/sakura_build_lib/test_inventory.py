"""Stable test inventory import and comparison.

The inventory freezes a stable test ID separately from the runtime runner and
GoogleTest selector.  A future runner split can therefore keep the ID while
changing only its runtime mapping.
"""

from __future__ import annotations

import hashlib
import json
import locale
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Mapping


INVENTORY_SCHEMA_VERSION = 1


class TestInventoryError(ValueError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def guarantee_fingerprint(tests: list[Mapping[str, Any]]) -> str:
    guarantees = [
        {
            "test_id": item["test_id"],
            "status": item["status"],
        }
        for item in sorted(tests, key=lambda value: value["test_id"])
    ]
    return "sha256:" + hashlib.sha256(_canonical_json(guarantees).encode("utf-8")).hexdigest()


def _without_comment(line: str) -> str:
    return line.split("#", 1)[0].rstrip()


def _is_disabled(suite: str, test: str) -> bool:
    return any(part.startswith("DISABLED_") for part in (suite + test).replace(".", "/").split("/"))


def parse_gtest_list(output: str, runner_id: str) -> list[dict[str, Any]]:
    if not runner_id or any(character.isspace() for character in runner_id):
        raise TestInventoryError("TEST_RUNNER_ID_INVALID", "runner ID must be non-empty and contain no whitespace")

    tests: list[dict[str, Any]] = []
    current_suite: str | None = None
    selectors: set[str] = set()
    for number, raw_line in enumerate(output.splitlines(), start=1):
        if not raw_line.strip():
            continue
        if not raw_line[0].isspace():
            candidate = _without_comment(raw_line).strip()
            if candidate.startswith("Running main() from "):
                continue
            if not candidate.endswith("."):
                raise TestInventoryError("TEST_DISCOVERY_FORMAT", f"line {number}: expected a suite ending in '.'")
            current_suite = candidate
            continue

        if current_suite is None:
            raise TestInventoryError("TEST_DISCOVERY_FORMAT", f"line {number}: test appeared before its suite")
        test_name = _without_comment(raw_line).strip()
        if not test_name:
            raise TestInventoryError("TEST_DISCOVERY_FORMAT", f"line {number}: empty test name")
        selector = current_suite + test_name
        if selector in selectors:
            raise TestInventoryError("TEST_SELECTOR_DUPLICATE", f"duplicate runtime selector: {selector}")
        selectors.add(selector)
        tests.append(
            {
                "test_id": f"tests1:{selector}",
                "runtime": {"runner_id": runner_id, "selector": selector},
                "status": "disabled" if _is_disabled(current_suite, test_name) else "enabled",
            }
        )

    if not tests:
        raise TestInventoryError("TEST_DISCOVERY_EMPTY", "GoogleTest discovery returned zero tests")
    return sorted(tests, key=lambda item: item["test_id"])


def collect_gtest_inventory(
    executable: Path,
    runner_id: str,
    repo_root: Path,
    source_revision: str,
    source_dirty: bool,
    timeout_seconds: int,
) -> dict[str, Any]:
    if timeout_seconds <= 0:
        raise TestInventoryError("TEST_TIMEOUT_INVALID", "timeout must be greater than zero")
    executable = executable.resolve()
    if not executable.is_file():
        raise TestInventoryError("TEST_EXECUTABLE_MISSING", f"test executable does not exist: {executable}")
    try:
        completed = subprocess.run(
            [str(executable), "--gtest_list_tests"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            encoding=locale.getpreferredencoding(False),
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise TestInventoryError("TEST_DISCOVERY_TIMEOUT", f"test discovery exceeded {timeout_seconds} seconds") from error
    except OSError as error:
        raise TestInventoryError("TEST_DISCOVERY_START", f"could not start test discovery: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "no diagnostic output"
        raise TestInventoryError("TEST_DISCOVERY_FAILED", f"test discovery exited {completed.returncode}: {detail}")

    tests = parse_gtest_list(completed.stdout, runner_id)
    executable_hash = hashlib.sha256()
    with executable.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            executable_hash.update(chunk)
    try:
        executable_path = executable.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        executable_path = str(executable)
    return {
        "schema_version": INVENTORY_SCHEMA_VERSION,
        "inventory_id": "legacy-tests1-msvc-x64-debug",
        "source_revision": source_revision,
        "source_dirty": source_dirty,
        "discovery": {
            "framework": "googletest",
            "executable": executable_path,
            "arguments": ["--gtest_list_tests"],
            "executable_sha256": executable_hash.hexdigest(),
        },
        "test_count": len(tests),
        "disabled_count": sum(item["status"] == "disabled" for item in tests),
        "guarantee_fingerprint": guarantee_fingerprint(tests),
        "tests": tests,
    }


def verify_runtime_mappings(
    inventory: Mapping[str, Any],
    runners: Mapping[str, Path],
    repo_root: Path,
    timeout_seconds: int,
) -> dict[str, Any]:
    """Verify that a split inventory is exactly discoverable from its runners."""
    validated = validate_inventory(inventory)
    expected: dict[str, set[str]] = {}
    for item in validated["tests"]:
        runtime = item["runtime"]
        expected.setdefault(runtime["runner_id"], set()).add(runtime["selector"])
    missing_runners = sorted(set(expected) - set(runners))
    extra_runners = sorted(set(runners) - set(expected))
    if missing_runners or extra_runners:
        raise TestInventoryError(
            "TEST_RUNNER_SET",
            f"runner set mismatch: missing={missing_runners}, extra={extra_runners}",
        )

    reports: list[dict[str, Any]] = []
    failures: list[str] = []
    for runner_id in sorted(expected):
        discovered = collect_gtest_inventory(
            runners[runner_id], runner_id, repo_root, "runtime-verification", True, timeout_seconds
        )
        actual = {item["runtime"]["selector"] for item in discovered["tests"]}
        missing = sorted(expected[runner_id] - actual)
        unexpected = sorted(actual - expected[runner_id])
        if missing:
            failures.append(f"{runner_id} missing {len(missing)} selectors")
        if unexpected:
            failures.append(f"{runner_id} exposed {len(unexpected)} untracked selectors")
        reports.append({
            "runner_id": runner_id,
            "expected_count": len(expected[runner_id]),
            "observed_count": len(actual),
            "missing_selectors": missing,
            "unexpected_selectors": unexpected,
        })
    return {
        "ok": not failures,
        "test_count": validated["test_count"],
        "guarantee_fingerprint": validated["guarantee_fingerprint"],
        "runners": reports,
        "failures": failures,
    }


def refresh_runtime_mappings(
    inventory: Mapping[str, Any],
    runners: Mapping[str, Path],
    remaps: Mapping[str, tuple[str, str]],
    repo_root: Path,
    source_revision: str,
    source_dirty: bool,
    timeout_seconds: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Refresh discovery while preserving stable IDs and rejecting silent loss.

    Existing runtime selectors keep their stable IDs. A disappeared selector is
    an error unless the caller explicitly maps its stable ID to a discovered
    runner/selector pair. Newly discovered selectors extend the guarantee set.
    """
    validated = validate_inventory(inventory)
    discovered_by_runtime: dict[tuple[str, str], dict[str, Any]] = {}
    runner_discovery: list[dict[str, Any]] = []
    for runner_id in sorted(runners):
        discovered = collect_gtest_inventory(
            runners[runner_id], runner_id, repo_root, source_revision, source_dirty, timeout_seconds
        )
        runner_discovery.append({
            "runner_id": runner_id,
            "executable": discovered["discovery"]["executable"],
            "executable_sha256": discovered["discovery"]["executable_sha256"],
        })
        for item in discovered["tests"]:
            runtime = item["runtime"]
            key = (runtime["runner_id"], runtime["selector"])
            if key in discovered_by_runtime:
                raise TestInventoryError("TEST_RUNTIME_DUPLICATE", f"duplicate runtime selector across discovery: {key}")
            discovered_by_runtime[key] = item

    known_ids = {item["test_id"] for item in validated["tests"]}
    unknown_remaps = sorted(set(remaps) - known_ids)
    if unknown_remaps:
        raise TestInventoryError("TEST_REMAP_UNKNOWN", f"remap references unknown stable IDs: {unknown_remaps}")

    refreshed: list[dict[str, Any]] = []
    claimed: set[tuple[str, str]] = set()
    applied_remaps: list[dict[str, str]] = []
    missing: list[str] = []
    for old in validated["tests"]:
        old_runtime = old["runtime"]
        old_key = (old_runtime["runner_id"], old_runtime["selector"])
        target_key = old_key if old_key in discovered_by_runtime else remaps.get(old["test_id"])
        if target_key is None or target_key not in discovered_by_runtime:
            missing.append(old["test_id"])
            continue
        if target_key in claimed:
            raise TestInventoryError("TEST_REMAP_COLLISION", f"multiple stable IDs map to runtime selector: {target_key}")
        current = discovered_by_runtime[target_key]
        if current["status"] != old["status"]:
            raise TestInventoryError(
                "TEST_STATUS_CHANGED",
                f"{old['test_id']} changed status from {old['status']} to {current['status']}",
            )
        claimed.add(target_key)
        refreshed.append({
            "test_id": old["test_id"],
            "runtime": dict(current["runtime"]),
            "status": old["status"],
        })
        if target_key != old_key:
            applied_remaps.append({
                "test_id": old["test_id"],
                "from": f"{old_key[0]}::{old_key[1]}",
                "to": f"{target_key[0]}::{target_key[1]}",
            })
    if missing:
        raise TestInventoryError(
            "TEST_RUNTIME_MISSING",
            f"{len(missing)} stable tests disappeared without explicit remap: {missing}",
        )

    additions: list[str] = []
    for key, current in sorted(discovered_by_runtime.items()):
        if key in claimed:
            continue
        # New component runners must own the stable ID namespace as well.  The
        # legacy tests1 prefix remains unchanged for existing entries, while a
        # newly discovered selector from (for example) sakura_security_tests
        # must not be silently attributed to tests1.
        test_id = f"{current['runtime']['runner_id']}:{current['runtime']['selector']}"
        if test_id in known_ids or any(item["test_id"] == test_id for item in refreshed):
            raise TestInventoryError("TEST_ID_COLLISION", f"new runtime selector collides with stable ID: {test_id}")
        refreshed.append({
            "test_id": test_id,
            "runtime": dict(current["runtime"]),
            "status": current["status"],
        })
        additions.append(test_id)

    refreshed.sort(key=lambda item: item["test_id"])
    discovery_payload = _canonical_json(runner_discovery)
    result = dict(validated)
    result.update({
        "source_revision": source_revision,
        "source_dirty": source_dirty,
        "discovery": {
            "framework": "googletest",
            "executable": "runtime-set:" + ",".join(sorted(runners)),
            "arguments": ["--gtest_list_tests"],
            "executable_sha256": hashlib.sha256(discovery_payload.encode("utf-8")).hexdigest(),
        },
        "test_count": len(refreshed),
        "disabled_count": sum(item["status"] == "disabled" for item in refreshed),
        "guarantee_fingerprint": guarantee_fingerprint(refreshed),
        "tests": refreshed,
    })
    return validate_inventory(result), {
        "previous_test_count": validated["test_count"],
        "test_count": len(refreshed),
        "additions": additions,
        "runtime_remaps": applied_remaps,
        "runners": runner_discovery,
    }


def validate_inventory(value: Any, location: str = "inventory") -> dict[str, Any]:
    if not isinstance(value, dict):
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}: expected object")
    expected = {
        "schema_version",
        "inventory_id",
        "source_revision",
        "source_dirty",
        "discovery",
        "test_count",
        "disabled_count",
        "guarantee_fingerprint",
        "tests",
    }
    unknown = sorted(set(value) - expected)
    missing = sorted(expected - set(value))
    if unknown or missing:
        raise TestInventoryError("TEST_INVENTORY_FIELDS", f"{location}: unknown={unknown}, missing={missing}")
    if value["schema_version"] != INVENTORY_SCHEMA_VERSION:
        raise TestInventoryError("TEST_INVENTORY_SCHEMA", f"{location}: unsupported schema version")
    if not isinstance(value["inventory_id"], str) or not value["inventory_id"]:
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.inventory_id: expected non-empty string")
    if not isinstance(value["source_revision"], str) or not value["source_revision"]:
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.source_revision: expected non-empty string")
    if not isinstance(value["source_dirty"], bool):
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.source_dirty: expected boolean")
    if not isinstance(value["discovery"], dict):
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.discovery: expected object")
    if set(value["discovery"]) != {"framework", "executable", "arguments", "executable_sha256"}:
        raise TestInventoryError("TEST_INVENTORY_FIELDS", f"{location}.discovery: fields must be framework/executable/arguments/executable_sha256")
    if value["discovery"]["framework"] != "googletest":
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.discovery.framework: expected googletest")
    if not isinstance(value["discovery"]["executable"], str) or not value["discovery"]["executable"]:
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.discovery.executable: expected non-empty string")
    if value["discovery"]["arguments"] != ["--gtest_list_tests"]:
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.discovery.arguments: unsupported discovery arguments")
    executable_sha256 = value["discovery"]["executable_sha256"]
    if not isinstance(executable_sha256, str) or len(executable_sha256) != 64 or any(character not in "0123456789abcdef" for character in executable_sha256):
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.discovery.executable_sha256: expected lowercase SHA-256")
    if not isinstance(value["tests"], list):
        raise TestInventoryError("TEST_INVENTORY_TYPE", f"{location}.tests: expected array")

    seen_ids: set[str] = set()
    selectors: set[tuple[str, str]] = set()
    normalized: list[dict[str, Any]] = []
    for index, item in enumerate(value["tests"]):
        item_location = f"{location}.tests[{index}]"
        if not isinstance(item, dict) or set(item) != {"test_id", "runtime", "status"}:
            raise TestInventoryError("TEST_INVENTORY_FIELDS", f"{item_location}: fields must be test_id/runtime/status")
        test_id = item["test_id"]
        runtime = item["runtime"]
        status = item["status"]
        if not isinstance(test_id, str) or not test_id:
            raise TestInventoryError("TEST_INVENTORY_TYPE", f"{item_location}.test_id: expected non-empty string")
        if test_id in seen_ids:
            raise TestInventoryError("TEST_ID_DUPLICATE", f"{item_location}: duplicate test ID {test_id}")
        seen_ids.add(test_id)
        if not isinstance(runtime, dict) or set(runtime) != {"runner_id", "selector"}:
            raise TestInventoryError("TEST_INVENTORY_FIELDS", f"{item_location}.runtime: fields must be runner_id/selector")
        if not all(isinstance(runtime.get(field), str) and runtime[field] for field in ("runner_id", "selector")):
            raise TestInventoryError("TEST_INVENTORY_TYPE", f"{item_location}.runtime: values must be non-empty strings")
        runtime_key = (runtime["runner_id"], runtime["selector"])
        if runtime_key in selectors:
            raise TestInventoryError("TEST_SELECTOR_DUPLICATE", f"{item_location}: duplicate runtime mapping {runtime_key}")
        selectors.add(runtime_key)
        if status not in {"enabled", "disabled"}:
            raise TestInventoryError("TEST_INVENTORY_TYPE", f"{item_location}.status: expected enabled or disabled")
        normalized.append({"test_id": test_id, "runtime": dict(runtime), "status": status})

    normalized.sort(key=lambda item: item["test_id"])
    if not isinstance(value["test_count"], int) or isinstance(value["test_count"], bool) or value["test_count"] != len(normalized):
        raise TestInventoryError("TEST_INVENTORY_COUNT", f"{location}: test_count does not match tests")
    disabled_count = sum(item["status"] == "disabled" for item in normalized)
    if not isinstance(value["disabled_count"], int) or isinstance(value["disabled_count"], bool) or value["disabled_count"] != disabled_count:
        raise TestInventoryError("TEST_INVENTORY_COUNT", f"{location}: disabled_count does not match tests")
    fingerprint = guarantee_fingerprint(normalized)
    if value["guarantee_fingerprint"] != fingerprint:
        raise TestInventoryError("TEST_INVENTORY_FINGERPRINT", f"{location}: guarantee fingerprint mismatch")
    return dict(value, tests=normalized)


def load_inventory(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise TestInventoryError("TEST_INVENTORY_MISSING", f"inventory does not exist: {path}") from error
    except json.JSONDecodeError as error:
        raise TestInventoryError("TEST_INVENTORY_JSON", f"invalid inventory JSON at {path}: {error}") from error
    return validate_inventory(value, str(path))


def compare_inventories(before: Mapping[str, Any], after: Mapping[str, Any]) -> dict[str, Any]:
    before_by_id = {item["test_id"]: item for item in before["tests"]}
    after_by_id = {item["test_id"]: item for item in after["tests"]}
    missing = sorted(set(before_by_id) - set(after_by_id))
    added = sorted(set(after_by_id) - set(before_by_id))
    status_changes = [
        {"test_id": test_id, "before": before_by_id[test_id]["status"], "after": after_by_id[test_id]["status"]}
        for test_id in sorted(set(before_by_id) & set(after_by_id))
        if before_by_id[test_id]["status"] != after_by_id[test_id]["status"]
    ]
    runtime_remaps = [
        {
            "test_id": test_id,
            "before": before_by_id[test_id]["runtime"],
            "after": after_by_id[test_id]["runtime"],
        }
        for test_id in sorted(set(before_by_id) & set(after_by_id))
        if before_by_id[test_id]["runtime"] != after_by_id[test_id]["runtime"]
    ]
    return {
        "ok": not missing and not added and not status_changes,
        "before_count": len(before_by_id),
        "after_count": len(after_by_id),
        "missing_test_ids": missing,
        "added_test_ids": added,
        "status_changes": status_changes,
        "runtime_remaps": runtime_remaps,
    }


def write_inventory(path: Path, value: Mapping[str, Any]) -> bool:
    metadata = {key: value[key] for key in sorted(value) if key != "tests"}
    lines = ["{"]
    metadata_items = list(metadata.items())
    for key, item in metadata_items:
        rendered = json.dumps(item, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        lines.append(f"  {json.dumps(key)}: {rendered},")
    lines.append('  "tests": [')
    tests = value["tests"]
    for index, item in enumerate(tests):
        suffix = "," if index + 1 < len(tests) else ""
        lines.append("    " + _canonical_json(item) + suffix)
    lines.extend(("  ]", "}"))
    text = "\n".join(lines) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return False
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True
