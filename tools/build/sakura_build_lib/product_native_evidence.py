"""MSBuild product observation kept separate from static repository provenance.

The legacy product is still monolithic.  This collector records what the
native compiler, resource compiler, and linker actually consumed without
claiming that a successful product build proves component isolation.  In
particular, a Debug link without a MAP file cannot prove which members of a
static archive were selected.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import uuid
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path, PureWindowsPath
from typing import Mapping, Sequence

from .model import SemanticGraph, evaluate_condition
from .package_restore import validate_package_restore
from .repository_path_safety import RepositoryPathSafetyError, safe_repository_path
from .runner import BuildError, EventWriter, msbuild_command


EVIDENCE_SCHEMA_VERSION = 4
_OUTPUT_ROOTS = frozenset({"build", "x64", "win32", "mingw"})
_PATH_INPUT_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".rc", ".rc2", ".obj", ".res", ".lib", ".natvis", ".manifest"})
_HEADER_SUFFIXES = frozenset({".h", ".hh", ".hpp", ".hxx", ".inl", ".inc"})
_OUTPUT_PROVIDER_SYMBOLS = (
    "sakura_output_provider_active_channel_v1",
    "sakura_output_provider_apply_v1",
    "sakura_output_provider_create_v1",
    "sakura_output_provider_destroy_v1",
    "sakura_output_provider_snapshot_measure_v1",
    "sakura_output_provider_snapshot_write_v1",
    "sakura_output_provider_stop_v1",
)
_OUTPUT_PROVIDER_SYMBOL_SET = frozenset(_OUTPUT_PROVIDER_SYMBOLS)
_OUTPUT_PROVIDER_SYMBOL_RE = re.compile(r"^sakura_output_provider_[A-Za-z0-9_]+$")
_OUTPUT_PROVIDER_ARCHIVE_NAME = "sakura_native_ffi.lib"
_MSVC_MAP_PUBLICS_HEADER_RE = re.compile(r"\bPublics\s+by\s+Value\b", re.IGNORECASE)
_MSVC_MAP_PUBLIC_ROW_RE = re.compile(
    r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+"
    r"(?P<symbol>\S+)\s+[0-9A-Fa-f]+\s+\S+\s+(?P<contributor>\S+)\s*$"
)
_MSVC_MAP_PUBLIC_ADDRESS_RE = re.compile(r"^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(?P<symbol>\S+)")
_MONITORED_MSBUILD_TARGET = re.compile(
    r"^(?:"
    r"Generate(?:VersionHeader|FuncCodeDefine|FuncCodeEnum|SakuraExeManifest|Bregonig|Migemo|CTags|Diff|Lang_.+)"
    r"|ConfigureCMakeTools|RunCompileTests|VcpkgInstallManifestDependencies"
    r")$",
    re.IGNORECASE,
)
_TARGET_START_RE = re.compile(
    r'(?:target|ターゲット)\s*"(?P<name>[^":]+):\s+\((?:target\s*id|ターゲット\s*ID):',
    re.IGNORECASE,
)
_EXEC_TASK_START_RE = re.compile(r'(?:task|タスク)\s+"Exec"\s+\(TaskId:', re.IGNORECASE)


_PACKAGE_DEPENDENCY_PHASES = ("generate", "compile", "link", "stage", "test", "runtime", "lifecycle")


def _declared_package_set_ids(graph: SemanticGraph, product_id: str, context_id: str) -> tuple[str, ...]:
    """Find package artifacts from the product dependency closure.

    Product-evidence fixtures intentionally omit compile-profile projections
    because they exercise native log parsing only. Deriving this narrow fact
    from graph edges keeps the evidence collector independent of the unrelated
    scheduler projection.
    """

    context = graph.context(context_id).as_mapping()
    nodes = set(graph.closure((product_id,), context_id, _PACKAGE_DEPENDENCY_PHASES))
    nodes.update(graph.final_link_closure((product_id,), context_id))
    return tuple(
        sorted(
            artifact.id
            for artifact in graph.artifacts.values()
            if artifact.id in nodes
            and artifact.artifact_kind == "package_set"
            and evaluate_condition(artifact.condition, context)
        )
    )


def _diagnostic_path_value(
    repo_root: Path,
    project_dir: Path,
    raw: str,
) -> str | None:
    value = raw.strip().strip('"').strip()
    value = re.sub(r"\s+\(TaskId:\s*\d+\)\s*$", "", value, flags=re.IGNORECASE)
    value = value.rstrip(".。")
    if not value or any(marker in value for marker in ("$", "%", "@")):
        return None
    kind, normalized = _path_value(repo_root, project_dir, value)
    return normalized if kind != "external" else None


def _paths_from_target_block(
    lines: Sequence[str],
    repo_root: Path,
    project_dir: Path,
    label: str,
) -> list[str]:
    values: set[str] = set()
    if label == "output":
        quoted = re.compile(r'(?:output file|出力ファイル)\s+"([^"]+)"', re.IGNORECASE)
        colon = re.compile(r"^\s*(?:output files?|出力ファイル)\s*:\s*(.+?)\s*$", re.IGNORECASE)
    else:
        quoted = re.compile(r'(?:input file|入力ファイル)\s+"([^"]+)"', re.IGNORECASE)
        colon = re.compile(r"^\s*(?:input files?|入力ファイル)\s*:\s*(.+?)\s*$", re.IGNORECASE)
    for line in lines:
        match = quoted.search(line)
        raw = match.group(1) if match else None
        if raw is None and (match := colon.match(line)):
            raw = match.group(1)
        if raw is None:
            continue
        value = _diagnostic_path_value(repo_root, project_dir, raw)
        if value is not None:
            values.add(value)
    return sorted(values)


def _parse_msbuild_target_observation(
    text: str,
    repo_root: Path,
    project_dir: Path,
) -> list[dict[str, object]]:
    """Normalize localized MSBuild diagnostic text into target terminal states.

    The diagnostic logger is intentionally ephemeral. Only paths, terminal
    outcomes, and actual Exec task starts enter durable evidence; timestamps,
    durations, task IDs, and the unique log path do not.
    """

    lines = text.splitlines()
    starts: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        match = _TARGET_START_RE.search(line)
        if match:
            starts.append((index, match.group("name")))

    invocations: dict[str, list[dict[str, object]]] = defaultdict(list)
    for position, (start, name) in enumerate(starts):
        end = starts[position + 1][0] if position + 1 < len(starts) else len(lines)
        if not _MONITORED_MSBUILD_TARGET.search(name):
            continue
        block = lines[start:end]
        joined = "\n".join(block)
        exec_task_count = sum(1 for line in block if _EXEC_TASK_START_RE.search(line))
        if re.search(r"all output files are up-to-date|すべての出力ファイルが入力ファイルに対して最新", joined, re.IGNORECASE):
            outcome = "up_to_date"
        elif exec_task_count or re.search(r"building target .* completely|ターゲット .* を完全にビルド", joined, re.IGNORECASE):
            outcome = "executed"
        else:
            outcome = "unclassified"
        invocations[name].append({
            "outcome": outcome,
            "exec_task_count": exec_task_count,
            "inputs": _paths_from_target_block(block, repo_root, project_dir, "input"),
            "outputs": _paths_from_target_block(block, repo_root, project_dir, "output"),
        })

    condition_patterns = (
        re.compile(r'target\s+"([^"]+)"\s+skipped,\s+due to false condition', re.IGNORECASE),
        re.compile(r'false\s+条件により、ターゲット\s+"([^"]+)"\s+を省略', re.IGNORECASE),
    )
    already_patterns = (
        re.compile(r'target\s+"([^"]+)"\s+skipped\.\s+Previously built successfully', re.IGNORECASE),
        re.compile(r'ターゲット\s+"([^"]+)"\s+を省略しました。以前に正しくビルド', re.IGNORECASE),
    )
    for line in lines:
        for patterns, outcome in ((condition_patterns, "condition_false"), (already_patterns, "already_built")):
            match = None
            for candidate in patterns:
                match = candidate.search(line)
                if match is not None:
                    break
            if match is None or not _MONITORED_MSBUILD_TARGET.search(match.group(1)):
                continue
            invocations[match.group(1)].append({
                "outcome": outcome,
                "exec_task_count": 0,
                "inputs": [],
                "outputs": [],
            })

    results: list[dict[str, object]] = []
    for name, records in sorted(invocations.items(), key=lambda item: item[0].casefold()):
        outcomes: dict[str, int] = defaultdict(int)
        inputs: set[str] = set()
        outputs: set[str] = set()
        exec_task_count = 0
        for record in records:
            outcomes[str(record["outcome"])] += 1
            inputs.update(str(value) for value in record["inputs"])
            outputs.update(str(value) for value in record["outputs"])
            exec_task_count += int(record["exec_task_count"])
        results.append({
            "target": name,
            "invocation_count": len(records),
            "terminal_outcomes": dict(sorted(outcomes.items())),
            "exec_task_count": exec_task_count,
            "execution_observed": exec_task_count > 0,
            "terminal_state_observed": "unclassified" not in outcomes,
            "inputs": sorted(inputs),
            "outputs": sorted(outputs),
        })
    return results


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _normalise_sha256(value: object) -> str | None:
    """Normalize a SHA-256 value without depending on another evidence module."""

    if not isinstance(value, str):
        return None
    text = value.strip().lower()
    if text.startswith("sha256:"):
        text = text[7:]
    if len(text) != 64 or any(char not in "0123456789abcdef" for char in text):
        return None
    return text


def _sha256_json(value: object) -> str:
    serialized = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return _sha256_bytes(serialized.encode("utf-8"))


def product_native_evidence_payload(evidence: Mapping[str, object]) -> dict[str, object]:
    """Return the canonical, hashable payload of one native observation."""

    return {
        key: value
        for key, value in evidence.items()
        if key not in {"schema_version", "collection_ok", "hard_evidence_hash", "hardEvidenceSha256"}
    }


def product_native_evidence_source_payload(evidence: Mapping[str, object]) -> dict[str, object]:
    """Return the source payload with a final-image binding removed.

    A final-image receipt is an observation made after the native product
    record was produced.  It must not change the source record's canonical
    identity or make its original link/MAP observations unverifiable.
    """

    payload = product_native_evidence_payload(evidence)
    link = payload.get("link")
    if isinstance(link, Mapping):
        source_link = dict(link)
        source_link.pop("final_image_stage", None)
        source_link.pop("finalImageStage", None)
        payload["link"] = source_link
    return payload


def product_native_evidence_hash(evidence: Mapping[str, object]) -> str:
    """Hash a native observation using the producer's canonical serializer."""

    return _sha256_json(product_native_evidence_payload(evidence))


