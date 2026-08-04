from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.model import (  # noqa: E402
    CompileProfiles,
    Component,
    Context,
    SemanticGraph,
)
from sakura_build_lib.repository_inventory import (  # noqa: E402
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
        'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/version.h" COMMAND generator DEPENDS version.h.in)\n'
        'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/manifest.rc" COMMAND generator)\n'
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
            [
                {
                    "backend": "cmake",
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
