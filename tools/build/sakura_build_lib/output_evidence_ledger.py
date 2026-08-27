"""Append-only, payload-free evidence ledger for Output adoption.

The ledger is deliberately a small boundary between evidence producers and
the adoption decision.  Producers may write richer reports, but this module
derives a bounded record from the report instead of accepting caller supplied
role labels.  Records contain hashes and typed status summaries only; paths,
commands, source text, and user data never cross this boundary.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Mapping, Sequence

from .runner import BuildError


SCHEMA_VERSION = 1
LEDGER_KIND = "output-adoption-ledger"
RECORD_KIND = "output-adoption-attempt"
GENESIS_HASH = "genesis"
DEFAULT_LEDGER_DIRECTORY = Path("build/evidence/output-adoption-ledger")

MAX_RECORDS = 4096
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_DEPTH = 16
MAX_LIST_ITEMS = 1024
MAX_STRING_LENGTH = 512
MAX_STATUS_RECORDS = 128
MAX_ARTIFACTS = 64

_HASH_RE = re.compile(r"^(?:sha256:)?([0-9a-fA-F]{64})$")
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40,64}$")
_RECORD_FILE_RE = re.compile(r"^record-(\d{8})-([0-9a-f]{16})\.json$")
_TEMP_SUFFIXES = (".tmp", ".partial", ".new", ".lock", ".inprogress")

# These names are rejected even when a producer claims payloadFree=true.  A
# relativePath is permitted in source reports because existing build reports
# use it for artifact identity; it is never copied to a ledger record.
_FORBIDDEN_INPUT_KEYS = frozenset(
    {
        "path",
        "filepath",
        "file_path",
        "directory",
        "directorypath",
        "command",
        "commandline",
        "arguments",
        "argv",
        "text",
        "document",
        "content",
        "body",
        "sourcetext",
        "samplemarkdown",
        "caption",
        "profilename",
        "exception",
        "message",
        "detail",
        "ownerid",
        "channelid",
        "operationid",
        "payload",
    }
)

_RECORD_KEYS = frozenset(
    {
        "schemaVersion",
        "record",
        "payloadFree",
        "attemptSequence",
        "attemptId",
        "sourceEvidenceKind",
        "sourceEvidenceSchemaVersion",
        "sourceEvidenceSha256",
        "previousRecordSha256",
        "backend",
        "backends",
        "platform",
        "configuration",
        "source",
        "selectors",
        "host",
        "toolchain",
        "package",
        "corpus",
        "commands",
        "artifacts",
        "results",
        "decision",
        "adoptionEligible",
        "recordSha256",
    }
)

_SOURCE_KEYS = frozenset({"commit", "dirty", "statusSha256", "complete"})
_SELECTOR_KEYS = frozenset(
    {"outputBackend", "utf16Backend", "outputProductionPackage", "utf16ProductionPackage", "proofResult", "proofSha256"}
)
_HOST_KEYS = frozenset(
    {
        "identitySha256",
        "os",
        "osVersion",
        "architecture",
        "cpuManufacturer",
        "cpuModel",
        "physicalCores",
        "logicalProcessors",
        "complete",
    }
)
_TOOLCHAIN_KEYS = frozenset(
    {
        "msvc",
        "rust",
        "rustLockSha256",
        "packagePlanSha256",
        "buildCommandSha256",
        "packageCommandSha256",
        "runtimeStageCommandSha256",
        "complete",
    }
)
_PACKAGE_KEYS = frozenset({"planSha256", "closureSha256", "status", "productionPackage", "complete"})
_CORPUS_KEYS = frozenset({"sampleSha256", "sampleSizeBytes", "seed", "profilePolicySha256", "complete"})
_COMMAND_KEYS = frozenset(
    {"buildSha256", "packageSha256", "runtimeStageSha256", "measurementSha256", "runnerSha256", "complete"}
)
_ARTIFACT_KEYS = frozenset({"backend", "sha256", "sizeBytes"})
_RESULT_KEYS = frozenset(
    {
        "sourceStatus",
        "pass",
        "startupGatePass",
        "acceptanceQualified",
        "performancePassed",
        "failureType",
        "terminationType",
        "terminationStatus",
        "cleanupVerified",
        "survivorCount",
        "failedLaunches",
        "skippedLaunches",
        "statuses",
        "statusesTruncated",
    }
)
_STATUS_KEYS = frozenset({"field", "value"})


class OutputEvidenceLedgerError(BuildError):
    """Typed input, append, or verification failure for the evidence ledger."""

    def __init__(self, code: str, message: str, exit_code: int = 5) -> None:
        super().__init__(code, message, exit_code)


def canonical_json_bytes(value: object) -> bytes:
    """Return the one canonical UTF-8 representation used for all hashes."""

    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError, UnicodeEncodeError) as error:
        raise OutputEvidenceLedgerError("OUTPUT_LEDGER_CANONICAL_JSON", "value is not canonical JSON", 5) from error


def canonical_json_sha256(value: object) -> str:
    """Hash canonical JSON and return a lowercase hexadecimal SHA-256 value."""

    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


# Short aliases make the helper convenient for build-contract tests while the
# descriptive names remain the public API used by the CLI.
sha256_json = canonical_json_sha256


def _fail(code: str, message: str) -> None:
    raise OutputEvidenceLedgerError(code, message, 5)


def _mapping(value: object, label: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} must be an object")
    return value


def _get(value: object, *names: str) -> object | None:
    if not isinstance(value, Mapping):
        return None
    for name in names:
        if name in value:
            return value[name]
    return None


def _nested(value: object, *names: str) -> object | None:
    current: object | None = value
    for name in names:
        current = _get(current, name)
        if current is None:
            return None
    return current


def _scalar(value: object, label: str, *, allow_none: bool = True) -> object | None:
    if value is None:
        if allow_none:
            return None
        _fail("OUTPUT_LEDGER_FIELD_MISSING", f"{label} is required")
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and not isinstance(value, bool):
        if abs(value) > 2**63 - 1:
            _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} is out of range")
        return value
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")) or abs(value) > 2**63 - 1:
            _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} is out of range")
        return value
    if isinstance(value, str):
        if not value or len(value) > MAX_STRING_LENGTH or any(ord(ch) < 0x20 for ch in value):
            _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} is not a bounded scalar")
        return value
    _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} must be a bounded scalar")
    return None


def _normalise_hash(value: object, label: str, *, allow_none: bool = True) -> str | None:
    if value is None:
        if allow_none:
            return None
        _fail("OUTPUT_LEDGER_HASH_MISSING", f"{label} is required")
    if not isinstance(value, str):
        _fail("OUTPUT_LEDGER_HASH_INVALID", f"{label} is not a SHA-256 value")
    match = _HASH_RE.fullmatch(value.strip())
    if not match:
        _fail("OUTPUT_LEDGER_HASH_INVALID", f"{label} is not a SHA-256 value")
    return match.group(1).lower()


def _normalise_commit(value: object) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or not _COMMIT_RE.fullmatch(value.strip()):
        return None
    return value.strip().lower()


def _normalise_bool(value: object, label: str) -> bool | None:
    if value is None:
        return None
    if not isinstance(value, bool):
        _fail("OUTPUT_LEDGER_FIELD_INVALID", f"{label} must be boolean")
    return value


def _normalise_backend(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    candidate = value.strip().lower()
    if candidate in {"cpp", "c++"}:
        return "cpp"
    if candidate == "rust":
        return "rust"
    if candidate.startswith("paired"):
        return "paired"
    return None


def _first_non_none(*values: object) -> object | None:
    for value in values:
        if value is not None:
            return value
    return None


def _validate_source_payload(value: object, *, depth: int = 0, label: str = "source") -> None:
    """Reject payload-bearing source values before extracting any fields."""

    if depth > MAX_DEPTH:
        _fail("OUTPUT_LEDGER_SOURCE_DEPTH", "source evidence is too deeply nested")
    if isinstance(value, Mapping):
        if len(value) > MAX_LIST_ITEMS:
            _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", "source evidence object is too large")
        for key, child in value.items():
            if not isinstance(key, str) or len(key) > MAX_STRING_LENGTH:
                _fail("OUTPUT_LEDGER_SOURCE_KEY", "source evidence has an invalid key")
            lowered = key.replace("-", "_").lower()
            if lowered in _FORBIDDEN_INPUT_KEYS:
                _fail("OUTPUT_LEDGER_PAYLOAD", f"source evidence contains forbidden field {key}")
            _validate_source_payload(child, depth=depth + 1, label=f"{label}.{key}")
        return
    if isinstance(value, list):
        if len(value) > MAX_LIST_ITEMS:
            _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", "source evidence list is too large")
        for index, child in enumerate(value):
            _validate_source_payload(child, depth=depth + 1, label=f"{label}[{index}]")
        return
    if value is None or isinstance(value, bool):
        return
    if isinstance(value, int):
        if abs(value) > 2**63 - 1:
            _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", f"{label} is out of range")
        return
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", f"{label} is not finite")
        return
    if isinstance(value, str):
        if len(value) > MAX_STRING_LENGTH or any(ord(ch) < 0x20 for ch in value):
            _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", f"{label} is not bounded")
        return
    _fail("OUTPUT_LEDGER_SOURCE_BOUNDED", f"{label} has unsupported JSON type")


def _load_source(source: Path | Mapping[str, object]) -> tuple[dict[str, object], str]:
    if isinstance(source, Mapping):
        document = dict(source)
    else:
        path = Path(source)
        try:
            if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_SOURCE_BYTES:
                _fail("OUTPUT_LEDGER_SOURCE_FILE", "source evidence is not a bounded regular file")
            raw = path.read_bytes()
            document = json.loads(raw.decode("utf-8-sig"))
        except OutputEvidenceLedgerError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise OutputEvidenceLedgerError("OUTPUT_LEDGER_SOURCE_PARSE", "source evidence could not be parsed", 5) from error
    if not isinstance(document, dict):
        _fail("OUTPUT_LEDGER_SOURCE_SCHEMA", "source evidence must be a JSON object")
    _validate_source_payload(document)
    if document.get("payloadFree") is not True and document.get("payload_free") is not True:
        _fail("OUTPUT_LEDGER_SOURCE_PAYLOAD", "source evidence must explicitly declare payloadFree=true")
    canonical = canonical_json_bytes(document)
    if len(canonical) > MAX_SOURCE_BYTES:
        _fail("OUTPUT_LEDGER_SOURCE_BYTES", "source evidence is too large after canonicalization")
    return document, hashlib.sha256(canonical).hexdigest()


def _record_name(value: object) -> str:
    record = _get(value, "record")
    if record is None and _get(value, "verifier") is not None:
        return "native-rust-incremental"
    if not isinstance(record, str) or not record.strip() or len(record) > MAX_STRING_LENGTH:
        _fail("OUTPUT_LEDGER_SOURCE_RECORD", "source evidence record kind is missing")
    return record.strip()


def _attempt_token(value: str) -> str:
    token = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip().lower())
    token = token.strip("_.-")
    return token[:48] or "evidence"


def _backend_candidates(source: Mapping[str, object]) -> set[str]:
    values: list[object] = []
    values.extend(_get(source, name) for name in ("backend", "provider", "outputBackend"))
    values.extend(
        [
            _nested(source, "provenance", "outputBackend"),
            _nested(source, "configuration", "environment", "SAKURA_OUTPUT_BACKEND"),
        ]
    )
    for key in ("artifacts", "executables"):
        items = _get(source, key)
        if isinstance(items, list):
            for item in items:
                values.extend([_get(item, "backend", "provider", "outputBackend")])
    backend_builds = _get(source, "backendBuilds")
    if isinstance(backend_builds, Mapping):
        values.extend(backend_builds.keys())
    provenance_builds = _nested(source, "provenance", "backendBuilds")
    if isinstance(provenance_builds, Mapping):
        values.extend(provenance_builds.keys())
    candidates = {_normalise_backend(value) for value in values}
    candidates.discard(None)
    # A direct label is only accepted when all independently observed labels
    # agree.  roleLabels is deliberately not one of the candidates.
    return {str(item) for item in candidates}


def _artifact_backend_candidates(source: Mapping[str, object]) -> set[str]:
    """Return provider identities observed in artifact/provider result rows."""

    values: list[object] = []
    for key in ("artifacts", "executables"):
        items = _get(source, key)
        if isinstance(items, list):
            for item in items:
                if isinstance(item, Mapping):
                    values.append(_get(item, "backend", "provider", "outputBackend"))
    for key in ("backendBuilds",):
        builds = _get(source, key)
        if isinstance(builds, Mapping):
            values.extend(builds.keys())
    builds = _nested(source, "provenance", "backendBuilds")
    if isinstance(builds, Mapping):
        values.extend(builds.keys())
    result = {_normalise_backend(value) for value in values}
    result.discard(None)
    return {str(item) for item in result if item in {"cpp", "rust"}}


def _derive_backend(source: Mapping[str, object]) -> tuple[str, list[str]]:
    candidates = _backend_candidates(source)
    if not candidates:
        _fail("OUTPUT_LEDGER_BACKEND_MISSING", "source evidence has no independently observable Output backend")
    if candidates == {"paired"}:
        _fail("OUTPUT_LEDGER_BACKEND_AMBIGUOUS", "paired backend label is not an independent provider observation")
    artifact_providers = _artifact_backend_candidates(source)
    if artifact_providers:
        # A producer-selected label must not override a provider identity
        # observed in the artifact rows.  A two-provider report is genuinely
        # paired; a one-provider report with an opposite selector is invalid.
        selector_values = list(_get(source, name) for name in ("backend", "provider", "outputBackend"))
        selector_values.extend(
            [
                _nested(source, "provenance", "outputBackend"),
                _nested(source, "configuration", "environment", "SAKURA_OUTPUT_BACKEND"),
            ]
        )
        selector_providers = {_normalise_backend(value) for value in selector_values}
        selector_providers.discard(None)
        selector_providers &= {"cpp", "rust"}
        if len(artifact_providers) == 1 and selector_providers - artifact_providers:
            _fail("OUTPUT_LEDGER_BACKEND_MISMATCH", "provider artifact and selector evidence disagree")
        providers = sorted(artifact_providers)
    else:
        providers = sorted(candidates & {"cpp", "rust"})
    if not providers:
        _fail("OUTPUT_LEDGER_BACKEND_INVALID", "source evidence has no cpp or rust provider")
    if len(providers) == 2:
        return "paired", providers
    # If a paired report contains both artifacts/providers, it must not be
    # reduced to a caller-selected single role.
    return providers[0], providers


def _derive_platform_configuration(source: Mapping[str, object]) -> tuple[str, str]:
    configuration = _get(source, "configuration", "config")
    if isinstance(configuration, Mapping):
        platform_value = _get(configuration, "platform")
        configuration_value = _get(configuration, "configuration", "config")
        if configuration_value is None:
            configuration_value = _get(source, "buildConfiguration")
    else:
        platform_value = _get(source, "platform")
        configuration_value = configuration
    platform = platform_value
    if isinstance(platform, Mapping):
        platform = _get(platform, "architecture", "platform", "name")
        if isinstance(platform, str) and platform.lower() in {"x64", "amd64", "x86_64"}:
            platform = "x64"
    if platform is None:
        platform = _nested(source, "platform", "os", "architecture")
    if isinstance(platform, str) and platform.strip().lower() in {"x64", "amd64", "x86_64"}:
        platform = "x64"
    if platform != "x64":
        _fail("OUTPUT_LEDGER_PLATFORM_INVALID", "source evidence platform is not x64")
    if not isinstance(configuration_value, str):
        _fail("OUTPUT_LEDGER_CONFIGURATION_MISSING", "source evidence configuration is missing")
    canonical_configuration = configuration_value.strip().lower()
    if canonical_configuration == "debug":
        return "x64", "Debug"
    if canonical_configuration == "release":
        return "x64", "Release"
    _fail("OUTPUT_LEDGER_CONFIGURATION_INVALID", "source evidence configuration is not Debug or Release")
    return "x64", "Debug"


def _first_hash(source: Mapping[str, object], candidates: Sequence[tuple[object, str]]) -> str | None:
    for value, label in candidates:
        if value is not None:
            return _normalise_hash(value, label)
    return None


def _derive_source(source: Mapping[str, object]) -> dict[str, object]:
    commit_values = [
        _get(source, "sourceCommit", "sourceHead", "commit"),
        _nested(source, "configuration", "sourceCommit"),
        _nested(source, "provenance", "sourceHead"),
        _nested(source, "provenance", "sourceCommit"),
    ]
    commit = next((item for item in (_normalise_commit(value) for value in commit_values) if item), None)
    dirty = _get(source, "sourceDirty", "dirty")
    if dirty is None:
        provenance = _get(source, "provenance") or {}
        dirty = _get(provenance, "sourceDirty", "dirty")
    if dirty is None:
        checkout = _get(source, "sharedCheckoutBefore")
        if isinstance(checkout, Mapping):
            line_count = _get(checkout, "statusLineCount")
            if isinstance(line_count, int) and not isinstance(line_count, bool) and line_count >= 0:
                # The incremental verifier does not emit sourceDirty, but its
                # status line count is the authoritative cleanliness signal.
                dirty = line_count != 0
    dirty = _normalise_bool(dirty, "sourceDirty")
    status_hash = _first_hash(
        source,
        [
            (_get(source, "sourceStatusSha256", "statusSha256"), "sourceStatusSha256"),
            (_get(_get(source, "provenance") or {}, "sourceStatusSha256", "statusSha256"), "sourceStatusSha256"),
            (_get(_get(source, "sharedCheckoutBefore") or {}, "statusSha256"), "sourceStatusSha256"),
        ],
    )
    return {
        "commit": commit,
        "dirty": dirty,
        "statusSha256": status_hash,
        "complete": commit is not None and dirty is False,
    }


def _derive_selectors(source: Mapping[str, object], backend: str) -> dict[str, object]:
    provenance = _mapping(_get(source, "provenance") or {}, "provenance")
    environment = _mapping(_nested(source, "configuration", "environment") or {}, "configuration.environment")
    output_values = [_get(source, name) for name in ("backend", "provider", "outputBackend")]
    output_values.extend(
        [
            _get(provenance, "outputBackend"),
            _get(environment, "SAKURA_OUTPUT_BACKEND"),
            _nested(source, "selectorProof", "outputBackend"),
        ]
    )
    observed = {_normalise_backend(value) for value in output_values}
    observed.discard(None)
    expected = _artifact_backend_candidates(source) or (set(_backend_candidates(source)) & {"cpp", "rust"})
    if "paired" in observed:
        if expected != {"cpp", "rust"}:
            _fail("OUTPUT_LEDGER_SELECTOR_MISMATCH", "paired selector requires independently observed cpp and rust artifacts")
        observed.remove("paired")
    if expected and observed and (observed - expected or (len(expected) == 1 and len(observed) > 1)):
        _fail("OUTPUT_LEDGER_SELECTOR_MISMATCH", "selector and provider evidence disagree")
    utf16 = _get(source, "utf16Backend") or _get(provenance, "utf16Backend") or _get(environment, "SAKURA_UTF16_BACKEND")
    utf16 = str(utf16).strip().lower() if utf16 is not None else None
    if utf16 in {"unverified", "unknown", ""}:
        utf16 = None
    if utf16 not in {None, "cpp"}:
        _fail("OUTPUT_LEDGER_SELECTOR_INVALID", "Output adoption evidence must pin UTF-16 to cpp")
    proof = _get(source, "selectorProof") or _nested(source, "provenance", "selectorProof")
    proof_result = _get(proof, "result") if isinstance(proof, Mapping) else None
    proof_hash = _first_hash(
        source,
        [
            (_get(source, "selectorProofSha256"), "selectorProofSha256"),
            (_get(proof, "selectorProofSha256") if isinstance(proof, Mapping) else None, "selectorProofSha256"),
            (_get(proof, "contractSha256") if isinstance(proof, Mapping) else None, "selectorProofSha256"),
        ],
    )
    production_output = _first_non_none(
        _get(source, "outputProductionPackage"),
        _get(provenance, "outputProductionPackage"),
        _get(environment, "SAKURA_OUTPUT_PRODUCTION_PACKAGE"),
    )
    production_utf16 = _first_non_none(
        _get(source, "utf16ProductionPackage"),
        _get(provenance, "utf16ProductionPackage"),
        _get(environment, "SAKURA_UTF16_PRODUCTION_PACKAGE"),
    )
    for label, value in (("outputProductionPackage", production_output), ("utf16ProductionPackage", production_utf16)):
        if isinstance(value, str):
            lowered = value.strip().lower()
            if lowered not in {"true", "false"}:
                _fail("OUTPUT_LEDGER_SELECTOR_INVALID", f"{label} must be boolean")
            if label == "outputProductionPackage":
                production_output = lowered == "true"
            else:
                production_utf16 = lowered == "true"
    return {
        "outputBackend": backend,
        "utf16Backend": utf16 or "unknown",
        "outputProductionPackage": _normalise_bool(production_output, "outputProductionPackage"),
        "utf16ProductionPackage": _normalise_bool(production_utf16, "utf16ProductionPackage"),
        "proofResult": _scalar(proof_result, "selectorProof.result"),
        "proofSha256": proof_hash,
    }


def _derive_host(source: Mapping[str, object]) -> dict[str, object]:
    host = _get(source, "host")
    if not isinstance(host, Mapping):
        platform = _get(source, "platform")
        host = platform if isinstance(platform, Mapping) else {}
    os_info = _get(host, "os")
    cpu_info = _get(host, "cpu")
    if not isinstance(os_info, Mapping):
        os_info = {}
    if not isinstance(cpu_info, Mapping):
        cpu_info = {}
    os_identity = _get(host, "osVersion", "version", "windowsImageIdentity") or _get(os_info, "version", "osVersion")
    cpu_manufacturer = _get(host, "cpuManufacturer", "manufacturer") or _get(cpu_info, "manufacturer")
    cpu_model = _get(host, "cpuModel", "model") or _get(cpu_info, "model")
    architecture = _get(host, "architecture") or _get(os_info, "architecture")
    physical = _get(host, "physicalCores") or _get(cpu_info, "physicalCores")
    logical = _get(host, "logicalProcessors") or _get(cpu_info, "logicalProcessors")
    try:
        physical_value = int(physical) if physical is not None and not isinstance(physical, bool) else None
        logical_value = int(logical) if logical is not None and not isinstance(logical, bool) else None
    except (TypeError, ValueError):
        _fail("OUTPUT_LEDGER_HOST_INVALID", "host core counts are invalid")
    complete = bool(
        isinstance(os_identity, str)
        and os_identity
        and isinstance(cpu_manufacturer, str)
        and cpu_manufacturer.strip().lower() != "unknown"
        and isinstance(cpu_model, str)
        and cpu_model.strip().lower() != "unknown"
        and isinstance(architecture, str)
        and architecture.strip().lower() not in {"", "unknown"}
        and physical_value is not None
        and physical_value > 0
        and logical_value is not None
        and logical_value > 0
    )
    return {
        "identitySha256": _normalise_hash(_get(host, "sha256", "hostSha256"), "host.identitySha256"),
        "os": _scalar(_get(host, "os", "platform") if not isinstance(_get(host, "os"), Mapping) else _get(os_info, "platform", "name"), "host.os"),
        "osVersion": _scalar(os_identity, "host.osVersion"),
        "architecture": _scalar(architecture, "host.architecture"),
        "cpuManufacturer": _scalar(cpu_manufacturer, "host.cpuManufacturer"),
        "cpuModel": _scalar(cpu_model, "host.cpuModel"),
        "physicalCores": physical_value,
        "logicalProcessors": logical_value,
        "complete": complete,
    }


def _derive_toolchain(source: Mapping[str, object]) -> dict[str, object]:
    provenance = _mapping(_get(source, "provenance") or {}, "provenance")
    toolchain = _mapping(_get(source, "toolchain") or _get(provenance, "toolchain") or {}, "toolchain")
    values = {
        "msvc": _get(toolchain, "msvc", "msvcIdentity", "msvcVersion"),
        "rust": _get(toolchain, "rust", "rustToolchain", "rustVersion"),
        "rustLockSha256": _get(toolchain, "rustLockSha256", "lockSha256", "cargoLockSha256"),
        "packagePlanSha256": _get(toolchain, "packagePlanSha256", "packagePlanHash"),
        "buildCommandSha256": _get(toolchain, "buildCommandSha256", "commandSha256"),
        "packageCommandSha256": _get(toolchain, "packagePlanCommandSha256", "packageCommandSha256"),
        "runtimeStageCommandSha256": _get(toolchain, "runtimeStageCommandSha256", "stageCommandSha256"),
    }
    environment = _mapping(_nested(source, "configuration", "environment") or {}, "configuration.environment")
    if values["msvc"] is None:
        values["msvc"] = _get(source, "msvcVersion", "msvcIdentity")
    if values["rust"] is None:
        values["rust"] = _get(source, "rustToolchain", "rustVersion")
    hashes = {key: _normalise_hash(value, key) for key, value in values.items() if key.endswith("Sha256")}
    result: dict[str, object] = {
        "msvc": _scalar(values["msvc"], "toolchain.msvc"),
        "rust": _scalar(values["rust"], "toolchain.rust"),
        **hashes,
    }
    result["complete"] = bool(result["msvc"] and result["rust"] and result["rustLockSha256"] and result["packagePlanSha256"])
    if not result.get("buildCommandSha256"):
        result["buildCommandSha256"] = _normalise_hash(_get(source, "buildCommandSha256"), "buildCommandSha256")
    if not result.get("packageCommandSha256"):
        result["packageCommandSha256"] = _normalise_hash(_get(source, "packagePlanCommandSha256"), "packageCommandSha256")
    if not result.get("runtimeStageCommandSha256"):
        result["runtimeStageCommandSha256"] = _normalise_hash(_get(source, "runtimeStageCommandSha256"), "runtimeStageCommandSha256")
    return result


def _derive_package(source: Mapping[str, object], selectors: Mapping[str, object]) -> dict[str, object]:
    provenance = _mapping(_get(source, "provenance") or {}, "provenance")
    dependency = _mapping(_get(source, "dependencyClosure") or _get(provenance, "dependencyClosure") or {}, "dependencyClosure")
    package = _mapping(_get(source, "packageRestore") or _get(source, "package") or {}, "package")
    plan = _first_hash(
        source,
        [
            (_get(source, "packagePlanSha256", "packagePlanHash"), "packagePlanSha256"),
            (_get(provenance, "packagePlanSha256", "packagePlanHash"), "packagePlanSha256"),
        ],
    )
    closure = _first_hash(
        source,
        [
            (_get(source, "dependencyClosureSha256", "closureSha256"), "dependencyClosureSha256"),
            (_get(dependency, "cppSha256", "rustSha256", "sha256"), "dependencyClosureSha256"),
        ],
    )
    status = _get(package, "type", "status", "validation")
    return {
        "planSha256": plan,
        "closureSha256": closure,
        "status": _scalar(status, "package.status"),
        "productionPackage": selectors.get("outputProductionPackage"),
        "complete": bool(plan and closure and status),
    }


def _derive_corpus(source: Mapping[str, object]) -> dict[str, object]:
    sample = _mapping(_get(source, "sample") or {}, "sample")
    seed = _get(source, "seed")
    if seed is None:
        seed = _get(_get(source, "configuration") or {}, "seed")
    if seed is not None and (not isinstance(seed, int) or isinstance(seed, bool) or seed < 0 or seed > 2**63 - 1):
        _fail("OUTPUT_LEDGER_CORPUS_INVALID", "corpus seed is invalid")
    sample_hash = _first_hash(
        source,
        [
            (_get(sample, "sha256", "sampleSha256", "campaignCopySha256"), "sampleSha256"),
            (_get(source, "sampleSha256", "corpusSha256"), "sampleSha256"),
        ],
    )
    sample_size = _get(sample, "sizeBytes", "campaignCopySizeBytes")
    if sample_size is not None and (
        not isinstance(sample_size, int)
        or isinstance(sample_size, bool)
        or sample_size < 0
        or sample_size > 2**63 - 1
    ):
        _fail("OUTPUT_LEDGER_CORPUS_INVALID", "corpus size is invalid")
    profile_hash = _first_hash(
        source,
        [
            (_get(_get(source, "profilePolicy") or {}, "sha256"), "profilePolicySha256"),
            (_get(source, "profilePolicySha256"), "profilePolicySha256"),
        ],
    )
    return {
        "sampleSha256": sample_hash,
        "sampleSizeBytes": sample_size,
        "seed": seed,
        "profilePolicySha256": profile_hash,
        "complete": bool(sample_hash and seed is not None),
    }


def _derive_commands(source: Mapping[str, object], toolchain: Mapping[str, object]) -> dict[str, object]:
    scripts = _mapping(_get(source, "scripts") or {}, "scripts")
    values = {
        "buildSha256": _get(source, "buildCommandSha256") or toolchain.get("buildCommandSha256"),
        "packageSha256": _get(source, "packagePlanCommandSha256") or toolchain.get("packageCommandSha256"),
        "runtimeStageSha256": _get(source, "runtimeStageCommandSha256") or toolchain.get("runtimeStageCommandSha256"),
        "measurementSha256": _get(source, "measurementCommandSha256", "commandSha256"),
        "runnerSha256": _get(scripts, "pairedRunnerSha256", "runnerSha256", "sharedStartupImplementationSha256"),
    }
    return {key: _normalise_hash(value, key) for key, value in values.items()} | {
        "complete": all(values.get(key) is not None for key in ("buildSha256", "measurementSha256")),
    }


def _artifact_items(source: Mapping[str, object], backend: str) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    items = _get(source, "artifacts")
    if not isinstance(items, list):
        items = _get(source, "executables")
    if isinstance(items, list):
        for item in items[:MAX_ARTIFACTS]:
            if not isinstance(item, Mapping):
                _fail("OUTPUT_LEDGER_ARTIFACT_INVALID", "artifact entry is not an object")
            item_backend = _normalise_backend(_get(item, "backend", "provider", "outputBackend")) or backend
            digest = _first_hash(
                item,
                [
                    (_get(item, "artifactSha256", "executableSha256", "sha256"), "artifactSha256"),
                ],
            )
            size = _get(item, "sizeBytes", "executableSizeBytes")
            if digest is None:
                continue
            if size is not None and (
                not isinstance(size, int)
                or isinstance(size, bool)
                or size < 0
                or size > 2**63 - 1
            ):
                _fail("OUTPUT_LEDGER_ARTIFACT_INVALID", "artifact size is invalid")
            result.append({"backend": item_backend, "sha256": digest, "sizeBytes": size})
    if not result:
        direct = _first_hash(
            source,
            [(_get(source, "artifactSha256", "executableSha256", "exeSha256"), "artifactSha256")],
        )
        if direct:
            size = _get(source, "sizeBytes", "executableSizeBytes")
            if size is not None and (
                not isinstance(size, int)
                or isinstance(size, bool)
                or size < 0
                or size > 2**63 - 1
            ):
                _fail("OUTPUT_LEDGER_ARTIFACT_INVALID", "artifact size is invalid")
            result.append({"backend": backend, "sha256": direct, "sizeBytes": size})
    return sorted(result, key=lambda item: (str(item["backend"]), str(item["sha256"])))


def _status_records(source: Mapping[str, object]) -> tuple[list[dict[str, object]], bool]:
    records: list[dict[str, object]] = []
    status_names = {
        "status",
        "type",
        "result",
        "resulttype",
        "failurecode",
        "failuretype",
        "reason",
        "reasoncode",
        "state",
        "phase",
        "code",
        "pass",
        "qualified",
        "startupgatepass",
        "performancethresholdpassed",
        "cleanupverified",
        "allcleanupverified",
        "allprocesscleanupverified",
        "allprofilecleanupverified",
        "allbundlecleanupverified",
        "exitcode",
        "durationseconds",
        "workactioncount",
        "actionrecordcount",
        "retainedactioncount",
        "unretainedactioncount",
        "observedsurvivorcount",
        "observedexpectedhelpercount",
        "postcleanupsurvivorcount",
        "postcleanuphelpercount",
        "survivorcount",
        "failedlaunches",
        "successfullaunches",
        "scheduledlaunches",
        "suppressedlaunches",
        "linecount",
        "bytecount",
        "diagnosticsparsefailed",
        "parserfailed",
        "validation",
        "mode",
    }

    def visit(value: object, prefix: str) -> None:
        if len(records) >= MAX_STATUS_RECORDS:
            return
        if isinstance(value, Mapping):
            for key, child in value.items():
                if not isinstance(key, str):
                    continue
                lowered = key.replace("-", "").lower()
                child_prefix = f"{prefix}.{key}" if prefix else key
                if lowered in {"actioncounts", "errorcodes", "unexpectedtoolnames"} and isinstance(child, Mapping):
                    for item_key, item_value in child.items():
                        if len(records) >= MAX_STATUS_RECORDS:
                            break
                        if (
                            isinstance(item_key, str)
                            and re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", item_key)
                            and isinstance(item_value, (str, bool, int, float))
                        ):
                            bounded = _scalar(item_value, f"{child_prefix}.{item_key}")
                            records.append({"field": f"{child_prefix}.{item_key}"[:MAX_STRING_LENGTH], "value": bounded})
                elif (
                    len(records) < MAX_STATUS_RECORDS
                    and lowered in status_names
                    and isinstance(child, (str, bool, int, float))
                ):
                    bounded = _scalar(child, child_prefix)
                    records.append({"field": child_prefix[:MAX_STRING_LENGTH], "value": bounded})
                if isinstance(child, (Mapping, list)):
                    visit(child, child_prefix)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                if len(records) >= MAX_STATUS_RECORDS:
                    break
                visit(child, f"{prefix}[{index}]")

    visit(source, "")
    return records, len(records) >= MAX_STATUS_RECORDS


def _derive_results(source: Mapping[str, object]) -> dict[str, object]:
    statuses, truncated = _status_records(source)
    failure = _get(source, "failure")
    termination = _get(source, "termination")
    cleanup = _get(source, "cleanup")
    acceptance = _get(source, "acceptance")
    performance = _get(source, "performance")
    source_status = _get(source, "status")
    if source_status is None:
        candidate_status = _get(source, "result")
        if isinstance(candidate_status, (str, bool, int)):
            source_status = candidate_status
    failure_type = _first_non_none(
        _get(failure, "type", "failureType"),
        _get(termination, "failureType", "type"),
    )
    result: dict[str, object] = {
        "sourceStatus": _scalar(source_status, "results.sourceStatus"),
        "pass": _normalise_bool(_get(source, "pass"), "results.pass"),
        "startupGatePass": _normalise_bool(_get(source, "startupGatePass"), "results.startupGatePass"),
        "acceptanceQualified": _normalise_bool(
            _first_non_none(_get(source, "acceptanceQualified"), _get(acceptance, "qualified")),
            "results.acceptanceQualified",
        ),
        "performancePassed": _normalise_bool(
            _get(source, "performanceThresholdPassed")
            if _get(source, "performanceThresholdPassed") is not None
            else _get(performance, "pass", "performanceThresholdPassed"),
            "results.performancePassed",
        ),
        "failureType": _scalar(failure_type, "results.failureType"),
        "terminationType": _scalar(_get(termination, "type", "failureType"), "results.terminationType"),
        "terminationStatus": _scalar(_get(termination, "status"), "results.terminationStatus"),
        "cleanupVerified": _normalise_bool(
            _get(cleanup, "allCleanupVerified", "cleanupVerified"), "results.cleanupVerified"
        ),
        "survivorCount": _get(cleanup, "survivorCount"),
        "failedLaunches": _get(acceptance, "failedLaunches"),
        "skippedLaunches": _get(acceptance, "suppressedLaunches"),
        "statuses": statuses,
        "statusesTruncated": truncated,
    }
    for key in ("survivorCount", "failedLaunches", "skippedLaunches"):
        value = result[key]
        if value is not None and (not isinstance(value, int) or value < 0 or value > 2**63 - 1):
            _fail("OUTPUT_LEDGER_RESULT_INVALID", f"results.{key} is invalid")
    return result


def extract_output_evidence(source: Path | Mapping[str, object]) -> dict[str, object]:
    """Extract a normalized attempt without accepting caller role labels."""

    document, source_hash = _load_source(source)
    source_kind = _record_name(document)
    source_schema = _get(document, "schemaVersion", "schema_version")
    if not isinstance(source_schema, int) or isinstance(source_schema, bool) or source_schema < 1:
        _fail("OUTPUT_LEDGER_SOURCE_SCHEMA", "source evidence schemaVersion is invalid")
    backend, backends = _derive_backend(document)
    platform, configuration = _derive_platform_configuration(document)
    source_info = _derive_source(document)
    selectors = _derive_selectors(document, backend)
    host = _derive_host(document)
    toolchain = _derive_toolchain(document)
    package = _derive_package(document, selectors)
    corpus = _derive_corpus(document)
    commands = _derive_commands(document, toolchain)
    artifacts = _artifact_items(document, backend)
    results = _derive_results(document)
    return {
        "schemaVersion": SCHEMA_VERSION,
        "record": RECORD_KIND,
        "payloadFree": True,
        "attemptSequence": 0,
        "attemptId": f"{_attempt_token(source_kind)}-{source_hash[:24]}",
        "sourceEvidenceKind": source_kind,
        "sourceEvidenceSchemaVersion": source_schema,
        "sourceEvidenceSha256": source_hash,
        "previousRecordSha256": GENESIS_HASH,
        "backend": backend,
        "backends": backends,
        "platform": platform,
        "configuration": configuration,
        "source": source_info,
        "selectors": selectors,
        "host": host,
        "toolchain": toolchain,
        "package": package,
        "corpus": corpus,
        "commands": commands,
        "artifacts": artifacts,
        "results": results,
        "decision": "HOLD",
        "adoptionEligible": False,
    }


def _ledger_records_directory(ledger_directory: Path) -> Path:
    root = Path(ledger_directory)
    if root.exists() and (root.is_symlink() or not root.is_dir()):
        _fail("OUTPUT_LEDGER_DIRECTORY", "ledger directory is not a regular directory")
    records = root / "records"
    if records.exists() and (records.is_symlink() or not records.is_dir()):
        _fail("OUTPUT_LEDGER_DIRECTORY", "ledger records directory is not a regular directory")
    return records


def _record_entries(ledger_directory: Path) -> tuple[list[tuple[int, Path]], list[dict[str, object]]]:
    records = _ledger_records_directory(ledger_directory)
    if not records.exists():
        return [], []
    entries: list[tuple[int, Path]] = []
    failures: list[dict[str, object]] = []
    try:
        children = list(os.scandir(records))
    except OSError as error:
        raise OutputEvidenceLedgerError("OUTPUT_LEDGER_READ", "ledger records could not be listed", 5) from error
    for entry in children:
        name = entry.name
        if entry.is_dir(follow_symlinks=False):
            failures.append({"code": "OUTPUT_LEDGER_NESTED_DIRECTORY"})
            continue
        if entry.is_symlink():
            failures.append({"code": "OUTPUT_LEDGER_REPARSE_RECORD"})
            continue
        if not entry.is_file(follow_symlinks=False):
            failures.append({"code": "OUTPUT_LEDGER_NONFILE_RECORD"})
            continue
        if name.startswith(".") or any(name.endswith(suffix) for suffix in _TEMP_SUFFIXES):
            failures.append({"code": "OUTPUT_LEDGER_TEMP_ARTIFACT"})
            continue
        match = _RECORD_FILE_RE.fullmatch(name)
        if not match:
            failures.append({"code": "OUTPUT_LEDGER_RECORD_NAME"})
            continue
        entries.append((int(match.group(1)), Path(entry.path)))
    if len(entries) > MAX_RECORDS:
        failures.append({"code": "OUTPUT_LEDGER_RECORD_LIMIT"})
    return sorted(entries), failures


def _read_record(path: Path) -> dict[str, object]:
    try:
        raw = path.read_bytes()
        if len(raw) > MAX_SOURCE_BYTES or raw.startswith(b"\xef\xbb\xbf"):
            _fail("OUTPUT_LEDGER_RECORD_BYTES", "record bytes are invalid")
        value = json.loads(raw.decode("utf-8"))
    except OutputEvidenceLedgerError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise OutputEvidenceLedgerError("OUTPUT_LEDGER_RECORD_PARSE", "record could not be parsed", 5) from error
    if not isinstance(value, dict):
        _fail("OUTPUT_LEDGER_RECORD_SCHEMA", "record must be a JSON object")
    return value


def _record_object(record: Mapping[str, object], key: str, expected: frozenset[str]) -> Mapping[str, object]:
    value = record.get(key)
    if not isinstance(value, Mapping) or set(value) != expected:
        _fail("OUTPUT_LEDGER_RECORD_SCHEMA", f"record.{key} fields do not match schema v1")
    return value


def _record_optional_bool(value: object, label: str) -> None:
    if value is not None:
        _normalise_bool(value, label)


def _record_optional_int(value: object, label: str) -> None:
    if value is not None and (not isinstance(value, int) or isinstance(value, bool) or value < 0 or value > 2**63 - 1):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", f"{label} is invalid")


def _record_optional_hash(value: object, label: str) -> None:
    _normalise_hash(value, label)


def _validate_nested_record_shape(record: Mapping[str, object]) -> None:
    source = _record_object(record, "source", _SOURCE_KEYS)
    commit = source.get("commit")
    if commit is not None and (_normalise_commit(commit) is None or commit != str(commit).lower()):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.source.commit is invalid")
    _record_optional_bool(source.get("dirty"), "record.source.dirty")
    _record_optional_hash(source.get("statusSha256"), "record.source.statusSha256")
    if not isinstance(source.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.source.complete is invalid")

    selectors = _record_object(record, "selectors", _SELECTOR_KEYS)
    if selectors.get("outputBackend") != record.get("backend"):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.selectors.outputBackend disagrees with backend")
    if selectors.get("utf16Backend") not in {"cpp", "unknown"}:
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.selectors.utf16Backend is invalid")
    _record_optional_bool(selectors.get("outputProductionPackage"), "record.selectors.outputProductionPackage")
    _record_optional_bool(selectors.get("utf16ProductionPackage"), "record.selectors.utf16ProductionPackage")
    if selectors.get("proofResult") is not None:
        _scalar(selectors.get("proofResult"), "record.selectors.proofResult")
    _record_optional_hash(selectors.get("proofSha256"), "record.selectors.proofSha256")

    host = _record_object(record, "host", _HOST_KEYS)
    _record_optional_hash(host.get("identitySha256"), "record.host.identitySha256")
    for key in ("os", "osVersion", "architecture", "cpuManufacturer", "cpuModel"):
        if host.get(key) is not None:
            _scalar(host.get(key), f"record.host.{key}")
    for key in ("physicalCores", "logicalProcessors"):
        _record_optional_int(host.get(key), f"record.host.{key}")
    if not isinstance(host.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.host.complete is invalid")

    toolchain = _record_object(record, "toolchain", _TOOLCHAIN_KEYS)
    for key in ("msvc", "rust"):
        if toolchain.get(key) is not None and not isinstance(toolchain.get(key), str):
            _fail("OUTPUT_LEDGER_RECORD_FIELD", f"record.toolchain.{key} is invalid")
        if toolchain.get(key) is not None:
            _scalar(toolchain.get(key), f"record.toolchain.{key}")
    for key in ("rustLockSha256", "packagePlanSha256", "buildCommandSha256", "packageCommandSha256", "runtimeStageCommandSha256"):
        _record_optional_hash(toolchain.get(key), f"record.toolchain.{key}")
    if not isinstance(toolchain.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.toolchain.complete is invalid")

    package = _record_object(record, "package", _PACKAGE_KEYS)
    _record_optional_hash(package.get("planSha256"), "record.package.planSha256")
    _record_optional_hash(package.get("closureSha256"), "record.package.closureSha256")
    if package.get("status") is not None:
        _scalar(package.get("status"), "record.package.status")
    _record_optional_bool(package.get("productionPackage"), "record.package.productionPackage")
    if not isinstance(package.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.package.complete is invalid")

    corpus = _record_object(record, "corpus", _CORPUS_KEYS)
    _record_optional_hash(corpus.get("sampleSha256"), "record.corpus.sampleSha256")
    _record_optional_int(corpus.get("sampleSizeBytes"), "record.corpus.sampleSizeBytes")
    _record_optional_int(corpus.get("seed"), "record.corpus.seed")
    _record_optional_hash(corpus.get("profilePolicySha256"), "record.corpus.profilePolicySha256")
    if not isinstance(corpus.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.corpus.complete is invalid")

    commands = _record_object(record, "commands", _COMMAND_KEYS)
    for key in ("buildSha256", "packageSha256", "runtimeStageSha256", "measurementSha256", "runnerSha256"):
        _record_optional_hash(commands.get(key), f"record.commands.{key}")
    if not isinstance(commands.get("complete"), bool):
        _fail("OUTPUT_LEDGER_RECORD_FIELD", "record.commands.complete is invalid")

    artifacts = record.get("artifacts")
    if not isinstance(artifacts, list):
        _fail("OUTPUT_LEDGER_RECORD_ARTIFACTS", "record artifacts are invalid")
    for artifact in artifacts:
        if not isinstance(artifact, Mapping) or set(artifact) != _ARTIFACT_KEYS:
            _fail("OUTPUT_LEDGER_RECORD_ARTIFACTS", "record artifact fields are invalid")
        if artifact.get("backend") not in {"cpp", "rust", "paired"}:
            _fail("OUTPUT_LEDGER_RECORD_ARTIFACTS", "record artifact backend is invalid")
        _normalise_hash(artifact.get("sha256"), "record.artifact.sha256", allow_none=False)
        _record_optional_int(artifact.get("sizeBytes"), "record.artifact.sizeBytes")

    results = _record_object(record, "results", _RESULT_KEYS)
    _scalar(results.get("sourceStatus"), "record.results.sourceStatus")
    for key in ("pass", "startupGatePass", "acceptanceQualified", "performancePassed", "cleanupVerified"):
        _record_optional_bool(results.get(key), f"record.results.{key}")
    for key in ("failureType", "terminationType", "terminationStatus"):
        if results.get(key) is not None:
            _scalar(results.get(key), f"record.results.{key}")
    for key in ("survivorCount", "failedLaunches", "skippedLaunches"):
        _record_optional_int(results.get(key), f"record.results.{key}")
    statuses = results.get("statuses")
    if not isinstance(statuses, list) or len(statuses) > MAX_STATUS_RECORDS:
        _fail("OUTPUT_LEDGER_RECORD_RESULTS", "record.results.statuses are invalid")
    for status in statuses:
        if not isinstance(status, Mapping) or set(status) != _STATUS_KEYS:
            _fail("OUTPUT_LEDGER_RECORD_RESULTS", "record result status fields are invalid")
        if not isinstance(status.get("field"), str) or not re.fullmatch(r"[A-Za-z0-9_.\[\]-]{1,512}", status["field"]):
            _fail("OUTPUT_LEDGER_RECORD_RESULTS", "record result status field is invalid")
        _scalar(status.get("value"), "record.results.status.value")
    if not isinstance(results.get("statusesTruncated"), bool):
        _fail("OUTPUT_LEDGER_RECORD_RESULTS", "record.results.statusesTruncated is invalid")


def _validate_record_shape(record: Mapping[str, object], sequence: int, previous: str) -> str:
    if set(record) != _RECORD_KEYS:
        _fail("OUTPUT_LEDGER_RECORD_SCHEMA", "record fields do not match schema v1")
    if record.get("schemaVersion") != SCHEMA_VERSION or record.get("record") != RECORD_KIND:
        _fail("OUTPUT_LEDGER_RECORD_SCHEMA", "record schema or kind is invalid")
    if record.get("payloadFree") is not True or record.get("decision") != "HOLD" or record.get("adoptionEligible") is not False:
        _fail("OUTPUT_LEDGER_RECORD_POLICY", "record is not an explicit HOLD payload-free record")
    if record.get("attemptSequence") != sequence or record.get("previousRecordSha256") != previous:
        _fail("OUTPUT_LEDGER_RECORD_CHAIN", "record sequence or hash chain is invalid")
    if not isinstance(record.get("attemptId"), str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", record["attemptId"]):
        _fail("OUTPUT_LEDGER_RECORD_ID", "record attemptId is invalid")
    if not isinstance(record.get("sourceEvidenceKind"), str) or not record["sourceEvidenceKind"]:
        _fail("OUTPUT_LEDGER_RECORD_SOURCE", "record source kind is invalid")
    if not isinstance(record.get("sourceEvidenceSchemaVersion"), int) or isinstance(record.get("sourceEvidenceSchemaVersion"), bool) or record["sourceEvidenceSchemaVersion"] < 1:
        _fail("OUTPUT_LEDGER_RECORD_SOURCE", "record source schema version is invalid")
    _normalise_hash(record.get("sourceEvidenceSha256"), "sourceEvidenceSha256", allow_none=False)
    expected_backends = {
        "cpp": ["cpp"],
        "rust": ["rust"],
        "paired": ["cpp", "rust"],
    }
    if record.get("backend") not in expected_backends or record.get("backends") != expected_backends.get(record.get("backend")):
        _fail("OUTPUT_LEDGER_RECORD_BACKEND", "record backend is invalid")
    if record.get("platform") != "x64" or record.get("configuration") not in {"Debug", "Release"}:
        _fail("OUTPUT_LEDGER_RECORD_CONFIGURATION", "record platform or configuration is invalid")
    if not isinstance(record.get("artifacts"), list) or len(record["artifacts"]) > MAX_ARTIFACTS:
        _fail("OUTPUT_LEDGER_RECORD_ARTIFACTS", "record artifacts are invalid")
    _validate_source_payload(record)
    _validate_nested_record_shape(record)
    without_hash = {key: value for key, value in record.items() if key != "recordSha256"}
    expected_hash = canonical_json_sha256(without_hash)
    if record.get("recordSha256") != expected_hash:
        _fail("OUTPUT_LEDGER_RECORD_HASH", "record canonical hash does not match")
    return expected_hash


def _verify_output_evidence_ledger_unlocked(ledger_directory: Path) -> dict[str, object]:
    try:
        entries, failures = _record_entries(Path(ledger_directory))
    except OutputEvidenceLedgerError as error:
        return {
            "ok": False,
            "ledgerKind": LEDGER_KIND,
            "schemaVersion": SCHEMA_VERSION,
            "recordCount": 0,
            "headSha256": GENESIS_HASH,
            "records": [],
            "failures": [{"code": error.code}],
        }
    expected_sequence = 1
    previous = GENESIS_HASH
    source_hashes: set[str] = set()
    attempt_ids: set[str] = set()
    summaries: list[dict[str, object]] = []
    for sequence, path in entries:
        if sequence != expected_sequence:
            failures.append({"code": "OUTPUT_LEDGER_SEQUENCE"})
            expected_sequence = sequence
        try:
            record = _read_record(path)
            record_hash = _validate_record_shape(record, sequence, previous)
            name_match = _RECORD_FILE_RE.fullmatch(path.name)
            if name_match is None or record_hash[:16] != name_match.group(2):
                _fail("OUTPUT_LEDGER_RECORD_NAME_HASH", "record filename hash does not match content")
            source_hash = str(record["sourceEvidenceSha256"])
            attempt_id = str(record["attemptId"])
            if source_hash in source_hashes:
                failures.append({"code": "OUTPUT_LEDGER_DUPLICATE_SOURCE"})
            if attempt_id in attempt_ids:
                failures.append({"code": "OUTPUT_LEDGER_DUPLICATE_ATTEMPT"})
            source_hashes.add(source_hash)
            attempt_ids.add(attempt_id)
            previous = record_hash
            summaries.append(
                {
                    "sequence": sequence,
                    "backend": record["backend"],
                    "configuration": record["configuration"],
                    "sourceEvidenceKind": record["sourceEvidenceKind"],
                    "sourceEvidenceSha256": record["sourceEvidenceSha256"],
                    "recordSha256": record_hash,
                    "resultStatus": _nested(record, "results", "sourceStatus"),
                }
            )
        except OutputEvidenceLedgerError as error:
            failures.append({"code": error.code})
            expected_sequence = sequence
        expected_sequence += 1
    return {
        "ok": not failures,
        "ledgerKind": LEDGER_KIND,
        "schemaVersion": SCHEMA_VERSION,
        "recordCount": len(entries),
        "headSha256": previous if entries and not failures else (previous if entries else GENESIS_HASH),
        "records": summaries,
        "failures": failures,
    }


def _ledger_lock_path(ledger_directory: Path) -> Path:
    return Path(ledger_directory) / ".append.lock"


def verify_output_evidence_ledger(ledger_directory: Path) -> dict[str, object]:
    """Verify records, rejecting an in-progress or abandoned append lock."""

    root = Path(ledger_directory)
    lock = _ledger_lock_path(root)
    if lock.exists() or lock.is_symlink():
        return {
            "ok": False,
            "ledgerKind": LEDGER_KIND,
            "schemaVersion": SCHEMA_VERSION,
            "recordCount": 0,
            "headSha256": GENESIS_HASH,
            "records": [],
            "failures": [{"code": "OUTPUT_LEDGER_APPEND_LOCK"}],
        }
    return _verify_output_evidence_ledger_unlocked(root)


def append_output_evidence(
    ledger_directory: Path,
    source: Path | Mapping[str, object],
    *,
    backend: str | None = None,
) -> dict[str, object]:
    """Append one immutable attempt record, rejecting duplicates and tamper."""

    root = Path(ledger_directory)
    # The lock is an exclusive, payload-free reservation.  It covers the
    # verify/derive/sequence/write transaction so two producers cannot both
    # publish a valid-looking record at the same sequence.
    _ledger_records_directory(root)
    root.mkdir(parents=True, exist_ok=True)
    lock = _ledger_lock_path(root)
    try:
        descriptor = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise OutputEvidenceLedgerError("OUTPUT_LEDGER_LOCKED", "another append is in progress or left an append lock", 5) from error
    except OSError as error:
        raise OutputEvidenceLedgerError("OUTPUT_LEDGER_LOCK", "append lock could not be created", 5) from error
    try:
        existing = _verify_output_evidence_ledger_unlocked(root)
        if not existing["ok"]:
            _fail("OUTPUT_LEDGER_INVALID", "cannot append to an invalid evidence ledger")
        normalized = extract_output_evidence(source)
        if backend is not None:
            requested = _normalise_backend(backend)
            if requested is None or requested != normalized["backend"]:
                _fail("OUTPUT_LEDGER_BACKEND_MISMATCH", "caller backend does not match source evidence")
        source_hash = str(normalized["sourceEvidenceSha256"])
        for item in existing["records"]:
            if item.get("sourceEvidenceSha256") == source_hash:
                _fail("OUTPUT_LEDGER_DUPLICATE", "source evidence has already been appended")
        sequence = int(existing["recordCount"]) + 1
        previous = str(existing["headSha256"])
        normalized["attemptSequence"] = sequence
        normalized["previousRecordSha256"] = previous
        record_hash = canonical_json_sha256(normalized)
        normalized["recordSha256"] = record_hash
        records_directory = _ledger_records_directory(root)
        records_directory.mkdir(parents=True, exist_ok=True)
        destination = records_directory / f"record-{sequence:08d}-{record_hash[:16]}.json"
        payload = canonical_json_bytes(normalized) + b"\n"
        try:
            with destination.open("xb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
        except FileExistsError as error:
            raise OutputEvidenceLedgerError("OUTPUT_LEDGER_DUPLICATE", "record destination already exists", 5) from error
        except OSError as error:
            raise OutputEvidenceLedgerError("OUTPUT_LEDGER_APPEND", "record could not be appended", 5) from error
        return {
            "ok": True,
            "ledgerKind": LEDGER_KIND,
            "schemaVersion": SCHEMA_VERSION,
            "sequence": sequence,
            "attemptId": normalized["attemptId"],
            "backend": normalized["backend"],
            "configuration": normalized["configuration"],
            "recordSha256": record_hash,
            "recordCount": sequence,
        }
    finally:
        try:
            os.close(descriptor)
        finally:
            try:
                lock.unlink()
            except FileNotFoundError:
                pass


# Explicit aliases keep callers readable and ease migration from the semantic
# inventory history API.
append_output_evidence_record = append_output_evidence
verify_output_evidence = verify_output_evidence_ledger


__all__ = [
    "DEFAULT_LEDGER_DIRECTORY",
    "LEDGER_KIND",
    "RECORD_KIND",
    "SCHEMA_VERSION",
    "OutputEvidenceLedgerError",
    "append_output_evidence",
    "append_output_evidence_record",
    "canonical_json_bytes",
    "canonical_json_sha256",
    "extract_output_evidence",
    "sha256_json",
    "verify_output_evidence",
    "verify_output_evidence_ledger",
]
