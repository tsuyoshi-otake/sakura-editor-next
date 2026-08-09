"""Coverage-map construction and fail-closed test selection.

The coverage map is deliberately independent from GitHub Actions.  A develop
workflow can generate one after a successful full test run, while a feature PR
workflow can use :func:`select_tests` without having to understand Cobertura's
XML format or the repository's module ownership rules.

The selector never turns an uncertain result into an empty test run.  Missing
or stale provenance, an unmapped production change, a test/configuration
change, or an invalid smoke selector all return a full-suite decision.
Documentation-only changes are the one intentional zero-impact case: they
return a smoke-only decision and still run the configured smoke selectors.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Iterable, Mapping, Sequence

from .test_inventory import TestInventoryError, validate_inventory


COVERAGE_MAP_SCHEMA_VERSION = 1
DEFAULT_FULL_FALLBACK_THRESHOLD = 0.65

_BASE_SHA_PATTERN = re.compile(r"^[0-9a-f]{40,64}$")
_SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_VALID_STATUS_PREFIXES = ("A", "C", "D", "M", "R", "T", "U")

# These patterns represent build/test topology, not ordinary source changes.
# Documentation is classified before these patterns so that a CLAUDE.md under
# tools/build does not accidentally force a full run.
DEFAULT_FORCE_FULL_PATTERNS: tuple[str, ...] = (
    "CMakeLists.txt",
    "**/CMakeLists.txt",
    "*.cmake",
    "**/*.cmake",
    "*.vcxproj",
    "**/*.vcxproj",
    "*.vcxproj.filters",
    "**/*.vcxproj.filters",
    "*.props",
    "**/*.props",
    "*.targets",
    "**/*.targets",
    "vcpkg.json",
    "**/vcpkg.json",
    "vcpkg-configuration.json",
    "**/vcpkg-configuration.json",
    "requirements.txt",
    "**/requirements.txt",
    "pyproject.toml",
    "**/pyproject.toml",
    "pytest.ini",
    "**/pytest.ini",
    "CMakePresets.json",
    "**/CMakePresets.json",
    "**/.coveragerc",
    ".clang-format",
    "**/.clang-format",
    "src/main/modules/**",
    "src/main/py/**",
    "src/test/cmake/**",
    "tools/build/**",
    ".github/workflows/**",
    ".github/actions/**",
    ".github/scripts/**",
    "**/pch.h",
    "**/Pch.h",
    "**/stdafx.h",
    "**/StdAfx.h",
    "**/*.rc",
    "**/*.rc2",
    "**/*.manifest",
)

_PRODUCTION_PREFIXES: tuple[str, ...] = (
    "sakura_core/",
    "src/main/",
    "sakura_lang/",
    "installer/",
    "tools/",
    "externals/",
)
_TEST_PREFIXES: tuple[str, ...] = ("src/test/", "tests/")
_DOC_SUFFIXES = {".md", ".markdown", ".rst", ".adoc"}
_DOC_BASENAMES = {"readme", "changelog", "changes", "license", "notice"}


class CoverageMapError(ValueError):
    """Typed input/provenance error for coverage map operations."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class ChangedFile:
    """A repository-relative path and its Git change status."""

    path: str
    status: str = "M"


def _canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise CoverageMapError("COVERAGE_MAP_TYPE", f"{label}: expected a non-empty string")
    return value.strip()


def _require_sha(value: Any, label: str, pattern: re.Pattern[str]) -> str:
    candidate = _require_string(value, label).lower()
    if not pattern.fullmatch(candidate):
        raise CoverageMapError("COVERAGE_MAP_SHA", f"{label}: invalid SHA-256 or commit SHA")
    return candidate


def _validate_selector(value: Any, label: str = "selector") -> str:
    selector = _require_string(value, label)
    if any(character.isspace() for character in selector) or ":" in selector or selector.startswith("-"):
        raise CoverageMapError(
            "COVERAGE_SELECTOR_INVALID",
            f"{label}: selectors must be positive, whitespace-free GoogleTest patterns",
        )
    return selector


def coverage_cache_key(
    base_sha: str,
    *,
    platform: str = "x64",
    schema_version: int = COVERAGE_MAP_SCHEMA_VERSION,
) -> str:
    """Return the cache key contract shared by map producers and consumers."""

    normalized_sha = _require_sha(base_sha, "base_sha", _BASE_SHA_PATTERN)
    if not isinstance(schema_version, int) or isinstance(schema_version, bool) or schema_version < 1:
        raise CoverageMapError("COVERAGE_MAP_SCHEMA", "schema_version must be a positive integer")
    normalized_platform = _require_string(platform, "platform").lower()
    if not re.fullmatch(r"[a-z0-9-]+", normalized_platform):
        raise CoverageMapError("COVERAGE_MAP_PLATFORM", "platform contains unsupported characters")
    return f"tia-map-windows-{normalized_platform}-{normalized_sha}-{schema_version}"


