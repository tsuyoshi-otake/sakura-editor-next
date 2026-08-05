"""Isolated native evidence for component incremental-build closure."""

from __future__ import annotations

import hashlib
import json
import os
import re
import statistics
import subprocess
import tempfile
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from .generator import generate, stale_outputs
from .model import SemanticGraph, load_semantic_graph
from .path_matrix import _copy_minimal_repository
from .runner import (
    COMPONENT_ISOLATED_ENVIRONMENT,
    BuildError,
    EventWriter,
    cmake_component_commands,
    msbuild_command,
    msvc_environment,
)


DEFAULT_TIMEOUT_SECONDS = 300
DEFAULT_SAMPLE_COUNT = 5


@dataclass(frozen=True)
class _RunResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float


@dataclass(frozen=True)
class _Artifact:
    label: str
    action: str
    identity: str
    path: Path


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            check=False,
            text=True,
        )
    else:
        process.kill()


def _run(
    argv: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
    events: EventWriter,
) -> _RunResult:
    started = time.perf_counter()
    events.emit("rebuild_command_started", argv=list(argv), cwd=str(cwd))
    process = subprocess.Popen(
        list(argv),
        cwd=cwd,
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        _terminate_process_tree(process)
        try:
            stdout, stderr = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
        events.emit("rebuild_command_finished", argv=list(argv), exit_code=8, status="timeout")
        raise BuildError(
            "REBUILD_EVIDENCE_TIMEOUT",
            f"native command exceeded {timeout_seconds}s: {subprocess.list2cmdline(list(argv))}",
            8,
        ) from error
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    file_log = ""
    for argument in argv:
        if argument.lower().startswith("/flp:logfile="):
            log_path = Path(argument.split("=", 1)[1].split(";", 1)[0])
            try:
                raw = log_path.read_bytes()
                file_log = raw.decode("utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8", errors="replace")
            except OSError:
                file_log = ""
            break
    events.emit(
        "rebuild_command_finished",
        argv=list(argv),
        exit_code=process.returncode,
        status="success" if process.returncode == 0 else "failure",
    )
    return _RunResult(tuple(argv), process.returncode, stdout + ("\n" + file_log if file_log else ""), stderr, elapsed_ms)


def _run_all(
    commands: Sequence[Sequence[str]],
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
    events: EventWriter,
) -> list[_RunResult]:
    results: list[_RunResult] = []
    for argv in commands:
        result = _run(argv, cwd, environment, timeout_seconds, events)
        results.append(result)
        if result.returncode:
            diagnostic = (result.stdout + "\n" + result.stderr).strip().splitlines()
            detail = "\n".join(diagnostic[-20:]) if diagnostic else "no diagnostic"
            raise BuildError("REBUILD_EVIDENCE_NATIVE_FAILED", f"native command failed ({result.returncode}): {detail}", 6)
    return results


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _file_state(path: Path) -> tuple[int, int, str] | None:
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    return stat.st_mtime_ns, stat.st_size, _sha256(path)


def _snapshot(artifacts: Sequence[_Artifact]) -> dict[str, tuple[int, int, str] | None]:
    return {artifact.label: _file_state(artifact.path) for artifact in artifacts}


def _projection_snapshot(repo_root: Path) -> dict[str, tuple[int, int, str]]:
    root = repo_root / "src/main/modules/generated"
    if not root.is_dir():
        return {}
    return {
        path.relative_to(repo_root).as_posix(): (path.stat().st_mtime_ns, path.stat().st_size, _sha256(path))
        for path in sorted(root.rglob("*"), key=lambda item: item.as_posix())
        if path.is_file()
    }


def _changed_labels(
    before: Mapping[str, tuple[int, int, str] | None],
    after: Mapping[str, tuple[int, int, str] | None],
) -> set[str]:
    return {label for label in set(before) | set(after) if before.get(label) != after.get(label)}


def _changed_projections(
    before: Mapping[str, tuple[int, int, str]],
    after: Mapping[str, tuple[int, int, str]],
) -> list[str]:
    return sorted(path for path in set(before) | set(after) if before.get(path) != after.get(path))


def _artifact_actions(artifacts: Sequence[_Artifact], changed: set[str]) -> dict[str, list[str]]:
    result = {"compile": [], "archive": [], "link": []}
    for artifact in artifacts:
        if artifact.label in changed:
            result[artifact.action].append(artifact.identity)
    return {action: sorted(values) for action, values in result.items()}


def _expected_actions(graph: SemanticGraph, root_id: str, context_id: str) -> dict[str, dict[str, list[str]]]:
    closure = graph.final_link_closure((root_id,), context_id)
    source_identities = sorted(
        f"{component_id}:{source}"
        for component_id in closure
        if component_id in graph.components
        for source in graph.components[component_id].sources
    )
    providers = [
        component_id
        for component_id in closure
        if component_id in graph.components
        and graph.components[component_id].sources
        and graph.components[component_id].kind not in {"test", "executable"}
    ]
    provider = next((component_id for component_id in providers if graph.components[component_id].public_headers), None)
    if provider is None:
        raise BuildError("REBUILD_EVIDENCE_PROVIDER", "pilot closure has no sourceful provider with a public contract", 2)
    private_cpp = next((source for source in graph.components[provider].sources if Path(source).suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}), None)
    public_header = next(iter(graph.components[provider].public_headers), None)
    private_header = next(iter(graph.components[provider].private_headers), None)
    if private_cpp is None or public_header is None:
        raise BuildError(
            "REBUILD_EVIDENCE_INPUT",
            "pilot requires one provider cpp and one public header",
            2,
        )
    archive = sorted(providers)
    expected = {
        "clean": {"compile": source_identities, "archive": archive, "link": [root_id]},
        "no_op": {"compile": [], "archive": [], "link": []},
        "private_cpp": {"compile": [f"{provider}:{private_cpp}"], "archive": [provider], "link": [root_id]},
        "public_contract": {"compile": source_identities, "archive": archive, "link": [root_id]},
        "design_time": {"compile": [], "archive": [], "link": []},
        "mutation_inputs": {
            "private_cpp": [private_cpp],
            "public_contract": [public_header],
        },
    }
    if private_header is not None:
        expected["private_header"] = {
            "compile": [f"{provider}:{private_cpp}"],
            "archive": [provider],
            "link": [root_id],
        }
        expected["mutation_inputs"]["private_header"] = [private_header]
    return expected