def product_native_evidence_source_hash(evidence: Mapping[str, object]) -> str:
    """Hash the canonical source-native record, excluding stage binding data."""

    return _sha256_json(product_native_evidence_source_payload(evidence))


def _sha256_file(path: Path, code: str) -> str:
    try:
        return _sha256_bytes(path.read_bytes())
    except OSError as error:
        raise BuildError(code, f"could not hash {path}: {error}", 5) from error


def _file_identity(path: Path, code: str) -> tuple[str, int]:
    """Return one observed file hash and size from the same read operation."""

    try:
        data = path.read_bytes()
    except OSError as error:
        raise BuildError(code, f"could not hash {path}: {error}", 5) from error
    return _sha256_bytes(data), len(data)


def _read_tracker(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise BuildError("NATIVE_PRODUCT_TLOG_READ", f"could not read {path}: {error}", 5) from error
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        encoding = "utf-16"
    elif data.count(b"\x00") > max(4, len(data) // 8):
        encoding = "utf-16-le"
    else:
        encoding = "utf-8-sig"
    return data.decode(encoding, errors="replace")


def _tracker_records(path: Path) -> tuple[tuple[tuple[str, ...], tuple[str, ...]], ...]:
    records: list[tuple[tuple[str, ...], tuple[str, ...]]] = []
    headers: tuple[str, ...] | None = None
    payload: list[str] = []
    for raw_line in _read_tracker(path).splitlines():
        line = raw_line.strip().lstrip("\ufeff")
        if line.startswith("^"):
            if headers is not None:
                records.append((headers, tuple(payload)))
            headers = tuple(item.strip() for item in line[1:].split("|") if item.strip())
            payload = []
        elif headers is not None and line:
            payload.append(line)
    if headers is not None:
        records.append((headers, tuple(payload)))
    return tuple(records)


def _repo_relative(repo_root: Path, path: Path) -> str | None:
    resolved_repo = repo_root.resolve()
    resolved_path = path.resolve(strict=False)
    try:
        common = os.path.commonpath((str(resolved_repo), str(resolved_path)))
    except ValueError:
        return None
    if os.path.normcase(common) != os.path.normcase(str(resolved_repo)):
        return None
    relative = Path(os.path.relpath(resolved_path, resolved_repo)).as_posix()
    return None if relative == "." or relative.startswith("../") else relative


def _resolve_path(project_dir: Path, value: str) -> Path:
    raw = value.strip().strip('"').replace("\\", os.sep)
    candidate = Path(raw)
    return candidate if candidate.is_absolute() else project_dir / candidate


def _is_output_path(relative: str) -> bool:
    return relative.split("/", 1)[0].lower() in _OUTPUT_ROOTS


def _path_value(repo_root: Path, project_dir: Path, value: str) -> tuple[str, str]:
    path = _resolve_path(project_dir, value)
    relative = _repo_relative(repo_root, path)
    if relative is None:
        return "external", os.path.normpath(str(path.resolve(strict=False)))
    return ("generated" if _is_output_path(relative) else "source"), relative


def _unsafe_relative_reference(value: object) -> bool:
    """Reject absolute or parent-containing keys from untrusted evidence."""

    if not isinstance(value, str) or not value:
        return True
    normalized = value.replace("\\", "/")
    path = Path(normalized)
    windows = PureWindowsPath(value)
    return (
        path.is_absolute()
        or windows.is_absolute()
        or bool(windows.anchor)
        or any(part == ".." for part in path.parts)
        or any(part == ".." for part in windows.parts)
    )


def _single_path_line(value: str) -> bool:
    stripped = value.strip().strip('"')
    if not stripped or stripped.startswith(("/", "-")):
        return False
    return Path(stripped.replace("\\", "/")).suffix.lower() in _PATH_INPUT_SUFFIXES


def _pch_usage(command: str) -> dict[str, object]:
    match = re.search(r'(?:^|\s)/Y([uc])(?:"([^"]+)"|(\S+))?', command, re.IGNORECASE)
    if match is None:
        return {"mode": "none", "header": None, "file": None}
    # `/fp:precise` is the floating-point model and is not the `/Fp` PCH file
    # option.  Tracker logs contain both, so the colon form must not win.
    file_match = re.search(r'(?:^|\s)/Fp(?!:)(?:"([^"]+)"|(\S+))', command, re.IGNORECASE)
    return {
        "mode": "create" if match.group(1).lower() == "c" else "use",
        "header": match.group(2) or match.group(3),
        "file": (file_match.group(1) or file_match.group(2)) if file_match else None,
    }


def _object_output(repo_root: Path, project_dir: Path, source: str, command: str) -> str | None:
    match = re.search(r'(?:^|\s)/Fo(?:"([^"]+)"|(\S+))', command, re.IGNORECASE)
    if match is None:
        return None
    raw = match.group(1) or match.group(2)
    path = _resolve_path(project_dir, raw)
    if raw.endswith(("\\", "/")) or not path.suffix:
        path = path / f"{Path(source).stem}.obj"
    return _repo_relative(repo_root, path)


def _collect_compile_records(
    repo_root: Path,
    project_dir: Path,
    command_tlog: Path,
    read_tlog: Path,
) -> tuple[list[dict[str, object]], set[str], set[str], set[str]]:
    commands: dict[str, str] = {}
    for headers, payload in _tracker_records(command_tlog):
        if not headers:
            continue
        key = os.path.normcase(os.path.normpath(str(_resolve_path(project_dir, headers[0]).resolve(strict=False))))
        commands[key] = " ".join(payload)

    units: list[dict[str, object]] = []
    source_inputs: set[str] = set()
    generated_inputs: set[str] = set()
    external_inputs: set[str] = set()
    for headers, payload in _tracker_records(read_tlog):
        if not headers:
            continue
        primary_path = _resolve_path(project_dir, headers[0])
        primary_kind, primary = _path_value(repo_root, project_dir, headers[0])
        if primary_kind != "source":
            continue
        key = os.path.normcase(os.path.normpath(str(primary_path.resolve(strict=False))))
        command = commands.get(key, "")
        unit_source_inputs = {primary}
        unit_generated_inputs: set[str] = set()
        unit_external_count = 0
        for raw in payload:
            if not _single_path_line(raw):
                continue
            kind, value = _path_value(repo_root, project_dir, raw)
            if kind == "source":
                unit_source_inputs.add(value)
            elif kind == "generated":
                unit_generated_inputs.add(value)
            else:
                external_inputs.add(value)
                unit_external_count += 1
        source_inputs.update(unit_source_inputs)
        generated_inputs.update(unit_generated_inputs)
        pch = _pch_usage(command)
        if pch["file"]:
            kind, value = _path_value(repo_root, project_dir, str(pch["file"]))
            pch["file"] = value
            if kind == "generated":
                generated_inputs.add(value)
        units.append({
            "source": primary,
            "object": _object_output(repo_root, project_dir, primary, command),
            "pch": pch,
            "source_inputs": sorted(unit_source_inputs),
            "generated_inputs": sorted(unit_generated_inputs),
            "external_input_count": unit_external_count,
        })
    return (
        sorted(units, key=lambda item: str(item["source"])),
        source_inputs,
        generated_inputs,
        external_inputs,
    )


def _collect_resource_records(
    repo_root: Path,
    project_dir: Path,
    read_tlog: Path,
) -> tuple[list[dict[str, object]], set[str], set[str], set[str]]:
    units: list[dict[str, object]] = []
    source_inputs: set[str] = set()
    generated_inputs: set[str] = set()
    external_inputs: set[str] = set()
    for headers, payload in _tracker_records(read_tlog):
        if not headers:
            continue
        kind, source = _path_value(repo_root, project_dir, headers[0])
        if kind != "source":
            continue
        unit_source_inputs = {source}
        unit_generated_inputs: set[str] = set()
        unit_external_count = 0
        for raw in payload:
            if not _single_path_line(raw):
                continue
            input_kind, value = _path_value(repo_root, project_dir, raw)
            if input_kind == "source":
                unit_source_inputs.add(value)
            elif input_kind == "generated":
                unit_generated_inputs.add(value)
            else:
                external_inputs.add(value)
                unit_external_count += 1
        source_inputs.update(unit_source_inputs)
        generated_inputs.update(unit_generated_inputs)
        units.append({
            "source": source,
            "source_inputs": sorted(unit_source_inputs),
            "generated_inputs": sorted(unit_generated_inputs),
            "external_input_count": unit_external_count,
        })
    return (
        sorted(units, key=lambda item: str(item["source"])),
        source_inputs,
        generated_inputs,
        external_inputs,
    )


def _command_libraries(command: str, repo_root: Path, project_dir: Path) -> list[str]:
    values: set[str] = set()
    for match in re.finditer(r'(?i)(?:"([^"]+\.lib)"|(?<!\S)([^"\s]+\.lib)(?!\S))', command):
        raw = (match.group(1) or match.group(2)).replace("\\", "/")
        if "/" not in raw and not Path(raw).is_absolute():
            values.add(f"name:{raw.lower()}")
            continue
        kind, value = _path_value(repo_root, project_dir, raw)
        values.add(f"repo:{value}" if kind != "external" else f"external:{value}")
    return sorted(values, key=str.lower)


def _link_output(command: str, project_dir: Path) -> Path | None:
    match = re.search(r'(?i)(?:^|\s)/OUT:(?:"([^"]+)"|(\S+))', command)
    return _resolve_path(project_dir, match.group(1) or match.group(2)) if match else None


def _link_map_output(command: str, project_dir: Path, output: Path) -> Path | None:
    explicit = re.search(r'(?i)(?:^|\s)/MAP:(?:"([^"]+)"|(\S+))', command)
    if explicit:
        return _resolve_path(project_dir, explicit.group(1) or explicit.group(2))
    if re.search(r'(?i)(?:^|\s)/MAP(?:\s|$)', command):
        return output.with_suffix(".map")
    return None


def _selected_archive_members_from_map(
    map_path: Path,
    direct_object_inputs: Sequence[str],
) -> list[str]:
    """Return MAP object contributors that were not direct linker inputs.

    MSVC MAP rows name the contributing object at the end of each symbol row.
    Comparing those names with the exact link tracker inputs distinguishes an
    archive member from a directly supplied object without relying on linker
    stdout localization.
    """

    direct_names = {Path(value).name.casefold() for value in direct_object_inputs}
    selected: set[str] = set()
    try:
        with map_path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = re.search(r'(?i)([^\s]+\.obj)\s*$', line)
                if not match:
                    continue
                token = match.group(1).replace("\\", "/")
                # MSVC can render an archive contributor as either
                # ``archive.lib:member.obj`` or ``target:member.obj``.
                # A drive-qualified direct path is safe here as Path still
                # reduces the suffix to the final basename.
                member = Path(token.rsplit(":", 1)[-1]).name
                if member.casefold() not in direct_names:
                    selected.add(member)
    except OSError as error:
        raise BuildError("NATIVE_PRODUCT_MAP_READ", f"could not read {map_path}: {error}", 5) from error
    return sorted(selected, key=str.casefold)


def _normalize_map_contributor(token: str) -> tuple[str, str] | None:
    """Normalize the ``Lib:Object`` contributor printed by an MSVC MAP.

    Rust objects in an MSVC MAP are normally rendered as ``.rcgu.o`` members,
    while native MSVC archive members use ``.obj``.  The archive name is
    normalized to a basename so that a MAP's short ``lib:member`` spelling and
    a path-qualified spelling have the same evidence identity.
    """

    value = token.strip()
    if not value:
        return None
    if value.endswith(")") and "(" in value:
        archive_token, member_token = value[:-1].rsplit("(", 1)
    elif ":" in value:
        archive_token, member_token = value.rsplit(":", 1)
    else:
        return None
    archive_token = archive_token.strip().replace("\\", "/")
    member_token = member_token.strip().replace("\\", "/")
    if not archive_token or not member_token:
        return None
    archive_name = Path(archive_token).name
    member_name = Path(member_token).name
    if not archive_name or not member_name:
        return None
    if not archive_name.casefold().endswith(".lib"):
        if "." in archive_name:
            return None
        archive_name += ".lib"
    archive_name = archive_name.casefold()
    if Path(member_name).suffix.lower() not in {".obj", ".o"}:
        return None
    return archive_name, member_name


def _output_provider_map_rows(map_path: Path) -> list[dict[str, object]]:
    """Read exact output-provider public rows from the MSVC MAP public table.

    Only the ``Publics by Value`` table is authoritative here.  Static-symbol
    rows and decorated C++ names can contain provider-name substrings, but are
    not final public symbol rows and must not be counted as evidence.
    """

    rows: list[dict[str, object]] = []
    in_publics = False
    try:
        with map_path.open("r", encoding="utf-8-sig", errors="replace") as stream:
            for line in stream:
                if not in_publics:
                    if _MSVC_MAP_PUBLICS_HEADER_RE.search(line):
                        in_publics = True
                    continue
                if re.match(r"^\s*Static symbols\s*$", line, re.IGNORECASE):
                    break
                row = _MSVC_MAP_PUBLIC_ROW_RE.match(line)
                if row is not None:
                    symbol = row.group("symbol")
                    contributor = row.group("contributor")
                else:
                    address = _MSVC_MAP_PUBLIC_ADDRESS_RE.match(line)
                    if address is None:
                        continue
                    symbol = address.group("symbol")
                    contributor = None
                if not _OUTPUT_PROVIDER_SYMBOL_RE.fullmatch(symbol):
                    continue
                normalized = _normalize_map_contributor(contributor) if contributor is not None else None
                rows.append({
                    "symbol": symbol,
                    "archive": normalized[0] if normalized is not None else None,
                    "member": normalized[1] if normalized is not None else None,
                })
    except OSError as error:
        raise BuildError("NATIVE_PRODUCT_MAP_READ", f"could not read {map_path}: {error}", 5) from error
    return rows


def _output_provider_map_evidence(
    map_path: Path,
    map_relative: str | None,
    map_hash: str,
    repository_libraries: Sequence[str],
) -> tuple[dict[str, object], dict[str, object]]:
    """Build provider-scoped MAP evidence without claiming selector identity.

    The exact symbol set and its contributors establish that the final image's
    MAP contains the provider boundary.  They do not distinguish a production
    Rust selector from a linked-but-unused C++ candidate; the LTCG compile-log
    selector contract remains the selector proof consumed by the link-size
    analyzer.
    """

    rows = _output_provider_map_rows(map_path)
    by_symbol: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        by_symbol[str(row["symbol"])].append(row)
    symbols = sorted(by_symbol, key=str.casefold)
    duplicate_count = sum(max(0, len(items) - 1) for items in by_symbol.values())
    missing_symbols = sorted(_OUTPUT_PROVIDER_SYMBOL_SET - set(symbols), key=str.casefold)
    unexpected_symbols = sorted(set(symbols) - _OUTPUT_PROVIDER_SYMBOL_SET, key=str.casefold)
    contributions = [
        {
            "symbol": str(row["symbol"]),
            "archive": row["archive"],
            "member": row["member"],
        }
        for symbol in symbols
        for row in sorted(
            by_symbol[symbol],
            key=lambda item: (
                str(item["archive"] or "").casefold(),
                str(item["member"] or "").casefold(),
            ),
        )
    ]
    contributing_archives = sorted(
        {str(row["archive"]) for row in rows if row["archive"] is not None},
        key=str.casefold,
    )
    contributing_members = sorted(
        {str(row["member"]) for row in rows if row["member"] is not None},
        key=str.casefold,
    )
    archive_input_count = sum(
        1
        for value in repository_libraries
        if Path(value.replace("\\", "/")).name.casefold() == _OUTPUT_PROVIDER_ARCHIVE_NAME
    )
    rows_have_contributors = bool(rows) and all(
        row["archive"] is not None and row["member"] is not None
        for row in rows
    )
    archive_identity_observed = (
        rows_have_contributors
        and contributing_archives == [_OUTPUT_PROVIDER_ARCHIVE_NAME]
        and archive_input_count == 1
    )
    exact_symbol_set = set(symbols) == _OUTPUT_PROVIDER_SYMBOL_SET and len(symbols) == len(_OUTPUT_PROVIDER_SYMBOL_SET)
    complete = map_relative is not None and exact_symbol_set and duplicate_count == 0 and archive_identity_observed
    try:
        map_size = map_path.stat().st_size
    except OSError as error:
        raise BuildError("NATIVE_PRODUCT_MAP_HASH", f"could not stat {map_path}: {error}", 5) from error
    common = {
        "provider": "output-provider",
        "method": "msvc_map_publics_by_value_provider_rows",
        "map": map_relative,
        "map_hash": map_hash,
        "map_size_bytes": map_size,
        "archive_name": _OUTPUT_PROVIDER_ARCHIVE_NAME if archive_identity_observed else None,
        "archive_input_count": archive_input_count,
        "contributing_archives": contributing_archives,
        "contributing_archive_count": len(contributing_archives),
        "contributing_members": contributing_members,
        "contributions": contributions,
        "selector_proof": "ltcg_compile_log_required",
    }
    member_evidence = {
        **common,
        "observed": complete,
        "members": contributing_members,
        "member_count": len(contributing_members),
        "missing_symbols": missing_symbols,
        "unexpected_symbols": unexpected_symbols,
        "duplicate_count": duplicate_count,
    }
    symbol_evidence = {
        "provider": "output-provider",
        "scope": "output-provider",
        "method": "msvc_map_publics_by_value_provider_rows",
        "map": map_relative,
        "map_hash": map_hash,
        "map_size_bytes": map_size,
        "observed": complete,
        "symbols": symbols,
        "symbol_count": len(symbols),
        "duplicate_count": duplicate_count,
        "missing_symbols": missing_symbols,
        "unexpected_symbols": unexpected_symbols,
        "contributing_archives": contributing_archives,
        "contributing_members": contributing_members,
        "contributions": contributions,
        "selector_proof": "ltcg_compile_log_required",
    }
    return member_evidence, symbol_evidence


def _collect_link_record(
    repo_root: Path,
    project_dir: Path,
    command_tlog: Path,
    read_tlog: Path,
) -> tuple[dict[str, object], set[str]]:
    command_records = _tracker_records(command_tlog)
    if not command_records:
        raise BuildError("NATIVE_PRODUCT_LINK_COMMAND_EMPTY", f"no link record in {command_tlog}", 5)
    command = " ".join(command_records[0][1])
    provider_archive_command_values = [
        (match.group(1) or match.group(2)).replace("\\", "/")
        for match in re.finditer(r'(?i)(?:"([^"]+\.lib)"|(?<!\S)([^"\s]+\.lib)(?!\S))', command)
        if Path((match.group(1) or match.group(2)).replace("\\", "/")).name.casefold() == _OUTPUT_PROVIDER_ARCHIVE_NAME
    ]
    raw_inputs: list[str] = []
    for match in re.finditer(r'(?i)(?:^|\s)/MANIFESTINPUT:(?:"([^"]+)"|(\S+))', command):
        raw_inputs.append(match.group(1) or match.group(2))
    raw_inputs.extend(provider_archive_command_values)
    read_records = _tracker_records(read_tlog)
    link_input_headers = [value for headers, _payload in read_records for value in headers]
    for headers, payload in read_records:
        raw_inputs.extend(headers)
        raw_inputs.extend(line for line in payload if _single_path_line(line))
    repo_inputs: set[str] = set()
    external_libraries: set[str] = set()
    external_input_count = 0
    for raw in raw_inputs:
        kind, value = _path_value(repo_root, project_dir, raw)
        if kind in {"source", "generated"}:
            repo_inputs.add(value)
        else:
            external_input_count += 1
            if Path(value).suffix.lower() == ".lib":
                external_libraries.add(value)
    output = _link_output(command, project_dir)
    if output is None or not output.is_file():
        raise BuildError("NATIVE_PRODUCT_OUTPUT_MISSING", f"observed link output is missing: {output}", 5)
    output_relative = _repo_relative(repo_root, output)
    product_hash, product_size = _file_identity(output, "NATIVE_PRODUCT_OUTPUT_HASH")
    object_inputs = sorted(value for value in repo_inputs if Path(value).suffix.lower() == ".obj")
    resource_inputs = sorted(value for value in repo_inputs if Path(value).suffix.lower() == ".res")
    repository_libraries = sorted(value for value in repo_inputs if Path(value).suffix.lower() == ".lib")
    provider_archive_input_values = provider_archive_command_values or [
        value
        for value in link_input_headers
        if Path(value.replace("\\", "/")).name.casefold() == _OUTPUT_PROVIDER_ARCHIVE_NAME
    ]
    if not provider_archive_input_values:
        provider_archive_input_values = [
            value
            for value in repository_libraries
            if Path(value.replace("\\", "/")).name.casefold() == _OUTPUT_PROVIDER_ARCHIVE_NAME
        ]
    provider_archive_input_count = len(provider_archive_input_values)
    provider_archive_hash: str | None = None
    provider_archive_size: int | None = None
    if provider_archive_input_count == 1:
        provider_archive_raw = provider_archive_input_values[0]
        archive_kind, provider_archive_relative = _path_value(repo_root, project_dir, provider_archive_raw)
        if archive_kind in {"source", "generated"}:
            provider_archive_hash, provider_archive_size = _file_identity(
                repo_root / provider_archive_relative,
                "NATIVE_PRODUCT_ARCHIVE_HASH",
            )
    map_path = _link_map_output(command, project_dir, output)
    map_relative: str | None = None
    map_hash: str | None = None
    map_size: int | None = None
    selected_members: list[str] = []
    output_provider_member_evidence: dict[str, object] | None = None
    output_provider_symbol_evidence: dict[str, object] | None = None
    map_observed = map_path is not None and map_path.is_file()
    if map_observed and map_path is not None:
        map_relative = _repo_relative(repo_root, map_path)
        map_hash, map_size = _file_identity(map_path, "NATIVE_PRODUCT_MAP_HASH")
        selected_members = _selected_archive_members_from_map(map_path, object_inputs)
        output_provider_member_evidence, output_provider_symbol_evidence = _output_provider_map_evidence(
            map_path,
            map_relative,
            map_hash,
            provider_archive_input_values,
        )
    return ({
        "output": output_relative,
        "product_hash": product_hash,
        "product_size_bytes": product_size,
        "linkCommandSha256": _sha256_bytes(command.encode("utf-8")),
        "repo_inputs": sorted(repo_inputs),
        "object_inputs": object_inputs,
        "resource_inputs": resource_inputs,
        "repository_libraries": repository_libraries,
        "resolved_external_libraries": sorted(external_libraries, key=str.lower),
        "external_input_count": external_input_count,
        "command_libraries": _command_libraries(command, repo_root, project_dir),
        "input_set_observed": bool(raw_inputs),
        "selected_archive_members_observed": map_observed,
        "selected_archive_member_evidence": {
            "method": "msvc_map_minus_direct_link_inputs",
            "map": map_relative,
            "map_hash": map_hash,
            "map_size_bytes": map_size,
            "members": selected_members,
            "member_count": len(selected_members),
        } if map_observed else None,
        "output_provider_member_evidence": output_provider_member_evidence,
        "output_provider_symbol_evidence": output_provider_symbol_evidence,
        "rustArchiveSha256": provider_archive_hash,
        "rustArchiveSizeBytes": provider_archive_size,
        "rustArchiveCount": provider_archive_input_count,
    }, repo_inputs)


def _declared_tlog_roots(project: Path, platform: str, configuration: str) -> tuple[Path, ...]:
    """Resolve literal MSBuild IntDir declarations before considering sibling variants."""
    try:
        root = ET.parse(project).getroot()
    except (ET.ParseError, OSError):
        return ()

    replacements = {
        "$(Platform)": platform,
        "$(Configuration)": configuration,
        "$(ProjectName)": project.stem,
        "$(MSBuildProjectName)": project.stem,
    }
    candidates: set[Path] = set()
    for node in root.findall(".//{*}IntDir"):
        value = (node.text or "").strip()
        for key, replacement in replacements.items():
            value = value.replace(key, replacement)
        if not value or "$(" in value:
            continue
        directory = Path(value.replace("\\", "/"))
        if not directory.is_absolute():
            directory = project.parent / directory
        candidates.add((directory.resolve() / f"{project.stem}.tlog").resolve())
    return tuple(sorted(candidates))


def _tlog_root(graph: SemanticGraph, product_id: str, context_id: str) -> tuple[Path, str]:
    component = graph.components.get(product_id)
    if component is None:
        raise BuildError("NATIVE_PRODUCT_UNKNOWN", f"unknown product: {product_id}", 2)
    context = graph.context(context_id)
    if context.backend != "msbuild":
        raise BuildError("NATIVE_PRODUCT_CONTEXT", f"native product observation requires MSBuild, got {context.backend}", 2)
    targets = component.backend_targets.get("msbuild", ())
    if len(targets) != 1:
        raise BuildError("NATIVE_PRODUCT_TARGET", f"product must have exactly one MSBuild target: {product_id}", 2)
    project_relative = targets[0]
    project = graph.repo_root / project_relative
    base = graph.repo_root / f"build/{context.platform}/{context.configuration}"
    candidates = sorted(
        path for path in base.glob(f"*/{project.stem}.tlog")
        if path.is_dir() and any(path.glob("CL.read.*.tlog")) and any(path.glob("link.read.*.tlog"))
    )
    declared = tuple(
        path
        for path in _declared_tlog_roots(project, context.platform, context.configuration)
        if path in candidates
    )
    if len(declared) == 1:
        return declared[0], project_relative
    if len(declared) > 1:
        raise BuildError(
            "NATIVE_PRODUCT_TLOG_AMBIGUOUS",
            f"MSBuild IntDir resolves to multiple {project.stem}.tlog directories: {', '.join(str(path) for path in declared)}",
            5,
        )
    if len(candidates) != 1:
        raise BuildError(
            "NATIVE_PRODUCT_TLOG_MISSING" if not candidates else "NATIVE_PRODUCT_TLOG_AMBIGUOUS",
            f"expected one {project.stem}.tlog below {base}, found {len(candidates)}",
            5,
        )
    return candidates[0], project_relative


def _one_tlog(root: Path, pattern: str) -> Path:
    matches = sorted(root.glob(pattern))
    if len(matches) != 1:
        raise BuildError(
            "NATIVE_PRODUCT_TLOG_MISSING" if not matches else "NATIVE_PRODUCT_TLOG_AMBIGUOUS",
            f"expected one {pattern} below {root}, found {len(matches)}",
            5,
        )
    return matches[0]


def _hash_repo_inputs(repo_root: Path, values: Sequence[str], code: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for relative in sorted(set(values)):
        path = repo_root / relative
        if not path.is_file():
            raise BuildError(code, f"observed repository input is missing: {relative}", 5)
        result[relative] = _sha256_file(path, code)
    return result


def collect_product_native_evidence(
    graph: SemanticGraph,
    product_id: str,
    context_id: str,
    *,
    build_observed: bool,
    build_target: str = "Build",
    diagnostic_log_path: Path | None = None,
    package_restore: Mapping[str, object] | None = None,
) -> dict[str, object]:
    tlog_root, project_relative = _tlog_root(graph, product_id, context_id)
    project_dir = (graph.repo_root / project_relative).parent
    tlogs = {
        "compiler_command": _one_tlog(tlog_root, "CL.command.*.tlog"),
        "compiler_read": _one_tlog(tlog_root, "CL.read.*.tlog"),
        "link_command": _one_tlog(tlog_root, "link.command.*.tlog"),
        "link_read": _one_tlog(tlog_root, "link.read.*.tlog"),
        "resource_read": _one_tlog(tlog_root, "rc.read.*.tlog"),
    }
    units, source_inputs, generated_inputs, external_compile = _collect_compile_records(
        graph.repo_root, project_dir, tlogs["compiler_command"], tlogs["compiler_read"]
    )
    resources, resource_source_inputs, resource_generated_inputs, external_resources = _collect_resource_records(
        graph.repo_root, project_dir, tlogs["resource_read"]
    )
    link, link_inputs = _collect_link_record(
        graph.repo_root, project_dir, tlogs["link_command"], tlogs["link_read"]
    )
    all_source_inputs = source_inputs | resource_source_inputs
    link_generated_inputs = {value for value in link_inputs if _is_output_path(value)}
    all_generated_inputs = generated_inputs | resource_generated_inputs | link_generated_inputs
    definition_candidates = (
        project_relative,
        "Directory.Build.props",
        "Directory.Build.targets",
        "src/main/modules/modules.json",
        "src/main/msbuild/cmake.props",
        "src/main/msbuild/cmake.targets",
        "src/main/cmake/sakura.cmake",
        "src/main/cmake/version.cmake",
        "src/main/cmake/version.h.in",
        "src/main/cmake/manifest.cmake",
        "src/main/cmake/manifest.in",
        "src/main/py/header_make.py",
        "sakura_core/Funccode_x.hsrc",
    )
    definitions = [relative for relative in definition_candidates if (graph.repo_root / relative).is_file()]
    freshness = {
        "source_inputs": _hash_repo_inputs(graph.repo_root, sorted(all_source_inputs), "NATIVE_PRODUCT_SOURCE_HASH"),
        "generated_inputs": _hash_repo_inputs(graph.repo_root, sorted(all_generated_inputs), "NATIVE_PRODUCT_GENERATED_HASH"),
        "definition_inputs": _hash_repo_inputs(graph.repo_root, definitions, "NATIVE_PRODUCT_DEFINITION_HASH"),
        "tracker_inputs": {
            path.relative_to(graph.repo_root).as_posix(): _sha256_file(path, "NATIVE_PRODUCT_TLOG_HASH")
            for path in sorted(tlogs.values())
        },
    }
    pch_counts: dict[str, int] = defaultdict(int)
    for unit in units:
        pch_counts[str(unit["pch"]["mode"])] += 1
    diagnostic_targets: list[dict[str, object]] = []
    diagnostic_log_observed = diagnostic_log_path is not None
    if diagnostic_log_path is not None:
        try:
            diagnostic_text = diagnostic_log_path.read_text(encoding="utf-8-sig", errors="replace")
        except OSError as error:
            raise BuildError(
                "NATIVE_PRODUCT_DIAGNOSTIC_LOG_READ",
                f"could not read {diagnostic_log_path}: {error}",
                5,
            ) from error
        diagnostic_targets = _parse_msbuild_target_observation(
            diagnostic_text,
            graph.repo_root,
            project_dir,
        )

    phase_inputs = {
        "compiler": set(generated_inputs),
        "resource": set(resource_generated_inputs),
        "link": {value for value in link_inputs if _is_output_path(value)},
    }
    normalized_phase_inputs = {
        phase: {value.casefold() for value in values}
        for phase, values in phase_inputs.items()
    }
    correlations: list[dict[str, object]] = []
    for target in diagnostic_targets:
        for produced in target["outputs"]:
            for phase, consumed in normalized_phase_inputs.items():
                if str(produced).casefold() not in consumed:
                    continue
                correlations.append({
                    "consumer": product_id,
                    "producer_target": target["target"],
                    "output": produced,
                    "consuming_phase": phase,
                    "terminal_outcomes": target["terminal_outcomes"],
                    "execution_observed": target["execution_observed"],
                })
    correlated_targets = {
        str(item["producer_target"]): bool(item["execution_observed"])
        for item in correlations
    }
    generator_scheduling_observed = bool(diagnostic_targets) and all(
        bool(target["terminal_state_observed"]) for target in diagnostic_targets
    )
    generator_execution_observed = bool(correlated_targets) and all(correlated_targets.values())
    stable_payload = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
        "backend": "msbuild",
        "project": project_relative,
        "build_observed": build_observed,
        "build_target": build_target,
        "compiler": {
            "translation_units": units,
            "translation_unit_count": len(units),
            "source_inputs": sorted(source_inputs),
            "generated_inputs": sorted(generated_inputs),
            "generated_header_inputs": sorted(value for value in generated_inputs if Path(value).suffix.lower() in _HEADER_SUFFIXES),
            "external_input_count": len(external_compile),
            "pch_mode_counts": dict(sorted(pch_counts.items())),
            "dependency_trace_observed": bool(units),
            "pch_usage_observed": bool(units),
        },
        "resource_compiler": {
            "units": resources,
            "unit_count": len(resources),
            "source_inputs": sorted(resource_source_inputs),
            "generated_inputs": sorted(resource_generated_inputs),
            "external_input_count": len(external_resources),
            "input_trace_observed": bool(resources),
            "resource_table_observed": False,
        },
        "generator_observation": {
            "diagnostic_log_observed": diagnostic_log_observed,
            "target_scheduling_observed": generator_scheduling_observed,
            "correlated_execution_observed": generator_execution_observed,
            "target_results": diagnostic_targets,
            "producer_consumer_correlations": sorted(
                correlations,
                key=lambda item: (
                    str(item["producer_target"]),
                    str(item["output"]),
                    str(item["consuming_phase"]),
                ),
            ),
        },
        "link": link,
        "package_restore": dict(package_restore) if package_restore is not None else {
            "required": bool(_declared_package_set_ids(graph, product_id, context_id)),
            "native_restore_execution_observed": False,
            "package_closure_validated": False,
            "reason": "no explicit package restore result was supplied for this native observation",
        },
        "freshness": freshness,
    }
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "collection_ok": True,
        **stable_payload,
        "hard_evidence_hash": product_native_evidence_hash(
            {"schema_version": EVIDENCE_SCHEMA_VERSION, "collection_ok": True, **stable_payload}
        ),
    }


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            text=True,
            check=False,
        )
    else:
        process.kill()


