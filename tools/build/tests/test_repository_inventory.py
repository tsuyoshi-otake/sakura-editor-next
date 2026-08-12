from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.model import (  # noqa: E402
    CompileProfiles,
    Component,
    Context,
    Edge,
    Artifact,
    SemanticGraph,
)
from sakura_build_lib.repository_inventory import (  # noqa: E402
    _component_owner,
    _include_dependency_observed,
    _source_owner,
    collect_repository_inventory,
    repository_inventory_summary,
    write_repository_inventory,
)
from sakura_build_lib.runner import BuildError  # noqa: E402


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _fixture(root: Path) -> SemanticGraph:
    _write(
        root / "app/main.cpp",
        '#include "../provider/provider.h"\n'
        '#include "../shared/unowned.h"\n'
        '#include "version.h"\n'
        '#include "generated_missing.h"\n'
        '#pragma comment(lib, "hidden.lib")\n',
    )
    _write(root / "app/sakura_rc.h", "#define IDS_APP 100\n")
    _write(root / "app/app.rc", '#include "sakura_rc.h"\nSTRINGTABLE BEGIN IDS_APP "app" END\n')
    _write(root / "tests/tests.rc", '#include "../app/sakura_rc.h"\n')
    _write(root / "provider/provider.cpp", '#include "provider.h"\n')
    _write(root / "provider/provider.h", "#pragma once\n")
    _write(root / "shared/unowned.h", "#pragma once\n")
    _write(root / "src/main/unowned.cpp", "int unowned_source = 0;\n")
    _write(
        root / "app.vcxproj",
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
        '  <ItemGroup>\n'
        '    <ClCompile Include="app\\main.cpp" />\n'
        '    <ClCompile Include="provider\\provider.cpp" />\n'
        '    <ClInclude Include="build\\$(Platform)\\version.h" />\n'
        '    <ResourceCompile Include="app\\app.rc" />\n'
        '  </ItemGroup>\n'
        '  <ItemDefinitionGroup><Link><AdditionalDependencies>$(FmtLibrary);kernel32.lib;%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup>\n'
        '  <Target Name="GenerateVersionHeader" Inputs="version.h.in" Outputs="build\\$(Platform)\\version.h" BeforeTargets="ClCompile" />\n'
        '</Project>\n',
    )
    _write(
        root / "tests.vcxproj",
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
        '  <ItemGroup>\n'
        '    <ResourceCompile Include="tests\\tests.rc" />\n'
        '    <ProjectReference Include="app.vcxproj" />\n'
        '  </ItemGroup>\n'
        '  <Target Name="CollectSakuraObjectsForTests1" BeforeTargets="Link">\n'
        '    <ReadLinesFromFile File="$(SakuraLinkInputsForTests1File)"><Output TaskParameter="Lines" ItemName="_SakuraLinkInputForTests1" /></ReadLinesFromFile>\n'
        '    <ItemGroup><Link Include="@(_SakuraLinkInputForTests1)" /></ItemGroup>\n'
        '  </Target>\n'
        '</Project>\n',
    )
    _write(
        root / "src/main/modules/generated/msbuild/projects/provider.vcxproj",
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003" />\n',
    )
    _write(
        root / "src/main/cmake/product.cmake",
        'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/version.h" BYPRODUCTS "${CMAKE_BINARY_DIR}/version.trace" COMMAND generator DEPENDS version.h.in)\n'
        'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/manifest.rc" COMMAND generator)\n'
        'add_custom_target(generate_runtime COMMAND copier BYPRODUCTS "${CMAKE_BINARY_DIR}/runtime.dll" DEPENDS runtime-source.dll)\n'
        "file(GLOB_RECURSE SOURCES app/*.cpp)\n",
    )
    _write(root / "vcpkg.json", json.dumps({"dependencies": ["fmt"]}) + "\n")
    _write(
        root / "Directory.Build.props",
        '<Project><PropertyGroup><VcpkgEnableManifest>true</VcpkgEnableManifest><VcpkgManifestRoot>$(MSBuildThisFileDirectory)</VcpkgManifestRoot><VcpkgInstalledDir>$(MSBuildThisFileDirectory)build</VcpkgInstalledDir></PropertyGroup><Import Project="$(VcpkgRoot)scripts\\buildsystems\\msbuild\\vcpkg.props" /></Project>\n',
    )
    _write(
        root / "Directory.Build.targets",
        '<Project><Import Project="$(VcpkgRoot)scripts\\buildsystems\\msbuild\\vcpkg.targets" /></Project>\n',
    )
    _write(
        root / "tools/vcpkg/scripts/buildsystems/msbuild/vcpkg.targets",
        '<Project><Target Name="VcpkgManifestInstall" BeforeTargets="ClCompile" Condition="$(VcpkgEnableManifest)"><Exec Command="vcpkg install" /></Target></Project>\n',
    )

    context = Context("msvc-x64-debug", "x64", "x64", "Debug", "msvc", "msbuild", "development", ())
    product = Component(
        "product",
        "legacy",
        "executable",
        "legacy",
        "legacy",
        (context.id,),
        "app",
        ("app/main.cpp",),
        (),
        (),
        (),
        (),
        (".",),
        "legacy application state",
        {"msbuild": ("app.vcxproj",)},
        None,
    )
    provider = Component(
        "provider",
        "platform",
        "implementation",
        "candidate",
        "generated",
        (context.id,),
        "provider",
        ("provider/provider.cpp",),
        ("provider/provider.h",),
        (),
        (),
        ("provider",),
        (),
        None,
        {"msbuild": ("src/main/modules/generated/msbuild/projects/provider.vcxproj",)},
        None,
    )
    tests = Component(
        "tests",
        "legacy-tests",
        "test",
        "legacy",
        "legacy",
        (context.id,),
        "tests",
        (),
        (),
        (),
        (),
        (),
        (".",),
        "legacy test process state",
        {"msbuild": ("tests.vcxproj",)},
        None,
    )
    profiles = CompileProfiles(1, {}, {}, {}, {})
    return SemanticGraph(
        root.resolve(),
        root / "src/main/modules/modules.json",
        3,
        "0.3.4",
        {context.id: context},
        {product.id: product, provider.id: provider, tests.id: tests},
        {},
        {},
        profiles,
        (),
        "sha256:test-graph",
    )


