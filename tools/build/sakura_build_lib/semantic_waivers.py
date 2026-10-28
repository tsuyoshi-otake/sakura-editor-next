"""Reviewed, short-lived exceptions for semantic-debt ratchet findings.

This policy runs only after the ordinary semantic scanner and comparison have
produced their raw evidence. A waiver is bound to exact finding identities,
the baseline commit, and newline-normalised hashes of every target file.
"""

from __future__ import annotations

import hashlib
import json
import re
from datetime import date, datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Mapping

from .runner import BuildError


SCHEMA_VERSION = 1
MAX_WAIVER_DAYS = 90
_SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_COMMIT_RE = re.compile(r"^[0-9a-f]{40,64}$")

_LEDGER_KEYS = {"schema_version", "waivers"}
_WAIVER_KEYS = {
    "owner",
    "reason",
    "tracking_issue",
    "created",
    "expires",
    "baseline_source_commit",
    "files",
    "new_findings",
    "missing_touched_reductions",
}
_FINDING_KEYS = {"rule_id", "path", "line", "column", "evidence_sha256"}
_REDUCTION_KEYS = {"path", "baseline_finding_count"}


def semantic_waiver_ledger_path(repo_root: Path, baseline_path: Path) -> Path:
    """Keep alternative baselines independent of the repository's default waiver."""
    default_baseline = repo_root / "tools/build/baselines/editor-core-semantic.json"
    if baseline_path.resolve() == default_baseline.resolve():
        return repo_root / "tools/build/baselines/semantic-waivers.json"
    return baseline_path.with_name(f"{baseline_path.stem}-waivers.json")


def _fail(code: str, detail: str) -> None:
    raise BuildError(code, detail, 2)


def _object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            _fail("SEMANTIC_WAIVER_DUPLICATE_KEY", f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_ledger(path: Path) -> Mapping[str, object]:
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return {"schema_version": SCHEMA_VERSION, "waivers": []}
    except (OSError, UnicodeError) as error:
        _fail("SEMANTIC_WAIVER_READ", f"could not read waiver ledger {path}: {error}")
    try:
        value = json.loads(raw, object_pairs_hook=_object_without_duplicate_keys)
    except (json.JSONDecodeError, UnicodeError) as error:
        _fail("SEMANTIC_WAIVER_INVALID", f"could not parse waiver ledger {path}: {error}")
    if not isinstance(value, dict):
        _fail("SEMANTIC_WAIVER_INVALID", "waiver ledger root must be an object")
    return value


def _require_keys(value: Mapping[str, object], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} keys differ; missing={missing}, extra={extra}")


def _nonempty_string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} must be a non-empty string")
    return value


def _positive_integer(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} must be a positive integer")
    return value


def _iso_date(value: object, label: str) -> date:
    if not isinstance(value, str):
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} must be an ISO date")
    try:
        parsed = date.fromisoformat(value)
    except ValueError:
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} must be an ISO date")
    if parsed.isoformat() != value:
        _fail("SEMANTIC_WAIVER_INVALID", f"{label} must use YYYY-MM-DD")
    return parsed


def _repo_file(root: Path, raw_path: object) -> tuple[str, Path]:
    path = _nonempty_string(raw_path, "waiver target path")
    if "\\" in path or "\x00" in path:
        _fail("SEMANTIC_WAIVER_PATH", f"waiver target must use a repository-relative POSIX path: {path!r}")
    relative = PurePosixPath(path)
    if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts) or ":" in path:
        _fail("SEMANTIC_WAIVER_PATH", f"waiver target escapes or is not relative: {path!r}")
    canonical = relative.as_posix()
    if canonical != path:
        _fail("SEMANTIC_WAIVER_PATH", f"waiver target is not canonical: {path!r}")
    repository = root.resolve()
    candidate = repository.joinpath(*relative.parts)
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        _fail("SEMANTIC_WAIVER_PATH", f"waiver target does not exist: {path}: {error}")
    if not resolved.is_relative_to(repository) or not resolved.is_file():
        _fail("SEMANTIC_WAIVER_PATH", f"waiver target escapes repository or is not a file: {path!r}")
    return canonical, resolved


