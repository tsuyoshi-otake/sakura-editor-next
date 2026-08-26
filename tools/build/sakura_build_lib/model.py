"""Strict semantic graph and build-intent model.

This module intentionally uses only the Python standard library.  MSBuild and
CMake keep ownership of low-level task scheduling; this model owns component
semantics, context projection, and requested phase closure only.
"""

from __future__ import annotations

import hashlib
import json
import re
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping


GENERATOR_VERSION = "0.3.7"

PROJECT_PROFILE_FIELDS = frozenset({
    "abi_family",
    "toolset",
    "arch",
    "configuration",
    "crt",
    "iterator_debug_level",
    "wchar_t_builtin",
    "default_pack",
    "unicode",
    "win32_winnt",
    "cpp_standard",
})

CONDITION_FIELDS = {
    "platform",
    "arch",
    "configuration",
    "toolchain",
    "backend",
    "role",
}
CONDITION_OPERATORS = {"all", "any", "not", "eq", "in", "has_feature"}
COMPONENT_KINDS = {"implementation", "contract", "composition", "executable", "test", "aggregate"}
INTERFACE_COMPONENT_KINDS = {"contract", "aggregate"}
CONTRACT_KINDS = {"inbound_api", "outbound_port", "wire_protocol", "abi_contract"}
ARTIFACT_KINDS = {
    "generated",
    "resource",
    "asset",
    "package_set",
    "staging_set",
    "test_fixture",
    "product",
    "source_set",
}
RUNTIME_ARTIFACT_KINDS = {"asset", "product", "staging_set"}
TOOL_IDS = {"sakura-module-generator", "header-make", "cmake", "msbuild", "cargo", "rc", "copy"}
MATURITY_LEVELS = {"legacy", "candidate", "transitional", "independent"}
BUILD_DEFINITIONS = {"legacy", "generated"}
EDGE_KINDS = {"api", "implementation", "tool", "asset", "package", "protocol", "test"}
PHASES = {"generate", "compile", "link", "test", "stage", "runtime", "lifecycle"}
VISIBILITIES = {"public", "private"}
PROPAGATIONS = {"none", "public"}
IMPLEMENTATION_LANGUAGES = {"cpp", "rust"}
AUTHORITY_MODES = {"none", "candidate-shadow", "production", "legacy-production", "retired"}
PRODUCTION_AUTHORITY_MODES = {"production", "legacy-production"}
PRODUCTION_EDGE_PHASES = {"link", "runtime"}


class ManifestError(ValueError):
    """A deterministic, user-actionable manifest validation error."""

    def __init__(self, code: str, location: str, message: str) -> None:
        super().__init__(f"{code} at {location}: {message}")
        self.code = code
        self.location = location
        self.message = message


def _fail(code: str, location: str, message: str) -> None:
    raise ManifestError(code, location, message)


def _canonical_text_hash(text: str) -> str:
    """Hash universal-newline text for checkout-independent projections."""

    canonical = text.replace("\r\n", "\n").replace("\r", "\n")
    return "sha256:" + hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _object(value: Any, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail("MANIFEST_TYPE", location, "expected object")
    return value


def _array(value: Any, location: str) -> list[Any]:
    if not isinstance(value, list):
        _fail("MANIFEST_TYPE", location, "expected array")
    return value


def _string(value: Any, location: str) -> str:
    if not isinstance(value, str) or not value:
        _fail("MANIFEST_TYPE", location, "expected non-empty string")
    return value


def _boolean(value: Any, location: str) -> bool:
    if not isinstance(value, bool):
        _fail("MANIFEST_TYPE", location, "expected boolean")
    return value


def _integer(value: Any, location: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        _fail("MANIFEST_TYPE", location, "expected integer")
    return value


def _closed_object(
    value: Any,
    location: str,
    *,
    required: Iterable[str],
    optional: Iterable[str] = (),
) -> dict[str, Any]:
    obj = _object(value, location)
    required_set = set(required)
    allowed = required_set | set(optional)
    unknown = sorted(set(obj) - allowed)
    missing = sorted(required_set - set(obj))
    if unknown:
        _fail("MANIFEST_UNKNOWN_FIELD", location, f"unknown fields: {', '.join(unknown)}")
    if missing:
        _fail("MANIFEST_MISSING_FIELD", location, f"missing fields: {', '.join(missing)}")
    return obj


def _string_list(value: Any, location: str, *, unique: bool = True) -> tuple[str, ...]:
    result = tuple(_string(item, f"{location}[{index}]") for index, item in enumerate(_array(value, location)))
    if unique and len(set(result)) != len(result):
        _fail("MANIFEST_DUPLICATE_VALUE", location, "duplicate values are not allowed")
    return result


def _repo_path(repo_root: Path, value: Any, location: str, *, must_exist: bool) -> str:
    raw = _string(value, location)
    candidate = Path(raw)
    if candidate.is_absolute():
        _fail("MANIFEST_ABSOLUTE_PATH", location, "path must be repository-relative")
    resolved = (repo_root / candidate).resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError:
        _fail("MANIFEST_PATH_ESCAPE", location, f"path escapes repository: {raw}")
    if must_exist and not resolved.exists():
        _fail("MANIFEST_PATH_MISSING", location, f"path does not exist: {raw}")
    return candidate.as_posix()


def _canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _version_tuple(value: str, location: str) -> tuple[int, ...]:
    try:
        parts = tuple(int(part) for part in value.split("."))
    except ValueError:
        _fail("MANIFEST_VERSION", location, f"invalid dotted version: {value}")
    if not parts:
        _fail("MANIFEST_VERSION", location, "version must not be empty")
    return parts


def normalize_condition(value: Any, location: str = "condition") -> Any:
    if value is True:
        return True
    obj = _object(value, location)
    if len(obj) != 1:
        _fail("CONDITION_SHAPE", location, "condition must contain exactly one operator")
    operator, operand = next(iter(obj.items()))
    if operator not in CONDITION_OPERATORS:
        _fail("CONDITION_OPERATOR", location, f"unsupported operator: {operator}")

    if operator in {"all", "any"}:
        children = [normalize_condition(item, f"{location}.{operator}[{index}]") for index, item in enumerate(_array(operand, f"{location}.{operator}"))]
        if not children:
            _fail("CONDITION_EMPTY", location, f"{operator} requires at least one child")
        unique = {_canonical_json(child): child for child in children}
        return {operator: [unique[key] for key in sorted(unique)]}

    if operator == "not":
        return {"not": normalize_condition(operand, f"{location}.not")}

    if operator == "has_feature":
        return {"has_feature": _string(operand, f"{location}.has_feature")}

    body = _closed_object(
        operand,
        f"{location}.{operator}",
        required={"field"},
        optional={"value"} if operator == "eq" else {"values"},
    )
    field = _string(body["field"], f"{location}.{operator}.field")
    if field not in CONDITION_FIELDS:
        _fail("CONDITION_FIELD", location, f"unsupported field: {field}")
    if operator == "eq":
        value_text = _string(body.get("value"), f"{location}.{operator}.value")
        return {"eq": {"field": field, "value": value_text}}
    values = sorted(set(_string_list(body.get("values"), f"{location}.{operator}.values")))
    if not values:
        _fail("CONDITION_EMPTY", location, "in requires at least one value")
    return {"in": {"field": field, "values": values}}


def evaluate_condition(condition: Any, context: Mapping[str, Any]) -> bool:
    if condition is True:
        return True
    operator, operand = next(iter(condition.items()))
    if operator == "all":
        return all(evaluate_condition(item, context) for item in operand)
    if operator == "any":
        return any(evaluate_condition(item, context) for item in operand)
    if operator == "not":
        return not evaluate_condition(operand, context)
    if operator == "has_feature":
        return operand in context.get("features", ())
    if operator == "eq":
        return context.get(operand["field"]) == operand["value"]
    if operator == "in":
        return context.get(operand["field"]) in operand["values"]
    raise AssertionError(f"validated condition contains unknown operator: {operator}")


@dataclass(frozen=True)
class Context:
    id: str
    platform: str
    arch: str
    configuration: str
    toolchain: str
    backend: str
    role: str
    features: tuple[str, ...]

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "platform": self.platform,
            "arch": self.arch,
            "configuration": self.configuration,
            "toolchain": self.toolchain,
            "backend": self.backend,
            "role": self.role,
            "features": list(self.features),
        }


@dataclass(frozen=True)
class Component:
    id: str
    family: str
    kind: str
    maturity: str
    build_definition: str
    supported_contexts: tuple[str, ...]
    owner: str
    sources: tuple[str, ...]
    public_headers: tuple[str, ...]
    private_headers: tuple[str, ...]
    ownership_exclusions: tuple[str, ...]
    public_include_roots: tuple[str, ...]
    private_include_roots: tuple[str, ...]
    state_owner: str | None
    backend_targets: Mapping[str, tuple[str, ...]]
    compile_profile: str | None
    system_libraries: tuple[str, ...] = ()
    implementation_language: str = "cpp"
    cargo_package: str | None = None
    cargo_target: str | None = None
    link_artifact: str | None = None
    authority_domain: str | None = None
    authority_mode: str = "none"
    thread_affinity: str = "unspecified"
    lifecycle_owner: str | None = None
    side_effects: tuple[str, ...] = ()
    provider_by_context: Mapping[str, str] = field(default_factory=dict)

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "family": self.family,
            "kind": self.kind,
            "maturity": self.maturity,
            "build_definition": self.build_definition,
            "supported_contexts": list(self.supported_contexts),
            "owner": self.owner,
            "sources": list(self.sources),
            "public_headers": list(self.public_headers),
            "private_headers": list(self.private_headers),
            "ownership_exclusions": list(self.ownership_exclusions),
            "public_include_roots": list(self.public_include_roots),
            "private_include_roots": list(self.private_include_roots),
            "state_owner": self.state_owner,
            "backend_targets": {key: list(value) for key, value in sorted(self.backend_targets.items())},
            "compile_profile": self.compile_profile,
            "implementation_language": self.implementation_language,
            "cargo_package": self.cargo_package,
            "cargo_target": self.cargo_target,
            "link_artifact": self.link_artifact,
            "authority_domain": self.authority_domain,
            "authority_mode": self.authority_mode,
            "thread_affinity": self.thread_affinity,
            "lifecycle_owner": self.lifecycle_owner,
            "side_effects": list(self.side_effects),
            "provider_by_context": dict(sorted(self.provider_by_context.items())),
            "system_libraries": list(self.system_libraries),
        }


