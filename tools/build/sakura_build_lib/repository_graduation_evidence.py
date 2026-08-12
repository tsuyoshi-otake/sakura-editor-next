"""Hard evidence for the non-compiler dependency classes in repository graduation.

Native product and PE-resource observations prove compiler, linker, generator,
package, and resource behavior.  This module closes the complementary classes
that cannot be inferred from a compiler command line: the staged runtime file
set, source-controlled test fixtures and stable test inventory, state-owner
inventory/ratchet, and versioned protocol fixtures with their contract runner.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path
from typing import Any, Mapping

from .model import SemanticGraph, evaluate_condition
from .product_native_evidence import validate_product_native_evidence
from .resource_native_evidence import validate_resource_native_evidence
from .runner import BuildError, cmake_component_build_dir
from .runtime_stage import observe_runtime_stage
from .semantic_inventory import collect_semantic_inventory, compare_semantic_inventory
from .test_inventory import TestInventoryError, load_inventory, verify_runtime_mappings


EVIDENCE_SCHEMA_VERSION = 1


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


def _inside_repo(repo_root: Path, value: Path, label: str) -> tuple[Path, str]:
    path = value if value.is_absolute() else repo_root / value
    try:
        relative = path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError as error:
        raise BuildError("GRADUATION_EVIDENCE_PATH", f"{label} must be inside the repository: {path}", 2) from error
    return path, relative


def _snapshot_input(repo_root: Path, relative: str) -> dict[str, object]:
    root = repo_root / relative
    if root.is_file():
        paths = (root,)
    elif root.is_dir():
        values: list[Path] = []
        for current, directories, names in os.walk(root, followlinks=False):
            directories[:] = sorted(name for name in directories if not Path(current, name).is_symlink())
            for name in sorted(names):
                candidate = Path(current, name)
                if candidate.is_symlink():
                    continue
                if candidate.is_file():
                    values.append(candidate)
        paths = tuple(values)
    else:
        raise BuildError("GRADUATION_FIXTURE_MISSING", f"fixture input is missing: {relative}", 5)
    if not paths:
        raise BuildError("GRADUATION_FIXTURE_EMPTY", f"fixture input contains no regular files: {relative}", 5)
    files: list[dict[str, object]] = []
    for path in paths:
        source_relative = path.resolve().relative_to(repo_root.resolve()).as_posix()
        files.append({
            "path": source_relative,
            "sha256": _sha256_file(path, "GRADUATION_FIXTURE_HASH"),
            "size": path.stat().st_size,
        })
    return {
        "input": relative,
        "files": files,
        "snapshot_hash": "sha256:" + hashlib.sha256(_canonical_json(files).encode("utf-8")).hexdigest(),
    }


def _active_fixture_artifacts(graph: SemanticGraph, context_id: str) -> tuple[object, ...]:
    context = graph.context(context_id).as_mapping()
    return tuple(
        artifact
        for artifact in sorted(graph.artifacts.values(), key=lambda value: value.id)
        if artifact.artifact_kind == "test_fixture" and evaluate_condition(artifact.condition, context)
    )


def _fixture_owner_edge_observed(graph: SemanticGraph, fixture_id: str, owner: str, context_id: str) -> bool:
    return any(
        edge.source == owner
        and edge.target == fixture_id
        and edge.kind == "test"
        and edge.required
        and "test" in edge.phases
        for edge in graph.active_edges(context_id)
    )


def collect_test_fixture_observation(graph: SemanticGraph, context_id: str) -> dict[str, object]:
    """Hash each active fixture and prove its test-owner edge is explicit."""

    fixtures: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []
    for artifact in _active_fixture_artifacts(graph, context_id):
        owner = graph.components.get(artifact.owner)
        owner_valid = owner is not None and owner.kind == "test"
        edge_valid = owner_valid and _fixture_owner_edge_observed(graph, artifact.id, artifact.owner, context_id)
        inputs = [_snapshot_input(graph.repo_root, relative) for relative in artifact.inputs]
        json_inputs: list[dict[str, object]] = []
        for snapshot in inputs:
            for file_info in snapshot["files"]:
                assert isinstance(file_info, Mapping)
                relative = str(file_info["path"])
                if Path(relative).suffix.lower() != ".json":
                    continue
                try:
                    parsed = json.loads((graph.repo_root / relative).read_text(encoding="utf-8"))
                    json_inputs.append({"path": relative, "root_kind": type(parsed).__name__})
                except (OSError, json.JSONDecodeError) as error:
                    failures.append({"code": "GRADUATION_FIXTURE_JSON_INVALID", "fixture": artifact.id, "path": relative, "detail": str(error)})
        if not owner_valid:
            failures.append({"code": "GRADUATION_FIXTURE_OWNER_INVALID", "fixture": artifact.id, "owner": artifact.owner})
        if not edge_valid:
            failures.append({"code": "GRADUATION_FIXTURE_EDGE_MISSING", "fixture": artifact.id, "owner": artifact.owner})
        fixtures.append({
            "fixture_id": artifact.id,
            "owner": artifact.owner,
            "owner_test_component": owner_valid,
            "owner_test_edge_observed": edge_valid,
            "inputs": inputs,
            "json_inputs": json_inputs,
        })
    if not fixtures:
        failures.append({"code": "GRADUATION_FIXTURE_NONE"})
    stable = {"context_id": context_id, "fixtures": fixtures, "failures": failures}
    return {
        "valid": not failures,
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }


def _state_owner_snapshot(graph: SemanticGraph) -> list[dict[str, str]]:
    return [
        {"component_id": component.id, "state_owner": component.state_owner}
        for component in sorted(graph.components.values(), key=lambda value: value.id)
        if component.state_owner is not None
    ]


def collect_state_owner_observation(graph: SemanticGraph, semantic_baseline: Path) -> dict[str, object]:
    """Bind declared state owners to the checked semantic-debt ratchet."""

    try:
        baseline = json.loads(semantic_baseline.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise BuildError("GRADUATION_STATE_BASELINE_MISSING", f"semantic baseline is missing: {semantic_baseline}", 5) from error
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError("GRADUATION_STATE_BASELINE_INVALID", f"could not read semantic baseline {semantic_baseline}: {error}", 5) from error
    current = collect_semantic_inventory(graph.repo_root)
    comparison = compare_semantic_inventory(current, baseline, repo_root=graph.repo_root)
    owners = _state_owner_snapshot(graph)
    owner_names: dict[str, list[str]] = {}
    for record in owners:
        owner_names.setdefault(record["state_owner"], []).append(record["component_id"])
    duplicates = [
        {"state_owner": owner, "components": components}
        for owner, components in sorted(owner_names.items())
        if len(components) > 1
    ]
    stable = {
        "semantic_baseline": semantic_baseline.resolve().relative_to(graph.repo_root.resolve()).as_posix(),
        "semantic_source_fingerprint": current["source_fingerprint"],
        "semantic_comparison_ok": bool(comparison["ok"]),
        "state_owners": owners,
        "duplicate_state_owners": duplicates,
    }
    return {
        "valid": bool(comparison["ok"]) and bool(owners) and not duplicates,
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }


def _relative_executable(graph: SemanticGraph, component_id: str, context_id: str) -> tuple[Path, str]:
    context = graph.context(context_id)
    if context.backend == "cmake":
        path = cmake_component_build_dir(graph.repo_root, component_id, context_id) / f"{component_id}.exe"
    else:
        path = graph.repo_root / f"build/components/{context_id}/{component_id}/bin/{component_id}.exe"
    if not path.is_file():
        raise BuildError(
            "GRADUATION_PROTOCOL_RUNNER_MISSING",
            f"protocol contract runner is missing; build/test {component_id} first: {path}",
            5,
        )
    return path, path.relative_to(graph.repo_root).as_posix()


def _run_protocol_runner(graph: SemanticGraph, component_id: str, context_id: str, timeout_seconds: int) -> dict[str, object]:
    if timeout_seconds < 1:
        raise BuildError("GRADUATION_PROTOCOL_TIMEOUT_INVALID", "protocol timeout must be at least one second", 2)
    executable, relative = _relative_executable(graph, component_id, context_id)
    try:
        completed = subprocess.run(
            [str(executable)],
            cwd=graph.repo_root,
            capture_output=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise BuildError("GRADUATION_PROTOCOL_TIMEOUT", f"protocol contract runner timed out: {relative}", 7) from error
    except OSError as error:
        raise BuildError("GRADUATION_PROTOCOL_START", f"could not start protocol contract runner {relative}: {error}", 7) from error
    if completed.returncode != 0:
        tail = (completed.stderr or completed.stdout)[-500:].decode("utf-8", errors="replace").strip()
        raise BuildError(
            "GRADUATION_PROTOCOL_FAILED",
            f"protocol contract runner {relative} exited {completed.returncode}: {tail or 'no diagnostic output'}",
            7,
        )
    return {
        "component_id": component_id,
        "executable": relative,
        "executable_sha256": _sha256_file(executable, "GRADUATION_PROTOCOL_RUNNER_HASH"),
        "exit_code": completed.returncode,
    }


def collect_protocol_observation(
    graph: SemanticGraph,
    context_id: str,
    fixture_observation: Mapping[str, object],
    timeout_seconds: int,
) -> dict[str, object]:
    """Validate each active wire contract against an owned fixture and runner."""

    fixtures = {
        str(item["fixture_id"]): item
        for item in fixture_observation["fixtures"]
        if isinstance(item, Mapping)
    }
    active_edges = graph.active_edges(context_id)
    contracts: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []
    for contract in sorted(graph.contracts.values(), key=lambda value: value.id):
        if contract.contract_kind != "wire_protocol":
            continue
        test_components = sorted({
            edge.source
            for edge in active_edges
            if edge.target == contract.contract_owner
            and edge.kind == "test"
            and edge.required
            and "test" in edge.phases
            and edge.source in graph.components
            and graph.components[edge.source].kind == "test"
        })
        matching_fixtures = sorted(
            fixture_id
            for fixture_id, fixture in fixtures.items()
            if fixture.get("owner") in test_components and fixture.get("owner_test_edge_observed") is True
        )
        generated_runners = [
            component_id
            for component_id in test_components
            if graph.components[component_id].build_definition == "generated"
        ]
        if not matching_fixtures:
            failures.append({"code": "GRADUATION_PROTOCOL_FIXTURE_MISSING", "contract": contract.id})
        if not generated_runners:
            failures.append({"code": "GRADUATION_PROTOCOL_RUNNER_MISSING", "contract": contract.id})
        runner_results = [
            _run_protocol_runner(graph, component_id, context_id, timeout_seconds)
            for component_id in generated_runners
        ]
        contracts.append({
            "contract_id": contract.id,
            "contract_owner": contract.contract_owner,
            "fixture_ids": matching_fixtures,
            "runner_results": runner_results,
        })
    if not contracts:
        failures.append({"code": "GRADUATION_PROTOCOL_NONE"})
    stable = {"context_id": context_id, "contracts": contracts, "failures": failures}
    return {
        "valid": not failures,
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }


def _test_inventory_observation(
    graph: SemanticGraph,
    inventory_path: Path,
    runners: Mapping[str, Path],
    timeout_seconds: int,
) -> dict[str, object]:
    try:
        inventory = load_inventory(inventory_path)
        verification = verify_runtime_mappings(inventory, runners, graph.repo_root, timeout_seconds)
    except TestInventoryError as error:
        raise BuildError("GRADUATION_TEST_INVENTORY", f"{error.code}: {error}", 7) from error
    runner_hashes: list[dict[str, str]] = []
    for runner_id, path in sorted(runners.items()):
        resolved, relative = _inside_repo(graph.repo_root, path, f"runner {runner_id}")
        if not resolved.is_file():
            raise BuildError("GRADUATION_TEST_RUNNER_MISSING", f"test inventory runner is missing: {relative}", 5)
        runner_hashes.append({
            "runner_id": runner_id,
            "path": relative,
            "sha256": _sha256_file(resolved, "GRADUATION_TEST_RUNNER_HASH"),
        })
    relative_inventory = inventory_path.resolve().relative_to(graph.repo_root.resolve()).as_posix()
    stable = {
        "inventory": relative_inventory,
        "inventory_sha256": _sha256_file(inventory_path, "GRADUATION_TEST_INVENTORY_HASH"),
        "test_count": inventory["test_count"],
        "guarantee_fingerprint": inventory["guarantee_fingerprint"],
        "runtime_verification": verification,
        "runners": runner_hashes,
    }
    return {
        "valid": bool(verification["ok"]),
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }


def _stable_payload(evidence: Mapping[str, object]) -> dict[str, object]:
    return {
        key: value
        for key, value in evidence.items()
        if key not in {"schema_version", "collection_ok", "hard_evidence_hash"}
    }


def collect_repository_graduation_evidence(
    graph: SemanticGraph,
    *,
    product_id: str,
    context_id: str,
    native_evidence_path: Path,
    resource_evidence_path: Path,
    test_inventory_path: Path,
    runners: Mapping[str, Path],
    semantic_baseline_path: Path,
    timeout_seconds: int,
) -> dict[str, object]:
    """Collect one bounded, reproducible proof for the residual R0 classes."""

    native_path, native_relative = _inside_repo(graph.repo_root, native_evidence_path, "native evidence")
    resource_path, resource_relative = _inside_repo(graph.repo_root, resource_evidence_path, "resource evidence")
    inventory_path, _inventory_relative = _inside_repo(graph.repo_root, test_inventory_path, "test inventory")
    baseline_path, _baseline_relative = _inside_repo(graph.repo_root, semantic_baseline_path, "semantic baseline")
    native = validate_product_native_evidence(graph, native_path, product_id, context_id)
    resource = validate_resource_native_evidence(graph, resource_path, native_path, product_id, context_id)
    if not native["valid"]:
        raise BuildError("GRADUATION_NATIVE_EVIDENCE_INVALID", f"native product evidence is invalid: {native.get('failures', [])}", 5)
    if not resource["valid"]:
        raise BuildError("GRADUATION_RESOURCE_EVIDENCE_INVALID", f"resource evidence is invalid: {resource.get('failures', [])}", 5)
    runtime = observe_runtime_stage(graph, product_id, context_id)
    fixtures = collect_test_fixture_observation(graph, context_id)
    state = collect_state_owner_observation(graph, baseline_path)
    test_inventory = _test_inventory_observation(graph, inventory_path, runners, timeout_seconds)
    protocol = collect_protocol_observation(graph, context_id, fixtures, timeout_seconds)
    coverage = {
        "runtime_asset": bool(runtime["valid"]),
        "test_fixture": bool(fixtures["valid"]) and bool(test_inventory["valid"]),
        "state": bool(state["valid"]),
        "protocol": bool(protocol["valid"]),
    }
    stable = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
        "native_evidence": {"path": native_relative, "hard_evidence_hash": native.get("hard_evidence_hash")},
        "resource_evidence": {"path": resource_relative, "hard_evidence_hash": resource.get("hard_evidence_hash")},
        "runtime_asset": runtime,
        "test_fixture": fixtures,
        "state": state,
        "protocol": protocol,
        "test_inventory": test_inventory,
        "coverage": coverage,
    }
    return {
        "schema_version": EVIDENCE_SCHEMA_VERSION,
        "collection_ok": True,
        **stable,
        "hard_evidence_hash": "sha256:" + hashlib.sha256(_canonical_json(stable).encode("utf-8")).hexdigest(),
    }


def _validate_recorded_fixture_observation(graph: SemanticGraph, context_id: str, expected: object) -> bool:
    if not isinstance(expected, Mapping):
        return False
    current = collect_test_fixture_observation(graph, context_id)
    return current.get("hard_evidence_hash") == expected.get("hard_evidence_hash") and current.get("valid") is True


def _validate_recorded_state_observation(graph: SemanticGraph, expected: object) -> bool:
    if not isinstance(expected, Mapping):
        return False
    baseline = expected.get("semantic_baseline")
    if not isinstance(baseline, str):
        return False
    current = collect_state_owner_observation(graph, graph.repo_root / baseline)
    return current.get("hard_evidence_hash") == expected.get("hard_evidence_hash") and current.get("valid") is True


def _validate_recorded_test_inventory(graph: SemanticGraph, expected: object) -> bool:
    if not isinstance(expected, Mapping):
        return False
    inventory = expected.get("inventory")
    runners = expected.get("runners")
    if not isinstance(inventory, str) or not isinstance(runners, list):
        return False
    inventory_path = graph.repo_root / inventory
    if not inventory_path.is_file() or _sha256_file(inventory_path, "GRADUATION_TEST_INVENTORY_HASH") != expected.get("inventory_sha256"):
        return False
    try:
        loaded = load_inventory(inventory_path)
    except TestInventoryError:
        return False
    if loaded.get("test_count") != expected.get("test_count") or loaded.get("guarantee_fingerprint") != expected.get("guarantee_fingerprint"):
        return False
    for runner in runners:
        if not isinstance(runner, Mapping) or not isinstance(runner.get("path"), str):
            return False
        path = graph.repo_root / str(runner["path"])
        if not path.is_file() or _sha256_file(path, "GRADUATION_TEST_RUNNER_HASH") != runner.get("sha256"):
            return False
    verification = expected.get("runtime_verification")
    return isinstance(verification, Mapping) and verification.get("ok") is True


def _validate_recorded_protocol(graph: SemanticGraph, context_id: str, expected: object) -> bool:
    if not isinstance(expected, Mapping) or expected.get("valid") is not True:
        return False
    for contract in expected.get("contracts", []):
        if not isinstance(contract, Mapping):
            return False
        for runner in contract.get("runner_results", []):
            if not isinstance(runner, Mapping) or not isinstance(runner.get("executable"), str):
                return False
            executable = graph.repo_root / str(runner["executable"])
            if not executable.is_file() or _sha256_file(executable, "GRADUATION_PROTOCOL_RUNNER_HASH") != runner.get("executable_sha256"):
                return False
            if runner.get("exit_code") != 0:
                return False
    return True


def validate_repository_graduation_evidence(
    graph: SemanticGraph,
    path: Path | None,
    *,
    product_id: str,
    context_id: str,
) -> dict[str, object]:
    """Revalidate persisted graduation evidence without re-running test bodies."""

    if path is None:
        return {
            "status": "not_provided",
            "valid": False,
            "evidence_path": None,
            "coverage": {"runtime_asset": False, "test_fixture": False, "state": False, "protocol": False},
            "failures": [{"code": "GRADUATION_EVIDENCE_NOT_PROVIDED"}],
        }
    resolved, relative = _inside_repo(graph.repo_root, path, "graduation evidence")
    if not resolved.is_file():
        return {
            "status": "missing",
            "valid": False,
            "evidence_path": relative,
            "coverage": {"runtime_asset": False, "test_fixture": False, "state": False, "protocol": False},
            "failures": [{"code": "GRADUATION_EVIDENCE_MISSING"}],
        }
    try:
        evidence = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError("GRADUATION_EVIDENCE_PARSE", f"could not parse {resolved}: {error}", 5) from error
    if not isinstance(evidence, dict) or evidence.get("schema_version") != EVIDENCE_SCHEMA_VERSION:
        raise BuildError("GRADUATION_EVIDENCE_SCHEMA", f"unsupported graduation evidence schema in {resolved}", 5)
    failures: list[dict[str, object]] = []
    for field, expected in {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "product_id": product_id,
        "context_id": context_id,
    }.items():
        if evidence.get(field) != expected:
            failures.append({"code": f"GRADUATION_EVIDENCE_{field.upper()}_MISMATCH", "expected": expected, "actual": evidence.get(field)})
    if evidence.get("hard_evidence_hash") != "sha256:" + hashlib.sha256(_canonical_json(_stable_payload(evidence)).encode("utf-8")).hexdigest():
        failures.append({"code": "GRADUATION_EVIDENCE_HASH_MISMATCH"})
    native = evidence.get("native_evidence") if isinstance(evidence.get("native_evidence"), Mapping) else {}
    resource = evidence.get("resource_evidence") if isinstance(evidence.get("resource_evidence"), Mapping) else {}
    native_path = graph.repo_root / str(native.get("path") or "")
    resource_path = graph.repo_root / str(resource.get("path") or "")
    native_validation = validate_product_native_evidence(graph, native_path, product_id, context_id)
    resource_validation = validate_resource_native_evidence(graph, resource_path, native_path, product_id, context_id)
    if not native_validation.get("valid") or native_validation.get("hard_evidence_hash") != native.get("hard_evidence_hash"):
        failures.append({"code": "GRADUATION_EVIDENCE_NATIVE_STALE"})
    if not resource_validation.get("valid") or resource_validation.get("hard_evidence_hash") != resource.get("hard_evidence_hash"):
        failures.append({"code": "GRADUATION_EVIDENCE_RESOURCE_STALE"})
    runtime = observe_runtime_stage(graph, product_id, context_id)
    recorded_runtime = evidence.get("runtime_asset") if isinstance(evidence.get("runtime_asset"), Mapping) else {}
    if not runtime.get("valid") or runtime.get("hard_evidence_hash") != recorded_runtime.get("hard_evidence_hash"):
        failures.append({"code": "GRADUATION_EVIDENCE_RUNTIME_STAGE_STALE"})
    if not _validate_recorded_fixture_observation(graph, context_id, evidence.get("test_fixture")):
        failures.append({"code": "GRADUATION_EVIDENCE_FIXTURE_STALE"})
    if not _validate_recorded_state_observation(graph, evidence.get("state")):
        failures.append({"code": "GRADUATION_EVIDENCE_STATE_STALE"})
    if not _validate_recorded_test_inventory(graph, evidence.get("test_inventory")):
        failures.append({"code": "GRADUATION_EVIDENCE_TEST_INVENTORY_STALE"})
    if not _validate_recorded_protocol(graph, context_id, evidence.get("protocol")):
        failures.append({"code": "GRADUATION_EVIDENCE_PROTOCOL_STALE"})
    recorded_coverage = evidence.get("coverage") if isinstance(evidence.get("coverage"), Mapping) else {}
    invalidates_all_coverage = any(
        item["code"]
        in {
            "GRADUATION_EVIDENCE_SEMANTIC_GRAPH_HASH_MISMATCH",
            "GRADUATION_EVIDENCE_PRODUCT_ID_MISMATCH",
            "GRADUATION_EVIDENCE_CONTEXT_ID_MISMATCH",
            "GRADUATION_EVIDENCE_HASH_MISMATCH",
        }
        for item in failures
    )
    stale_codes_by_class = {
        "runtime_asset": {"GRADUATION_EVIDENCE_RUNTIME_STAGE_STALE"},
        "test_fixture": {
            "GRADUATION_EVIDENCE_FIXTURE_STALE",
            "GRADUATION_EVIDENCE_TEST_INVENTORY_STALE",
        },
        "state": {"GRADUATION_EVIDENCE_STATE_STALE"},
        "protocol": {"GRADUATION_EVIDENCE_PROTOCOL_STALE"},
    }
    coverage = {
        key: (
            not invalidates_all_coverage
            and bool(recorded_coverage.get(key))
            and not any(item["code"] in stale_codes_by_class[key] for item in failures)
        )
        for key in stale_codes_by_class
    }
    return {
        "status": "observed" if not failures else "invalid",
        "valid": not failures,
        "evidence_path": relative,
        "coverage": coverage,
        "hard_evidence_hash": evidence.get("hard_evidence_hash"),
        "failures": failures,
    }


def write_repository_graduation_evidence(path: Path, evidence: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