def _mutation_applicability(graph: SemanticGraph, root_id: str, context_id: str) -> dict[str, dict[str, object]]:
    """Classify mutation kinds without manufacturing dependencies for a leaf.

    A headless leaf with no resource, component generator, or PCH must not gain
    one merely so the rebuild probe has something to touch.  Conversely, an
    omitted classification must not be mistaken for a passing mutation test.
    The manifest owns resource/generated dependencies; generated component
    projects currently make the small-leaf PCH policy explicit as NotUsing.
    """
    closure = set(graph.final_link_closure((root_id,), context_id))
    components = [graph.components[node_id] for node_id in sorted(closure) if node_id in graph.components]
    artifacts = [
        artifact
        for artifact in graph.artifacts.values()
        if artifact.owner in closure or artifact.id in closure
    ]
    resource_inputs = sorted({
        path
        for component in components
        for path in (*component.sources, *component.public_headers, *component.private_headers)
        if Path(path).suffix.lower() == ".rc"
    } | {
        path
        for artifact in artifacts
        if artifact.artifact_kind == "resource"
        for path in artifact.inputs
    })
    generated_inputs = sorted({
        path
        for artifact in artifacts
        if artifact.artifact_kind == "generated"
        for path in artifact.inputs
    })
    generated_components = sorted(
        component.id for component in components if component.build_definition == "generated" and component.sources
    )
    legacy_components = sorted(
        component.id for component in components if component.build_definition != "generated" and component.sources
    )
    pch_status = "not_applicable" if generated_components and not legacy_components else "not_classified"
    pch_reason = (
        "generated small-leaf projects explicitly set PrecompiledHeader=NotUsing and emit no "
        "target_precompile_headers; adding a PCH would enlarge invalidation for this pilot"
        if pch_status == "not_applicable"
        else "the closure contains a legacy source owner whose PCH policy is not represented by the semantic manifest"
    )
    return {
        "pch": {
            "status": pch_status,
            "applicable": False if pch_status == "not_applicable" else None,
            "reason": pch_reason,
            "witnesses": generated_components if pch_status == "not_applicable" else legacy_components,
        },
        "resource": {
            "status": "applicable" if resource_inputs else "not_applicable",
            "applicable": bool(resource_inputs),
            "reason": "owned resource inputs exist" if resource_inputs else "the headless leaf closure owns no resource input",
            "witnesses": resource_inputs,
        },
        "component_generated_input": {
            "status": "applicable" if generated_inputs else "not_applicable",
            "applicable": bool(generated_inputs),
            "reason": (
                "owned generated-artifact inputs exist"
                if generated_inputs
                else "the leaf closure owns no component-specific generated artifact"
            ),
            "witnesses": generated_inputs,
        },
        "projection_input": {
            "status": "covered_by_staleness_gate",
            "applicable": True,
            "reason": "modules.json and compile-profiles.json are generator inputs; ordinary component builds must fail stale instead of regenerating",
            "witnesses": ["src/main/modules/modules.json", "src/main/modules/compile-profiles.json"],
        },
    }


