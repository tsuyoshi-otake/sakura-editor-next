"""Strict, payload-free Output link and size evidence.

This module joins two independently produced native-link observations with
the two Output provider build manifests.  It deliberately projects only
bounded identities, counts, and result codes.  Paths, linker command lines,
MAP text, and archive member names never enter the generated report.  Generic
archive/member and export observations are not treated as Output-provider
selection proof; that proof must be explicitly provider-scoped.

The backend stored by final-image staging is only a partition and caller
assertion.  It is not trusted as provider-selection evidence by itself.  The
strict consumer cross-checks the startup manifest selector (and any reliable
native selector fields when present) against the expected backend and the
staged receipt; absent native selector fields are intentionally reported only
through the manifest/stage contract, never filled by a heuristic.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
from pathlib import Path
from typing import Mapping, Sequence

from .output_final_image_evidence import (
    OutputFinalImageEvidenceError,
    RECORD_KIND as FINAL_IMAGE_RECORD_KIND,
    validate_output_final_image_stage,
)
from .product_native_evidence import (
    product_native_evidence_hash,
    product_native_evidence_source_hash,
    validate_output_provider_evidence_for_final_image,
)
from .repository_path_safety import RepositoryPathSafetyError, safe_repository_path
from .runner import BuildError


SCHEMA_VERSION = 1
RECORD_KIND = "output-link-size-evidence"
DEFAULT_SIZE_THRESHOLD_PERCENT = 5.0
MAX_FAILURES = 64
MAX_MEMBER_COUNT = 4096

_HASH_RE = re.compile(r"^(?:sha256:)?([0-9a-fA-F]{64})$")
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40,64}$")
_EXPECTED_BACKENDS = ("cpp", "rust")
_EXPECTED_CONFIGURATIONS = ("Debug", "Release")
_EXPECTED_PROVIDER_SYMBOLS = frozenset(
    {
        "sakura_output_provider_active_channel_v1",
        "sakura_output_provider_apply_v1",
        "sakura_output_provider_create_v1",
        "sakura_output_provider_destroy_v1",
        "sakura_output_provider_snapshot_measure_v1",
        "sakura_output_provider_snapshot_write_v1",
        "sakura_output_provider_stop_v1",
    }
)
_NATIVE_EVIDENCE_SCHEMA_VERSION = 4
_MANIFEST_SCHEMA_VERSION = 1
_MANIFEST_RECORDS = frozenset({"output-provider-build-manifest", "output-startup-build-manifest"})
_REPORT_KEYS = frozenset(
    {
        "schemaVersion",
        "record",
        "payloadFree",
        "status",
        "source",
        "platform",
        "configuration",
        "selectors",
        "images",
        "link",
        "sizeGate",
        "inputs",
        "failures",
        "decision",
        "adoptionEligible",
        "reportSha256",
    }
)
_SOURCE_KEYS = frozenset({"commit", "dirty", "statusSha256", "complete"})
_SELECTOR_KEYS = frozenset(
    {
        "outputBackend",
        "utf16Backend",
        "outputProductionPackage",
        "utf16ProductionPackage",
        "proofResult",
        "proofSha256",
        "compileRustSelectorDefineCount",
    }
)
_IMAGE_KEYS = frozenset({"sha256", "sizeBytes"})
_LINK_KEYS = frozenset(
    {
        "linkCommandSha256",
        "mapSha256",
        "mapSizeBytes",
        "staticRustArchiveSha256",
        "staticRustArchiveSizeBytes",
        "staticRustArchiveCount",
        "selectedMemberCount",
        "selectedMemberSetSha256",
        "providerSymbolCount",
        "duplicateProviderSymbolCount",
    }
)
_SIZE_GATE_KEYS = frozenset(
    {
        "thresholdPercent",
        "cppSizeBytes",
        "rustSizeBytes",
        "deltaBytes",
        "deltaPercent",
        "pass",
    }
)
_INPUT_KEYS = frozenset(
    {"cppNativeSha256", "rustNativeSha256", "cppManifestSha256", "rustManifestSha256"}
)
_FAILURE_CODES = frozenset(
    {
        "SOURCE_PROVENANCE_UNPROVEN",
        "SOURCE_DIRTY",
        "SOURCE_MISMATCH",
        "PLATFORM_CONFIGURATION_UNPROVEN",
        "PLATFORM_CONFIGURATION_MISMATCH",
        "SELECTOR_PROOF_INCOMPLETE",
        "SELECTOR_PROOF_HASH_MISMATCH",
        "SELECTOR_MISMATCH",
        "SELECTOR_UNRESOLVED_SYMBOL_UNPROVEN",
        "SELECTOR_UNRESOLVED_SYMBOL_SET_MISMATCH",
        "SELECTOR_UNRESOLVED_SYMBOL_COUNT_MISMATCH",
        "NATIVE_SELECTOR_UNPROVEN",
        "PRODUCTION_PACKAGE_UNPROVEN",
        "NATIVE_EVIDENCE_UNAVAILABLE",
        "NATIVE_EVIDENCE_TAMPERED",
        "FINAL_IMAGE_UNPROVEN",
        "FINAL_IMAGE_MISMATCH",
        "FINAL_IMAGE_TAMPERED",
        "FINAL_IMAGE_PATH_UNSAFE",
        "FINAL_IMAGE_STAGE_UNPROVEN",
        "FINAL_IMAGE_STAGE_TAMPERED",
        "FINAL_IMAGE_STAGE_PATH_UNSAFE",
        "FINAL_IMAGE_STAGE_SELECTOR_MISMATCH",
        "FINAL_IMAGE_STAGE_BINDING_MISMATCH",
        "FINAL_IMAGE_MANIFEST_IDENTITY_UNPROVEN",
        "FINAL_IMAGE_MANIFEST_IDENTITY_MISMATCH",
        "LINK_COMMAND_UNPROVEN",
        "MAP_UNPROVEN",
        "MAP_PATH_UNSAFE",
        "MAP_HASH_MISMATCH",
        "SELECTED_MEMBER_UNPROVEN",
        "SELECTED_MEMBER_SCOPE_UNPROVEN",
        "SELECTED_MEMBER_COUNT_MISMATCH",
        "SELECTED_MEMBER_DUPLICATE",
        "PROVIDER_MEMBER_UNPROVEN",
        "PROVIDER_MEMBER_SCOPE_UNPROVEN",
        "STATIC_RUST_ARCHIVE_UNPROVEN",
        "STATIC_RUST_ARCHIVE_COUNT_UNPROVEN",
        "DUPLICATE_STATIC_RUST_ARCHIVE",
        "STATIC_RUST_ARCHIVE_MISMATCH",
        "PROVIDER_SYMBOL_UNPROVEN",
        "PROVIDER_SYMBOL_SET_MISMATCH",
        "FINAL_PROVIDER_SYMBOL_UNPROVEN",
        "FINAL_PROVIDER_SYMBOL_SCOPE_UNPROVEN",
        "PROVIDER_SYMBOL_DUPLICATE_COUNT_UNPROVEN",
        "DUPLICATE_PROVIDER_SYMBOLS",
        "NATIVE_EVIDENCE_HASH_UNPROVEN",
        "NATIVE_EVIDENCE_SCHEMA",
        "MANIFEST_SCHEMA",
        "THRESHOLD_INVALID",
    }
)


class OutputLinkSizeEvidenceError(BuildError):
    """Typed input or report-schema failure for Output link evidence."""

    def __init__(self, code: str, message: str, exit_code: int = 5) -> None:
        super().__init__(code, message, exit_code)


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_json(value: object) -> str:
    return _sha256_bytes(_canonical_json(value))


def _normalise_hash(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    match = _HASH_RE.fullmatch(value.strip())
    return match.group(1).lower() if match else None


def _normalise_commit(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    value = value.strip()
    return value.lower() if _COMMIT_RE.fullmatch(value) else None


def _get(value: object, *names: str) -> object:
    if not isinstance(value, Mapping):
        return None
    for name in names:
        if name in value:
            return value[name]
    return None


def _mapping(value: object) -> Mapping[str, object]:
    return value if isinstance(value, Mapping) else {}


def _bool_value(value: object) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "1", "on", "yes"}:
            return True
        if lowered in {"false", "0", "off", "no"}:
            return False
    return None


def _int_value(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int) and 0 <= value <= 2**63 - 1:
        return value
    return None


def _signed_int_value(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int) and -(2**63) <= value <= 2**63 - 1:
        return value
    return None


def _float_value(value: object) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def _path_from_reference(repo_root: Path | None, reference: object) -> Path | None:
    if repo_root is None or not isinstance(reference, str) or not reference.strip():
        return None
    try:
        return safe_repository_path(
            repo_root,
            reference,
            code="OUTPUT_LINK_SIZE_PATH_UNSAFE",
            reject_parent_segments=True,
            reject_absolute=True,
            require_regular_file=True,
        )
    except RepositoryPathSafetyError:
        return None


def _file_identity(repo_root: Path | None, reference: object) -> tuple[str, int] | None:
    path = _path_from_reference(repo_root, reference)
    if path is None:
        return None
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
        return digest.hexdigest(), path.stat().st_size
    except OSError:
        return None


def _load_source(source: Path | Mapping[str, object]) -> tuple[dict[str, object], str]:
    if isinstance(source, Path):
        if source.is_symlink() or not source.is_file():
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_INPUT", "evidence input is not a regular file")
        try:
            data = source.read_bytes()
            value = json.loads(data.decode("utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_INPUT", "evidence input is not valid JSON") from error
        digest = _sha256_bytes(data)
    else:
        value = dict(source)
        digest = _sha256_json(value)
    if not isinstance(value, dict):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_INPUT", "evidence input must be a JSON object")
    return value, digest


def _add_failure(failures: list[str], code: str) -> None:
    if code not in _FAILURE_CODES:
        code = "NATIVE_EVIDENCE_SCHEMA"
    if code not in failures and len(failures) < MAX_FAILURES:
        failures.append(code)


def _source_descriptor(value: Mapping[str, object]) -> dict[str, object]:
    nested = _mapping(_get(value, "source"))
    commit = _normalise_commit(_get(value, "sourceHead", "sourceCommit")) or _normalise_commit(
        _get(nested, "commit", "sourceHead", "sourceCommit")
    )
    dirty = _bool_value(_get(value, "sourceDirty"))
    if dirty is None:
        dirty = _bool_value(_get(nested, "dirty"))
    status_hash = _normalise_hash(_get(value, "sourceStatusSha256")) or _normalise_hash(
        _get(nested, "statusSha256", "sourceStatusSha256")
    )
    return {"commit": commit, "dirty": dirty, "statusSha256": status_hash}


def _context_descriptor(value: Mapping[str, object]) -> tuple[str | None, str | None]:
    platform = _get(value, "platform")
    configuration = _get(value, "configuration")
    if isinstance(platform, Mapping):
        platform = _get(platform, "architecture", "platform")
    if isinstance(configuration, Mapping):
        configuration = _get(configuration, "configuration", "name")
    platform_value = str(platform).strip() if isinstance(platform, str) else None
    configuration_value = str(configuration).strip() if isinstance(configuration, str) else None
    context = _get(value, "context_id", "contextId")
    if isinstance(context, str):
        match = re.fullmatch(r"(?:msvc|cmake-msvc)-(?P<platform>x64)-(?P<configuration>debug|release)", context.strip(), re.IGNORECASE)
        if match:
            context_platform = match.group("platform")
            context_configuration = match.group("configuration").capitalize()
            platform_value = platform_value or context_platform
            configuration_value = configuration_value or context_configuration
    if platform_value is not None:
        platform_value = platform_value.lower() if platform_value.lower() == "x64" else platform_value
    if configuration_value is not None:
        lowered_configuration = configuration_value.lower()
        if lowered_configuration in {"debug", "release"}:
            configuration_value = lowered_configuration.capitalize()
    return platform_value, configuration_value


def _selector_descriptor(
    manifest: Mapping[str, object],
    expected_backend: str,
    failures: list[str],
) -> tuple[dict[str, object], Mapping[str, object]]:
    proof = _mapping(_get(manifest, "selectorProof", "selector_proof"))
    manifest_backend = _get(manifest, "backend")
    manifest_backend = str(manifest_backend).strip().lower() if isinstance(manifest_backend, str) else None
    output_backend = _get(manifest, "outputBackend")
    output_backend = str(output_backend).strip().lower() if isinstance(output_backend, str) else None
    utf16_backend = _get(manifest, "utf16Backend")
    utf16_backend = str(utf16_backend).strip().lower() if isinstance(utf16_backend, str) else None
    output_package = _bool_value(_get(manifest, "outputProductionPackage"))
    utf16_package = _bool_value(_get(manifest, "utf16ProductionPackage"))
    proof_output = _get(proof, "outputBackend")
    proof_output = str(proof_output).strip().lower() if isinstance(proof_output, str) else None
    proof_utf16 = _get(proof, "utf16Backend")
    proof_utf16 = str(proof_utf16).strip().lower() if isinstance(proof_utf16, str) else None
    proof_output_package = _bool_value(_get(proof, "outputProductionPackage"))
    proof_utf16_package = _bool_value(_get(proof, "utf16ProductionPackage"))
    define_count = _int_value(_get(proof, "compileCommandRustSelectorDefineCount"))
    selector = {
        "outputBackend": output_backend,
        "utf16Backend": utf16_backend,
        "outputProductionPackage": output_package,
        "utf16ProductionPackage": utf16_package,
        "proofResult": _get(proof, "result"),
        "proofSha256": _normalise_hash(_get(manifest, "selectorProofSha256")),
        "compileRustSelectorDefineCount": define_count,
    }
    manifest_proof_hash = selector["proofSha256"]
    proof_contract_hash = _normalise_hash(_get(proof, "selectorContractSha256"))
    if manifest_proof_hash is None or proof_contract_hash is None or manifest_proof_hash != proof_contract_hash:
        _add_failure(failures, "SELECTOR_PROOF_HASH_MISMATCH")
    values = (
        manifest_backend,
        output_backend,
        utf16_backend,
        output_package,
        utf16_package,
        proof_output,
        proof_utf16,
        proof_output_package,
        proof_utf16_package,
    )
    if (
        manifest_backend != expected_backend
        or output_backend != expected_backend
        or manifest_backend != output_backend
        or proof_output not in {None, expected_backend}
    ):
        _add_failure(failures, "SELECTOR_MISMATCH")
    if utf16_backend != "cpp" or proof_utf16 not in {None, "cpp"}:
        _add_failure(failures, "SELECTOR_MISMATCH")
    if output_package is not False or utf16_package is not False:
        _add_failure(failures, "PRODUCTION_PACKAGE_UNPROVEN")
    if proof_output_package not in {None, False} or proof_utf16_package not in {None, False}:
        _add_failure(failures, "PRODUCTION_PACKAGE_UNPROVEN")
    manifest_configuration = _context_descriptor(manifest)[1]
    expected_proof_result = {
        "Debug": "dumpbin-unresolved-refs-verified",
        "Release": "msvc-ltcg-compile-selector-verified",
    }.get(manifest_configuration)
    expected_define_count = 0 if manifest_configuration == "Debug" else (0 if expected_backend == "cpp" else 1)
    compile_log_proof = _get(proof, "compileLogProof")
    compile_command_has_gl = _get(proof, "compileCommandHasGl")
    if manifest_configuration == "Debug":
        compile_requirements_valid = compile_log_proof is False and compile_command_has_gl is False
    else:
        compile_requirements_valid = compile_log_proof is True and compile_command_has_gl is True
    if (
        selector["proofResult"] != expected_proof_result
        or define_count != expected_define_count
        or not compile_requirements_valid
        or any(value is None for value in values)
        or selector["proofSha256"] is None
    ):
        _add_failure(failures, "SELECTOR_PROOF_INCOMPLETE")
    symbols = _get(proof, "definedProviderSymbols")
    symbol_count = _int_value(_get(proof, "definedProviderSymbolCount"))
    if not isinstance(symbols, list) or any(not isinstance(item, str) for item in symbols):
        _add_failure(failures, "PROVIDER_SYMBOL_UNPROVEN")
    else:
        if set(symbols) != _EXPECTED_PROVIDER_SYMBOLS or len(symbols) != len(set(symbols)):
            _add_failure(failures, "PROVIDER_SYMBOL_SET_MISMATCH")
        if symbol_count != len(symbols):
            _add_failure(failures, "PROVIDER_SYMBOL_SET_MISMATCH")
    if _get(proof, "rustArchiveResult") != "dumpbin-defined-exports-verified":
        _add_failure(failures, "STATIC_RUST_ARCHIVE_UNPROVEN")
    if manifest_configuration == "Debug":
        unresolved = _get(proof, "unresolvedProviderSymbols")
        unresolved_count = _int_value(_get(proof, "unresolvedProviderSymbolCount"))
        expected_unresolved = sorted(_EXPECTED_PROVIDER_SYMBOLS) if expected_backend == "rust" else []
        if not isinstance(unresolved, list) or any(not isinstance(item, str) for item in unresolved):
            _add_failure(failures, "SELECTOR_UNRESOLVED_SYMBOL_UNPROVEN")
        else:
            normalized_unresolved = sorted((item.strip().lower() for item in unresolved))
            if normalized_unresolved != expected_unresolved or len(normalized_unresolved) != len(set(normalized_unresolved)):
                _add_failure(failures, "SELECTOR_UNRESOLVED_SYMBOL_SET_MISMATCH")
            if unresolved_count != len(normalized_unresolved):
                _add_failure(failures, "SELECTOR_UNRESOLVED_SYMBOL_COUNT_MISMATCH")
    if not values or output_backend is None or utf16_backend is None:
        _add_failure(failures, "SELECTOR_PROOF_INCOMPLETE")
    return selector, proof


def _native_selector_descriptor(
    native: Mapping[str, object], expected_backend: str, failures: list[str]
) -> None:
    proof = _mapping(_get(native, "selectorProof", "selector_proof"))
    raw_values = {
        "outputBackend": (_get(native, "outputBackend"), _get(proof, "outputBackend")),
        "utf16Backend": (_get(native, "utf16Backend"), _get(proof, "utf16Backend")),
        "outputProductionPackage": (
            _get(native, "outputProductionPackage"),
            _get(proof, "outputProductionPackage"),
        ),
        "utf16ProductionPackage": (
            _get(native, "utf16ProductionPackage"),
            _get(proof, "utf16ProductionPackage"),
        ),
    }
    if all(all(value is None for value in values) for values in raw_values.values()):
        return
    for field, values in raw_values.items():
        for value in values:
            if value is None:
                continue
            if field in {"outputBackend", "utf16Backend"}:
                if not isinstance(value, str) or value.strip().lower() not in {"cpp", "rust"}:
                    _add_failure(failures, "NATIVE_SELECTOR_UNPROVEN")
            elif _bool_value(value) is None:
                _add_failure(failures, "NATIVE_SELECTOR_UNPROVEN")
    output_backend_values = [
        str(value).strip().lower() for value in raw_values["outputBackend"] if isinstance(value, str)
    ]
    utf16_backend_values = [
        str(value).strip().lower() for value in raw_values["utf16Backend"] if isinstance(value, str)
    ]
    output_package_values = [
        parsed for value in raw_values["outputProductionPackage"] if (parsed := _bool_value(value)) is not None
    ]
    utf16_package_values = [
        parsed for value in raw_values["utf16ProductionPackage"] if (parsed := _bool_value(value)) is not None
    ]
    output_backend = output_backend_values[0] if output_backend_values else None
    utf16_backend = utf16_backend_values[0] if utf16_backend_values else None
    output_package = output_package_values[0] if output_package_values else None
    utf16_package = utf16_package_values[0] if utf16_package_values else None
    if any(len(values) > 1 and len(set(values)) != 1 for values in (
        output_backend_values,
        utf16_backend_values,
        output_package_values,
        utf16_package_values,
    )):
        _add_failure(failures, "SELECTOR_MISMATCH")
    if output_backend is not None and output_backend != expected_backend:
        _add_failure(failures, "SELECTOR_MISMATCH")
    if utf16_backend is not None and utf16_backend != "cpp":
        _add_failure(failures, "SELECTOR_MISMATCH")
    if output_package is not None and output_package is not False:
        _add_failure(failures, "PRODUCTION_PACKAGE_UNPROVEN")
    if utf16_package is not None and utf16_package is not False:
        _add_failure(failures, "PRODUCTION_PACKAGE_UNPROVEN")


def _native_hard_hash_matches(native: Mapping[str, object]) -> bool:
    observed = _normalise_hash(_get(native, "hard_evidence_hash", "hardEvidenceSha256"))
    if observed is None:
        return False
    try:
        return observed == _normalise_hash(product_native_evidence_hash(native))
    except (TypeError, ValueError):
        return False


def _validate_native_input_shape(native: Mapping[str, object], failures: list[str]) -> None:
    if native.get("schema_version") != _NATIVE_EVIDENCE_SCHEMA_VERSION:
        _add_failure(failures, "NATIVE_EVIDENCE_SCHEMA")


def _validate_manifest_input_shape(manifest: Mapping[str, object], failures: list[str]) -> None:
    if (
        manifest.get("schemaVersion") != _MANIFEST_SCHEMA_VERSION
        or manifest.get("record") not in _MANIFEST_RECORDS
        or manifest.get("payloadFree") is not True
        or manifest.get("status") != "committed"
    ):
        _add_failure(failures, "MANIFEST_SCHEMA")


def _native_link_command_hash(native: Mapping[str, object]) -> str | None:
    link = _mapping(_get(native, "link"))
    value = _normalise_hash(_get(link, "linkCommandSha256", "commandSha256", "command_hash"))
    if value:
        return value
    trackers = _mapping(_get(native, "freshness", "freshnessHashes")).get("tracker_inputs")
    if not isinstance(trackers, Mapping):
        return None
    matches = [
        _normalise_hash(item)
        for name, item in trackers.items()
        if isinstance(name, str) and re.search(r"(?:^|[\\/])link\.command\.[^\\/]+\.tlog$", name, re.IGNORECASE)
    ]
    matches = [item for item in matches if item is not None]
    return matches[0] if len(matches) == 1 else None


def _validate_native_final_image_stage(
    native: Mapping[str, object],
    repo_root: Path | None,
    backend: str,
    platform: str | None,
    configuration: str | None,
    failures: list[str],
    *,
    required: bool,
) -> Mapping[str, object] | None:
    link = _mapping(_get(native, "link"))
    stage = _mapping(_get(link, "final_image_stage", "finalImageStage"))
    if not stage:
        if required:
            _add_failure(failures, "FINAL_IMAGE_STAGE_UNPROVEN")
        return None
    if set(stage) != {"record", "receipt", "receiptSha256", "sourceNativeEvidenceSha256"} or stage.get("record") != FINAL_IMAGE_RECORD_KIND:
        _add_failure(failures, "FINAL_IMAGE_STAGE_UNPROVEN")
        return None
    receipt_reference = _get(stage, "receipt", "receiptPath")
    receipt_hash = _normalise_hash(_get(stage, "receiptSha256", "receipt_hash"))
    source_hash = _normalise_hash(_get(stage, "sourceNativeEvidenceSha256", "source_native_evidence_sha256"))
    if not isinstance(receipt_reference, str) or not receipt_reference or receipt_hash is None or source_hash is None:
        _add_failure(failures, "FINAL_IMAGE_STAGE_UNPROVEN")
        return None
    if repo_root is None:
        _add_failure(failures, "FINAL_IMAGE_STAGE_UNPROVEN")
        return None
    try:
        source_native_hash = _normalise_hash(product_native_evidence_source_hash(native))
    except (TypeError, ValueError):
        source_native_hash = None
    if source_native_hash is None or source_native_hash != source_hash:
        _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
    try:
        summary = validate_output_final_image_stage(
            repo_root / receipt_reference,
            repo_root=repo_root,
            expected_backend=backend,
            expected_platform=platform,
            expected_configuration=configuration,
            expected_native_hash=source_hash,
        )
    except OutputFinalImageEvidenceError as error:
        code = {
            "OUTPUT_FINAL_IMAGE_RECEIPT_PATH_UNSAFE": "FINAL_IMAGE_STAGE_PATH_UNSAFE",
            "OUTPUT_FINAL_IMAGE_FILE_PATH_UNSAFE": "FINAL_IMAGE_STAGE_PATH_UNSAFE",
            "OUTPUT_FINAL_IMAGE_SOURCE_PATH_UNSAFE": "FINAL_IMAGE_STAGE_PATH_UNSAFE",
            "OUTPUT_FINAL_IMAGE_SELECTOR_MISMATCH": "FINAL_IMAGE_STAGE_SELECTOR_MISMATCH",
            "OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH": "FINAL_IMAGE_STAGE_BINDING_MISMATCH",
            "OUTPUT_FINAL_IMAGE_TAMPERED": "FINAL_IMAGE_STAGE_TAMPERED",
        }.get(error.code, "FINAL_IMAGE_STAGE_UNPROVEN")
        _add_failure(failures, code)
        return None
    if _normalise_hash(summary["receiptSha256"]) != receipt_hash:
        _add_failure(failures, "FINAL_IMAGE_STAGE_TAMPERED")
    if _normalise_hash(summary["files"]["exe"]["sha256"]) != _normalise_hash(_get(link, "product_hash", "productSha256", "outputSha256")):
        _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
    if summary["files"]["exe"]["sizeBytes"] != _int_value(_get(link, "product_size_bytes", "productSizeBytes", "outputSizeBytes")):
        _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
    output_reference = _get(link, "output", "productPath", "outputPath")
    if not isinstance(output_reference, str) or Path(output_reference).name.casefold() != "sakura.exe":
        _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
    elif repo_root is not None:
        try:
            # Original identity paths need not still exist, but they must be
            # lexical repository-relative paths without a reparse ancestor.
            safe_repository_path(
                repo_root,
                output_reference,
                code="OUTPUT_LINK_SIZE_PATH_UNSAFE",
                reject_parent_segments=True,
                reject_absolute=True,
            )
        except RepositoryPathSafetyError:
            _add_failure(failures, "FINAL_IMAGE_STAGE_PATH_UNSAFE")
    for key in (
        "selected_archive_member_evidence",
        "output_provider_member_evidence",
        "output_provider_symbol_evidence",
        "selectedArchiveMemberEvidence",
        "outputProviderMemberEvidence",
        "outputProviderSymbolEvidence",
    ):
        value = link.get(key)
        if not isinstance(value, Mapping):
            continue
        map_reference = _get(value, "map", "mapPath")
        if not isinstance(map_reference, str) or not map_reference:
            _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
        elif repo_root is not None:
            try:
                safe_repository_path(
                    repo_root,
                    map_reference,
                    code="OUTPUT_LINK_SIZE_PATH_UNSAFE",
                    reject_parent_segments=True,
                    reject_absolute=True,
                )
            except RepositoryPathSafetyError:
                _add_failure(failures, "FINAL_IMAGE_STAGE_PATH_UNSAFE")
        if _normalise_hash(_get(value, "map_hash", "mapHash", "mapSha256")) != _normalise_hash(summary["files"]["map"]["sha256"]):
            _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
        for size_key in ("map_size_bytes", "mapSizeBytes"):
            if size_key in value and value.get(size_key) != summary["files"]["map"]["sizeBytes"]:
                _add_failure(failures, "FINAL_IMAGE_STAGE_BINDING_MISMATCH")
    if _normalise_hash(stage.get("sourceNativeEvidenceSha256")) != _normalise_hash(summary["sourceNativeEvidenceSha256"]):
        _add_failure(failures, "FINAL_IMAGE_STAGE_TAMPERED")
    return summary


def _native_output_identity(
    native: Mapping[str, object], repo_root: Path | None, failures: list[str], *, stage_summary: Mapping[str, object] | None = None
) -> tuple[str | None, int | None]:
    link = _mapping(_get(native, "link"))
    reference = _get(link, "output", "productPath", "outputPath")
    observed_hash = _normalise_hash(_get(link, "product_hash", "productSha256", "outputSha256"))
    observed_size = _int_value(_get(link, "product_size_bytes", "productSizeBytes", "outputSizeBytes"))
    if _get(native, "product_id", "productId") != "sakura_app":
        _add_failure(failures, "FINAL_IMAGE_UNPROVEN")
    if not isinstance(reference, str) or Path(reference).name.casefold() != "sakura.exe":
        _add_failure(failures, "FINAL_IMAGE_UNPROVEN")
    if reference is None:
        _add_failure(failures, "FINAL_IMAGE_UNPROVEN")
    if stage_summary is not None:
        staged_files = _mapping(stage_summary.get("files"))
        staged_exe = _mapping(staged_files.get("exe"))
        staged_hash = _normalise_hash(staged_exe.get("sha256"))
        staged_size = _int_value(staged_exe.get("sizeBytes"))
        if staged_hash is None or staged_size is None:
            _add_failure(failures, "FINAL_IMAGE_STAGE_UNPROVEN")
        else:
            return staged_hash, staged_size
    if reference is not None and repo_root is not None:
        identity = _file_identity(repo_root, reference)
        if identity is None:
            _add_failure(failures, "FINAL_IMAGE_PATH_UNSAFE")
        else:
            actual_hash, actual_size = identity
            if observed_hash is not None and observed_hash != actual_hash:
                _add_failure(failures, "FINAL_IMAGE_TAMPERED")
            if observed_size is not None and observed_size != actual_size:
                _add_failure(failures, "FINAL_IMAGE_TAMPERED")
            observed_hash = observed_hash or actual_hash
            observed_size = observed_size if observed_size is not None else actual_size
    return observed_hash, observed_size


def _manifest_output_identity(manifest: Mapping[str, object], failures: list[str]) -> tuple[str | None, int | None]:
    # Provider manifests identify tests1.exe, not the final GUI image.  Only
    # the startup producer owns a final-image identity; never infer it from a
    # generic artifact list or a provider manifest's tests1 hash.
    if manifest.get("record") != "output-startup-build-manifest":
        return None, None
    hash_aliases = ("exeSha256", "artifactSha256", "artifactSha256After", "artifactHashAfter")
    size_aliases = ("artifactSizeBytesAfter", "artifactSizeAfter")
    present_hashes = [_normalise_hash(manifest[name]) for name in hash_aliases if name in manifest]
    present_sizes = [_int_value(manifest[name]) for name in size_aliases if name in manifest]
    if not present_hashes or any(value is None for value in present_hashes) or not present_sizes or any(value is None for value in present_sizes):
        _add_failure(failures, "FINAL_IMAGE_MANIFEST_IDENTITY_UNPROVEN")
        return None, None
    if len(set(present_hashes)) != 1 or len(set(present_sizes)) != 1:
        _add_failure(failures, "FINAL_IMAGE_MANIFEST_IDENTITY_MISMATCH")
    return present_hashes[0], present_sizes[0]


def _provider_member_evidence(
    native: Mapping[str, object],
    repo_root: Path | None,
    failures: list[str],
    *,
    expected_backend: str,
) -> tuple[str | None, int | None, int | None, str | None, str | None]:
    link = _mapping(_get(native, "link"))
    member_evidence = _mapping(
        _get(
            link,
            "output_provider_member_evidence",
            "outputProviderMemberEvidence",
            "provider_member_evidence",
            "providerMemberEvidence",
        )
    )
    if not member_evidence:
        member_evidence = _mapping(
            _get(native, "output_provider_member_evidence", "outputProviderMemberEvidence")
        )
    observed = _get(member_evidence, "observed", "providerSpecific", "outputProvider") is True
    symbol_evidence = _mapping(
        _get(
            link,
            "output_provider_symbol_evidence",
            "outputProviderSymbolEvidence",
            "provider_symbol_evidence",
            "providerSymbolEvidence",
        )
    )
    if not symbol_evidence:
        symbol_evidence = _mapping(_get(native, "output_provider_symbol_evidence", "outputProviderSymbolEvidence"))
    negative_cpp = (
        expected_backend == "cpp"
        and _get(member_evidence, "observed", "providerSpecific", "outputProvider") is False
        and _get(symbol_evidence, "observed", "providerSpecific", "outputProvider") is False
    )
    if not observed and not negative_cpp:
        _add_failure(failures, "PROVIDER_MEMBER_UNPROVEN")
    map_reference = _get(member_evidence, "map", "mapPath")
    map_hash = _normalise_hash(_get(member_evidence, "map_hash", "mapHash", "mapSha256"))
    map_size = _int_value(_get(member_evidence, "map_size_bytes", "mapSizeBytes"))
    if map_reference is None:
        _add_failure(failures, "MAP_UNPROVEN")
    elif repo_root is not None:
        identity = _file_identity(repo_root, map_reference)
        if identity is None:
            _add_failure(failures, "MAP_PATH_UNSAFE")
        else:
            actual_hash, actual_size = identity
            if map_hash is not None and map_hash != actual_hash:
                _add_failure(failures, "MAP_HASH_MISMATCH")
            if map_size is not None and map_size != actual_size:
                _add_failure(failures, "MAP_HASH_MISMATCH")
            map_hash = map_hash or actual_hash
            map_size = map_size if map_size is not None else actual_size
    if map_hash is None or map_size is None:
        _add_failure(failures, "MAP_UNPROVEN")

    members = _get(member_evidence, "members", "selectedMembers")
    member_count = _int_value(_get(member_evidence, "member_count", "memberCount"))
    member_hash: str | None = None
    if not isinstance(members, list) or (not members and not negative_cpp) or len(members) > MAX_MEMBER_COUNT:
        _add_failure(failures, "PROVIDER_MEMBER_UNPROVEN")
    elif any(not isinstance(item, str) or not item.strip() for item in members):
        _add_failure(failures, "PROVIDER_MEMBER_UNPROVEN")
    else:
        normalized = [item.strip().replace("\\", "/") for item in members]
        if len(normalized) != len(set(normalized)):
            _add_failure(failures, "SELECTED_MEMBER_DUPLICATE")
        else:
            member_hash = _sha256_bytes("\n".join(sorted(normalized, key=str.casefold)).encode("utf-8"))
            if member_count != len(normalized):
                _add_failure(failures, "SELECTED_MEMBER_COUNT_MISMATCH")
    if negative_cpp and members == [] and member_count != 0:
        _add_failure(failures, "SELECTED_MEMBER_COUNT_MISMATCH")
    scope = _get(member_evidence, "archive", "library", "provider")
    if not isinstance(scope, str) or scope.strip().lower() not in {"provider", "output-provider", "output_provider"}:
        _add_failure(failures, "PROVIDER_MEMBER_SCOPE_UNPROVEN")
    return map_hash, map_size, member_count, member_hash, str(scope).strip() if isinstance(scope, str) else None


def _archive_identity(
    native: Mapping[str, object], proof: Mapping[str, object], failures: list[str]
) -> tuple[str | None, int | None, int | None]:
    # Selector-proof archive fields describe the archive/export contract, but
    # do not establish which member was selected for the final image.  Use
    # only native link observations for the archive identity and count.
    link = _mapping(_get(native, "link"))
    archive_hash = _normalise_hash(_get(link, "rustArchiveSha256", "staticRustArchiveSha256"))
    archive_size = _int_value(_get(link, "rustArchiveSizeBytes", "staticRustArchiveSizeBytes"))
    proof_hash = _normalise_hash(_get(proof, "rustArchiveSha256", "staticRustArchiveSha256"))
    proof_size = _int_value(_get(proof, "rustArchiveSizeBytes", "staticRustArchiveSizeBytes"))
    if proof_hash is None or proof_size is None:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_UNPROVEN")
    if archive_hash is not None and proof_hash is not None and archive_hash != proof_hash:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_MISMATCH")
    if archive_size is not None and proof_size is not None and archive_size != proof_size:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_MISMATCH")
    explicit_count = _int_value(_get(link, "rustArchiveCount", "staticRustArchiveCount", "archiveCount"))
    observed_count: int | None = None
    libraries = _get(link, "repository_libraries", "repositoryLibraries", "libraries")
    if isinstance(libraries, list):
        observed_count = sum(
            1
            for item in libraries
            if isinstance(item, str) and Path(item.replace("\\", "/")).name.casefold() == "sakura_native_ffi.lib"
        )
    count = explicit_count if explicit_count is not None else observed_count
    if explicit_count is not None and observed_count is not None and explicit_count != observed_count:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_MISMATCH")
    if count is None:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_COUNT_UNPROVEN")
    elif count == 0:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_COUNT_UNPROVEN")
    elif count > 1:
        _add_failure(failures, "DUPLICATE_STATIC_RUST_ARCHIVE")
    if observed_count is not None and observed_count > 1:
        _add_failure(failures, "DUPLICATE_STATIC_RUST_ARCHIVE")
    if archive_hash is None or archive_size is None:
        _add_failure(failures, "STATIC_RUST_ARCHIVE_UNPROVEN")
    return archive_hash, archive_size, count


def _provider_symbols(
    native: Mapping[str, object], failures: list[str], *, expected_backend: str
) -> tuple[int | None, int | None]:
    link = _mapping(_get(native, "link"))
    evidence = _mapping(
        _get(
            link,
            "output_provider_symbol_evidence",
            "outputProviderSymbolEvidence",
            "provider_symbol_evidence",
            "providerSymbolEvidence",
        )
    )
    if not evidence:
        evidence = _mapping(_get(native, "output_provider_symbol_evidence", "outputProviderSymbolEvidence"))
    link_member = _mapping(
        _get(
            link,
            "output_provider_member_evidence",
            "outputProviderMemberEvidence",
            "provider_member_evidence",
            "providerMemberEvidence",
        )
    )
    if not link_member:
        link_member = _mapping(_get(native, "output_provider_member_evidence", "outputProviderMemberEvidence"))
    negative_cpp = (
        expected_backend == "cpp"
        and _get(evidence, "observed", "providerSpecific", "outputProvider") is False
        and _get(link_member, "observed", "providerSpecific", "outputProvider") is False
    )
    if _get(evidence, "observed", "providerSpecific", "outputProvider") is not True and not negative_cpp:
        _add_failure(failures, "FINAL_PROVIDER_SYMBOL_UNPROVEN")
    scope = _get(evidence, "scope", "provider", "kind")
    if not isinstance(scope, str) or scope.strip().lower() not in {"provider", "output-provider", "output_provider"}:
        _add_failure(failures, "FINAL_PROVIDER_SYMBOL_SCOPE_UNPROVEN")
    symbols = _get(evidence, "symbols", "definedSymbols", "definedProviderSymbols")
    symbol_count = _int_value(_get(evidence, "symbol_count", "symbolCount", "definedProviderSymbolCount"))
    if negative_cpp and symbols == []:
        if symbol_count != 0:
            _add_failure(failures, "PROVIDER_SYMBOL_SET_MISMATCH")
        duplicate = _int_value(_get(evidence, "duplicate_count", "duplicateCount", "duplicateProviderSymbolCount"))
        if duplicate is None:
            _add_failure(failures, "PROVIDER_SYMBOL_DUPLICATE_COUNT_UNPROVEN")
        elif duplicate > 0:
            _add_failure(failures, "DUPLICATE_PROVIDER_SYMBOLS")
        return 0, duplicate
    if not isinstance(symbols, list) or any(not isinstance(item, str) for item in symbols):
        _add_failure(failures, "FINAL_PROVIDER_SYMBOL_UNPROVEN")
        symbol_count = None
    else:
        symbols_invalid = set(symbols) != _EXPECTED_PROVIDER_SYMBOLS or len(symbols) != len(set(symbols))
        if symbols_invalid:
            _add_failure(failures, "PROVIDER_SYMBOL_SET_MISMATCH")
        if symbol_count != len(symbols):
            _add_failure(failures, "PROVIDER_SYMBOL_SET_MISMATCH")
            symbols_invalid = True
        symbol_count = None if symbols_invalid else len(symbols)
    duplicate = _int_value(_get(evidence, "duplicate_count", "duplicateCount", "duplicateProviderSymbolCount"))
    if duplicate is None:
        _add_failure(failures, "PROVIDER_SYMBOL_DUPLICATE_COUNT_UNPROVEN")
    elif duplicate > 0:
        _add_failure(failures, "DUPLICATE_PROVIDER_SYMBOLS")
    return symbol_count, duplicate


def _record_provider_contract_failures(
    validation: object, failures: list[str]
) -> None:
    """Project native final-image provider failures into this report's codes.

    ``product_native_evidence`` is the source of truth for the full provider
    projection contract.  This report deliberately exposes only its bounded
    failure vocabulary, so retain the category without copying the native
    payload or silently accepting a malformed negative C++ projection.
    """

    if not isinstance(validation, Mapping) or validation.get("valid") is not True:
        provider_failures = (
            validation.get("failures")
            if isinstance(validation, Mapping)
            else None
        )
        codes = {
            item.get("code")
            for item in provider_failures
            if isinstance(item, Mapping) and isinstance(item.get("code"), str)
        } if isinstance(provider_failures, list) else set()
        mapped = False
        if any("SYMBOL" in code for code in codes):
            _add_failure(failures, "FINAL_PROVIDER_SYMBOL_UNPROVEN")
            mapped = True
        if any("MEMBER" in code or "UNPROVEN" in code for code in codes):
            _add_failure(failures, "PROVIDER_MEMBER_UNPROVEN")
            mapped = True
        if any("MAP" in code for code in codes):
            _add_failure(failures, "MAP_UNPROVEN")
            mapped = True
        if not mapped:
            _add_failure(failures, "PROVIDER_MEMBER_UNPROVEN")
            _add_failure(failures, "FINAL_PROVIDER_SYMBOL_UNPROVEN")


def _consensus(values: Sequence[object]) -> object:
    present = [value for value in values if value is not None]
    if len(present) != len(values) or not present or any(value != present[0] for value in present[1:]):
        return None
    return present[0]


def _validate_report_scalars(report: Mapping[str, object]) -> None:
    def require_keys(value: Mapping[str, object], expected: frozenset[str], label: str) -> None:
        if set(value) != expected:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"{label} fields do not match schema v1")

    require_keys(report, _REPORT_KEYS, "report")
    if report.get("schemaVersion") != SCHEMA_VERSION or report.get("record") != RECORD_KIND:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report schema or kind is invalid")
    if report.get("payloadFree") is not True or report.get("decision") != "HOLD" or report.get("adoptionEligible") is not False:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_POLICY", "report is not an explicit HOLD payload-free record")
    if report.get("status") not in {"complete", "incomplete"}:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report status is invalid")
    source = report.get("source")
    if not isinstance(source, Mapping):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report source is invalid")
    require_keys(source, _SOURCE_KEYS, "report.source")
    commit = source.get("commit")
    if commit is not None and _normalise_commit(commit) != commit:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report source commit is invalid")
    status_hash = source.get("statusSha256")
    if status_hash is not None and _normalise_hash(status_hash) != status_hash:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report source status hash is invalid")
    if source.get("dirty") is not None and not isinstance(source.get("dirty"), bool):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report source dirty flag is invalid")
    if not isinstance(source.get("complete"), bool):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report source completeness is invalid")
    if source.get("complete") and (commit is None or status_hash is None or source.get("dirty") is not False):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "complete source has missing provenance")
    if report.get("status") == "complete" and source.get("complete") is not True:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "complete report has incomplete source")
    selectors = report.get("selectors")
    if not isinstance(selectors, Mapping) or set(selectors) != set(_EXPECTED_BACKENDS):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report selectors are invalid")
    for backend in _EXPECTED_BACKENDS:
        selector = selectors[backend]
        if not isinstance(selector, Mapping):
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report selector is invalid")
        require_keys(selector, _SELECTOR_KEYS, f"report.selectors.{backend}")
        for key in ("outputBackend", "utf16Backend", "proofResult"):
            if selector.get(key) is not None and not isinstance(selector.get(key), str):
                raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report selector {key} is invalid")
        for key in ("outputProductionPackage", "utf16ProductionPackage"):
            if selector.get(key) is not None and not isinstance(selector.get(key), bool):
                raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report selector {key} is invalid")
        proof_hash = selector.get("proofSha256")
        if proof_hash is not None and _normalise_hash(proof_hash) != proof_hash:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report selector proof hash is invalid")
        define_count = selector.get("compileRustSelectorDefineCount")
        if define_count is not None and _int_value(define_count) is None:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report selector define count is invalid")
    for group_name in ("images", "link"):
        group = report.get(group_name)
        if not isinstance(group, Mapping) or set(group) != set(_EXPECTED_BACKENDS):
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report {group_name} are invalid")
        expected = _IMAGE_KEYS if group_name == "images" else _LINK_KEYS
        for backend in _EXPECTED_BACKENDS:
            value = group[backend]
            if not isinstance(value, Mapping):
                raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report {group_name} entry is invalid")
            require_keys(value, expected, f"report.{group_name}.{backend}")
            for key in value:
                field = value[key]
                if key.endswith("Sha256") or key == "selectedMemberSetSha256":
                    if field is not None and _normalise_hash(field) != field:
                        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report {group_name} hash is invalid")
                elif key.endswith("Bytes") or key.endswith("Count") or key == "selectedMemberCount":
                    if field is not None and _int_value(field) is None:
                        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report {group_name} count is invalid")
    size_gate = report.get("sizeGate")
    if not isinstance(size_gate, Mapping):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size gate is invalid")
    require_keys(size_gate, _SIZE_GATE_KEYS, "report.sizeGate")
    inputs = report.get("inputs")
    if not isinstance(inputs, Mapping):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report inputs are invalid")
    require_keys(inputs, _INPUT_KEYS, "report.inputs")
    failures = report.get("failures")
    if not isinstance(failures, list) or len(failures) > MAX_FAILURES or any(item not in _FAILURE_CODES for item in failures):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report failures are invalid")
    if (report.get("status") == "complete") != (len(failures) == 0):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report status and failures disagree")
    if report.get("platform") is not None and (not isinstance(report.get("platform"), str) or report["platform"] != "x64"):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report platform is invalid")
    if report.get("configuration") is not None and report.get("configuration") not in _EXPECTED_CONFIGURATIONS:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report configuration is invalid")
    if report.get("status") == "complete" and (report.get("platform") is None or report.get("configuration") is None):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "complete report has no build context")
    for key in _INPUT_KEYS:
        if _normalise_hash(inputs.get(key)) != inputs.get(key):
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report input hash {key} is invalid")
    threshold = _float_value(size_gate.get("thresholdPercent"))
    if threshold is None or threshold < 0 or threshold > 100:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size threshold is invalid")
    for key in ("cppSizeBytes", "rustSizeBytes"):
        value = size_gate.get(key)
        if value is not None and _int_value(value) is None:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", f"report size gate {key} is invalid")
    delta_bytes = size_gate.get("deltaBytes")
    if delta_bytes is not None and _signed_int_value(delta_bytes) is None:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size gate deltaBytes is invalid")
    delta_percent = size_gate.get("deltaPercent")
    if delta_percent is not None and _float_value(delta_percent) is None:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size delta is invalid")
    if not isinstance(size_gate.get("pass"), bool):
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size gate result is invalid")
    if report.get("status") == "incomplete" and size_gate.get("pass") is not False:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "incomplete report cannot pass size gate")
    if report.get("status") == "complete":
        if size_gate.get("cppSizeBytes") is None or size_gate.get("rustSizeBytes") is None:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "complete report has no image sizes")
        if size_gate["cppSizeBytes"] <= 0:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "complete report has invalid C++ image size")
        if size_gate.get("deltaBytes") != size_gate["rustSizeBytes"] - size_gate["cppSizeBytes"]:
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size delta is inconsistent")
        expected_percent = 100.0 * size_gate["deltaBytes"] / size_gate["cppSizeBytes"]
        if size_gate["cppSizeBytes"] <= 0 or size_gate.get("deltaPercent") is None or not math.isclose(
            float(size_gate["deltaPercent"]), expected_percent, rel_tol=1e-12, abs_tol=1e-9
        ):
            raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_SCHEMA", "report size percentage is inconsistent")
    without_hash = {key: value for key, value in report.items() if key != "reportSha256"}
    expected_hash = _sha256_json(without_hash)
    if _normalise_hash(report.get("reportSha256")) != expected_hash:
        raise OutputLinkSizeEvidenceError("OUTPUT_LINK_SIZE_HASH", "report hash does not match canonical content")


def validate_output_link_size_evidence(report: Mapping[str, object]) -> dict[str, object]:
    """Validate one generated report and return a bounded summary."""

    _validate_report_scalars(report)
    return {
        "ok": report["status"] == "complete" and report["sizeGate"]["pass"] is True,
        "status": report["status"],
        "sizeGatePass": report["sizeGate"]["pass"],
        "failureCount": len(report["failures"]),
        "decision": report["decision"],
        "adoptionEligible": report["adoptionEligible"],
    }


def build_output_link_size_evidence(
    cpp_native: Path | Mapping[str, object],
    rust_native: Path | Mapping[str, object],
    cpp_manifest: Path | Mapping[str, object],
    rust_manifest: Path | Mapping[str, object],
    *,
    repo_root: Path | None = None,
    threshold_percent: float = DEFAULT_SIZE_THRESHOLD_PERCENT,
    require_immutable_stage: bool = False,
) -> dict[str, object]:
    """Build a bounded comparison report from four existing evidence inputs."""

    threshold = _float_value(threshold_percent)
    if threshold is None or threshold < 0 or threshold > 100:
        raise OutputLinkSizeEvidenceError("THRESHOLD_INVALID", "size threshold must be between 0 and 100")
    native_values: dict[str, dict[str, object]] = {}
    manifest_values: dict[str, dict[str, object]] = {}
    input_hashes: dict[str, str] = {}
    for backend, native_source, manifest_source in (
        ("cpp", cpp_native, cpp_manifest),
        ("rust", rust_native, rust_manifest),
    ):
        native, native_hash = _load_source(native_source)
        manifest, manifest_hash = _load_source(manifest_source)
        native_values[backend] = native
        manifest_values[backend] = manifest
        input_hashes[f"{backend}NativeSha256"] = native_hash
        input_hashes[f"{backend}ManifestSha256"] = manifest_hash

    failures: list[str] = []
    for backend in _EXPECTED_BACKENDS:
        _validate_native_input_shape(native_values[backend], failures)
        _validate_manifest_input_shape(manifest_values[backend], failures)
    manifest_source_descriptors = [_source_descriptor(manifest_values[backend]) for backend in _EXPECTED_BACKENDS]
    native_source_descriptors = [_source_descriptor(native_values[backend]) for backend in _EXPECTED_BACKENDS]
    source_commit = _consensus([item["commit"] for item in manifest_source_descriptors])
    source_dirty = _consensus([item["dirty"] for item in manifest_source_descriptors])
    source_status_hash = _consensus([item["statusSha256"] for item in manifest_source_descriptors])
    if source_commit is None or source_dirty is None or source_status_hash is None:
        _add_failure(failures, "SOURCE_PROVENANCE_UNPROVEN")
    if len({item["commit"] for item in manifest_source_descriptors if item["commit"] is not None}) > 1:
        _add_failure(failures, "SOURCE_MISMATCH")
    if len({item["statusSha256"] for item in manifest_source_descriptors if item["statusSha256"] is not None}) > 1:
        _add_failure(failures, "SOURCE_MISMATCH")
    if len({item["dirty"] for item in manifest_source_descriptors if item["dirty"] is not None}) > 1:
        _add_failure(failures, "SOURCE_MISMATCH")
    if any(item["dirty"] is True for item in manifest_source_descriptors):
        _add_failure(failures, "SOURCE_DIRTY")
    for native_source in native_source_descriptors:
        if native_source["commit"] is not None and native_source["commit"] != source_commit:
            _add_failure(failures, "SOURCE_MISMATCH")
        if native_source["statusSha256"] is not None and native_source["statusSha256"] != source_status_hash:
            _add_failure(failures, "SOURCE_MISMATCH")
        if native_source["dirty"] is True:
            _add_failure(failures, "SOURCE_DIRTY")
        if native_source["dirty"] is not None and native_source["dirty"] != source_dirty:
            _add_failure(failures, "SOURCE_MISMATCH")

    manifest_contexts = [_context_descriptor(manifest_values[backend]) for backend in _EXPECTED_BACKENDS]
    native_contexts = [_context_descriptor(native_values[backend]) for backend in _EXPECTED_BACKENDS]
    platform = _consensus([item[0] for item in manifest_contexts])
    configuration = _consensus([item[1] for item in manifest_contexts])
    if platform != "x64" or configuration not in _EXPECTED_CONFIGURATIONS:
        _add_failure(failures, "PLATFORM_CONFIGURATION_UNPROVEN")
    for native_context in native_contexts:
        if native_context[0] is not None and native_context[0] != platform:
            _add_failure(failures, "PLATFORM_CONFIGURATION_MISMATCH")
        if native_context[1] is not None and native_context[1] != configuration:
            _add_failure(failures, "PLATFORM_CONFIGURATION_MISMATCH")

    selectors: dict[str, dict[str, object]] = {}
    proofs: dict[str, Mapping[str, object]] = {}
    for backend in _EXPECTED_BACKENDS:
        selectors[backend], proofs[backend] = _selector_descriptor(manifest_values[backend], backend, failures)
        _native_selector_descriptor(native_values[backend], backend, failures)
        native = native_values[backend]
        if _get(native, "collection_ok") is not True or _get(native, "build_observed") is not True:
            _add_failure(failures, "NATIVE_EVIDENCE_UNAVAILABLE")
        if _normalise_hash(_get(native, "hard_evidence_hash", "hardEvidenceSha256")) is None:
            _add_failure(failures, "NATIVE_EVIDENCE_HASH_UNPROVEN")
        elif not _native_hard_hash_matches(native):
            _add_failure(failures, "NATIVE_EVIDENCE_TAMPERED")
        # The MAP projection is not selector proof by itself.  Validate its
        # backend-aware shape here, after the manifest selector above has been
        # checked: Rust remains exact 7/1, while C++ may use only the complete
        # LTCG negative 0/0 projection.
        try:
            provider_validation = validate_output_provider_evidence_for_final_image(
                _get(native, "link"),
                expected_backend=backend,
            )
        except Exception:
            provider_validation = None
        _record_provider_contract_failures(provider_validation, failures)

    stage_summaries: dict[str, Mapping[str, object]] = {}
    for backend in _EXPECTED_BACKENDS:
        stage_summary = _validate_native_final_image_stage(
            native_values[backend],
            repo_root,
            backend,
            platform if isinstance(platform, str) else None,
            configuration if isinstance(configuration, str) else None,
            failures,
            required=require_immutable_stage,
        )
        if stage_summary is not None:
            stage_summaries[backend] = stage_summary

    images: dict[str, dict[str, object]] = {}
    links: dict[str, dict[str, object]] = {}
    for backend in _EXPECTED_BACKENDS:
        native_image_hash, native_image_size = _native_output_identity(
            native_values[backend],
            repo_root,
            failures,
            stage_summary=stage_summaries.get(backend),
        )
        manifest_image_hash, manifest_image_size = _manifest_output_identity(manifest_values[backend], failures)
        if native_image_hash is not None and manifest_image_hash is not None and native_image_hash != manifest_image_hash:
            _add_failure(failures, "FINAL_IMAGE_MISMATCH")
        if native_image_size is not None and manifest_image_size is not None and native_image_size != manifest_image_size:
            _add_failure(failures, "FINAL_IMAGE_MISMATCH")
        # A product-native observation can identify the file it inspected, but
        # it cannot bind that observation to the published startup artifact.
        # Only an output-startup-build-manifest supplies that final-image
        # binding; provider manifests intentionally remain incomplete here.
        if manifest_image_hash is None or manifest_image_size is None:
            _add_failure(failures, "FINAL_IMAGE_UNPROVEN")
        image_hash = manifest_image_hash or native_image_hash
        image_size = manifest_image_size if manifest_image_size is not None else native_image_size
        if image_hash is None or image_size is None:
            _add_failure(failures, "FINAL_IMAGE_UNPROVEN")
        images[backend] = {"sha256": image_hash, "sizeBytes": image_size}

        map_hash, map_size, member_count, member_hash, _scope = _provider_member_evidence(
            native_values[backend],
            None if require_immutable_stage else repo_root,
            failures,
            expected_backend=backend,
        )
        archive_hash, archive_size, archive_count = _archive_identity(native_values[backend], proofs[backend], failures)
        symbol_count, duplicate_count = _provider_symbols(
            native_values[backend], failures, expected_backend=backend
        )
        link_command_hash = _native_link_command_hash(native_values[backend])
        if link_command_hash is None:
            _add_failure(failures, "LINK_COMMAND_UNPROVEN")
        links[backend] = {
            "linkCommandSha256": link_command_hash,
            "mapSha256": map_hash,
            "mapSizeBytes": map_size,
            "staticRustArchiveSha256": archive_hash,
            "staticRustArchiveSizeBytes": archive_size,
            "staticRustArchiveCount": archive_count,
            "selectedMemberCount": member_count,
            "selectedMemberSetSha256": member_hash,
            "providerSymbolCount": symbol_count,
            "duplicateProviderSymbolCount": duplicate_count,
        }

    cpp_size = images["cpp"]["sizeBytes"]
    rust_size = images["rust"]["sizeBytes"]
    delta_bytes: int | None = None
    delta_percent: float | None = None
    gate_pass = False
    if isinstance(cpp_size, int) and isinstance(rust_size, int) and cpp_size > 0:
        delta_bytes = rust_size - cpp_size
        delta_percent = (100.0 * delta_bytes) / cpp_size
        gate_pass = delta_percent <= threshold and not failures

    report: dict[str, object] = {
        "schemaVersion": SCHEMA_VERSION,
        "record": RECORD_KIND,
        "payloadFree": True,
        "status": "complete" if not failures else "incomplete",
        "source": {
            "commit": source_commit,
            "dirty": source_dirty,
            "statusSha256": source_status_hash,
            "complete": source_commit is not None and source_dirty is False and source_status_hash is not None,
        },
        "platform": platform if isinstance(platform, str) else None,
        "configuration": configuration if isinstance(configuration, str) else None,
        "selectors": selectors,
        "images": images,
        "link": links,
        "sizeGate": {
            "thresholdPercent": threshold,
            "cppSizeBytes": cpp_size if isinstance(cpp_size, int) else None,
            "rustSizeBytes": rust_size if isinstance(rust_size, int) else None,
            "deltaBytes": delta_bytes,
            "deltaPercent": delta_percent,
            "pass": gate_pass,
        },
        "inputs": input_hashes,
        "failures": failures,
        "decision": "HOLD",
        "adoptionEligible": False,
    }
    report["reportSha256"] = _sha256_json(report)
    _validate_report_scalars(report)
    return report


def write_output_link_size_evidence(path: Path, report: Mapping[str, object]) -> None:
    """Atomically write a validated report without exposing payload fields."""

    _validate_report_scalars(report)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    data = json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        temporary.write_text(data, encoding="utf-8", newline="\n")
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


__all__ = [
    "DEFAULT_SIZE_THRESHOLD_PERCENT",
    "OutputLinkSizeEvidenceError",
    "build_output_link_size_evidence",
    "validate_output_link_size_evidence",
    "write_output_link_size_evidence",
]
