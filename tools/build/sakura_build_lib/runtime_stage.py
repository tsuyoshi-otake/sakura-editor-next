"""Explicit, content-addressed runtime staging for the product closure.

The compiler/linker produces the product and language-resource binaries in
configuration-specific output directories.  This module is the only owner of
the reusable runtime staging tree.  It copies exactly the graph-declared
runtime closure, writes a deterministic receipt, and never lets a native build
discover or copy ambient files on its own.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, Mapping

from .model import SemanticGraph
from .runner import BuildError, EventWriter


STAGE_SCHEMA_VERSION = 1
RECEIPT_NAME = ".sakura-runtime-stage.json"
_LOCK_POLL_SECONDS = 0.1


def _canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _sha256_file(path: Path, code: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise BuildError(code, f"could not hash {path}: {error}", 5) from error
    return "sha256:" + digest.hexdigest()


def _relative_path(repo_root: Path, value: object, location: str, *, must_exist: bool) -> str:
    if not isinstance(value, str) or not value:
        raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location} must be a non-empty repository-relative path", 5)
    candidate = Path(value)
    if candidate.is_absolute() or ".." in candidate.parts:
        raise BuildError("RUNTIME_STAGE_PATH", f"{location} must stay below the repository: {value}", 5)
    normalized = candidate.as_posix()
    path = repo_root / normalized
    if must_exist and not path.is_file():
        raise BuildError("RUNTIME_STAGE_SOURCE_MISSING", f"{location} does not exist as a file: {normalized}", 5)
    return normalized


def _load_stage_document(repo_root: Path, path: Path) -> dict[str, Mapping[str, object]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise BuildError("RUNTIME_STAGE_CONFIG_MISSING", f"runtime stage configuration is missing: {path}", 5) from error
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError("RUNTIME_STAGE_CONFIG_INVALID", f"could not read runtime stage configuration {path}: {error}", 5) from error
    if not isinstance(raw, dict) or set(raw) != {"schema_version", "staging_sets"}:
        raise BuildError("RUNTIME_STAGE_SCHEMA", "runtime stage configuration has unexpected fields", 5)
    if raw["schema_version"] != STAGE_SCHEMA_VERSION:
        raise BuildError("RUNTIME_STAGE_SCHEMA", f"runtime stage configuration must use schema {STAGE_SCHEMA_VERSION}", 5)
    values = raw["staging_sets"]
    if not isinstance(values, list) or not values:
        raise BuildError("RUNTIME_STAGE_SCHEMA", "runtime stage configuration must contain staging_sets", 5)
    result: dict[str, Mapping[str, object]] = {}
    for index, value in enumerate(values):
        location = f"staging_sets[{index}]"
        if not isinstance(value, dict) or set(value) != {"id", "context_id", "entries"}:
            raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location} has unexpected fields", 5)
        stage_id = value["id"]
        context_id = value["context_id"]
        entries = value["entries"]
        if not isinstance(stage_id, str) or not stage_id:
            raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location}.id must be a non-empty string", 5)
        if stage_id in result:
            raise BuildError("RUNTIME_STAGE_SCHEMA", f"duplicate runtime staging-set ID: {stage_id}", 5)
        if not isinstance(context_id, str) or not context_id:
            raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location}.context_id must be a non-empty string", 5)
        if not isinstance(entries, list) or not entries:
            raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location}.entries must be a non-empty array", 5)
        result[stage_id] = value
    return result


def _reachable_stage_sets(graph: SemanticGraph, product_id: str, context_id: str) -> tuple[str, ...]:
    if product_id not in graph.components:
        raise BuildError("RUNTIME_STAGE_PRODUCT_UNKNOWN", f"unknown product component: {product_id}", 2)
    active_edges = graph.active_edges(context_id)
    reachable = {product_id}
    pending = [product_id]
    while pending:
        source = pending.pop()
        for edge in active_edges:
            if edge.source != source or not {"stage", "runtime"}.intersection(edge.phases):
                continue
            if edge.target in reachable:
                continue
            reachable.add(edge.target)
            pending.append(edge.target)
    return tuple(
        sorted(
            node_id
            for node_id in reachable
            if node_id in graph.artifacts and graph.artifacts[node_id].artifact_kind == "staging_set"
        )
    )


def _stage_edges_are_declared(graph: SemanticGraph, stage_id: str, provider_id: str, context_id: str) -> bool:
    return any(
        edge.source == stage_id
        and edge.target == provider_id
        and edge.kind == "asset"
        and edge.required
        and {"stage", "runtime"}.issubset(edge.phases)
        for edge in graph.active_edges(context_id)
    )


def _stage_specification(
    graph: SemanticGraph,
    product_id: str,
    context_id: str,
) -> tuple[dict[str, object], ...]:
    stage_ids = _reachable_stage_sets(graph, product_id, context_id)
    if not stage_ids:
        return ()
    documents: dict[str, Mapping[str, object]] = {}
    specifications: list[dict[str, object]] = []
    for stage_id in stage_ids:
        artifact = graph.artifacts[stage_id]
        if len(artifact.inputs) != 1:
            raise BuildError(
                "RUNTIME_STAGE_INPUTS",
                f"runtime staging set {stage_id} must declare exactly one stage configuration input",
                5,
            )
        config_relative = artifact.inputs[0]
        config_path = graph.repo_root / config_relative
        if config_relative not in documents:
            documents.update(_load_stage_document(graph.repo_root, config_path))
        document = documents.get(stage_id)
        if document is None:
            raise BuildError("RUNTIME_STAGE_CONFIG_ENTRY", f"{config_relative} has no entry for {stage_id}", 5)
        if document["context_id"] != context_id:
            raise BuildError(
                "RUNTIME_STAGE_CONTEXT",
                f"runtime stage {stage_id} targets {document['context_id']}, not {context_id}",
                5,
            )
        entries = document["entries"]
        assert isinstance(entries, list)
        expected_outputs = set(artifact.outputs)
        receipt_outputs = sorted(path for path in expected_outputs if Path(path).name == RECEIPT_NAME)
        if len(receipt_outputs) != 1:
            raise BuildError(
                "RUNTIME_STAGE_RECEIPT",
                f"runtime staging set {stage_id} must declare exactly one {RECEIPT_NAME} output",
                5,
            )
        receipt = receipt_outputs[0]
        payload_outputs = expected_outputs - {receipt}
        normalized_entries: list[dict[str, str]] = []
        roles: set[str] = set()
        destinations: set[str] = set()
        for index, entry in enumerate(entries):
            location = f"{config_relative}:{stage_id}.entries[{index}]"
            if not isinstance(entry, dict) or set(entry) != {"artifact_id", "destination", "role", "source"}:
                raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location} has unexpected fields", 5)
            artifact_id = entry["artifact_id"]
            role = entry["role"]
            if not isinstance(artifact_id, str) or artifact_id not in graph.artifacts:
                raise BuildError("RUNTIME_STAGE_PROVIDER", f"{location}.artifact_id is not a declared artifact", 5)
            if not isinstance(role, str) or not role:
                raise BuildError("RUNTIME_STAGE_SCHEMA", f"{location}.role must be a non-empty string", 5)
            if role in roles:
                raise BuildError("RUNTIME_STAGE_ROLE_DUPLICATE", f"{stage_id} declares role {role} more than once", 5)
            roles.add(role)
            source = _relative_path(graph.repo_root, entry["source"], f"{location}.source", must_exist=False)
            destination = _relative_path(graph.repo_root, entry["destination"], f"{location}.destination", must_exist=False)
            if source not in graph.artifacts[artifact_id].outputs:
                raise BuildError(
                    "RUNTIME_STAGE_PROVIDER_OUTPUT",
                    f"{location}.source is not an output of {artifact_id}: {source}",
                    5,
                )
            if not _stage_edges_are_declared(graph, stage_id, artifact_id, context_id):
                raise BuildError(
                    "RUNTIME_STAGE_EDGE",
                    f"runtime staging set {stage_id} lacks required stage/runtime edge to {artifact_id}",
                    5,
                )
            if destination not in payload_outputs:
                raise BuildError(
                    "RUNTIME_STAGE_OUTPUT",
                    f"{location}.destination is not a declared payload output of {stage_id}: {destination}",
                    5,
                )
            if destination in destinations:
                raise BuildError("RUNTIME_STAGE_DESTINATION_DUPLICATE", f"{stage_id} declares {destination} more than once", 5)
            destinations.add(destination)
            normalized_entries.append({
                "artifact_id": artifact_id,
                "destination": destination,
                "role": role,
                "source": source,
            })
        if destinations != payload_outputs:
            raise BuildError(
                "RUNTIME_STAGE_OUTPUT_SET",
                f"runtime staging set {stage_id} outputs do not exactly match its configuration entries",
                5,
            )
        receipt_parent = Path(receipt).parent
        if any(Path(entry["destination"]).parent != receipt_parent for entry in normalized_entries):
            raise BuildError(
                "RUNTIME_STAGE_LAYOUT",
                f"runtime staging set {stage_id} must keep payload and receipt under one owned directory",
                5,
            )
        specifications.append({
            "config": config_relative,
            "entries": tuple(sorted(normalized_entries, key=lambda item: item["role"])),
            "receipt": receipt,
            "stage_id": stage_id,
        })
    return tuple(specifications)


def _receipt_payload(graph: SemanticGraph, context_id: str, specification: Mapping[str, object]) -> dict[str, object]:
    entries = specification["entries"]
    assert isinstance(entries, tuple)
    files: list[dict[str, object]] = []
    for entry in entries:
        assert isinstance(entry, Mapping)
        source = graph.repo_root / str(entry["source"])
        if not source.is_file():
            raise BuildError("RUNTIME_STAGE_SOURCE_MISSING", f"runtime source is missing: {entry['source']}", 5)
        files.append({
            **{key: str(entry[key]) for key in ("artifact_id", "destination", "role", "source")},
            "sha256": _sha256_file(source, "RUNTIME_STAGE_SOURCE_HASH"),
            "size": source.stat().st_size,
        })
    return {
        "schema_version": STAGE_SCHEMA_VERSION,
        "semantic_graph_hash": graph.semantic_graph_hash,
        "context_id": context_id,
        "staging_set_id": specification["stage_id"],
        "files": files,
    }


def _write_if_different(path: Path, content: bytes) -> bool:
    try:
        if path.read_bytes() == content:
            return False
    except FileNotFoundError:
        pass
    except OSError as error:
        raise BuildError("RUNTIME_STAGE_OUTPUT_READ", f"could not inspect staged output {path}: {error}", 5) from error
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".stage-", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        os.replace(temporary, path)
    except OSError as error:
        raise BuildError("RUNTIME_STAGE_OUTPUT_WRITE", f"could not publish staged output {path}: {error}", 5) from error
    finally:
        temporary.unlink(missing_ok=True)
    return True


def _copy_if_different(source: Path, destination: Path) -> bool:
    source_hash = _sha256_file(source, "RUNTIME_STAGE_SOURCE_HASH")
    try:
        if destination.is_file() and _sha256_file(destination, "RUNTIME_STAGE_DESTINATION_HASH") == source_hash:
            return False
    except OSError as error:
        raise BuildError("RUNTIME_STAGE_OUTPUT_READ", f"could not inspect staged output {destination}: {error}", 5) from error
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".stage-", suffix=".tmp", dir=destination.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream, source.open("rb") as input_stream:
            shutil.copyfileobj(input_stream, stream, length=1024 * 1024)
        os.replace(temporary, destination)
    except OSError as error:
        raise BuildError("RUNTIME_STAGE_OUTPUT_WRITE", f"could not publish staged output {destination}: {error}", 5) from error
    finally:
        temporary.unlink(missing_ok=True)
    return True


@contextmanager
def _stage_lock(repo_root: Path, stage_id: str, timeout_seconds: int) -> Iterator[None]:
    if timeout_seconds < 1:
        raise BuildError("RUNTIME_STAGE_LOCK_TIMEOUT_INVALID", "stage lock timeout must be at least one second", 2)
    lock_root = repo_root / "build/staging/.locks"
    lock_root.mkdir(parents=True, exist_ok=True)
    lock_path = lock_root / f"{stage_id}-{uuid.uuid5(uuid.NAMESPACE_URL, stage_id).hex[:12]}.lock"
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            lock_path.mkdir()
            break
        except FileExistsError:
            if time.monotonic() >= deadline:
                raise BuildError("RUNTIME_STAGE_LOCK_TIMEOUT", f"timed out waiting for stage owner lock: {lock_path}", 8)
            time.sleep(_LOCK_POLL_SECONDS)
        except OSError as error:
            raise BuildError("RUNTIME_STAGE_LOCK_FAILED", f"could not acquire stage owner lock {lock_path}: {error}", 5) from error
    try:
        yield
    finally:
        try:
            lock_path.rmdir()
        except OSError as error:
            raise BuildError("RUNTIME_STAGE_LOCK_RELEASE_FAILED", f"could not release stage owner lock {lock_path}: {error}", 9) from error


def stage_runtime_artifacts(
    graph: SemanticGraph,
    product_id: str,
    context_id: str,
    *,
    timeout_seconds: int = 60,
    events: EventWriter | None = None,
) -> dict[str, object]:
    """Stage only the graph-declared runtime closure and publish receipts."""

    specifications = _stage_specification(graph, product_id, context_id)
    if not specifications:
        return {"status": "not_applicable", "context_id": context_id, "staging_sets": []}
    results: list[dict[str, object]] = []
    for specification in specifications:
        stage_id = str(specification["stage_id"])
        copied = 0
        receipt_changed = False
        with _stage_lock(graph.repo_root, stage_id, timeout_seconds):
            payload = _receipt_payload(graph, context_id, specification)
            for entry in specification["entries"]:
                assert isinstance(entry, Mapping)
                if _copy_if_different(
                    graph.repo_root / str(entry["source"]),
                    graph.repo_root / str(entry["destination"]),
                ):
                    copied += 1
            receipt = graph.repo_root / str(specification["receipt"])
            receipt_changed = _write_if_different(
                receipt,
                (_canonical_json(payload) + "\n").encode("utf-8"),
            )
        status = "staged" if copied or receipt_changed else "reused"
        result = {
            "status": status,
            "staging_set_id": stage_id,
            "copied_file_count": copied,
            "receipt_changed": receipt_changed,
            "receipt": str(specification["receipt"]),
        }
        results.append(result)
        if events is not None:
            events.emit(
                "runtime_stage",
                component_id=product_id,
                context_id=context_id,
                status=status,
                staging_set_id=stage_id,
                copied_file_count=copied,
                receipt_changed=receipt_changed,
                receipt=str(specification["receipt"]),
            )
    return {"status": "staged" if any(item["status"] == "staged" for item in results) else "reused", "context_id": context_id, "staging_sets": results}


def observe_runtime_stage(
    graph: SemanticGraph,
    product_id: str,
    context_id: str,
) -> dict[str, object]:
    """Validate that staged files exactly match declared product/resource outputs."""

    specifications = _stage_specification(graph, product_id, context_id)
    if not specifications:
        return {"valid": False, "status": "not_applicable", "context_id": context_id, "staging_sets": []}
    observations: list[dict[str, object]] = []
    failures: list[dict[str, str]] = []
    for specification in specifications:
        payload = _receipt_payload(graph, context_id, specification)
        receipt_relative = str(specification["receipt"])
        receipt_path = graph.repo_root / receipt_relative
        receipt_valid = False
        try:
            observed_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            receipt_valid = observed_receipt == payload
        except (OSError, json.JSONDecodeError):
            receipt_valid = False
        files: list[dict[str, object]] = []
        for item in payload["files"]:
            assert isinstance(item, Mapping)
            destination = graph.repo_root / str(item["destination"])
            destination_hash = _sha256_file(destination, "RUNTIME_STAGE_DESTINATION_HASH") if destination.is_file() else None
            matches = destination_hash == item["sha256"]
            files.append({
                "artifact_id": item["artifact_id"],
                "destination": item["destination"],
                "role": item["role"],
                "sha256": destination_hash,
                "source_sha256": item["sha256"],
                "matches_source": matches,
            })
            if not matches:
                failures.append({
                    "code": "RUNTIME_STAGE_CONTENT_MISMATCH",
                    "staging_set_id": str(specification["stage_id"]),
                    "destination": str(item["destination"]),
                })
        if not receipt_valid:
            failures.append({
                "code": "RUNTIME_STAGE_RECEIPT_INVALID",
                "staging_set_id": str(specification["stage_id"]),
                "receipt": receipt_relative,
            })
        observations.append({
            "staging_set_id": specification["stage_id"],
            "receipt": receipt_relative,
            "receipt_valid": receipt_valid,
            "files": files,
        })
    stable = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "context_id": context_id,
        "product_id": product_id,
        "staging_sets": observations,
        "failures": failures,
    }
    return {
        "valid": not failures,
        "status": "observed" if not failures else "invalid",
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }
