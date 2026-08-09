"""Hermetic R0/R1 inventory and ratchet for Editor Core semantic debt.

The v2 inventory deliberately observes only tracked first-party inputs.  It is
not an AST verifier, but it is lexical enough to avoid treating comments and
ordinary string literals as C++ code.  Baselines contain individual findings so
that debt cannot be moved between files and hidden by a global total.
"""

from __future__ import annotations

import bisect
import hashlib
import json
import os
import re
import subprocess
import tempfile
from collections import Counter, defaultdict
from dataclasses import dataclass
from difflib import SequenceMatcher
from pathlib import Path
from typing import Iterable, Mapping, Sequence

from .runner import BuildError


SCHEMA_VERSION = 2
INVENTORY_KIND = "editor-core-semantic"
HISTORY_KIND = "editor-core-semantic-baseline-acceptance"

SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})
CONFIG_SUFFIXES = frozenset({".cmake", ".filters", ".props", ".targets", ".vcxproj", ".yml", ".yaml"})
CONFIG_FILENAMES = frozenset({"CMakeLists.txt"})
SOURCE_ROOTS = ("sakura_core", "sakura_lang", "src/main", "src/test", "tools")
CONFIG_ROOTS = (".github", "sakura_core", "sakura_lang", "src/main", "src/test", "tools")
EXCLUDED_PREFIXES = (
    "build/",
    "externals/",
    "mingw/",
    "src/main/modules/generated/",
    "tools/build/baselines/",
    "tools/vcpkg/",
    "tools/vcpkg-local-registry/",
    "win32/",
    "x64/",
)
REGULAR_GIT_MODES = frozenset({"100644", "100755"})

GETTER_NAMES = (
    ("GetDllShareData", "global.get_dll_share_data"),
    ("GetEditWnd", "global.get_edit_wnd"),
    ("GetEditDoc", "global.get_edit_doc"),
)

RULE_CATALOG = (
    ("global.get_dll_share_data", "code", 1),
    ("global.get_edit_wnd", "code", 1),
    ("global.get_edit_doc", "code", 1),
    ("memory.raw_new", "code", 1),
    ("memory.raw_delete", "code", 1),
    ("error.catch_all", "code", 1),
    ("boundary.win32_type", "code", 1),
    ("state.public_mutable_field", "code", 1),
    ("state.legacy_selection_lock_direct_access", "code", 1),
    ("state.legacy_selection_range_direct_access", "code", 1),
    ("state.mutable_member", "code", 1),
    ("state.raw_pointer_member", "code", 1),
    ("test.publicization_macro", "code", 1),
    ("resource.stop_required_acquisition", "code", 1),
    ("boundary.private_include", "code", 1),
    ("test.monolith_link", "config", 1),
    ("test.filtered_or_skipped", "config", 1),
)

_GETTER_PATTERNS = {
    rule_id: re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}\s*\(")
    for name, rule_id in GETTER_NAMES
}
_CODE_PATTERNS = (
    ("memory.raw_new", re.compile(r"\bnew(?:\s*\[\s*\])?\s+[A-Za-z_][A-Za-z0-9_:<>]*(?:\s*[\*&])?")),
    ("memory.raw_delete", re.compile(r"\bdelete(?:\s*\[\s*\])?\s*[A-Za-z_][A-Za-z0-9_]*")),
    ("error.catch_all", re.compile(r"\bcatch\s*\(\s*\.\.\.\s*\)")),
    ("boundary.win32_type", re.compile(r"\b(?:HWND|WPARAM|LPARAM)\b")),
    ("state.legacy_selection_lock_direct_access", re.compile(r"\bm_bSelectingLock\b")),
    ("state.mutable_member", re.compile(r"\bmutable\b")),
    ("test.publicization_macro", re.compile(r"(?m)^\s*#\s*define\s+(?:private|protected)\s+public\b")),
    (
        "resource.stop_required_acquisition",
        re.compile(r"\b(?:std::(?:j)?thread|CreateThread|CreateProcess(?:W|A)?|SetTimer|Subscribe)\s*(?:<[^>]*>)?\s*\("),
    ),
)
_LEGACY_SELECTION_RANGE_DIRECT_ACCESS_RE = re.compile(
    r"\bm_sSelect(?:Bgn|Old)?\b"
    r"|(?<!const\s)\bCLayoutRange\s*&\s*GetSelect\s*\("
    r"|\bGetSelect\s*\(\s*\)\s*(?:=|\.Set(?:From|To|ToX)?\s*\(|\.Clear\s*\(|\.Get(?:From|To)Pointer\s*\()"
)
_PRIVATE_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]*/(?:platform|window|view|doc)/[^>\"]+)[>\"]")
_MONOLITH_LINK_RE = re.compile(r"(?:CollectSakuraObjectsForTests1|SakuraLinkInputsForTests1|sakura_core/.*\.obj)")
_FILTER_RE = re.compile(r"(?:gtest_filter|--filter|\bskip(?:ped|ping)?\b|exclude(?:d|ing)?)", re.IGNORECASE)
_RAW_LITERAL_START_RE = re.compile(r"(?:u8|u|U|L)?R\"(?P<delimiter>[^\s()\\\"\r\n]{0,16})\(")
_CLASS_OPEN_RE = re.compile(r"\b(?P<kind>class|struct)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s+final)?[^;{]*\{")
_ACCESS_RE = re.compile(r"^\s*(public|private|protected)\s*:")
_PUBLIC_MUTABLE_FIELD_RE = re.compile(
    r"^\s*(?!(?:static|const|constexpr|using|typedef|friend|class|struct|enum|template)\b)"
    r"(?P<declaration>[^;(){}]+?)\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^;]*)?;\s*$"
)
_RAW_POINTER_MEMBER_RE = re.compile(
    r"^\s*(?!(?:static|const|constexpr)\b)[^;(){}]*\*\s*(?P<name>m_[A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^;]*)?;\s*$"
)