@dataclass(frozen=True)
class Contract:
    id: str
    contract_kind: str
    contract_owner: str
    capability_id: str
    canonical_value_types: tuple[str, ...]
    contract_decision: Mapping[str, str] | None

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "contract_kind": self.contract_kind,
            "contract_owner": self.contract_owner,
            "capability_id": self.capability_id,
            "canonical_value_types": list(self.canonical_value_types),
            "contract_decision": dict(sorted(self.contract_decision.items())) if self.contract_decision else None,
        }


@dataclass(frozen=True)
class Artifact:
    id: str
    owner: str
    artifact_kind: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    tool_id: str | None
    condition: Any

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "owner": self.owner,
            "artifact_kind": self.artifact_kind,
            "inputs": list(self.inputs),
            "outputs": list(self.outputs),
            "tool_id": self.tool_id,
            "condition": self.condition,
        }


@dataclass(frozen=True)
class RuntimeProvider:
    """A runtime authority record independent of source/build ownership.

    A provider deliberately points at an existing build component instead of
    owning source paths itself.  This is the bounded representation needed for
    C++ production plus a linked Rust shadow candidate: the graph can prove
    authority and link reachability without inventing a second owner for the
    same implementation files.
    """

    id: str
    authority_domain: str
    authority_mode: str
    implementation_language: str
    build_component: str
    supported_contexts: tuple[str, ...]
    cargo_package: str | None
    cargo_target: str | None
    link_artifact: str | None
    thread_affinity: str
    lifecycle_owner: str
    side_effects: tuple[str, ...]

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "authority_domain": self.authority_domain,
            "authority_mode": self.authority_mode,
            "implementation_language": self.implementation_language,
            "build_component": self.build_component,
            "supported_contexts": list(self.supported_contexts),
            "cargo_package": self.cargo_package,
            "cargo_target": self.cargo_target,
            "link_artifact": self.link_artifact,
            "thread_affinity": self.thread_affinity,
            "lifecycle_owner": self.lifecycle_owner,
            "side_effects": list(self.side_effects),
        }


@dataclass(frozen=True)
class CompileProfiles:
    schema_version: int
    project_profiles: Mapping[str, Mapping[str, Any]]
    contract_profiles: Mapping[str, Mapping[str, Any]]
    link_profiles: Mapping[str, Mapping[str, Any]]
    context_profiles: Mapping[str, Mapping[str, str]]

    def as_mapping(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "project_profiles": [dict(self.project_profiles[key]) for key in sorted(self.project_profiles)],
            "contract_profiles": [dict(self.contract_profiles[key]) for key in sorted(self.contract_profiles)],
            "link_profiles": [dict(self.link_profiles[key]) for key in sorted(self.link_profiles)],
            "context_profiles": [dict(self.context_profiles[key]) for key in sorted(self.context_profiles)],
        }


@dataclass(frozen=True)
class Edge:
    id: str
    source: str
    target: str
    kind: str
    phases: tuple[str, ...]
    visibility: str
    propagation: str
    contract_profile: str | None
    condition: Any
    required: bool
    witnesses: tuple[Mapping[str, str], ...]

    def as_mapping(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "from": self.source,
            "to": self.target,
            "kind": self.kind,
            "phases": list(self.phases),
            "visibility": self.visibility,
            "propagation": self.propagation,
            "contract_profile": self.contract_profile,
            "condition": self.condition,
            "required": self.required,
            "witnesses": [dict(sorted(item.items())) for item in self.witnesses],
        }