def _native_observation_command(argv: Sequence[str], build_target: str) -> list[str]:
    result = list(argv)
    if build_target == "Rebuild":
        # Rebuild owns compiler/link outputs, but the nested CMake workspace is
        # shared by the app and language-resource projects. Only this explicit
        # full generator observation may remove it before CoreClean.
        result.append("/p:SakuraCleanCMakeToolsBuildDir=true")
    return result


def observe_product_native_evidence(
    graph: SemanticGraph,
    product_id: str,
    context_id: str,
    jobs: int,
    timeout_seconds: int,
    events: EventWriter,
    *,
    build_target: str = "Build",
    package_restore: Mapping[str, object] | None = None,
    final_image_stage_root: Path | None = None,
    final_image_backend: str | None = None,
) -> dict[str, object]:
    if timeout_seconds < 1:
        raise BuildError("TIMEOUT_INVALID", "--timeout-seconds must be at least 1", 2)
    if build_target not in {"Build", "Rebuild"}:
        raise BuildError("NATIVE_PRODUCT_BUILD_TARGET", f"unsupported observation build target: {build_target}", 2)
    component = graph.components.get(product_id)
    if component is None:
        raise BuildError("NATIVE_PRODUCT_UNKNOWN", f"unknown product: {product_id}", 2)
    context = graph.context(context_id)
    targets = component.backend_targets.get("msbuild", ())
    if context.backend != "msbuild" or len(targets) != 1:
        raise BuildError("NATIVE_PRODUCT_TARGET", f"{product_id} must name one MSBuild product target in {context_id}", 2)
    argv = _native_observation_command(
        msbuild_command(
            graph.repo_root,
            graph.repo_root / targets[0],
            context.platform,
            context.configuration,
            jobs,
            build_target=build_target,
        ),
        build_target,
    )
    log_root = graph.repo_root / "build/evidence/r0/.native-product-logs"
    log_root.mkdir(parents=True, exist_ok=True)
    diagnostic_log = log_root / f"{os.getpid()}-{uuid.uuid4().hex}.log"
    argv.extend([
        "/p:SakuraNativeProductMapEvidence=true",
        "/v:minimal",
        "/fl",
        f"/flp:logfile={diagnostic_log};verbosity=diagnostic;encoding=UTF-8",
    ])
    events.emit(
        "native_product_observation_started",
        component_id=product_id,
        context_id=context_id,
        build_target=build_target,
        argv=argv,
    )
    try:
        process = subprocess.Popen(
            argv,
            cwd=graph.repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
        )
    except OSError as error:
        diagnostic_log.unlink(missing_ok=True)
        try:
            log_root.rmdir()
        except OSError:
            pass
        events.emit(
            "native_product_observation_finished",
            component_id=product_id,
            context_id=context_id,
            build_target=build_target,
            status="start_failed",
            exit_code=3,
        )
        raise BuildError("NATIVE_PRODUCT_BUILD_START_FAILED", f"could not start MSBuild: {error}", 3) from error
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            _terminate_process_tree(process)
            try:
                stdout, stderr = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
            events.emit(
                "native_product_observation_finished",
                component_id=product_id,
                context_id=context_id,
                build_target=build_target,
                status="timeout",
                exit_code=8,
            )
            raise BuildError("NATIVE_PRODUCT_BUILD_TIMEOUT", f"MSBuild exceeded {timeout_seconds}s", 8) from error
        events.emit(
            "native_product_observation_finished",
            component_id=product_id,
            context_id=context_id,
            build_target=build_target,
            status="success" if process.returncode == 0 else "failure",
            exit_code=process.returncode,
        )
        if process.returncode:
            lines = (stdout + "\n" + stderr).strip().splitlines()
            raise BuildError("NATIVE_PRODUCT_BUILD_FAILED", "\n".join(lines[-20:]) or f"MSBuild failed with {process.returncode}", 6)
        if not diagnostic_log.is_file():
            raise BuildError(
                "NATIVE_PRODUCT_DIAGNOSTIC_LOG_MISSING",
                "MSBuild succeeded without producing its unique diagnostic log",
                5,
            )
        result = collect_product_native_evidence(
            graph,
            product_id,
            context_id,
            build_observed=True,
            build_target=build_target,
            diagnostic_log_path=diagnostic_log,
            package_restore=package_restore,
        )
        if final_image_stage_root is not None or final_image_backend is not None:
            if final_image_stage_root is None or final_image_backend not in {"cpp", "rust"}:
                raise BuildError(
                    "NATIVE_PRODUCT_FINAL_IMAGE_ARGUMENT",
                    "--final-image-stage-root and --final-image-backend must be supplied together",
                    2,
                )
            link = result.get("link")
            if not isinstance(link, dict):
                raise BuildError("NATIVE_PRODUCT_FINAL_IMAGE_INPUT", "native link observation is missing", 5)
            provider_validation = validate_output_provider_evidence_for_final_image(
                link,
                graph=graph,
                expected_backend=final_image_backend,
            )
            if not provider_validation["valid"]:
                raise BuildError(
                    "NATIVE_PRODUCT_FINAL_IMAGE_PROVIDER_UNPROVEN",
                    "native output-provider MAP evidence is not qualified for final-image staging",
                    5,
                )
            output = link.get("output")
            provider_member = link.get("output_provider_member_evidence")
            map_value = provider_member.get("map") if isinstance(provider_member, dict) else None
            if not isinstance(output, str) or not isinstance(map_value, str):
                raise BuildError(
                    "NATIVE_PRODUCT_FINAL_IMAGE_INPUT",
                    "native observation does not identify both the final image and MAP",
                    5,
                )
            from .output_final_image_evidence import bind_native_evidence_to_final_image, stage_output_final_image

            receipt = stage_output_final_image(
                repo_root=graph.repo_root,
                stage_root=final_image_stage_root,
                backend=final_image_backend,
                platform=context.platform,
                configuration=context.configuration,
                source_native_evidence_sha256=str(result["hard_evidence_hash"]),
                executable_path=graph.repo_root / output,
                map_path=graph.repo_root / map_value,
                native_evidence=result,
            )
            result = bind_native_evidence_to_final_image(result, receipt)
        return result
    finally:
        diagnostic_log.unlink(missing_ok=True)
        try:
            log_root.rmdir()
        except OSError:
            pass


