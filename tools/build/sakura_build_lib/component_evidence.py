"""Post-build evidence collection for one generated component closure."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Iterable

from .model import SemanticGraph
from .runner import (
    BuildError,
    NATIVE_PATH_IDENTITY_FILE,
    cmake_component_build_dir,
    find_cmake_tool,
    native_execution_root,
)


class ComponentEvidenceError(RuntimeError):
    pass


# MSVC emits this stable default set when a generated Windows target uses the
# standard CRT/Win32 project settings.  These are toolchain defaults, not
# component-owned dependencies; component-specific native APIs still have to be
# declared by the owning manifest component (for example `advapi32`).
IMPLICIT_MSVC_SYSTEM_LIBRARIES = frozenset({
    "advapi32.lib",
    "comdlg32.lib",
    "gdi32.lib",
    "kernel32.lib",
    "odbc32.lib",
    "odbccp32.lib",
    "ole32.lib",
    "oleaut32.lib",
    "shell32.lib",
    "user32.lib",
    "uuid.lib",
    "winspool.lib",
})


def _read_text(path: Path, *, encoding: str = "utf-8") -> str:
    try:
        return path.read_text(encoding=encoding, errors="replace")
    except FileNotFoundError as error:
        raise ComponentEvidenceError(f"required evidence file is missing: {path}") from error


def _relative_to_repo(repo_root: Path, value: Path) -> str | None:
    try:
        return value.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return None


def _declared_inputs(
    graph: SemanticGraph,
    component_ids: Iterable[str],
    context_id: str,
    *,
    compiled_component_ids: Iterable[str] | None = None,
) -> set[str]:
    """Return the repository inputs a native leaf is expected to consume.

    A compile-only dependency exports its public headers to the consumer but is
    not a native target in that consumer's build.  Its implementation sources
    and generated ABI force-include therefore must not be treated as observed
    inputs.  Components in the final link closure do compile, so they retain
    the full source/header/ABI input contract.
    """

    compiled = set(component_ids if compiled_component_ids is None else compiled_component_ids)
    result: set[str] = set()
    for component_id in component_ids:
        component = graph.components.get(component_id)
        if component is None:
            continue
        result.update(component.public_headers)
        if component_id not in compiled:
            continue
        result.update(component.sources)
        result.update(component.private_headers)
        if component.build_definition == "generated" and component.sources:
            result.add(f"src/main/modules/generated/abi/{context_id}/{component.id}.h")
    return result


def _expected_link_providers(graph: SemanticGraph, component_id: str, context_id: str) -> set[str]:
    result: set[str] = set()
    for dependency_id in graph.final_link_closure((component_id,), context_id):
        if dependency_id == component_id or dependency_id not in graph.components:
            continue
        dependency = graph.components[dependency_id]
        if dependency.kind not in {"contract", "aggregate"} or dependency.sources:
            result.add(f"{dependency.id}.lib".lower())
    return result


def _expected_map_members(
    graph: SemanticGraph,
    providers: Iterable[str],
) -> dict[str, tuple[str, ...]]:
    """Return linker-MAP member spellings for each generated provider.

    MSVC MAP files record selected archive members as ``target:object.obj``;
    they do not retain the ``target.lib`` suffix that appears in the link
    command.  CMake/Ninja keeps the source extension in its object name while
    MSBuild does not, so both deterministic spellings are valid evidence.
    """

    result: dict[str, tuple[str, ...]] = {}
    for provider in providers:
        component_id = provider.removesuffix(".lib")
        component = graph.components.get(component_id)
        if component is None:
            result[provider] = ()
            continue
        target_prefix = f"{component_id.lower()}:"
        members: set[str] = set()
        for source in component.sources:
            source_name = Path(source).name.lower()
            members.add(f"{target_prefix}{Path(source_name).stem}.obj")
            members.add(f"{target_prefix}{source_name}.obj")
        result[provider] = tuple(sorted(members))
    return result


def _missing_map_providers(observed_map_members: dict[str, list[str]]) -> list[str]:
    """Require an observed selected member, not a provider-name substring."""
    return sorted(provider for provider, members in observed_map_members.items() if not members)


def _hard_evidence_hash(evidence: dict[str, object]) -> str:
    """Hash path-root-independent hard component evidence.

    Absolute toolchain/SDK paths and the checkout root are deliberately absent.
    The payload retains semantic closure, repository-relative inputs, selected
    link providers/members, package/import policy, and failures.
    """
    keys = (
        "semantic_graph_hash",
        "component_id",
        "context_id",
        "backend",
        "closure",
        "final_link_closure",
        "declared_repo_inputs",
        "observed_repo_inputs",
        "expected_link_providers",
        "declared_system_libraries",
        "implicit_system_libraries",
        "observed_link_libraries",
        "expected_link_map_members",
        "observed_link_map_members",
        "package_restore_observed",
        "root_build_imports_suppressed",
        "failures",
    )
    payload = {key: evidence[key] for key in keys}
    serialized = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(serialized.encode("utf-8")).hexdigest()


def _link_libraries(command_text: str) -> set[str]:
    without_outputs = re.sub(r"/(?:implib|out):(?:\"[^\"]+\"|\S+)", "", command_text, flags=re.IGNORECASE)
    result: set[str] = set()
    for quoted, plain in re.findall(r'"([^\"]+\.lib)"|([^\s\"]+\.lib)', without_outputs, flags=re.IGNORECASE):
        token = (quoted or plain).replace("\\", "/")
        result.add(token.rsplit("/", 1)[-1].lower())
    return result


def _declared_system_libraries(graph: SemanticGraph, component_ids: Iterable[str]) -> set[str]:
    result: set[str] = set()
    for component_id in component_ids:
        component = graph.components.get(component_id)
        if component is None:
            continue
        for library in component.system_libraries:
            normalized = library.lower()
            result.add(normalized if normalized.endswith(".lib") else f"{normalized}.lib")
    return result


def _msbuild_evidence(
    graph: SemanticGraph,
    component_id: str,
    context_id: str,
    compiled_component_ids: tuple[str, ...],
) -> dict[str, object]:
    """Collect MSBuild evidence from the projects this invocation actually built.

    Compile-only edges export public headers but do not cause their provider's
    generated MSBuild project to run.  Looking through an old provider tlog in
    that case makes prior, unrelated builds appear as inputs to the current
    leaf.  The final link closure is the set of projects that do produce native
    compilation/link evidence for this invocation.
    """

    repo = graph.repo_root
    observed_repo_inputs: set[str] = set()
    external_input_count = 0
    command_parts: list[str] = []
    for current_id in compiled_component_ids:
        component = graph.components.get(current_id)
        if component is None or component.build_definition != "generated":
            continue
        obj_dir = repo / f"build/components/{context_id}/{current_id}/obj"
        read_logs = sorted(obj_dir.glob("*.tlog/CL.read.*.tlog"))
        if component.sources and not read_logs:
            raise ComponentEvidenceError(f"MSBuild compiler dependency log is missing for {current_id}: {obj_dir}")
        for log in read_logs:
            for raw_line in _read_text(log, encoding="utf-16").splitlines():
                line = raw_line.lstrip("^").strip()
                if not line or not Path(line).is_absolute():
                    continue
                relative = _relative_to_repo(repo, Path(line))
                if relative is None:
                    external_input_count += 1
                else:
                    observed_repo_inputs.add(relative)
        for pattern in ("*.tlog/CL.command.*.tlog", "*.tlog/Lib.command.*.tlog", "*.tlog/link.command.*.tlog"):
            for log in sorted(obj_dir.glob(pattern)):
                command_parts.append(_read_text(log, encoding="utf-16"))
    root_obj = repo / f"build/components/{context_id}/{component_id}/obj"
    root_link_logs = sorted(root_obj.glob("*.tlog/link.command.*.tlog"))
    if not root_link_logs:
        raise ComponentEvidenceError(f"MSBuild link command log is missing: {root_obj}")
    link_command = "\n".join(_read_text(path, encoding="utf-16") for path in root_link_logs)
    map_path = repo / f"build/components/{context_id}/{component_id}/bin/{component_id}.map"
    map_text = _read_text(map_path)
    project_targets = graph.components[component_id].backend_targets.get("msbuild", ())
    project_text = "\n".join(_read_text(repo / item) for item in project_targets)
    imports_suppressed = (
        "<ImportDirectoryBuildProps>false</ImportDirectoryBuildProps>" in project_text
        and "<ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets>" in project_text
    )
    return {
        "observed_repo_inputs": sorted(observed_repo_inputs),
        "external_input_count": external_input_count,
        "link_command": link_command,
        "map_path": map_path.relative_to(repo).as_posix(),
        "map_text": map_text,
        "build_command_text": "\n".join(command_parts),
        "root_build_imports_suppressed": imports_suppressed,
        "package_metadata_mentions": [],
    }


def _ninja_output(build_dir: Path, arguments: list[str]) -> str:
    completed = subprocess.run(
        [find_cmake_tool("ninja"), "-C", str(build_dir), *arguments],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if completed.returncode:
        raise ComponentEvidenceError(f"ninja {' '.join(arguments)} failed with exit code {completed.returncode}: {completed.stderr.strip()}")
    return completed.stdout


def _cmake_evidence(graph: SemanticGraph, component_id: str, context_id: str, declared_inputs: set[str]) -> dict[str, object]:
    repo = graph.repo_root
    relative_build_dir = cmake_component_build_dir(repo, component_id, context_id).relative_to(repo)
    identity_path = repo / relative_build_dir / NATIVE_PATH_IDENTITY_FILE
    try:
        identity = json.loads(_read_text(identity_path))
        if identity.get("schema_version") != 1:
            raise ValueError(f"unsupported schema: {identity.get('schema_version')!r}")
        recorded_repo = Path(identity["repo_root"])
        recorded_execution_root = Path(identity["execution_root"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise ComponentEvidenceError(f"invalid native path identity {identity_path}: {error}") from error
    if os.path.normcase(str(recorded_repo.resolve())) != os.path.normcase(str(repo.resolve())):
        raise ComponentEvidenceError(
            f"native path identity belongs to a different checkout: {recorded_repo} != {repo}"
        )
    try:
        with native_execution_root(repo, preferred_alias=recorded_execution_root) as execution_repo:
            build_dir = execution_repo / relative_build_dir
            dependencies = _ninja_output(build_dir, ["-t", "deps"])
            commands = _ninja_output(build_dir, ["-t", "commands", component_id])
            observed_repo_inputs: set[str] = set()
            external_input_count = 0
            for raw_line in dependencies.splitlines():
                if not raw_line[:1].isspace():
                    continue
                candidate = raw_line.strip()
                if not candidate:
                    continue
                path = Path(candidate)
                if not path.is_absolute():
                    path = build_dir / path
                relative = _relative_to_repo(repo, path)
                if relative is None:
                    external_input_count += 1
                else:
                    observed_repo_inputs.add(relative)
            normalized_commands = commands.replace("\\", "/").lower()
            for declared in declared_inputs:
                if (execution_repo / declared).as_posix().lower() in normalized_commands:
                    observed_repo_inputs.add(declared)
            map_text = _read_text(build_dir / f"{component_id}.map")
            cache_text = _read_text(build_dir / "CMakeCache.txt")
            package_mentions = [
                line
                for line in cache_text.splitlines()
                if "vcpkg" in line.lower() or line.upper().startswith("CMAKE_TOOLCHAIN_FILE")
            ]
            sentinel_texts = [
                _read_text(build_dir / "Directory.Build.props"),
                _read_text(build_dir / "Directory.Build.targets"),
            ]
    except BuildError as error:
        raise ComponentEvidenceError(str(error)) from error
    return {
        "observed_repo_inputs": sorted(observed_repo_inputs),
        "external_input_count": external_input_count,
        "link_command": commands,
        "map_path": (relative_build_dir / f"{component_id}.map").as_posix(),
        "map_text": map_text,
        "build_command_text": commands + "\n" + cache_text,
        "root_build_imports_suppressed": all(value == "<Project />\n" for value in sentinel_texts),
        "package_metadata_mentions": package_mentions,
    }


def collect_component_evidence(graph: SemanticGraph, component_id: str, context_id: str) -> dict[str, object]:
    component = graph.components.get(component_id)
    if component is None:
        raise ComponentEvidenceError(f"unknown component: {component_id}")
    context = graph.context(context_id)
    closure = graph.closure((component_id,), context_id, ("compile", "link", "test"))
    final_link_closure = graph.final_link_closure((component_id,), context_id)
    build_closure = tuple(sorted(set(closure) | set(final_link_closure)))
    declared_inputs = _declared_inputs(
        graph,
        build_closure,
        context_id,
        compiled_component_ids=final_link_closure,
    )
    if context.backend == "msbuild":
        raw = _msbuild_evidence(graph, component_id, context_id, final_link_closure)
    elif context.backend == "cmake":
        raw = _cmake_evidence(graph, component_id, context_id, declared_inputs)
    else:
        raise ComponentEvidenceError(f"unsupported evidence backend: {context.backend}")

    observed_inputs = set(raw["observed_repo_inputs"])
    expected_providers = _expected_link_providers(graph, component_id, context_id)
    declared_system_libraries = _declared_system_libraries(graph, final_link_closure)
    expected_map_members = _expected_map_members(graph, expected_providers)
    observed_libraries = _link_libraries(str(raw["link_command"]))
    implicit_system_libraries = observed_libraries & IMPLICIT_MSVC_SYSTEM_LIBRARIES
    map_text = str(raw["map_text"]).lower()
    observed_map_members = {
        provider: [member for member in members if member in map_text]
        for provider, members in expected_map_members.items()
    }
    missing_map_providers = _missing_map_providers(observed_map_members)
    forbidden_tokens = sorted({
        token
        for token in ("tests1", "sakura_lang", "bregonig", "migemo", "funccode")
        if token in str(raw["build_command_text"]).lower()
    })
    package_mentions = list(raw["package_metadata_mentions"])
    package_restore_observed = "vcpkg" in str(raw["build_command_text"]).lower() or bool(package_mentions)
    failures: list[dict[str, object]] = []

    checks = (
        ("UNDECLARED_REPO_INPUT", sorted(observed_inputs - declared_inputs)),
        ("DECLARED_INPUT_NOT_OBSERVED", sorted(declared_inputs - observed_inputs)),
        ("LINK_PROVIDER_MISSING", sorted(expected_providers - observed_libraries)),
        (
            "LINK_PROVIDER_UNDECLARED",
            sorted(observed_libraries - expected_providers - declared_system_libraries - IMPLICIT_MSVC_SYSTEM_LIBRARIES),
        ),
        ("LINK_MAP_PROVIDER_MISSING", missing_map_providers),
        ("FORBIDDEN_BUILD_INPUT", forbidden_tokens),
        ("PACKAGE_METADATA_PRESENT", package_mentions),
    )
    for code, values in checks:
        if values:
            failures.append({"code": code, "values": values})
    if package_restore_observed:
        failures.append({"code": "PACKAGE_RESTORE_OBSERVED"})
    if not raw["root_build_imports_suppressed"]:
        failures.append({"code": "ROOT_BUILD_IMPORTS_NOT_SUPPRESSED"})

    result = {
        "evidence_schema": 1,
        "ok": not failures,
        "semantic_graph_hash": graph.semantic_graph_hash,
        "component_id": component_id,
        "context_id": context_id,
        "backend": context.backend,
        "closure": list(closure),
        "final_link_closure": list(final_link_closure),
        "declared_repo_inputs": sorted(declared_inputs),
        "observed_repo_inputs": sorted(observed_inputs),
        "external_input_count": raw["external_input_count"],
        "expected_link_providers": sorted(expected_providers),
        "declared_system_libraries": sorted(declared_system_libraries),
        "implicit_system_libraries": sorted(implicit_system_libraries),
        "observed_link_libraries": sorted(observed_libraries),
        "expected_link_map_members": {
            provider: list(members) for provider, members in sorted(expected_map_members.items())
        },
        "observed_link_map_members": {
            provider: members for provider, members in sorted(observed_map_members.items())
        },
        "link_map": raw["map_path"],
        "package_restore_observed": package_restore_observed,
        "root_build_imports_suppressed": raw["root_build_imports_suppressed"],
        "failures": failures,
    }
    result["hard_evidence_hash"] = _hard_evidence_hash(result)
    return result


def write_component_evidence(path: Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
    path.write_text(text, encoding="utf-8", newline="\n")