def _unique_path(paths: Sequence[Path], description: str) -> Path:
    matches = [path for path in paths if path.is_file()]
    if len(matches) != 1:
        rendered = ", ".join(str(path) for path in matches) or "none"
        raise BuildError("REBUILD_EVIDENCE_ARTIFACT", f"expected one {description}, found {len(matches)}: {rendered}", 6)
    return matches[0]


def _discover_artifacts(graph: SemanticGraph, root_id: str, context_id: str) -> list[_Artifact]:
    context = graph.context(context_id)
    closure = graph.final_link_closure((root_id,), context_id)
    artifacts: list[_Artifact] = []
    if context.backend == "msbuild":
        for component_id in closure:
            component = graph.components.get(component_id)
            if component is None:
                continue
            output_root = graph.repo_root / f"build/components/{context_id}/{component_id}"
            for source in component.sources:
                artifacts.append(_Artifact(
                    f"compile:{component_id}:{source}",
                    "compile",
                    f"{component_id}:{source}",
                    output_root / f"obj/{Path(source).stem}.obj",
                ))
            if component.sources and component.kind not in {"test", "executable"}:
                artifacts.append(_Artifact(
                    f"archive:{component_id}",
                    "archive",
                    component_id,
                    output_root / f"bin/{component_id}.lib",
                ))
        artifacts.append(_Artifact(
            f"link:{root_id}",
            "link",
            root_id,
            graph.repo_root / f"build/components/{context_id}/{root_id}/bin/{root_id}.exe",
        ))
        return artifacts

    build_dir = graph.repo_root / f"build/components/{context_id}/{root_id}/cmake-ninja-isolated"
    object_paths = tuple(build_dir.rglob("*.obj"))
    for component_id in closure:
        component = graph.components.get(component_id)
        if component is None:
            continue
        for source in component.sources:
            source_name = Path(source).name.lower()
            candidates = [
                path for path in object_paths
                if path.name.lower() in {f"{source_name}.obj", f"{Path(source_name).stem}.obj"}
                and f"CMakeFiles/{component_id}.dir".lower() in path.as_posix().lower()
            ]
            object_path = _unique_path(candidates, f"object for {component_id}:{source}")
            artifacts.append(_Artifact(
                f"compile:{component_id}:{source}",
                "compile",
                f"{component_id}:{source}",
                object_path,
            ))
        if component.sources and component.kind not in {"test", "executable"}:
            archive = _unique_path(tuple(build_dir.glob(f"{component_id}.lib")), f"archive for {component_id}")
            artifacts.append(_Artifact(f"archive:{component_id}", "archive", component_id, archive))
    executable = _unique_path(tuple(build_dir.glob(f"{root_id}.exe")), f"executable for {root_id}")
    artifacts.append(_Artifact(f"link:{root_id}", "link", root_id, executable))
    return artifacts


