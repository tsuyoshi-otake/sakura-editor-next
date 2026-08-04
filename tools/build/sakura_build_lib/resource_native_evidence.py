"""Native PE resource-table evidence derived from a validated product build."""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
from typing import Callable, Mapping, Sequence

from .model import SemanticGraph
from .product_native_evidence import validate_product_native_evidence
from .runner import BuildError


EVIDENCE_SCHEMA_VERSION = 1
_LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x00000020
_LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE = 0x00000040
_ERROR_RESOURCE_TYPE_NOT_FOUND = 1813
_MAX_RESOURCE_ENTRIES = 100_000
_MAX_RESOURCE_ENTRY_BYTES = 64 * 1024 * 1024
_MAX_RESOURCE_TOTAL_BYTES = 512 * 1024 * 1024


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path, code: str) -> str:
    try:
        with path.open("rb") as stream:
            digest = hashlib.sha256()
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise BuildError(code, f"could not hash {path}: {error}", 5) from error
    return "sha256:" + digest.hexdigest()


def _sha256_json(value: object) -> str:
    serialized = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return _sha256_bytes(serialized.encode("utf-8"))


def _hard_evidence_payload(stable_payload: Mapping[str, object]) -> dict[str, object]:
    """Exclude the separately validated native hash from root-independent resource identity."""

    payload = dict(stable_payload)
    source_native = payload.get("source_native_evidence")
    if isinstance(source_native, Mapping):
        payload["source_native_evidence"] = {
            "path": source_native.get("path"),
        }
    return payload


def _identifier(pointer: int) -> dict[str, object]:
    if pointer <= 0xFFFF:
        return {"kind": "id", "value": pointer}
    return {"kind": "name", "value": ctypes.wstring_at(pointer)}


def _identifier_key(value: Mapping[str, object]) -> tuple[int, object]:
    if value.get("kind") == "id":
        return (0, int(value["value"]))
    return (1, str(value["value"]))


def _normalize_entries(entries: Sequence[Mapping[str, object]]) -> list[dict[str, object]]:
    normalized = [
        {
            "type": dict(entry["type"]),
            "name": dict(entry["name"]),
            "language_id": int(entry["language_id"]),
            "size": int(entry["size"]),
            "content_hash": str(entry["content_hash"]),
        }
        for entry in entries
    ]
    return sorted(
        normalized,
        key=lambda entry: (
            _identifier_key(entry["type"]),
            _identifier_key(entry["name"]),
            int(entry["language_id"]),
        ),
    )


def _resource_table_payload(entries: Sequence[Mapping[str, object]]) -> dict[str, object]:
    normalized = _normalize_entries(entries)
    type_counts: dict[tuple[str, object], int] = {}
    type_values: dict[tuple[str, object], dict[str, object]] = {}
    for entry in normalized:
        resource_type = entry["type"]
        key = (str(resource_type["kind"]), resource_type["value"])
        type_values[key] = dict(resource_type)
        type_counts[key] = type_counts.get(key, 0) + 1
    counts = [
        {"type": type_values[key], "entry_count": type_counts[key]}
        for key in sorted(type_counts, key=lambda item: _identifier_key(type_values[item]))
    ]
    return {
        "entries": normalized,
        "entry_count": len(normalized),
        "distinct_type_count": len(counts),
        "numeric_type_entry_count": sum(1 for entry in normalized if entry["type"]["kind"] == "id"),
        "named_type_entry_count": sum(1 for entry in normalized if entry["type"]["kind"] == "name"),
        "language_ids": sorted({int(entry["language_id"]) for entry in normalized}),
        "total_bytes": sum(int(entry["size"]) for entry in normalized),
        "type_entry_counts": counts,
    }


