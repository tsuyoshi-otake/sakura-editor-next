from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

import sakura_build
from sakura_build_lib import generator as build_generator
from sakura_build_lib.checkout_invariance import verify_checkout_invariance
from sakura_build_lib import test_inventory as test_inventory_module
from sakura_build_lib.generator import generate, stale_component_outputs, stale_outputs
from sakura_build_lib.model import GENERATOR_VERSION, ManifestError, evaluate_condition, load_semantic_graph, normalize_condition
from sakura_build_lib.runner import (
    COMPONENT_ISOLATED_ENVIRONMENT,
    BuildError,
    EventWriter,
    allocate_parallelism,
    cmake_commands,
    cmake_component_build_dir,
    cmake_component_commands,
    distribution_commands,
    msbuild_command,
    msbuild_log_path,
    mingw_environment,
    run_commands,
    solution_commands,
)
from sakura_build_lib.test_inventory import (
    TestInventoryError,
    compare_inventories,
    guarantee_fingerprint,
    parse_gtest_list,
    refresh_runtime_mappings,
    validate_inventory,
    verify_runtime_mappings,
)


def manifest_data() -> dict:
    return {
        "schema_version": 3,
        "minimum_generator_version": "0.2.1",
        "contexts": [
            {"id": "ctx", "platform": "x64", "arch": "x64", "configuration": "Debug", "toolchain": "msvc", "backend": "msbuild", "role": "test", "features": []}
        ],
        "components": [
            {"id": "provider", "family": "test", "kind": "implementation", "maturity": "candidate", "build_definition": "legacy", "supported_contexts": ["ctx"], "owner": "provider", "sources": ["provider"], "public_headers": [], "private_headers": [], "ownership_exclusions": [], "public_include_roots": ["provider"], "private_include_roots": [], "state_owner": None, "backend_targets": {"msbuild": ["provider.vcxproj"]}, "compile_profile": "project-compile"},
            {"id": "consumer", "family": "test", "kind": "implementation", "maturity": "candidate", "build_definition": "legacy", "supported_contexts": ["ctx"], "owner": "consumer", "sources": ["consumer"], "public_headers": [], "private_headers": [], "ownership_exclusions": [], "public_include_roots": [], "private_include_roots": ["consumer"], "state_owner": None, "backend_targets": {"msbuild": ["consumer.vcxproj"]}, "compile_profile": "project-compile"}
        ],
        "contracts": [],
        "artifacts": [],
        "edges": [
            {"id": "consumer-to-provider", "from": "consumer", "to": "provider", "kind": "api", "phases": ["compile", "link"], "visibility": "public", "propagation": "public", "contract_profile": "contract-edge", "condition": True, "required": True, "witnesses": [{"context": "ctx", "probe": "tests/contract.cpp"}]}
        ]
    }


def compile_profiles_data() -> dict:
    return {
        "schema_version": 1,
        "project_profiles": [{"id": "project-compile", "abi_family": "msvc", "toolset": "v143", "arch": "x64", "configuration": "Debug", "crt": "MTd", "iterator_debug_level": 2, "wchar_t_builtin": True, "default_pack": 8, "unicode": True, "win32_winnt": "0x0A00", "cpp_standard": "c++20"}],
        "contract_profiles": [{"id": "contract-edge", "uses_stl_types": True, "crosses_allocator_boundary": False, "exception_boundary": "no_throw", "rtti_boundary": "none", "layout_boundary": "value", "required_project_fields": ["abi_family", "arch", "crt", "iterator_debug_level", "default_pack"]}],
        "link_profiles": [{"id": "link-instrumentation", "lto": "off", "sanitizer": "none", "debug_format": "pdb", "incremental_link": False}],
        "context_profiles": [{"id": "context-ctx", "context_id": "ctx", "project_profile": "project-compile", "link_profile": "link-instrumentation"}],
    }


class RepositoryFixture:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "provider").mkdir()
        (self.root / "consumer").mkdir()
        (self.root / "provider.vcxproj").touch()
        (self.root / "consumer.vcxproj").touch()
        (self.root / "tests").mkdir()
        (self.root / "tests/contract.cpp").touch()
        manifest = self.root / "src/main/modules/modules.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps(manifest_data()), encoding="utf-8")
        (manifest.parent / "compile-profiles.json").write_text(json.dumps(compile_profiles_data()), encoding="utf-8")
        (manifest.parent / "schema-v3.json").write_text("{}\n", encoding="utf-8")
        return self.root, manifest

    def __exit__(self, *args):
        self.temporary.cleanup()


def generated_dual_backend_data(root: Path) -> tuple[dict, dict]:
    """Return a sourceful generated fixture with MSBuild and CMake projections."""
    value = manifest_data()
    value["contexts"].append({"id": "cmake-ctx", "platform": "x64", "arch": "x64", "configuration": "Debug", "toolchain": "msvc", "backend": "cmake", "role": "test", "features": []})
    value["edges"][0]["witnesses"].append({"context": "cmake-ctx", "probe": "tests/contract.cpp"})
    for component in value["components"]:
        component["build_definition"] = "generated"
        component["compile_profile"] = None
        component["supported_contexts"].append("cmake-ctx")
        component["backend_targets"] = {
            "msbuild": [f"src/main/modules/generated/msbuild/projects/{component['id']}.vcxproj"],
            "cmake": [component["id"]],
        }
        source = root / component["id"] / f"{component['id']}.cpp"
        source.write_text("// generated graph fixture\n", encoding="utf-8")
        component["sources"] = [source.relative_to(root).as_posix()]
    profiles = compile_profiles_data()
    profiles["context_profiles"].append({"id": "context-cmake-ctx", "context_id": "cmake-ctx", "project_profile": "project-compile", "link_profile": "link-instrumentation"})
    return value, profiles


def generated_debug_release_data(root: Path) -> tuple[dict, dict]:
    """Return a generated fixture with a Debug-only edge and MSBuild Release root."""
    value, profiles = generated_dual_backend_data(root)
    value["contexts"].append({
        "id": "ctx-release",
        "platform": "x64",
        "arch": "x64",
        "configuration": "Release",
        "toolchain": "msvc",
        "backend": "msbuild",
        "role": "test",
        "features": [],
    })
    for component in value["components"]:
        component["supported_contexts"].append("ctx-release")
    value["edges"][0]["condition"] = {"eq": {"field": "configuration", "value": "Debug"}}
    release_profile = dict(profiles["project_profiles"][0])
    release_profile.update({"id": "project-release", "configuration": "Release", "crt": "MT", "iterator_debug_level": 0})
    profiles["project_profiles"].append(release_profile)
    profiles["context_profiles"].append({
        "id": "context-ctx-release",
        "context_id": "ctx-release",
        "project_profile": "project-release",
        "link_profile": "link-instrumentation",
    })
    return value, profiles


