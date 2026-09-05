"""Fail-closed pull-request CI planning.

The first narrowing phase deliberately recognizes only documentation-only
pull requests.  Every other change class retains the full native build.  This
keeps the decision monotonic while later component-aware plans are developed
from the semantic dependency graph.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Mapping


CI_PLAN_SCHEMA_VERSION = 1
_SHA_PATTERN = re.compile(r"^[0-9a-fA-F]{40,64}$")
_VALID_STATUS_PREFIXES = frozenset("ACDMRTU")
_DOC_SUFFIXES = frozenset({".md", ".markdown", ".rst", ".adoc"})
_DOC_BASENAMES = frozenset({"readme", "changelog", "changes", "license", "notice"})


class CiPlanError(ValueError):
    """A typed planner input or Git-diff failure."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class CiChangedFile:
    path: str
    status: str = "M"


def _require_sha(value: str | None, label: str) -> str:
    candidate = (value or "").strip()
    if not _SHA_PATTERN.fullmatch(candidate):
        raise CiPlanError("CI_PLAN_SHA_INVALID", f"{label} must be a 40-64 digit hexadecimal Git object ID")
    return candidate.lower()


def _normalise_path(value: object) -> str:
    if not isinstance(value, (str, Path)):
        raise CiPlanError("CI_PLAN_PATH_INVALID", "changed-file path must be a string")
    raw = str(value).replace("\\", "/").strip()
    path = PurePosixPath(raw)
    if not raw or path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise CiPlanError("CI_PLAN_PATH_INVALID", f"changed-file path must be repository-relative: {value}")
    return path.as_posix()


def _normalise_change(value: CiChangedFile | Mapping[str, object] | str | Path) -> CiChangedFile:
    if isinstance(value, CiChangedFile):
        path_value, status_value = value.path, value.status
    elif isinstance(value, Mapping):
        path_value, status_value = value.get("path"), value.get("status", "M")
    else:
        path_value, status_value = value, "M"
    status = str(status_value).strip().upper() or "M"
    if status[0] not in _VALID_STATUS_PREFIXES:
        raise CiPlanError("CI_PLAN_STATUS_INVALID", f"unsupported changed-file status: {status}")
    return CiChangedFile(_normalise_path(path_value), status)


def _is_documentation(change: CiChangedFile) -> bool:
    # A deletion, rename/copy, type change, or unresolved merge has wider
    # effects than its destination suffix can prove.  Keep those full.
    if change.status[0] not in {"A", "M"}:
        return False
    path = PurePosixPath(change.path)
    lower_path = change.path.lower()
    if path.suffix.lower() in {".tla", ".cfg"}:
        return False
    if lower_path.startswith("docs/"):
        return True
    if path.suffix.lower() in _DOC_SUFFIXES:
        return True
    return path.stem.lower() in _DOC_BASENAMES and path.suffix.lower() in {"", ".txt"}


def plan_ci(
    changed_files: Iterable[CiChangedFile | Mapping[str, object] | str | Path],
    *,
    event_name: str,
    repository: str = "",
    head_repository: str = "",
    base_sha: str | None = None,
    head_sha: str | None = None,
) -> dict[str, object]:
    """Return one explicit terminal plan for the supplied event and changes."""

    normalized_event = event_name.strip().lower()
    normalized_changes = tuple(_normalise_change(value) for value in changed_files)
    normalized_base = base_sha.lower() if base_sha else None
    normalized_head = head_sha.lower() if head_sha else None

    if normalized_event != "pull_request":
        mode = "full_native"
        reasons = ["non_pull_request_event"]
    else:
        normalized_base = _require_sha(base_sha, "base SHA")
        normalized_head = _require_sha(head_sha, "head SHA")
        if not repository or not head_repository or repository.casefold() != head_repository.casefold():
            mode = "full_native"
            reasons = ["untrusted_or_unknown_head_repository"]
        elif not normalized_changes:
            mode = "full_native"
            reasons = ["no_changed_files"]
        elif all(_is_documentation(change) for change in normalized_changes):
            mode = "docs_only"
            reasons = ["documentation_only"]
        else:
            mode = "full_native"
            reasons = ["native_or_unknown_change"]

    run_native = mode == "full_native"
    return {
        "ok": True,
        "schema_version": CI_PLAN_SCHEMA_VERSION,
        "event_name": normalized_event,
        "base_sha": normalized_base,
        "head_sha": normalized_head,
        "mode": mode,
        "reason_codes": reasons,
        "changed_files": [
            {"path": change.path, "status": change.status} for change in normalized_changes
        ],
        "jobs": {
            "check_encoding": True,
            "native_build": run_native,
        },
    }


def changed_files_between(repo: Path, base_sha: str, head_sha: str) -> tuple[CiChangedFile, ...]:
    """Read a rename-aware, NUL-delimited Git diff without shell parsing."""

    base = _require_sha(base_sha, "base SHA")
    head = _require_sha(head_sha, "head SHA")
    command = ("git", "diff", "--name-status", "-z", "--find-renames", f"{base}...{head}", "--")
    try:
        completed = subprocess.run(command, cwd=repo, check=False, capture_output=True)
    except OSError as error:
        raise CiPlanError("CI_PLAN_GIT_UNAVAILABLE", f"could not execute git diff: {error}") from error
    if completed.returncode != 0:
        detail = os.fsdecode(completed.stderr).strip() or f"git diff exited with {completed.returncode}"
        raise CiPlanError("CI_PLAN_DIFF_FAILED", detail)

    tokens = completed.stdout.split(b"\0")
    if tokens and tokens[-1] == b"":
        tokens.pop()
    changes: list[CiChangedFile] = []
    index = 0
    while index < len(tokens):
        status = os.fsdecode(tokens[index]).strip().upper()
        index += 1
        if not status or status[0] not in _VALID_STATUS_PREFIXES:
            raise CiPlanError("CI_PLAN_DIFF_FORMAT", f"git diff emitted an invalid status: {status!r}")
        path_count = 2 if status[0] in {"C", "R"} else 1
        if index + path_count > len(tokens):
            raise CiPlanError("CI_PLAN_DIFF_FORMAT", f"git diff omitted a path for status {status}")
        paths = [os.fsdecode(token) for token in tokens[index : index + path_count]]
        index += path_count
        # Rename/copy destinations are enough for evidence; the status itself
        # forces the plan to full_native.
        changes.append(_normalise_change(CiChangedFile(paths[-1], status)))
    return tuple(changes)


def write_ci_plan(path: Path, value: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as stream:
        stream.write(payload)
        temporary = Path(stream.name)
    temporary.replace(path)