def _normalise_repo_path(repo_root: Path, value: str | Path, label: str) -> str:
    """Return a canonical POSIX path known to be inside ``repo_root``."""

    raw = str(value).strip()
    if not raw:
        raise CoverageMapError("COVERAGE_PATH_INVALID", f"{label}: path is empty")
    slash_value = raw.replace("\\", "/")
    # Reject traversal before resolving.  A deleted file is allowed, but a
    # caller must not be able to hide an outside path behind ``..``.
    if any(part == ".." for part in PurePosixPath(slash_value).parts):
        raise CoverageMapError("COVERAGE_PATH_ESCAPE", f"{label}: path contains '..': {raw}")

    root = repo_root.resolve()
    windows_absolute = PureWindowsPath(raw).is_absolute()
    path = Path(raw) if windows_absolute or Path(raw).is_absolute() else root / Path(slash_value)
    try:
        relative = path.resolve().relative_to(root)
    except ValueError as error:
        raise CoverageMapError("COVERAGE_PATH_ESCAPE", f"{label}: path escapes repository: {raw}") from error
    result = relative.as_posix()
    if not result or result == "." or result.startswith("../"):
        raise CoverageMapError("COVERAGE_PATH_INVALID", f"{label}: invalid repository-relative path: {raw}")
    return result


def _normalise_map_path(value: Any, label: str) -> str:
    raw = _require_string(value, label).replace("\\", "/")
    if PurePosixPath(raw).is_absolute() or PureWindowsPath(raw).is_absolute() or any(
        part == ".." for part in PurePosixPath(raw).parts
    ):
        raise CoverageMapError("COVERAGE_MAP_PATH", f"{label}: expected a repository-relative path")
    return raw


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise CoverageMapError("COVERAGE_INPUT_READ", f"could not read coverage input: {path}") from error
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    """Return a file's SHA-256 for CLI provenance collection."""

    return _hash_file(path)


def _xml_local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _resolve_coverage_filename(repo_root: Path, raw_filename: str, sources: Sequence[str], label: str) -> str:
    raw = _require_string(raw_filename, label)
    absolute = PureWindowsPath(raw).is_absolute() or Path(raw).is_absolute()
    candidates: list[Path]
    if absolute:
        candidates = [Path(raw)]
    else:
        candidates = [Path(source) / raw for source in sources if source.strip()]
        candidates.append(repo_root / raw.replace("\\", "/"))
    last_error: CoverageMapError | None = None
    for candidate in candidates:
        try:
            return _normalise_repo_path(repo_root, candidate, label)
        except CoverageMapError as error:
            last_error = error
    if last_error is not None:
        raise last_error
    raise CoverageMapError("COVERAGE_PATH_INVALID", f"{label}: no usable path")


def parse_cobertura_fragment(path: Path, repo_root: Path, selector: str) -> set[str]:
    """Return repository source paths covered by one suite-level XML file."""

    selector = _validate_selector(selector)
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        raise CoverageMapError("COVERAGE_XML_INVALID", f"could not parse Cobertura XML: {path}") from error

    sources = [
        text.strip()
        for node in root.iter()
        if _xml_local_name(node.tag) == "source" and (text := (node.text or "").strip())
    ]
    covered: set[str] = set()
    class_nodes = [node for node in root.iter() if _xml_local_name(node.tag) == "class"]
    for class_node in class_nodes:
        filename = class_node.attrib.get("filename")
        if filename is None:
            raise CoverageMapError("COVERAGE_XML_CLASS", f"class without filename in {path}")
        lines = [node for node in class_node.iter() if _xml_local_name(node.tag) == "line"]
        has_hits = False
        for line in lines:
            raw_hits = line.attrib.get("hits")
            if raw_hits is None:
                raise CoverageMapError("COVERAGE_XML_LINE", f"line without hits in {path}")
            try:
                has_hits = has_hits or float(raw_hits) > 0
            except ValueError as error:
                raise CoverageMapError("COVERAGE_XML_LINE", f"invalid line hit count in {path}") from error
        if has_hits:
            covered.add(_resolve_coverage_filename(repo_root, filename, sources, f"{path}:class.filename"))
    return covered


