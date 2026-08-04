#!/usr/bin/env python3
"""Canonical Sakura Editor NEXT build command-line interface."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from sakura_build_lib.abi_fixture import ABI_FIXTURES, run_abi_fixture
from sakura_build_lib.generator import generate, stale_component_outputs, stale_outputs
from sakura_build_lib.component_evidence import ComponentEvidenceError, collect_component_evidence, write_component_evidence
from sakura_build_lib.model import ManifestError, PHASES, load_semantic_graph
from sakura_build_lib.path_matrix import run_basic_path_matrix
from sakura_build_lib.product_native_evidence import (
    observe_product_native_evidence,
    write_product_native_evidence,
)
from sakura_build_lib.rebuild_evidence import run_rebuild_closure_rehearsal, write_rebuild_evidence
from sakura_build_lib.resource_native_evidence import (
    collect_resource_native_evidence,
    write_resource_native_evidence,
)
from sakura_build_lib.repository_inventory import (
    collect_repository_inventory,
    repository_inventory_summary,
    write_repository_inventory,
)
from sakura_build_lib.runner import (
    BuildError,
    EventWriter,
    cmake_component_commands,
    cmake_component_test_commands,
    cmake_commands,
    distribution_commands,
    mingw_environment,
    msvc_environment,
    msbuild_command,
    native_execution_root,
    run_commands,
    write_native_path_identity,
)
from sakura_build_lib.test_inventory import (
    TestInventoryError,
    collect_gtest_inventory,
    compare_inventories,
    load_inventory,
    write_inventory,
)

EXIT_USAGE = 2
EXIT_TOOL = 3
EXIT_STALE = 4
EXIT_GRAPH = 5
EXIT_BUILD = 6
EXIT_TEST = 7
EXIT_TIMEOUT = 8
EXIT_CLEANUP = 9
EXIT_PERFORMANCE = 10


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def context_id(platform: str, configuration: str) -> str:
    if platform == "x64":
        return f"msvc-x64-{configuration.lower()}"
    if platform == "MinGW":
        return f"mingw-x64-{configuration.lower()}"
    raise BuildError("PLATFORM_INVALID", f"unsupported platform: {platform}", EXIT_USAGE)


def validate_legacy_pair(platform: str, configuration: str, expected: str | None = None) -> None:
    if expected and platform != expected:
        raise BuildError("PLATFORM_INVALID", f"expected platform {expected}, got {platform}", EXIT_USAGE)
    if platform not in {"x64", "MinGW"}:
        raise BuildError("PLATFORM_INVALID", f"unsupported platform: {platform}", EXIT_USAGE)
    if configuration not in {"Debug", "Release"}:
        raise BuildError("CONFIGURATION_INVALID", f"unsupported configuration: {configuration}", EXIT_USAGE)


def output(value: object, output_format: str) -> None:
    if output_format == "json":
        print(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2))
    elif isinstance(value, str):
        print(value)
    else:
        print(json.dumps(value, ensure_ascii=False, sort_keys=True))


def add_execution_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--jobs", type=int, default=os.environ.get("SAKURA_BUILD_JOBS", "1"), help="global native scheduler budget (default: SAKURA_BUILD_JOBS or 1)")
    parser.add_argument("--dry-run", action="store_true")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="sakura-build", description=__doc__)
    root.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    root.add_argument("--manifest", type=Path)
    root.add_argument("--format", choices=("text", "json"), default="text")
    root.add_argument("--log-jsonl", type=Path)
    commands = root.add_subparsers(dest="command", required=True)

    commands.add_parser("doctor")

    manifest = commands.add_parser("manifest")
    manifest_commands = manifest.add_subparsers(dest="manifest_command", required=True)
    manifest_commands.add_parser("check")
    migrate = manifest_commands.add_parser("migrate")
    migrate.add_argument("--from", dest="from_version", type=int, required=True)
    migrate.add_argument("--to", dest="to_version", type=int, required=True)

    generate_parser = commands.add_parser("generate")
    generate_parser.add_argument("--check", action="store_true")

    graph = commands.add_parser("graph")
    graph_commands = graph.add_subparsers(dest="graph_command", required=True)
    graph_check = graph_commands.add_parser("check")
    graph_check.add_argument("--context")
    graph_check.add_argument("--all-contexts", action="store_true")
    graph_project = graph_commands.add_parser("project")
    graph_project.add_argument("--context", required=True)

    inventory = commands.add_parser("inventory")
    inventory_commands = inventory.add_subparsers(dest="inventory_command", required=True)
    inventory_repository = inventory_commands.add_parser("repository")
    inventory_repository.add_argument("--context", default="msvc-x64-debug")
    inventory_repository.add_argument("--product", default="sakura_app")
    inventory_repository.add_argument("--provider", default="sakura_uri")
    inventory_repository.add_argument("--strict", action="store_true", help="fail when blockers or incomplete dependency classes remain")
    inventory_repository.add_argument("--output", type=Path, default=Path("build/evidence/r0/repository-inventory.json"))
    inventory_repository.add_argument("--native-evidence", type=Path, help="merge a separately collected native product observation")
    inventory_repository.add_argument("--resource-evidence", type=Path, help="merge a separately collected native PE resource-table observation")
    inventory_observe_product = inventory_commands.add_parser("observe-product")
    inventory_observe_product.add_argument("--context", default="msvc-x64-debug")
    inventory_observe_product.add_argument("--product", default="sakura_app")
    inventory_observe_product.add_argument("--jobs", type=int, default=os.environ.get("SAKURA_BUILD_JOBS", "1"))
    inventory_observe_product.add_argument("--timeout-seconds", type=int, default=900)
    inventory_observe_product.add_argument(
        "--rebuild",
        action="store_true",
        help="clean and rebuild the product so generator Exec tasks can be observed",
    )
    inventory_observe_product.add_argument("--output", type=Path, default=Path("build/evidence/r0/native-msbuild-product.json"))
    inventory_observe_resources = inventory_commands.add_parser("observe-resources")
    inventory_observe_resources.add_argument("--context", default="msvc-x64-debug")
    inventory_observe_resources.add_argument("--product", default="sakura_app")
    inventory_observe_resources.add_argument("--native-evidence", type=Path, required=True)
    inventory_observe_resources.add_argument("--output", type=Path, default=Path("build/evidence/r0/native-resource-table.json"))

    verify = commands.add_parser("verify")
    verify_commands = verify.add_subparsers(dest="verify_command", required=True)
    verify_component = verify_commands.add_parser("component-boundary")
    verify_component.add_argument("component")
    verify_component.add_argument("--context", required=True)
    verify_component.add_argument("--output", type=Path)
    verify_rebuild = verify_commands.add_parser("rebuild-closure")
    verify_rebuild.add_argument("component")
    verify_rebuild.add_argument(
        "--contexts",
        default="msvc-x64-debug,cmake-msvc-x64-debug",
        help="comma-separated component contexts",
    )
    verify_rebuild.add_argument("--jobs", type=int, default=1)
    verify_rebuild.add_argument("--samples", type=int, default=5)
    verify_rebuild.add_argument("--timeout-seconds", type=int, default=300)
    verify_rebuild.add_argument("--workspace-root", type=Path)
    verify_rebuild.add_argument("--output", type=Path, default=Path("build/evidence/r1a/rebuild-closure/evidence.json"))

    plan = commands.add_parser("plan")
    plan_commands = plan.add_subparsers(dest="plan_command", required=True)
    plan_component = plan_commands.add_parser("component")
    plan_component.add_argument("component")
    plan_component.add_argument("--context", required=True)
    plan_component.add_argument("--phases", default="generate,compile,link")

    build = commands.add_parser("build")
    build_commands = build.add_subparsers(dest="build_command", required=True)
    for name in ("dev", "solution", "distribution"):
        item = build_commands.add_parser(name)
        item.add_argument("platform", choices=("x64",))
        item.add_argument("configuration", choices=("Debug", "Release"))
        add_execution_options(item)
    build_component = build_commands.add_parser("component")
    build_component.add_argument("component")
    build_component.add_argument("--context", required=True)
    add_execution_options(build_component)
    build_fixture = build_commands.add_parser("fixture")
    build_fixture.add_argument("fixture", choices=ABI_FIXTURES)
    build_fixture.add_argument("--context", default="msvc-x64-debug")
    build_fixture.add_argument("--timeout-seconds", type=int, default=60)

    test = commands.add_parser("test")
    test_commands = test.add_subparsers(dest="test_command", required=True)
    test_all = test_commands.add_parser("all")
    test_all.add_argument("platform", choices=("MinGW",))
    test_all.add_argument("configuration", choices=("Debug", "Release"))
    add_execution_options(test_all)
    test_component = test_commands.add_parser("component")
    test_component.add_argument("component")
    test_component.add_argument("--context", required=True)
    add_execution_options(test_component)
    test_inventory = test_commands.add_parser("inventory")
    inventory_commands = test_inventory.add_subparsers(dest="inventory_command", required=True)
    inventory_collect = inventory_commands.add_parser("collect")
    inventory_collect.add_argument("--runner-id", default="tests1")
    inventory_collect.add_argument("--executable", type=Path, required=True)
    inventory_collect.add_argument("--output", type=Path, required=True)
    inventory_collect.add_argument("--source-revision")
    inventory_collect.add_argument("--timeout-seconds", type=int, default=60)
    inventory_compare = inventory_commands.add_parser("compare")
    inventory_compare.add_argument("before", type=Path)
    inventory_compare.add_argument("after", type=Path)

    path_matrix = commands.add_parser("path-matrix")
    path_matrix_commands = path_matrix.add_subparsers(dest="path_matrix_command", required=True)
    path_matrix_test = path_matrix_commands.add_parser("test")
    path_matrix_test_commands = path_matrix_test.add_subparsers(dest="path_matrix_test_command", required=True)
    path_matrix_component = path_matrix_test_commands.add_parser("component")
    path_matrix_component.add_argument("component")
    path_matrix_component.add_argument(
        "--contexts",
        default="msvc-x64-debug,cmake-msvc-x64-debug",
        help="comma-separated component contexts",
    )
    path_matrix_component.add_argument("--jobs", type=int, default=1)
    path_matrix_component.add_argument("--timeout-seconds", type=int, default=300)
    path_matrix_component.add_argument("--workspace-root", type=Path)

    compat = commands.add_parser("compat", help="legacy batch shim entry point")
    compat.add_argument("entrypoint", choices=("build-dev", "build-sln", "build-all", "build-gnu"))
    compat.add_argument("legacy_args", nargs=argparse.REMAINDER)
    return root


def graph_check(graph, contexts: list[str]) -> dict[str, object]:
    failures: list[dict[str, object]] = []
    for current in contexts:
        for phase in sorted(PHASES):
            for component_set in graph.strongly_connected_components(current, phase):
                failures.append({"code": "GRAPH_CYCLE", "context": current, "phase": phase, "components": component_set})
        for mismatch in graph.abi_profile_mismatches(current):
            failures.append({"code": "ABI_PROFILE_MISMATCH", "context": current, **mismatch})
        for edge in graph.active_edges(current):
            if edge.required and not any(witness["context"] == current for witness in edge.witnesses):
                failures.append({"code": "WITNESS_MISSING", "context": current, "edge": edge.id})
    return {"ok": not failures, "contexts": contexts, "failures": failures}


def _event_writer(path: Path | None):
    if path is None:
        return None, EventWriter()
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = path.open("a", encoding="utf-8", newline="\n")
    return stream, EventWriter(stream)


def _run_build(args, graph, events: EventWriter) -> int:
    repo = graph.repo_root
    command = args.build_command
    if command == "fixture":
        result = run_abi_fixture(repo, args.fixture, args.context, args.timeout_seconds, events)
        output(result, args.format)
        return 0 if result["ok"] else EXIT_BUILD
    if command in {"dev", "solution", "distribution"}:
        validate_legacy_pair(args.platform, args.configuration, "x64")
        if command == "dev":
            target = repo / "sakura_core/sakura.vcxproj"
            target_name = os.environ.get("SAKURA_DEV_BUILD_TARGET", "Build")
            commands = [msbuild_command(repo, target, args.platform, args.configuration, args.jobs, build_target=target_name)]
        elif command == "solution":
            commands = [msbuild_command(repo, repo / "sakura.sln", args.platform, args.configuration, args.jobs)]
        else:
            commands = distribution_commands(repo, args.platform, args.configuration, args.jobs)
        env = {"SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1"} if command == "distribution" else {}
        return run_commands(commands, repo, dry_run=args.dry_run, events=events, environment=env)

    stale = stale_component_outputs(graph, args.component, args.context)
    if stale:
        raise BuildError(
            "GENERATED_MODEL_STALE",
            f"committed build projections are stale ({', '.join(stale)}); run: py -3 tools/build/sakura_build.py generate",
            EXIT_STALE,
        )
    intent = graph.build_intent((args.component,), args.context, ("generate", "compile", "link"))
    backend = graph.context(args.context).backend
    targets = intent["backend_targets"].get(backend, [])
    if len(targets) != 1:
        raise BuildError("COMPONENT_TARGET_AMBIGUOUS", f"component closure has {len(targets)} {backend} targets; R1a supports exactly one", EXIT_USAGE)
    context = graph.context(args.context)
    if backend == "msbuild":
        commands = [msbuild_command(repo, repo / targets[0], context.platform, context.configuration, args.jobs)]
        return run_commands(commands, repo, dry_run=args.dry_run, events=events)
    if args.dry_run:
        execution_root = repo
        commands = cmake_component_commands(execution_root, args.component, args.context, context.configuration, context.toolchain, args.jobs)
        environment = mingw_environment() if context.toolchain == "mingw" else None
        return run_commands(commands, execution_root, dry_run=True, events=events, environment=environment, isolate_cmake_environment=True)
    with native_execution_root(repo, events) as execution_root:
        commands = cmake_component_commands(execution_root, args.component, args.context, context.configuration, context.toolchain, args.jobs)
        environment = mingw_environment() if context.toolchain == "mingw" else (msvc_environment(execution_root) if context.toolchain == "msvc" else None)
        result = run_commands(
            commands,
            execution_root,
            dry_run=False,
            events=events,
            environment=environment,
            isolate_cmake_environment=True,
        )
        if result:
            return result
        build_dir = execution_root / f"build/components/{args.context}/{args.component}/cmake-ninja-isolated"
        write_native_path_identity(build_dir, repo, execution_root)
        return 0


def _run_test_component(args, graph, events: EventWriter) -> int:
    component = graph.components.get(args.component)
    if component is None:
        raise BuildError("COMPONENT_UNKNOWN", f"unknown component: {args.component}", EXIT_USAGE)
    if component.kind != "test":
        raise BuildError("COMPONENT_NOT_TEST", f"component is not a test target: {args.component}", EXIT_USAGE)
    stale = stale_component_outputs(graph, args.component, args.context)
    if stale:
        raise BuildError(
            "GENERATED_MODEL_STALE",
            f"committed build projections are stale ({', '.join(stale)}); run: py -3 tools/build/sakura_build.py generate",
            EXIT_STALE,
        )
    intent = graph.build_intent((args.component,), args.context, ("generate", "compile", "link", "test"))
    context = graph.context(args.context)
    targets = intent["backend_targets"].get(context.backend, [])
    if len(targets) != 1:
        raise BuildError("COMPONENT_TARGET_AMBIGUOUS", f"test component has {len(targets)} {context.backend} targets; exactly one is required", EXIT_USAGE)
    if context.backend == "msbuild":
        build_commands = [msbuild_command(graph.repo_root, graph.repo_root / targets[0], context.platform, context.configuration, args.jobs)]
        executable = graph.repo_root / f"build/components/{args.context}/{args.component}/bin/{args.component}.exe"
        test_commands = [[str(executable)]]
        result = run_commands(build_commands, graph.repo_root, dry_run=args.dry_run, events=events, failure_exit_code=EXIT_BUILD)
        if result:
            return result
        return run_commands(test_commands, graph.repo_root, dry_run=args.dry_run, events=events, failure_exit_code=EXIT_TEST)

    if args.dry_run:
        execution_root = graph.repo_root
        build_commands = cmake_component_commands(execution_root, args.component, args.context, context.configuration, context.toolchain, args.jobs)
        test_commands = cmake_component_test_commands(execution_root, args.component, args.context, context.configuration, args.jobs)
        environment = mingw_environment() if context.toolchain == "mingw" else None
        result = run_commands(build_commands, execution_root, dry_run=True, events=events, environment=environment, failure_exit_code=EXIT_BUILD, isolate_cmake_environment=True)
        if result:
            return result
        return run_commands(test_commands, execution_root, dry_run=True, events=events, environment=environment, failure_exit_code=EXIT_TEST, isolate_cmake_environment=True)

    with native_execution_root(graph.repo_root, events) as execution_root:
        build_commands = cmake_component_commands(execution_root, args.component, args.context, context.configuration, context.toolchain, args.jobs)
        test_commands = cmake_component_test_commands(execution_root, args.component, args.context, context.configuration, args.jobs)
        environment = mingw_environment() if context.toolchain == "mingw" else (msvc_environment(execution_root) if context.toolchain == "msvc" else None)
        result = run_commands(build_commands, execution_root, dry_run=False, events=events, environment=environment, failure_exit_code=EXIT_BUILD, isolate_cmake_environment=True)
        if result:
            return result
        build_dir = execution_root / f"build/components/{args.context}/{args.component}/cmake-ninja-isolated"
        write_native_path_identity(build_dir, graph.repo_root, execution_root)
        return run_commands(test_commands, execution_root, dry_run=False, events=events, environment=environment, failure_exit_code=EXIT_TEST, isolate_cmake_environment=True)


def _run_compat(args, graph, events: EventWriter) -> int:
    values = args.legacy_args
    if len(values) < 2:
        raise BuildError("LEGACY_ARGUMENTS", f"{args.entrypoint} requires platform and configuration", EXIT_USAGE)
    platform, configuration = values[0:2]
    try:
        compat_jobs = int(os.environ.get("SAKURA_BUILD_JOBS", str(os.cpu_count() or 1)))
    except ValueError as error:
        raise BuildError("JOBS_INVALID", "SAKURA_BUILD_JOBS must be an integer", EXIT_USAGE) from error
    if args.entrypoint == "build-gnu":
        validate_legacy_pair(platform, configuration, "MinGW")
        run_tests = len(values) < 3 or not values[2]
        return run_commands(cmake_commands(graph.repo_root, configuration, compat_jobs, run_tests=run_tests), graph.repo_root, dry_run=False, events=events, environment=mingw_environment())
    validate_legacy_pair(platform, configuration, "x64")
    if args.entrypoint == "build-dev":
        commands = [msbuild_command(graph.repo_root, graph.repo_root / "sakura_core/sakura.vcxproj", platform, configuration, compat_jobs, build_target=os.environ.get("SAKURA_DEV_BUILD_TARGET", "Build"))]
        env = {}
    elif args.entrypoint == "build-sln":
        commands = [msbuild_command(graph.repo_root, graph.repo_root / "sakura.sln", platform, configuration, compat_jobs)]
        env = {}
    else:
        commands = distribution_commands(graph.repo_root, platform, configuration, compat_jobs)
        env = {"SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1"}
    return run_commands(commands, graph.repo_root, dry_run=False, events=events, environment=env)


def _normalize_global_options(argv: list[str]) -> list[str]:
    """Allow common value options before or after subcommands."""
    global_names = {"--repo-root", "--manifest", "--format", "--log-jsonl"}
    prefix: list[str] = []
    remainder: list[str] = []
    index = 0
    while index < len(argv):
        item = argv[index]
        if item in global_names:
            if index + 1 >= len(argv):
                remainder.append(item)
                index += 1
                continue
            prefix.extend((item, argv[index + 1]))
            index += 2
        else:
            remainder.append(item)
            index += 1
    return prefix + remainder


def _git_source_state(repo: Path) -> tuple[str, bool]:
    import subprocess

    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        raise BuildError("SOURCE_REVISION_UNAVAILABLE", "git rev-parse HEAD failed", EXIT_TOOL)
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=repo,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if status.returncode != 0:
        raise BuildError("SOURCE_STATUS_UNAVAILABLE", "git status --porcelain failed", EXIT_TOOL)
    return completed.stdout.strip(), bool(status.stdout.strip())


def _run_test_inventory(args, repo: Path) -> int:
    try:
        if args.inventory_command == "collect":
            executable = args.executable if args.executable.is_absolute() else repo / args.executable
            destination = args.output if args.output.is_absolute() else repo / args.output
            detected_revision, source_dirty = _git_source_state(repo)
            revision = args.source_revision or detected_revision
            inventory = collect_gtest_inventory(executable, args.runner_id, repo, revision, source_dirty, args.timeout_seconds)
            changed = write_inventory(destination, inventory)
            output(
                {
                    "ok": True,
                    "changed": changed,
                    "output": str(destination),
                    "test_count": inventory["test_count"],
                    "disabled_count": inventory["disabled_count"],
                    "guarantee_fingerprint": inventory["guarantee_fingerprint"],
                },
                args.format,
            )
            return 0
        before_path = args.before if args.before.is_absolute() else repo / args.before
        after_path = args.after if args.after.is_absolute() else repo / args.after
        result = compare_inventories(load_inventory(before_path), load_inventory(after_path))
        output(result, args.format)
        return 0 if result["ok"] else EXIT_TEST
    except TestInventoryError as error:
        raise BuildError(error.code, str(error), EXIT_TEST) from error


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = parser().parse_args(_normalize_global_options(raw_argv))
    repo = args.repo_root.resolve()
    manifest = args.manifest.resolve() if args.manifest else None
    stream = None
    try:
        stream, events = _event_writer(args.log_jsonl)
        graph = load_semantic_graph(repo, manifest)
        events.emit("manifest_loaded", graph_hash=graph.semantic_graph_hash)
        if args.command == "doctor":
            result = {"ok": True, "python": sys.version.split()[0], "repo_root": str(repo), "manifest": str(graph.manifest_path), "graph_hash": graph.semantic_graph_hash}
            output(result, args.format)
            return 0
        if args.command == "manifest":
            if args.manifest_command == "migrate":
                raise BuildError(
                    "MANIFEST_MIGRATION_UNAVAILABLE",
                    f"no registered migration from schema {args.from_version} to {args.to_version}; the manifest was not modified",
                    EXIT_USAGE,
                )
            output({"ok": True, "schema_version": graph.schema_version, "graph_hash": graph.semantic_graph_hash}, args.format)
            return 0
        if args.command == "generate":
            stale = stale_outputs(graph)
            if args.check:
                output({"ok": not stale, "stale": stale}, args.format)
                return 0 if not stale else EXIT_STALE
            changed = generate(graph)
            output({"ok": True, "changed": changed}, args.format)
            return 0
        if args.command == "graph":
            if args.graph_command == "project":
                output(graph.project(args.context), args.format)
                return 0
            contexts = sorted(graph.contexts) if args.all_contexts else [args.context or sorted(graph.contexts)[0]]
            result = graph_check(graph, contexts)
            output(result, args.format)
            return 0 if result["ok"] else EXIT_GRAPH
        if args.command == "inventory":
            destination = args.output if args.output.is_absolute() else repo / args.output
            try:
                destination.resolve().relative_to(repo)
            except ValueError as error:
                raise BuildError("EVIDENCE_PATH_ESCAPE", f"inventory output must be inside the repository: {destination}", EXIT_USAGE) from error
            if args.inventory_command == "observe-product":
                result = observe_product_native_evidence(
                    graph,
                    args.product,
                    args.context,
                    args.jobs,
                    args.timeout_seconds,
                    events,
                    build_target="Rebuild" if args.rebuild else "Build",
                )
                write_product_native_evidence(destination, result)
                compiler = result["compiler"]
                resource = result["resource_compiler"]
                link = result["link"]
                generator_observation = result["generator_observation"]
                output(
                    {
                        "collection_ok": result["collection_ok"],
                        "semantic_graph_hash": result["semantic_graph_hash"],
                        "hard_evidence_hash": result["hard_evidence_hash"],
                        "context_id": result["context_id"],
                        "product_id": result["product_id"],
                        "translation_unit_count": compiler["translation_unit_count"],
                        "compiler_source_input_count": len(compiler["source_inputs"]),
                        "generated_header_input_count": len(compiler["generated_header_inputs"]),
                        "pch_mode_counts": compiler["pch_mode_counts"],
                        "resource_unit_count": resource["unit_count"],
                        "link_object_count": len(link["object_inputs"]),
                        "link_resource_count": len(link["resource_inputs"]),
                        "selected_archive_members_observed": link["selected_archive_members_observed"],
                        "build_target": result["build_target"],
                        "generator_target_scheduling_observed": generator_observation["target_scheduling_observed"],
                        "generator_execution_observed": generator_observation["correlated_execution_observed"],
                        "generated_producer_consumer_correlation_count": len(generator_observation["producer_consumer_correlations"]),
                        "output": str(destination),
                    },
                    args.format,
                )
                return 0
            if args.inventory_command == "observe-resources":
                native_evidence = args.native_evidence if args.native_evidence.is_absolute() else repo / args.native_evidence
                try:
                    native_evidence.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"native evidence must be inside the repository: {native_evidence}", EXIT_USAGE) from error
                result = collect_resource_native_evidence(
                    graph,
                    native_evidence,
                    args.product,
                    args.context,
                )
                write_resource_native_evidence(destination, result)
                resource_table = result["resource_table"]
                output(
                    {
                        "collection_ok": result["collection_ok"],
                        "semantic_graph_hash": result["semantic_graph_hash"],
                        "hard_evidence_hash": result["hard_evidence_hash"],
                        "context_id": result["context_id"],
                        "product_id": result["product_id"],
                        "resource_table_entry_count": resource_table["entry_count"],
                        "resource_table_distinct_type_count": resource_table["distinct_type_count"],
                        "resource_table_language_ids": resource_table["language_ids"],
                        "resource_table_total_bytes": resource_table["total_bytes"],
                        "resource_table_hash": resource_table["table_hash"],
                        "resource_id_compatibility_observed": result["resource_id_compatibility"]["observed"],
                        "output": str(destination),
                    },
                    args.format,
                )
                return 0
            native_evidence = None
            if args.native_evidence is not None:
                native_evidence = args.native_evidence if args.native_evidence.is_absolute() else repo / args.native_evidence
                try:
                    native_evidence.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"native evidence must be inside the repository: {native_evidence}", EXIT_USAGE) from error
            resource_evidence = None
            if args.resource_evidence is not None:
                resource_evidence = args.resource_evidence if args.resource_evidence.is_absolute() else repo / args.resource_evidence
                try:
                    resource_evidence.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"resource evidence must be inside the repository: {resource_evidence}", EXIT_USAGE) from error
            result = collect_repository_inventory(
                graph,
                product_id=args.product,
                provider_id=args.provider,
                context_id=args.context,
                native_evidence_path=native_evidence,
                resource_evidence_path=resource_evidence,
            )
            write_repository_inventory(destination, result)
            rendered = repository_inventory_summary(result, output_path=destination)
            output(rendered, args.format)
            return 0 if not args.strict or result["graduation_ready"] else EXIT_GRAPH
        if args.command == "verify":
            if args.verify_command == "rebuild-closure":
                contexts = tuple(value for value in args.contexts.split(",") if value)
                if not contexts:
                    raise BuildError("REBUILD_CONTEXTS_EMPTY", "--contexts must name at least one context", EXIT_USAGE)
                destination = args.output if args.output.is_absolute() else repo / args.output
                try:
                    destination.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"evidence output must be inside the repository: {destination}", EXIT_USAGE) from error
                result = run_rebuild_closure_rehearsal(
                    repo,
                    args.component,
                    contexts,
                    args.jobs,
                    args.samples,
                    args.timeout_seconds,
                    events,
                    workspace_root=args.workspace_root,
                )
                write_rebuild_evidence(destination, result)
                result = {**result, "output": str(destination)}
                output(result, args.format)
                if result.get("status") == "cleanup_failed":
                    return EXIT_CLEANUP
                if result.get("ok"):
                    return 0
                return int(result.get("failure_exit_code", EXIT_PERFORMANCE))
            try:
                result = collect_component_evidence(graph, args.component, args.context)
            except ComponentEvidenceError as error:
                raise BuildError("COMPONENT_EVIDENCE_UNAVAILABLE", str(error), EXIT_GRAPH) from error
            if args.output:
                destination = args.output if args.output.is_absolute() else repo / args.output
                try:
                    destination.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"evidence output must be inside the repository: {destination}", EXIT_USAGE) from error
                write_component_evidence(destination, result)
                result = {**result, "output": str(destination)}
            output(result, args.format)
            return 0 if result["ok"] else EXIT_GRAPH
        if args.command == "plan":
            phases = tuple(item for item in args.phases.split(",") if item)
            output(graph.build_intent((args.component,), args.context, phases), args.format)
            return 0
        if args.command == "build":
            return _run_build(args, graph, events)
        if args.command == "test":
            if args.test_command == "inventory":
                return _run_test_inventory(args, repo)
            if args.test_command == "component":
                return _run_test_component(args, graph, events)
            validate_legacy_pair(args.platform, args.configuration, "MinGW")
            return run_commands(cmake_commands(repo, args.configuration, args.jobs, run_tests=True), repo, dry_run=args.dry_run, events=events, environment=mingw_environment(), failure_exit_code=EXIT_TEST)
        if args.command == "path-matrix":
            contexts = tuple(value for value in args.contexts.split(",") if value)
            if not contexts:
                raise BuildError("PATH_MATRIX_CONTEXTS_EMPTY", "--contexts must name at least one context", EXIT_USAGE)
            if args.jobs < 1:
                raise BuildError("JOBS_INVALID", "--jobs must be at least 1", EXIT_USAGE)
            if args.timeout_seconds < 1:
                raise BuildError("TIMEOUT_INVALID", "--timeout-seconds must be at least 1", EXIT_USAGE)
            result = run_basic_path_matrix(
                repo,
                args.component,
                contexts,
                args.jobs,
                timeout_seconds=args.timeout_seconds,
                workspace_root=args.workspace_root,
            )
            output(result, args.format)
            if result.get("status") == "cleanup_failed":
                return EXIT_CLEANUP
            return 0 if result.get("ok") else EXIT_BUILD
        if args.command == "compat":
            return _run_compat(args, graph, events)
        raise AssertionError(args.command)
    except ManifestError as error:
        print(str(error), file=sys.stderr)
        return EXIT_USAGE
    except BuildError as error:
        print(f"{error.code}: {error}", file=sys.stderr)
        return error.exit_code
    finally:
        if stream is not None:
            stream.close()


if __name__ == "__main__":
    if os.name == "nt":
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        if hasattr(sys.stderr, "reconfigure"):
            sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    raise SystemExit(main())