def _phase_result(
    phase_id: str,
    expected: Mapping[str, list[str]],
    observed: Mapping[str, list[str]],
    projection_changes: Sequence[str],
    runs: Sequence[_RunResult],
    *,
    test_exit_code: int | None,
    allowed_explicit_configure_count: int = 0,
) -> dict[str, object]:
    missing = {
        action: sorted(set(expected[action]) - set(observed[action]))
        for action in ("compile", "archive", "link")
    }
    unexpected = {
        action: sorted(set(observed[action]) - set(expected[action]))
        for action in ("compile", "archive", "link")
    }
    combined_log = "\n".join(result.stdout + "\n" + result.stderr for result in runs)
    package_restore = bool(re.search(r"(?i)(?:vcpkg\.exe|VcpkgRestore)", combined_log))
    configure_count = sum(1 for result in runs if "-S" in result.argv)
    failures = [
        *(f"missing {action}: {', '.join(values)}" for action, values in missing.items() if values),
        *(f"unexpected {action}: {', '.join(values)}" for action, values in unexpected.items() if values),
    ]
    if projection_changes:
        failures.append(f"generated projections changed: {', '.join(projection_changes)}")
    if package_restore:
        failures.append("package restore was observed")
    if configure_count > allowed_explicit_configure_count:
        failures.append(
            f"explicit configure count {configure_count} exceeds {allowed_explicit_configure_count}"
        )
    if test_exit_code not in {None, 0}:
        failures.append(f"component test exited {test_exit_code}")
    return {
        "id": phase_id,
        "ok": not failures,
        "expected": dict(expected),
        "observed": dict(observed),
        "missing": missing,
        "unexpected": unexpected,
        "projection_changes": list(projection_changes),
        "package_restore_observed": package_restore,
        "explicit_configure_count": configure_count,
        "native_command_count": len(runs),
        "elapsed_ms": round(sum(result.elapsed_ms for result in runs), 3),
        "test_exit_code": test_exit_code,
        "failures": failures,
    }


def _append_mutation(path: Path, marker: str) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(f"\n// {marker}\n")


def _environment_for_context(graph: SemanticGraph, context_id: str) -> dict[str, str]:
    context = graph.context(context_id)
    environment = dict(os.environ)
    environment["MSBUILDDISABLENODEREUSE"] = "1"
    if context.toolchain == "msvc" and context.backend == "cmake":
        environment.update(msvc_environment(graph.repo_root, environment))
        blocked = {name.upper() for name in COMPONENT_ISOLATED_ENVIRONMENT}
        environment = {key: value for key, value in environment.items() if key.upper() not in blocked}
        # The direct subprocess path used by the rebuild rehearsal does not
        # pass through runner.run_commands, so pin the same English
        # `/showIncludes` prefix here. Without this, an ambient Japanese
        # VSLANG makes every CMake phase reconfigure and invalidates the
        # no-op/rebuild-closure measurement.
        environment["VSLANG"] = "1033"
    return environment


def _native_commands(
    graph: SemanticGraph,
    root_id: str,
    context_id: str,
    jobs: int,
    phase_id: str,
    log_root: Path,
    *,
    configure_if_needed: bool = True,
) -> list[list[str]]:
    context = graph.context(context_id)
    if context.backend == "msbuild":
        target = graph.repo_root / graph.components[root_id].backend_targets["msbuild"][0]
        command = msbuild_command(
            graph.repo_root,
            target,
            context.platform,
            context.configuration,
            jobs,
        )
        log = log_root / f"{phase_id}-{uuid.uuid4().hex}.log"
        command.extend(["/v:minimal", "/fl", f"/flp:logfile={log};verbosity=diagnostic"])
        return [command]
    commands = cmake_component_commands(
        graph.repo_root,
        root_id,
        context_id,
        context.configuration,
        context.toolchain,
        jobs,
        configure_if_needed=configure_if_needed,
    )
    for command in commands:
        if "--build" in command:
            command.append("--verbose")
    return commands


def _run_test_executable(graph: SemanticGraph, root_id: str, context_id: str, environment: Mapping[str, str], timeout_seconds: int, events: EventWriter) -> int:
    context = graph.context(context_id)
    if context.backend == "msbuild":
        executable = graph.repo_root / f"build/components/{context_id}/{root_id}/bin/{root_id}.exe"
    else:
        executable = graph.repo_root / f"build/components/{context_id}/{root_id}/cmake-ninja-isolated/{root_id}.exe"
    return _run([str(executable)], graph.repo_root, environment, timeout_seconds, events).returncode


def _run_phase(
    graph: SemanticGraph,
    root_id: str,
    context_id: str,
    jobs: int,
    phase_id: str,
    expected: Mapping[str, list[str]],
    artifacts: Sequence[_Artifact] | None,
    environment: Mapping[str, str],
    timeout_seconds: int,
    events: EventWriter,
    log_root: Path,
    *,
    run_test: bool,
) -> tuple[dict[str, object], list[_Artifact]]:
    before_projection = _projection_snapshot(graph.repo_root)
    before = _snapshot(artifacts or ())
    commands = _native_commands(
        graph,
        root_id,
        context_id,
        jobs,
        phase_id,
        log_root,
        configure_if_needed=artifacts is None,
    )
    results = _run_all(commands, graph.repo_root, environment, timeout_seconds, events)
    current_artifacts = list(artifacts or _discover_artifacts(graph, root_id, context_id))
    after = _snapshot(current_artifacts)
    changed = (
        {label for label, state in after.items() if state is not None}
        if artifacts is None
        else _changed_labels(before, after)
    )
    observed = _artifact_actions(current_artifacts, changed)
    after_projection = _projection_snapshot(graph.repo_root)
    test_exit = _run_test_executable(graph, root_id, context_id, environment, timeout_seconds, events) if run_test else None
    return (
        _phase_result(
            phase_id,
            expected,
            observed,
            _changed_projections(before_projection, after_projection),
            results,
            test_exit_code=test_exit,
            allowed_explicit_configure_count=(1 if artifacts is None and graph.context(context_id).backend == "cmake" else 0),
        ),
        current_artifacts,
    )


def _design_time_evidence(
    graph: SemanticGraph,
    root_id: str,
    context_id: str,
    samples: int,
    artifacts: Sequence[_Artifact],
    environment: Mapping[str, str],
    timeout_seconds: int,
    events: EventWriter,
    log_root: Path,
) -> dict[str, object] | None:
    context = graph.context(context_id)
    if context.backend != "msbuild":
        return None
    expected = {"compile": [], "archive": [], "link": []}
    sample_reports: list[dict[str, object]] = []
    elapsed: list[float] = []
    target = graph.repo_root / graph.components[root_id].backend_targets["msbuild"][0]
    for index in range(samples):
        before = _snapshot(artifacts)
        before_projection = _projection_snapshot(graph.repo_root)
        command = msbuild_command(
            graph.repo_root,
            target,
            context.platform,
            context.configuration,
            1,
            build_target="ClCompile",
        )
        log = log_root / f"design-time-{index}-{uuid.uuid4().hex}.log"
        command.extend([
            "/p:DesignTimeBuild=true",
            "/p:SkipCompilerExecution=true",
            "/v:minimal",
            "/fl",
            f"/flp:logfile={log};verbosity=diagnostic",
        ])
        results = _run_all([command], graph.repo_root, environment, timeout_seconds, events)
        elapsed.append(results[0].elapsed_ms)
        observed = _artifact_actions(artifacts, _changed_labels(before, _snapshot(artifacts)))
        report = _phase_result(
            f"design_time_{index + 1}",
            expected,
            observed,
            _changed_projections(before_projection, _projection_snapshot(graph.repo_root)),
            results,
            test_exit_code=None,
        )
        sample_reports.append(report)
    failures = [failure for report in sample_reports for failure in report["failures"]]
    return {
        "ok": not failures,
        "sample_count": samples,
        "samples_ms": [round(value, 3) for value in elapsed],
        "median_ms": round(statistics.median(elapsed), 3),
        "failures": failures,
        "samples": sample_reports,
        "comparison_status": "baseline_only",
    }