def _enumerate_pe_resource_table(path: Path) -> list[dict[str, object]]:
    if os.name != "nt":
        raise BuildError(
            "RESOURCE_TABLE_PLATFORM_UNSUPPORTED",
            "native PE resource enumeration requires Windows",
            5,
        )

    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    enum_type_callback = ctypes.WINFUNCTYPE(
        wintypes.BOOL,
        wintypes.HMODULE,
        ctypes.c_void_p,
        ctypes.c_ssize_t,
    )
    enum_name_callback = ctypes.WINFUNCTYPE(
        wintypes.BOOL,
        wintypes.HMODULE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_ssize_t,
    )
    enum_language_callback = ctypes.WINFUNCTYPE(
        wintypes.BOOL,
        wintypes.HMODULE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.WORD,
        ctypes.c_ssize_t,
    )

    kernel32.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
    kernel32.LoadLibraryExW.restype = wintypes.HMODULE
    kernel32.FreeLibrary.argtypes = (wintypes.HMODULE,)
    kernel32.FreeLibrary.restype = wintypes.BOOL
    kernel32.EnumResourceTypesW.argtypes = (wintypes.HMODULE, enum_type_callback, ctypes.c_ssize_t)
    kernel32.EnumResourceTypesW.restype = wintypes.BOOL
    kernel32.EnumResourceNamesW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, enum_name_callback, ctypes.c_ssize_t)
    kernel32.EnumResourceNamesW.restype = wintypes.BOOL
    kernel32.EnumResourceLanguagesW.argtypes = (
        wintypes.HMODULE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        enum_language_callback,
        ctypes.c_ssize_t,
    )
    kernel32.EnumResourceLanguagesW.restype = wintypes.BOOL
    kernel32.FindResourceExW.argtypes = (
        wintypes.HMODULE,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.WORD,
    )
    kernel32.FindResourceExW.restype = ctypes.c_void_p
    kernel32.SizeofResource.argtypes = (wintypes.HMODULE, ctypes.c_void_p)
    kernel32.SizeofResource.restype = wintypes.DWORD
    kernel32.LoadResource.argtypes = (wintypes.HMODULE, ctypes.c_void_p)
    kernel32.LoadResource.restype = ctypes.c_void_p
    kernel32.LockResource.argtypes = (ctypes.c_void_p,)
    kernel32.LockResource.restype = ctypes.c_void_p

    module = kernel32.LoadLibraryExW(
        str(path),
        None,
        _LOAD_LIBRARY_AS_IMAGE_RESOURCE | _LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE,
    )
    if not module:
        error = ctypes.get_last_error()
        raise BuildError(
            "RESOURCE_TABLE_IMAGE_OPEN",
            f"could not open PE image {path} as data: Win32 error {error}",
            5,
        )

    entries: list[dict[str, object]] = []
    total_bytes = 0
    callback_error: list[BaseException] = []
    callback_refs: list[object] = []

    def fail_callback(error: BaseException) -> bool:
        callback_error.append(error)
        return False

    def on_type(_module: int, type_pointer: int, _parameter: int) -> bool:
        type_value = int(type_pointer or 0)

        def on_name(_inner_module: int, _type_pointer: int, name_pointer: int, _inner_parameter: int) -> bool:
            name_value = int(name_pointer or 0)

            def on_language(
                _language_module: int,
                _language_type: int,
                _language_name: int,
                language_id: int,
                _language_parameter: int,
            ) -> bool:
                nonlocal total_bytes
                try:
                    if len(entries) >= _MAX_RESOURCE_ENTRIES:
                        raise BuildError(
                            "RESOURCE_TABLE_ENTRY_LIMIT",
                            f"PE resource table exceeds {_MAX_RESOURCE_ENTRIES} entries",
                            5,
                        )
                    resource = kernel32.FindResourceExW(
                        module,
                        ctypes.c_void_p(type_value),
                        ctypes.c_void_p(name_value),
                        language_id,
                    )
                    if not resource:
                        raise OSError(f"FindResourceExW failed with Win32 error {ctypes.get_last_error()}")
                    size = int(kernel32.SizeofResource(module, resource))
                    if size > _MAX_RESOURCE_ENTRY_BYTES:
                        raise BuildError(
                            "RESOURCE_TABLE_ENTRY_SIZE_LIMIT",
                            f"PE resource entry exceeds {_MAX_RESOURCE_ENTRY_BYTES} bytes",
                            5,
                        )
                    if total_bytes + size > _MAX_RESOURCE_TOTAL_BYTES:
                        raise BuildError(
                            "RESOURCE_TABLE_TOTAL_SIZE_LIMIT",
                            f"PE resource table exceeds {_MAX_RESOURCE_TOTAL_BYTES} bytes",
                            5,
                        )
                    handle = kernel32.LoadResource(module, resource)
                    if not handle:
                        raise OSError(f"LoadResource failed with Win32 error {ctypes.get_last_error()}")
                    pointer = kernel32.LockResource(handle)
                    if size and not pointer:
                        raise OSError(f"LockResource failed with Win32 error {ctypes.get_last_error()}")
                    data = ctypes.string_at(pointer, size) if size else b""
                    total_bytes += size
                    entries.append({
                        "type": _identifier(type_value),
                        "name": _identifier(name_value),
                        "language_id": int(language_id),
                        "size": size,
                        "content_hash": _sha256_bytes(data),
                    })
                    return True
                except BaseException as error:  # ctypes callbacks cannot propagate exceptions safely.
                    return fail_callback(error)

            language_callback = enum_language_callback(on_language)
            callback_refs.append(language_callback)
            ctypes.set_last_error(0)
            result = kernel32.EnumResourceLanguagesW(
                module,
                ctypes.c_void_p(type_value),
                ctypes.c_void_p(name_value),
                language_callback,
                0,
            )
            if not result and not callback_error:
                return fail_callback(OSError(
                    f"EnumResourceLanguagesW failed with Win32 error {ctypes.get_last_error()}"
                ))
            return not callback_error

        name_callback = enum_name_callback(on_name)
        callback_refs.append(name_callback)
        ctypes.set_last_error(0)
        result = kernel32.EnumResourceNamesW(
            module,
            ctypes.c_void_p(type_value),
            name_callback,
            0,
        )
        if not result and not callback_error:
            return fail_callback(OSError(
                f"EnumResourceNamesW failed with Win32 error {ctypes.get_last_error()}"
            ))
        return not callback_error

    type_callback = enum_type_callback(on_type)
    callback_refs.append(type_callback)
    try:
        ctypes.set_last_error(0)
        result = kernel32.EnumResourceTypesW(module, type_callback, 0)
        error = ctypes.get_last_error()
        if callback_error:
            if isinstance(callback_error[0], BuildError):
                raise callback_error[0]
            raise BuildError(
                "RESOURCE_TABLE_ENUMERATION",
                f"could not enumerate PE resources in {path}: {callback_error[0]}",
                5,
            ) from callback_error[0]
        if not result and error not in {0, _ERROR_RESOURCE_TYPE_NOT_FOUND}:
            raise BuildError(
                "RESOURCE_TABLE_ENUMERATION",
                f"could not enumerate PE resource types in {path}: Win32 error {error}",
                5,
            )
    finally:
        kernel32.FreeLibrary(module)

    return _normalize_entries(entries)