def write_product_native_evidence(path: Path, evidence: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _validate_output_provider_map_reference(
    graph: SemanticGraph,
    value: object,
    label: str,
    failures: list[dict[str, object]],
) -> tuple[str | None, int | None]:
    if not isinstance(value, dict):
        return None, None
    observed = value.get("observed") is True
    strict = value.get("method") == "msvc_map_publics_by_value_provider_rows"
    map_relative = value.get("map")
    map_hash = value.get("map_hash")
    map_size = value.get("map_size_bytes")
    metadata_required = observed or strict
    if metadata_required and (
        not isinstance(map_relative, str)
        or not map_relative
        or not isinstance(map_hash, str)
        or not isinstance(map_size, int)
        or isinstance(map_size, bool)
        or map_size < 0
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_METADATA_MISSING", "field": label})
    if map_relative is None:
        return None, None
    if not isinstance(map_relative, str) or not map_relative:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_PATH_MISSING", "field": label})
        return None, None
    if _unsafe_relative_reference(map_relative):
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_PATH_ESCAPE",
            "field": label,
            "path": map_relative,
        })
        return None, None
    try:
        map_path = safe_repository_path(
            graph.repo_root,
            map_relative,
            code="NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_PATH_UNSAFE",
            reject_parent_segments=True,
        )
    except RepositoryPathSafetyError:
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_PATH_UNSAFE",
            "field": label,
            "path": map_relative,
        })
        return None, None
    if not map_path.is_file():
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_MISSING",
            "field": label,
            "path": map_relative,
        })
        return None, None
    try:
        actual_hash = _sha256_file(map_path, "NATIVE_PRODUCT_EVIDENCE_MAP_VALIDATE")
        actual_size = map_path.stat().st_size
    except OSError as error:
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_READ",
            "field": label,
            "detail": str(error),
        })
        return None, None
    if isinstance(map_hash, str) and map_hash != actual_hash:
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_MAP_CHANGED",
            "field": label,
            "path": map_relative,
        })
    if isinstance(map_size, int) and not isinstance(map_size, bool) and map_size != actual_size:
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_MAP_CHANGED",
            "field": label,
            "path": map_relative,
        })
    return map_relative, actual_size


