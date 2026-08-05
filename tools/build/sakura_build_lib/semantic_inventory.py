"""R0/R1 inventory and ratchet for Editor Core semantic debt.

This is intentionally a source-observation tool, not an AST claim.  It fixes a
repeatable baseline for the high-risk legacy access patterns called out by the
Editor Core decomposition track and prevents new occurrences from being added.
The inventory is diagnostic; the ratchet only gates explicitly named patterns.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
from collections import Counter, defaultdict
from pathlib import Path
from typing import Mapping

from .runner import BuildError


SCHEMA_VERSION = 1
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})
SOURCE_ROOTS = ("sakura_core", "sakura_lang", "src/main", "src/test", "tools")
SKIP_DIRECTORIES = frozenset({".git", ".vs", "build", "x64", "win32", "mingw", "__pycache__", ".pytest_cache"})

GETTER_NAMES = ("GetDllShareData", "GetEditWnd", "GetEditDoc")
_GETTER_PATTERNS = {
    name: re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}\s*\(") for name in GETTER_NAMES
}
_RAW_NEW_RE = re.compile(r"\bnew\s+[A-Za-z_][A-Za-z0-9_:<>]*(?:\s*[\*&])?")
_RAW_DELETE_RE = re.compile(r"\bdelete(?:\[\])?\s*[A-Za-z_][A-Za-z0-9_]*")
_CATCH_ALL_RE = re.compile(r"\bcatch\s*\(\s*\.\.\.\s*\)")
_WIN32_PARAM_RE = re.compile(r"\b(?:HWND|WPARAM|LPARAM)\b")
_PRIVATE_INCLUDE_RE = re.compile(r"#\s*include\s*[<\"]([^>\"]*/(?:platform|window|view|doc)/[^>\"]+)[>\"]")
_MONOLITH_LINK_RE = re.compile(r"(?:CollectSakuraObjectsForTests1|SakuraLinkInputsForTests1|sakura_core/.*\.obj)")
_FILTER_RE = re.compile(r"(?:gtest_filter|--filter|\bskip(?:ped|ping)?\b|exclude(?:d|ing)?)", re.IGNORECASE)

RATCHET_METRICS = (
    "direct_global_getter_calls",
    "raw_new_expression_count",
    "raw_delete_expression_count",
    "catch_all_count",
    "win32_parameter_mention_count",
    "private_include_mention_count",
    "monolith_link_hint_count",
    "filtered_test_hint_count",
)


def _read_source(path: Path) -> str:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    return data.decode(encoding, errors="replace")


def _relative(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def _source_files(root: Path) -> tuple[tuple[str, Path], ...]:
    files: list[tuple[str, Path]] = []
    for source_root in SOURCE_ROOTS:
        base = root / source_root
        if not base.is_dir():
            continue
        for current, directories, names in os.walk(base, followlinks=False):
            current_path = Path(current)
            directories[:] = sorted(
                name
                for name in directories
                if name not in SKIP_DIRECTORIES and not Path(current, name).is_symlink()
            )
            for name in sorted(names):
                path = current_path / name
                if path.suffix.lower() in SOURCE_SUFFIXES and not path.is_symlink():
                    files.append((_relative(root, path), path))
    return tuple(sorted(files, key=lambda item: item[0].lower()))


def _scan_files(root: Path, files: tuple[tuple[str, Path], ...]) -> dict[str, object]:
    getter_counts: Counter[str] = Counter()
    getter_by_file: dict[str, dict[str, int]] = {}
    hotspot: dict[str, dict[str, int]] = {}
    source_hash = hashlib.sha256()
    source_line_count = 0
    include_count = 0
    raw_new_count = 0
    raw_delete_count = 0
    catch_all_count = 0
    win32_parameter_count = 0
    private_include_count = 0

    for relative, path in files:
        text = _read_source(path)
        line_count = text.count("\n") + (0 if not text or text.endswith("\n") else 1)
        source_line_count += line_count
        include_count += sum(1 for line in text.splitlines() if re.match(r"^\s*#\s*include\b", line))
        file_getters: dict[str, int] = {}
        for name, pattern in _GETTER_PATTERNS.items():
            count = len(pattern.findall(text))
            if count:
                getter_counts[name] += count
                file_getters[name] = count
        if file_getters:
            getter_by_file[relative] = file_getters
        raw_new_count += len(_RAW_NEW_RE.findall(text))
        raw_delete_count += len(_RAW_DELETE_RE.findall(text))
        catch_all_count += len(_CATCH_ALL_RE.findall(text))
        win32_parameter_count += len(_WIN32_PARAM_RE.findall(text))
        private_include_count += len(_PRIVATE_INCLUDE_RE.findall(text))
        source_hash.update(relative.encode("utf-8"))
        source_hash.update(b"\0")
        source_hash.update(hashlib.sha256(text.encode("utf-8")).digest())

        if (
            relative.endswith(("/CEditWnd.cpp", "/CEditWnd.h"))
            or "/view/CEditView" in relative
            or relative.endswith(("/doc/CEditDoc.cpp", "/doc/CEditDoc.h"))
            or relative.endswith("/CEditApp.cpp")
            or relative.endswith(("/env/CShareData.h", "/env/DLLSHAREDATA.h"))
        ):
            hotspot[relative] = {
                "line_count": line_count,
                "include_count": sum(1 for line in text.splitlines() if re.match(r"^\s*#\s*include\b", line)),
                "getter_call_count": sum(file_getters.values()),
                "raw_new_expression_count": len(_RAW_NEW_RE.findall(text)),
                "raw_delete_expression_count": len(_RAW_DELETE_RE.findall(text)),
                "catch_all_count": len(_CATCH_ALL_RE.findall(text)),
            }

    monolith_hints: list[str] = []
    filtered_hints: list[str] = []
    for relative_root in ("sakura_core", "src/test", ".github", "tools"):
        base = root / relative_root
        if not base.is_dir():
            continue
        for current, directories, names in os.walk(base, followlinks=False):
            directories[:] = [name for name in directories if name not in SKIP_DIRECTORIES]
            for name in sorted(names):
                path = Path(current) / name
                if path.suffix.lower() not in {".vcxproj", ".filters", ".cmake", ".cpp", ".h", ".py", ".ps1", ".bat", ".yml", ".yaml"}:
                    continue
                relative = _relative(root, path)
                text = _read_source(path)
                if _MONOLITH_LINK_RE.search(text):
                    monolith_hints.append(relative)
                if _FILTER_RE.search(text) and ("workflow" in relative.lower() or "test" in relative.lower() or "ci" in relative.lower()):
                    filtered_hints.append(relative)

    metrics: dict[str, object] = {
        "source_file_count": len(files),
        "source_line_count": source_line_count,
        "include_directive_count": include_count,
        "direct_global_getter_calls": dict(sorted(getter_counts.items())),
        "direct_global_getter_calls_by_file": {
            key: dict(sorted(value.items())) for key, value in sorted(getter_by_file.items())
        },
        "raw_new_expression_count": raw_new_count,
        "raw_delete_expression_count": raw_delete_count,
        "catch_all_count": catch_all_count,
        "win32_parameter_mention_count": win32_parameter_count,
        "private_include_mention_count": private_include_count,
        "monolith_link_hint_count": len(monolith_hints),
        "filtered_test_hint_count": len(filtered_hints),
    }
    return {
        "metrics": metrics,
        "hotspots": dict(sorted(hotspot.items())),
        "monolith_link_hint_files": sorted(set(monolith_hints)),
        "filtered_test_hint_files": sorted(set(filtered_hints)),
        "source_fingerprint": "sha256:" + source_hash.hexdigest(),
    }


def collect_semantic_inventory(repo_root: Path) -> dict[str, object]:
    """Collect a deterministic Editor Core baseline from source text."""

    root = repo_root.resolve()
    if not root.is_dir():
        raise BuildError("SEMANTIC_REPO_ROOT", f"repository root does not exist: {root}", 2)
    files = _source_files(root)
    scanned = _scan_files(root, files)
    return {
        "schema_version": SCHEMA_VERSION,
        "inventory_kind": "editor-core-semantic",
        "scope": list(SOURCE_ROOTS),
        **scanned,
    }


def _flatten_numeric(value: object, prefix: str = "") -> dict[str, int]:
    result: dict[str, int] = {}
    if isinstance(value, bool):
        return result
    if isinstance(value, int):
        result[prefix] = value
    elif isinstance(value, Mapping):
        for key, child in value.items():
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            result.update(_flatten_numeric(child, child_prefix))
    return result


def compare_semantic_inventory(current: Mapping[str, object], baseline: Mapping[str, object]) -> dict[str, object]:
    """Compare ratcheted high-risk metrics without rejecting source growth."""

    if baseline.get("schema_version") != SCHEMA_VERSION or baseline.get("inventory_kind") != "editor-core-semantic":
        raise BuildError("SEMANTIC_BASELINE_SCHEMA", "semantic baseline schema or kind is incompatible", 2)
    current_metrics = current.get("metrics", {})
    baseline_metrics = baseline.get("metrics", {})
    if not isinstance(current_metrics, Mapping) or not isinstance(baseline_metrics, Mapping):
        raise BuildError("SEMANTIC_METRICS_INVALID", "semantic inventory metrics must be objects", 2)
    current_flat = _flatten_numeric(current_metrics)
    baseline_flat = _flatten_numeric(baseline_metrics)
    increases: list[dict[str, object]] = []
    for key, before in sorted(baseline_flat.items()):
        root_key = key.split(".", 1)[0]
        if root_key not in RATCHET_METRICS:
            continue
        after = current_flat.get(key, 0)
        if after > before:
            increases.append({"metric": key, "baseline": before, "current": after, "delta": after - before})
    return {
        "ok": not increases,
        "baseline_source_fingerprint": baseline.get("source_fingerprint"),
        "current_source_fingerprint": current.get("source_fingerprint"),
        "increases": increases,
        "ratcheted_metrics": list(RATCHET_METRICS),
    }


def write_semantic_inventory(path: Path, inventory: Mapping[str, object]) -> bool:
    """Write JSON atomically and return whether bytes changed."""

    path.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(inventory, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    encoded = content.encode("utf-8")
    try:
        if path.read_bytes() == encoded:
            return False
    except FileNotFoundError:
        pass
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def semantic_inventory_summary(
    inventory: Mapping[str, object],
    comparison: Mapping[str, object] | None = None,
    *,
    output_path: Path | None = None,
) -> dict[str, object]:
    metrics = inventory.get("metrics", {})
    return {
        "inventory_kind": inventory.get("inventory_kind"),
        "schema_version": inventory.get("schema_version"),
        "source_fingerprint": inventory.get("source_fingerprint"),
        "hotspot_count": len(inventory.get("hotspots", {})),
        "metrics": metrics,
        "comparison": comparison,
        "output": str(output_path) if output_path else None,
    }