def _inside_repository(repo_root: Path, path: Path, code: str) -> tuple[Path, str]:
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError as error:
        raise BuildError(code, f"path escapes repository: {path}", 5) from error
    return resolved, relative


def collect_resource_native_evidence(
    graph: SemanticGraph,
    native_evidence_path: Path,
    product_id: str,
    context_id: str,
    *,
    table_reader: Callable[[Path], list[dict[str, object]]] = _enumerate_pe_resource_table,
) -> dict[str, object]:
    native_validation = validate_product_native_evidence(
        graph,
        native_evidence_path,
        product_id,
        context_id,
    )
    if not native_validation["valid"]:
        raise BuildError(
            "RESOURCE_TABLE_NATIVE_EVIDENCE_INVALID",
            "native product evidence must be current before resource-table observation",
            5,
        )

    try:
        native = json.loads(native_evidence_path.read_text(encoding="utf-8"))
        product_relative = str(native["link"]["output"])
        native_hard_hash = str(native["hard_evidence_hash"])
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise BuildError(
            "RESOURCE_TABLE_NATIVE_EVIDENCE_PARSE",
            f"could not read native product coordinates from {native_evidence_path}: {error}",
            5,
        ) from error

    product_path, product_relative = _inside_repository(
        graph.repo_root,
        graph.repo_root / product_relative,
        "RESOURCE_TABLE_PRODUCT_PATH_ESCAPE",
    )
    if not product_path.is_file():
        raise BuildError(
            "RESOURCE_TABLE_PRODUCT_MISSING",
            f"native product is missing: {product_relative}",
            5,
        )
    native_path, native_relative = _inside_repository(
        graph.repo_root,
        native_evidence_path,
        "RESOURCE_TABLE_NATIVE_EVIDENCE_PATH_ESCAPE",
    )
    del native_path

    product_hash = _sha256_file(product_path, "RESOURCE_TABLE_PRODUCT_HASH")
    table_payload = _resource_table_payload(table_reader(product_path))
    resource_table = {
        **table_payload,
        "table_hash": _sha256_json(table_payload),
    }
    stable_payload = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
        "backend": "msbuild-pe",
        "source_native_evidence": {
            "path": native_relative,
            "hard_evidence_hash": native_hard_hash,
        },
        "product": {
            "path": product_relative,
            "hash": product_hash,
        },
        "resource_table": resource_table,
        "resource_id_compatibility": {
            "observed": False,
            "reason": "top-level PE resource table is observed; canonical numeric ID and nested dialog/menu/control compatibility baseline is not established",
        },
    }
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "collection_ok": True,
        **stable_payload,
        "hard_evidence_hash": _sha256_json(_hard_evidence_payload(stable_payload)),
    }


