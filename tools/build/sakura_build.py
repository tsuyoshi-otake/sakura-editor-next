#!/usr/bin/env python3
"""Canonical Sakura Editor NEXT build command-line interface."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Mapping, Sequence

from sakura_build_lib.abi_fixture import ABI_FIXTURES, run_abi_fixture
from sakura_build_lib.checkout_invariance import verify_checkout_invariance
from sakura_build_lib.ci_plan import (
    CiPlanError,
    changed_files_between,
    plan_ci,
    write_ci_plan,
)
from sakura_build_lib.generator import generate, stale_component_outputs, stale_outputs
from sakura_build_lib.component_evidence import ComponentEvidenceError, collect_component_evidence, write_component_evidence
from sakura_build_lib.coverage_map import (
    ChangedFile,
    CoverageMapError,
    build_coverage_map,
    coverage_cache_key,
    load_coverage_map,
    load_module_index,
    merge_coverage_map_partials,
    plan_coverage_map_shard,
    select_tests,
    sha256_file,
    write_json,
    write_coverage_map,
)
from sakura_build_lib.model import ManifestError, PHASES, load_semantic_graph
from sakura_build_lib.path_matrix import run_basic_path_matrix
from sakura_build_lib.package_restore import (
    package_gc,
    plan_package_restore,
    restore_package_closure,
    validate_package_restore,
)
from sakura_build_lib.product_native_evidence import (
    observe_product_native_evidence,
    write_product_native_evidence,
)
from sakura_build_lib.rebuild_evidence import run_rebuild_closure_rehearsal, write_rebuild_evidence
from sakura_build_lib.resource_native_evidence import (
    collect_resource_id_baseline,
    collect_resource_native_evidence,
    write_resource_native_evidence,
)
from sakura_build_lib.resource_id_compatibility import (
    require_resource_id_baseline_version_advance,
    write_resource_id_baseline,
)
from sakura_build_lib.output_evidence_ledger import (
    DEFAULT_LEDGER_DIRECTORY,
    append_output_evidence,
    verify_output_evidence_ledger,
)
from sakura_build_lib.output_link_size_evidence import (
    DEFAULT_SIZE_THRESHOLD_PERCENT,
    OutputLinkSizeEvidenceError,
    build_output_link_size_evidence,
    validate_output_link_size_evidence,
    write_output_link_size_evidence,
)
from sakura_build_lib.repository_path_safety import RepositoryPathSafetyError, safe_repository_path
from sakura_build_lib.runtime_stage import stage_runtime_artifacts
from sakura_build_lib.repository_inventory import (
    collect_repository_inventory,
    repository_inventory_summary,
    write_repository_inventory,
)
from sakura_build_lib.repository_graduation_evidence import (
    collect_repository_graduation_evidence,
    write_repository_graduation_evidence,
)
from sakura_build_lib.semantic_inventory import (
    accept_semantic_inventory,
    collect_semantic_inventory,
    compare_semantic_inventory,
    semantic_inventory_summary,
    write_semantic_inventory,
)
from sakura_build_lib.runner import (
    BuildError,
    EventWriter,
    assembly_listings_enabled,
    cmake_component_build_dir,
    cmake_component_commands,
    cmake_component_test_commands,
    cmake_commands,
    distribution_commands,
    mingw_environment,
    msvc_environment,
    msbuild_command,
    msbuild_log_path,
    native_execution_root,
    native_selector_preflight,
    run_commands,
    solution_commands,
    write_native_path_identity,
)
from sakura_build_lib.test_inventory import (
    TestInventoryError,
    collect_gtest_inventory,
    compare_inventories,
    load_inventory,
    refresh_runtime_mappings,
    verify_runtime_mappings,
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
EXIT_RATCHET = 11
EXIT_LINT = 12

RESOURCE_SOURCE_ROLES = {
    "ja-JP": (Path("sakura_core/sakura_rc.rc"), Path("sakura_core/sakura_rc.rc2")),
    "en-US": (Path("sakura_lang/sakura_rc_en-US.rc"), Path("sakura_lang/sakura_rc_en-US.rc2")),
    "zh-CN": (Path("sakura_lang/sakura_rc_zh-CN.rc"), Path("sakura_lang/sakura_rc_zh-CN.rc2")),
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[2]


def context_id(platform: str, configuration: str) -> str:
    if platform == "x64":
        return f"msvc-x64-{configuration.lower()}"
    if platform == "MinGW":
        return f"mingw-x64-{configuration.lower()}"
    raise BuildError("PLATFORM_INVALID", f"unsupported platform: {platform}", EXIT_USAGE)


def _repository_path(repo: Path, value: Path, label: str) -> Path:
    try:
        return safe_repository_path(
            repo,
            value,
            code="EVIDENCE_PATH_ESCAPE",
            reject_parent_segments=True,
        )
    except RepositoryPathSafetyError as error:
        raise BuildError(
            "EVIDENCE_PATH_ESCAPE",
            f"{label} must be inside the repository",
            EXIT_USAGE,
        ) from error


def _role_paths(repo: Path, values: list[str], option: str) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        role, separator, raw_path = value.partition("=")
        role = role.strip()
        raw_path = raw_path.strip()
        if separator != "=" or not role or not raw_path:
            raise BuildError(
                "RESOURCE_ID_ROLE_PATH",
                f"{option} expects ROLE=PATH, got: {value}",
                EXIT_USAGE,
            )
        if role in result:
            raise BuildError(
                "RESOURCE_ID_ROLE_DUPLICATE",
                f"{option} repeats role {role}",
                EXIT_USAGE,
            )
        result[role] = _repository_path(repo, Path(raw_path), option)
    return result


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
    parser.add_argument(
        "--package-timeout-seconds",
        type=int,
        default=1800,
        help="maximum time owned by an explicit package restore (default: 1800)",
    )


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

    lint = commands.add_parser("lint")
    lint_commands = lint.add_subparsers(dest="lint_command", required=True)
    lint_commands.add_parser(
        "checkout-invariance",
        help="verify that LF and CRLF checkouts keep architecture-gate inputs equivalent",
    )

    ci = commands.add_parser("ci", help="plan fail-closed CI work for an exact event diff")
    ci_commands = ci.add_subparsers(dest="ci_command", required=True)
    ci_plan = ci_commands.add_parser("plan")
    ci_plan.add_argument("--event-name", required=True)
    ci_plan.add_argument("--repository", default="")
    ci_plan.add_argument("--head-repository", default="")
    ci_plan.add_argument("--base-sha")
    ci_plan.add_argument("--head-sha")
    ci_plan.add_argument("--output", type=Path, default=Path("build/ci-plan.json"))

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
    inventory_repository.add_argument("--graduation-evidence", type=Path, help="merge validated runtime/fixture/state/protocol graduation evidence")
    inventory_semantic = inventory_commands.add_parser(
        "semantic",
        help="collect the Editor Core semantic-debt baseline and enforce its ratchet",
    )
    inventory_semantic.add_argument(
        "--baseline",
        type=Path,
        default=Path("tools/build/baselines/editor-core-semantic.json"),
    )
    inventory_semantic.add_argument(
        "--output",
        type=Path,
        default=Path("build/evidence/r0/editor-core-semantic.json"),
    )
    semantic_action = inventory_semantic.add_mutually_exclusive_group()
    semantic_action.add_argument(
        "--accept-current",
        action="store_true",
        help="accept a clean, exact-SHA observation and write an immutable ledger record",
    )
    semantic_action.add_argument(
        "--collect-only",
        action="store_true",
        help="collect the v2 inventory without reading or changing a baseline",
    )
    inventory_semantic.add_argument(
        "--history-dir",
        type=Path,
        help="append-only baseline acceptance ledger directory (default: beside --baseline)",
    )
    inventory_semantic.add_argument(
        "--source-commit",
        help="full HEAD object ID required with --accept-current",
    )
    inventory_semantic.add_argument(
        "--reason",
        help="reviewed reason required with --accept-current",
    )
    inventory_semantic.add_argument(
        "--tracking-issue",
        type=int,
        help="positive GitHub Issue number required with --accept-current",
    )
    inventory_semantic.add_argument(
        "--strict",
        action="store_true",
        help="return a non-zero exit code for a new finding or missing touched-file reduction",
    )
    inventory_observe_product = inventory_commands.add_parser("observe-product")
    inventory_observe_product.add_argument("--context", default="msvc-x64-debug")
    inventory_observe_product.add_argument("--product", default="sakura_app")
    inventory_observe_product.add_argument("--jobs", type=int, default=os.environ.get("SAKURA_BUILD_JOBS", "1"))
    inventory_observe_product.add_argument("--timeout-seconds", type=int, default=900)
    inventory_observe_product.add_argument("--package-timeout-seconds", type=int, default=1800)
    inventory_observe_product.add_argument(
        "--rebuild",
        action="store_true",
        help="clean and rebuild the product so generator Exec tasks can be observed",
    )
    inventory_observe_product.add_argument(
        "--final-image-stage-root",
        type=Path,
        help="create a create-new immutable EXE/MAP stage and bind the native record to it",
    )
    inventory_observe_product.add_argument(
        "--final-image-backend",
        choices=("cpp", "rust"),
        help="Output provider selector recorded in --final-image-stage-root (required with that option)",
    )
    inventory_observe_product.add_argument("--output", type=Path, default=Path("build/evidence/r0/native-msbuild-product.json"))
    inventory_observe_resources = inventory_commands.add_parser("observe-resources")
    inventory_observe_resources.add_argument("--context", default="msvc-x64-debug")
    inventory_observe_resources.add_argument("--product", default="sakura_app")
    inventory_observe_resources.add_argument("--native-evidence", type=Path, required=True)
    inventory_observe_resources.add_argument("--resource-id-baseline", type=Path)
    inventory_observe_resources.add_argument(
        "--compat-image",
        action="append",
        default=[],
        metavar="ROLE=PATH",
        help="additional resource-only image for numeric-ID compatibility (repeatable)",
    )
    inventory_observe_resources.add_argument("--output", type=Path, default=Path("build/evidence/r0/native-resource-table.json"))
    inventory_observe_graduation = inventory_commands.add_parser("observe-graduation")
    inventory_observe_graduation.add_argument("--context", default="msvc-x64-debug")
    inventory_observe_graduation.add_argument("--product", default="sakura_app")
    inventory_observe_graduation.add_argument("--native-evidence", type=Path, required=True)
    inventory_observe_graduation.add_argument("--resource-evidence", type=Path, required=True)
    inventory_observe_graduation.add_argument("--test-inventory", type=Path, default=Path("src/test/test-inventory.json"))
    inventory_observe_graduation.add_argument("--semantic-baseline", type=Path, default=Path("tools/build/baselines/editor-core-semantic.json"))
    inventory_observe_graduation.add_argument("--runner", action="append", required=True, help="runner-id=executable (repeat for every stable inventory runner)")
    inventory_observe_graduation.add_argument("--timeout-seconds", type=int, default=60)
    inventory_observe_graduation.add_argument("--output", type=Path, default=Path("build/evidence/r0/repository-graduation.json"))
    inventory_snapshot_resource_ids = inventory_commands.add_parser("snapshot-resource-ids")
    inventory_snapshot_resource_ids.add_argument(
        "--header",
        type=Path,
        default=Path("src/main/resources/sakura_rc.h"),
    )
    inventory_snapshot_resource_ids.add_argument(
        "--image",
        action="append",
        required=True,
        metavar="ROLE=PATH",
        help="compiled image used to establish the compatibility contract (repeatable)",
    )
    inventory_snapshot_resource_ids.add_argument("--compatibility-version", type=int, default=1)
    inventory_snapshot_resource_ids.add_argument(
        "--accept-current",
        action="store_true",
        required=True,
        help="explicitly accept the current numeric contracts as the new golden baseline",
    )
    inventory_snapshot_resource_ids.add_argument(
        "--output",
        type=Path,
        default=Path("tools/build/baselines/sakura_resource_ids.json"),
    )

    evidence = commands.add_parser(
        "evidence",
        help="append or verify payload-free adoption evidence",
    )
    evidence_commands = evidence.add_subparsers(dest="evidence_command", required=True)
    evidence_append = evidence_commands.add_parser(
        "output-append",
        help="derive and append one immutable Output adoption attempt record",
    )
    evidence_append.add_argument(
        "--source",
        "--input",
        dest="source",
        type=Path,
        required=True,
        help="producer-generated payload-free evidence JSON",
    )
    evidence_append.add_argument(
        "--ledger-dir",
        type=Path,
        default=DEFAULT_LEDGER_DIRECTORY,
        help="append-only ledger directory (default: build/evidence/output-adoption-ledger)",
    )
    evidence_append.add_argument(
        "--backend",
        choices=("cpp", "rust", "paired"),
        help="optional assertion; the backend is always derived from source evidence",
    )
    evidence_verify = evidence_commands.add_parser(
        "output-verify",
        help="verify Output adoption ledger records, hashes, and chain",
    )
    evidence_verify.add_argument(
        "--ledger-dir",
        type=Path,
        default=DEFAULT_LEDGER_DIRECTORY,
        help="append-only ledger directory (default: build/evidence/output-adoption-ledger)",
    )
    evidence_link_size = evidence_commands.add_parser(
        "output-link-size",
        help="compare payload-free C++/Rust Output link, MAP, archive, symbol, and immutable final-image evidence",
    )
    evidence_link_size.add_argument(
        "--cpp-native-evidence",
        type=Path,
        required=True,
        help="C++ product-native evidence JSON bound to an immutable final-image stage",
    )
    evidence_link_size.add_argument(
        "--rust-native-evidence",
        type=Path,
        required=True,
        help="Rust product-native evidence JSON bound to an immutable final-image stage",
    )
    evidence_link_size.add_argument(
        "--cpp-manifest",
        type=Path,
        required=True,
        help="C++ output-startup-build-manifest JSON (provider manifest is accepted only as incomplete evidence)",
    )
    evidence_link_size.add_argument(
        "--rust-manifest",
        type=Path,
        required=True,
        help="Rust output-startup-build-manifest JSON (provider manifest is accepted only as incomplete evidence)",
    )
    evidence_link_size.add_argument(
        "--output",
        type=Path,
        default=Path("build/evidence/output-link-size.json"),
        help="payload-free comparison report path inside the repository (default: build/evidence/output-link-size.json)",
    )
    evidence_link_size.add_argument(
        "--threshold-percent",
        type=float,
        default=DEFAULT_SIZE_THRESHOLD_PERCENT,
        help="maximum Rust image-size increase (default: 5)",
    )

    package = commands.add_parser(
        "package",
        help="plan, restore, validate, or collect only the graph-declared package closure",
    )
    package_commands = package.add_subparsers(dest="package_command", required=True)
    for name in ("plan", "restore", "validate"):
        item = package_commands.add_parser(name)
        item.add_argument("component", nargs="?", default="sakura_app")
        item.add_argument("--context", default="msvc-x64-debug")
        if name == "restore":
            item.add_argument("--timeout-seconds", type=int, default=1800)
    package_gc_parser = package_commands.add_parser("gc")
    package_gc_parser.add_argument("--keep", type=int, default=3)
    package_gc_parser.add_argument("--max-bytes", type=int, default=8 * 1024 * 1024 * 1024)
    package_gc_parser.add_argument("--apply", action="store_true", help="remove the reported inactive cache entries")

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

    stage = commands.add_parser("stage")
    stage_commands = stage.add_subparsers(dest="stage_command", required=True)
    stage_runtime = stage_commands.add_parser("runtime")
    stage_runtime.add_argument("--context", required=True)
    stage_runtime.add_argument("--product", default="sakura_app")
    stage_runtime.add_argument("--timeout-seconds", type=int, default=60)

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
    inventory_verify_runtime = inventory_commands.add_parser("verify-runtime")
    inventory_verify_runtime.add_argument("inventory", type=Path)
    inventory_verify_runtime.add_argument("--runner", action="append", required=True, help="runner-id=executable")
    inventory_verify_runtime.add_argument("--timeout-seconds", type=int, default=60)
    inventory_refresh_runtime = inventory_commands.add_parser("refresh-runtime")
    inventory_refresh_runtime.add_argument("inventory", type=Path)
    inventory_refresh_runtime.add_argument("--runner", action="append", required=True, help="runner-id=executable")
    inventory_refresh_runtime.add_argument(
        "--remap",
        action="append",
        default=[],
        help="stable-test-id=runner-id::selector; required when an existing selector was renamed",
    )
    inventory_refresh_runtime.add_argument("--timeout-seconds", type=int, default=60)

    coverage_map = test_commands.add_parser(
        "coverage-map",
        help="build or consume a develop-based Cobertura coverage map",
    )
    coverage_map_commands = coverage_map.add_subparsers(dest="coverage_map_command", required=True)
    coverage_map_merge = coverage_map_commands.add_parser("merge")
    coverage_map_merge.add_argument("--base-sha", required=True)
    coverage_map_merge.add_argument("--test-binary", type=Path, required=True)
    coverage_map_merge.add_argument("--inventory", type=Path, required=True)
    coverage_map_merge.add_argument("--fragment", action="append", required=True, metavar="SELECTOR::PATH")
    coverage_map_merge.add_argument("--output", type=Path, required=True)
    coverage_map_merge.add_argument("--platform", default="x64")
    coverage_map_merge.add_argument("--configuration", choices=("Debug", "Release"), default="Debug")
    coverage_map_merge.add_argument("--runner-id", default="tests1")

    coverage_map_merge_partials = coverage_map_commands.add_parser("merge-partials")
    coverage_map_merge_partials.add_argument("--base-sha", required=True)
    coverage_map_merge_partials.add_argument("--inventory", type=Path, required=True)
    coverage_map_merge_partials.add_argument("--partial-map", action="append", required=True, type=Path)
    coverage_map_merge_partials.add_argument("--output", type=Path, required=True)

    coverage_map_plan = coverage_map_commands.add_parser("plan")
    coverage_map_plan.add_argument("--inventory", type=Path, required=True)
    coverage_map_plan.add_argument("--runner-id", default="tests1")
    coverage_map_plan.add_argument("--shard-index", type=int, required=True)
    coverage_map_plan.add_argument("--shard-count", type=int, required=True)
    coverage_map_plan.add_argument("--exclude-selector", action="append", default=[])
    coverage_map_plan.add_argument("--output", type=Path, required=True)

    coverage_map_validate = coverage_map_commands.add_parser("validate")
    coverage_map_validate.add_argument("--map", dest="coverage_map", type=Path, required=True)
    coverage_map_validate.add_argument("--inventory", type=Path, required=True)
    coverage_map_validate.add_argument("--base-sha")

    coverage_map_select = coverage_map_commands.add_parser("select")
    coverage_map_select.add_argument("--map", dest="coverage_map", type=Path)
    coverage_map_select.add_argument("--inventory", type=Path, required=True)
    coverage_map_select.add_argument("--base-sha")
    coverage_map_select.add_argument("--changed-file", action="append", default=[], metavar="[STATUS::]PATH")
    coverage_map_select.add_argument("--smoke-selector", action="append", default=[])
    coverage_map_select.add_argument("--exclude-selector", action="append", default=[])
    coverage_map_select.add_argument("--modules-json", type=Path)
    coverage_map_select.add_argument("--module-path", action="append", default=[], metavar="MODULE::PATH")
    coverage_map_select.add_argument("--threshold", type=float, default=0.65)
    coverage_map_select.add_argument("--output", type=Path)

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


def _package_result_for_output(result: dict[str, object]) -> dict[str, object]:
    """Render package plans without leaking Path objects into JSON output."""

    rendered: dict[str, object] = {}
    for key, value in result.items():
        if isinstance(value, Path):
            rendered[key] = str(value)
        elif isinstance(value, dict):
            rendered[key] = _package_result_for_output(dict(value))
        elif isinstance(value, list):
            rendered[key] = [str(item) if isinstance(item, Path) else item for item in value]
        else:
            rendered[key] = value
    return rendered


def _ensure_package_closure(
    graph,
    roots: tuple[str, ...],
    context_id_value: str,
    *,
    dry_run: bool,
    timeout_seconds: int,
    events: EventWriter,
) -> dict[str, object]:
    """Make package restoration an explicit predecessor of a native build.

    A dry run intentionally reports the semantic closure only.  It never
    creates an installed root, preserving the contract that dry-run has no
    restore side effect.
    """

    if dry_run:
        result = plan_package_restore(graph, roots, context_id_value)
        events.emit(
            "package_restore_planned",
            context_id=context_id_value,
            status="planned",
            plan_hash=result.get("plan_hash"),
            required=bool(result.get("required")),
        )
        return result
    return restore_package_closure(
        graph,
        roots,
        context_id_value,
        timeout_seconds=timeout_seconds,
        events=events,
    )


def production_package_environment(
    environment: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Return the environment contract required by production packaging.

    Production packaging uses the rollback-first C++ implementation for both
    UTF-16 and Output until their adoption gates are completed independently.
    The strict native selector preflight runs before this environment is handed
    to package restore or compilation, and the selected backends plus their
    independent production-package contracts are returned explicitly so a
    stale ambient environment cannot change the package after validation.
    """
    validated = native_selector_preflight(
        "msvc",
        environment,
        production_package=True,
    )
    return {
        "SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1",
        "SAKURA_UTF16_BACKEND": validated["SAKURA_UTF16_BACKEND"],
        "SAKURA_OUTPUT_BACKEND": validated["SAKURA_OUTPUT_BACKEND"],
        "SAKURA_UTF16_PRODUCTION_PACKAGE": "true",
        "SAKURA_OUTPUT_PRODUCTION_PACKAGE": "true",
    }