def _provider_contribution_rows(value: object) -> list[dict[str, object]] | None:
    if not isinstance(value, list):
        return None
    rows: list[dict[str, object]] = []
    for row in value:
        if not isinstance(row, dict):
            return None
        symbol = row.get("symbol")
        archive = row.get("archive")
        member = row.get("member")
        if not isinstance(symbol, str) or not symbol:
            return None
        if archive is not None and (not isinstance(archive, str) or not archive):
            return None
        if member is not None and (not isinstance(member, str) or not member):
            return None
        rows.append({"symbol": symbol, "archive": archive, "member": member})
    return rows


def _validate_output_provider_schema(
    graph: SemanticGraph,
    link: Mapping[str, object],
    failures: list[dict[str, object]],
) -> None:
    member = link.get("output_provider_member_evidence")
    symbols = link.get("output_provider_symbol_evidence")
    if member is None and symbols is None:
        return
    if not isinstance(member, dict) or not isinstance(symbols, dict):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})
        return

    member_map, _member_map_size = _validate_output_provider_map_reference(
        graph, member, "member", failures
    )
    symbol_map, _symbol_map_size = _validate_output_provider_map_reference(
        graph, symbols, "symbol", failures
    )
    if member_map is not None and symbol_map is not None and member_map != symbol_map:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_MISMATCH"})

    member_method = member.get("method")
    symbol_method = symbols.get("method")
    member_strict = member_method == "msvc_map_publics_by_value_provider_rows"
    symbol_strict = symbol_method == "msvc_map_publics_by_value_provider_rows"
    if member_method is not None and not member_strict:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if symbol_method is not None and not symbol_strict:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    if member_strict != symbol_strict:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})

    member_observed = member.get("observed")
    symbol_observed = symbols.get("observed")
    if not isinstance(member_observed, bool):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if not isinstance(symbol_observed, bool):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    if isinstance(member_observed, bool) and isinstance(symbol_observed, bool) and member_observed != symbol_observed:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})

    member_scope = member.get("provider")
    if not isinstance(member_scope, str) or member_scope.strip().lower() not in {
        "provider", "output-provider", "output_provider",
    }:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    symbol_scope = symbols.get("scope")
    if not isinstance(symbol_scope, str) or symbol_scope.strip().lower() not in {
        "provider", "output-provider", "output_provider",
    }:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})

    members = member.get("members")
    member_count = member.get("member_count")
    if (
        not isinstance(members, list)
        or any(not isinstance(item, str) or not item.strip() for item in members)
        or not isinstance(member_count, int)
        or isinstance(member_count, bool)
        or member_count != len(members)
        or len({item.strip().replace("\\", "/") for item in members}) != len(members)
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    member_duplicate_count = member.get("duplicate_count")
    if (
        not isinstance(member_duplicate_count, int)
        or isinstance(member_duplicate_count, bool)
        or member_duplicate_count < 0
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    archive_input_count = member.get("archive_input_count")
    if (
        not isinstance(archive_input_count, int)
        or isinstance(archive_input_count, bool)
        or archive_input_count < 0
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})

    provider_symbols = symbols.get("symbols")
    symbol_count = symbols.get("symbol_count")
    if (
        not isinstance(provider_symbols, list)
        or any(not isinstance(item, str) or not item for item in provider_symbols)
        or not isinstance(symbol_count, int)
        or isinstance(symbol_count, bool)
        or symbol_count != len(provider_symbols)
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    symbol_duplicate_count = symbols.get("duplicate_count")
    if (
        not isinstance(symbol_duplicate_count, int)
        or isinstance(symbol_duplicate_count, bool)
        or symbol_duplicate_count < 0
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})

    member_contributions = _provider_contribution_rows(member.get("contributions"))
    symbol_contributions = _provider_contribution_rows(symbols.get("contributions"))
    if member_strict and member_contributions is None:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if symbol_strict and symbol_contributions is None:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    if member_strict and symbol_strict and member_contributions != symbol_contributions:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})

    if member_strict and symbol_strict and isinstance(member_map, str) and member_map == symbol_map:
        map_path = (graph.repo_root / member_map).resolve()
        if map_path.is_file():
            try:
                current_map_hash = _sha256_file(map_path, "NATIVE_PRODUCT_EVIDENCE_MAP_VALIDATE")
                repository_libraries = link.get("repository_libraries")
                libraries = repository_libraries if isinstance(repository_libraries, list) else []
                derived_member, derived_symbols = _output_provider_map_evidence(
                    map_path,
                    member_map,
                    current_map_hash,
                    [value for value in libraries if isinstance(value, str)],
                )
            except BuildError as error:
                failures.append({
                    "code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_READ",
                    "detail": str(error),
                })
            else:
                if derived_member != member or derived_symbols != symbols:
                    failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_CONTENT_MISMATCH"})

    if (
        member_strict
        and symbol_strict
        and member_observed is False
        and isinstance(archive_input_count, int)
        and not isinstance(archive_input_count, bool)
        and archive_input_count > 0
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})

    if not (member_strict and symbol_strict and member_observed is True and symbol_observed is True):
        return

    if (
        not isinstance(provider_symbols, list)
        or any(not isinstance(item, str) or not item for item in provider_symbols)
        or set(provider_symbols) != _OUTPUT_PROVIDER_SYMBOL_SET
        or len(provider_symbols) != len(_OUTPUT_PROVIDER_SYMBOL_SET)
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    if member_duplicate_count != 0 or symbol_duplicate_count != 0:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})
    if member.get("missing_symbols") != [] or member.get("unexpected_symbols") != []:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if symbols.get("missing_symbols") != [] or symbols.get("unexpected_symbols") != []:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
    if member.get("archive_name") != _OUTPUT_PROVIDER_ARCHIVE_NAME:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if member.get("contributing_archives") != [_OUTPUT_PROVIDER_ARCHIVE_NAME]:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if member.get("contributing_archive_count") != 1:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if not isinstance(member_contributions, list) or len(member_contributions) != len(_OUTPUT_PROVIDER_SYMBOLS):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    else:
        contribution_symbols = [row["symbol"] for row in member_contributions]
        if set(contribution_symbols) != _OUTPUT_PROVIDER_SYMBOL_SET or len(contribution_symbols) != len(_OUTPUT_PROVIDER_SYMBOL_SET):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
        if any(
            row["archive"] != _OUTPUT_PROVIDER_ARCHIVE_NAME
            or not isinstance(row["member"], str)
            or not row["member"]
            for row in member_contributions
        ):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
    if member.get("contributing_members") != members:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})


