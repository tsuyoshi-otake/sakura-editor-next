from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = TOOLS_BUILD.parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from sakura_build_lib.product_native_evidence import (  # noqa: E402
    collect_product_native_evidence,
    product_native_evidence_hash,
    product_native_evidence_source_hash,
    validate_output_provider_evidence_for_final_image,
    validate_product_native_evidence,
    write_product_native_evidence,
)
from sakura_build_lib.output_final_image_evidence import (  # noqa: E402
    bind_native_evidence_to_final_image,
    stage_output_final_image,
)
from sakura_build_lib.repository_inventory import collect_repository_inventory  # noqa: E402
from sakura_build_lib.runner import BuildError  # noqa: E402
from tools.build.tests.test_repository_inventory import _fixture, _write  # noqa: E402


EXPECTED_OUTPUT_PROVIDER_SYMBOLS = [
    "sakura_output_provider_active_channel_v1",
    "sakura_output_provider_apply_v1",
    "sakura_output_provider_create_v1",
    "sakura_output_provider_destroy_v1",
    "sakura_output_provider_snapshot_measure_v1",
    "sakura_output_provider_snapshot_write_v1",
    "sakura_output_provider_stop_v1",
]


def _write_tlog(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-16", newline="\n")


def _native_fixture(root: Path):
    graph = _fixture(root)
    generated = root / "build/x64/version.h"
    _write(generated, "#define VERSION 1\n")
    generated_manifest = root / "build/x64/app.exe.manifest"
    _write(generated_manifest, "<assembly />\n")
    output_dir = root / "build/x64/Debug/app"
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "main.obj").write_bytes(b"main-object")
    (output_dir / "provider.obj").write_bytes(b"provider-object")
    (output_dir / "app.res").write_bytes(b"resource")
    (output_dir / "app.pch").write_bytes(b"pch")
    executable = root / "x64/Debug/app.exe"
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.write_bytes(b"product")

    main = root / "app/main.cpp"
    provider = root / "provider/provider.cpp"
    provider_header = root / "provider/provider.h"
    shared_header = root / "shared/unowned.h"
    resource = root / "app/app.rc"
    resource_header = root / "app/sakura_rc.h"
    tlog = output_dir / "app.tlog"
    _write_tlog(
        tlog / "CL.command.1.tlog",
        [
            f"^{main}",
            '/c /fp:precise /Yu"provider/provider.h" /Fp"build\\x64\\Debug\\app\\app.pch" /Fo"build\\x64\\Debug\\app\\"',
            f"^{provider}",
            '/c /Fo"build\\x64\\Debug\\app\\"',
        ],
    )
    _write_tlog(
        tlog / "CL.read.1.tlog",
        [
            f"^{main}",
            str(provider_header),
            str(shared_header),
            str(generated),
            r"C:\SDK\include\windows.h",
            f"^{provider}",
            str(provider_header),
        ],
    )
    _write_tlog(
        tlog / "link.command.1.tlog",
        [
            f"^{output_dir / 'main.obj'}|{output_dir / 'provider.obj'}|{output_dir / 'app.res'}",
            f'/OUT:"{executable}" /MANIFESTINPUT:"{generated_manifest}" '
            f'KERNEL32.LIB "{root / "build/x64/vcpkg/fmtd.lib"}"',
        ],
    )
    _write_tlog(
        tlog / "link.read.1.tlog",
        [
            f"^{output_dir / 'main.obj'}|{output_dir / 'provider.obj'}|{output_dir / 'app.res'}",
            str(root / "build/x64/vcpkg/fmtd.lib"),
            r"C:\SDK\lib\kernel32.lib",
        ],
    )
    _write_tlog(
        tlog / "rc.read.1.tlog",
        [
            f"^{resource}",
            str(resource_header),
            str(generated),
            r"C:\SDK\include\windows.h",
        ],
    )
    (root / "build/x64/vcpkg").mkdir(parents=True, exist_ok=True)
    (root / "build/x64/vcpkg/fmtd.lib").write_bytes(b"fmt")
    return graph, tlog