def _run_build(args, graph, events: EventWriter) -> int:
    repo = graph.repo_root
    command = args.build_command
    if command == "fixture":
        result = run_abi_fixture(repo, args.fixture, args.context, args.timeout_seconds, events)
        output(result, args.format)
        return 0 if result["ok"] else EXIT_BUILD
    if command in {"dev", "solution", "distribution"}:
        validate_legacy_pair(args.platform, args.configuration, "x64")
        env = (
            production_package_environment()
            if command == "distribution"
            else native_selector_preflight("msvc")
        )
        package_roots = ("sakura_app", "tests1") if command == "solution" else ("sakura_app",)
        _ensure_package_closure(
            graph,
            package_roots,
            context_id(args.platform, args.configuration),
            dry_run=args.dry_run,
            timeout_seconds=args.package_timeout_seconds,
            events=events,
        )
        if command == "dev":
            target = repo / "sakura_core/sakura.vcxproj"
            target_name = os.environ.get("SAKURA_DEV_BUILD_TARGET", "Build")
            commands = [msbuild_command(repo, target, args.platform, args.configuration, args.jobs, build_target=target_name)]
        elif command == "solution":
            commands = solution_commands(
                repo,
                args.platform,
                args.configuration,
                args.jobs,
                log_file=msbuild_log_path(repo, args.platform, args.configuration),
                assembly_listings=assembly_listings_enabled(os.environ),
            )
        else:
            commands = distribution_commands(repo, args.platform, args.configuration, args.jobs)
        result = run_commands(commands, repo, dry_run=args.dry_run, events=events, environment=env)
        if result or args.dry_run:
            return result
        stage_runtime_artifacts(
            graph,
            "sakura_app",
            context_id(args.platform, args.configuration),
            events=events,
        )
        return 0

    stale = stale_component_outputs(graph, args.component, args.context)
    if stale:
        raise BuildError(
            "GENERATED_MODEL_STALE",
            f"committed build projections are stale ({', '.join(stale)}); run: py -3 tools/build/sakura_build.py generate",
            EXIT_STALE,
        )
    intent = graph.build_intent((args.component,), args.context, ("generate", "compile", "link"))
    _ensure_package_closure(
        graph,
        (args.component,),
        args.context,
        dry_run=args.dry_run,
        timeout_seconds=args.package_timeout_seconds,
        events=events,
    )
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
        build_dir = cmake_component_build_dir(execution_root, args.component, args.context)
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
    _ensure_package_closure(
        graph,
        (args.component,),
        args.context,
        dry_run=args.dry_run,
        timeout_seconds=args.package_timeout_seconds,
        events=events,
    )
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
        build_dir = cmake_component_build_dir(execution_root, args.component, args.context)
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
        native_environment = native_selector_preflight("mingw")
        run_tests = len(values) < 3 or not values[2]
        package_result = _ensure_package_closure(
            graph,
            ("sakura_app", "tests1"),
            context_id(platform, configuration),
            dry_run=False,
            timeout_seconds=1800,
            events=events,
        )
        return run_commands(
            cmake_commands(
                graph.repo_root,
                configuration,
                compat_jobs,
                run_tests=run_tests,
                package_cmake_config=Path(str(package_result["active_cmake_relative"])),
                environment=native_environment,
            ),
            graph.repo_root,
            dry_run=False,
            events=events,
            environment=mingw_environment(native_environment),
        )
    validate_legacy_pair(platform, configuration, "x64")
    native_environment = (
        production_package_environment()
        if args.entrypoint == "build-all"
        else native_selector_preflight("msvc")
    )
    package_roots = ("sakura_app", "tests1") if args.entrypoint == "build-sln" else ("sakura_app",)
    _ensure_package_closure(
        graph,
        package_roots,
        context_id(platform, configuration),
        dry_run=False,
        timeout_seconds=1800,
        events=events,
    )
    if args.entrypoint == "build-dev":
        commands = [msbuild_command(graph.repo_root, graph.repo_root / "sakura_core/sakura.vcxproj", platform, configuration, compat_jobs, build_target=os.environ.get("SAKURA_DEV_BUILD_TARGET", "Build"))]
        env = native_environment
    elif args.entrypoint == "build-sln":
        # CI builds the solution here and packages it with a separate
        # ``zipArtifacts.bat`` step, which requires this log.
        commands = solution_commands(
            graph.repo_root,
            platform,
            configuration,
            compat_jobs,
            log_file=msbuild_log_path(graph.repo_root, platform, configuration),
            assembly_listings=assembly_listings_enabled(os.environ),
        )
        env = native_environment
    else:
        commands = distribution_commands(graph.repo_root, platform, configuration, compat_jobs)
        env = native_environment
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


