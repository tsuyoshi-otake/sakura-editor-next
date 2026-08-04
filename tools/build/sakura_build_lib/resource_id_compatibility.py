"""Canonical and compiled numeric Resource ID compatibility contracts."""

from __future__ import annotations

import hashlib
import json
import os
import re
import struct
from collections import Counter
from pathlib import Path
from typing import Callable, Mapping, Sequence

from .runner import BuildError


BASELINE_SCHEMA_VERSION = 1
DEFAULT_COMPATIBILITY_VERSION = 1
_MAX_NESTED_ITEMS = 100_000
_MAX_NESTING_DEPTH = 64
_DS_SETFONT = 0x00000040
_MF_POPUP = 0x0010
_MF_END = 0x0080
_MENUEX_POPUP = 0x0001
_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(-?(?:0[xX][0-9A-Fa-f]+|[0-9]+))\s*(?://.*)?$"
)
_DEFINE_START_RE = re.compile(r"^\s*#\s*define\b")
_IDENTIFIER_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _sha256_json(value: object) -> str:
    serialized = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return _sha256_bytes(serialized.encode("utf-8"))


def _read_bytes(path: Path, code: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise BuildError(code, f"could not read {path}: {error}", 5) from error


def _decode_text(data: bytes) -> str:
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    return data.decode("utf-8-sig")


def _inside_repository(repo_root: Path, path: Path, code: str) -> tuple[Path, str]:
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError as error:
        raise BuildError(code, f"path escapes repository: {path}", 5) from error
    return resolved, relative


def parse_resource_header(repo_root: Path, path: Path) -> dict[str, object]:
    resolved, relative = _inside_repository(
        repo_root,
        path,
        "RESOURCE_ID_HEADER_PATH_ESCAPE",
    )
    try:
        text = _decode_text(_read_bytes(resolved, "RESOURCE_ID_HEADER_READ"))
    except UnicodeError as error:
        raise BuildError(
            "RESOURCE_ID_HEADER_ENCODING",
            f"could not decode {relative}: {error}",
            5,
        ) from error

    definitions: dict[str, int] = {}
    names: set[str] = set()
    unparsed: list[dict[str, object]] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        match = _DEFINE_RE.match(line)
        if match is None:
            if _DEFINE_START_RE.match(line):
                unparsed.append({"line": line_number, "text": line.strip()})
            continue
        name, raw_value = match.groups()
        if name in names:
            raise BuildError(
                "RESOURCE_ID_HEADER_DUPLICATE_NAME",
                f"duplicate resource definition {name} in {relative}:{line_number}",
                5,
            )
        names.add(name)
        definitions[name] = int(raw_value, 0)
    if unparsed:
        raise BuildError(
            "RESOURCE_ID_HEADER_UNPARSED_DEFINE",
            f"{relative} has {len(unparsed)} non-numeric or malformed #define entries",
            5,
        )
    if not definitions:
        raise BuildError(
            "RESOURCE_ID_HEADER_EMPTY",
            f"no numeric resource definitions found in {relative}",
            5,
        )
    definitions = dict(sorted(definitions.items()))
    mapping_hash = _sha256_json(definitions)
    return {
        "path": relative,
        "definition_count": len(definitions),
        "definitions": definitions,
        "mapping_hash": mapping_hash,
    }


def _strip_comments_and_strings(text: str) -> str:
    """Remove RC comments and quoted text while preserving identifier boundaries."""

    result: list[str] = []
    index = 0
    state = "code"
    while index < len(text):
        current = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if current == "/" and following == "/":
                result.extend((" ", " "))
                index += 2
                state = "line_comment"
                continue
            if current == "/" and following == "*":
                result.extend((" ", " "))
                index += 2
                state = "block_comment"
                continue
            if current == '"':
                result.append(" ")
                index += 1
                state = "string"
                continue
            result.append(current)
            index += 1
            continue
        if state == "line_comment":
            if current in "\r\n":
                result.append(current)
                state = "code"
            else:
                result.append(" ")
            index += 1
            continue
        if state == "block_comment":
            if current == "*" and following == "/":
                result.extend((" ", " "))
                index += 2
                state = "code"
            else:
                result.append(current if current in "\r\n" else " ")
                index += 1
            continue
        if current == "\\" and following:
            result.extend((" ", " "))
            index += 2
            continue
        if current == '"' and following == '"':
            result.extend((" ", " "))
            index += 2
            continue
        if current == '"':
            result.append(" ")
            index += 1
            state = "code"
            continue
        result.append(current if current in "\r\n" else " ")
        index += 1
    return "".join(result)


def collect_resource_source_contract(
    repo_root: Path,
    role: str,
    paths: Sequence[Path],
    definitions: Mapping[str, int],
) -> dict[str, object]:
    if not role:
        raise BuildError("RESOURCE_ID_SOURCE_ROLE_EMPTY", "resource source role is empty", 5)
    usage: Counter[str] = Counter()
    source_paths: list[str] = []
    for path in paths:
        resolved, relative = _inside_repository(
            repo_root,
            path,
            "RESOURCE_ID_SOURCE_PATH_ESCAPE",
        )
        try:
            text = _decode_text(_read_bytes(resolved, "RESOURCE_ID_SOURCE_READ"))
        except UnicodeError as error:
            raise BuildError(
                "RESOURCE_ID_SOURCE_ENCODING",
                f"could not decode {relative}: {error}",
                5,
            ) from error
        source_paths.append(relative)
        stripped = _strip_comments_and_strings(text)
        for token in _IDENTIFIER_RE.findall(stripped):
            if token in definitions:
                usage[token] += 1
    symbols = {name: usage[name] for name in sorted(usage)}
    stable = {
        "role": role,
        "paths": sorted(source_paths),
        "symbols": symbols,
        "symbol_count": len(symbols),
        "occurrence_count": sum(usage.values()),
    }
    return {**stable, "contract_hash": _sha256_json(stable)}


class _TemplateParseError(ValueError):
    pass


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    def _read(self, size: int) -> bytes:
        if size < 0 or self.position + size > len(self.data):
            raise _TemplateParseError(
                f"truncated resource at offset {self.position}, need {size} bytes"
            )
        value = self.data[self.position:self.position + size]
        self.position += size
        return value

    def u8(self) -> int:
        return self._read(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self._read(2))[0]

    def i16(self) -> int:
        return struct.unpack("<h", self._read(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self._read(4))[0]

    def peek_u16(self, offset: int = 0) -> int:
        position = self.position + offset
        if position < 0 or position + 2 > len(self.data):
            raise _TemplateParseError(f"truncated resource at offset {position}")
        return struct.unpack_from("<H", self.data, position)[0]

    def align(self, boundary: int) -> None:
        if boundary <= 0 or boundary & (boundary - 1):
            raise _TemplateParseError(f"invalid alignment {boundary}")
        aligned = (self.position + boundary - 1) & ~(boundary - 1)
        self._read(aligned - self.position)

    def skip(self, size: int) -> None:
        self._read(size)

    def utf16z(self) -> str:
        units: list[int] = []
        while True:
            value = self.u16()
            if value == 0:
                break
            units.append(value)
            if len(units) > _MAX_NESTED_ITEMS:
                raise _TemplateParseError("unterminated or oversized UTF-16 string")
        raw = b"".join(struct.pack("<H", value) for value in units)
        return raw.decode("utf-16-le", errors="strict")

    def ordinal_or_string(self) -> tuple[str, int | str | None]:
        first = self.u16()
        if first == 0:
            return ("none", None)
        if first == 0xFFFF:
            return ("id", self.u16())
        self.position -= 2
        return ("name", self.utf16z())


def _parse_dialog(data: bytes) -> dict[str, object]:
    reader = _Reader(data)
    extended = reader.peek_u16() == 1 and reader.peek_u16(2) == 0xFFFF
    menu_ids: list[int] = []
    controls: list[int] = []
    if extended:
        reader.u16()
        reader.u16()
        reader.u32()
        reader.u32()
        style = reader.u32()
        item_count = reader.u16()
        for _ in range(4):
            reader.i16()
        menu_kind, menu_value = reader.ordinal_or_string()
        if menu_kind == "id":
            menu_ids.append(int(menu_value))
        reader.ordinal_or_string()
        reader.utf16z()
        if style & _DS_SETFONT:
            reader.u16()
            reader.u16()
            reader.u8()
            reader.u8()
            reader.utf16z()
        if item_count > _MAX_NESTED_ITEMS:
            raise _TemplateParseError(f"dialog item count exceeds {_MAX_NESTED_ITEMS}")
        for _ in range(item_count):
            reader.align(4)
            reader.u32()
            reader.u32()
            reader.u32()
            for _coordinate in range(4):
                reader.i16()
            control_id = reader.u32()
            reader.ordinal_or_string()
            reader.ordinal_or_string()
            extra = reader.u16()
            reader.skip(extra)
            if control_id != 0xFFFFFFFF:
                controls.append(control_id)
    else:
        style = reader.u32()
        reader.u32()
        item_count = reader.u16()
        for _ in range(4):
            reader.i16()
        menu_kind, menu_value = reader.ordinal_or_string()
        if menu_kind == "id":
            menu_ids.append(int(menu_value))
        reader.ordinal_or_string()
        reader.utf16z()
        if style & _DS_SETFONT:
            reader.u16()
            reader.utf16z()
        if item_count > _MAX_NESTED_ITEMS:
            raise _TemplateParseError(f"dialog item count exceeds {_MAX_NESTED_ITEMS}")
        for _ in range(item_count):
            reader.align(4)
            reader.u32()
            reader.u32()
            for _coordinate in range(4):
                reader.i16()
            control_id = reader.u16()
            reader.ordinal_or_string()
            reader.ordinal_or_string()
            extra = reader.u16()
            reader.skip(extra)
            if control_id != 0xFFFF:
                controls.append(control_id)
    return {
        "format": "extended" if extended else "standard",
        "control_ids": sorted(controls),
        "menu_ids": sorted(menu_ids),
    }


def _parse_standard_menu_items(
    reader: _Reader,
    commands: list[int],
    item_ids: list[int],
    depth: int,
    counter: list[int],
) -> None:
    if depth > _MAX_NESTING_DEPTH:
        raise _TemplateParseError(f"menu nesting exceeds {_MAX_NESTING_DEPTH}")
    while True:
        counter[0] += 1
        if counter[0] > _MAX_NESTED_ITEMS:
            raise _TemplateParseError(f"menu item count exceeds {_MAX_NESTED_ITEMS}")
        option = reader.u16()
        popup = bool(option & _MF_POPUP)
        if popup:
            reader.utf16z()
            _parse_standard_menu_items(reader, commands, item_ids, depth + 1, counter)
        else:
            command = reader.u16()
            reader.utf16z()
            if command:
                commands.append(command)
                item_ids.append(command)
        if option & _MF_END:
            return


def _parse_extended_menu_items(
    reader: _Reader,
    commands: list[int],
    item_ids: list[int],
    depth: int,
    counter: list[int],
) -> None:
    if depth > _MAX_NESTING_DEPTH:
        raise _TemplateParseError(f"extended menu nesting exceeds {_MAX_NESTING_DEPTH}")
    while True:
        counter[0] += 1
        if counter[0] > _MAX_NESTED_ITEMS:
            raise _TemplateParseError(f"extended menu item count exceeds {_MAX_NESTED_ITEMS}")
        reader.align(4)
        reader.u32()
        reader.u32()
        command = reader.u32()
        resource_info = reader.u16()
        reader.utf16z()
        if command:
            item_ids.append(command)
        if resource_info & _MENUEX_POPUP:
            reader.align(4)
            reader.u32()
            _parse_extended_menu_items(reader, commands, item_ids, depth + 1, counter)
        elif command:
            commands.append(command)
        if resource_info & _MF_END:
            return


def _parse_menu(data: bytes) -> dict[str, object]:
    reader = _Reader(data)
    version = reader.u16()
    offset = reader.u16()
    first_item = 4 + offset
    if first_item > len(data):
        raise _TemplateParseError(f"menu item offset {first_item} exceeds resource size")
    reader.position = first_item
    commands: list[int] = []
    item_ids: list[int] = []
    counter = [0]
    if version == 0:
        _parse_standard_menu_items(reader, commands, item_ids, 0, counter)
        format_name = "standard"
    elif version == 1:
        _parse_extended_menu_items(reader, commands, item_ids, 0, counter)
        format_name = "extended"
    else:
        raise _TemplateParseError(f"unsupported menu template version {version}")
    return {
        "format": format_name,
        "command_ids": sorted(commands),
        "item_ids": sorted(item_ids),
    }


def _parse_accelerators(data: bytes) -> dict[str, object]:
    if len(data) % 8:
        raise _TemplateParseError(f"accelerator resource size {len(data)} is not divisible by 8")
    reader = _Reader(data)
    commands: list[int] = []
    terminal_seen = False
    entry_count = len(data) // 8
    if entry_count > _MAX_NESTED_ITEMS:
        raise _TemplateParseError(f"accelerator count exceeds {_MAX_NESTED_ITEMS}")
    for index in range(entry_count):
        flags = reader.u16()
        reader.u16()
        command = reader.u16()
        reader.u16()
        if command:
            commands.append(command)
        if flags & 0x80:
            terminal_seen = True
            if index != entry_count - 1:
                raise _TemplateParseError("accelerator terminal entry is not last")
    if entry_count and not terminal_seen:
        raise _TemplateParseError("accelerator table has no terminal entry")
    return {"command_ids": sorted(commands)}


def _parse_string_block(data: bytes, block_id: int) -> dict[str, object]:
    if block_id <= 0:
        raise _TemplateParseError(f"invalid string block ID {block_id}")
    reader = _Reader(data)
    string_ids: list[int] = []
    for index in range(16):
        length = reader.u16()
        if length:
            reader.skip(length * 2)
            string_ids.append((block_id - 1) * 16 + index)
    if reader.position != len(data):
        raise _TemplateParseError(
            f"string block has {len(data) - reader.position} trailing bytes"
        )
    return {"string_ids": string_ids}


def _identifier_key(value: Mapping[str, object]) -> tuple[int, object]:
    return (0, int(value["value"])) if value.get("kind") == "id" else (1, str(value["value"]))


def _resource_record_key(value: Mapping[str, object]) -> tuple[object, ...]:
    return (
        _identifier_key(value["type"]),
        _identifier_key(value["name"]),
        int(value["language_id"]),
    )


class ResourceIdContractBuilder:
    """Collect a content-independent numeric contract while PE data is enumerated."""

    def __init__(self) -> None:
        self._top_level: list[dict[str, object]] = []
        self._dialogs: list[dict[str, object]] = []
        self._menus: list[dict[str, object]] = []
        self._accelerators: list[dict[str, object]] = []
        self._strings: list[dict[str, object]] = []

    def observe(
        self,
        resource_type: Mapping[str, object],
        resource_name: Mapping[str, object],
        language_id: int,
        data: bytes,
    ) -> None:
        base = {
            "type": dict(resource_type),
            "name": dict(resource_name),
            "language_id": int(language_id),
        }
        self._top_level.append(base)
        if resource_type.get("kind") != "id":
            return
        type_id = int(resource_type["value"])
        try:
            if type_id == 5:
                self._dialogs.append({**base, **_parse_dialog(data)})
            elif type_id == 4:
                self._menus.append({**base, **_parse_menu(data)})
            elif type_id == 9:
                self._accelerators.append({**base, **_parse_accelerators(data)})
            elif type_id == 6:
                if resource_name.get("kind") != "id":
                    raise _TemplateParseError("string-table block has a named resource name")
                self._strings.append({
                    **base,
                    **_parse_string_block(data, int(resource_name["value"])),
                })
        except (UnicodeError, struct.error, _TemplateParseError) as error:
            raise BuildError(
                "RESOURCE_ID_TEMPLATE_PARSE",
                f"could not parse resource type {resource_type} name {resource_name} language {language_id}: {error}",
                5,
            ) from error

    def finish(self) -> dict[str, object]:
        stable = {
            "top_level": sorted(self._top_level, key=_resource_record_key),
            "dialogs": sorted(self._dialogs, key=_resource_record_key),
            "menus": sorted(self._menus, key=_resource_record_key),
            "accelerators": sorted(self._accelerators, key=_resource_record_key),
            "string_blocks": sorted(self._strings, key=_resource_record_key),
        }
        stable["counts"] = {
            "top_level": len(stable["top_level"]),
            "dialogs": len(stable["dialogs"]),
            "menus": len(stable["menus"]),
            "accelerators": len(stable["accelerators"]),
            "string_blocks": len(stable["string_blocks"]),
            "dialog_controls": sum(len(item["control_ids"]) for item in stable["dialogs"]),
            "dialog_menus": sum(len(item["menu_ids"]) for item in stable["dialogs"]),
            "menu_commands": sum(len(item["command_ids"]) for item in stable["menus"]),
            "menu_items": sum(len(item["item_ids"]) for item in stable["menus"]),
            "accelerator_commands": sum(len(item["command_ids"]) for item in stable["accelerators"]),
            "strings": sum(len(item["string_ids"]) for item in stable["string_blocks"]),
        }
        return {**stable, "contract_hash": _sha256_json(stable)}


def _validate_contract_hash(contract: Mapping[str, object], code: str) -> None:
    stable = {key: value for key, value in contract.items() if key != "contract_hash"}
    if contract.get("contract_hash") != _sha256_json(stable):
        raise BuildError(code, "resource compatibility contract hash mismatch", 5)


def build_resource_id_baseline(
    repo_root: Path,
    header_path: Path,
    source_roles: Mapping[str, Sequence[Path]],
    image_contracts: Mapping[str, Mapping[str, object]],
    *,
    compatibility_version: int = DEFAULT_COMPATIBILITY_VERSION,
) -> dict[str, object]:
    if compatibility_version <= 0:
        raise BuildError(
            "RESOURCE_ID_BASELINE_VERSION",
            "resource compatibility version must be positive",
            5,
        )
    header = parse_resource_header(repo_root, header_path)
    definitions = {
        str(name): int(value)
        for name, value in header["definitions"].items()
    }
    sources = [
        collect_resource_source_contract(repo_root, role, paths, definitions)
        for role, paths in sorted(source_roles.items())
    ]
    images: list[dict[str, object]] = []
    for role, contract in sorted(image_contracts.items()):
        _validate_contract_hash(contract, "RESOURCE_ID_IMAGE_CONTRACT_HASH")
        images.append({"role": role, "contract": dict(contract)})
    if {item["role"] for item in sources} != {item["role"] for item in images}:
        raise BuildError(
            "RESOURCE_ID_BASELINE_ROLE_MISMATCH",
            "resource source and image roles must match exactly",
            5,
        )
    stable = {
        "compatibility_version": compatibility_version,
        "header": header,
        "sources": sources,
        "images": images,
    }
    return {
        "schema_version": BASELINE_SCHEMA_VERSION,
        **stable,
        "baseline_hash": _sha256_json(stable),
    }


def write_resource_id_baseline(path: Path, baseline: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(baseline, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
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


def require_resource_id_baseline_version_advance(
    repo_root: Path,
    path: Path,
    baseline: Mapping[str, object],
) -> None:
    """Refuse a changed golden contract unless its compatibility version advances."""

    if not path.is_file():
        return
    existing, _relative, _hash = load_resource_id_baseline(repo_root, path)
    if existing == baseline:
        return
    old_version = int(existing.get("compatibility_version", 0))
    new_version = int(baseline.get("compatibility_version", 0))
    if new_version <= old_version:
        raise BuildError(
            "RESOURCE_ID_BASELINE_VERSION_NOT_ADVANCED",
            f"changed resource compatibility baseline requires a version greater than {old_version}",
            5,
        )


def load_resource_id_baseline(repo_root: Path, path: Path) -> tuple[dict[str, object], str, str]:
    resolved, relative = _inside_repository(
        repo_root,
        path,
        "RESOURCE_ID_BASELINE_PATH_ESCAPE",
    )
    try:
        baseline = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(
            "RESOURCE_ID_BASELINE_PARSE",
            f"could not parse {relative}: {error}",
            5,
        ) from error
    if not isinstance(baseline, dict) or baseline.get("schema_version") != BASELINE_SCHEMA_VERSION:
        raise BuildError(
            "RESOURCE_ID_BASELINE_SCHEMA",
            f"unsupported resource baseline schema in {relative}",
            5,
        )
    stable = {
        key: value for key, value in baseline.items()
        if key not in {"schema_version", "baseline_hash"}
    }
    expected_hash = _sha256_json(stable)
    if baseline.get("baseline_hash") != expected_hash:
        raise BuildError(
            "RESOURCE_ID_BASELINE_HASH",
            f"resource baseline hash mismatch in {relative}",
            5,
        )
    header = baseline.get("header")
    if not isinstance(header, dict) or not isinstance(header.get("definitions"), dict):
        raise BuildError("RESOURCE_ID_BASELINE_HEADER", "resource baseline header is missing", 5)
    if header.get("mapping_hash") != _sha256_json(header["definitions"]):
        raise BuildError("RESOURCE_ID_BASELINE_HEADER_HASH", "resource baseline header hash mismatch", 5)
    sources = baseline.get("sources")
    images = baseline.get("images")
    if not isinstance(sources, list) or not isinstance(images, list):
        raise BuildError("RESOURCE_ID_BASELINE_CONTRACTS", "resource baseline contracts are missing", 5)
    roles: set[str] = set()
    for source in sources:
        if not isinstance(source, dict):
            raise BuildError("RESOURCE_ID_BASELINE_SOURCE", "malformed source contract", 5)
        _validate_contract_hash(source, "RESOURCE_ID_BASELINE_SOURCE_HASH")
        role = str(source.get("role") or "")
        if not role or role in roles:
            raise BuildError("RESOURCE_ID_BASELINE_SOURCE_ROLE", "duplicate or empty source role", 5)
        roles.add(role)
    image_roles: set[str] = set()
    for image in images:
        if not isinstance(image, dict) or not isinstance(image.get("contract"), dict):
            raise BuildError("RESOURCE_ID_BASELINE_IMAGE", "malformed image contract", 5)
        role = str(image.get("role") or "")
        if not role or role in image_roles:
            raise BuildError("RESOURCE_ID_BASELINE_IMAGE_ROLE", "duplicate or empty image role", 5)
        image_roles.add(role)
        _validate_contract_hash(image["contract"], "RESOURCE_ID_BASELINE_IMAGE_HASH")
    if roles != image_roles:
        raise BuildError(
            "RESOURCE_ID_BASELINE_ROLE_MISMATCH",
            "resource baseline source and image roles differ",
            5,
        )
    return baseline, relative, expected_hash


def evaluate_resource_id_compatibility(
    repo_root: Path,
    baseline_path: Path,
    image_paths: Mapping[str, Path],
    image_contract_reader: Callable[[Path], Mapping[str, object]],
) -> dict[str, object]:
    baseline, baseline_relative, baseline_hash = load_resource_id_baseline(repo_root, baseline_path)
    header_baseline = baseline["header"]
    header_path = repo_root / str(header_baseline["path"])
    current_header = parse_resource_header(repo_root, header_path)
    definitions = {
        str(name): int(value)
        for name, value in current_header["definitions"].items()
    }
    failures: list[dict[str, object]] = []
    if current_header["definitions"] != header_baseline["definitions"]:
        failures.append({
            "code": "RESOURCE_ID_HEADER_MAPPING_CHANGED",
            "expected_hash": header_baseline["mapping_hash"],
            "actual_hash": current_header["mapping_hash"],
        })

    inputs: list[dict[str, object]] = [{
        "kind": "baseline",
        "path": baseline_relative,
        "hash": _sha256_bytes(_read_bytes(repo_root / baseline_relative, "RESOURCE_ID_BASELINE_READ")),
    }, {
        "kind": "header",
        "path": str(current_header["path"]),
        "hash": _sha256_bytes(_read_bytes(header_path, "RESOURCE_ID_HEADER_READ")),
    }]

    source_summaries: list[dict[str, object]] = []
    expected_sources = {str(item["role"]): item for item in baseline["sources"]}
    for role, expected in sorted(expected_sources.items()):
        paths = [repo_root / str(value) for value in expected["paths"]]
        current = collect_resource_source_contract(repo_root, role, paths, definitions)
        source_summaries.append({
            "role": role,
            "contract_hash": current["contract_hash"],
            "symbol_count": current["symbol_count"],
            "occurrence_count": current["occurrence_count"],
        })
        if current["symbols"] != expected["symbols"]:
            failures.append({
                "code": "RESOURCE_ID_SOURCE_CONTRACT_CHANGED",
                "role": role,
                "expected_hash": expected["contract_hash"],
                "actual_hash": current["contract_hash"],
            })
        for source_path in paths:
            resolved, relative = _inside_repository(
                repo_root,
                source_path,
                "RESOURCE_ID_SOURCE_PATH_ESCAPE",
            )
            inputs.append({
                "kind": "source",
                "role": role,
                "path": relative,
                "hash": _sha256_bytes(_read_bytes(resolved, "RESOURCE_ID_SOURCE_READ")),
            })

    expected_images = {str(item["role"]): item["contract"] for item in baseline["images"]}
    if set(image_paths) != set(expected_images):
        failures.append({
            "code": "RESOURCE_ID_IMAGE_ROLES_CHANGED",
            "expected": sorted(expected_images),
            "actual": sorted(image_paths),
        })
    image_summaries: list[dict[str, object]] = []
    for role in sorted(set(image_paths) & set(expected_images)):
        resolved, relative = _inside_repository(
            repo_root,
            image_paths[role],
            "RESOURCE_ID_IMAGE_PATH_ESCAPE",
        )
        if not resolved.is_file():
            raise BuildError("RESOURCE_ID_IMAGE_MISSING", f"resource image is missing: {relative}", 5)
        current = dict(image_contract_reader(resolved))
        _validate_contract_hash(current, "RESOURCE_ID_IMAGE_CONTRACT_HASH")
        expected = expected_images[role]
        image_summaries.append({
            "role": role,
            "path": relative,
            "contract_hash": current["contract_hash"],
            "counts": current["counts"],
        })
        inputs.append({
            "kind": "image",
            "role": role,
            "path": relative,
            "hash": _sha256_bytes(_read_bytes(resolved, "RESOURCE_ID_IMAGE_READ")),
        })
        if current != expected:
            failures.append({
                "code": "RESOURCE_ID_IMAGE_CONTRACT_CHANGED",
                "role": role,
                "expected_hash": expected["contract_hash"],
                "actual_hash": current["contract_hash"],
            })

    inputs.sort(key=lambda item: (str(item["kind"]), str(item.get("role") or ""), str(item["path"])))
    observed = not failures
    return {
        "observed": observed,
        "compatibility_version": baseline["compatibility_version"],
        "baseline": {"path": baseline_relative, "hash": baseline_hash},
        "header": {
            "path": current_header["path"],
            "definition_count": current_header["definition_count"],
            "mapping_hash": current_header["mapping_hash"],
        },
        "sources": source_summaries,
        "images": image_summaries,
        "inputs": inputs,
        "failures": failures,
    }


def validate_resource_id_compatibility_inputs(
    repo_root: Path,
    compatibility: Mapping[str, object],
) -> list[dict[str, object]]:
    failures: list[dict[str, object]] = []
    inputs = compatibility.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        return [{"code": "RESOURCE_ID_INPUTS_MISSING"}]
    for item in inputs:
        if not isinstance(item, dict):
            failures.append({"code": "RESOURCE_ID_INPUT_MALFORMED"})
            continue
        raw_path = str(item.get("path") or "")
        if not raw_path:
            failures.append({"code": "RESOURCE_ID_INPUT_PATH_MISSING"})
            continue
        resolved, relative = _inside_repository(
            repo_root,
            repo_root / raw_path,
            "RESOURCE_ID_INPUT_PATH_ESCAPE",
        )
        if not resolved.is_file():
            failures.append({"code": "RESOURCE_ID_INPUT_MISSING", "path": relative})
            continue
        actual_hash = _sha256_bytes(_read_bytes(resolved, "RESOURCE_ID_INPUT_READ"))
        if actual_hash != item.get("hash"):
            failures.append({
                "code": "RESOURCE_ID_INPUT_CHANGED",
                "path": relative,
                "expected": item.get("hash"),
                "actual": actual_hash,
            })
    return failures