class ManifestTests(unittest.TestCase):
    def test_schema_hash_is_independent_of_checkout_line_endings(self):
        with RepositoryFixture() as (root, manifest):
            lf_graph = load_semantic_graph(root, manifest)
            generate(lf_graph)

            result = verify_checkout_invariance(root, manifest, current_graph=lf_graph)

            self.assertTrue(result["ok"])
            self.assertTrue(result["semantic_graph"]["line_endings_invariant"])
            self.assertTrue(result["semantic_graph"]["projection_staleness_invariant"])
            self.assertTrue(result["semantic_graph"]["content_change_affects_graph"])
            self.assertTrue(result["semantic_inventory_scanner"]["line_endings_invariant"])
            self.assertTrue(result["semantic_inventory_scanner"]["current_matches_canonical"])

    def test_rejects_unknown_field(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["unexpected"] = True
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "MANIFEST_UNKNOWN_FIELD"):
                load_semantic_graph(root, manifest)

    def test_rejects_path_escape(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][0]["sources"] = ["../outside"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "MANIFEST_PATH_ESCAPE"):
                load_semantic_graph(root, manifest)

    def test_rejects_overlapping_ownership(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][1]["sources"] = ["provider"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "COMPONENT_PATH_OVERLAP"):
                load_semantic_graph(root, manifest)

    def test_rejects_duplicate_contract_id_across_graph(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["contracts"].append({"id": "consumer", "contract_kind": "inbound_api", "contract_owner": "consumer", "capability_id": "test.read", "canonical_value_types": [], "contract_decision": None})
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "MANIFEST_DUPLICATE_ID"):
                load_semantic_graph(root, manifest)

    def test_rejects_artifact_output_path_escape(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["artifacts"].append({"id": "generated-test", "owner": "consumer", "artifact_kind": "generated", "inputs": [], "outputs": ["../outside.txt"], "tool_id": "sakura-module-generator", "condition": True})
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "MANIFEST_PATH_ESCAPE"):
                load_semantic_graph(root, manifest)

    def test_rejects_manifest_outside_repository(self):
        with RepositoryFixture() as (root, _manifest), tempfile.TemporaryDirectory() as outside:
            external = Path(outside) / "modules.json"
            external.write_text(json.dumps(manifest_data()), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "MANIFEST_PATH_ESCAPE"):
                load_semantic_graph(root, external)

    def test_condition_is_canonical_and_evaluable(self):
        condition = normalize_condition({"all": [{"eq": {"field": "arch", "value": "x64"}}, {"has_feature": "fast"}, {"has_feature": "fast"}]})
        self.assertEqual(2, len(condition["all"]))
        self.assertTrue(evaluate_condition(condition, {"arch": "x64", "features": ["fast"]}))

    def test_closure_follows_consumer_to_dependency(self):
        with RepositoryFixture() as (root, manifest):
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("consumer", "provider"), graph.closure(("consumer",), "ctx", ("compile",)))
            self.assertEqual(("consumer",), graph.closure(("consumer",), "ctx", ("test",)))
            self.assertEqual((), graph.closure(("consumer",), "ctx", ("generate",)))

    def test_build_intent_passes_only_root_target_to_native_scheduler(self):
        with RepositoryFixture() as (root, manifest):
            graph = load_semantic_graph(root, manifest)
            intent = graph.build_intent(("consumer",), "ctx", ("compile", "link"))
            self.assertEqual(["consumer.vcxproj"], intent["backend_targets"]["msbuild"])
            self.assertEqual(["consumer", "provider"], intent["closure_by_phase"]["compile"])

    def test_context_and_build_intent_project_active_artifact_sets(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["artifacts"] = [
                {
                    "id": "consumer-package-set",
                    "owner": "consumer",
                    "artifact_kind": "package_set",
                    "inputs": ["tests/contract.cpp"],
                    "outputs": ["package:fixture"],
                    "tool_id": None,
                    "condition": True,
                },
                {
                    "id": "consumer-generated-output",
                    "owner": "consumer",
                    "artifact_kind": "generated",
                    "inputs": ["tests/contract.cpp"],
                    "outputs": ["generated/fixture.h"],
                    "tool_id": "sakura-module-generator",
                    "condition": {"eq": {"field": "toolchain", "value": "msvc"}},
                },
                {
                    "id": "consumer-staging-set",
                    "owner": "consumer",
                    "artifact_kind": "staging_set",
                    "inputs": ["tests/contract.cpp"],
                    "outputs": ["stage/fixture"],
                    "tool_id": "copy",
                    "condition": {"eq": {"field": "configuration", "value": "Release"}},
                },
            ]
            value["edges"].append({
                "id": "consumer-to-package-set",
                "from": "consumer",
                "to": "consumer-package-set",
                "kind": "package",
                "phases": ["compile", "link"],
                "visibility": "private",
                "propagation": "none",
                "contract_profile": None,
                "condition": True,
                "required": True,
                "witnesses": [{"context": "ctx", "probe": "tests/contract.cpp"}],
            })
            manifest.write_text(json.dumps(value), encoding="utf-8")

            graph = load_semantic_graph(root, manifest)
            projection = graph.project("ctx")
            self.assertEqual(["consumer-package-set"], projection["package_sets"])
            self.assertEqual(["consumer-generated-output"], projection["generated_artifacts"])
            self.assertEqual([], projection["staging_sets"])

            intent = graph.build_intent(("consumer",), "ctx", ("compile", "link"))
            self.assertEqual(["consumer-package-set"], intent["package_sets"])
            self.assertEqual([], intent["generated_artifacts"])
            self.assertEqual([], intent["staging_sets"])

    def test_required_runtime_stage_edges_use_static_configuration_witnesses(self):
        """A native build must not need its own not-yet-linked stage inputs to validate the graph."""
        repository_root = TOOLS_BUILD.parents[1]
        graph = load_semantic_graph(repository_root, repository_root / "src/main/modules/modules.json")
        staging_sets = {
            artifact_id
            for artifact_id, artifact in graph.artifacts.items()
            if artifact.artifact_kind == "staging_set"
        }
        stage_edges = [
            edge
            for edge in graph.edges
            if edge.source in staging_sets
            and edge.required
            and {"stage", "runtime"}.issubset(edge.phases)
        ]

        self.assertTrue(stage_edges)
        for edge in stage_edges:
            self.assertTrue(edge.witnesses)
            self.assertEqual(
                {"src/main/runtime/sakura-editor-runtime-stage.json"},
                {witness["probe"] for witness in edge.witnesses},
            )

    def test_contract_profile_belongs_to_edge_not_component(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][0]["compile_profile"] = "contract-edge"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "COMPILE_PROFILE_REFERENCE"):
                load_semantic_graph(root, manifest)

    def test_rejects_unknown_edge_contract_profile(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["edges"][0]["contract_profile"] = "unknown-contract"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "CONTRACT_PROFILE_REFERENCE"):
                load_semantic_graph(root, manifest)

    def test_abi_profile_mismatch_is_scoped_by_edge_policy(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            profiles = compile_profiles_data()
            provider_profile = dict(profiles["project_profiles"][0])
            provider_profile.update({"id": "provider-pack-8", "default_pack": 8})
            consumer_profile = dict(profiles["project_profiles"][0])
            consumer_profile.update({"id": "consumer-pack-1", "default_pack": 1})
            profiles["project_profiles"].extend((provider_profile, consumer_profile))
            value["components"][0]["compile_profile"] = "provider-pack-8"
            value["components"][1]["compile_profile"] = "consumer-pack-1"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")

            graph = load_semantic_graph(root, manifest)
            self.assertEqual(
                ({
                    "edge": "consumer-to-provider",
                    "contract_profile": "contract-edge",
                    "field": "default_pack",
                    "consumer": "consumer",
                    "consumer_profile": "consumer-pack-1",
                    "consumer_value": 1,
                    "provider": "provider",
                    "provider_profile": "provider-pack-8",
                    "provider_value": 8,
                },),
                graph.abi_profile_mismatches("ctx"),
            )

            profiles["contract_profiles"][0]["required_project_fields"] = ["abi_family", "arch"]
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            self.assertEqual((), load_semantic_graph(root, manifest).abi_profile_mismatches("ctx"))

    def test_rejects_unknown_contract_profile_field(self):
        with RepositoryFixture() as (root, manifest):
            profiles = compile_profiles_data()
            profiles["contract_profiles"][0]["required_project_fields"].append("overall_hash")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "CONTRACT_PROFILE_FIELD"):
                load_semantic_graph(root, manifest)

    def test_legacy_ownership_exclusion_allows_extracted_leaf(self):
        with RepositoryFixture() as (root, manifest):
            (root / "provider/extracted").mkdir()
            value = manifest_data()
            value["components"][0]["ownership_exclusions"] = ["provider/extracted"]
            value["components"][1]["sources"] = ["provider/extracted"]
            value["components"][1]["private_include_roots"] = ["provider/extracted"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("provider/extracted",), graph.components["provider"].ownership_exclusions)

    def test_rejects_missing_non_generated_msbuild_target(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][1]["backend_targets"]["msbuild"] = ["missing.vcxproj"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "BACKEND_TARGET_MISSING"):
                load_semantic_graph(root, manifest)

    def test_generated_leaf_cannot_link_to_legacy_monolith(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][1]["build_definition"] = "generated"
            value["components"][1]["backend_targets"]["msbuild"] = ["src/main/modules/generated/msbuild/projects/consumer.vcxproj"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "GENERATED_TO_LEGACY_DEPENDENCY"):
                load_semantic_graph(root, manifest)

    def test_rejects_sourceful_contract_component(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][0]["kind"] = "contract"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "COMPONENT_SOURCEFUL_INTERFACE"):
                load_semantic_graph(root, manifest)

    def test_rejects_private_header_include_and_state_interface_ownership(self):
        for field, value in (
            ("private_headers", ["provider/private.h"]),
            ("private_include_roots", ["provider"]),
            ("state_owner", "provider-state"),
        ):
            with self.subTest(field=field), RepositoryFixture() as (root, manifest):
                (root / "provider/private.h").touch()
                data = manifest_data()
                data["components"][0]["kind"] = "contract"
                data["components"][0]["sources"] = []
                data["components"][0][field] = value
                manifest.write_text(json.dumps(data), encoding="utf-8")
                expected = {
                    "private_headers": "COMPONENT_PRIVATE_HEADER_INTERFACE",
                    "private_include_roots": "COMPONENT_PRIVATE_INCLUDE_INTERFACE",
                    "state_owner": "COMPONENT_STATE_OWNER_INTERFACE",
                }[field]
                with self.assertRaisesRegex(ManifestError, expected):
                    load_semantic_graph(root, manifest)

    def test_rejects_runtime_artifact_owned_by_interface_component(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["components"][0]["kind"] = "contract"
            value["components"][0]["sources"] = []
            value["artifacts"].append({
                "id": "provider-runtime-asset",
                "owner": "provider",
                "artifact_kind": "asset",
                "inputs": [],
                "outputs": [],
                "tool_id": None,
                "condition": True,
            })
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "COMPONENT_RUNTIME_ARTIFACT_OWNER"):
                load_semantic_graph(root, manifest)

    def test_private_compile_edge_does_not_leak_through_dependency(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            (root / "private-provider").mkdir()
            (root / "private-provider.vcxproj").touch()
            value["components"].append({"id": "private-provider", "family": "test", "kind": "implementation", "maturity": "candidate", "build_definition": "legacy", "supported_contexts": ["ctx"], "owner": "private-provider", "sources": ["private-provider"], "public_headers": [], "private_headers": [], "ownership_exclusions": [], "public_include_roots": [], "private_include_roots": ["private-provider"], "state_owner": None, "backend_targets": {"msbuild": ["private-provider.vcxproj"]}, "compile_profile": "project-compile"})
            value["edges"].append({"id": "provider-to-private", "from": "provider", "to": "private-provider", "kind": "implementation", "phases": ["compile"], "visibility": "private", "propagation": "none", "contract_profile": None, "condition": True, "required": False, "witnesses": []})
            manifest.write_text(json.dumps(value), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("consumer", "provider"), graph.closure(("consumer",), "ctx", ("compile",)))
            self.assertEqual(("private-provider", "provider"), graph.closure(("provider",), "ctx", ("compile",)))

    def test_private_link_and_non_propagating_edges_remain_in_final_static_closure_only(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            (root / "private-provider").mkdir()
            (root / "private-provider.vcxproj").touch()
            value["components"].append({"id": "private-provider", "family": "test", "kind": "implementation", "maturity": "candidate", "build_definition": "legacy", "supported_contexts": ["ctx"], "owner": "private-provider", "sources": ["private-provider"], "public_headers": [], "private_headers": [], "ownership_exclusions": [], "public_include_roots": [], "private_include_roots": ["private-provider"], "state_owner": None, "backend_targets": {"msbuild": ["private-provider.vcxproj"]}, "compile_profile": "project-compile"})
            value["edges"].append({"id": "provider-to-private-link", "from": "provider", "to": "private-provider", "kind": "implementation", "phases": ["link"], "visibility": "private", "propagation": "none", "contract_profile": None, "condition": True, "required": False, "witnesses": []})
            manifest.write_text(json.dumps(value), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("consumer", "provider"), graph.closure(("consumer",), "ctx", ("link",)))
            self.assertEqual(("consumer", "private-provider", "provider"), graph.final_link_closure(("consumer",), "ctx"))
            intent = graph.build_intent(("consumer",), "ctx", ("compile", "link"))
            self.assertEqual(["consumer", "private-provider", "provider"], intent["final_link_closure"])

    def test_final_link_provider_projection_keeps_private_archive_on_native_backends(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            private_directory = root / "private-provider"
            private_directory.mkdir()
            private_source = private_directory / "private-provider.cpp"
            private_source.write_text("// private static provider fixture\n", encoding="utf-8")
            value["components"].append({
                "id": "private-provider",
                "family": "test",
                "kind": "implementation",
                "maturity": "candidate",
                "build_definition": "generated",
                "supported_contexts": ["ctx", "cmake-ctx"],
                "owner": "private-provider",
                "sources": ["private-provider/private-provider.cpp"],
                "public_headers": [],
                "private_headers": [],
                "ownership_exclusions": [],
                "public_include_roots": ["private-provider"],
                "private_include_roots": [],
                "state_owner": None,
                "backend_targets": {
                    "msbuild": ["src/main/modules/generated/msbuild/projects/private-provider.vcxproj"],
                    "cmake": ["private-provider"],
                },
                "compile_profile": None,
            })
            value["components"][1]["kind"] = "test"
            value["edges"].append({
                "id": "provider-to-private-provider",
                "from": "provider",
                "to": "private-provider",
                "kind": "implementation",
                "phases": ["link"],
                "visibility": "private",
                "propagation": "none",
                "contract_profile": None,
                "condition": True,
                "required": False,
                "witnesses": [],
            })
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("consumer", "provider"), graph.closure(("consumer",), "ctx", ("link",)))
            self.assertEqual(("consumer", "private-provider", "provider"), graph.final_link_closure(("consumer",), "ctx"))
            generate(graph)
            consumer_msbuild = (root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj").read_text(encoding="utf-8")
            provider_msbuild = (root / "src/main/modules/generated/msbuild/projects/provider.vcxproj").read_text(encoding="utf-8")
            self.assertIn("provider.vcxproj", consumer_msbuild)
            self.assertIn("private-provider.vcxproj", consumer_msbuild)
            self.assertIn("private-provider.vcxproj", provider_msbuild)
            consumer_cmake = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("target_link_libraries(consumer PUBLIC provider)", consumer_cmake)
            self.assertIn("target_link_libraries(provider PRIVATE private-provider)", consumer_cmake)
            self.assertIn("target_link_libraries(consumer PRIVATE private-provider)", consumer_cmake)
            self.assertIn("add_library(private-provider STATIC", consumer_cmake)
            from sakura_build_lib.component_evidence import _expected_link_providers
            self.assertEqual({"provider.lib", "private-provider.lib"}, _expected_link_providers(graph, "consumer", "ctx"))

    def test_msbuild_project_reference_is_conditioned_to_active_context(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_debug_release_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            self.assertEqual(("consumer", "provider"), graph.final_link_closure(("consumer",), "ctx"))
            self.assertEqual(("consumer",), graph.final_link_closure(("consumer",), "ctx-release"))
            generate(graph)

            project_path = root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj"
            project = ET.fromstring(project_path.read_text(encoding="utf-8"))
            references = [item for item in project.iter() if item.tag.rsplit("}", 1)[-1] == "ProjectReference"]
            self.assertEqual(1, len(references))
            self.assertEqual("provider.vcxproj", references[0].attrib["Include"])
            self.assertEqual("'$(Configuration)|$(Platform)'=='Debug|x64'", references[0].attrib["Condition"])

    def test_system_libraries_are_declared_by_owner_and_projected_to_both_backends(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            value["components"][0]["system_libraries"] = ["advapi32"]
            value["components"][1]["kind"] = "test"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")

            graph = load_semantic_graph(root, manifest)
            generate(graph)

            consumer_msbuild = (root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj").read_text(encoding="utf-8")
            self.assertIn("advapi32.lib", consumer_msbuild)
            provider_cmake = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/provider/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("target_link_libraries(provider PUBLIC advapi32)", provider_cmake)
            consumer_cmake = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("target_link_libraries(consumer PUBLIC provider)", consumer_cmake)

    def test_rejects_duplicate_msbuild_physical_context_key(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["contexts"].append({
                "id": "ctx-duplicate",
                "platform": "X64",
                "arch": "x64",
                "configuration": "DEBUG",
                "toolchain": "msvc",
                "backend": "msbuild",
                "role": "other-role",
                "features": [],
            })
            for component in value["components"]:
                component["supported_contexts"].append("ctx-duplicate")
            profiles = compile_profiles_data()
            profiles["context_profiles"].append({
                "id": "context-ctx-duplicate",
                "context_id": "ctx-duplicate",
                "project_profile": "project-compile",
                "link_profile": "link-instrumentation",
            })
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "COMPONENT_MSBUILD_CONTEXT_COLLISION"):
                load_semantic_graph(root, manifest)

    def test_compile_only_edge_does_not_create_native_link_dependency(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            generated_root = root / "src/main/modules/generated/msbuild/projects"
            generated_root.mkdir(parents=True)
            for component in value["components"]:
                component["build_definition"] = "generated"
                component["backend_targets"]["msbuild"] = [
                    f"src/main/modules/generated/msbuild/projects/{component['id']}.vcxproj"
                ]
                (generated_root / f"{component['id']}.vcxproj").touch()
            value["edges"][0]["phases"] = ["compile"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            consumer_project = (generated_root / "consumer.vcxproj").read_text(encoding="utf-8")
            self.assertNotIn("<ProjectReference", consumer_project)
            self.assertIn("$(SakuraRepoRoot)\\provider", consumer_project)

    def test_compile_only_edge_passes_public_include_only_in_both_backends(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            value["edges"][0]["phases"] = ["compile"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            msbuild_project = (root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj").read_text(encoding="utf-8")
            self.assertIn("$(SakuraRepoRoot)\\provider", msbuild_project)
            self.assertNotIn("<ProjectReference", msbuild_project)
            cmake_project = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn('target_include_directories(consumer PRIVATE "${SAKURA_REPO_ROOT}/provider")', cmake_project)
            self.assertNotIn("add_library(provider", cmake_project)
            self.assertNotIn("target_link_libraries(consumer", cmake_project)

    def test_header_only_interface_generates_no_archive_or_compile_target(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            provider = value["components"][0]
            provider["kind"] = "contract"
            provider["sources"] = []
            header = root / "provider/provider.h"
            header.write_text("#pragma once\nstruct ContractValue {};\n", encoding="utf-8")
            provider["public_headers"] = ["provider/provider.h"]
            value["edges"][0]["phases"] = ["compile"]
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            provider_msbuild = (root / "src/main/modules/generated/msbuild/projects/provider.vcxproj").read_text(encoding="utf-8")
            self.assertIn("<ConfigurationType>Utility</ConfigurationType>", provider_msbuild)
            self.assertIn("<ClInclude Include=", provider_msbuild)
            self.assertNotIn("<ClCompile>", provider_msbuild)
            self.assertNotIn("<Lib>", provider_msbuild)
            self.assertNotIn(".lib", provider_msbuild.lower())
            provider_cmake = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/provider/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("add_library(provider INTERFACE)", provider_cmake)
            self.assertIn("target_sources(provider INTERFACE", provider_cmake)
            self.assertNotIn("add_library(provider STATIC", provider_cmake)

    def test_rejects_unknown_propagation(self):
        with RepositoryFixture() as (root, manifest):
            value = manifest_data()
            value["edges"][0]["propagation"] = "free-form"
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "EDGE_PROPAGATION"):
                load_semantic_graph(root, manifest)

    def test_generator_is_deterministic_and_check_detects_staleness(self):
        with RepositoryFixture() as (root, manifest):
            graph = load_semantic_graph(root, manifest)
            first = generate(graph)
            first_mtime = (root / "src/main/modules/generated/manifest.stamp.json").stat().st_mtime_ns
            second = generate(graph)
            self.assertTrue(first)
            self.assertEqual([], second)
            self.assertEqual(first_mtime, (root / "src/main/modules/generated/manifest.stamp.json").stat().st_mtime_ns)
            self.assertEqual([], stale_outputs(graph))
            stamp = root / "src/main/modules/generated/manifest.stamp.json"
            self.assertIn(GENERATOR_VERSION, stamp.read_text(encoding="utf-8"))
            stamp.write_text("stale\n", encoding="utf-8")
            self.assertIn("different:src/main/modules/generated/manifest.stamp.json", stale_outputs(graph))

    def test_component_stale_check_ignores_unrelated_generated_file_but_full_check_finds_it(self):
        with RepositoryFixture() as (root, manifest):
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            unrelated = root / "src/main/modules/generated/unrelated-corruption.txt"
            unrelated.write_text("not part of this component projection\n", encoding="utf-8")
            self.assertIn("unexpected:src/main/modules/generated/unrelated-corruption.txt", stale_outputs(graph))
            self.assertEqual([], stale_component_outputs(graph, "consumer", "ctx"))

    def test_component_stale_check_accepts_git_crlf_checkout(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            project = root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj"
            project.write_bytes(project.read_bytes().replace(b"\n", b"\r\n"))

            self.assertEqual([], stale_outputs(graph))
            self.assertEqual([], stale_component_outputs(graph, "consumer", "ctx"))

    def test_generated_projects_force_include_edge_scoped_abi_stamps(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)

            header = (root / "src/main/modules/generated/abi/ctx/consumer.h").read_text(encoding="utf-8")
            self.assertIn("sakura.edge.consumer-to-provider.default_pack", header)
            self.assertIn("sakura.edge.consumer-to-provider.iterator_debug_level", header)
            project = (root / "src/main/modules/generated/msbuild/projects/consumer.vcxproj").read_text(encoding="utf-8")
            self.assertIn("<StructMemberAlignment>8Bytes</StructMemberAlignment>", project)
            self.assertIn("<ForcedIncludeFiles>", project)
            cmake = (root / "src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("/Zp8", cmake)
            self.assertIn("/FI${SAKURA_REPO_ROOT}/src/main/modules/generated/abi/cmake-ctx/consumer.h", cmake)

    def test_legacy_consumer_projection_replaces_embedded_source_and_rolls_back(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            consumer = next(item for item in value["components"] if item["id"] == "consumer")
            consumer["build_definition"] = "legacy"
            consumer["compile_profile"] = "project-compile"
            consumer["backend_targets"] = {"msbuild": ["projects/consumer.vcxproj"], "cmake": ["consumer"]}
            (root / "projects").mkdir()
            (root / "projects/consumer.vcxproj").write_text(
                '<?xml version="1.0" encoding="utf-8"?>\n'
                '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
                '  <ItemGroup><ClCompile Include="..\\provider\\provider.cpp" /></ItemGroup>\n'
                '</Project>\n',
                encoding="utf-8",
            )
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)

            props_path = root / "src/main/modules/generated/msbuild/consumers/consumer.props"
            props = props_path.read_text(encoding="utf-8")
            self.assertIn('<ClCompile Remove="..\\provider\\provider.cpp"', props)
            self.assertIn("generated\\msbuild\\projects\\provider.vcxproj", props)
            self.assertIn("generated\\abi\\ctx\\consumer.h", props)
            self.assertIn("<SetConfiguration", props)
            self.assertIn(">Configuration=Debug</SetConfiguration>", props)
            self.assertIn("<SetPlatform", props)
            self.assertIn(">Platform=x64</SetPlatform>", props)

            ownership_path = root / "src/main/modules/generated/cmake/legacy/source-ownership.cmake"
            ownership = ownership_path.read_text(encoding="utf-8")
            self.assertIn('list(REMOVE_ITEM SOURCES "${CMAKE_SOURCE_DIR}/provider/provider.cpp")', ownership)
            self.assertIn("add_library(provider STATIC", ownership)
            consumer_cmake_path = root / "src/main/modules/generated/cmake/legacy/consumers/consumer.cmake"
            consumer_cmake = consumer_cmake_path.read_text(encoding="utf-8")
            self.assertIn('target_link_libraries("${SAKURA_LEGACY_CONSUMER_LINK_TARGET}" PRIVATE provider)', consumer_cmake)
            self.assertIn("generated/abi/cmake-ctx/consumer.h", consumer_cmake)
            self.assertNotIn("execution-charset", consumer_cmake)
            self.assertEqual([], stale_component_outputs(graph, "consumer", "ctx"))
            self.assertEqual([], stale_component_outputs(graph, "consumer", "cmake-ctx"))

            value["edges"] = []
            manifest.write_text(json.dumps(value), encoding="utf-8")
            rollback_graph = load_semantic_graph(root, manifest)
            generate(rollback_graph)
            self.assertFalse(props_path.exists())
            self.assertFalse(consumer_cmake_path.exists())
            self.assertFalse(ownership_path.exists())

    def test_component_stale_check_is_backend_and_root_scoped(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)

            unused_cmake_fragment = root / "src/main/modules/generated/cmake/ctx.cmake"
            unused_cmake_fragment.write_text("corrupt unused CMake projection\n", encoding="utf-8")
            self.assertEqual([], stale_component_outputs(graph, "consumer", "ctx"))
            self.assertIn("different:src/main/modules/generated/cmake/ctx.cmake", stale_outputs(graph))
            generate(graph)

            dependency_cmake_project = root / "src/main/modules/generated/cmake/projects/cmake-ctx/provider/CMakeLists.txt"
            dependency_cmake_project.write_text("corrupt non-root CMake project\n", encoding="utf-8")
            self.assertEqual([], stale_component_outputs(graph, "consumer", "cmake-ctx"))
            self.assertIn("different:src/main/modules/generated/cmake/projects/cmake-ctx/provider/CMakeLists.txt", stale_outputs(graph))

            root_cmake_project = root / "src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt"
            root_cmake_project.write_text("corrupt root CMake project\n", encoding="utf-8")
            self.assertIn("different:src/main/modules/generated/cmake/projects/cmake-ctx/consumer/CMakeLists.txt", stale_component_outputs(graph, "consumer", "cmake-ctx"))

    def test_msbuild_component_stale_check_follows_project_reference_closure(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_dual_backend_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)
            provider_project = root / "src/main/modules/generated/msbuild/projects/provider.vcxproj"
            provider_project.write_text("malformed provider project\n", encoding="utf-8")
            self.assertIn(
                "different:src/main/modules/generated/msbuild/projects/provider.vcxproj",
                stale_component_outputs(graph, "consumer", "ctx"),
            )

    def test_msbuild_component_stale_check_follows_only_selected_context_closure(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_debug_release_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)

            provider_project = root / "src/main/modules/generated/msbuild/projects/provider.vcxproj"
            provider_project.write_text("corrupt Debug-only provider project\n", encoding="utf-8")
            provider_stale = "different:src/main/modules/generated/msbuild/projects/provider.vcxproj"

            self.assertIn(provider_stale, stale_component_outputs(graph, "consumer", "ctx"))
            self.assertNotIn(provider_stale, stale_component_outputs(graph, "consumer", "ctx-release"))
            self.assertIn(provider_stale, stale_outputs(graph))

    def test_component_stale_check_does_not_render_expected_projection(self):
        with RepositoryFixture() as (root, manifest):
            value, profiles = generated_debug_release_data(root)
            manifest.write_text(json.dumps(value), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(profiles), encoding="utf-8")
            graph = load_semantic_graph(root, manifest)
            generate(graph)

            with (
                patch.object(build_generator, "expected_outputs", side_effect=AssertionError("full projection rendered")),
                patch.object(build_generator, "_msbuild_project", side_effect=AssertionError("project rendered")),
            ):
                self.assertEqual([], stale_component_outputs(graph, "consumer", "ctx-release"))

    def test_unicode_and_space_repository_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            nested = Path(temporary) / "空 白"
            nested.mkdir()
            (nested / "provider").mkdir()
            (nested / "consumer").mkdir()
            (nested / "provider.vcxproj").touch()
            (nested / "consumer.vcxproj").touch()
            (nested / "tests").mkdir()
            (nested / "tests/contract.cpp").touch()
            manifest = nested / "src/main/modules/modules.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps(manifest_data()), encoding="utf-8")
            (manifest.parent / "compile-profiles.json").write_text(json.dumps(compile_profiles_data()), encoding="utf-8")
            (manifest.parent / "schema-v3.json").write_text("{}\n", encoding="utf-8")
            graph = load_semantic_graph(nested, manifest)
            self.assertTrue(generate(graph))
            self.assertEqual([], stale_outputs(graph))


class Utf16PackagingContractTests(unittest.TestCase):
    def test_distribution_environment_sets_the_production_contract(self):
        self.assertEqual(
            {
                "SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1",
                "SAKURA_UTF16_BACKEND": "rust",
                "SAKURA_UTF16_PRODUCTION_PACKAGE": "true",
            },
            sakura_build.production_package_environment({}),
        )

    def test_production_environment_requires_rust_backend(self):
        for backend in ("rust",):
            with self.subTest(backend=backend):
                self.assertEqual(
                    {
                        "SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1",
                        "SAKURA_UTF16_BACKEND": backend,
                        "SAKURA_UTF16_PRODUCTION_PACKAGE": "true",
                    },
                    sakura_build.production_package_environment(
                        {"SAKURA_UTF16_BACKEND": backend}
                    ),
                )

        for backend in ("cpp", "both", " cpp ", "CPP", "unknown"):
            with self.subTest(backend=backend):
                environment = {"SAKURA_UTF16_BACKEND": backend}
                with self.assertRaisesRegex(
                    BuildError,
                    "SAKURA_UTF16_PRODUCTION_PACKAGE=true requires "
                    "SAKURA_UTF16_BACKEND=rust;",
                ):
                    sakura_build.production_package_environment(environment)
                self.assertEqual(backend, environment["SAKURA_UTF16_BACKEND"])

    def test_batch_packagers_scope_and_reject_both_backend(self):
        repository = TOOLS_BUILD.parents[1]
        for name in ("build-installer.bat", "zipArtifacts.bat"):
            body = (repository / name).read_text(encoding="utf-8")
            body_lower = body.lower()
            setlocal = body_lower.find("setlocal")
            production_flag = body.find('set "SAKURA_UTF16_PRODUCTION_PACKAGE=true"')
            self.assertGreaterEqual(setlocal, 0, name)
            self.assertGreaterEqual(production_flag, 0, name)
            self.assertLess(setlocal, production_flag, name)
            self.assertIn(
                'if not defined SAKURA_UTF16_BACKEND set "SAKURA_UTF16_BACKEND=rust"',
                body,
                name,
            )
            self.assertNotIn('if "%SAKURA_UTF16_BACKEND%" == "both"', body, name)
            self.assertIn(
                "Production packaging requires SAKURA_UTF16_BACKEND=rust;",
                body,
                name,
            )
            self.assertIn("exit /b 1", body, name)

    def test_mingw_environment_and_cmake_command_force_cpp_backend(self):
        environment = mingw_environment({"PATH": "sentinel", "SAKURA_UTF16_BACKEND": "rust"})
        self.assertEqual("cpp", environment["SAKURA_UTF16_BACKEND"])
        with patch("sakura_build_lib.runner.find_cmake_tool", side_effect=lambda name: name):
            with patch("sakura_build_lib.runner.Path.is_file", return_value=True):
                generated = cmake_commands(
                    TOOLS_BUILD.parents[1],
                    "Debug",
                    1,
                    run_tests=False,
                    package_cmake_config=Path("build/pkg/v/a/x64-mingw-static.cmake"),
                )
        self.assertIn("-DSAKURA_UTF16_BACKEND=cpp", generated[0])


class RunnerTests(unittest.TestCase):
    _DISCOVERY_POISON = {
        "cmake_toolchain_file": "ambient-lowercase-toolchain.cmake",
        "CMAKE_PROJECT_TOP_LEVEL_INCLUDES": "ambient-top-level.cmake",
        "CMAKE_PREFIX_PATH": "ambient-prefix",
        "CMAKE_USER_MAKE_RULES_OVERRIDE": "ambient-rules.cmake",
        "CMAKE_USER_MAKE_RULES_OVERRIDE_CXX": "ambient-rules-cxx.cmake",
        "VCPKG_ROOT": "ambient-vcpkg",
        "VCPKG_DEFAULT_TRIPLET": "ambient-default",
        "VCPKG_TARGET_TRIPLET": "ambient-target",
        "VCPKG_FEATURE_FLAGS": "ambient-features",
    }

    def test_jobs_one_sets_explicit_single_compiler_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(root, target, "x64", "Debug", 1, environment={"CMD_MSBUILD": sys.executable})
            self.assertIn("/m:1", command)
            self.assertIn("/p:MultiProcessorCompilation=true", command)
            self.assertIn("/p:CL_MPCount=1", command)

    def test_explicit_listing_phase_sets_the_global_listing_property(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(
                root,
                target,
                "x64",
                "Release",
                1,
                environment={"CMD_MSBUILD": sys.executable},
                assembly_listings=True,
            )
            self.assertIn("/m:1", command)
            self.assertIn("/p:MultiProcessorCompilation=true", command)
            self.assertIn("/p:CL_MPCount=1", command)
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=true", command)

    def test_explicit_listing_phase_overrides_an_ambient_listing_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(
                root,
                target,
                "x64",
                "Release",
                1,
                environment={"CMD_MSBUILD": sys.executable, "SAKURA_GENERATE_ASSEMBLY_LISTINGS": "1"},
                assembly_listings=False,
            )
            self.assertIn("/p:MultiProcessorCompilation=true", command)
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=false", command)

    def test_verification_rebuilds_do_not_write_the_packaged_build_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(root, target, "x64", "Release", 1, environment={"CMD_MSBUILD": sys.executable})
            self.assertNotIn("/fileLogger", command)
            self.assertFalse((root / "build/logs").exists())

    def test_unset_diagnostics_leave_the_command_unchanged(self):
        # Pinned to the exact argv the pre-diagnostics msbuild_command() produced,
        # so a regression that starts adding switches by default fails this test
        # even though it never sets SAKURA_MSBUILD_BINLOG/SAKURA_MSBUILD_PERFORMANCE_SUMMARY.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(root, target, "x64", "Debug", 1, environment={"CMD_MSBUILD": sys.executable})
            self.assertEqual(
                [
                    sys.executable,
                    str(target),
                    "/p:Platform=x64",
                    "/p:Configuration=Debug",
                    "/t:Build",
                    "/nr:false",
                    "/m:1",
                    "/p:MultiProcessorCompilation=true",
                    "/p:CL_MPCount=1",
                ],
                command,
            )

    def test_binlog_env_var_adds_the_binlog_switch_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            binlog_path = str(root / "msbuild.binlog")
            command = msbuild_command(
                root,
                target,
                "x64",
                "Debug",
                1,
                environment={"CMD_MSBUILD": sys.executable, "SAKURA_MSBUILD_BINLOG": binlog_path},
            )
            self.assertEqual(1, command.count(f"/bl:{binlog_path}"))

    def test_performance_summary_env_var_adds_the_switch_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(
                root,
                target,
                "x64",
                "Debug",
                1,
                environment={"CMD_MSBUILD": sys.executable, "SAKURA_MSBUILD_PERFORMANCE_SUMMARY": "1"},
            )
            self.assertEqual(1, command.count("/clp:PerformanceSummary"))

    def test_performance_summary_false_spelling_adds_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            command = msbuild_command(
                root,
                target,
                "x64",
                "Debug",
                1,
                environment={"CMD_MSBUILD": sys.executable, "SAKURA_MSBUILD_PERFORMANCE_SUMMARY": "false"},
            )
            self.assertNotIn("/clp:PerformanceSummary", command)

    def test_both_diagnostics_together_add_both_switches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            binlog_path = str(root / "msbuild.binlog")
            command = msbuild_command(
                root,
                target,
                "x64",
                "Debug",
                1,
                environment={
                    "CMD_MSBUILD": sys.executable,
                    "SAKURA_MSBUILD_BINLOG": binlog_path,
                    "SAKURA_MSBUILD_PERFORMANCE_SUMMARY": "true",
                },
            )
            self.assertIn(f"/bl:{binlog_path}", command)
            self.assertIn("/clp:PerformanceSummary", command)

    def test_invalid_performance_summary_value_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            with self.assertRaises(BuildError) as raised:
                msbuild_command(
                    root,
                    target,
                    "x64",
                    "Debug",
                    1,
                    environment={"CMD_MSBUILD": sys.executable, "SAKURA_MSBUILD_PERFORMANCE_SUMMARY": "yes"},
                )
            self.assertEqual("MSBUILD_PERFORMANCE_SUMMARY_INVALID", raised.exception.code)

    def test_empty_binlog_path_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "project.vcxproj"
            target.touch()
            with self.assertRaises(BuildError) as raised:
                msbuild_command(
                    root,
                    target,
                    "x64",
                    "Debug",
                    1,
                    environment={"CMD_MSBUILD": sys.executable, "SAKURA_MSBUILD_BINLOG": "   "},
                )
            self.assertEqual("MSBUILD_BINLOG_INVALID", raised.exception.code)

    def test_solution_build_writes_the_log_zip_artifacts_requires(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            solution = root / "sakura.sln"
            solution.touch()
            expected = root / "build/logs/msbuild-x64-Release.log"
            command = msbuild_command(
                root,
                solution,
                "x64",
                "Release",
                1,
                environment={"CMD_MSBUILD": sys.executable},
                log_file=msbuild_log_path(root, "x64", "Release"),
            )
            self.assertIn("/fileLogger", command)
            self.assertIn(f"/fileLoggerParameters:LogFile={expected};Verbosity=normal;Encoding=UTF-8", command)
            # MSBuild does not create the directory of ``LogFile`` itself.
            self.assertTrue(expected.parent.is_dir())

    def test_distribution_build_writes_the_log_before_packaging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "sakura.sln").touch()
            with patch("sakura_build_lib.runner.find_msbuild", return_value=sys.executable):
                commands = distribution_commands(root, "x64", "Release", 1)
            expected = root / "build/logs/msbuild-x64-Release.log"
            self.assertIn(f"/fileLoggerParameters:LogFile={expected};Verbosity=normal;Encoding=UTF-8", commands[0])
            self.assertIn("/p:MultiProcessorCompilation=true", commands[0])
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=false", commands[0])
            self.assertEqual(str(root / "sakura_core" / "sakura.vcxproj"), commands[1][1])
            self.assertIn("/m:1", commands[1])
            self.assertIn("/p:MultiProcessorCompilation=true", commands[1])
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=true", commands[1])
            packaging = [index for index, command in enumerate(commands) if any("zipArtifacts.bat" in item for item in command)]
            self.assertEqual([len(commands) - 1], packaging)

    def test_listing_solution_build_finishes_tests_before_product_listings(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "sakura.sln").touch()
            with patch("sakura_build_lib.runner.find_msbuild", return_value=sys.executable):
                commands = solution_commands(
                    root,
                    "x64",
                    "Release",
                    8,
                    log_file=msbuild_log_path(root, "x64", "Release"),
                    assembly_listings=True,
                )
            self.assertEqual(2, len(commands))
            self.assertEqual(str(root / "sakura.sln"), commands[0][1])
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=false", commands[0])
            self.assertEqual(str(root / "sakura_core" / "sakura.vcxproj"), commands[1][1])
            self.assertIn("/p:SAKURA_GENERATE_ASSEMBLY_LISTINGS=true", commands[1])
            # One project at a time keeps this phase away from the tests1
            # relink, which is the reason the phase exists.  Compiling that one
            # project's files one at a time is a separate thing and buys
            # nothing: the shipped listings come from the LTCG pass, which the
            # project serializes with /CGTHREADS:1.  See issue #203.
            self.assertIn("/m:1", commands[1])
            self.assertIn("/p:CL_MPCount=8", commands[1])

    def test_a_phase_may_serialize_projects_without_serializing_the_compiler(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch("sakura_build_lib.runner.find_msbuild", return_value=sys.executable):
                command = msbuild_command(
                    root,
                    root / "sakura_core" / "sakura.vcxproj",
                    "x64",
                    "Release",
                    12,
                    project_parallelism=1,
                )
            self.assertIn("/m:1", command)
            self.assertIn("/p:CL_MPCount=12", command)
            with patch("sakura_build_lib.runner.find_msbuild", return_value=sys.executable):
                with self.assertRaises(BuildError) as raised:
                    msbuild_command(
                        root,
                        root / "sakura_core" / "sakura.vcxproj",
                        "x64",
                        "Release",
                        12,
                        project_parallelism=0,
                    )
            self.assertEqual("JOBS_INVALID", raised.exception.code)

    def test_release_listing_product_link_discards_only_provisional_listings(self):
        project = Path(__file__).resolve().parents[3] / "sakura_core" / "sakura.vcxproj"
        root = ET.parse(project).getroot()
        namespace = root.tag.partition("}")[0] + "}"
        target = next(
            candidate
            for candidate in root.findall(f"{namespace}Target")
            if candidate.attrib.get("Name") == "RemoveSakuraAssemblyListingsBeforeLink"
        )
        self.assertEqual("Link", target.attrib.get("BeforeTargets"))
        self.assertIn("$(SAKURA_GENERATE_ASSEMBLY_LISTINGS)", target.attrib.get("Condition", ""))
        self.assertIn("$(Configuration)'=='Release", target.attrib.get("Condition", ""))
        listing = target.find(f"{namespace}ItemGroup/{namespace}_SakuraAssemblyListing")
        self.assertIsNotNone(listing)
        self.assertEqual("$(IntDir)*.asm", listing.attrib.get("Include"))
        delete = target.find(f"{namespace}Delete")
        self.assertIsNotNone(delete)
        self.assertEqual("@(_SakuraAssemblyListing)", delete.attrib.get("Files"))

    def test_parallel_budget_reaches_the_compiler_undivided(self):
        # Splitting the budget between nodes and compilers starved the compile
        # phase, because tests1 depends on sakura and the two large projects
        # never compile at the same time.  See issue #201 for the measurements.
        for budget in range(1, 33):
            allocation = allocate_parallelism(budget)
            self.assertEqual(budget, allocation.budget)
            self.assertEqual(budget, allocation.projects)
            self.assertEqual(budget, allocation.compiler_processes)

    def test_parallel_budget_rejects_a_nonpositive_job_count(self):
        for budget in (0, -1):
            with self.assertRaises(BuildError) as raised:
                allocate_parallelism(budget)
            self.assertEqual("JOBS_INVALID", raised.exception.code)

    def test_component_cmake_configures_only_when_native_tree_is_not_reusable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "src/main/modules/generated/cmake/projects/ctx/leaf"
            source.mkdir(parents=True)
            (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.24)\n", encoding="utf-8")
            build = cmake_component_build_dir(root, "leaf", "ctx")

            with patch("sakura_build_lib.runner.find_cmake_tool", side_effect=lambda name: name):
                first = cmake_component_commands(root, "leaf", "ctx", "Debug", "msvc", 1)
                self.assertEqual(2, len(first))
                self.assertEqual("-S", first[0][1])

                build.mkdir(parents=True, exist_ok=True)
                (build / "build.ninja").touch()
                (build / "CMakeCache.txt").write_text(
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n"
                    "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                    "CMAKE_BUILD_TYPE:STRING=Debug\n",
                    encoding="utf-8",
                )
                no_op = cmake_component_commands(root, "leaf", "ctx", "Debug", "msvc", 1)
                self.assertEqual(1, len(no_op))
                self.assertEqual("--build", no_op[0][1])

                forced_build_only = cmake_component_commands(
                    root,
                    "leaf",
                    "ctx",
                    "Debug",
                    "msvc",
                    1,
                    configure_if_needed=False,
                )
                self.assertEqual(1, len(forced_build_only))
                self.assertEqual("--build", forced_build_only[0][1])

                compiler = build / "CMakeFiles/3.31.6-msvc6"
                compiler.mkdir(parents=True)
                (compiler / "CMakeCXXCompiler.cmake").write_text(
                    'set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "繝｡繝｢: 繧､繝ｳ繧ｯ繝ｫ繝ｼ繝・繝輔ぃ繧､繫:  ")\n',
                    encoding="utf-8",
                )
                stale_prefix = cmake_component_commands(root, "leaf", "ctx", "Debug", "msvc", 1)
                self.assertEqual(2, len(stale_prefix))
                self.assertEqual("-S", stale_prefix[0][1])

                moved = source.with_name("moved")
                moved.mkdir()
                (moved / "CMakeLists.txt").touch()
                (build / "CMakeCache.txt").write_text(
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={moved}\n"
                    "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                    "CMAKE_BUILD_TYPE:STRING=Debug\n",
                    encoding="utf-8",
                )
                moved_tree = cmake_component_commands(root, "leaf", "ctx", "Debug", "msvc", 1)
                self.assertEqual(2, len(moved_tree))

    def test_component_cmake_build_directory_is_stable_compact_and_context_specific(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            component = "sakura_editor_document_core_tests"
            first = cmake_component_build_dir(root, component, "cmake-msvc-x64-debug")
            repeated = cmake_component_build_dir(root, component, "cmake-msvc-x64-debug")
            release = cmake_component_build_dir(root, component, "cmake-msvc-x64-release")
            legacy = root / "build/components/cmake-msvc-x64-debug" / component / "cmake-ninja-isolated"

            self.assertEqual(first, repeated)
            self.assertNotEqual(first, release)
            self.assertEqual(root / "build/components/cmake", first.parent)
            self.assertLess(len(str(first)), len(str(legacy)))

    def test_component_cmake_environment_removes_discovery_poison_after_toolchain_overlay_case_insensitively(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = SimpleNamespace(returncode=0)
            toolchain_environment = {
                "PATH": "toolchain-path",
                "VSCMD_VER": "17.0",
                "VCPKG_ROOT": "toolchain-inherited-poison",
            }
            with patch.dict(os.environ, self._DISCOVERY_POISON, clear=False):
                with patch("sakura_build_lib.runner.subprocess.run", return_value=completed) as process:
                    result = run_commands(
                        [["cmake", "--build", "component"]],
                        root,
                        dry_run=False,
                        events=EventWriter(),
                        environment=toolchain_environment,
                        isolate_cmake_environment=True,
                    )
            self.assertEqual(0, result)
            child_environment = process.call_args.kwargs["env"]
            self.assertNotIn("cmake_toolchain_file", child_environment)
            for name in COMPONENT_ISOLATED_ENVIRONMENT:
                self.assertNotIn(name, child_environment)
                self.assertNotIn(name.lower(), child_environment)
            self.assertEqual("toolchain-path", child_environment["PATH"])
            self.assertEqual("17.0", child_environment["VSCMD_VER"])
            self.assertEqual("1033", child_environment["VSLANG"])
            self.assertEqual("1", child_environment["MSBUILDDISABLENODEREUSE"])

    def test_legacy_and_full_cmake_environment_keeps_discovery_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completed = SimpleNamespace(returncode=0)
            with patch.dict(os.environ, self._DISCOVERY_POISON, clear=False):
                with patch("sakura_build_lib.runner.subprocess.run", return_value=completed) as process:
                    result = run_commands(
                        [["cmake", "-S", "repo", "-B", "build"]],
                        root,
                        dry_run=False,
                        events=EventWriter(),
                        environment={"PATH": "legacy-path"},
                    )
            self.assertEqual(0, result)
            child_environment = process.call_args.kwargs["env"]
            child_environment_upper = {key.upper(): value for key, value in child_environment.items()}
            for name, value in self._DISCOVERY_POISON.items():
                self.assertEqual(value, child_environment_upper[name.upper()])
            self.assertEqual("legacy-path", child_environment["PATH"])

    def test_child_failure_stops_following_commands_and_preserves_terminal_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            events_stream = io.StringIO()
            failed = SimpleNamespace(returncode=17)
            with patch("sakura_build_lib.runner.subprocess.run", return_value=failed) as process:
                result = run_commands(
                    [["first"], ["must-not-run"]],
                    root,
                    dry_run=False,
                    events=EventWriter(events_stream),
                    failure_exit_code=7,
                )
            self.assertEqual(7, result)
            process.assert_called_once()
            finished = json.loads(events_stream.getvalue().splitlines()[-1])
            self.assertEqual(17, finished["exit_code"])


class TestInventoryTests(unittest.TestCase):
    def test_runtime_verification_requires_exact_selectors_across_split_runners(self):
        tests = parse_gtest_list("Suite.\n  Legacy\n  Leaf\n", "tests1")
        next(item for item in tests if item["runtime"]["selector"] == "Suite.Leaf")["runtime"] = {
            "runner_id": "leaf-tests",
            "selector": "Suite.Leaf",
        }
        inventory = {
            "schema_version": 1,
            "inventory_id": "split",
            "source_revision": "a",
            "source_dirty": True,
            "discovery": {"framework": "googletest", "executable": "old.exe", "arguments": ["--gtest_list_tests"], "executable_sha256": "0" * 64},
            "test_count": 2,
            "disabled_count": 0,
            "guarantee_fingerprint": guarantee_fingerprint(tests),
            "tests": tests,
        }

        def discovered(_path, runner_id, *_args):
            selector = "Suite.Legacy" if runner_id == "tests1" else "Suite.Leaf"
            runtime_tests = [{"test_id": f"tests1:{selector}", "runtime": {"runner_id": runner_id, "selector": selector}, "status": "enabled"}]
            return {"tests": runtime_tests}

        with patch.object(test_inventory_module, "collect_gtest_inventory", side_effect=discovered):
            result = verify_runtime_mappings(
                inventory,
                {"tests1": Path("tests1.exe"), "leaf-tests": Path("leaf.exe")},
                Path.cwd(),
                1,
            )

        self.assertTrue(result["ok"])
        self.assertEqual(2, result["test_count"])

    def test_runtime_verification_reports_missing_and_unexpected_selectors(self):
        tests = parse_gtest_list("Suite.\n  Expected\n", "leaf-tests")
        inventory = {
            "schema_version": 1,
            "inventory_id": "split",
            "source_revision": "a",
            "source_dirty": True,
            "discovery": {"framework": "googletest", "executable": "old.exe", "arguments": ["--gtest_list_tests"], "executable_sha256": "0" * 64},
            "test_count": 1,
            "disabled_count": 0,
            "guarantee_fingerprint": guarantee_fingerprint(tests),
            "tests": tests,
        }
        runtime_tests = [{
            "test_id": "tests1:Suite.Unexpected",
            "runtime": {"runner_id": "leaf-tests", "selector": "Suite.Unexpected"},
            "status": "enabled",
        }]

        with patch.object(test_inventory_module, "collect_gtest_inventory", return_value={"tests": runtime_tests}):
            result = verify_runtime_mappings(
                inventory,
                {"leaf-tests": Path("leaf.exe")},
                Path.cwd(),
                1,
            )

        self.assertFalse(result["ok"])
        self.assertEqual(["Suite.Expected"], result["runners"][0]["missing_selectors"])
        self.assertEqual(["Suite.Unexpected"], result["runners"][0]["unexpected_selectors"])

    def test_runtime_refresh_preserves_ids_requires_remap_and_adds_guarantees(self):
        old_tests = parse_gtest_list("Suite.\n  RenamedOld\n", "tests1")
        inventory = {
            "schema_version": 1,
            "inventory_id": "split",
            "source_revision": "a",
            "source_dirty": False,
            "discovery": {"framework": "googletest", "executable": "old.exe", "arguments": ["--gtest_list_tests"], "executable_sha256": "0" * 64},
            "test_count": 1,
            "disabled_count": 0,
            "guarantee_fingerprint": guarantee_fingerprint(old_tests),
            "tests": old_tests,
        }
        current_tests = parse_gtest_list("Suite.\n  RenamedNew\n  Added\n", "leaf-tests")
        current = {
            "discovery": {"executable": "leaf.exe", "executable_sha256": "1" * 64},
            "tests": current_tests,
        }
        runners = {"leaf-tests": Path("leaf.exe")}

        with patch.object(test_inventory_module, "collect_gtest_inventory", return_value=current):
            with self.assertRaisesRegex(TestInventoryError, "disappeared without explicit remap"):
                refresh_runtime_mappings(inventory, runners, {}, Path.cwd(), "b", True, 1)
            refreshed, report = refresh_runtime_mappings(
                inventory,
                runners,
                {"tests1:Suite.RenamedOld": ("leaf-tests", "Suite.RenamedNew")},
                Path.cwd(),
                "b",
                True,
                1,
            )

        remapped = next(item for item in refreshed["tests"] if item["test_id"] == "tests1:Suite.RenamedOld")
        self.assertEqual("Suite.RenamedNew", remapped["runtime"]["selector"])
        self.assertIn("leaf-tests:Suite.Added", {item["test_id"] for item in refreshed["tests"]})
        self.assertEqual(1, len(report["runtime_remaps"]))
        self.assertEqual(1, len(report["additions"]))

    def test_import_separates_stable_id_from_runtime_selector(self):
        tests = parse_gtest_list(
            "Running main() from repo\\code-main.cpp\n"
            "PlainSuite.\n"
            "  Works\n"
            "Instantiation/ParamSuite.  # TypeParam = int\n"
            "  Value/0  # GetParam() = 42\n"
            "DISABLED_Suite.\n"
            "  Case\n",
            "tests1",
        )
        self.assertEqual(3, len(tests))
        plain = next(item for item in tests if item["test_id"] == "tests1:PlainSuite.Works")
        self.assertEqual({"runner_id": "tests1", "selector": "PlainSuite.Works"}, plain["runtime"])
        self.assertEqual("disabled", tests[0]["status"])

    def test_discovery_zero_tests_is_failure(self):
        with self.assertRaisesRegex(TestInventoryError, "zero tests"):
            parse_gtest_list("Running main() from code-main.cpp\n", "tests1")

    def test_compare_allows_runtime_remap_but_not_guarantee_drift(self):
        tests = parse_gtest_list("Suite.\n  Case\n", "tests1")
        before = {
            "schema_version": 1,
            "inventory_id": "old",
            "source_revision": "a",
            "source_dirty": False,
            "discovery": {"framework": "googletest", "executable": "old.exe", "arguments": ["--gtest_list_tests"], "executable_sha256": "0" * 64},
            "test_count": 1,
            "disabled_count": 0,
            "guarantee_fingerprint": guarantee_fingerprint(tests),
            "tests": tests,
        }
        before = validate_inventory(before)
        after = json.loads(json.dumps(before))
        after["discovery"]["executable"] = "new.exe"
        after["tests"][0]["runtime"] = {"runner_id": "leaf-tests", "selector": "NewSuite.Case"}
        after = validate_inventory(after)
        comparison = compare_inventories(before, after)
        self.assertTrue(comparison["ok"])
        self.assertEqual(1, len(comparison["runtime_remaps"]))

        after["tests"][0]["status"] = "disabled"
        after["disabled_count"] = 1
        after["guarantee_fingerprint"] = guarantee_fingerprint(after["tests"])
        after = validate_inventory(after)
        self.assertFalse(compare_inventories(before, after)["ok"])


if __name__ == "__main__":
    unittest.main()