def _enabled_tests(inventory: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    try:
        validated = validate_inventory(inventory)
    except TestInventoryError as error:
        raise CoverageMapError(error.code, str(error)) from error
    return [item for item in validated["tests"] if item["status"] == "enabled"]


def _matching_tests(selector: str, tests: Sequence[Mapping[str, Any]]) -> set[str]:
    return {
        item["test_id"]
        for item in tests
        if _gtest_pattern_matches(selector, item["runtime"]["selector"])
    }


@lru_cache(maxsize=1024)
def _compile_gtest_pattern(pattern: str) -> re.Pattern[str]:
    expression = "".join(
        ".*" if character == "*" else "." if character == "?" else re.escape(character)
        for character in pattern
    )
    return re.compile(expression)


def _gtest_pattern_matches(pattern: str, value: str) -> bool:
    """Match Google's ``*``/``?`` filter syntax without fnmatch character classes."""

    return _compile_gtest_pattern(pattern).fullmatch(value) is not None


def _validate_selector_set(
    selectors: Iterable[Any],
    label: str,
    tests: Sequence[Mapping[str, Any]] | None = None,
    selector_match_cache: dict[str, bool] | None = None,
) -> list[str]:
    if not isinstance(selectors, (list, tuple, set)):
        raise CoverageMapError("COVERAGE_MAP_TYPE", f"{label}: expected an array")
    result = sorted({_validate_selector(value, f"{label}[]") for value in selectors})
    if tests is not None:
        for selector in result:
            if selector_match_cache is None:
                matches_inventory = bool(_matching_tests(selector, tests))
            else:
                matches_inventory = selector_match_cache.get(selector)
                if matches_inventory is None:
                    matches_inventory = bool(_matching_tests(selector, tests))
                    selector_match_cache[selector] = matches_inventory
            if not matches_inventory:
                raise CoverageMapError("COVERAGE_SELECTOR_UNKNOWN", f"{label}: selector matches no enabled inventory test: {selector}")
    return result


def validate_coverage_map(value: Any, inventory: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Validate and normalize a coverage map, optionally against inventory."""

    if not isinstance(value, dict):
        raise CoverageMapError("COVERAGE_MAP_TYPE", "coverage map must be an object")
    expected = {
        "schema_version",
        "base_sha",
        "platform",
        "configuration",
        "runner_id",
        "test_binary_sha256",
        "inventory_guarantee_fingerprint",
        "test_count",
        "source_to_tests",
        "fragments",
    }
    if set(value) != expected:
        raise CoverageMapError(
            "COVERAGE_MAP_FIELDS",
            f"coverage map fields must be {sorted(expected)}; got {sorted(value)}",
        )
    if value["schema_version"] != COVERAGE_MAP_SCHEMA_VERSION:
        raise CoverageMapError("COVERAGE_MAP_SCHEMA", "unsupported coverage map schema version")
    base_sha = _require_sha(value["base_sha"], "base_sha", _BASE_SHA_PATTERN)
    platform = _require_string(value["platform"], "platform")
    configuration = _require_string(value["configuration"], "configuration")
    runner_id = _require_string(value["runner_id"], "runner_id")
    binary_sha = _require_sha(value["test_binary_sha256"], "test_binary_sha256", _SHA256_PATTERN)
    fingerprint = _require_string(value["inventory_guarantee_fingerprint"], "inventory_guarantee_fingerprint")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", fingerprint):
        raise CoverageMapError("COVERAGE_MAP_FINGERPRINT", "inventory_guarantee_fingerprint must be sha256:<64 lowercase hex>")
    test_count = value["test_count"]
    if not isinstance(test_count, int) or isinstance(test_count, bool) or test_count < 1:
        raise CoverageMapError("COVERAGE_MAP_COUNT", "test_count must be a positive integer")

    if not isinstance(value["source_to_tests"], dict):
        raise CoverageMapError("COVERAGE_MAP_TYPE", "source_to_tests must be an object")
    map_entries: dict[str, list[str]] = {}
    for raw_path, selectors in value["source_to_tests"].items():
        path = _normalise_map_path(raw_path, "source_to_tests path")
        map_entries[path] = _validate_selector_set(selectors, f"source_to_tests[{path}]")
        if not map_entries[path]:
            raise CoverageMapError("COVERAGE_MAP_EMPTY", f"source_to_tests[{path}] must not be empty")

    if not isinstance(value["fragments"], list):
        raise CoverageMapError("COVERAGE_MAP_TYPE", "fragments must be an array")
    fragments: list[dict[str, str]] = []
    seen_fragments: set[tuple[str, str]] = set()
    for index, item in enumerate(value["fragments"]):
        if not isinstance(item, dict) or set(item) != {"selector", "path", "sha256"}:
            raise CoverageMapError("COVERAGE_MAP_FIELDS", f"fragments[{index}] must contain selector/path/sha256")
        fragment_selector = _validate_selector(item["selector"], f"fragments[{index}].selector")
        fragment_path = _require_string(item["path"], f"fragments[{index}].path")
        fragment_sha = _require_sha(item["sha256"], f"fragments[{index}].sha256", _SHA256_PATTERN)
        key = (fragment_selector, fragment_path)
        if key in seen_fragments:
            raise CoverageMapError("COVERAGE_MAP_DUPLICATE", f"duplicate coverage fragment: {key}")
        seen_fragments.add(key)
        fragments.append({"selector": fragment_selector, "path": fragment_path, "sha256": fragment_sha})

    validated = {
        "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
        "base_sha": base_sha,
        "platform": platform,
        "configuration": configuration,
        "runner_id": runner_id,
        "test_binary_sha256": binary_sha,
        "inventory_guarantee_fingerprint": fingerprint,
        "test_count": test_count,
        "source_to_tests": {path: map_entries[path] for path in sorted(map_entries)},
        "fragments": sorted(fragments, key=lambda item: (item["selector"], item["path"])),
    }
    if inventory is not None:
        try:
            inventory_value = validate_inventory(inventory)
        except TestInventoryError as error:
            raise CoverageMapError(error.code, str(error)) from error
        if inventory_value["guarantee_fingerprint"] != fingerprint:
            raise CoverageMapError("COVERAGE_MAP_INVENTORY", "coverage map inventory guarantee fingerprint is stale")
        if inventory_value["test_count"] != test_count:
            raise CoverageMapError("COVERAGE_MAP_INVENTORY", "coverage map test_count is stale")
        enabled = _enabled_tests(inventory_value)
        # A coverage map can contain many source paths that refer to the same
        # suite selector.  Validate each unique selector once rather than
        # repeatedly scanning the full inventory for every source-path entry.
        selector_match_cache: dict[str, bool] = {}
        for path, selectors in map_entries.items():
            _validate_selector_set(
                selectors,
                f"source_to_tests[{path}]",
                enabled,
                selector_match_cache,
            )
        for index, item in enumerate(fragments):
            _validate_selector_set(
                (item["selector"],),
                f"fragments[{index}].selector",
                enabled,
                selector_match_cache,
            )
    return validated


def load_coverage_map(path: Path, inventory: Mapping[str, Any] | None = None, expected_base_sha: str | None = None) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise CoverageMapError("COVERAGE_MAP_MISSING", f"coverage map does not exist: {path}") from error
    except (OSError, json.JSONDecodeError) as error:
        raise CoverageMapError("COVERAGE_MAP_READ", f"could not read coverage map: {path}") from error
    validated = validate_coverage_map(value, inventory)
    if expected_base_sha is not None:
        expected = _require_sha(expected_base_sha, "expected_base_sha", _BASE_SHA_PATTERN)
        if validated["base_sha"] != expected:
            raise CoverageMapError(
                "COVERAGE_MAP_BASE_SHA",
                f"coverage map base SHA {validated['base_sha']} does not match expected {expected}",
            )
    return validated


def _write_json_atomic(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
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


def write_coverage_map(path: Path, value: Mapping[str, Any]) -> None:
    _write_json_atomic(path, validate_coverage_map(value))


def write_json(path: Path, value: Mapping[str, Any]) -> None:
    """Write a deterministic JSON decision/evidence file atomically."""

    _write_json_atomic(path, value)


def build_coverage_map(
    *,
    base_sha: str,
    test_binary_sha256: str,
    inventory: Mapping[str, Any],
    fragments: Iterable[tuple[str, Path]],
    repo_root: Path,
    platform: str = "x64",
    configuration: str = "Debug",
    runner_id: str = "tests1",
) -> dict[str, Any]:
    """Merge suite-level Cobertura fragments into a deterministic map."""

    inventory_value = validate_inventory(inventory)
    enabled = _enabled_tests(inventory_value)
    source_to_tests: dict[str, set[str]] = {}
    fragment_values: list[dict[str, str]] = []
    for selector_value, fragment_path_value in fragments:
        selector = _validate_selector(selector_value, "fragment selector")
        if not _matching_tests(selector, enabled):
            raise CoverageMapError("COVERAGE_SELECTOR_UNKNOWN", f"fragment selector matches no enabled inventory test: {selector}")
        fragment_path = Path(fragment_path_value)
        covered = parse_cobertura_fragment(fragment_path, repo_root, selector)
        for source_path in covered:
            source_to_tests.setdefault(source_path, set()).add(selector)
        try:
            fragment_display = fragment_path.resolve().relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            fragment_display = fragment_path.name
        fragment_values.append({"selector": selector, "path": fragment_display, "sha256": _hash_file(fragment_path)})

    value = {
        "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
        "base_sha": base_sha.lower(),
        "platform": platform,
        "configuration": configuration,
        "runner_id": runner_id,
        "test_binary_sha256": test_binary_sha256.lower(),
        "inventory_guarantee_fingerprint": inventory_value["guarantee_fingerprint"],
        "test_count": inventory_value["test_count"],
        "source_to_tests": {
            path: sorted(selectors) for path, selectors in sorted(source_to_tests.items())
        },
        "fragments": fragment_values,
    }
    return validate_coverage_map(value, inventory_value)


def plan_coverage_map_shard(
    *,
    inventory: Mapping[str, Any],
    runner_id: str,
    shard_index: int,
    shard_count: int,
    excluded_selectors: Sequence[str] = (),
) -> dict[str, Any]:
    """Return one deterministic suite-level coverage-map shard.

    A map is collected from one executable at a time.  Splitting its suite
    selectors across a fixed number of shards makes the expensive coverage
    collection parallel without changing the selector granularity that a
    feature PR later executes.
    """

    if not isinstance(shard_count, int) or isinstance(shard_count, bool) or shard_count < 1:
        raise CoverageMapError("COVERAGE_SHARD_INVALID", "shard_count must be a positive integer")
    if (
        not isinstance(shard_index, int)
        or isinstance(shard_index, bool)
        or shard_index < 0
        or shard_index >= shard_count
    ):
        raise CoverageMapError("COVERAGE_SHARD_INVALID", "shard_index must be within shard_count")

    inventory_value = validate_inventory(inventory)
    normalized_runner = _require_string(runner_id, "runner_id")
    runner_tests = [
        item
        for item in _enabled_tests(inventory_value)
        if item["runtime"]["runner_id"] == normalized_runner
    ]
    if not runner_tests:
        raise CoverageMapError("COVERAGE_RUNNER_UNKNOWN", f"runner_id has no enabled inventory tests: {normalized_runner}")

    excluded = _validate_selector_set(excluded_selectors, "excluded_selectors", runner_tests)
    suite_selectors: set[str] = set()
    for item in runner_tests:
        selector = item["runtime"]["selector"]
        suite, separator, test_name = selector.rpartition(".")
        if not separator or not suite or not test_name:
            raise CoverageMapError(
                "COVERAGE_SUITE_INVALID",
                f"runner {normalized_runner} has a selector without suite/test delimiter: {selector}",
            )
        if any(_gtest_pattern_matches(pattern, selector) for pattern in excluded):
            continue
        suite_selectors.add(_validate_selector(f"{suite}.*", "suite selector"))

    selectors = sorted(suite_selectors)
    return {
        "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
        "runner_id": normalized_runner,
        "shard_index": shard_index,
        "shard_count": shard_count,
        "total_suite_count": len(selectors),
        "selectors": selectors[shard_index::shard_count],
    }


def merge_coverage_map_partials(
    *,
    partial_maps: Iterable[Mapping[str, Any]],
    inventory: Mapping[str, Any],
    expected_base_sha: str,
) -> dict[str, Any]:
    """Merge independently collected shard maps after checking provenance.

    Each shard validates and parses its Cobertura XML where it was produced.
    Only its compact, validated partial map crosses the workflow-job boundary.
    The final map preserves fragment digests as provenance while avoiding a
    transfer of hundreds of verbose XML reports.
    """

    inventory_value = validate_inventory(inventory)
    expected = _require_sha(expected_base_sha, "expected_base_sha", _BASE_SHA_PATTERN)
    values = [validate_coverage_map(value, inventory_value) for value in partial_maps]
    if not values:
        raise CoverageMapError("COVERAGE_MAP_PARTIAL_EMPTY", "at least one partial coverage map is required")

    reference = values[0]
    provenance_fields = (
        "base_sha",
        "platform",
        "configuration",
        "runner_id",
        "test_binary_sha256",
        "inventory_guarantee_fingerprint",
        "test_count",
    )
    if reference["base_sha"] != expected:
        raise CoverageMapError("COVERAGE_MAP_BASE_SHA", "partial coverage map base SHA does not match expected base SHA")

    source_to_tests: dict[str, set[str]] = {}
    fragments: list[dict[str, str]] = []
    seen_selectors: set[str] = set()
    for index, partial in enumerate(values):
        for field in provenance_fields:
            if partial[field] != reference[field]:
                raise CoverageMapError(
                    "COVERAGE_MAP_PARTIAL_PROVENANCE",
                    f"partial coverage map {index} disagrees on {field}",
                )
        for selector in (fragment["selector"] for fragment in partial["fragments"]):
            if selector in seen_selectors:
                raise CoverageMapError(
                    "COVERAGE_MAP_PARTIAL_DUPLICATE",
                    f"coverage suite appears in multiple partial maps: {selector}",
                )
            seen_selectors.add(selector)
        fragments.extend(partial["fragments"])
        for path, selectors in partial["source_to_tests"].items():
            source_to_tests.setdefault(path, set()).update(selectors)

    return validate_coverage_map(
        {
            "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
            **{field: reference[field] for field in provenance_fields},
            "source_to_tests": {
                path: sorted(selectors) for path, selectors in sorted(source_to_tests.items())
            },
            "fragments": fragments,
        },
        inventory_value,
    )


def _path_matches_pattern(path: str, pattern: str) -> bool:
    if fnmatch.fnmatchcase(path, pattern):
        return True
    if pattern.startswith("**/") and fnmatch.fnmatchcase(path, pattern[3:]):
        return True
    return False


def _is_documentation(path: str) -> bool:
    name = PurePosixPath(path).name.lower()
    stem = PurePosixPath(path).stem.lower()
    return PurePosixPath(path).suffix.lower() in _DOC_SUFFIXES or stem in _DOC_BASENAMES or name in {
        "license",
        "notice",
    }


def _is_deleted_or_renamed(status: str) -> bool:
    normalized = status.strip().upper()
    return normalized.startswith("D") or normalized.startswith("R")


def _is_production_path(path: str) -> bool:
    return path.startswith(_PRODUCTION_PREFIXES)


def _is_test_path(path: str) -> bool:
    return path.startswith(_TEST_PREFIXES)


def _component_value(component: Any, field: str) -> Sequence[str]:
    if isinstance(component, Mapping):
        value = component.get(field, ())
    else:
        value = getattr(component, field, ())
    return value if isinstance(value, (list, tuple)) else ()


def build_module_index(components: Mapping[str, Any], repo_root: Path) -> dict[str, tuple[str, ...]]:
    """Build a path-root index from SemanticGraph components or raw JSON."""

    result: dict[str, tuple[str, ...]] = {}
    fields = ("sources", "public_headers", "private_headers", "public_include_roots", "private_include_roots")
    for component_id, component in components.items():
        roots: set[str] = set()
        for field in fields:
            for raw_path in _component_value(component, field):
                if str(raw_path).replace("\\", "/") in {"", "."}:
                    # The legacy aggregate component uses the repository root
                    # as a private include root.  It cannot prove ownership of
                    # a changed path, so it must not make every production
                    # change select every mapped suite.  Skip that broad root;
                    # concrete source/header roots remain useful evidence.
                    continue
                else:
                    roots.add(_normalise_repo_path(repo_root, raw_path, f"modules.{component_id}.{field}"))
        if roots:
            result[str(component_id)] = tuple(sorted(roots))
    return result


def load_module_index(path: Path, repo_root: Path) -> dict[str, tuple[str, ...]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise CoverageMapError("MODULES_MISSING", f"modules.json does not exist: {path}") from error
    except (OSError, json.JSONDecodeError) as error:
        raise CoverageMapError("MODULES_READ", f"could not read modules.json: {path}") from error
    components = value.get("components") if isinstance(value, dict) else None
    if not isinstance(components, list):
        raise CoverageMapError("MODULES_FORMAT", "modules.json components must be an array")
    by_id: dict[str, Any] = {}
    for index, component in enumerate(components):
        if not isinstance(component, dict) or not isinstance(component.get("id"), str) or not component["id"]:
            raise CoverageMapError("MODULES_FORMAT", f"components[{index}] must have a non-empty id")
        by_id[component["id"]] = component
    return build_module_index(by_id, repo_root)


def _module_ids_for_path(path: str, module_index: Mapping[str, Sequence[str]]) -> set[str]:
    return {
        component_id
        for component_id, roots in module_index.items()
        if any(root in {"", "."} or path == root or path.startswith(root.rstrip("/") + "/") for root in roots)
    }


def _changed_file(value: ChangedFile | Mapping[str, Any] | str, repo_root: Path) -> ChangedFile:
    if isinstance(value, ChangedFile):
        path_value, status_value = value.path, value.status
    elif isinstance(value, Mapping):
        path_value, status_value = value.get("path"), value.get("status", "M")
    else:
        path_value, status_value = value, "M"
    if not isinstance(path_value, (str, Path)):
        raise CoverageMapError("CHANGE_PATH_INVALID", "changed file path must be a string")
    status = str(status_value).strip().upper() or "M"
    if not status[0] in _VALID_STATUS_PREFIXES:
        raise CoverageMapError("CHANGE_STATUS_INVALID", f"unsupported changed-file status: {status}")
    return ChangedFile(_normalise_repo_path(repo_root, path_value, "changed file"), status)


def _full_decision(total_enabled: int, threshold: float, reasons: Sequence[str], *, changed_paths: Sequence[str] = ()) -> dict[str, Any]:
    reason_codes = list(dict.fromkeys(reasons)) or ["full_requested"]
    return {
        "ok": True,
        "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
        "mode": "full",
        "full_fallback": True,
        "reason_codes": reason_codes,
        "gtest_filter": "",
        "changed_paths": list(changed_paths),
        "impacted_paths": [],
        "impacted_selectors": [],
        "run_selectors": [],
        "impacted_test_count": 0,
        "run_test_count": total_enabled,
        "total_enabled_tests": total_enabled,
        "threshold": threshold,
    }


def _filter_for_selectors(selectors: Sequence[str], excluded: Sequence[str]) -> str:
    positive = ":".join(sorted(set(selectors)))
    if not excluded:
        return positive
    negative = ":".join(sorted(set(excluded)))
    return f"{positive}-{negative}" if positive else f"-{negative}"


def select_tests(
    *,
    changed_files: Iterable[ChangedFile | Mapping[str, Any] | str],
    coverage_map: Mapping[str, Any] | None,
    inventory: Mapping[str, Any],
    repo_root: Path,
    expected_base_sha: str | None,
    smoke_selectors: Sequence[str],
    module_index: Mapping[str, Sequence[str]] | None = None,
    excluded_selectors: Sequence[str] = (),
    threshold: float = DEFAULT_FULL_FALLBACK_THRESHOLD,
    force_full_patterns: Sequence[str] = DEFAULT_FORCE_FULL_PATTERNS,
) -> dict[str, Any]:
    """Decide between full, impacted-suite, and smoke-only execution."""

    if threshold <= 0 or threshold > 1:
        raise CoverageMapError("COVERAGE_THRESHOLD_INVALID", "threshold must be greater than zero and at most one")
    try:
        inventory_value = validate_inventory(inventory)
    except TestInventoryError as error:
        return _full_decision(0, threshold, ["inventory_invalid"])
    enabled = _enabled_tests(inventory_value)
    total_enabled = len(enabled)

    try:
        normalized_changes = [_changed_file(value, repo_root) for value in changed_files]
    except CoverageMapError:
        return _full_decision(total_enabled, threshold, ["changed_path_invalid"])
    changed_paths = [item.path for item in normalized_changes]

    if coverage_map is None:
        return _full_decision(total_enabled, threshold, ["coverage_map_missing"], changed_paths=changed_paths)
    if expected_base_sha is None:
        return _full_decision(total_enabled, threshold, ["coverage_map_base_sha_missing"], changed_paths=changed_paths)
    try:
        validated_map = validate_coverage_map(coverage_map, inventory_value)
        if validated_map["base_sha"] != _require_sha(expected_base_sha, "expected_base_sha", _BASE_SHA_PATTERN):
            raise CoverageMapError("COVERAGE_MAP_BASE_SHA", "coverage map base SHA does not match expected base SHA")
    except CoverageMapError as error:
        return _full_decision(total_enabled, threshold, [error.code.lower()], changed_paths=changed_paths)

    try:
        smoke = _validate_selector_set(smoke_selectors, "smoke_selectors", enabled)
        excluded = _validate_selector_set(excluded_selectors, "excluded_selectors")
    except CoverageMapError as error:
        return _full_decision(total_enabled, threshold, [error.code.lower()], changed_paths=changed_paths)
    smoke_tests = set().union(*(_matching_tests(selector, enabled) for selector in smoke)) if smoke else set()
    excluded_tests = set().union(*(_matching_tests(selector, enabled) for selector in excluded)) if excluded else set()
    if smoke_tests.intersection(excluded_tests):
        return _full_decision(total_enabled, threshold, ["smoke_selector_excluded"], changed_paths=changed_paths)

    if not normalized_changes:
        if not smoke:
            return _full_decision(total_enabled, threshold, ["smoke_selector_missing"], changed_paths=changed_paths)
        run_count = len(smoke_tests.difference(excluded_tests))
        return {
            "ok": True,
            "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
            "mode": "smoke",
            "full_fallback": False,
            "reason_codes": ["no_changed_files"],
            "gtest_filter": _filter_for_selectors(smoke, excluded),
            "changed_paths": [],
            "impacted_paths": [],
            "impacted_selectors": [],
            "run_selectors": smoke,
            "impacted_test_count": 0,
            "run_test_count": run_count,
            "total_enabled_tests": total_enabled,
            "threshold": threshold,
        }

    # Classification is intentionally conservative.  A deletion/rename or a
    # change to test/build topology must not be hidden by a stale map entry.
    for changed in normalized_changes:
        if _is_deleted_or_renamed(changed.status):
            return _full_decision(total_enabled, threshold, ["deletion_or_rename"], changed_paths=changed_paths)
        if _is_documentation(changed.path):
            continue
        if changed.path in {".gitignore", ".editorconfig"}:
            return _full_decision(total_enabled, threshold, ["repository_configuration"], changed_paths=changed_paths)
        if any(_path_matches_pattern(changed.path, pattern) for pattern in force_full_patterns):
            return _full_decision(total_enabled, threshold, ["build_or_test_topology"], changed_paths=changed_paths)
        if _is_test_path(changed.path) or changed.path.startswith("externals/"):
            return _full_decision(total_enabled, threshold, ["test_or_external_source"], changed_paths=changed_paths)
        if not _is_production_path(changed.path):
            return _full_decision(total_enabled, threshold, ["unknown_change_class"], changed_paths=changed_paths)

    map_sources: Mapping[str, Sequence[str]] = validated_map["source_to_tests"]
    module_index = module_index or {}
    source_to_modules = {
        source: _module_ids_for_path(source, module_index)
        for source in map_sources
    }
    impacted_paths: set[str] = set()
    impacted_selectors: set[str] = set()
    for changed in normalized_changes:
        if _is_documentation(changed.path):
            continue
        direct = map_sources.get(changed.path)
        if direct:
            impacted_paths.add(changed.path)
            impacted_selectors.update(direct)
            continue
        module_ids = _module_ids_for_path(changed.path, module_index)
        if module_ids:
            module_sources = {
                source
                for source, source_modules in source_to_modules.items()
                if module_ids.intersection(source_modules)
            }
            if module_sources:
                impacted_paths.update(module_sources)
                for source in module_sources:
                    impacted_selectors.update(map_sources[source])
                continue
            return _full_decision(total_enabled, threshold, ["module_has_no_coverage"], changed_paths=changed_paths)
        return _full_decision(total_enabled, threshold, ["coverage_path_unmapped"], changed_paths=changed_paths)

    impacted_tests = set().union(*(_matching_tests(selector, enabled) for selector in impacted_selectors)) if impacted_selectors else set()
    if impacted_tests.intersection(excluded_tests):
        return _full_decision(total_enabled, threshold, ["impacted_tests_excluded"], changed_paths=changed_paths)
    if total_enabled and len(impacted_tests) / total_enabled >= threshold:
        return _full_decision(total_enabled, threshold, ["selected_test_threshold"], changed_paths=changed_paths)
    if not smoke:
        return _full_decision(total_enabled, threshold, ["smoke_selector_missing"], changed_paths=changed_paths)

    run_selectors = sorted(impacted_selectors.union(smoke))
    run_tests = set().union(*(_matching_tests(selector, enabled) for selector in run_selectors)) if run_selectors else set()
    run_tests.difference_update(excluded_tests)
    if not run_tests:
        return _full_decision(total_enabled, threshold, ["selection_zero_after_smoke"], changed_paths=changed_paths)
    mode = "selected" if impacted_selectors else "smoke"
    reasons = ["mapped_source_change"] if impacted_selectors else ["no_impacted_production_sources"]
    return {
        "ok": True,
        "schema_version": COVERAGE_MAP_SCHEMA_VERSION,
        "mode": mode,
        "full_fallback": False,
        "reason_codes": reasons,
        "gtest_filter": _filter_for_selectors(run_selectors, excluded),
        "changed_paths": changed_paths,
        "impacted_paths": sorted(impacted_paths),
        "impacted_selectors": sorted(impacted_selectors),
        "run_selectors": run_selectors,
        "impacted_test_count": len(impacted_tests),
        "run_test_count": len(run_tests),
        "total_enabled_tests": total_enabled,
        "threshold": threshold,
    }