@dataclass(frozen=True)
class SemanticGraph:
    repo_root: Path
    manifest_path: Path
    schema_version: int
    minimum_generator_version: str
    contexts: Mapping[str, Context]
    components: Mapping[str, Component]
    contracts: Mapping[str, Contract]
    artifacts: Mapping[str, Artifact]
    compile_profiles: CompileProfiles
    edges: tuple[Edge, ...]
    semantic_graph_hash: str
    runtime_providers: Mapping[str, RuntimeProvider] = field(default_factory=dict)

    def context(self, context_id: str) -> Context:
        try:
            return self.contexts[context_id]
        except KeyError:
            _fail("CONTEXT_UNKNOWN", "--context", f"unknown context: {context_id}")

    def active_edges(self, context_id: str) -> tuple[Edge, ...]:
        context = self.context(context_id).as_mapping()
        return tuple(
            edge
            for edge in self.edges
            if evaluate_condition(edge.condition, context)
            and self._node_supports_context(edge.source, context_id)
            and self._node_supports_context(edge.target, context_id)
        )

    def active_runtime_providers(self, context_id: str) -> tuple[RuntimeProvider, ...]:
        """Return authority records applicable to one concrete build context."""

        self.context(context_id)
        return tuple(
            provider
            for provider in sorted(self.runtime_providers.values(), key=lambda item: item.id)
            if context_id in provider.supported_contexts
        )

    def production_runtime_providers(self, context_id: str) -> Mapping[str, RuntimeProvider]:
        """Return the unique production provider for every active authority domain."""

        result: dict[str, RuntimeProvider] = {}
        for provider in self.active_runtime_providers(context_id):
            if provider.authority_mode in PRODUCTION_AUTHORITY_MODES:
                result[provider.authority_domain] = provider
        return result

    def _node_supports_context(self, node_id: str, context_id: str) -> bool:
        component = self.components.get(node_id)
        return component is None or context_id in component.supported_contexts

    def project_profile_id(self, component_id: str, context_id: str) -> str:
        """Resolve a component override or the context's default compile profile."""
        component = self.components.get(component_id)
        if component is None:
            _fail("COMPONENT_UNKNOWN", "compile_profile", f"unknown component: {component_id}")
        if context_id not in component.supported_contexts:
            _fail("COMPONENT_CONTEXT_UNSUPPORTED", "compile_profile", f"component {component_id} does not support {context_id}")
        return component.compile_profile or self.compile_profiles.context_profiles[context_id]["project_profile"]

    def project_profile(self, component_id: str, context_id: str) -> Mapping[str, Any]:
        return self.compile_profiles.project_profiles[self.project_profile_id(component_id, context_id)]

    def abi_profile_mismatches(self, context_id: str) -> tuple[Mapping[str, Any], ...]:
        """Compare consumer/provider profiles using each edge's declared policy."""
        mismatches: list[Mapping[str, Any]] = []
        for edge in self.active_edges(context_id):
            if edge.contract_profile is None or not {"compile", "link"}.intersection(edge.phases):
                continue
            if edge.source not in self.components or edge.target not in self.components:
                continue
            policy = self.compile_profiles.contract_profiles[edge.contract_profile]
            consumer = self.project_profile(edge.source, context_id)
            provider = self.project_profile(edge.target, context_id)
            for field in policy["required_project_fields"]:
                if consumer[field] != provider[field]:
                    mismatches.append({
                        "edge": edge.id,
                        "contract_profile": edge.contract_profile,
                        "field": field,
                        "consumer": edge.source,
                        "consumer_profile": consumer["id"],
                        "consumer_value": consumer[field],
                        "provider": edge.target,
                        "provider_profile": provider["id"],
                        "provider_value": provider[field],
                    })
        return tuple(mismatches)

    def _closure(
        self,
        roots: Iterable[str],
        context_id: str,
        phases: Iterable[str],
        *,
        include_private_link: bool = False,
    ) -> tuple[str, ...]:
        phase_set = set(phases)
        unknown_phases = sorted(phase_set - PHASES)
        if unknown_phases:
            _fail("PHASE_UNKNOWN", "closure", f"unknown phases: {', '.join(unknown_phases)}")
        root_set = set(roots)
        nodes = set(self.components) | set(self.contracts) | set(self.artifacts)
        unknown_roots = sorted(root_set - nodes)
        if unknown_roots:
            _fail("COMPONENT_UNKNOWN", "closure", f"unknown roots: {', '.join(unknown_roots)}")
        unsupported_roots = sorted(root for root in root_set if not self._node_supports_context(root, context_id))
        if unsupported_roots:
            _fail("COMPONENT_CONTEXT_UNSUPPORTED", "closure", f"components do not support {context_id}: {', '.join(unsupported_roots)}")
        result = set(root_set) if phase_set.intersection({"compile", "link", "test"}) else set()
        adjacency: dict[str, list[Edge]] = {}
        for edge in self.active_edges(context_id):
            if phase_set.intersection(edge.phases):
                adjacency.setdefault(edge.source, []).append(edge)
        depth = {root: 0 for root in root_set}
        pending = deque(sorted(root_set))
        while pending:
            source = pending.popleft()
            for edge in adjacency.get(source, ()):
                # A dependency edge is direct for its declaring owner, but only
                # its phase-specific public surface propagates through another
                # consumer.  Final static linking is intentionally separate:
                # private archive providers remain in that closure even when
                # public usage propagation is disabled.
                if depth[source] > 0:
                    propagated = include_private_link and phase_set == {"link"}
                    if not propagated:
                        propagated = any(
                            (
                                phase == "compile" and edge.visibility == "public"
                            )
                            or (
                                phase == "link" and edge.propagation == "public"
                            )
                            or (
                                phase not in {"compile", "link"}
                                and edge.visibility == "public"
                                and edge.propagation == "public"
                            )
                            for phase in phase_set.intersection(edge.phases)
                        )
                    if not propagated:
                        continue
                candidate_depth = depth[source] + 1
                if edge.target not in depth or candidate_depth < depth[edge.target]:
                    depth[edge.target] = candidate_depth
                    result.add(edge.target)
                    pending.append(edge.target)
        return tuple(sorted(result))

    def closure(self, roots: Iterable[str], context_id: str, phases: Iterable[str]) -> tuple[str, ...]:
        """Return the public semantic usage closure for the requested phases."""
        return self._closure(roots, context_id, phases)

    def final_link_closure(self, roots: Iterable[str], context_id: str) -> tuple[str, ...]:
        """Return every link-phase provider needed by a final static link.

        ``propagation=none`` suppresses public usage propagation; it does not
        remove a provider's implementation archive from the final link.
        """
        return self._closure(roots, context_id, ("link",), include_private_link=True)

    def _artifact_sets(
        self,
        context_id: str,
        node_ids: Iterable[str] | None = None,
    ) -> dict[str, list[str]]:
        """Project active artifact IDs by kind for a context or build closure.

        The manifest keeps artifacts in the same semantic graph as components,
        but consumers need stable kind-specific views when planning package,
        generation, or staging work.  Keep the projection ID-only and sorted so
        it remains deterministic across backends and checkout ordering.
        """
        context = self.context(context_id).as_mapping()
        allowed = set(node_ids) if node_ids is not None else None
        result = {kind: [] for kind in ("package_set", "generated", "staging_set")}
        for artifact in self.artifacts.values():
            if allowed is not None and artifact.id not in allowed:
                continue
            if not evaluate_condition(artifact.condition, context):
                continue
            values = result.get(artifact.artifact_kind)
            if values is not None:
                values.append(artifact.id)
        return {kind: sorted(values) for kind, values in result.items()}

    def project(self, context_id: str) -> dict[str, Any]:
        context = self.context(context_id)
        active_edges = self.active_edges(context_id)
        active_nodes = sorted(component.id for component in self.components.values() if context_id in component.supported_contexts)
        artifact_sets = self._artifact_sets(context_id)
        return {
            "projection_schema": 1,
            "semantic_graph_hash": self.semantic_graph_hash,
            "generator_version": GENERATOR_VERSION,
            "context_id": context_id,
            "context": context.as_mapping(),
            "active_components": active_nodes,
            "active_contracts": sorted(self.contracts),
            "active_artifacts": sorted(
                artifact.id
                for artifact in self.artifacts.values()
                if evaluate_condition(artifact.condition, context.as_mapping())
            ),
            "active_edges": [edge.id for edge in active_edges],
            "active_runtime_providers": [provider.id for provider in self.active_runtime_providers(context_id)],
            "production_runtime_providers": {
                domain: provider.id
                for domain, provider in sorted(self.production_runtime_providers(context_id).items())
            },
            "compile_profiles": [
                self.compile_profiles.context_profiles[context_id]["project_profile"],
                self.compile_profiles.context_profiles[context_id]["link_profile"],
            ],
            "package_sets": artifact_sets["package_set"],
            "generated_artifacts": artifact_sets["generated"],
            "staging_sets": artifact_sets["staging_set"],
        }

    def build_intent(self, roots: Iterable[str], context_id: str, phases: Iterable[str]) -> dict[str, Any]:
        root_list = tuple(sorted(set(roots)))
        phase_list = tuple(dict.fromkeys(phases))
        projection = self.project(context_id)
        projection_hash = "sha256:" + hashlib.sha256(_canonical_json(projection).encode("utf-8")).hexdigest()
        closures = {phase: list(self.closure(root_list, context_id, (phase,))) for phase in phase_list}
        final_link_closure = list(self.final_link_closure(root_list, context_id)) if "link" in phase_list else []
        complete_closure = sorted({node for values in closures.values() for node in values})
        artifact_sets = self._artifact_sets(context_id, (*complete_closure, *final_link_closure))
        backend_targets: dict[str, list[str]] = {}
        selected_backend = self.context(context_id).backend
        # Native build systems schedule the semantic closure through their own
        # target references.  Passing every dependency target here would build
        # the same closure twice and makes the canonical scheduler ambiguous.
        for node in root_list:
            component = self.components.get(node)
            if component is None:
                continue
            for backend, targets in component.backend_targets.items():
                if backend == selected_backend:
                    backend_targets.setdefault(backend, []).extend(targets)
        return {
            "intent_schema": 1,
            "semantic_graph_hash": self.semantic_graph_hash,
            "projection_hash": projection_hash,
            "context_id": context_id,
            "roots": list(root_list),
            "requested_phases": list(phase_list),
            "closure_by_phase": closures,
            "final_link_closure": final_link_closure,
            "backend_targets": {key: sorted(set(value)) for key, value in sorted(backend_targets.items())},
            "package_sets": artifact_sets["package_set"],
            "generated_artifacts": artifact_sets["generated"],
            "staging_sets": artifact_sets["staging_set"],
        }

    def strongly_connected_components(self, context_id: str, phase: str) -> list[tuple[str, ...]]:
        adjacency = {node: [] for node in set(self.components) | set(self.contracts) | set(self.artifacts)}
        for edge in self.active_edges(context_id):
            if phase in edge.phases:
                adjacency[edge.source].append(edge.target)

        index = 0
        indices: dict[str, int] = {}
        lowlinks: dict[str, int] = {}
        stack: list[str] = []
        on_stack: set[str] = set()
        result: list[tuple[str, ...]] = []

        def visit(node: str) -> None:
            nonlocal index
            indices[node] = index
            lowlinks[node] = index
            index += 1
            stack.append(node)
            on_stack.add(node)
            for target in adjacency[node]:
                if target not in indices:
                    visit(target)
                    lowlinks[node] = min(lowlinks[node], lowlinks[target])
                elif target in on_stack:
                    lowlinks[node] = min(lowlinks[node], indices[target])
            if lowlinks[node] == indices[node]:
                members: list[str] = []
                while True:
                    member = stack.pop()
                    on_stack.remove(member)
                    members.append(member)
                    if member == node:
                        break
                if len(members) > 1:
                    result.append(tuple(sorted(members)))

        for node in sorted(adjacency):
            if node not in indices:
                visit(node)
        return sorted(result)