def _provider_map_text(
    *,
    symbols: list[str] | None = None,
    duplicate_symbol: str | None = None,
    member: str = "sakura_native_ffi-abc123.sakura_native_ffi-cgu.0.rcgu.o",
) -> str:
    rows = list(EXPECTED_OUTPUT_PROVIDER_SYMBOLS if symbols is None else symbols)
    if duplicate_symbol is not None:
        rows.append(duplicate_symbol)
    lines = [
        "  Address         Publics by Value              Rva+Base               Lib:Object",
        "",
    ]
    for index, symbol in enumerate(rows):
        address = f"{0x17430 + index * 0x10:08x}"
        lines.append(
            f" 0001:{address[-8:]}       {symbol} {0x140018430 + index * 0x10:016x} f   "
            f"sakura_native_ffi:{member}"
        )
    lines.extend([
        "",
        " Static symbols",
        " 0001:00000000 ?decorated_sakura_output_provider_stop_v1@@YAXXZ 0000000140000000 f app.obj",
    ])
    return "\r\n".join(lines) + "\r\n"


def _provider_map_fixture(
    root: Path,
    *,
    map_text: str | None = None,
    duplicate_symbol: str | None = None,
    duplicate_archive_input: bool = False,
):
    graph, tlog = _native_fixture(root)
    output_dir = graph.repo_root / "build/x64/Debug/app"
    executable = graph.repo_root / "x64/Debug/sakura.exe"
    executable.write_bytes(b"provider-product")
    provider_library = graph.repo_root / "build/components/sakura_native_ffi.lib"
    provider_library.parent.mkdir(parents=True, exist_ok=True)
    provider_library.write_bytes(b"sakura-native-ffi")
    link_command = f'/OUT:"{executable}" /MAP'
    if duplicate_archive_input:
        link_command += f' "{provider_library}" "{provider_library}"'
    link_command += " KERNEL32.LIB"
    _write_tlog(
        tlog / "link.command.1.tlog",
        [
            f"^{output_dir / 'main.obj'}|{output_dir / 'app.res'}|{provider_library}",
            link_command,
        ],
    )
    _write_tlog(
        tlog / "link.read.1.tlog",
        [
            f"^{output_dir / 'main.obj'}|{output_dir / 'app.res'}|{provider_library}",
            str(provider_library),
            r"C:\SDK\lib\kernel32.lib",
        ],
    )
    map_path = executable.with_suffix(".map")
    map_path.write_text(
        _provider_map_text(duplicate_symbol=duplicate_symbol) if map_text is None else map_text,
        encoding="ascii",
        newline="",
    )
    return graph, tlog, executable, map_path, provider_library