@dataclass(frozen=True)
class TrackedFile:
    """One regular, tracked first-party input and the rule categories it serves."""

    relative: str
    path: Path
    categories: tuple[str, ...]


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _sha256(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _scope_definition() -> dict[str, object]:
    return {
        "source_roots": list(SOURCE_ROOTS),
        "config_roots": list(CONFIG_ROOTS),
        "source_suffixes": sorted(SOURCE_SUFFIXES),
        "config_suffixes": sorted(CONFIG_SUFFIXES),
        "config_filenames": sorted(CONFIG_FILENAMES),
        "excluded_prefixes": list(EXCLUDED_PREFIXES),
        "regular_git_modes": sorted(REGULAR_GIT_MODES),
    }


def _rule_catalog_definition() -> list[dict[str, object]]:
    return [
        {"id": rule_id, "category": category, "version": version}
        for rule_id, category, version in RULE_CATALOG
    ]


def _normalise_line_endings(data: bytes) -> bytes:
    """Canonicalise checkout-specific line endings without changing source content."""

    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def _scanner_version() -> str:
    """Make scanner implementation changes fail closed against an old baseline."""

    return _sha256(_normalise_line_endings(Path(__file__).read_bytes()))


def _decode_source(data: bytes) -> str:
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    return data.decode(encoding, errors="replace")


def _read_source(path: Path, relative: str) -> tuple[bytes, str]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BuildError("SEMANTIC_TRACKED_FILE_MISSING", f"could not read tracked input {relative}: {error}", 2) from error
    return data, _decode_source(data)


def _git(root: Path, arguments: Sequence[str], code: str, *, input_data: bytes | None = None) -> bytes:
    try:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=root,
            input=input_data,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        raise BuildError(code, f"could not execute git {' '.join(arguments)}: {error}", 2) from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise BuildError(code, f"git {' '.join(arguments)} failed: {detail or completed.returncode}", 2)
    return completed.stdout


def _git_text(root: Path, arguments: Sequence[str], code: str) -> str:
    return _git(root, arguments, code).decode("utf-8", errors="replace").strip()


def _normalise_git_path(raw: bytes) -> str:
    value = raw.decode("utf-8", errors="surrogateescape").replace("\\", "/")
    if not value or value.startswith("/") or value.startswith("../") or "/../" in value:
        raise BuildError("SEMANTIC_GIT_PATH", f"unsupported tracked path: {value!r}", 2)
    return value


def _index_entries(root: Path) -> tuple[tuple[str, str], ...]:
    output = _git(root, ("ls-files", "--stage", "-z"), "SEMANTIC_GIT_INDEX")
    result: list[tuple[str, str]] = []
    for record in output.split(b"\0"):
        if not record:
            continue
        metadata, separator, raw_path = record.partition(b"\t")
        if not separator:
            raise BuildError("SEMANTIC_GIT_INDEX", "git ls-files --stage produced an invalid record", 2)
        pieces = metadata.split(b" ")
        if len(pieces) != 3:
            raise BuildError("SEMANTIC_GIT_INDEX", "git ls-files --stage metadata was invalid", 2)
        mode, _object_id, stage = pieces
        if stage != b"0":
            continue
        result.append((mode.decode("ascii"), _normalise_git_path(raw_path)))
    return tuple(sorted(result, key=lambda item: item[1]))


def _tree_entries(root: Path, commit: str) -> tuple[tuple[str, str], ...]:
    output = _git(root, ("ls-tree", "-r", "-z", commit), "SEMANTIC_SOURCE_COMMIT")
    result: list[tuple[str, str]] = []
    for record in output.split(b"\0"):
        if not record:
            continue
        metadata, separator, raw_path = record.partition(b"\t")
        if not separator:
            raise BuildError("SEMANTIC_SOURCE_COMMIT", "git ls-tree produced an invalid record", 2)
        pieces = metadata.split(b" ")
        if len(pieces) != 3:
            raise BuildError("SEMANTIC_SOURCE_COMMIT", "git ls-tree metadata was invalid", 2)
        mode, _kind, _object_id = pieces
        result.append((mode.decode("ascii"), _normalise_git_path(raw_path)))
    return tuple(sorted(result, key=lambda item: item[1]))


def _is_below(relative: str, root: str) -> bool:
    normalized = root.rstrip("/")
    return relative == normalized or relative.startswith(normalized + "/")


def _is_excluded(relative: str) -> bool:
    return any(relative.startswith(prefix) for prefix in EXCLUDED_PREFIXES)


def _categories_for_path(relative: str) -> tuple[str, ...]:
    if _is_excluded(relative):
        return ()
    suffix = Path(relative).suffix.lower()
    categories: list[str] = []
    if suffix in SOURCE_SUFFIXES and any(_is_below(relative, root) for root in SOURCE_ROOTS):
        categories.append("code")
    if (suffix in CONFIG_SUFFIXES or Path(relative).name in CONFIG_FILENAMES) and any(
        _is_below(relative, root) for root in CONFIG_ROOTS
    ):
        categories.append("config")
    return tuple(categories)


def _in_declared_scope(relative: str) -> bool:
    return any(_is_below(relative, root) for root in {*SOURCE_ROOTS, *CONFIG_ROOTS}) and not _is_excluded(relative)


def _tracked_files(root: Path, entries: Iterable[tuple[str, str]]) -> tuple[TrackedFile, ...]:
    files: list[TrackedFile] = []
    for mode, relative in entries:
        categories = _categories_for_path(relative)
        if mode == "160000":
            continue
        if mode == "120000":
            if _in_declared_scope(relative):
                raise BuildError("SEMANTIC_SYMLINK_SOURCE", f"tracked symlink is not an allowed semantic input: {relative}", 2)
            continue
        if not categories:
            continue
        if mode not in REGULAR_GIT_MODES:
            raise BuildError("SEMANTIC_GIT_MODE", f"tracked semantic input has unsupported git mode {mode}: {relative}", 2)
        files.append(TrackedFile(relative, root / Path(relative), categories))
    return tuple(files)


def _path_set_hash(files: Iterable[TrackedFile]) -> str:
    paths = sorted(item.relative for item in files)
    return _sha256("\0".join(paths).encode("utf-8") + b"\0")


def _mask_span(characters: list[str], start: int, end: int) -> None:
    for index in range(start, min(end, len(characters))):
        if characters[index] not in "\r\n":
            characters[index] = " "


def _is_identifier_character(value: str) -> bool:
    return value.isalnum() or value == "_"


def _mask_cpp_non_code(text: str) -> str:
    """Mask comments and literals while preserving offsets, line numbers, and columns."""

    characters = list(text)
    index = 0
    length = len(text)
    while index < length:
        current = text[index]
        following = text[index + 1] if index + 1 < length else ""
        if current == "/" and following == "/":
            end = text.find("\n", index + 2)
            end = length if end < 0 else end
            _mask_span(characters, index, end)
            index = end
            continue
        if current == "/" and following == "*":
            close = text.find("*/", index + 2)
            end = length if close < 0 else close + 2
            _mask_span(characters, index, end)
            index = end
            continue
        previous = text[index - 1] if index else ""
        raw_match = (
            _RAW_LITERAL_START_RE.match(text, index)
            if current in {"R", "u", "U", "L"} and not (previous and _is_identifier_character(previous))
            else None
        )
        if raw_match:
            delimiter = raw_match.group("delimiter")
            closing = ")" + delimiter + "\""
            close = text.find(closing, raw_match.end())
            end = length if close < 0 else close + len(closing)
            _mask_span(characters, index, end)
            index = end
            continue
        if current in {'"', "'"}:
            quote = current
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == quote:
                    end += 1
                    break
                end += 1
            _mask_span(characters, index, end)
            index = end
            continue
        index += 1
    return "".join(characters)


def _line_starts(text: str) -> tuple[int, ...]:
    starts = [0]
    starts.extend(index + 1 for index, value in enumerate(text) if value == "\n")
    return tuple(starts)


def _line_column(starts: Sequence[int], index: int) -> tuple[int, int]:
    line_index = bisect.bisect_right(starts, index) - 1
    return line_index + 1, index - starts[line_index] + 1


def _evidence_hash(rule_id: str, value: str) -> str:
    normalized = " ".join(value.split())
    return _sha256((rule_id + "\0" + normalized).encode("utf-8"))


def _finding(rule_id: str, path: str, starts: Sequence[int], index: int, evidence: str) -> dict[str, object]:
    line, column = _line_column(starts, index)
    return {
        "rule_id": rule_id,
        "path": path,
        "line": line,
        "column": column,
        "evidence_sha256": _evidence_hash(rule_id, evidence),
    }


def _regex_findings(rule_id: str, pattern: re.Pattern[str], text: str, path: str, starts: Sequence[int]) -> list[dict[str, object]]:
    return [_finding(rule_id, path, starts, match.start(), match.group(0)) for match in pattern.finditer(text)]


def _public_state_findings(masked: str, path: str, starts: Sequence[int]) -> list[dict[str, object]]:
    """Heuristically inventory public data members without claiming a full C++ parse."""

    findings: list[dict[str, object]] = []
    depth = 0
    scopes: list[dict[str, object]] = []
    offset = 0
    for line in masked.splitlines(keepends=True):
        while scopes and depth < int(scopes[-1]["member_depth"]):
            scopes.pop()
        active = scopes[-1] if scopes and depth == int(scopes[-1]["member_depth"]) else None
        access_match = _ACCESS_RE.match(line)
        if active and access_match:
            active["access"] = access_match.group(1)
        elif active and active["access"] == "public":
            field_match = _PUBLIC_MUTABLE_FIELD_RE.match(line.rstrip("\r\n"))
            if field_match:
                declaration = field_match.group("declaration")
                if "*" not in declaration and "&" not in declaration:
                    start = offset + field_match.start("name")
                    findings.append(_finding("state.public_mutable_field", path, starts, start, field_match.group(0)))
            pointer_match = _RAW_POINTER_MEMBER_RE.match(line.rstrip("\r\n"))
            if pointer_match:
                start = offset + pointer_match.start("name")
                findings.append(_finding("state.raw_pointer_member", path, starts, start, pointer_match.group(0)))

        class_match = _CLASS_OPEN_RE.search(line)
        opening_depth = depth
        depth += line.count("{") - line.count("}")
        if class_match:
            member_depth = opening_depth + 1
            scopes.append({"member_depth": member_depth, "access": "public" if class_match.group("kind") == "struct" else "private"})
        while scopes and depth < int(scopes[-1]["member_depth"]):
            scopes.pop()
        offset += len(line)
    return findings


def _cpp_findings(relative: str, text: str) -> list[dict[str, object]]:
    masked = _mask_cpp_non_code(text)
    starts = _line_starts(masked)
    findings: list[dict[str, object]] = []
    for rule_id, pattern in _GETTER_PATTERNS.items():
        findings.extend(_regex_findings(rule_id, pattern, masked, relative, starts))
    for rule_id, pattern in _CODE_PATTERNS:
        findings.extend(_regex_findings(rule_id, pattern, masked, relative, starts))
    if relative not in {"sakura_core/view/CViewSelect.cpp", "sakura_core/view/CViewSelect.h"}:
        findings.extend(
            _regex_findings(
                "state.legacy_selection_range_direct_access",
                _LEGACY_SELECTION_RANGE_DIRECT_ACCESS_RE,
                masked,
                relative,
                starts,
            )
        )
    findings.extend(_public_state_findings(masked, relative, starts))

    original_lines = text.splitlines(keepends=True)
    masked_lines = masked.splitlines(keepends=True)
    offset = 0
    for original, code_line in zip(original_lines, masked_lines):
        if re.match(r"^\s*#\s*include\b", code_line):
            include = _PRIVATE_INCLUDE_RE.match(original)
            if include:
                start = offset + include.start(1)
                findings.append(_finding("boundary.private_include", relative, starts, start, include.group(1)))
        offset += len(code_line)
    return findings


def _strip_config_comment(line: str) -> str:
    """Remove an unquoted # comment while retaining meaningful command strings."""

    quote = ""
    escaped = False
    for index, value in enumerate(line):
        if escaped:
            escaped = False
            continue
        if value == "\\":
            escaped = True
            continue
        if quote:
            if value == quote:
                quote = ""
            continue
        if value in {'"', "'"}:
            quote = value
            continue
        if value == "#":
            # Keep offsets stable: findings emitted from the normalized text use
            # positions in the original file for their line/column values.
            return line[:index] + "".join("\n" if tail == "\n" else "\r" if tail == "\r" else " " for tail in line[index:])
    return line


def _config_findings(relative: str, text: str) -> list[dict[str, object]]:
    findings: list[dict[str, object]] = []
    starts = _line_starts(text)
    normalized_lines = [_strip_config_comment(line) for line in text.splitlines(keepends=True)]
    normalized = "".join(normalized_lines)
    for match in _MONOLITH_LINK_RE.finditer(normalized):
        findings.append(_finding("test.monolith_link", relative, starts, match.start(), match.group(0)))
    is_test_or_ci = "test" in relative.lower() or relative.startswith(".github/") or "ci" in relative.lower()
    if is_test_or_ci:
        for match in _FILTER_RE.finditer(normalized):
            findings.append(_finding("test.filtered_or_skipped", relative, starts, match.start(), match.group(0)))
    return findings


def _hotspot(relative: str) -> bool:
    return (
        relative.endswith(("/CEditWnd.cpp", "/CEditWnd.h"))
        or "/view/CEditView" in relative
        or relative.endswith(("/doc/CEditDoc.cpp", "/doc/CEditDoc.h"))
        or relative.endswith("/CEditApp.cpp")
        or relative.endswith(("/env/CShareData.h", "/env/DLLSHAREDATA.h"))
    )


def _scan_files(files: Sequence[TrackedFile]) -> dict[str, object]:
    source_hash = hashlib.sha256()
    source_line_count = 0
    include_count = 0
    source_files = 0
    all_findings: list[dict[str, object]] = []
    per_path_findings: dict[str, list[dict[str, object]]] = defaultdict(list)
    getter_by_file: dict[str, Counter[str]] = defaultdict(Counter)

    getter_rule_to_name = {rule_id: name for name, rule_id in GETTER_NAMES}
    for item in files:
        data, text = _read_source(item.path, item.relative)
        source_hash.update(item.relative.encode("utf-8"))
        source_hash.update(b"\0")
        source_hash.update(hashlib.sha256(data).digest())
        findings: list[dict[str, object]] = []
        if "code" in item.categories:
            source_files += 1
            source_line_count += text.count("\n") + (0 if not text or text.endswith("\n") else 1)
            include_count += sum(1 for line in _mask_cpp_non_code(text).splitlines() if re.match(r"^\s*#\s*include\b", line))
            findings.extend(_cpp_findings(item.relative, text))
        if "config" in item.categories:
            findings.extend(_config_findings(item.relative, text))
        for finding in findings:
            rule_id = str(finding["rule_id"])
            if rule_id in getter_rule_to_name:
                getter_by_file[item.relative][getter_rule_to_name[rule_id]] += 1
        all_findings.extend(findings)
        per_path_findings[item.relative].extend(findings)

    all_findings.sort(key=lambda item: (str(item["rule_id"]), str(item["path"]), int(item["line"]), int(item["column"]), str(item["evidence_sha256"])))
    per_rule = Counter(str(finding["rule_id"]) for finding in all_findings)
    direct_getters = {name: per_rule.get(rule_id, 0) for name, rule_id in GETTER_NAMES}
    hotspot: dict[str, dict[str, int]] = {}
    for item in files:
        if "code" not in item.categories or not _hotspot(item.relative):
            continue
        data, text = _read_source(item.path, item.relative)
        del data
        path_findings = per_path_findings[item.relative]
        rule_counts = Counter(str(finding["rule_id"]) for finding in path_findings)
        hotspot[item.relative] = {
            "line_count": text.count("\n") + (0 if not text or text.endswith("\n") else 1),
            "include_count": sum(1 for line in _mask_cpp_non_code(text).splitlines() if re.match(r"^\s*#\s*include\b", line)),
            "getter_call_count": sum(rule_counts[rule_id] for _name, rule_id in GETTER_NAMES),
            "raw_new_expression_count": rule_counts["memory.raw_new"],
            "raw_delete_expression_count": rule_counts["memory.raw_delete"],
            "catch_all_count": rule_counts["error.catch_all"],
        }

    metrics: dict[str, object] = {
        "tracked_file_count": len(files),
        "source_file_count": source_files,
        "source_line_count": source_line_count,
        "include_directive_count": include_count,
        "finding_count": len(all_findings),
        "finding_count_by_rule": dict(sorted(per_rule.items())),
        "direct_global_getter_calls": {name: count for name, count in direct_getters.items() if count},
        "direct_global_getter_calls_by_file": {
            path: dict(sorted(counts.items())) for path, counts in sorted(getter_by_file.items()) if counts
        },
        "raw_new_expression_count": per_rule["memory.raw_new"],
        "raw_delete_expression_count": per_rule["memory.raw_delete"],
        "catch_all_count": per_rule["error.catch_all"],
        "win32_parameter_mention_count": per_rule["boundary.win32_type"],
        "private_include_mention_count": per_rule["boundary.private_include"],
        "monolith_link_hint_count": per_rule["test.monolith_link"],
        "filtered_test_hint_count": per_rule["test.filtered_or_skipped"],
    }
    return {
        "metrics": metrics,
        "findings": all_findings,
        "hotspots": dict(sorted(hotspot.items())),
        "source_fingerprint": _sha256(source_hash.digest()),
    }


def collect_semantic_inventory(repo_root: Path) -> dict[str, object]:
    """Collect a deterministic inventory from tracked first-party source/config inputs only."""

    root = repo_root.resolve()
    if not root.is_dir():
        raise BuildError("SEMANTIC_REPO_ROOT", f"repository root does not exist: {root}", 2)
    files = _tracked_files(root, _index_entries(root))
    scanned = _scan_files(files)
    source_commit = _git_text(root, ("rev-parse", "HEAD"), "SEMANTIC_SOURCE_COMMIT")
    scope = _scope_definition()
    rule_catalog = _rule_catalog_definition()
    return {
        "schema_version": SCHEMA_VERSION,
        "inventory_kind": INVENTORY_KIND,
        "source_commit": source_commit,
        "scanner_version": _scanner_version(),
        "tracked_source_set_sha256": _path_set_hash(files),
        "scope_definition_sha256": _sha256(_canonical_json(scope)),
        "rule_catalog_sha256": _sha256(_canonical_json(rule_catalog)),
        "scope_definition": scope,
        "rule_catalog": rule_catalog,
        "scope": list(SOURCE_ROOTS),
        "tracked_source_paths": [item.relative for item in files],
        **scanned,
    }


def _require_inventory(value: Mapping[str, object], name: str) -> None:
    required = {
        "schema_version",
        "inventory_kind",
        "source_commit",
        "scanner_version",
        "tracked_source_set_sha256",
        "scope_definition_sha256",
        "rule_catalog_sha256",
        "source_fingerprint",
        "scope_definition",
        "rule_catalog",
        "tracked_source_paths",
        "findings",
    }
    missing = sorted(required - set(value))
    if value.get("schema_version") != SCHEMA_VERSION or value.get("inventory_kind") != INVENTORY_KIND or missing:
        raise BuildError("SEMANTIC_BASELINE_SCHEMA", f"{name} semantic inventory is incompatible with schema v{SCHEMA_VERSION}: missing {', '.join(missing)}", 2)
    if not isinstance(value.get("findings"), list):
        raise BuildError("SEMANTIC_FINDINGS_INVALID", f"{name} semantic findings must be an array", 2)


def _finding_key(value: Mapping[str, object]) -> tuple[str, str, int, int, str]:
    try:
        return (
            str(value["rule_id"]),
            str(value["path"]),
            int(value["line"]),
            int(value["column"]),
            str(value["evidence_sha256"]),
        )
    except (KeyError, TypeError, ValueError) as error:
        raise BuildError("SEMANTIC_FINDINGS_INVALID", f"invalid semantic finding: {value!r}", 2) from error


def _name_status(root: Path, baseline_commit: str) -> tuple[dict[str, str], set[str], set[str]]:
    """Return rename maps and changed paths from baseline commit to the current worktree."""

    output = _git(root, ("diff", "--name-status", "-z", "-M", baseline_commit, "--"), "SEMANTIC_DIFF")
    records = [item for item in output.split(b"\0") if item]
    index = 0
    renames: dict[str, str] = {}
    changed: set[str] = set()
    pure_renames: set[str] = set()
    while index < len(records):
        status = records[index].decode("ascii", errors="replace")
        index += 1
        if status.startswith(("R", "C")):
            if index + 1 >= len(records):
                raise BuildError("SEMANTIC_DIFF", "git diff rename record was incomplete", 2)
            old_path = _normalise_git_path(records[index])
            new_path = _normalise_git_path(records[index + 1])
            index += 2
            if status.startswith("R"):
                renames[old_path] = new_path
                changed.add(new_path)
                if status == "R100":
                    pure_renames.add(new_path)
            else:
                changed.add(new_path)
            continue
        if index >= len(records):
            raise BuildError("SEMANTIC_DIFF", "git diff path record was incomplete", 2)
        path = _normalise_git_path(records[index])
        index += 1
        changed.add(path)
    return renames, changed, pure_renames


def _ensure_baseline_ancestor(root: Path, baseline_commit: str) -> None:
    try:
        completed = subprocess.run(
            ["git", "merge-base", "--is-ancestor", baseline_commit, "HEAD"],
            cwd=root,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        raise BuildError("SEMANTIC_BASELINE_ANCESTRY", f"could not verify baseline ancestry: {error}", 2) from error
    if completed.returncode != 0:
        raise BuildError("SEMANTIC_BASELINE_ANCESTRY", f"baseline source commit is not an ancestor of HEAD: {baseline_commit}", 2)


def _git_blob_texts(root: Path, commit: str, paths: Sequence[str]) -> dict[str, str]:
    if not paths:
        return {}
    query = b"".join((f"{commit}:{path}\n".encode("utf-8", errors="surrogateescape") for path in paths))
    output = _git(root, ("cat-file", "--batch"), "SEMANTIC_BASELINE_CONTENT", input_data=query)
    result: dict[str, str] = {}
    offset = 0
    for path in paths:
        newline = output.find(b"\n", offset)
        if newline < 0:
            raise BuildError("SEMANTIC_BASELINE_CONTENT", f"git cat-file response was truncated for {path}", 2)
        header = output[offset:newline].decode("utf-8", errors="replace")
        offset = newline + 1
        pieces = header.split()
        if len(pieces) != 3 or pieces[1] != "blob":
            raise BuildError("SEMANTIC_BASELINE_CONTENT", f"baseline blob is unavailable for {path}: {header}", 2)
        try:
            size = int(pieces[2])
        except ValueError as error:
            raise BuildError("SEMANTIC_BASELINE_CONTENT", f"baseline blob size is invalid for {path}: {header}", 2) from error
        data = output[offset:offset + size]
        if len(data) != size:
            raise BuildError("SEMANTIC_BASELINE_CONTENT", f"baseline blob was truncated for {path}", 2)
        offset += size
        if offset < len(output) and output[offset:offset + 1] == b"\n":
            offset += 1
        result[path] = _decode_source(data)
    return result


def _unchanged_line_map(before: str, after: str) -> dict[int, int]:
    """Map unchanged baseline lines to current lines without treating insertions as debt moves."""

    # Git object contents use LF, while a Windows worktree can use CRLF for the
    # same tracked source.  Line terminators are not semantic source changes and
    # must not turn every existing finding into a new one.
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    mapping: dict[int, int] = {}
    matcher = SequenceMatcher(a=before_lines, b=after_lines, autojunk=False)
    for tag, before_start, before_end, after_start, _after_end in matcher.get_opcodes():
        if tag != "equal":
            continue
        for offset in range(before_end - before_start):
            mapping[before_start + offset + 1] = after_start + offset + 1
    return mapping


def _current_path_text(root: Path, relative: str) -> str | None:
    path = root / Path(relative)
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise BuildError("SEMANTIC_CURRENT_CONTENT", f"could not read current input {relative}: {error}", 2) from error
    return _decode_source(data)


def _rule_counts(findings: Iterable[Mapping[str, object]]) -> dict[str, int]:
    counts = Counter(str(value["rule_id"]) for value in findings)
    return dict(sorted(counts.items()))


def compare_semantic_inventory(
    current: Mapping[str, object],
    baseline: Mapping[str, object],
    *,
    repo_root: Path,
) -> dict[str, object]:
    """Fail closed on scanner drift and on each new finding, not merely on global totals."""

    _require_inventory(current, "current")
    _require_inventory(baseline, "baseline")
    for key in ("scanner_version", "scope_definition_sha256", "rule_catalog_sha256"):
        if current.get(key) != baseline.get(key):
            raise BuildError("SEMANTIC_BASELINE_SCHEMA", f"semantic {key} differs from baseline; collect and review a new schema v{SCHEMA_VERSION} baseline", 2)
    root = repo_root.resolve()
    baseline_commit = str(baseline["source_commit"])
    _ensure_baseline_ancestor(root, baseline_commit)
    renames, changed_paths, pure_renames = _name_status(root, baseline_commit)

    baseline_findings = [dict(item) for item in baseline["findings"] if isinstance(item, Mapping)]
    current_findings = [dict(item) for item in current["findings"] if isinstance(item, Mapping)]
    baseline_keys = [_finding_key(item) for item in baseline_findings]
    current_keys = [_finding_key(item) for item in current_findings]
    if len(set(current_keys)) != len(current_keys):
        raise BuildError("SEMANTIC_FINDINGS_INVALID", "current semantic findings are not unique", 2)

    baseline_paths = sorted({path for _rule, path, _line, _column, _evidence in baseline_keys})
    baseline_texts = _git_blob_texts(root, baseline_commit, baseline_paths)
    current_key_set = set(current_keys)
    matched_current: set[tuple[str, str, int, int, str]] = set()
    matched_baseline: set[tuple[str, str, int, int, str]] = set()
    line_maps: dict[tuple[str, str], dict[int, int]] = {}

    for old_path in baseline_paths:
        current_path = renames.get(old_path, old_path)
        current_text = _current_path_text(root, current_path)
        if current_text is None:
            continue
        line_maps[(old_path, current_path)] = _unchanged_line_map(baseline_texts[old_path], current_text)

    for baseline_key in baseline_keys:
        rule_id, old_path, old_line, column, evidence = baseline_key
        current_path = renames.get(old_path, old_path)
        current_line = line_maps.get((old_path, current_path), {}).get(old_line)
        if current_line is None:
            continue
        candidate = (rule_id, current_path, current_line, column, evidence)
        if candidate in current_key_set:
            matched_baseline.add(baseline_key)
            matched_current.add(candidate)

    new_keys = sorted(set(current_keys) - matched_current)
    removed_keys = sorted(set(baseline_keys) - matched_baseline)
    baseline_logical_count: Counter[str] = Counter()
    removed_by_path: Counter[str] = Counter()
    for _rule, old_path, _line, _column, _evidence in baseline_keys:
        baseline_logical_count[renames.get(old_path, old_path)] += 1
    for _rule, old_path, _line, _column, _evidence in removed_keys:
        removed_by_path[renames.get(old_path, old_path)] += 1

    missing_reductions: list[dict[str, object]] = []
    for path in sorted(changed_paths):
        if path in pure_renames or baseline_logical_count[path] == 0:
            continue
        if removed_by_path[path] == 0:
            missing_reductions.append({"path": path, "baseline_finding_count": baseline_logical_count[path]})

    def serialise(keys: Iterable[tuple[str, str, int, int, str]]) -> list[dict[str, object]]:
        return [
            {"rule_id": rule, "path": path, "line": line, "column": column, "evidence_sha256": evidence}
            for rule, path, line, column, evidence in keys
        ]

    new_findings = serialise(new_keys)
    removed_findings = serialise(removed_keys)
    current_rule_counts = _rule_counts(current_findings)
    baseline_rule_counts = _rule_counts(baseline_findings)
    per_rule_delta = {
        rule_id: current_rule_counts.get(rule_id, 0) - baseline_rule_counts.get(rule_id, 0)
        for rule_id in sorted(set(current_rule_counts) | set(baseline_rule_counts))
    }
    return {
        "ok": not new_findings and not missing_reductions,
        "baseline_source_commit": baseline_commit,
        "current_source_commit": current.get("source_commit"),
        "baseline_source_fingerprint": baseline.get("source_fingerprint"),
        "current_source_fingerprint": current.get("source_fingerprint"),
        "baseline_tracked_source_set_sha256": baseline.get("tracked_source_set_sha256"),
        "current_tracked_source_set_sha256": current.get("tracked_source_set_sha256"),
        "new_findings": new_findings,
        "removed_findings": removed_findings,
        "increases": new_findings,
        "missing_touched_reductions": missing_reductions,
        "renames": dict(sorted(renames.items())),
        "changed_paths": sorted(changed_paths),
        "per_rule_delta": dict(sorted(per_rule_delta.items())),
        "ratcheted_rules": [rule_id for rule_id, _category, _version in RULE_CATALOG],
    }


def _inventory_bytes(inventory: Mapping[str, object]) -> bytes:
    return json.dumps(inventory, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8") + b"\n"


def _write_bytes_atomic(path: Path, content: bytes) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_bytes() == content:
            return False
    except FileNotFoundError:
        pass
    file_descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(file_descriptor, "wb") as stream:
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def write_semantic_inventory(path: Path, inventory: Mapping[str, object]) -> bool:
    """Write an inventory atomically and return whether its bytes changed."""

    return _write_bytes_atomic(path, _inventory_bytes(inventory))


def semantic_history_directory(baseline_path: Path) -> Path:
    return baseline_path.with_name(f"{baseline_path.stem}-history")


def _is_ci_environment(environment: Mapping[str, str] | None = None) -> bool:
    source = os.environ if environment is None else environment
    ci_markers = ("CI", "GITHUB_ACTIONS", "BUILD_BUILDID", "TF_BUILD")
    return any(source.get(name, "").strip().lower() not in {"", "0", "false", "no"} for name in ci_markers)


def _current_tree_path_hash(root: Path, commit: str) -> str:
    files = _tracked_files(root, _tree_entries(root, commit))
    return _path_set_hash(files)


def _history_record_path(history_directory: Path, source_commit: str) -> Path:
    if not re.fullmatch(r"[0-9a-fA-F]{40,64}", source_commit):
        raise BuildError("SEMANTIC_SOURCE_COMMIT", f"source commit is not a full hexadecimal object ID: {source_commit}", 2)
    return history_directory / f"{source_commit.lower()}.json"


def _repository_path(root: Path, path: Path, label: str) -> Path:
    candidate = path if path.is_absolute() else root / path
    resolved = candidate.resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise BuildError("SEMANTIC_PATH_ESCAPE", f"{label} must be inside the repository: {path}", 2) from error
    return resolved


def _previous_delta(previous: Mapping[str, object] | None, current: Mapping[str, object]) -> tuple[dict[str, int], dict[str, int]]:
    if previous is None or previous.get("schema_version") != SCHEMA_VERSION:
        return {}, {}
    previous_findings = [item for item in previous.get("findings", []) if isinstance(item, Mapping)]
    current_findings = [item for item in current.get("findings", []) if isinstance(item, Mapping)]
    before_rules = _rule_counts(previous_findings)
    after_rules = _rule_counts(current_findings)
    before_files = Counter(str(item["path"]) for item in previous_findings)
    after_files = Counter(str(item["path"]) for item in current_findings)
    rule_delta = {key: after_rules.get(key, 0) - before_rules.get(key, 0) for key in sorted(set(before_rules) | set(after_rules))}
    file_delta = {key: after_files.get(key, 0) - before_files.get(key, 0) for key in sorted(set(before_files) | set(after_files))}
    return rule_delta, file_delta


def accept_semantic_inventory(
    repo_root: Path,
    inventory: Mapping[str, object],
    baseline_path: Path,
    *,
    history_directory: Path | None,
    source_commit: str,
    accepted_reason: str,
    tracking_issue: int,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Accept a v2 baseline only from an exact clean source commit outside CI.

    The baseline and its immutable history record are staged together.  A normal
    exception restores the old baseline and removes the newly-created record.
    """

    _require_inventory(inventory, "current")
    root = repo_root.resolve()
    if _is_ci_environment(environment):
        raise BuildError("SEMANTIC_ACCEPT_CI_FORBIDDEN", "--accept-current is forbidden in CI", 2)
    if not accepted_reason.strip():
        raise BuildError("SEMANTIC_ACCEPT_REASON", "--accept-current requires a non-empty --reason", 2)
    if tracking_issue <= 0:
        raise BuildError("SEMANTIC_ACCEPT_ISSUE", "--accept-current requires a positive --tracking-issue", 2)
    head = _git_text(root, ("rev-parse", "HEAD"), "SEMANTIC_SOURCE_COMMIT")
    if head != source_commit:
        raise BuildError("SEMANTIC_ACCEPT_COMMIT", f"HEAD {head} does not match --source-commit {source_commit}", 2)
    if _git_text(root, ("status", "--porcelain"), "SEMANTIC_SOURCE_STATUS"):
        raise BuildError("SEMANTIC_ACCEPT_DIRTY", "--accept-current requires git status --porcelain to be empty", 2)
    if inventory.get("source_commit") != source_commit:
        raise BuildError("SEMANTIC_ACCEPT_COMMIT", "inventory source_commit does not match --source-commit", 2)
    if inventory.get("scanner_version") != _scanner_version():
        raise BuildError("SEMANTIC_ACCEPT_SCANNER", "inventory scanner_version does not match this scanner", 2)
    expected_scope_hash = _sha256(_canonical_json(_scope_definition()))
    if inventory.get("scope_definition_sha256") != expected_scope_hash:
        raise BuildError("SEMANTIC_ACCEPT_SCOPE", "inventory scope_definition_sha256 does not match this scanner", 2)
    expected_rule_catalog_hash = _sha256(_canonical_json(_rule_catalog_definition()))
    if inventory.get("rule_catalog_sha256") != expected_rule_catalog_hash:
        raise BuildError("SEMANTIC_ACCEPT_RULE_CATALOG", "inventory rule_catalog_sha256 does not match this scanner", 2)
    expected_path_hash = _current_tree_path_hash(root, source_commit)
    if inventory.get("tracked_source_set_sha256") != expected_path_hash:
        raise BuildError("SEMANTIC_ACCEPT_PATH_SET", "inventory tracked source set does not match the exact source commit", 2)

    baseline_path = _repository_path(root, baseline_path, "semantic baseline")
    history = _repository_path(root, history_directory or semantic_history_directory(baseline_path), "semantic history directory")
    record_path = _history_record_path(history, source_commit)
    if record_path.exists():
        raise BuildError("SEMANTIC_ACCEPT_HISTORY_EXISTS", f"baseline history record already exists: {record_path}", 2)
    try:
        previous_bytes = baseline_path.read_bytes()
        previous = json.loads(previous_bytes.decode("utf-8"))
    except FileNotFoundError:
        previous_bytes = None
        previous = None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BuildError("SEMANTIC_BASELINE_INVALID", f"could not read previous semantic baseline: {baseline_path}", 2) from error

    new_bytes = _inventory_bytes(inventory)
    rule_delta, file_delta = _previous_delta(previous if isinstance(previous, Mapping) else None, inventory)
    record = {
        "schema_version": 1,
        "history_kind": HISTORY_KIND,
        "previous_baseline_sha256": _sha256(previous_bytes) if previous_bytes is not None else "sha256:absent",
        "new_baseline_sha256": _sha256(new_bytes),
        "source_commit": source_commit,
        "source_fingerprint": inventory["source_fingerprint"],
        "scanner_version": inventory["scanner_version"],
        "tracked_source_set_sha256": inventory["tracked_source_set_sha256"],
        "scope_definition_sha256": inventory["scope_definition_sha256"],
        "rule_catalog_sha256": inventory["rule_catalog_sha256"],
        "per_rule_delta": rule_delta,
        "per_file_delta": file_delta,
        "accepted_reason": accepted_reason,
        "tracking_issue": tracking_issue,
    }
    record_bytes = _inventory_bytes(record)
    history.mkdir(parents=True, exist_ok=True)
    record_created = False
    try:
        with record_path.open("xb") as stream:
            stream.write(record_bytes)
        record_created = True
        baseline_changed = _write_bytes_atomic(baseline_path, new_bytes)
    except Exception:
        try:
            if previous_bytes is None:
                baseline_path.unlink(missing_ok=True)
            else:
                _write_bytes_atomic(baseline_path, previous_bytes)
            if record_created:
                record_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    return {
        "baseline_changed": baseline_changed,
        "history_record": str(record_path),
        "history_record_sha256": _sha256(record_bytes),
    }


def semantic_inventory_summary(
    inventory: Mapping[str, object],
    comparison: Mapping[str, object] | None = None,
    *,
    output_path: Path | None = None,
) -> dict[str, object]:
    metrics = inventory.get("metrics", {})
    compact_metrics = {
        key: metrics.get(key) if isinstance(metrics, Mapping) else None
        for key in (
            "tracked_file_count",
            "source_file_count",
            "source_line_count",
            "include_directive_count",
            "finding_count",
            "finding_count_by_rule",
            "direct_global_getter_calls",
        )
    }
    return {
        "inventory_kind": inventory.get("inventory_kind"),
        "schema_version": inventory.get("schema_version"),
        "source_commit": inventory.get("source_commit"),
        "source_fingerprint": inventory.get("source_fingerprint"),
        "tracked_source_set_sha256": inventory.get("tracked_source_set_sha256"),
        "finding_count": len(inventory.get("findings", [])),
        "hotspot_count": len(inventory.get("hotspots", {})),
        "metrics": compact_metrics,
        "comparison": comparison,
        "output": str(output_path) if output_path else None,
    }