def _not_applicable_phase(phase_id: str, reason: str) -> dict[str, object]:
    return {
        "id": phase_id,
        "ok": True,
        "status": "not_applicable",
        "expected": {"compile": [], "archive": [], "link": []},
        "observed": {"compile": [], "archive": [], "link": []},
        "missing": {"compile": [], "archive": [], "link": []},
        "unexpected": {"compile": [], "archive": [], "link": []},
        "projection_changes": [],
        "package_restore_observed": False,
        "explicit_configure_count": 0,
        "native_command_count": 0,
        "elapsed_ms": 0.0,
        "test_exit_code": None,
        "failures": [],
        "reason": reason,
    }


def _context_rehearsal(
    graph: SemanticGraph,
    root_id: str,
    context_id: str,
    jobs: int,
    samples: int,
    timeout_seconds: int,
    events: EventWriter,
) -> dict[str, object]:
    expected = _expected_actions(graph, root_id, context_id)
    mutation_inputs = expected.pop("mutation_inputs")
    assert isinstance(mutation_inputs, dict)
    environment = _environment_for_context(graph, context_id)
    log_root = graph.repo_root / f"build/evidence/rebuild-closure/{context_id}"
    log_root.mkdir(parents=True, exist_ok=True)

    clean, artifacts = _run_phase(
        graph, root_id, context_id, jobs, "clean", expected["clean"], None,
        environment, timeout_seconds, events, log_root, run_test=True,
    )

    no_op_samples: list[dict[str, object]] = []
    for index in range(samples):
        sample, artifacts = _run_phase(
            graph, root_id, context_id, jobs, f"no_op_{index + 1}", expected["no_op"], artifacts,
            environment, timeout_seconds, events, log_root, run_test=False,
        )
        no_op_samples.append(sample)
    no_op_failures = [failure for sample in no_op_samples for failure in sample["failures"]]
    no_op = {
        "id": "no_op",
        "ok": not no_op_failures,
        "sample_count": samples,
        "samples_ms": [sample["elapsed_ms"] for sample in no_op_samples],
        "median_ms": round(statistics.median(float(sample["elapsed_ms"]) for sample in no_op_samples), 3),
        "explicit_configure_count": sum(int(sample["explicit_configure_count"]) for sample in no_op_samples),
        "failures": no_op_failures,
        "samples": no_op_samples,
    }

    private_cpp_path = graph.repo_root / mutation_inputs["private_cpp"][0]
    _append_mutation(private_cpp_path, "sakura rebuild evidence private cpp")
    private_cpp, artifacts = _run_phase(
        graph, root_id, context_id, jobs, "private_cpp", expected["private_cpp"], artifacts,
        environment, timeout_seconds, events, log_root, run_test=True,
    )

    if "private_header" in mutation_inputs:
        private_header_path = graph.repo_root / mutation_inputs["private_header"][0]
        _append_mutation(private_header_path, "sakura rebuild evidence private header")
        private_header, artifacts = _run_phase(
            graph, root_id, context_id, jobs, "private_header", expected["private_header"], artifacts,
            environment, timeout_seconds, events, log_root, run_test=True,
        )
    else:
        private_header = _not_applicable_phase(
            "private_header",
            "provider declares no private header; private-header invalidation is not applicable",
        )

    public_header_path = graph.repo_root / mutation_inputs["public_contract"][0]
    _append_mutation(public_header_path, "sakura rebuild evidence public contract")
    public_contract, artifacts = _run_phase(
        graph, root_id, context_id, jobs, "public_contract", expected["public_contract"], artifacts,
        environment, timeout_seconds, events, log_root, run_test=True,
    )

    design_time = _design_time_evidence(
        graph, root_id, context_id, samples, artifacts, environment,
        timeout_seconds, events, log_root,
    )
    phases = [clean, no_op, private_cpp, private_header, public_contract]
    failures = [failure for phase in phases for failure in phase["failures"]]
    if design_time is not None:
        failures.extend(design_time["failures"])
    return {
        "context_id": context_id,
        "backend": graph.context(context_id).backend,
        "ok": not failures,
        "expected": expected,
        "mutation_applicability": _mutation_applicability(graph, root_id, context_id),
        "phases": phases,
        "design_time": design_time,
        "failures": failures,
    }