def _normalised_file_hash(path: Path, relative: str) -> str:
    try:
        content = path.read_bytes()
    except OSError as error:
        _fail("SEMANTIC_WAIVER_PATH", f"could not read waiver target {relative}: {error}")
    content = content.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return "sha256:" + hashlib.sha256(content).hexdigest()


def _finding_key(value: Mapping[str, object], label: str) -> tuple[str, str, int, int, str]:
    _require_keys(value, _FINDING_KEYS, label)
    rule = _nonempty_string(value["rule_id"], f"{label}.rule_id")
    path = _nonempty_string(value["path"], f"{label}.path")
    line = _positive_integer(value["line"], f"{label}.line")
    column = _positive_integer(value["column"], f"{label}.column")
    evidence = value["evidence_sha256"]
    if not isinstance(evidence, str) or _SHA256_RE.fullmatch(evidence) is None:
        _fail("SEMANTIC_WAIVER_INVALID", f"{label}.evidence_sha256 must be a lowercase SHA-256")
    return rule, path, line, column, evidence


def _reduction_key(value: Mapping[str, object], label: str) -> tuple[str, int]:
    _require_keys(value, _REDUCTION_KEYS, label)
    path = _nonempty_string(value["path"], f"{label}.path")
    count = _positive_integer(value["baseline_finding_count"], f"{label}.baseline_finding_count")
    return path, count