class RepositoryInventoryTests(unittest.TestCase):
    def test_generated_leaf_does_not_claim_its_owner_directory(self) -> None:
        """Generated leaves own declared inputs, not every sibling below owner."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "provider/legacy.cpp", "int legacy_provider = 0;\n")

            self.assertEqual("provider", _component_owner(graph, "provider/provider.cpp"))
            self.assertEqual("provider", _component_owner(graph, "provider/provider.h"))
            self.assertIsNone(_component_owner(graph, "provider/legacy.cpp"))

    def test_declared_artifact_paths_are_owned_without_becoming_components(self) -> None:
        """Generated/tool inputs remain graph-owned but never impersonate code leaves."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "generated/abi/provider.h", "#pragma once\n")
            graph = replace(
                graph,
                artifacts={
                    "generated-abi": Artifact(
                        "generated-abi",
                        "product",
                        "generated",
                        ("src/main/modules/modules.json",),
                        ("generated/abi",),
                        "sakura-module-generator",
                        True,
                    ),
                },
            )

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

            self.assertEqual("generated-abi", _source_owner(graph, "generated/abi/provider.h"))
            self.assertNotIn("generated/abi/provider.h", inventory["source"]["unowned_repo_files"])

    def test_resource_id_contract_requires_declared_compile_edges(self) -> None:
        """A shared resource ID table is valid only through its declared artifact."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "src/main/resources/sakura_rc.h", "#define IDS_APP 100\n")
            _write(graph.repo_root / "app/app.rc", '#include "../src/main/resources/sakura_rc.h"\nSTRINGTABLE BEGIN IDS_APP "app" END\n')
            _write(graph.repo_root / "tests/tests.rc", '#include "../src/main/resources/sakura_rc.h"\n')
            graph = replace(
                graph,
                artifacts={
                    "resource-id-contract": Artifact(
                        "resource-id-contract",
                        "product",
                        "resource",
                        ("src/main/resources/sakura_rc.h",),
                        (),
                        None,
                        True,
                    ),
                },
                edges=(
                    Edge(
                        "product-to-resource-id-contract",
                        "product",
                        "resource-id-contract",
                        "asset",
                        ("compile",),
                        "private",
                        "none",
                        None,
                        True,
                        True,
                        (),
                    ),
                    Edge(
                        "tests-to-resource-id-contract",
                        "tests",
                        "resource-id-contract",
                        "asset",
                        ("compile",),
                        "private",
                        "none",
                        None,
                        True,
                        True,
                        (),
                    ),
                ),
            )

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        resources = inventory["resource_provenance"]
        codes = {finding["code"] for finding in inventory["findings"]}
        self.assertEqual(["resource-id-contract"], resources["canonical_sakura_rc_header_contracts"])
        self.assertEqual([], resources["canonical_sakura_rc_header_contract_edges_missing"])
        self.assertNotIn("SHARED_RESOURCE_ID_HEADER_COUPLING", codes)
        self.assertNotIn("RESOURCE_ID_CONTRACT_EDGE_MISSING", codes)

    def test_local_literal_library_property_is_not_an_unresolved_link_expression(self) -> None:
        """Debug/Release archive names remain observable without a fake property parser."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(
                graph.repo_root / "app.vcxproj",
                '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
                '  <PropertyGroup Condition="\'$(Configuration)\' == \'Debug\'"><FmtLibrary>fmtd.lib</FmtLibrary></PropertyGroup>\n'
                '  <PropertyGroup Condition="\'$(Configuration)\' == \'Release\'"><FmtLibrary>fmt.lib</FmtLibrary></PropertyGroup>\n'
                '  <ItemDefinitionGroup><Link><AdditionalDependencies>$(FmtLibrary);kernel32.lib;%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup>\n'
                '</Project>\n',
            )

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        link_record = inventory["product_link_provenance"]["msbuild_additional_dependencies"][0]
        codes = {finding["code"] for finding in inventory["findings"]}
        self.assertEqual({"$(FmtLibrary)": ["fmt.lib", "fmtd.lib"]}, link_record["resolved_expression_libraries"])
        self.assertEqual([], link_record["unresolved_expression_tokens"])
        self.assertNotIn("LINK_DEPENDENCY_EXPRESSION_UNRESOLVED", codes)

    def test_declared_vcpkg_package_prefix_classifies_external_include(self) -> None:
        """Only an explicit package-set output may classify a missing vendor include."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "app/main.cpp", '#include "cmigemo/migemo.h"\n')
            graph = replace(
                graph,
                artifacts={
                    "vcpkg-root-package-set": Artifact(
                        "vcpkg-root-package-set",
                        "product",
                        "package_set",
                        ("vcpkg.json",),
                        ("vcpkg:cmigemo",),
                        "vcpkg",
                        True,
                    ),
                },
            )

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        generated = inventory["generated_provenance"]
        classified = generated["classified_unresolved_generated_includes"]
        self.assertEqual("cmigemo/migemo.h", classified[0]["include"])
        self.assertEqual("vcpkg-root-package-set", classified[0]["declared_outputs"][0]["artifact"])
        self.assertEqual([], generated["unclassified_unresolved_quoted_includes"])

    def test_declared_generated_include_does_not_leave_include_coverage_partial(self) -> None:
        """Declared generated headers belong to provenance, not unresolved include debt."""

        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "app/main.cpp", '#include "version.h"\n')
            graph = replace(
                graph,
                artifacts={
                    "generated-version": Artifact(
                        "generated-version",
                        "product",
                        "generated",
                        ("version.h.in",),
                        ("version.h",),
                        "generator",
                        True,
                    ),
                },
            )
            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        generated = inventory["generated_provenance"]
        self.assertEqual([], generated["unclassified_unresolved_quoted_includes"])
        self.assertTrue(
            _include_dependency_observed(
                {
                    "unowned_repo_includes": [],
                    "unresolved_quoted_includes": inventory["source"]["unresolved_quoted_includes"],
                },
                generated,
                {"compiler_dependency_observed": True},
            )
        )
        self.assertFalse(
            _include_dependency_observed(
                {"unowned_repo_includes": [], "unresolved_quoted_includes": []},
                {"unclassified_unresolved_quoted_includes": [{"include": "missing.h"}]},
                {"compiler_dependency_observed": True},
            )
        )
        self.assertEqual("version.h", generated["classified_unresolved_generated_includes"][0]["include"])

    def test_adversarial_inventory_exposes_embedded_and_hidden_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        codes = {finding["code"] for finding in inventory["findings"]}
        self.assertTrue(inventory["collection_ok"])
        self.assertFalse(inventory["graduation_ready"])
        self.assertEqual("partial", {item["class"]: item["status"] for item in inventory["coverage"]}["include"])
        self.assertEqual("embedded_in_product", inventory["product_reachability"]["status"])
        self.assertFalse(inventory["product_reachability"]["independent"])
        self.assertIn("UNDECLARED_INCLUDE_EDGE", codes)
        self.assertIn("UNOWNED_REPO_FILE_SET", codes)
        self.assertIn("UNOWNED_REPO_INCLUDE", codes)
        self.assertIn("UNRESOLVED_QUOTED_INCLUDE_SET", codes)
        self.assertIn("SOURCE_LINK_DIRECTIVE", codes)
        self.assertIn("PRODUCT_EMBEDS_PROVIDER_SOURCE", codes)
        self.assertIn("PRODUCT_PROVIDER_EDGE_MISSING", codes)
        self.assertIn("CMAKE_SOURCE_OWNERSHIP_OPAQUE", codes)
        self.assertIn("GENERATOR_DECLARED_INPUT_GAP", codes)
        self.assertIn("ROOT_PACKAGE_SET_UNCLASSIFIED", codes)
        self.assertIn("NATIVE_GENERATOR_EXECUTION_UNOBSERVED", codes)
        self.assertIn("NATIVE_PRODUCT_EVIDENCE_NOT_PROVIDED", codes)
        self.assertIn("SHARED_RESOURCE_ID_HEADER_COUPLING", codes)
        self.assertIn("LINK_DEPENDENCY_EXPRESSION_UNRESOLVED", codes)
        self.assertIn("TEST_LINKS_PRODUCT_OBJECTS", codes)
        self.assertIn("GLOBAL_VCPKG_RESTORE_SCOPE", codes)
        self.assertEqual("shared/unowned.h", inventory["source"]["unowned_repo_includes"][0]["target"])
        self.assertEqual(1, len(inventory["generated_provenance"]["classified_unresolved_generated_includes"]))
        self.assertEqual("version.h", inventory["generated_provenance"]["classified_unresolved_generated_includes"][0]["include"])
        self.assertTrue(inventory["generated_provenance"]["msbuild_generated_header_items"][0]["contains_expression"])
        self.assertEqual(
            ["${CMAKE_BINARY_DIR}/version.h"],
            inventory["generated_provenance"]["cmake_custom_commands"][0]["outputs"],
        )
        self.assertEqual(
            ["${CMAKE_BINARY_DIR}/version.trace"],
            inventory["generated_provenance"]["cmake_custom_commands"][0]["byproducts"],
        )
        runtime_targets = [
            item
            for item in inventory["generated_provenance"]["cmake_custom_commands"]
            if item.get("target") == "generate_runtime"
        ]
        self.assertEqual(1, len(runtime_targets))
        self.assertEqual("custom-target", runtime_targets[0]["kind"])
        self.assertEqual(
            ["${CMAKE_BINARY_DIR}/runtime.dll"], runtime_targets[0]["byproducts"]
        )
        self.assertEqual(["runtime-source.dll"], runtime_targets[0]["inputs"])
        self.assertEqual(
            [
                {
                    "backend": "cmake",
                    "byproducts": [],
                    "ordinal": 2,
                    "outputs": ["${CMAKE_BINARY_DIR}/manifest.rc"],
                    "source": "src/main/cmake/product.cmake",
                }
            ],
            inventory["generated_provenance"]["declared_input_gaps"],
        )
        self.assertEqual(2, len(inventory["resource_provenance"]["canonical_sakura_rc_header_consumers"]))
        self.assertEqual(1, len(inventory["product_link_provenance"]["test_product_object_aggregation"]))
        self.assertTrue(inventory["package_provenance"]["global_restore_scope"])
        self.assertFalse(inventory["package_provenance"]["component_package_isolation_verified"])

    def test_product_provider_projection_is_reported_as_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(
                graph.repo_root / "app.vcxproj",
                '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
                '  <ItemGroup><ClCompile Include="app\\main.cpp" /><ClCompile Include="provider\\provider.cpp" /></ItemGroup>\n'
                '  <Import Project="src\\main\\modules\\generated\\msbuild\\consumers\\product.props" Condition="Exists(\'src\\main\\modules\\generated\\msbuild\\consumers\\product.props\')" />\n'
                '</Project>\n',
            )
            _write(
                graph.repo_root / "src/main/modules/generated/msbuild/consumers/product.props",
                '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
                '  <ItemGroup>\n'
                '    <ClCompile Remove="provider\\provider.cpp" />\n'
                '    <ProjectReference Include="$(MSBuildProjectDirectory)\\src\\main\\modules\\generated\\msbuild\\projects\\provider.vcxproj" />\n'
                '  </ItemGroup>\n'
                '</Project>\n',
            )
            _write(
                graph.repo_root / "src/main/cmake/product.cmake",
                'file(GLOB_RECURSE SOURCES app/*.cpp provider/*.cpp)\n'
                'include("${CMAKE_SOURCE_DIR}/src/main/modules/generated/cmake/legacy/source-ownership.cmake" OPTIONAL)\n'
                'include("${CMAKE_SOURCE_DIR}/src/main/modules/generated/cmake/legacy/consumers/product.cmake" OPTIONAL)\n',
            )
            _write(
                graph.repo_root / "src/main/modules/generated/cmake/legacy/source-ownership.cmake",
                'list(REMOVE_ITEM SOURCES "${CMAKE_SOURCE_DIR}/provider/provider.cpp")\n'
                'add_library(provider STATIC "${CMAKE_SOURCE_DIR}/provider/provider.cpp")\n',
            )
            _write(
                graph.repo_root / "src/main/modules/generated/cmake/legacy/consumers/product.cmake",
                'target_link_libraries("${SAKURA_LEGACY_CONSUMER_LINK_TARGET}" PRIVATE provider)\n',
            )
            graph = replace(
                graph,
                edges=(
                    Edge(
                        "product-to-provider",
                        "product",
                        "provider",
                        "implementation",
                        ("compile", "link"),
                        "private",
                        "none",
                        None,
                        True,
                        True,
                        (),
                    ),
                ),
            )

            inventory = collect_repository_inventory(
                graph,
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        reachability = inventory["product_reachability"]
        self.assertEqual("independent_provider", reachability["status"])
        self.assertTrue(reachability["independent"])
        self.assertTrue(reachability["msbuild"]["consumer_projection_imported"])
        self.assertEqual(["provider/provider.cpp"], reachability["msbuild"]["provider_sources_removed_by_projection"])
        self.assertEqual([], reachability["msbuild"]["provider_sources_compiled_directly"])
        self.assertTrue(reachability["cmake"]["source_ownership_verified"])
        codes = {finding["code"] for finding in inventory["findings"]}
        self.assertNotIn("PRODUCT_EMBEDS_PROVIDER_SOURCE", codes)
        self.assertNotIn("PRODUCT_PROVIDER_EDGE_MISSING", codes)
        self.assertNotIn("CMAKE_SOURCE_OWNERSHIP_OPAQUE", codes)

    def test_hard_evidence_hash_is_checkout_independent(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            first_inventory = collect_repository_inventory(
                _fixture(Path(first)),
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )
            second_inventory = collect_repository_inventory(
                _fixture(Path(second)),
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )

        self.assertEqual(first_inventory["hard_evidence_hash"], second_inventory["hard_evidence_hash"])

    def test_malformed_package_manifest_has_typed_terminal_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "vcpkg.json", "{not-json\n")

            with self.assertRaises(BuildError) as caught:
                collect_repository_inventory(
                    graph,
                    product_id="product",
                    provider_id="provider",
                    context_id="msvc-x64-debug",
                )

        self.assertEqual("INVENTORY_PACKAGE_MANIFEST", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)

    def test_malformed_msbuild_definition_has_typed_terminal_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            graph = _fixture(Path(temporary))
            _write(graph.repo_root / "Directory.Build.props", "<Project><broken></Project>\n")

            with self.assertRaises(BuildError) as caught:
                collect_repository_inventory(
                    graph,
                    product_id="product",
                    provider_id="provider",
                    context_id="msvc-x64-debug",
                )

        self.assertEqual("INVENTORY_MSBUILD_PARSE", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)

    def test_writer_preserves_unchanged_file_and_summary_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inventory = collect_repository_inventory(
                _fixture(root),
                product_id="product",
                provider_id="provider",
                context_id="msvc-x64-debug",
            )
            output = root / "evidence/inventory.json"
            write_repository_inventory(output, inventory)
            old_time = 1_700_000_000_000_000_000
            os.utime(output, ns=(old_time, old_time))

            write_repository_inventory(output, inventory)
            summary = repository_inventory_summary(inventory, output_path=output)

            self.assertEqual(old_time, output.stat().st_mtime_ns)
            self.assertNotIn("source", summary)
            self.assertNotIn("findings", summary)
            self.assertEqual(len(inventory["findings"]), summary["finding_count"])
            self.assertEqual("embedded_in_product", summary["product_reachability"]["status"])
            self.assertEqual(1, summary["generated_provenance"]["classified_include_count"])
            self.assertEqual(1, summary["product_link_provenance"]["test_product_object_aggregation_count"])
            self.assertTrue(summary["package_provenance"]["global_restore_scope"])


if __name__ == "__main__":
    unittest.main()