def run_rebuild_closure_rehearsal(
    repo_root: Path,
    component_id: str,
    contexts: Sequence[str],
    jobs: int,
    samples: int,
    timeout_seconds: int,
    events: EventWriter,
    *,
    workspace_root: Path | None = None,
) -> dict[str, object]:
    if jobs < 1 or samples < 1 or timeout_seconds < 1:
        raise BuildError("REBUILD_EVIDENCE_ARGUMENT", "jobs, samples, and timeout must be at least 1", 2)
    # Keep the default below the mandated user temp root, but deliberately
    # short: MSVC's compiler probe runs before the generated project can set
    # CMAKE_OBJECT_PATH_MAX and still encounters legacy path-length limits.
    workspace_base = (workspace_root or (Path.home() / "tmp/srb")).resolve()
    if workspace_base == Path.home().resolve() or workspace_base.parent == workspace_base:
        raise BuildError("REBUILD_EVIDENCE_WORKSPACE", f"unsafe workspace root: {workspace_base}", 2)
    workspace_base.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix="r-", dir=workspace_base)
    work_root = Path(temporary.name)
    report: dict[str, object] = {
        "schema_version": 1,
        "component_id": component_id,
        "contexts": list(contexts),
        "results": [],
        "status": "running",
    }
    cleanup_error: str | None = None
    try:
        for context_index, context_id in enumerate(contexts):
            isolated = work_root / f"c{context_index}" / "r"
            _copy_minimal_repository(repo_root, isolated)
            graph = load_semantic_graph(isolated)
            generate(graph)
            stale = stale_outputs(graph)
            if stale:
                raise BuildError("REBUILD_EVIDENCE_GENERATION", f"isolated projections are stale: {stale}", 4)
            report["results"].append(
                _context_rehearsal(
                    graph,
                    component_id,
                    context_id,
                    jobs,
                    samples,
                    timeout_seconds,
                    events,
                )
            )
        results = report["results"]
        assert isinstance(results, list)
        report["ok"] = all(result["ok"] for result in results)
        report["status"] = "success" if report["ok"] else "failed"
    except (BuildError, OSError, ValueError) as error:
        report["ok"] = False
        report["status"] = "failed"
        report["failure_code"] = getattr(error, "code", type(error).__name__)
        report["failure"] = f"{report['failure_code']}: {error}"
        report["failure_exit_code"] = error.exit_code if isinstance(error, BuildError) else 6
    finally:
        try:
            temporary.cleanup()
        except OSError as error:
            cleanup_error = str(error)
        try:
            workspace_base.rmdir()
        except OSError:
            pass
    report["workspace_cleaned"] = not work_root.exists()
    if cleanup_error is not None or not report["workspace_cleaned"]:
        report["ok"] = False
        report["status"] = "cleanup_failed"
        report["cleanup_error"] = cleanup_error or "temporary rebuild workspace remains"
    return report


def write_rebuild_evidence(path: Path, evidence: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(
            json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