class ProductNativeEvidenceTests(unittest.TestCase):
    def test_validation_rejects_absolute_and_parent_freshness_keys(self) -> None:
        for unsafe_kind in ("absolute", "parent"):
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                graph, _tlog = _native_fixture(root)
                evidence = collect_product_native_evidence(
                    graph,
                    "product",
                    "msvc-x64-debug",
                    build_observed=True,
                )
                source_inputs = evidence["freshness"]["source_inputs"]
                original_key, original_hash = next(iter(source_inputs.items()))
                del source_inputs[original_key]
                unsafe_key = str(root / original_key) if unsafe_kind == "absolute" else "../" + original_key
                source_inputs[unsafe_key] = original_hash
                evidence["hard_evidence_hash"] = product_native_evidence_hash(evidence)
                evidence_path = root / "build/evidence/native-product.json"
                write_product_native_evidence(evidence_path, evidence)
                validation = validate_product_native_evidence(
                    graph,
                    evidence_path,
                    "product",
                    "msvc-x64-debug",
                )
                self.assertIn(
                    "NATIVE_PRODUCT_EVIDENCE_INPUT_PATH_UNSAFE",
                    {item["code"] for item in validation["failures"]},
                )

    def test_validation_rejects_reparse_ancestor_in_freshness_key(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            graph, _tlog = _native_fixture(root)
            alias = root / "source-alias"
            try:
                os.symlink(root / "app", alias, target_is_directory=True)
            except OSError:
                self.skipTest("symbolic links are unavailable on this Windows host")
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            source_inputs = evidence["freshness"]["source_inputs"]
            original_key, original_hash = next(
                (key, value) for key, value in source_inputs.items() if str(key).startswith("app/")
            )
            del source_inputs[original_key]
            source_inputs["source-alias/" + original_key.split("/", 1)[1]] = original_hash
            evidence["hard_evidence_hash"] = product_native_evidence_hash(evidence)
            evidence_path = root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )
            self.assertIn(
                "NATIVE_PRODUCT_EVIDENCE_INPUT_PATH_UNSAFE",
                {item["code"] for item in validation["failures"]},
            )

    def test_declared_intdir_selects_product_tlogs_over_release_variants(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, tlog = _native_fixture(Path(temporary))
            project = graph.repo_root / "app.vcxproj"
            project.write_text(
                project.read_text(encoding="utf-8").replace(
                    "  <ItemGroup>\n",
                    "  <PropertyGroup><IntDir>build\\$(Platform)\\$(Configuration)\\app\\</IntDir></PropertyGroup>\n"
                    "  <ItemGroup>\n",
                    1,
                ),
                encoding="utf-8",
            )
            shutil.copytree(tlog, graph.repo_root / "build/x64/Debug/app-avx2/app.tlog")

            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )

            tracker_inputs = evidence["freshness"]["tracker_inputs"]
            self.assertTrue(tracker_inputs)
            self.assertTrue(all("app-avx2" not in value for value in tracker_inputs))

    def test_diagnostic_targets_distinguish_execution_from_localized_skips_and_correlate_exact_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            generated = graph.repo_root / "build/x64/version.h"
            diagnostic = graph.repo_root / "build/evidence/msbuild-diagnostic.log"
            _write(
                diagnostic,
                "\n".join([
                    'Target "GenerateVersionHeader: (Target ID: 61)" (entry point):',
                    'Building target "GenerateVersionHeader" completely.',
                    f'Output file "{generated}" does not exist.',
                    'Task "Exec" (TaskId: 8)',
                    'Done executing task "Exec".',
                    'Done building target "GenerateVersionHeader".',
                    'false 条件により、ターゲット "GenerateFuncCodeEnum" を省略しました。',
                    '',
                ]),
            )

            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
                build_target="Rebuild",
                diagnostic_log_path=diagnostic,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            evidence_mtime = evidence_path.stat().st_mtime_ns
            write_product_native_evidence(evidence_path, evidence)
            self.assertEqual(evidence_mtime, evidence_path.stat().st_mtime_ns)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
                native_evidence_path=evidence_path,
            )

        generator = evidence["generator_observation"]
        self.assertTrue(generator["target_scheduling_observed"])
        self.assertTrue(generator["correlated_execution_observed"])
        self.assertEqual(2, len(generator["producer_consumer_correlations"]))
        self.assertEqual(
            {"compiler", "resource"},
            {item["consuming_phase"] for item in generator["producer_consumer_correlations"]},
        )
        for correlation in generator["producer_consumer_correlations"]:
            self.assertEqual("GenerateVersionHeader", correlation["producer_target"])
            self.assertEqual("build/x64/version.h", correlation["output"])
        enum_target = next(item for item in generator["target_results"] if item["target"] == "GenerateFuncCodeEnum")
        self.assertEqual({"condition_false": 1}, enum_target["terminal_outcomes"])
        self.assertTrue(validation["coverage"]["generator_execution_observed"])
        self.assertTrue(inventory["generated_provenance"]["native_execution_observed"])
        codes = {item["code"] for item in inventory["findings"]}
        self.assertNotIn("NATIVE_GENERATOR_EXECUTION_UNOBSERVED", codes)
        self.assertNotIn("NATIVE_GENERATOR_CONSUMER_CORRELATION_UNOBSERVED", codes)

    def test_up_to_date_generator_is_scheduling_evidence_not_execution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            generated = graph.repo_root / "build/x64/version.h"
            diagnostic = graph.repo_root / "build/evidence/msbuild-diagnostic.log"
            _write(
                diagnostic,
                "\n".join([
                    'ファイル "cmake.targets" 内にあるターゲット "GenerateVersionHeader: (ターゲット ID: 61)" (エントリ ポイント):',
                    'すべての出力ファイルが入力ファイルに対して最新なので、ターゲット "GenerateVersionHeader" を省略します。',
                    f'出力ファイル: {generated}',
                    'プロジェクト内のターゲット "GenerateVersionHeader" のビルドが終了しました。',
                    '',
                ]),
            )

            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
                diagnostic_log_path=diagnostic,
            )

        generator = evidence["generator_observation"]
        self.assertTrue(generator["target_scheduling_observed"])
        self.assertFalse(generator["correlated_execution_observed"])
        self.assertEqual({"up_to_date": 1}, generator["target_results"][0]["terminal_outcomes"])
        self.assertFalse(generator["producer_consumer_correlations"][0]["execution_observed"])

    def test_unrelated_target_exec_is_not_attributed_to_generator(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            generated = graph.repo_root / "build/x64/version.h"
            diagnostic = graph.repo_root / "build/evidence/msbuild-diagnostic.log"
            _write(
                diagnostic,
                "\n".join([
                    'Target "GenerateVersionHeader: (Target ID: 61)" (entry point):',
                    f'Output file: {generated}',
                    'Done building target "GenerateVersionHeader".',
                    'Target "UnrelatedDeployment: (Target ID: 62)" (entry point):',
                    'Task "Exec" (TaskId: 9)',
                    'Done executing task "Exec".',
                    'Done building target "UnrelatedDeployment".',
                    '',
                ]),
            )

            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
                diagnostic_log_path=diagnostic,
            )

        generator = evidence["generator_observation"]
        self.assertFalse(generator["target_scheduling_observed"])
        self.assertFalse(generator["correlated_execution_observed"])
        self.assertEqual(0, generator["target_results"][0]["exec_task_count"])
        self.assertEqual({"unclassified": 1}, generator["target_results"][0]["terminal_outcomes"])

    def test_tampered_generator_observation_breaks_hard_evidence_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence["generator_observation"]["correlated_execution_observed"] = True
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)

            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_HASH_MISMATCH",
            {item["code"] for item in validation["failures"]},
        )

    def test_collect_validate_and_merge_preserve_partial_native_scope(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
                native_evidence_path=evidence_path,
            )

        self.assertEqual(2, evidence["compiler"]["translation_unit_count"])
        self.assertEqual(["build/x64/version.h"], evidence["compiler"]["generated_header_inputs"])
        self.assertEqual({"none": 1, "use": 1}, evidence["compiler"]["pch_mode_counts"])
        self.assertEqual(
            "build/x64/Debug/app/app.pch",
            next(unit for unit in evidence["compiler"]["translation_units"] if unit["source"] == "app/main.cpp")["pch"]["file"],
        )
        self.assertTrue(evidence["link"]["input_set_observed"])
        self.assertFalse(evidence["link"]["selected_archive_members_observed"])
        self.assertIn("name:kernel32.lib", evidence["link"]["command_libraries"])
        self.assertTrue(validation["valid"])
        self.assertTrue(inventory["native_product_observation"]["valid"])
        self.assertTrue(inventory["generated_provenance"]["native_input_consumption_observed"])
        self.assertTrue(inventory["resource_provenance"]["native_compiler_inputs_observed"])
        self.assertTrue(inventory["product_link_provenance"]["native_input_set_observed"])
        self.assertFalse(inventory["product_link_provenance"]["native_product_link_closure_observed"])
        self.assertEqual("provider", inventory["native_product_observation"]["consumer_provider_edges"][0]["provider"])
        codes = {item["code"] for item in inventory["findings"]}
        self.assertNotIn("NATIVE_PRODUCT_EVIDENCE_NOT_PROVIDED", codes)
        self.assertIn("NATIVE_PRODUCT_LINK_CLOSURE_UNOBSERVED", codes)

    def test_link_map_proves_selected_provider_archive_member_and_closure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, tlog = _native_fixture(Path(temporary))
            output_dir = graph.repo_root / "build/x64/Debug/app"
            executable = graph.repo_root / "x64/Debug/app.exe"
            provider_library = graph.repo_root / "build/components/provider.lib"
            provider_library.parent.mkdir(parents=True, exist_ok=True)
            provider_library.write_bytes(b"provider-library")
            _write_tlog(
                tlog / "link.command.1.tlog",
                [
                    f"^{output_dir / 'main.obj'}|{output_dir / 'app.res'}|{provider_library}",
                    f'/OUT:"{executable}" /MAP KERNEL32.LIB',
                ],
            )
            _write_tlog(
                tlog / "link.read.1.tlog",
                [
                    f"^{output_dir / 'main.obj'}|{output_dir / 'app.res'}|{provider_library}",
                    str(provider_library),
                    r"C:\SDK\lib\kernel32.lib",
                ],
            )
            _write(
                executable.with_suffix(".map"),
                " 0001:00000000 direct 00000000 f main.obj\n"
                " 0001:00000010 provider 00000010 f provider:provider.obj\n",
            )

            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
                native_evidence_path=evidence_path,
            )

            self.assertTrue(evidence["link"]["selected_archive_members_observed"])
            self.assertNotIn(
                "repo:build/components/provider.lib",
                evidence["link"]["command_libraries"],
            )
            self.assertIn(
                "build/components/provider.lib",
                evidence["link"]["repository_libraries"],
            )
            self.assertEqual(
                ["provider.obj"],
                evidence["link"]["selected_archive_member_evidence"]["members"],
            )
            self.assertTrue(inventory["product_link_provenance"]["native_product_link_closure_observed"])
            edge = inventory["native_product_observation"]["consumer_provider_edges"][0]
            self.assertEqual("provider", edge["provider"])
            self.assertEqual("provider.obj", edge["witnesses"][0]["archive_member"])

            executable.with_suffix(".map").write_text("changed\n", encoding="utf-8")
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_MAP_CHANGED",
            {item["code"] for item in validation["failures"]},
        )

    def test_realistic_msvc_map_generates_provider_scoped_symbol_and_member_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, map_path, _archive = _provider_map_fixture(Path(temporary))
            expected_map_size = map_path.stat().st_size
            expected_archive_hash = "sha256:" + hashlib.sha256(_archive.read_bytes()).hexdigest()
            expected_archive_size = _archive.stat().st_size
            expected_output = graph.repo_root / "x64/Debug/sakura.exe"
            expected_link_command_hash = "sha256:" + hashlib.sha256(
                f'/OUT:"{expected_output}" /MAP KERNEL32.LIB'.encode("utf-8")
            ).hexdigest()
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        member_evidence = evidence["link"]["output_provider_member_evidence"]
        symbol_evidence = evidence["link"]["output_provider_symbol_evidence"]
        self.assertTrue(member_evidence["observed"])
        self.assertEqual("output-provider", member_evidence["provider"])
        self.assertEqual("sakura_native_ffi.lib", member_evidence["archive_name"])
        self.assertEqual(1, member_evidence["member_count"])
        self.assertEqual(7, len(member_evidence["contributions"]))
        self.assertEqual("sakura_native_ffi.lib", member_evidence["contributing_archives"][0])
        self.assertTrue(member_evidence["members"][0].endswith(".rcgu.o"))
        self.assertTrue(symbol_evidence["observed"])
        self.assertEqual("output-provider", symbol_evidence["scope"])
        self.assertEqual(EXPECTED_OUTPUT_PROVIDER_SYMBOLS, symbol_evidence["symbols"])
        self.assertEqual(7, symbol_evidence["symbol_count"])
        self.assertEqual(0, symbol_evidence["duplicate_count"])
        self.assertEqual(map_path.relative_to(graph.repo_root).as_posix(), member_evidence["map"])
        self.assertEqual(member_evidence["map_hash"], symbol_evidence["map_hash"])
        self.assertEqual(expected_map_size, evidence["link"]["selected_archive_member_evidence"]["map_size_bytes"])
        self.assertEqual(expected_map_size, member_evidence["map_size_bytes"])
        self.assertEqual(expected_map_size, symbol_evidence["map_size_bytes"])
        self.assertEqual(len(b"provider-product"), evidence["link"]["product_size_bytes"])
        self.assertEqual(expected_archive_hash, evidence["link"]["rustArchiveSha256"])
        self.assertEqual(expected_archive_size, evidence["link"]["rustArchiveSizeBytes"])
        self.assertEqual(1, evidence["link"]["rustArchiveCount"])
        self.assertEqual(expected_link_command_hash, evidence["link"]["linkCommandSha256"])
        self.assertTrue(validation["valid"])
        self.assertTrue(validation["coverage"]["output_provider_member_evidence_observed"])
        self.assertTrue(validation["coverage"]["output_provider_symbol_evidence_observed"])

    def test_final_image_cpp_accepts_only_the_complete_ltcg_negative_projection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(
                Path(temporary),
                map_text=_provider_map_text(symbols=[]),
            )
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            link = evidence["link"]
            self.assertFalse(validate_output_provider_evidence_for_final_image(link)["valid"])
            self.assertTrue(
                validate_output_provider_evidence_for_final_image(
                    link,
                    graph=graph,
                    expected_backend="cpp",
                )["valid"]
            )
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    link,
                    expected_backend="rust",
                )["valid"]
            )
            self.assertEqual([], link["output_provider_member_evidence"]["members"])
            self.assertEqual(0, link["output_provider_member_evidence"]["member_count"])
            self.assertEqual([], link["output_provider_symbol_evidence"]["symbols"])
            self.assertEqual(0, link["output_provider_symbol_evidence"]["symbol_count"])
            self.assertEqual(1, link["output_provider_member_evidence"]["archive_input_count"])

            for path, field, value in (
                ("output_provider_symbol_evidence", "symbols", [EXPECTED_OUTPUT_PROVIDER_SYMBOLS[0]]),
                ("output_provider_member_evidence", "members", ["unexpected.obj"]),
                ("output_provider_member_evidence", "archive_name", "sakura_native_ffi.lib"),
                ("output_provider_member_evidence", "archive_input_count", 2),
                ("output_provider_member_evidence", "member_count", False),
                ("output_provider_member_evidence", "contributing_archive_count", False),
                ("output_provider_member_evidence", "missing_symbols", []),
                ("output_provider_symbol_evidence", "symbol_count", False),
                ("output_provider_symbol_evidence", "duplicate_count", 1),
            ):
                malformed = copy.deepcopy(link)
                malformed[path][field] = value
                self.assertFalse(
                    validate_output_provider_evidence_for_final_image(
                        malformed,
                        expected_backend="cpp",
                    )["valid"],
                    (path, field),
                )
            missing_archive_name = copy.deepcopy(link)
            del missing_archive_name["output_provider_member_evidence"]["archive_name"]
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    missing_archive_name,
                    expected_backend="cpp",
                )["valid"]
            )
            symbol_archive_count = copy.deepcopy(link)
            symbol_archive_count["output_provider_symbol_evidence"]["contributing_archive_count"] = 1
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    symbol_archive_count,
                    expected_backend="cpp",
                )["valid"]
            )
            mixed = copy.deepcopy(link)
            mixed["output_provider_member_evidence"]["observed"] = True
            self.assertFalse(
                validate_output_provider_evidence_for_final_image(
                    mixed,
                    expected_backend="cpp",
                )["valid"]
            )

    def test_final_image_positive_provider_requires_one_member_and_strict_archive_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            original = evidence["link"]
            one_member = original["output_provider_member_evidence"]["members"][0]
            two_members = [one_member, "other-provider.obj"]
            contributions = [
                {
                    "symbol": symbol,
                    "archive": "sakura_native_ffi.lib",
                    "member": two_members[index % 2],
                }
                for index, symbol in enumerate(EXPECTED_OUTPUT_PROVIDER_SYMBOLS)
            ]
            two_member_link = copy.deepcopy(original)
            two_member_link["output_provider_member_evidence"]["members"] = two_members
            two_member_link["output_provider_member_evidence"]["member_count"] = 2
            two_member_link["output_provider_member_evidence"]["contributing_members"] = two_members
            two_member_link["output_provider_member_evidence"]["contributions"] = contributions
            two_member_link["output_provider_symbol_evidence"]["contributing_members"] = two_members
            two_member_link["output_provider_symbol_evidence"]["contributions"] = contributions
            for backend in (None, "cpp", "rust"):
                validation = validate_output_provider_evidence_for_final_image(
                    two_member_link,
                    expected_backend=backend,
                )
                self.assertFalse(validation["valid"], backend)

            for evidence_name in (
                "output_provider_member_evidence",
                "output_provider_symbol_evidence",
            ):
                for value in (True, 1.0):
                    malformed = copy.deepcopy(original)
                    malformed[evidence_name]["contributing_archive_count"] = value
                    for backend in (None, "cpp", "rust"):
                        validation = validate_output_provider_evidence_for_final_image(
                            malformed,
                            expected_backend=backend,
                        )
                        self.assertFalse(validation["valid"], (evidence_name, value, backend))

    def test_realistic_msvc_map_collector_shape_binds_to_final_image(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, executable, map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            receipt = stage_output_final_image(
                repo_root=graph.repo_root,
                stage_root=graph.repo_root / "staged-final-images",
                backend="rust",
                platform="x64",
                configuration="Debug",
                source_native_evidence_sha256=evidence["hard_evidence_hash"],
                executable_path=executable,
                map_path=map_path,
                native_evidence=evidence,
            )
            bound = bind_native_evidence_to_final_image(evidence, receipt)

        self.assertTrue(validate_output_provider_evidence_for_final_image(evidence["link"])["valid"])
        self.assertEqual(receipt["sourceNativeEvidenceSha256"], product_native_evidence_source_hash(bound))
        self.assertEqual(bound["hard_evidence_hash"], product_native_evidence_hash(bound))

    def test_duplicate_identical_provider_archive_arguments_are_unproven(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(
                Path(temporary),
                duplicate_archive_input=True,
            )
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )

        link = evidence["link"]
        self.assertEqual(2, link["rustArchiveCount"])
        self.assertIsNone(link["rustArchiveSha256"])
        self.assertIsNone(link["rustArchiveSizeBytes"])
        self.assertFalse(link["output_provider_member_evidence"]["observed"])
        validation = validate_output_provider_evidence_for_final_image(link)
        self.assertFalse(validation["valid"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN",
            {item["code"] for item in validation["failures"]},
        )

    def test_final_image_provider_requires_selected_members_observed_as_bool_true(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            original = evidence["link"]
            for value in (False, 1, "true", None):
                link = copy.deepcopy(original)
                link["selected_archive_members_observed"] = value
                validation = validate_output_provider_evidence_for_final_image(link)
                self.assertFalse(validation["valid"], value)
                self.assertIn(
                    "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN",
                    {item["code"] for item in validation["failures"]},
                )

    def test_final_image_provider_requires_strict_map_sizes_for_all_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            original = evidence["link"]
            for key in (
                "selected_archive_member_evidence",
                "output_provider_member_evidence",
                "output_provider_symbol_evidence",
            ):
                for value in (True, "not-an-integer", -1):
                    link = copy.deepcopy(original)
                    link[key]["map_size_bytes"] = value
                    validation = validate_output_provider_evidence_for_final_image(link)
                    self.assertFalse(validation["valid"], (key, value))
                    self.assertIn(
                        "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_SCHEMA",
                        {item["code"] for item in validation["failures"]},
                    )

    def test_final_image_provider_requires_three_map_identities_to_consensus(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            original = evidence["link"]
            for key, field, value in (
                ("selected_archive_member_evidence", "map", "x64/Debug/other.map"),
                ("output_provider_member_evidence", "map_hash", "sha256:" + "0" * 64),
                ("output_provider_symbol_evidence", "map_size_bytes", 0),
            ):
                link = copy.deepcopy(original)
                link[key][field] = value
                validation = validate_output_provider_evidence_for_final_image(link)
                self.assertFalse(validation["valid"], (key, field))
                self.assertIn(
                    "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_MAP_MISMATCH",
                    {item["code"] for item in validation["failures"]},
                )

    def test_final_image_provider_requires_product_link_and_archive_identity_types(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            original = evidence["link"]
            for key, value, code in (
                ("product_size_bytes", True, "NATIVE_PRODUCT_EVIDENCE_OUTPUT_IDENTITY_SCHEMA"),
                ("product_hash", "not-a-sha256", "NATIVE_PRODUCT_EVIDENCE_OUTPUT_IDENTITY_SCHEMA"),
                ("linkCommandSha256", "not-a-sha256", "NATIVE_PRODUCT_EVIDENCE_LINK_COMMAND_SCHEMA"),
                ("rustArchiveSizeBytes", False, "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_SCHEMA"),
                ("rustArchiveCount", True, "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_ARCHIVE_SCHEMA"),
            ):
                link = copy.deepcopy(original)
                link[key] = value
                validation = validate_output_provider_evidence_for_final_image(link)
                self.assertFalse(validation["valid"], key)
                self.assertIn(code, {item["code"] for item in validation["failures"]})

    def test_provider_map_missing_symbol_is_unproven_and_records_missing_set(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(
                Path(temporary),
                map_text=_provider_map_text(symbols=EXPECTED_OUTPUT_PROVIDER_SYMBOLS[:-1]),
            )
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        member_evidence = evidence["link"]["output_provider_member_evidence"]
        symbol_evidence = evidence["link"]["output_provider_symbol_evidence"]
        self.assertFalse(member_evidence["observed"])
        self.assertFalse(symbol_evidence["observed"])
        self.assertEqual([EXPECTED_OUTPUT_PROVIDER_SYMBOLS[-1]], member_evidence["missing_symbols"])
        self.assertEqual([EXPECTED_OUTPUT_PROVIDER_SYMBOLS[-1]], symbol_evidence["missing_symbols"])
        self.assertEqual(6, symbol_evidence["symbol_count"])
        self.assertFalse(validation["valid"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN",
            {item["code"] for item in validation["failures"]},
        )

    def test_provider_map_duplicate_symbol_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(
                Path(temporary),
                duplicate_symbol=EXPECTED_OUTPUT_PROVIDER_SYMBOLS[0],
            )
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        member_evidence = evidence["link"]["output_provider_member_evidence"]
        symbol_evidence = evidence["link"]["output_provider_symbol_evidence"]
        self.assertFalse(member_evidence["observed"])
        self.assertFalse(symbol_evidence["observed"])
        self.assertEqual(1, member_evidence["duplicate_count"])
        self.assertEqual(1, symbol_evidence["duplicate_count"])
        self.assertFalse(validation["valid"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_OUTPUT_PROVIDER_UNPROVEN",
            {item["code"] for item in validation["failures"]},
        )

    def test_provider_map_unexpected_symbol_breaks_exact_set(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, _map_path, _archive = _provider_map_fixture(
                Path(temporary),
                map_text=_provider_map_text(
                    symbols=EXPECTED_OUTPUT_PROVIDER_SYMBOLS + ["sakura_output_provider_extra_v1"]
                ),
            )
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )

        symbol_evidence = evidence["link"]["output_provider_symbol_evidence"]
        self.assertFalse(symbol_evidence["observed"])
        self.assertEqual(["sakura_output_provider_extra_v1"], symbol_evidence["unexpected_symbols"])
        self.assertEqual(8, symbol_evidence["symbol_count"])

    def test_provider_map_hash_is_revalidated_after_collection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog, _executable, map_path, _archive = _provider_map_fixture(Path(temporary))
            evidence = collect_product_native_evidence(
                graph,
                "product",
                "msvc-x64-debug",
                build_observed=True,
            )
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            map_path.write_text("tampered map\n", encoding="ascii", newline="")
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        provider_map_failures = {
            item.get("field")
            for item in validation["failures"]
            if item["code"] == "NATIVE_PRODUCT_EVIDENCE_MAP_CHANGED"
        }
        self.assertIn("member", provider_map_failures)
        self.assertIn("symbol", provider_map_failures)

    def test_changed_source_input_makes_evidence_stale(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=True)
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)
            _write(graph.repo_root / "provider/provider.h", "#pragma once\n// changed\n")

            validation = validate_product_native_evidence(graph, evidence_path, "product", "msvc-x64-debug")
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
                native_evidence_path=evidence_path,
            )

        self.assertFalse(validation["valid"])
        self.assertEqual("stale_or_mismatched", validation["status"])
        self.assertIn("NATIVE_PRODUCT_EVIDENCE_INPUT_CHANGED", {item["code"] for item in validation["failures"]})
        self.assertFalse(inventory["native_product_observation"]["valid"])
        self.assertFalse(inventory["product_link_provenance"]["native_input_set_observed"])
        self.assertIn("NATIVE_PRODUCT_EVIDENCE_INPUT_CHANGED", {item["code"] for item in inventory["findings"]})

    def test_link_manifest_is_hashed_and_invalidates_stale_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=True)
            manifest = "build/x64/app.exe.manifest"
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)

            self.assertIn(manifest, evidence["link"]["repo_inputs"])
            self.assertIn(manifest, evidence["freshness"]["generated_inputs"])

            _write(graph.repo_root / manifest, "<assembly changed=\"true\" />\n")
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        self.assertEqual("stale_or_mismatched", validation["status"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_INPUT_CHANGED",
            {item["code"] for item in validation["failures"]},
        )

    def test_changed_product_binary_makes_evidence_stale(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=True)
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)

            (graph.repo_root / "x64/Debug/app.exe").write_bytes(b"changed-product")
            validation = validate_product_native_evidence(
                graph,
                evidence_path,
                "product",
                "msvc-x64-debug",
            )

        self.assertFalse(validation["valid"])
        self.assertEqual("stale_or_mismatched", validation["status"])
        self.assertIn(
            "NATIVE_PRODUCT_EVIDENCE_PRODUCT_CHANGED",
            {item["code"] for item in validation["failures"]},
        )

    def test_context_mismatch_is_explicit_and_not_observed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=True)
            evidence["context_id"] = "msvc-x64-release"
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)

            validation = validate_product_native_evidence(graph, evidence_path, "product", "msvc-x64-debug")

        self.assertFalse(validation["valid"])
        self.assertIn("NATIVE_PRODUCT_EVIDENCE_CONTEXT_ID_MISMATCH", {item["code"] for item in validation["failures"]})

    def test_unbuilt_tracker_snapshot_is_not_valid_native_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence = collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=False)
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            write_product_native_evidence(evidence_path, evidence)

            validation = validate_product_native_evidence(graph, evidence_path, "product", "msvc-x64-debug")

        self.assertFalse(validation["valid"])
        self.assertIn("NATIVE_PRODUCT_EVIDENCE_BUILD_NOT_OBSERVED", {item["code"] for item in validation["failures"]})

    def test_missing_link_tracker_has_typed_terminal_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, tlog = _native_fixture(Path(temporary))
            (tlog / "link.read.1.tlog").unlink()

            with self.assertRaises(BuildError) as caught:
                collect_product_native_evidence(graph, "product", "msvc-x64-debug", build_observed=True)

        self.assertEqual("NATIVE_PRODUCT_TLOG_MISSING", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)

    def test_malformed_evidence_has_typed_terminal_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph, _tlog = _native_fixture(Path(temporary))
            evidence_path = graph.repo_root / "build/evidence/native-product.json"
            _write(evidence_path, "{not-json\n")

            with self.assertRaises(BuildError) as caught:
                validate_product_native_evidence(graph, evidence_path, "product", "msvc-x64-debug")

        self.assertEqual("NATIVE_PRODUCT_EVIDENCE_PARSE", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)


if __name__ == "__main__":
    unittest.main()
