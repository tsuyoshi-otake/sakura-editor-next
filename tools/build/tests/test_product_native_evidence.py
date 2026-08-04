from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.product_native_evidence import (  # noqa: E402
    collect_product_native_evidence,
    validate_product_native_evidence,
    write_product_native_evidence,
)
from sakura_build_lib.repository_inventory import collect_repository_inventory  # noqa: E402
from sakura_build_lib.runner import BuildError  # noqa: E402
from test_repository_inventory import _fixture, _write  # noqa: E402


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


class ProductNativeEvidenceTests(unittest.TestCase):
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