def write_resource_native_evidence(path: Path, evidence: Mapping[str, object]) -> None:
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


def validate_resource_native_evidence(
    graph: SemanticGraph,
    path: Path | None,
    native_evidence_path: Path | None,
    product_id: str,
    context_id: str,
) -> dict[str, object]:
    if path is None:
        return {
            "status": "not_provided",
            "valid": False,
            "evidence_path": None,
            "failures": [{"code": "RESOURCE_TABLE_EVIDENCE_NOT_PROVIDED"}],
        }
    try:
        evidence_path, relative_path = _inside_repository(
            graph.repo_root,
            path,
            "RESOURCE_TABLE_EVIDENCE_PATH_ESCAPE",
        )
    except BuildError:
        raise
    if not evidence_path.is_file():
        return {
            "status": "missing",
            "valid": False,
            "evidence_path": relative_path,
            "failures": [{"code": "RESOURCE_TABLE_EVIDENCE_MISSING"}],
        }
    try:
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError("RESOURCE_TABLE_EVIDENCE_PARSE", f"could not parse {path}: {error}", 5) from error
    if not isinstance(evidence, dict) or evidence.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        raise BuildError("RESOURCE_TABLE_EVIDENCE_SCHEMA", f"unsupported evidence schema in {path}", 5)

    failures: list[dict[str, object]] = []
    expected = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
        "backend": "msbuild-pe",
    }
    for field, value in expected.items():
        if evidence.get(field) != value:
            failures.append({
                "code": f"RESOURCE_TABLE_EVIDENCE_{field.upper()}_MISMATCH",
                "expected": value,
                "actual": evidence.get(field),
            })

    if native_evidence_path is None:
        failures.append({"code": "RESOURCE_TABLE_NATIVE_EVIDENCE_NOT_PROVIDED"})
        native_validation: Mapping[str, object] = {"valid": False}
    else:
        native_validation = validate_product_native_evidence(
            graph,
            native_evidence_path,
            product_id,
            context_id,
        )
        if not native_validation["valid"]:
            failures.append({"code": "RESOURCE_TABLE_NATIVE_EVIDENCE_INVALID"})

    source_native = evidence.get("source_native_evidence")
    if not isinstance(source_native, dict):
        failures.append({"code": "RESOURCE_TABLE_SOURCE_NATIVE_MISSING"})
        source_native = {}
    if source_native.get("hard_evidence_hash") != native_validation.get("hard_evidence_hash"):
        failures.append({"code": "RESOURCE_TABLE_SOURCE_NATIVE_HASH_MISMATCH"})
    if native_evidence_path is not None:
        try:
            _native_resolved, native_relative = _inside_repository(
                graph.repo_root,
                native_evidence_path,
                "RESOURCE_TABLE_NATIVE_EVIDENCE_PATH_ESCAPE",
            )
        except BuildError:
            raise
        if source_native.get("path") != native_relative:
            failures.append({"code": "RESOURCE_TABLE_SOURCE_NATIVE_PATH_MISMATCH"})

    product = evidence.get("product")
    if not isinstance(product, dict):
        failures.append({"code": "RESOURCE_TABLE_PRODUCT_MISSING"})
        product = {}
    product_relative = str(product.get("path") or "")
    if not product_relative:
        failures.append({"code": "RESOURCE_TABLE_PRODUCT_PATH_MISSING"})
    else:
        product_path, _relative = _inside_repository(
            graph.repo_root,
            graph.repo_root / product_relative,
            "RESOURCE_TABLE_PRODUCT_PATH_ESCAPE",
        )
        if not product_path.is_file():
            failures.append({"code": "RESOURCE_TABLE_PRODUCT_MISSING", "path": product_relative})
        elif _sha256_file(product_path, "RESOURCE_TABLE_EVIDENCE_VALIDATE") != product.get("hash"):
            failures.append({"code": "RESOURCE_TABLE_PRODUCT_CHANGED", "path": product_relative})

    resource_table = evidence.get("resource_table")
    if not isinstance(resource_table, dict) or not isinstance(resource_table.get("entries"), list):
        failures.append({"code": "RESOURCE_TABLE_ENTRIES_MISSING"})
        resource_table = {}
    else:
        expected_table = _resource_table_payload(resource_table["entries"])
        if any(resource_table.get(key) != value for key, value in expected_table.items()):
            failures.append({"code": "RESOURCE_TABLE_METADATA_MISMATCH"})
        if resource_table.get("table_hash") != _sha256_json(expected_table):
            failures.append({"code": "RESOURCE_TABLE_CONTENT_HASH_MISMATCH"})

    stable_payload = {
        key: value for key, value in evidence.items()
        if key not in {"schema_version", "collection_ok", "hard_evidence_hash"}
    }
    if evidence.get("hard_evidence_hash") != _sha256_json(_hard_evidence_payload(stable_payload)):
        failures.append({"code": "RESOURCE_TABLE_EVIDENCE_HASH_MISMATCH"})

    valid = not failures
    compatibility = evidence.get("resource_id_compatibility")
    compatibility_observed = (
        isinstance(compatibility, dict) and compatibility.get("observed") is True
    )
    return {
        "status": "observed" if valid else "stale_or_mismatched",
        "valid": valid,
        "evidence_path": relative_path,
        "hard_evidence_hash": evidence.get("hard_evidence_hash"),
        "failures": failures,
        "coverage": {
            "resource_table_observed": valid and bool(resource_table.get("entries")),
            "resource_id_compatibility_observed": valid and compatibility_observed,
        },
        "product": product if valid else {},
        "resource_table": resource_table if valid else {},
        "resource_id_compatibility": compatibility if valid else {},
    }