def _validate_final_image_link_contract(
    graph: SemanticGraph | None,
    link: Mapping[str, object],
    failures: list[dict[str, object]],
    expected_backend: str | None,
) -> None:
    """Validate the strict native link contract used by final-image staging.

    This deliberately does not call :func:`_validate_output_provider_schema`.
    That helper is the compatibility validator for ordinary native inventory,
    where provider MAP evidence is optional and older records may omit the
    newer identity fields.  Final-image staging has a separate, closed
    contract whose three MAP records and raw archive count must all agree.
    """

    def _canonical_map_identity(value: object, label: str) -> tuple[str, str, int] | None:
        if not isinstance(value, Mapping):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_SCHEMA", "field": label})
            return None
        map_reference = value.get("map")
        map_hash = _normalise_sha256(value.get("map_hash"))
        map_size = value.get("map_size_bytes")
        if (
            not isinstance(map_reference, str)
            or not map_reference
            or _unsafe_relative_reference(map_reference)
            or map_hash is None
            or not isinstance(map_size, int)
            or isinstance(map_size, bool)
            or map_size < 0
        ):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_SCHEMA", "field": label})
            return None
        return map_reference.replace("\\", "/").casefold(), map_hash, map_size

    def _strict_int(value: object) -> bool:
        return isinstance(value, int) and not isinstance(value, bool) and value >= 0

    # The selected-member projection is required independently of the two
    # provider projections.  In particular, do not infer this boolean from a
    # present MAP record: a stale or hand-authored record must fail closed.
    if link.get("selected_archive_members_observed") is not True:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})
    selected = link.get("selected_archive_member_evidence")
    member = link.get("output_provider_member_evidence")
    symbols = link.get("output_provider_symbol_evidence")
    selected_identity = _canonical_map_identity(selected, "selected")
    member_identity = _canonical_map_identity(member, "member")
    symbol_identity = _canonical_map_identity(symbols, "symbol")
    identities = [identity for identity in (selected_identity, member_identity, symbol_identity) if identity is not None]
    if len(identities) != 3:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})
    elif len(set(identities)) != 1:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_MISMATCH"})

    if expected_backend not in (None, "cpp", "rust"):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})

    if not isinstance(member, Mapping) or not isinstance(symbols, Mapping):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})
    else:
        member_observed = member.get("observed")
        symbol_observed = symbols.get("observed")
        negative_cpp = (
            expected_backend == "cpp"
            and member_observed is False
            and symbol_observed is False
        )
        if member_observed is not True and not negative_cpp:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})
        if symbol_observed is not True and not negative_cpp:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN"})
        if member.get("method") != "msvc_map_publics_by_value_provider_rows":
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
        if symbols.get("method") != "msvc_map_publics_by_value_provider_rows":
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
        if member.get("provider") not in {"provider", "output-provider", "output_provider"}:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
        if symbols.get("provider") not in {"provider", "output-provider", "output_provider"}:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
        if symbols.get("scope") not in {"provider", "output-provider", "output_provider"}:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})

        provider_symbols = symbols.get("symbols")
        symbol_count = symbols.get("symbol_count")
        members = member.get("members")
        member_count = member.get("member_count")
        member_names = {
            item.replace("\\", "/").casefold()
            for item in members
            if isinstance(item, str)
        } if isinstance(members, list) else set()
        archive_input_count = member.get("archive_input_count")
        member_contributions = _provider_contribution_rows(member.get("contributions"))
        symbol_contributions = _provider_contribution_rows(symbols.get("contributions"))

        if negative_cpp:
            expected_missing = sorted(_OUTPUT_PROVIDER_SYMBOL_SET, key=str.casefold)
            # An LTCG C++ image may legitimately discard the unused Rust
            # provider exports.  Accept that only as a complete negative
            # projection: all provider rows, members, and contributions are
            # empty, while the one archive input and the three MAP identities
            # remain bound.  A MAP by itself is not selector proof; callers
            # must pair this shape with the backend's compile-selector proof.
            if (
                provider_symbols != []
                or not _strict_int(symbol_count)
                or symbol_count != 0
                or members != []
                or not _strict_int(member_count)
                or member_count != 0
                or member.get("contributing_members") != []
                or symbols.get("contributing_members") != []
                or member_contributions != []
                or symbol_contributions != []
                or member.get("contributing_archives") != []
                or symbols.get("contributing_archives") != []
                or not _strict_int(member.get("contributing_archive_count"))
                or member.get("contributing_archive_count") != 0
                or "archive_name" not in member
                or member.get("archive_name") is not None
                or (
                    "archive_name" in symbols
                    and symbols.get("archive_name") is not None
                )
                or (
                    "contributing_archive_count" in symbols
                    and (
                        not _strict_int(symbols.get("contributing_archive_count"))
                        or symbols.get("contributing_archive_count") != 0
                    )
                )
                or not _strict_int(archive_input_count)
                or archive_input_count != 1
                or member.get("missing_symbols") != expected_missing
                or symbols.get("missing_symbols") != expected_missing
                or member.get("unexpected_symbols") != []
                or symbols.get("unexpected_symbols") != []
                or not _strict_int(member.get("duplicate_count"))
                or member.get("duplicate_count") != 0
                or not _strict_int(symbols.get("duplicate_count"))
                or symbols.get("duplicate_count") != 0
            ):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"})
        else:
            if (
                not isinstance(provider_symbols, list)
                or any(not isinstance(symbol, str) or not symbol for symbol in provider_symbols)
                or not _strict_int(symbol_count)
                or symbol_count != len(provider_symbols)
                or symbol_count != len(_OUTPUT_PROVIDER_SYMBOLS)
                or set(provider_symbols) != _OUTPUT_PROVIDER_SYMBOL_SET
            ):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SYMBOL_SCHEMA"})
            for evidence, label in ((member, "member"), (symbols, "symbol")):
                duplicate_count = evidence.get("duplicate_count")
                if not _strict_int(duplicate_count) or duplicate_count != 0:
                    failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA", "field": label})
                if evidence.get("missing_symbols") != [] or evidence.get("unexpected_symbols") != []:
                    failures.append({"code": f"NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_{label.upper()}_SCHEMA"})

            if (
                not isinstance(members, list)
                or not members
                or len(members) != 1
                or any(not isinstance(item, str) or not item for item in members)
                or len(set(item.replace("\\", "/").casefold() for item in members)) != len(members)
                or not _strict_int(member_count)
                or member_count != 1
                or member.get("contributing_members") != members
                or symbols.get("contributing_members") != members
            ):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})
            if not _strict_int(archive_input_count) or archive_input_count != 1:
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})

            if (
                member.get("archive_name") != _OUTPUT_PROVIDER_ARCHIVE_NAME
                or member.get("contributing_archives") != [_OUTPUT_PROVIDER_ARCHIVE_NAME]
                or not _strict_int(member.get("contributing_archive_count"))
                or member.get("contributing_archive_count") != 1
                or symbols.get("contributing_archives") != [_OUTPUT_PROVIDER_ARCHIVE_NAME]
                or (
                    "contributing_archive_count" in symbols
                    and (
                        not _strict_int(symbols.get("contributing_archive_count"))
                        or symbols.get("contributing_archive_count") != 1
                    )
                )
            ):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})

            if (
                member_contributions is None
                or symbol_contributions is None
                or member_contributions != symbol_contributions
                or len(member_contributions) != len(_OUTPUT_PROVIDER_SYMBOLS)
                or {row["symbol"] for row in member_contributions} != _OUTPUT_PROVIDER_SYMBOL_SET
                or any(
                    row["archive"] != _OUTPUT_PROVIDER_ARCHIVE_NAME
                    or not isinstance(row["member"], str)
                    or not row["member"]
                    or row["member"].replace("\\", "/").casefold() not in member_names
                    for row in member_contributions or []
                )
            ):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MEMBER_SCHEMA"})

    output_reference = link.get("output")
    output_hash = _normalise_sha256(link.get("product_hash"))
    output_size = link.get("product_size_bytes")
    if (
        not isinstance(output_reference, str)
        or not output_reference
        or _unsafe_relative_reference(output_reference)
        or Path(output_reference).name.casefold() != "sakura.exe"
        or output_hash is None
        or not _strict_int(output_size)
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_IDENTITY_SCHEMA"})
    link_command_hash = _normalise_sha256(link.get("linkCommandSha256"))
    if link_command_hash is None:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_LINK_COMMAND_SCHEMA"})

    archive_hash = _normalise_sha256(link.get("rustArchiveSha256"))
    archive_size = link.get("rustArchiveSizeBytes")
    archive_count = link.get("rustArchiveCount")
    if archive_hash is None or not _strict_int(archive_size) or not _strict_int(archive_count) or archive_count != 1:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_SCHEMA"})
    if isinstance(member, Mapping):
        member_archive_count = member.get("archive_input_count")
        if not _strict_int(member_archive_count) or member_archive_count != archive_count:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_SCHEMA"})

    # A repository-library list is a deduplicated convenience projection, so it
    # cannot replace the raw top-level count.  When present, however, it must
    # not introduce a second provider archive identity.
    libraries = link.get("repository_libraries")
    archive_paths = [
        item
        for item in libraries
        if isinstance(item, str)
        and Path(item.replace("\\", "/")).name.casefold() == _OUTPUT_PROVIDER_ARCHIVE_NAME
    ] if isinstance(libraries, list) else []
    if isinstance(libraries, list) and len(archive_paths) != 1:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_UNPROVEN"})

    if graph is None:
        return

    # Rehash the source MAP when the graph is available.  This also binds the
    # selected-member projection, which ordinary validation intentionally does
    # not require to be provider-scoped.
    map_path_value = selected.get("map") if isinstance(selected, Mapping) else None
    if not isinstance(map_path_value, str) or not map_path_value or _unsafe_relative_reference(map_path_value):
        return
    try:
        map_path = safe_repository_path(
            graph.repo_root,
            map_path_value,
            code="NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_PATH_UNSAFE",
            reject_parent_segments=True,
        )
        actual_map_hash, actual_map_size = _file_identity(
            map_path,
            "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_READ",
        )
    except (BuildError, RepositoryPathSafetyError):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_UNPROVEN"})
    else:
        actual_map_hash = _normalise_sha256(actual_map_hash)
        expected_identity = selected_identity
        if (
            expected_identity is None
            or actual_map_hash != expected_identity[1]
            or actual_map_size != expected_identity[2]
        ):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_MISMATCH"})

    # Rehash the source archive whenever graph-backed validation is requested;
    # without a path in repository_libraries there is no trustworthy source
    # identity to read, so the final-image proof remains unproven.
    if len(archive_paths) != 1:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_UNPROVEN"})
        return
    try:
        archive_path = safe_repository_path(
            graph.repo_root,
            archive_paths[0],
            code="NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_PATH_UNSAFE",
            reject_parent_segments=True,
        )
        actual_archive_hash, actual_archive_size = _file_identity(
            archive_path,
            "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_READ",
        )
    except (BuildError, RepositoryPathSafetyError):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_UNPROVEN"})
        return
    if (
        _normalise_sha256(actual_archive_hash) != archive_hash
        or actual_archive_size != archive_size
    ):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_MISMATCH"})


def validate_output_provider_evidence_for_final_image(
    link: Mapping[str, object],
    *,
    graph: SemanticGraph | None = None,
    expected_backend: str | None = None,
) -> dict[str, object]:
    """Validate the strict provider MAP contract required for final-image staging.

    Ordinary native inventory validation intentionally accepts a missing
    provider projection because a MAP is optional there.  Final-image staging
    has a stronger contract: Rust (and the default ``None`` mode) requires
    both provider projections to be observed and to prove the exact provider
    v1 symbol/member set.  A C++ final image may use the explicit negative
    projection emitted by LTCG when those unused Rust exports are discarded;
    that shape still requires one archive input, empty provider rows, and all
    three MAP identities.  It is not selector proof by itself: callers must
    pair it with the backend-specific compile-selector proof.  ``graph`` is
    supplied by the collector path so the source MAP can be re-hashed; binding
    callers can omit it because the staged receipt supplies the immutable MAP
    identity.
    """

    if not isinstance(link, Mapping):
        return {
            "valid": False,
            "failures": [{"code": "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_SCHEMA"}],
        }
    failures: list[dict[str, object]] = []
    _validate_final_image_link_contract(graph, link, failures, expected_backend)
    if failures:
        return {"valid": False, "failures": failures}
    return {"valid": True, "failures": []}


def validate_product_native_evidence(
    graph: SemanticGraph,
    path: Path | None,
    product_id: str,
    context_id: str,
) -> dict[str, object]:
    if path is None:
        return {
            "status": "not_provided",
            "valid": False,
            "evidence_path": None,
            "failures": [{"code": "NATIVE_PRODUCT_EVIDENCE_NOT_PROVIDED"}],
        }
    try:
        relative_path = path.resolve().relative_to(graph.repo_root.resolve()).as_posix()
    except ValueError:
        relative_path = None
    if not path.is_file():
        return {
            "status": "missing",
            "valid": False,
            "evidence_path": relative_path,
            "failures": [{"code": "NATIVE_PRODUCT_EVIDENCE_MISSING"}],
        }
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError("NATIVE_PRODUCT_EVIDENCE_PARSE", f"could not parse {path}: {error}", 5) from error
    if not isinstance(evidence, dict) or evidence.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        raise BuildError("NATIVE_PRODUCT_EVIDENCE_SCHEMA", f"unsupported evidence schema in {path}", 5)

    failures: list[dict[str, object]] = []
    expected = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
        "backend": "msbuild",
    }
    for field, value in expected.items():
        if evidence.get(field) != value:
            failures.append({
                "code": f"NATIVE_PRODUCT_EVIDENCE_{field.upper()}_MISMATCH",
                "expected": value,
                "actual": evidence.get(field),
            })
    if evidence.get("build_observed") is not True:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_BUILD_NOT_OBSERVED"})
    freshness = evidence.get("freshness")
    if not isinstance(freshness, dict):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_FRESHNESS_MISSING"})
        freshness = {}
    for group in ("source_inputs", "generated_inputs", "definition_inputs", "tracker_inputs"):
        hashes = freshness.get(group)
        if not isinstance(hashes, dict):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_HASH_GROUP_MISSING", "group": group})
            continue
        for relative, expected_hash in sorted(hashes.items(), key=lambda item: str(item[0])):
            if _unsafe_relative_reference(relative):
                failures.append({
                    "code": "NATIVE_PRODUCT_EVIDENCE_INPUT_PATH_UNSAFE",
                    "group": group,
                    "path": relative,
                })
                continue
            try:
                input_path = safe_repository_path(
                    graph.repo_root,
                    relative,
                    code="NATIVE_PRODUCT_EVIDENCE_INPUT_PATH_UNSAFE",
                    reject_parent_segments=True,
                )
            except RepositoryPathSafetyError:
                failures.append({
                    "code": "NATIVE_PRODUCT_EVIDENCE_INPUT_PATH_UNSAFE",
                    "group": group,
                    "path": relative,
                })
                continue
            if not input_path.is_file():
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_INPUT_MISSING", "group": group, "path": relative})
                continue
            actual_hash = _sha256_file(input_path, "NATIVE_PRODUCT_EVIDENCE_VALIDATE")
            if actual_hash != expected_hash:
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_INPUT_CHANGED", "group": group, "path": relative})

    if evidence.get("hard_evidence_hash") != product_native_evidence_hash(evidence):
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_HASH_MISMATCH"})
    compiler = evidence.get("compiler") if isinstance(evidence.get("compiler"), dict) else {}
    resource = evidence.get("resource_compiler") if isinstance(evidence.get("resource_compiler"), dict) else {}
    generator = evidence.get("generator_observation") if isinstance(evidence.get("generator_observation"), dict) else {}
    link = evidence.get("link") if isinstance(evidence.get("link"), dict) else {}
    package = evidence.get("package_restore") if isinstance(evidence.get("package_restore"), dict) else {}
    package_required = bool(_declared_package_set_ids(graph, product_id, context_id))
    if bool(package.get("required")) != package_required:
        failures.append({
            "code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_REQUIREMENT_MISMATCH",
            "expected": package_required,
            "actual": package.get("required"),
        })
    if package_required:
        try:
            current_package = validate_package_restore(graph, (product_id,), context_id)
        except BuildError as error:
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_VALIDATE_FAILED", "detail": str(error)})
            current_package = {"valid": False}
        if not current_package.get("valid"):
            failures.append({
                "code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_CACHE_INVALID",
                "failures": current_package.get("failures", []),
            })
        elif package.get("plan_hash") != current_package.get("plan_hash"):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_PLAN_MISMATCH"})
        elif not package.get("package_closure_validated"):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_CLOSURE_UNVALIDATED"})
        elif not package.get("native_restore_execution_observed"):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PACKAGE_RESTORE_UNOBSERVED"})
    if evidence.get("build_target") not in {"Build", "Rebuild"}:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_BUILD_TARGET_INVALID"})
    product_relative = link.get("output")
    if not isinstance(product_relative, str) or not product_relative:
        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_PATH_MISSING"})
    else:
        if _unsafe_relative_reference(product_relative):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_PATH_ESCAPE", "path": product_relative})
        else:
            try:
                product_path = safe_repository_path(
                    graph.repo_root,
                    product_relative,
                    code="NATIVE_PRODUCT_EVIDENCE_PRODUCT_PATH_UNSAFE",
                    reject_parent_segments=True,
                )
            except RepositoryPathSafetyError:
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_PATH_UNSAFE", "path": product_relative})
                product_path = None
            if product_path is None:
                pass
            elif not product_path.is_file():
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_MISSING", "path": product_relative})
            elif not isinstance(link.get("product_hash"), str):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_HASH_MISSING"})
            elif _sha256_file(product_path, "NATIVE_PRODUCT_EVIDENCE_PRODUCT_VALIDATE") != link.get("product_hash"):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_PRODUCT_CHANGED", "path": product_relative})
    selected_evidence = link.get("selected_archive_member_evidence")
    if link.get("selected_archive_members_observed"):
        if not isinstance(selected_evidence, dict):
            failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_METADATA_MISSING"})
        else:
            map_relative = selected_evidence.get("map")
            map_hash = selected_evidence.get("map_hash")
            if not isinstance(map_relative, str) or not map_relative:
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_PATH_MISSING"})
            elif _unsafe_relative_reference(map_relative):
                failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_PATH_ESCAPE", "path": map_relative})
            else:
                try:
                    map_path = safe_repository_path(
                        graph.repo_root,
                        map_relative,
                        code="NATIVE_PRODUCT_EVIDENCE_MAP_PATH_UNSAFE",
                        reject_parent_segments=True,
                    )
                except RepositoryPathSafetyError:
                    failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_PATH_UNSAFE", "path": map_relative})
                else:
                    if not map_path.is_file():
                        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_MISSING", "path": map_relative})
                    elif not isinstance(map_hash, str):
                        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_HASH_MISSING"})
                    elif _sha256_file(map_path, "NATIVE_PRODUCT_EVIDENCE_MAP_VALIDATE") != map_hash:
                        failures.append({"code": "NATIVE_PRODUCT_EVIDENCE_MAP_CHANGED", "path": map_relative})
    _validate_output_provider_schema(graph, link, failures)
    valid = not failures
    return {
        "status": "observed" if valid else "stale_or_mismatched",
        "valid": valid,
        "evidence_path": relative_path,
        "hard_evidence_hash": evidence.get("hard_evidence_hash"),
        "failures": failures,
        "coverage": {
            "compiler_dependency_observed": valid and bool(compiler.get("dependency_trace_observed")),
            "pch_usage_observed": valid and bool(compiler.get("pch_usage_observed")),
            "generated_input_consumption_observed": valid and bool(compiler.get("generated_inputs")),
            "generator_target_scheduling_observed": valid and bool(generator.get("target_scheduling_observed")),
            "generator_execution_observed": valid and bool(generator.get("correlated_execution_observed")),
            "generated_producer_consumer_correlation_observed": valid and bool(generator.get("producer_consumer_correlations")),
            "resource_compiler_inputs_observed": valid and bool(resource.get("input_trace_observed")),
            "resource_table_observed": valid and bool(resource.get("resource_table_observed")),
            "link_input_set_observed": valid and bool(link.get("input_set_observed")),
            "selected_archive_members_observed": valid and bool(link.get("selected_archive_members_observed")),
            "output_provider_member_evidence_observed": valid and isinstance(
                link.get("output_provider_member_evidence"), dict
            ) and link["output_provider_member_evidence"].get("observed") is True,
            "output_provider_symbol_evidence_observed": valid and isinstance(
                link.get("output_provider_symbol_evidence"), dict
            ) and link["output_provider_symbol_evidence"].get("observed") is True,
            "package_restore_execution_observed": valid and bool(package.get("native_restore_execution_observed")),
            "package_closure_validated": valid and bool(package.get("package_closure_validated")),
        },
        "counts": {
            "translation_units": int(compiler.get("translation_unit_count", 0)),
            "compiler_source_inputs": len(compiler.get("source_inputs", [])),
            "generated_header_inputs": len(compiler.get("generated_header_inputs", [])),
            "resource_units": int(resource.get("unit_count", 0)),
            "link_objects": len(link.get("object_inputs", [])),
            "link_resources": len(link.get("resource_inputs", [])),
            "repository_libraries": len(link.get("repository_libraries", [])),
            "external_libraries": len(link.get("resolved_external_libraries", [])),
            "generator_targets": len(generator.get("target_results", [])),
            "generated_producer_consumer_correlations": len(generator.get("producer_consumer_correlations", [])),
        },
        "translation_units": compiler.get("translation_units", []) if valid else [],
        "link": {
            "command_libraries": link.get("command_libraries", []) if valid else [],
            "repository_libraries": link.get("repository_libraries", []) if valid else [],
            "object_inputs": link.get("object_inputs", []) if valid else [],
            "resource_inputs": link.get("resource_inputs", []) if valid else [],
            "product_hash": link.get("product_hash") if valid else None,
            "selected_archive_member_evidence": link.get("selected_archive_member_evidence") if valid else None,
            "output_provider_member_evidence": link.get("output_provider_member_evidence") if valid else None,
            "output_provider_symbol_evidence": link.get("output_provider_symbol_evidence") if valid else None,
        },
        "generator_observation": {
            "build_target": evidence.get("build_target") if valid else None,
            "target_results": generator.get("target_results", []) if valid else [],
            "producer_consumer_correlations": generator.get("producer_consumer_correlations", []) if valid else [],
        },
    }