def _run_test_coverage_map(args, repo: Path) -> int:
    def resolve(value: Path) -> Path:
        return value if value.is_absolute() else repo / value

    try:
        inventory_path = resolve(args.inventory)
        inventory = load_inventory(inventory_path)
        command = args.coverage_map_command
        if command == "merge":
            fragments: list[tuple[str, Path]] = []
            for value in args.fragment:
                selector, separator, raw_path = value.partition("::")
                if separator != "::" or not selector or not raw_path:
                    raise CoverageMapError("COVERAGE_FRAGMENT_ARGUMENT", f"expected SELECTOR::PATH: {value}")
                fragments.append((selector, resolve(Path(raw_path))))
            binary = resolve(args.test_binary)
            result = build_coverage_map(
                base_sha=args.base_sha,
                test_binary_sha256=sha256_file(binary),
                inventory=inventory,
                fragments=fragments,
                repo_root=repo,
                platform=args.platform,
                configuration=args.configuration,
                runner_id=args.runner_id,
            )
            destination = resolve(args.output)
            write_coverage_map(destination, result)
            output(
                {
                    "ok": True,
                    "output": str(destination),
                    "base_sha": result["base_sha"],
                    "cache_key": coverage_cache_key(result["base_sha"], platform=result["platform"]),
                    "source_count": len(result["source_to_tests"]),
                    "fragment_count": len(result["fragments"]),
                    "test_count": result["test_count"],
                    "inventory_guarantee_fingerprint": result["inventory_guarantee_fingerprint"],
                },
                args.format,
            )
            return 0
        if command == "validate":
            result = load_coverage_map(
                resolve(args.coverage_map),
                inventory,
                expected_base_sha=args.base_sha,
            )
            output(
                {
                    "ok": True,
                    "base_sha": result["base_sha"],
                    "source_count": len(result["source_to_tests"]),
                    "fragment_count": len(result["fragments"]),
                    "test_count": result["test_count"],
                },
                args.format,
            )
            return 0
        if command == "merge-partials":
            partials = [
                load_coverage_map(resolve(path), inventory, expected_base_sha=args.base_sha)
                for path in args.partial_map
            ]
            result = merge_coverage_map_partials(
                partial_maps=partials,
                inventory=inventory,
                expected_base_sha=args.base_sha,
            )
            destination = resolve(args.output)
            write_coverage_map(destination, result)
            output(
                {
                    "ok": True,
                    "output": str(destination),
                    "base_sha": result["base_sha"],
                    "cache_key": coverage_cache_key(result["base_sha"], platform=result["platform"]),
                    "source_count": len(result["source_to_tests"]),
                    "fragment_count": len(result["fragments"]),
                    "test_count": result["test_count"],
                    "inventory_guarantee_fingerprint": result["inventory_guarantee_fingerprint"],
                },
                args.format,
            )
            return 0
        if command == "plan":
            result = plan_coverage_map_shard(
                inventory=inventory,
                runner_id=args.runner_id,
                shard_index=args.shard_index,
                shard_count=args.shard_count,
                excluded_selectors=args.exclude_selector,
            )
            destination = resolve(args.output)
            write_json(destination, result)
            output({"ok": True, "output": str(destination), **result}, args.format)
            return 0

        coverage_value = None
        if args.coverage_map is not None:
            coverage_path = resolve(args.coverage_map)
            if coverage_path.exists():
                try:
                    coverage_value = json.loads(coverage_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    # An unreadable map is input uncertainty, not a reason to
                    # abort the job. select_tests will return full fallback.
                    coverage_value = {}

        module_index: dict[str, Sequence[str]] = {}
        if args.modules_json is not None:
            module_index.update(load_module_index(resolve(args.modules_json), repo))
        for value in args.module_path:
            module_id, separator, raw_path = value.partition("::")
            if separator != "::" or not module_id or not raw_path:
                raise CoverageMapError("MODULE_PATH_ARGUMENT", f"expected MODULE::PATH: {value}")
            existing = list(module_index.get(module_id, ()))
            existing.append(raw_path.replace("\\", "/"))
            module_index[module_id] = tuple(sorted(set(existing)))

        changed_files: list[ChangedFile] = []
        for value in args.changed_file:
            status, separator, raw_path = value.partition("::")
            if separator == "::" and status and status[0].upper() in "ACDMRTU":
                changed_files.append(ChangedFile(raw_path, status))
            else:
                changed_files.append(ChangedFile(value, "M"))
        result = select_tests(
            changed_files=changed_files,
            coverage_map=coverage_value,
            inventory=inventory,
            repo_root=repo,
            expected_base_sha=args.base_sha,
            smoke_selectors=args.smoke_selector,
            module_index=module_index,
            excluded_selectors=args.exclude_selector,
            threshold=args.threshold,
        )
        if args.output is not None:
            destination = resolve(args.output)
            write_json(destination, result)
            result = {**result, "output": str(destination)}
        output(result, args.format)
        return 0
    except (CoverageMapError, TestInventoryError) as error:
        code = getattr(error, "code", "COVERAGE_MAP_ERROR")
        raise BuildError(code, str(error), EXIT_TEST) from error


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
        if args.inventory_command in {"verify-runtime", "refresh-runtime"}:
            inventory_path = args.inventory if args.inventory.is_absolute() else repo / args.inventory
            runners: dict[str, Path] = {}
            for value in args.runner:
                runner_id, separator, executable_value = value.partition("=")
                if not separator or not runner_id or not executable_value or runner_id in runners:
                    raise TestInventoryError("TEST_RUNNER_ARGUMENT", f"expected unique runner-id=executable: {value}")
                executable = Path(executable_value)
                runners[runner_id] = executable if executable.is_absolute() else repo / executable
            if args.inventory_command == "refresh-runtime":
                remaps: dict[str, tuple[str, str]] = {}
                for value in args.remap:
                    test_id, separator, runtime_value = value.partition("=")
                    runner_id, runtime_separator, selector = runtime_value.partition("::")
                    if (
                        not separator
                        or runtime_separator != "::"
                        or not test_id
                        or not runner_id
                        or not selector
                        or test_id in remaps
                    ):
                        raise TestInventoryError(
                            "TEST_REMAP_ARGUMENT",
                            f"expected unique stable-test-id=runner-id::selector: {value}",
                        )
                    remaps[test_id] = (runner_id, selector)
                revision, source_dirty = _git_source_state(repo)
                refreshed, report = refresh_runtime_mappings(
                    load_inventory(inventory_path),
                    runners,
                    remaps,
                    repo,
                    revision,
                    source_dirty,
                    args.timeout_seconds,
                )
                report.update({
                    "ok": True,
                    "changed": write_inventory(inventory_path, refreshed),
                    "output": str(inventory_path),
                    "guarantee_fingerprint": refreshed["guarantee_fingerprint"],
                })
                output(report, args.format)
                return 0
            result = verify_runtime_mappings(
                load_inventory(inventory_path), runners, repo, args.timeout_seconds
            )
            output(result, args.format)
            return 0 if result["ok"] else EXIT_TEST
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
        if args.command == "lint":
            result = verify_checkout_invariance(repo, manifest, current_graph=graph)
            output(result, args.format)
            return 0 if result["ok"] else EXIT_LINT
        if args.command == "ci":
            try:
                changed_files = (
                    changed_files_between(repo, args.base_sha, args.head_sha)
                    if args.event_name.strip().lower() == "pull_request"
                    else ()
                )
                result = plan_ci(
                    changed_files,
                    event_name=args.event_name,
                    repository=args.repository,
                    head_repository=args.head_repository,
                    base_sha=args.base_sha,
                    head_sha=args.head_sha,
                )
                destination = _repository_path(repo, args.output, "--output")
                write_ci_plan(destination, result)
            except CiPlanError as error:
                raise BuildError(error.code, str(error), EXIT_USAGE) from error
            output({**result, "output": str(destination)}, args.format)
            return 0
        if args.command == "graph":
            if args.graph_command == "project":
                output(graph.project(args.context), args.format)
                return 0
            contexts = sorted(graph.contexts) if args.all_contexts else [args.context or sorted(graph.contexts)[0]]
            result = graph_check(graph, contexts)
            output(result, args.format)
            return 0 if result["ok"] else EXIT_GRAPH
        if args.command == "package":
            if args.package_command == "gc":
                output(package_gc(repo, keep=args.keep, max_bytes=args.max_bytes, apply=args.apply), args.format)
                return 0
            if args.package_command == "plan":
                result = plan_package_restore(graph, (args.component,), args.context)
                output(_package_result_for_output(result), args.format)
                return 0
            if args.package_command == "restore":
                result = restore_package_closure(
                    graph,
                    (args.component,),
                    args.context,
                    timeout_seconds=args.timeout_seconds,
                    events=events,
                )
                output(_package_result_for_output(result), args.format)
                return 0
            result = validate_package_restore(graph, (args.component,), args.context)
            output(_package_result_for_output(result), args.format)
            return 0 if result["valid"] else EXIT_GRAPH
        if args.command == "evidence":
            if args.evidence_command == "output-append":
                ledger_directory = _repository_path(repo, args.ledger_dir, "--ledger-dir")
                source_path = _repository_path(repo, args.source, "--source")
                result = append_output_evidence(
                    ledger_directory,
                    source_path,
                    backend=args.backend,
                )
                output(result, args.format)
                return 0
            if args.evidence_command == "output-verify":
                ledger_directory = _repository_path(repo, args.ledger_dir, "--ledger-dir")
                result = verify_output_evidence_ledger(ledger_directory)
                output(result, args.format)
                return 0 if result["ok"] else EXIT_RATCHET
            try:
                cpp_native = _repository_path(repo, args.cpp_native_evidence, "--cpp-native-evidence")
                rust_native = _repository_path(repo, args.rust_native_evidence, "--rust-native-evidence")
                cpp_manifest = _repository_path(repo, args.cpp_manifest, "--cpp-manifest")
                rust_manifest = _repository_path(repo, args.rust_manifest, "--rust-manifest")
                destination = _repository_path(repo, args.output, "--output")
                result = build_output_link_size_evidence(
                    cpp_native,
                    rust_native,
                    cpp_manifest,
                    rust_manifest,
                    repo_root=repo,
                    threshold_percent=args.threshold_percent,
                    require_immutable_stage=True,
                )
                write_output_link_size_evidence(destination, result)
                validation = validate_output_link_size_evidence(result)
                output({**validation, "output": str(destination)}, args.format)
                return 0 if validation["ok"] else EXIT_GRAPH
            except OutputLinkSizeEvidenceError as error:
                raise BuildError(error.code, str(error), error.exit_code) from error
        if args.command == "inventory":
            destination = args.output if args.output.is_absolute() else repo / args.output
            try:
                destination.resolve().relative_to(repo)
            except ValueError as error:
                raise BuildError("EVIDENCE_PATH_ESCAPE", f"inventory output must be inside the repository: {destination}", EXIT_USAGE) from error
            if args.inventory_command == "snapshot-resource-ids":
                image_paths = _role_paths(repo, args.image, "--image")
                header_path = _repository_path(repo, args.header, "--header")
                source_roles = {
                    role: tuple(repo / path for path in paths)
                    for role, paths in RESOURCE_SOURCE_ROLES.items()
                }
                result = collect_resource_id_baseline(
                    repo,
                    header_path,
                    source_roles,
                    image_paths,
                    compatibility_version=args.compatibility_version,
                )
                require_resource_id_baseline_version_advance(repo, destination, result)
                write_resource_id_baseline(destination, result)
                output(
                    {
                        "ok": True,
                        "compatibility_version": result["compatibility_version"],
                        "baseline_hash": result["baseline_hash"],
                        "header_definition_count": result["header"]["definition_count"],
                        "source_roles": [item["role"] for item in result["sources"]],
                        "image_roles": [
                            {
                                "role": item["role"],
                                "contract_hash": item["contract"]["contract_hash"],
                                "counts": item["contract"]["counts"],
                            }
                            for item in result["images"]
                        ],
                        "output": str(destination),
                    },
                    args.format,
                )
                return 0
            if args.inventory_command == "semantic":
                baseline_path = _repository_path(repo, args.baseline, "--baseline")
                accept_options_supplied = any(
                    value is not None
                    for value in (args.history_dir, args.source_commit, args.reason, args.tracking_issue)
                )
                if args.collect_only and args.strict:
                    raise BuildError("SEMANTIC_COLLECT_STRICT", "--collect-only cannot be combined with --strict", EXIT_USAGE)
                if not args.accept_current and accept_options_supplied:
                    raise BuildError(
                        "SEMANTIC_ACCEPT_ARGUMENT",
                        "--history-dir, --source-commit, --reason, and --tracking-issue require --accept-current",
                        EXIT_USAGE,
                    )
                if args.accept_current:
                    if not args.source_commit:
                        raise BuildError("SEMANTIC_ACCEPT_COMMIT", "--accept-current requires --source-commit", EXIT_USAGE)
                    if args.reason is None:
                        raise BuildError("SEMANTIC_ACCEPT_REASON", "--accept-current requires --reason", EXIT_USAGE)
                    if args.tracking_issue is None:
                        raise BuildError("SEMANTIC_ACCEPT_ISSUE", "--accept-current requires --tracking-issue", EXIT_USAGE)
                current = collect_semantic_inventory(repo)
                baseline_changed = False
                comparison = None
                if args.accept_current:
                    history_directory = (
                        _repository_path(repo, args.history_dir, "--history-dir") if args.history_dir is not None else None
                    )
                    accepted = accept_semantic_inventory(
                        repo,
                        current,
                        baseline_path,
                        history_directory=history_directory,
                        source_commit=args.source_commit,
                        accepted_reason=args.reason,
                        tracking_issue=args.tracking_issue,
                    )
                    baseline_changed = bool(accepted["baseline_changed"])
                    comparison = {
                        "ok": True,
                        "accepted_current": True,
                        "baseline_source_fingerprint": current["source_fingerprint"],
                        "current_source_fingerprint": current["source_fingerprint"],
                        "increases": [],
                        "new_findings": [],
                        "missing_touched_reductions": [],
                        "ratcheted_rules": [],
                        **accepted,
                    }
                elif not args.collect_only:
                    try:
                        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
                    except FileNotFoundError as error:
                        raise BuildError(
                            "SEMANTIC_BASELINE_MISSING",
                            f"semantic baseline is missing; rerun with --accept-current: {baseline_path}",
                            EXIT_USAGE,
                        ) from error
                    except (OSError, json.JSONDecodeError) as error:
                        raise BuildError("SEMANTIC_BASELINE_INVALID", f"could not read semantic baseline: {baseline_path}", EXIT_USAGE) from error
                    comparison = compare_semantic_inventory(current, baseline, repo_root=repo)
                write_semantic_inventory(destination, current)
                report = semantic_inventory_summary(current, comparison, output_path=destination)
                report["baseline"] = str(baseline_path)
                report["baseline_changed"] = baseline_changed
                output(report, args.format)
                return 0 if comparison is None or not args.strict or comparison["ok"] else EXIT_RATCHET
            if args.inventory_command == "observe-graduation":
                native_evidence = _repository_path(repo, args.native_evidence, "--native-evidence")
                resource_evidence = _repository_path(repo, args.resource_evidence, "--resource-evidence")
                test_inventory_path = _repository_path(repo, args.test_inventory, "--test-inventory")
                semantic_baseline_path = _repository_path(repo, args.semantic_baseline, "--semantic-baseline")
                destination = _repository_path(repo, args.output, "--output")
                runners = _role_paths(repo, args.runner, "--runner")
                result = collect_repository_graduation_evidence(
                    graph,
                    product_id=args.product,
                    context_id=args.context,
                    native_evidence_path=native_evidence,
                    resource_evidence_path=resource_evidence,
                    test_inventory_path=test_inventory_path,
                    runners=runners,
                    semantic_baseline_path=semantic_baseline_path,
                    timeout_seconds=args.timeout_seconds,
                )
                write_repository_graduation_evidence(destination, result)
                output(
                    {
                        "collection_ok": result["collection_ok"],
                        "semantic_graph_hash": result["semantic_graph_hash"],
                        "hard_evidence_hash": result["hard_evidence_hash"],
                        "coverage": result["coverage"],
                        "runtime_stage_status": result["runtime_asset"]["status"],
                        "fixture_count": len(result["test_fixture"]["fixtures"]),
                        "test_inventory_count": result["test_inventory"]["test_count"],
                        "protocol_contract_count": len(result["protocol"]["contracts"]),
                        "output": str(destination),
                    },
                    args.format,
                )
                return 0
            if args.inventory_command == "observe-product":
                if (args.final_image_stage_root is None) != (args.final_image_backend is None):
                    raise BuildError(
                        "NATIVE_PRODUCT_FINAL_IMAGE_ARGUMENT",
                        "--final-image-stage-root and --final-image-backend must be supplied together",
                        EXIT_USAGE,
                    )
                final_image_stage_root = (
                    _repository_path(repo, args.final_image_stage_root, "--final-image-stage-root")
                    if args.final_image_stage_root is not None
                    else None
                )
                package_result = _ensure_package_closure(
                    graph,
                    (args.product,),
                    args.context,
                    dry_run=False,
                    timeout_seconds=args.package_timeout_seconds,
                    events=events,
                )
                result = observe_product_native_evidence(
                    graph,
                    args.product,
                    args.context,
                    args.jobs,
                    args.timeout_seconds,
                    events,
                    build_target="Rebuild" if args.rebuild else "Build",
                    package_restore=package_result,
                    final_image_stage_root=final_image_stage_root,
                    final_image_backend=args.final_image_backend,
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
                        "package_restore_status": package_result["status"],
                        "package_restore_execution_observed": package_result["native_restore_execution_observed"],
                        "output": str(destination),
                    },
                    args.format,
                )
                return 0
            if args.inventory_command == "observe-resources":
                native_evidence = _repository_path(repo, args.native_evidence, "--native-evidence")
                resource_id_baseline = None
                compatibility_images: dict[str, Path] = {}
                if args.resource_id_baseline is not None:
                    resource_id_baseline = _repository_path(
                        repo,
                        args.resource_id_baseline,
                        "--resource-id-baseline",
                    )
                    compatibility_images = _role_paths(repo, args.compat_image, "--compat-image")
                result = collect_resource_native_evidence(
                    graph,
                    native_evidence,
                    args.product,
                    args.context,
                    resource_id_baseline_path=resource_id_baseline,
                    compatibility_images=compatibility_images,
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
            graduation_evidence = None
            if args.graduation_evidence is not None:
                graduation_evidence = args.graduation_evidence if args.graduation_evidence.is_absolute() else repo / args.graduation_evidence
                try:
                    graduation_evidence.resolve().relative_to(repo)
                except ValueError as error:
                    raise BuildError("EVIDENCE_PATH_ESCAPE", f"graduation evidence must be inside the repository: {graduation_evidence}", EXIT_USAGE) from error
            result = collect_repository_inventory(
                graph,
                product_id=args.product,
                provider_id=args.provider,
                context_id=args.context,
                native_evidence_path=native_evidence,
                resource_evidence_path=resource_evidence,
                graduation_evidence_path=graduation_evidence,
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
        if args.command == "stage":
            result = stage_runtime_artifacts(
                graph,
                args.product,
                args.context,
                timeout_seconds=args.timeout_seconds,
                events=events,
            )
            output(result, args.format)
            return 0
        if args.command == "build":
            return _run_build(args, graph, events)
        if args.command == "test":
            if args.test_command == "inventory":
                return _run_test_inventory(args, repo)
            if args.test_command == "coverage-map":
                return _run_test_coverage_map(args, repo)
            if args.test_command == "component":
                return _run_test_component(args, graph, events)
            validate_legacy_pair(args.platform, args.configuration, "MinGW")
            package_result = _ensure_package_closure(
                graph,
                ("sakura_app", "tests1"),
                context_id(args.platform, args.configuration),
                dry_run=args.dry_run,
                timeout_seconds=args.package_timeout_seconds,
                events=events,
            )
            package_config = Path(str(package_result["active_cmake_relative"])) if package_result.get("required") else None
            return run_commands(
                cmake_commands(
                    repo,
                    args.configuration,
                    args.jobs,
                    run_tests=True,
                    package_cmake_config=package_config,
                ),
                repo,
                dry_run=args.dry_run,
                events=events,
                environment=mingw_environment(),
                failure_exit_code=EXIT_TEST,
            )
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