def _parse_context(value: Any, location: str) -> Context:
    obj = _closed_object(
        value,
        location,
        required={"id", "platform", "arch", "configuration", "toolchain", "backend", "role", "features"},
    )
    return Context(
        id=_string(obj["id"], f"{location}.id"),
        platform=_string(obj["platform"], f"{location}.platform"),
        arch=_string(obj["arch"], f"{location}.arch"),
        configuration=_string(obj["configuration"], f"{location}.configuration"),
        toolchain=_string(obj["toolchain"], f"{location}.toolchain"),
        backend=_string(obj["backend"], f"{location}.backend"),
        role=_string(obj["role"], f"{location}.role"),
        features=_string_list(obj["features"], f"{location}.features"),
    )


def _parse_component(repo_root: Path, value: Any, location: str) -> Component:
    obj = _closed_object(
        value,
        location,
        required={"id", "family", "kind", "maturity", "build_definition", "supported_contexts", "owner", "sources", "public_headers", "private_headers", "ownership_exclusions", "public_include_roots", "private_include_roots", "state_owner", "backend_targets", "compile_profile", "implementation_language", "cargo_package", "cargo_target", "link_artifact", "authority_domain", "authority_mode", "thread_affinity", "lifecycle_owner", "side_effects", "provider_by_context"},
        optional={"system_libraries"},
    )
    kind = _string(obj["kind"], f"{location}.kind")
    if kind not in COMPONENT_KINDS:
        _fail("COMPONENT_KIND", f"{location}.kind", f"unsupported kind: {kind}")
    maturity = _string(obj["maturity"], f"{location}.maturity")
    if maturity not in MATURITY_LEVELS:
        _fail("COMPONENT_MATURITY", f"{location}.maturity", f"unsupported maturity: {maturity}")
    build_definition = _string(obj["build_definition"], f"{location}.build_definition")
    if build_definition not in BUILD_DEFINITIONS:
        _fail("COMPONENT_BUILD_DEFINITION", f"{location}.build_definition", f"unsupported build definition: {build_definition}")
    targets_obj = _object(obj["backend_targets"], f"{location}.backend_targets")
    unknown_backends = sorted(set(targets_obj) - {"msbuild", "cmake"})
    if unknown_backends:
        _fail("BACKEND_UNKNOWN", f"{location}.backend_targets", f"unknown backends: {', '.join(unknown_backends)}")
    state_owner = obj["state_owner"]
    if state_owner is not None:
        state_owner = _string(state_owner, f"{location}.state_owner")
    compile_profile = obj["compile_profile"]
    if compile_profile is not None:
        compile_profile = _string(compile_profile, f"{location}.compile_profile")
    implementation_language = _string(obj["implementation_language"], f"{location}.implementation_language")
    if implementation_language not in IMPLEMENTATION_LANGUAGES:
        _fail(
            "IMPLEMENTATION_LANGUAGE",
            f"{location}.implementation_language",
            f"unsupported implementation language: {implementation_language}",
        )

    def nullable_string(field: str) -> str | None:
        value = obj[field]
        return None if value is None else _string(value, f"{location}.{field}")

    cargo_package = nullable_string("cargo_package")
    cargo_target = nullable_string("cargo_target")
    link_artifact = nullable_string("link_artifact")
    if implementation_language == "rust":
        missing_rust_metadata = [
            field
            for field, value in (
                ("cargo_package", cargo_package),
                ("cargo_target", cargo_target),
                ("link_artifact", link_artifact),
            )
            if value is None
        ]
        if missing_rust_metadata:
            _fail(
                "RUST_METADATA_MISSING",
                location,
                f"Rust components require: {', '.join(missing_rust_metadata)}",
            )
    elif any(value is not None for value in (cargo_package, cargo_target, link_artifact)):
        _fail(
            "CPP_RUST_METADATA",
            location,
            "C++ components must not declare Cargo package, target, or link artifact metadata",
        )

    authority_domain = nullable_string("authority_domain")
    authority_mode = _string(obj["authority_mode"], f"{location}.authority_mode")
    if authority_mode not in AUTHORITY_MODES:
        _fail("AUTHORITY_MODE", f"{location}.authority_mode", f"unsupported authority mode: {authority_mode}")
    if authority_mode == "none" and authority_domain is not None:
        _fail("AUTHORITY_DOMAIN", f"{location}.authority_domain", "authority_mode 'none' must not declare a domain")
    if authority_mode != "none" and authority_domain is None:
        _fail("AUTHORITY_DOMAIN", f"{location}.authority_domain", "an authority domain is required when authority_mode is not 'none'")
    thread_affinity = _string(obj["thread_affinity"], f"{location}.thread_affinity")
    lifecycle_owner = nullable_string("lifecycle_owner")
    side_effects = _string_list(obj["side_effects"], f"{location}.side_effects")
    if authority_mode == "candidate-shadow" and side_effects:
        _fail(
            "AUTHORITY_SHADOW_SIDE_EFFECTS",
            f"{location}.side_effects",
            "candidate-shadow authorities must declare no side effects",
        )
    providers_obj = _object(obj["provider_by_context"], f"{location}.provider_by_context")
    provider_by_context: dict[str, str] = {}
    for context_id, provider in sorted(providers_obj.items()):
        if not isinstance(context_id, str) or not context_id:
            _fail("AUTHORITY_CONTEXT_REFERENCE", f"{location}.provider_by_context", "context IDs must be non-empty strings")
        provider_by_context[context_id] = _string(provider, f"{location}.provider_by_context.{context_id}")
    if authority_mode == "none" and provider_by_context:
        _fail(
            "AUTHORITY_PROVIDER_MAP",
            f"{location}.provider_by_context",
            "authority_mode 'none' must not declare providers",
        )
    system_libraries = _string_list(obj.get("system_libraries", []), f"{location}.system_libraries")
    for index, library in enumerate(system_libraries):
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", library):
            _fail("SYSTEM_LIBRARY_NAME", f"{location}.system_libraries[{index}]", "library names must not contain path separators or shell metacharacters")
    backend_targets: dict[str, tuple[str, ...]] = {}
    for backend, values in sorted(targets_obj.items()):
        parsed: list[str] = []
        for index, value in enumerate(_array(values, f"{location}.backend_targets.{backend}")):
            target_location = f"{location}.backend_targets.{backend}[{index}]"
            if backend == "msbuild":
                target = _repo_path(repo_root, value, target_location, must_exist=False)
                target_path = repo_root / target
                generated_root = (repo_root / "src/main/modules/generated").resolve()
                if not target_path.exists():
                    try:
                        target_path.resolve().relative_to(generated_root)
                    except ValueError:
                        _fail("BACKEND_TARGET_MISSING", target_location, f"MSBuild target does not exist: {target}")
                if target_path.suffix.lower() not in {".vcxproj", ".sln"}:
                    _fail("BACKEND_TARGET_KIND", target_location, "MSBuild target must be a .vcxproj or .sln")
            else:
                target = _string(value, target_location)
                if not re.fullmatch(r"[A-Za-z0-9_.+-]+", target):
                    _fail("BACKEND_TARGET_KIND", target_location, f"invalid CMake target name: {target}")
            parsed.append(target)
        if len(set(parsed)) != len(parsed):
            _fail("MANIFEST_DUPLICATE_VALUE", f"{location}.backend_targets.{backend}", "duplicate values are not allowed")
        backend_targets[backend] = tuple(parsed)
    sources = tuple(
        _repo_path(repo_root, item, f"{location}.sources[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["sources"], f"{location}.sources"))
    )
    public_headers = tuple(
        _repo_path(repo_root, item, f"{location}.public_headers[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["public_headers"], f"{location}.public_headers"))
    )
    private_headers = tuple(
        _repo_path(repo_root, item, f"{location}.private_headers[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["private_headers"], f"{location}.private_headers"))
    )
    ownership_exclusions = tuple(
        _repo_path(repo_root, item, f"{location}.ownership_exclusions[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["ownership_exclusions"], f"{location}.ownership_exclusions"))
    )
    public_include_roots = tuple(
        _repo_path(repo_root, item, f"{location}.public_include_roots[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["public_include_roots"], f"{location}.public_include_roots"))
    )
    private_include_roots = tuple(
        _repo_path(repo_root, item, f"{location}.private_include_roots[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["private_include_roots"], f"{location}.private_include_roots"))
    )
    if kind in INTERFACE_COMPONENT_KINDS:
        # The component graph can prove only ownership represented by these
        # fields. Do not
        # invent a generic thread/OS-handle lifecycle IR for unobservable
        # invariants.
        if sources:
            _fail(
                "COMPONENT_SOURCEFUL_INTERFACE",
                f"{location}.sources",
                "contract and aggregate components must not own implementation sources",
            )
        if private_headers:
            _fail(
                "COMPONENT_PRIVATE_HEADER_INTERFACE",
                f"{location}.private_headers",
                "contract and aggregate components may expose public headers only",
            )
        if private_include_roots:
            _fail(
                "COMPONENT_PRIVATE_INCLUDE_INTERFACE",
                f"{location}.private_include_roots",
                "contract and aggregate components may expose public include roots only",
            )
        if state_owner is not None:
            _fail(
                "COMPONENT_STATE_OWNER_INTERFACE",
                f"{location}.state_owner",
                "contract and aggregate components must not own state",
            )
    return Component(
        id=_string(obj["id"], f"{location}.id"),
        family=_string(obj["family"], f"{location}.family"),
        kind=kind,
        maturity=maturity,
        build_definition=build_definition,
        supported_contexts=_string_list(obj["supported_contexts"], f"{location}.supported_contexts"),
        owner=_string(obj["owner"], f"{location}.owner"),
        sources=sources,
        public_headers=public_headers,
        private_headers=private_headers,
        ownership_exclusions=ownership_exclusions,
        public_include_roots=public_include_roots,
        private_include_roots=private_include_roots,
        state_owner=state_owner,
        backend_targets=backend_targets,
        compile_profile=compile_profile,
        implementation_language=implementation_language,
        cargo_package=cargo_package,
        cargo_target=cargo_target,
        link_artifact=link_artifact,
        authority_domain=authority_domain,
        authority_mode=authority_mode,
        thread_affinity=thread_affinity,
        lifecycle_owner=lifecycle_owner,
        side_effects=side_effects,
        provider_by_context=provider_by_context,
        system_libraries=system_libraries,
    )


def _parse_contract(value: Any, location: str) -> Contract:
    obj = _closed_object(
        value,
        location,
        required={"id", "contract_kind", "contract_owner", "capability_id", "canonical_value_types", "contract_decision"},
    )
    kind = _string(obj["contract_kind"], f"{location}.contract_kind")
    if kind not in CONTRACT_KINDS:
        _fail("CONTRACT_KIND", f"{location}.contract_kind", f"unsupported kind: {kind}")
    decision_value = obj["contract_decision"]
    decision: Mapping[str, str] | None = None
    if decision_value is not None:
        decision_obj = _closed_object(
            decision_value,
            f"{location}.contract_decision",
            required={"owner_reason", "why_not_inbound_api", "adapter_owner"},
        )
        decision = {
            key: _string(decision_obj[key], f"{location}.contract_decision.{key}")
            for key in ("owner_reason", "why_not_inbound_api", "adapter_owner")
        }
    if kind == "outbound_port" and decision is None:
        _fail("CONTRACT_DECISION_MISSING", f"{location}.contract_decision", "outbound_port requires an ownership decision")
    return Contract(
        id=_string(obj["id"], f"{location}.id"),
        contract_kind=kind,
        contract_owner=_string(obj["contract_owner"], f"{location}.contract_owner"),
        capability_id=_string(obj["capability_id"], f"{location}.capability_id"),
        canonical_value_types=_string_list(obj["canonical_value_types"], f"{location}.canonical_value_types"),
        contract_decision=decision,
    )


def _parse_artifact(repo_root: Path, value: Any, location: str) -> Artifact:
    obj = _closed_object(
        value,
        location,
        required={"id", "owner", "artifact_kind", "inputs", "outputs", "tool_id", "condition"},
    )
    kind = _string(obj["artifact_kind"], f"{location}.artifact_kind")
    if kind not in ARTIFACT_KINDS:
        _fail("ARTIFACT_KIND", f"{location}.artifact_kind", f"unsupported kind: {kind}")
    tool_id = obj["tool_id"]
    if tool_id is not None:
        tool_id = _string(tool_id, f"{location}.tool_id")
        if tool_id not in TOOL_IDS:
            _fail("TOOL_ID_UNKNOWN", f"{location}.tool_id", f"unsupported tool_id: {tool_id}")
    inputs = tuple(
        _repo_path(repo_root, item, f"{location}.inputs[{index}]", must_exist=True)
        for index, item in enumerate(_array(obj["inputs"], f"{location}.inputs"))
    )
    outputs = tuple(
        _repo_path(repo_root, item, f"{location}.outputs[{index}]", must_exist=False)
        for index, item in enumerate(_array(obj["outputs"], f"{location}.outputs"))
    )
    return Artifact(
        id=_string(obj["id"], f"{location}.id"),
        owner=_string(obj["owner"], f"{location}.owner"),
        artifact_kind=kind,
        inputs=inputs,
        outputs=outputs,
        tool_id=tool_id,
        condition=normalize_condition(obj["condition"], f"{location}.condition"),
    )


def _parse_runtime_provider(value: Any, location: str) -> RuntimeProvider:
    """Parse a non-owning runtime authority record.

    Runtime providers are intentionally not Components: a provider may share
    the legacy monolith's source ownership while still having an independent
    production/candidate authority and Cargo link contract.
    """

    obj = _closed_object(
        value,
        location,
        required={
            "id",
            "authority_domain",
            "authority_mode",
            "implementation_language",
            "build_component",
            "supported_contexts",
            "cargo_package",
            "cargo_target",
            "link_artifact",
            "thread_affinity",
            "lifecycle_owner",
            "side_effects",
        },
    )
    authority_mode = _string(obj["authority_mode"], f"{location}.authority_mode")
    if authority_mode not in PRODUCTION_AUTHORITY_MODES | {"candidate-shadow", "retired"}:
        _fail("RUNTIME_PROVIDER_MODE", f"{location}.authority_mode", f"unsupported authority mode: {authority_mode}")
    implementation_language = _string(obj["implementation_language"], f"{location}.implementation_language")
    if implementation_language not in IMPLEMENTATION_LANGUAGES:
        _fail("RUNTIME_PROVIDER_LANGUAGE", f"{location}.implementation_language", f"unsupported implementation language: {implementation_language}")
    supported_contexts = _string_list(obj["supported_contexts"], f"{location}.supported_contexts")
    if not supported_contexts:
        _fail("RUNTIME_PROVIDER_CONTEXT_EMPTY", f"{location}.supported_contexts", "at least one supported context is required")

    def nullable_string(field: str) -> str | None:
        value = obj[field]
        return None if value is None else _string(value, f"{location}.{field}")

    cargo_package = nullable_string("cargo_package")
    cargo_target = nullable_string("cargo_target")
    link_artifact = nullable_string("link_artifact")
    if implementation_language == "rust":
        missing = [
            field
            for field, item in (("cargo_package", cargo_package), ("cargo_target", cargo_target), ("link_artifact", link_artifact))
            if item is None
        ]
        if missing:
            _fail("RUNTIME_PROVIDER_RUST_METADATA", location, f"Rust providers require: {', '.join(missing)}")
    elif any(item is not None for item in (cargo_package, cargo_target, link_artifact)):
        _fail("RUNTIME_PROVIDER_CPP_METADATA", location, "C++ providers must not declare Cargo or link-artifact metadata")

    side_effects = _string_list(obj["side_effects"], f"{location}.side_effects")
    if authority_mode == "candidate-shadow" and side_effects:
        _fail("RUNTIME_PROVIDER_SHADOW_SIDE_EFFECTS", f"{location}.side_effects", "candidate-shadow providers must declare no side effects")

    return RuntimeProvider(
        id=_string(obj["id"], f"{location}.id"),
        authority_domain=_string(obj["authority_domain"], f"{location}.authority_domain"),
        authority_mode=authority_mode,
        implementation_language=implementation_language,
        build_component=_string(obj["build_component"], f"{location}.build_component"),
        supported_contexts=supported_contexts,
        cargo_package=cargo_package,
        cargo_target=cargo_target,
        link_artifact=link_artifact,
        thread_affinity=_string(obj["thread_affinity"], f"{location}.thread_affinity"),
        lifecycle_owner=_string(obj["lifecycle_owner"], f"{location}.lifecycle_owner"),
        side_effects=side_effects,
    )


def _parse_edge(value: Any, location: str) -> Edge:
    obj = _closed_object(
        value,
        location,
        required={"id", "from", "to", "kind", "phases", "visibility", "propagation", "contract_profile", "condition", "required", "witnesses"},
    )
    kind = _string(obj["kind"], f"{location}.kind")
    if kind not in EDGE_KINDS:
        _fail("EDGE_KIND", f"{location}.kind", f"unsupported kind: {kind}")
    phases = _string_list(obj["phases"], f"{location}.phases")
    unknown_phases = sorted(set(phases) - PHASES)
    if unknown_phases:
        _fail("PHASE_UNKNOWN", f"{location}.phases", f"unknown phases: {', '.join(unknown_phases)}")
    visibility = _string(obj["visibility"], f"{location}.visibility")
    if visibility not in VISIBILITIES:
        _fail("EDGE_VISIBILITY", f"{location}.visibility", f"unsupported visibility: {visibility}")
    propagation = _string(obj["propagation"], f"{location}.propagation")
    if propagation not in PROPAGATIONS:
        _fail("EDGE_PROPAGATION", f"{location}.propagation", f"unsupported propagation: {propagation}")
    if visibility == "private" and propagation != "none":
        _fail("EDGE_PROPAGATION", f"{location}.propagation", "private edges must use propagation 'none'")
    contract_profile = obj["contract_profile"]
    if contract_profile is not None:
        contract_profile = _string(contract_profile, f"{location}.contract_profile")
    required = obj["required"]
    if not isinstance(required, bool):
        _fail("MANIFEST_TYPE", f"{location}.required", "expected boolean")
    witnesses: list[Mapping[str, str]] = []
    for index, item in enumerate(_array(obj["witnesses"], f"{location}.witnesses")):
        witness = _closed_object(item, f"{location}.witnesses[{index}]", required={"context", "probe"})
        witnesses.append({
            "context": _string(witness["context"], f"{location}.witnesses[{index}].context"),
            "probe": _string(witness["probe"], f"{location}.witnesses[{index}].probe"),
        })
    return Edge(
        id=_string(obj["id"], f"{location}.id"),
        source=_string(obj["from"], f"{location}.from"),
        target=_string(obj["to"], f"{location}.to"),
        kind=kind,
        phases=phases,
        visibility=visibility,
        propagation=propagation,
        contract_profile=contract_profile,
        condition=normalize_condition(obj["condition"], f"{location}.condition"),
        required=required,
        witnesses=tuple(witnesses),
    )


def _load_compile_profiles(repo_root: Path) -> CompileProfiles:
    path = repo_root / "src/main/modules/compile-profiles.json"
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        _fail("COMPILE_PROFILES_NOT_FOUND", str(path), "compile profile catalog does not exist")
    except json.JSONDecodeError as error:
        _fail("COMPILE_PROFILES_JSON", str(path), f"{error.msg} at line {error.lineno}, column {error.colno}")
    root = _closed_object(
        raw,
        "compile-profiles.$",
        required={"schema_version", "project_profiles", "contract_profiles", "link_profiles", "context_profiles"},
    )
    if root["schema_version"] != 1:
        _fail("COMPILE_PROFILES_SCHEMA", "compile-profiles.$.schema_version", "expected 1")

    def records(name: str, required: set[str], field_types: Mapping[str, str]) -> dict[str, Mapping[str, Any]]:
        result: dict[str, Mapping[str, Any]] = {}
        for index, value in enumerate(_array(root[name], f"compile-profiles.$.{name}")):
            location = f"compile-profiles.$.{name}[{index}]"
            obj = _closed_object(value, location, required=required)
            normalized: dict[str, Any] = {}
            for field in sorted(required):
                kind = field_types.get(field, "string")
                if kind == "bool":
                    normalized[field] = _boolean(obj[field], f"{location}.{field}")
                elif kind == "int":
                    normalized[field] = _integer(obj[field], f"{location}.{field}")
                elif kind == "string_list":
                    normalized[field] = _string_list(obj[field], f"{location}.{field}")
                else:
                    normalized[field] = _string(obj[field], f"{location}.{field}")
            identifier = normalized["id"]
            if identifier in result:
                _fail("COMPILE_PROFILE_DUPLICATE_ID", f"compile-profiles.$.{name}", f"duplicate ID: {identifier}")
            result[identifier] = normalized
        return result

    project_fields = {"id", *PROJECT_PROFILE_FIELDS}
    project_profiles = records(
        "project_profiles",
        project_fields,
        {"iterator_debug_level": "int", "default_pack": "int", "wchar_t_builtin": "bool", "unicode": "bool"},
    )
    contract_fields = {"id", "uses_stl_types", "crosses_allocator_boundary", "exception_boundary", "rtti_boundary", "layout_boundary", "required_project_fields"}
    contract_profiles = records(
        "contract_profiles",
        contract_fields,
        {"uses_stl_types": "bool", "crosses_allocator_boundary": "bool", "required_project_fields": "string_list"},
    )
    for profile_id, profile in contract_profiles.items():
        unknown_fields = sorted(set(profile["required_project_fields"]) - PROJECT_PROFILE_FIELDS)
        if unknown_fields:
            _fail(
                "CONTRACT_PROFILE_FIELD",
                f"compile-profiles.$.contract_profiles.{profile_id}.required_project_fields",
                f"unknown project profile fields: {', '.join(unknown_fields)}",
            )
    link_fields = {"id", "lto", "sanitizer", "debug_format", "incremental_link"}
    link_profiles = records("link_profiles", link_fields, {"incremental_link": "bool"})
    context_fields = {"id", "context_id", "project_profile", "link_profile"}
    context_records = records("context_profiles", context_fields, {})
    context_profiles: dict[str, Mapping[str, str]] = {}
    for record in context_records.values():
        context_id = record["context_id"]
        if context_id in context_profiles:
            _fail("COMPILE_PROFILE_DUPLICATE_CONTEXT", "compile-profiles.$.context_profiles", f"duplicate context: {context_id}")
        if record["project_profile"] not in project_profiles:
            _fail("COMPILE_PROFILE_REFERENCE", "compile-profiles.$.context_profiles", f"unknown project profile: {record['project_profile']}")
        if record["link_profile"] not in link_profiles:
            _fail("COMPILE_PROFILE_REFERENCE", "compile-profiles.$.context_profiles", f"unknown link profile: {record['link_profile']}")
        context_profiles[context_id] = record
    return CompileProfiles(1, project_profiles, contract_profiles, link_profiles, context_profiles)


def load_semantic_graph(
    repo_root: Path,
    manifest_path: Path | None = None,
    *,
    schema_text: str | None = None,
) -> SemanticGraph:
    repo_root = repo_root.resolve()
    manifest_path = (manifest_path or repo_root / "src/main/modules/modules.json").resolve()
    try:
        manifest_path.relative_to(repo_root)
    except ValueError:
        _fail("MANIFEST_PATH_ESCAPE", "--manifest", f"manifest must be inside repository: {manifest_path}")
    try:
        raw = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        _fail("MANIFEST_NOT_FOUND", str(manifest_path), "manifest does not exist")
    except json.JSONDecodeError as error:
        _fail("MANIFEST_JSON", str(manifest_path), f"{error.msg} at line {error.lineno}, column {error.colno}")
    root = _closed_object(raw, "$", required={"schema_version", "minimum_generator_version", "contexts", "components", "contracts", "artifacts", "runtime_providers", "edges"})
    schema_version = root["schema_version"]
    if schema_version != 4:
        _fail("MANIFEST_SCHEMA_VERSION", "$.schema_version", f"expected 4, got {schema_version!r}; run explicit manifest migrate")
    minimum_generator_version = _string(root["minimum_generator_version"], "$.minimum_generator_version")
    if _version_tuple(GENERATOR_VERSION, "generator") < _version_tuple(minimum_generator_version, "$.minimum_generator_version"):
        _fail("GENERATOR_TOO_OLD", "$.minimum_generator_version", f"generator {GENERATOR_VERSION} is older than required {minimum_generator_version}")
    compile_profiles = _load_compile_profiles(repo_root)
    schema_path = repo_root / "src/main/modules/schema-v4.json"
    try:
        checked_out_schema_text = schema_path.read_text(encoding="utf-8")
    except FileNotFoundError:
        _fail("MANIFEST_SCHEMA_NOT_FOUND", str(schema_path), "schema file does not exist")
    schema_hash = _canonical_text_hash(schema_text if schema_text is not None else checked_out_schema_text)

    contexts_list = [_parse_context(item, f"$.contexts[{index}]") for index, item in enumerate(_array(root["contexts"], "$.contexts"))]
    components_list = [_parse_component(repo_root, item, f"$.components[{index}]") for index, item in enumerate(_array(root["components"], "$.components"))]
    contracts_list = [_parse_contract(item, f"$.contracts[{index}]") for index, item in enumerate(_array(root["contracts"], "$.contracts"))]
    artifacts_list = [_parse_artifact(repo_root, item, f"$.artifacts[{index}]") for index, item in enumerate(_array(root["artifacts"], "$.artifacts"))]
    runtime_providers_list = [_parse_runtime_provider(item, f"$.runtime_providers[{index}]") for index, item in enumerate(_array(root["runtime_providers"], "$.runtime_providers"))]
    edges = tuple(_parse_edge(item, f"$.edges[{index}]") for index, item in enumerate(_array(root["edges"], "$.edges")))

    def unique(items: Iterable[Any], kind: str) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for item in items:
            if item.id in result:
                _fail("MANIFEST_DUPLICATE_ID", f"$.{kind}", f"duplicate ID: {item.id}")
            result[item.id] = item
        return result

    contexts = unique(contexts_list, "contexts")
    components = unique(components_list, "components")
    contracts = unique(contracts_list, "contracts")
    artifacts = unique(artifacts_list, "artifacts")
    runtime_providers = unique(runtime_providers_list, "runtime_providers")
    edge_ids = unique(edges, "edges")
    id_groups = (set(contexts), set(components), set(contracts), set(artifacts), set(runtime_providers), set(edge_ids))
    overlap: list[str] = []
    for left_index, left in enumerate(id_groups):
        for right in id_groups[left_index + 1 :]:
            overlap.extend(left & right)
    overlap = sorted(set(overlap))
    if overlap:
        _fail("MANIFEST_DUPLICATE_ID", "$", f"IDs must be globally unique: {', '.join(overlap)}")
    missing_profile_contexts = sorted(set(contexts) - set(compile_profiles.context_profiles))
    extra_profile_contexts = sorted(set(compile_profiles.context_profiles) - set(contexts))
    if missing_profile_contexts or extra_profile_contexts:
        _fail(
            "COMPILE_PROFILE_CONTEXT_SET",
            "compile-profiles.$.context_profiles",
            f"missing={missing_profile_contexts}, extra={extra_profile_contexts}",
        )
    known_compile_profiles = set(compile_profiles.project_profiles)
    for component in components.values():
        unknown_component_contexts = sorted(set(component.supported_contexts) - set(contexts))
        if unknown_component_contexts:
            _fail("COMPONENT_CONTEXT_REFERENCE", f"$.components.{component.id}.supported_contexts", f"unknown contexts: {', '.join(unknown_component_contexts)}")
        if not component.supported_contexts:
            _fail("COMPONENT_CONTEXT_EMPTY", f"$.components.{component.id}.supported_contexts", "at least one supported context is required")
        supported_backends = {contexts[context_id].backend for context_id in component.supported_contexts}
        missing_backends = sorted(backend for backend in supported_backends if not component.backend_targets.get(backend))
        if missing_backends:
            _fail("COMPONENT_BACKEND_TARGET_MISSING", f"$.components.{component.id}.backend_targets", f"missing targets for supported backends: {', '.join(missing_backends)}")
        msbuild_context_keys: dict[tuple[str, str], str] = {}
        for context_id in component.supported_contexts:
            context = contexts[context_id]
            if context.backend != "msbuild":
                continue
            # MSBuild property and condition matching is case-insensitive, so
            # differently-cased spellings still address the same physical
            # project configuration.
            key = (context.configuration.casefold(), context.platform.casefold())
            previous = msbuild_context_keys.get(key)
            if previous is not None:
                _fail(
                    "COMPONENT_MSBUILD_CONTEXT_COLLISION",
                    f"$.components.{component.id}.supported_contexts",
                    f"contexts {previous} and {context_id} share the MSBuild key {context.configuration}|{context.platform}",
                )
            msbuild_context_keys[key] = context_id
        if component.build_definition == "generated":
            for target in component.backend_targets.get("msbuild", ()):
                if not target.startswith("src/main/modules/generated/msbuild/projects/"):
                    _fail("GENERATED_TARGET_LOCATION", f"$.components.{component.id}.backend_targets.msbuild", f"generated MSBuild target is outside generated projects: {target}")
        if component.compile_profile is not None and component.compile_profile not in known_compile_profiles:
            _fail("COMPILE_PROFILE_REFERENCE", f"$.components.{component.id}.compile_profile", f"unknown profile: {component.compile_profile}")
        unknown_provider_contexts = sorted(set(component.provider_by_context) - set(contexts))
        if unknown_provider_contexts:
            _fail(
                "AUTHORITY_CONTEXT_REFERENCE",
                f"$.components.{component.id}.provider_by_context",
                f"unknown contexts: {', '.join(unknown_provider_contexts)}",
            )
        unsupported_provider_contexts = sorted(set(component.provider_by_context) - set(component.supported_contexts))
        if unsupported_provider_contexts:
            _fail(
                "AUTHORITY_CONTEXT_UNSUPPORTED",
                f"$.components.{component.id}.provider_by_context",
                f"provider contexts are not supported by the component: {', '.join(unsupported_provider_contexts)}",
            )
    graph_nodes = set(components) | set(contracts) | set(artifacts)
    for contract in contracts.values():
        if contract.contract_owner not in components:
            _fail("CONTRACT_OWNER", f"$.contracts.{contract.id}.contract_owner", f"unknown component: {contract.contract_owner}")
    for artifact in artifacts.values():
        if artifact.owner not in graph_nodes:
            _fail("ARTIFACT_OWNER", f"$.artifacts.{artifact.id}.owner", f"unknown owner: {artifact.owner}")
        owner = components.get(artifact.owner)
        if owner is not None and owner.kind in INTERFACE_COMPONENT_KINDS and artifact.artifact_kind in RUNTIME_ARTIFACT_KINDS:
            _fail(
                "COMPONENT_RUNTIME_ARTIFACT_OWNER",
                f"$.artifacts.{artifact.id}.owner",
                "contract and aggregate components must not own runtime-facing artifacts",
            )
    for edge in edges:
        if edge.source not in graph_nodes:
            _fail("EDGE_ENDPOINT", f"$.edges.{edge.id}.from", f"unknown graph node: {edge.source}")
        if edge.target not in graph_nodes:
            _fail("EDGE_ENDPOINT", f"$.edges.{edge.id}.to", f"unknown graph node: {edge.target}")
        source_component = components.get(edge.source)
        target_component = components.get(edge.target)
        if (
            source_component is not None
            and target_component is not None
            and source_component.build_definition == "generated"
            and target_component.build_definition == "legacy"
            and {"compile", "link"}.intersection(edge.phases)
        ):
            _fail("GENERATED_TO_LEGACY_DEPENDENCY", f"$.edges.{edge.id}", "generated components must not compile or link against the legacy monolith")
        if edge.contract_profile is not None and edge.contract_profile not in compile_profiles.contract_profiles:
            _fail("CONTRACT_PROFILE_REFERENCE", f"$.edges.{edge.id}.contract_profile", f"unknown profile: {edge.contract_profile}")
        for witness in edge.witnesses:
            if witness["context"] not in contexts:
                _fail("WITNESS_CONTEXT", f"$.edges.{edge.id}.witnesses", f"unknown context: {witness['context']}")
            _repo_path(repo_root, witness["probe"], f"$.edges.{edge.id}.witnesses.probe", must_exist=True)

    # Runtime providers are deliberately separate from Components.  Validate
    # the references here, after all graph IDs and edge conditions are known,
    # so the authority record cannot silently become a source/build owner.
    runtime_authority_records: dict[tuple[str, str], list[RuntimeProvider]] = {}
    for provider in runtime_providers.values():
        unknown_contexts = sorted(set(provider.supported_contexts) - set(contexts))
        if unknown_contexts:
            _fail(
                "RUNTIME_PROVIDER_CONTEXT_REFERENCE",
                f"$.runtime_providers.{provider.id}.supported_contexts",
                f"unknown contexts: {', '.join(unknown_contexts)}",
            )
        build_component = components.get(provider.build_component)
        if build_component is None:
            _fail(
                "RUNTIME_PROVIDER_BUILD_COMPONENT",
                f"$.runtime_providers.{provider.id}.build_component",
                f"unknown component: {provider.build_component}",
            )
        unsupported_contexts = sorted(set(provider.supported_contexts) - set(build_component.supported_contexts))
        if unsupported_contexts:
            _fail(
                "RUNTIME_PROVIDER_COMPONENT_CONTEXT",
                f"$.runtime_providers.{provider.id}.supported_contexts",
                f"build component {provider.build_component} does not support: {', '.join(unsupported_contexts)}",
            )
        if provider.link_artifact is not None:
            artifact = artifacts.get(provider.link_artifact)
            if artifact is None:
                _fail(
                    "RUNTIME_PROVIDER_LINK_ARTIFACT",
                    f"$.runtime_providers.{provider.id}.link_artifact",
                    f"unknown artifact: {provider.link_artifact}",
                )
            if artifact.owner != provider.build_component:
                _fail(
                    "RUNTIME_PROVIDER_LINK_OWNER",
                    f"$.runtime_providers.{provider.id}.link_artifact",
                    f"artifact {provider.link_artifact} must be owned by build component {provider.build_component}",
                )
        for context_id in provider.supported_contexts:
            runtime_authority_records.setdefault((provider.authority_domain, context_id), []).append(provider)

    def active_provider_edges(provider: RuntimeProvider, context_id: str) -> tuple[Edge, ...]:
        context = contexts[context_id].as_mapping()
        return tuple(
            edge
            for edge in edges
            if PRODUCTION_EDGE_PHASES.intersection(edge.phases)
            and evaluate_condition(edge.condition, context)
            and all(
                endpoint not in components or context_id in components[endpoint].supported_contexts
                for endpoint in (edge.source, edge.target)
            )
            and (
                provider.build_component in {edge.source, edge.target}
                or (provider.link_artifact is not None and provider.link_artifact in {edge.source, edge.target})
            )
        )

    for (domain, context_id), providers in sorted(runtime_authority_records.items()):
        for provider in providers:
            active_edges = active_provider_edges(provider, context_id)
            if provider.authority_mode == "retired" and active_edges:
                _fail(
                    "RUNTIME_PROVIDER_RETIRED_EDGE",
                    f"$.runtime_providers.{provider.id}.authority_mode",
                    f"retired provider has production edges in {context_id}: {', '.join(edge.id for edge in active_edges)}",
                )
        production = [
            provider
            for provider in providers
            if provider.authority_mode in PRODUCTION_AUTHORITY_MODES
        ]
        if len(production) != 1:
            _fail(
                "RUNTIME_AUTHORITY_PRODUCTION_COUNT",
                f"$.runtime_authority[{domain}][{context_id}]",
                f"expected exactly one production provider, found {len(production)}",
            )
        for provider in providers:
            active_edges = active_provider_edges(provider, context_id)
            if provider.link_artifact is not None:
                artifact_edges = tuple(
                    edge
                    for edge in active_edges
                    if provider.link_artifact in {edge.source, edge.target}
                )
                if not artifact_edges:
                    _fail(
                        "RUNTIME_PROVIDER_LINK_NOT_IN_GRAPH",
                        f"$.runtime_providers.{provider.id}.link_artifact",
                        f"link artifact {provider.link_artifact} is absent from the context link/runtime graph for {context_id}",
                    )
        production_provider = production[0]
        if not active_provider_edges(production_provider, context_id):
            _fail(
                "RUNTIME_PROVIDER_NOT_IN_GRAPH",
                f"$.runtime_providers.{production_provider.id}.build_component",
                f"production provider is absent from the context link/runtime graph for {context_id}",
            )

    # Runtime authority is intentionally separate from source/build ownership.
    # A component may describe a candidate shadow, a retained legacy provider,
    # or a production provider independently of its build_definition.
    authority_records: dict[tuple[str, str], list[tuple[Component, str]]] = {}
    for component in components.values():
        if component.authority_mode == "none":
            continue
        if not component.provider_by_context:
            _fail(
                "AUTHORITY_PROVIDER_MAP",
                f"$.components.{component.id}.provider_by_context",
                "an authority component must declare at least one applicable context",
            )
        assert component.authority_domain is not None
        for context_id, provider_id in component.provider_by_context.items():
            provider = components.get(provider_id)
            if provider is None:
                _fail(
                    "AUTHORITY_PROVIDER_REFERENCE",
                    f"$.components.{component.id}.provider_by_context.{context_id}",
                    f"unknown component provider: {provider_id}",
                )
            if context_id not in provider.supported_contexts:
                _fail(
                    "AUTHORITY_PROVIDER_CONTEXT",
                    f"$.components.{component.id}.provider_by_context.{context_id}",
                    f"provider {provider_id} does not support context {context_id}",
                )
            authority_records.setdefault((component.authority_domain, context_id), []).append((component, provider_id))

    for component in components.values():
        if component.authority_mode != "retired":
            continue
        retired_edges = [
            edge.id
            for edge in edges
            if PRODUCTION_EDGE_PHASES.intersection(edge.phases)
            and component.id in {edge.source, edge.target}
        ]
        if retired_edges:
            _fail(
                "AUTHORITY_RETIRED_EDGE",
                f"$.components.{component.id}.authority_mode",
                f"retired authority has production edges: {', '.join(sorted(retired_edges))}",
            )

    for (domain, context_id), records in sorted(authority_records.items()):
        production = [
            (component, provider_id)
            for component, provider_id in records
            if component.authority_mode in PRODUCTION_AUTHORITY_MODES
        ]
        if len(production) != 1:
            _fail(
                "AUTHORITY_PRODUCTION_COUNT",
                f"$.authority[{domain}][{context_id}]",
                f"expected exactly one production authority, found {len(production)}",
            )
        provider_id = production[0][1]
        provider = components[provider_id]
        active_production_edges = [
            edge
            for edge in edges
            if PRODUCTION_EDGE_PHASES.intersection(edge.phases)
            and provider_id in {edge.source, edge.target}
            and evaluate_condition(edge.condition, contexts[context_id].as_mapping())
            and context_id in provider.supported_contexts
        ]
        if not active_production_edges:
            _fail(
                "AUTHORITY_PROVIDER_NOT_IN_GRAPH",
                f"$.components.{production[0][0].id}.provider_by_context.{context_id}",
                f"production provider {provider_id} is absent from the context link/runtime graph",
            )

    owned_paths: list[tuple[str, Path, str]] = []
    for component in components.values():
        for category, values in (
            ("sources", component.sources),
            ("public_headers", component.public_headers),
            ("private_headers", component.private_headers),
        ):
            for value in values:
                owned_paths.append((component.id, (repo_root / value).resolve(), category))
        owned_roots = [(repo_root / value).resolve() for value in (*component.sources, *component.public_headers, *component.private_headers)]
        for exclusion in component.ownership_exclusions:
            exclusion_path = (repo_root / exclusion).resolve()
            if not any(exclusion_path == root or exclusion_path.is_relative_to(root) for root in owned_roots):
                _fail("COMPONENT_EXCLUSION_OUTSIDE_OWNERSHIP", f"$.components.{component.id}.ownership_exclusions", f"exclusion is outside owned paths: {exclusion}")
    for index, (left_owner, left_path, left_category) in enumerate(owned_paths):
        for right_owner, right_path, right_category in owned_paths[index + 1 :]:
            if left_owner == right_owner:
                continue
            try:
                left_path.relative_to(right_path)
                overlaps = True
            except ValueError:
                try:
                    right_path.relative_to(left_path)
                    overlaps = True
                except ValueError:
                    overlaps = False
            if overlaps:
                intersection = left_path if left_path.is_relative_to(right_path) else right_path
                left_exclusions = [(repo_root / value).resolve() for value in components[left_owner].ownership_exclusions]
                right_exclusions = [(repo_root / value).resolve() for value in components[right_owner].ownership_exclusions]
                left_owns_intersection = not any(intersection == item or intersection.is_relative_to(item) for item in left_exclusions)
                right_owns_intersection = not any(intersection == item or intersection.is_relative_to(item) for item in right_exclusions)
                overlaps = left_owns_intersection and right_owns_intersection
            if overlaps:
                _fail(
                    "COMPONENT_PATH_OVERLAP",
                    "$.components",
                    f"{left_owner}.{left_category} and {right_owner}.{right_category} overlap: {left_path} / {right_path}",
                )

    normalized = {
        "schema_version": schema_version,
        "minimum_generator_version": minimum_generator_version,
        "schema_hash": schema_hash,
        "contexts": [contexts[key].as_mapping() for key in sorted(contexts)],
        "components": [components[key].as_mapping() for key in sorted(components)],
        "contracts": [contracts[key].as_mapping() for key in sorted(contracts)],
        "artifacts": [artifacts[key].as_mapping() for key in sorted(artifacts)],
        "runtime_providers": [runtime_providers[key].as_mapping() for key in sorted(runtime_providers)],
        "compile_profiles": compile_profiles.as_mapping(),
        "edges": [edge_ids[key].as_mapping() for key in sorted(edge_ids)],
    }
    graph_hash = "sha256:" + hashlib.sha256(_canonical_json(normalized).encode("utf-8")).hexdigest()
    return SemanticGraph(
        repo_root=repo_root,
        manifest_path=manifest_path,
        schema_version=schema_version,
        minimum_generator_version=minimum_generator_version,
        contexts=contexts,
        components=components,
        contracts=contracts,
        artifacts=artifacts,
        runtime_providers=runtime_providers,
        compile_profiles=compile_profiles,
        edges=edges,
        semantic_graph_hash=graph_hash,
    )