def _validated_waivers(
    ledger: Mapping[str, object],
    *,
    repo_root: Path,
    baseline_source_commit: object,
    today: date,
) -> tuple[list[tuple[dict[str, object], list[tuple[str, str, int, int, str]], list[tuple[str, int]]]], list[dict[str, object]]]:
    _require_keys(ledger, _LEDGER_KEYS, "waiver ledger")
    if type(ledger["schema_version"]) is not int or ledger["schema_version"] != SCHEMA_VERSION:
        _fail("SEMANTIC_WAIVER_SCHEMA", f"unsupported waiver schema version: {ledger['schema_version']!r}")
    records = ledger["waivers"]
    if not isinstance(records, list):
        _fail("SEMANTIC_WAIVER_INVALID", "waivers must be an array")

    baseline_commit = baseline_source_commit
    if not isinstance(baseline_commit, str) or _COMMIT_RE.fullmatch(baseline_commit) is None:
        _fail("SEMANTIC_WAIVER_BASELINE", "raw comparison has no valid baseline source commit")

    validated = []
    fingerprints: list[dict[str, object]] = []
    seen_findings: set[tuple[str, str, int, int, str]] = set()
    seen_reductions: set[tuple[str, int]] = set()
    seen_records: set[str] = set()
    for index, raw in enumerate(records):
        label = f"waivers[{index}]"
        if not isinstance(raw, dict):
            _fail("SEMANTIC_WAIVER_INVALID", f"{label} must be an object")
        _require_keys(raw, _WAIVER_KEYS, label)
        _nonempty_string(raw["owner"], f"{label}.owner")
        _nonempty_string(raw["reason"], f"{label}.reason")
        _positive_integer(raw["tracking_issue"], f"{label}.tracking_issue")
        created = _iso_date(raw["created"], f"{label}.created")
        expires = _iso_date(raw["expires"], f"{label}.expires")
        if created > today:
            _fail("SEMANTIC_WAIVER_FUTURE", f"{label} was created in the future")
        if expires < today:
            _fail("SEMANTIC_WAIVER_EXPIRED", f"{label} expired on {expires.isoformat()}")
        if expires < created or (expires - created).days > MAX_WAIVER_DAYS:
            _fail("SEMANTIC_WAIVER_EXPIRY", f"{label} must expire within {MAX_WAIVER_DAYS} days of creation")
        record_commit = raw["baseline_source_commit"]
        if not isinstance(record_commit, str) or _COMMIT_RE.fullmatch(record_commit) is None:
            _fail("SEMANTIC_WAIVER_INVALID", f"{label}.baseline_source_commit must be a full commit ID")
        if record_commit != baseline_commit:
            _fail("SEMANTIC_WAIVER_BASELINE", f"{label} targets baseline {record_commit}, current baseline is {baseline_commit}")

        raw_files = raw["files"]
        if not isinstance(raw_files, dict) or not raw_files:
            _fail("SEMANTIC_WAIVER_INVALID", f"{label}.files must be a non-empty path-to-hash object")
        file_hashes: dict[str, str] = {}
        for raw_path, expected_hash in raw_files.items():
            path, resolved = _repo_file(repo_root, raw_path)
            if not isinstance(expected_hash, str) or _SHA256_RE.fullmatch(expected_hash) is None:
                _fail("SEMANTIC_WAIVER_INVALID", f"{label}.files[{path!r}] must be a lowercase SHA-256")
            actual_hash = _normalised_file_hash(resolved, path)
            if actual_hash != expected_hash:
                _fail("SEMANTIC_WAIVER_FINGERPRINT", f"{label} target fingerprint changed: {path}")
            file_hashes[path] = expected_hash
            fingerprints.append({"record": index, "path": path, "sha256": expected_hash})

        raw_findings = raw["new_findings"]
        raw_reductions = raw["missing_touched_reductions"]
        if not isinstance(raw_findings, list) or not isinstance(raw_reductions, list) or not raw_findings and not raw_reductions:
            _fail("SEMANTIC_WAIVER_INVALID", f"{label} must target at least one finding or reduction")
        finding_keys = []
        for item_index, item in enumerate(raw_findings):
            if not isinstance(item, dict):
                _fail("SEMANTIC_WAIVER_INVALID", f"{label}.new_findings[{item_index}] must be an object")
            key = _finding_key(item, f"{label}.new_findings[{item_index}]")
            if key[1] not in file_hashes:
                _fail("SEMANTIC_WAIVER_INVALID", f"{label} has no file fingerprint for finding path {key[1]!r}")
            if key in seen_findings:
                _fail("SEMANTIC_WAIVER_DUPLICATE", f"finding is waived more than once: {key}")
            seen_findings.add(key)
            finding_keys.append(key)
        reduction_keys = []
        for item_index, item in enumerate(raw_reductions):
            if not isinstance(item, dict):
                _fail("SEMANTIC_WAIVER_INVALID", f"{label}.missing_touched_reductions[{item_index}] must be an object")
            key = _reduction_key(item, f"{label}.missing_touched_reductions[{item_index}]")
            if key[0] not in file_hashes:
                _fail("SEMANTIC_WAIVER_INVALID", f"{label} has no file fingerprint for reduction path {key[0]!r}")
            if key in seen_reductions:
                _fail("SEMANTIC_WAIVER_DUPLICATE", f"reduction is waived more than once: {key}")
            seen_reductions.add(key)
            reduction_keys.append(key)
        target_paths = {key[1] for key in finding_keys} | {key[0] for key in reduction_keys}
        if target_paths != set(file_hashes):
            _fail("SEMANTIC_WAIVER_INVALID", f"{label}.files must fingerprint exactly its target paths")

        record_hash = hashlib.sha256(json.dumps(raw, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
        if record_hash in seen_records:
            _fail("SEMANTIC_WAIVER_DUPLICATE", f"duplicate waiver record at index {index}")
        seen_records.add(record_hash)
        validated.append((raw, finding_keys, reduction_keys))
    return validated, fingerprints


def apply_semantic_waivers(
    comparison: Mapping[str, object],
    *,
    repo_root: Path,
    ledger_path: Path,
    today: date | None = None,
) -> dict[str, object]:
    """Return raw comparison plus reviewed waiver decisions and effective status.

    The comparison's ``ok``, ``new_findings``, and
    ``missing_touched_reductions`` fields are retained unchanged as raw evidence.
    Invalid or stale ledger data always fails closed, even for non-strict output.
    """

    for key in ("ok", "new_findings", "missing_touched_reductions", "baseline_source_commit"):
        if key not in comparison:
            _fail("SEMANTIC_WAIVER_COMPARISON", f"raw semantic comparison is missing {key}")
    if not isinstance(comparison["ok"], bool):
        _fail("SEMANTIC_WAIVER_COMPARISON", "raw semantic comparison ok must be a boolean")
    raw_findings = comparison["new_findings"]
    raw_reductions = comparison["missing_touched_reductions"]
    if not isinstance(raw_findings, list) or not isinstance(raw_reductions, list):
        _fail("SEMANTIC_WAIVER_COMPARISON", "raw semantic comparison findings/reductions must be arrays")
    if comparison["ok"] != (not raw_findings and not raw_reductions):
        _fail("SEMANTIC_WAIVER_COMPARISON", "raw semantic comparison ok does not match its findings and reductions")

    now = today or datetime.now(timezone.utc).date()
    ledger = _read_ledger(ledger_path)
    waivers, fingerprints = _validated_waivers(
        ledger,
        repo_root=repo_root,
        baseline_source_commit=comparison["baseline_source_commit"],
        today=now,
    )
    raw_finding_keys = {
        _finding_key(item, f"raw new_findings[{index}]")
        for index, item in enumerate(raw_findings)
        if isinstance(item, dict)
    }
    if len(raw_finding_keys) != len(raw_findings):
        _fail("SEMANTIC_WAIVER_COMPARISON", "raw semantic findings contain invalid or duplicate entries")
    raw_reduction_keys = {
        _reduction_key(item, f"raw missing_touched_reductions[{index}]")
        for index, item in enumerate(raw_reductions)
        if isinstance(item, dict)
    }
    if len(raw_reduction_keys) != len(raw_reductions):
        _fail("SEMANTIC_WAIVER_COMPARISON", "raw semantic reductions contain invalid or duplicate entries")

    waived_findings: set[tuple[str, str, int, int, str]] = set()
    waived_reductions: set[tuple[str, int]] = set()
    for index, (_record, findings, reductions) in enumerate(waivers):
        for key in findings:
            if key not in raw_finding_keys:
                _fail("SEMANTIC_WAIVER_STALE_TARGET", f"waivers[{index}] does not exactly match a raw new finding: {key}")
            waived_findings.add(key)
        for key in reductions:
            if key not in raw_reduction_keys:
                _fail("SEMANTIC_WAIVER_STALE_TARGET", f"waivers[{index}] does not exactly match a raw missing reduction: {key}")
            waived_reductions.add(key)

    def finding_dict(key: tuple[str, str, int, int, str]) -> dict[str, object]:
        return dict(zip(("rule_id", "path", "line", "column", "evidence_sha256"), key))

    def reduction_dict(key: tuple[str, int]) -> dict[str, object]:
        return {"path": key[0], "baseline_finding_count": key[1]}

    unwaived_findings = raw_finding_keys - waived_findings
    unwaived_reductions = raw_reduction_keys - waived_reductions
    effective_ok = not unwaived_findings and not unwaived_reductions
    result = dict(comparison)
    result["raw_ok"] = comparison["ok"]
    result["effective_ok"] = effective_ok
    result["waiver_review"] = {
        "ledger": str(ledger_path),
        "waived": {
            "new_findings": [finding_dict(key) for key in sorted(waived_findings)],
            "missing_touched_reductions": [reduction_dict(key) for key in sorted(waived_reductions)],
        },
        "unwaived": {
            "new_findings": [finding_dict(key) for key in sorted(unwaived_findings)],
            "missing_touched_reductions": [reduction_dict(key) for key in sorted(unwaived_reductions)],
        },
        "file_fingerprints": fingerprints,
        "effective_ok": effective_ok,
    }
    return result
