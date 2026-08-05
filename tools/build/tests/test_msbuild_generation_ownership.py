from __future__ import annotations

import json
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.product_native_evidence import _native_observation_command  # noqa: E402
from sakura_build_lib.model import load_semantic_graph  # noqa: E402


MSBUILD_NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"


class MsbuildGenerationOwnershipTests(unittest.TestCase):
    def test_uri_tests_are_owned_only_by_the_split_runner(self) -> None:
        removed_source = "UriIdentityTest.cpp"
        self.assertFalse(REPO_ROOT.joinpath("src/test/cpp/tests1/platform", removed_source).exists())
        for relative in (
            "sakura_core/tests1.vcxproj",
            "sakura_core/tests1.vcxproj.filters",
            "src/test/cmake/tests1.cmake",
        ):
            with self.subTest(projection=relative):
                self.assertNotIn(removed_source, (REPO_ROOT / relative).read_text(encoding="utf-8-sig"))

        inventory = json.loads((REPO_ROOT / "src/test/test-inventory.json").read_text(encoding="utf-8"))
        uri_tests = [
            item for item in inventory["tests"]
            if item["test_id"].startswith("tests1:UriIdentity.")
        ]
        self.assertEqual(9, len(uri_tests))
        self.assertEqual({"sakura_uri_tests"}, {item["runtime"]["runner_id"] for item in uri_tests})

    def test_serialization_tests_are_owned_only_by_the_split_runner(self) -> None:
        removed_source = "JsoncDocumentTest.cpp"
        self.assertFalse(REPO_ROOT.joinpath("src/test/cpp/tests1/platform", removed_source).exists())
        for relative in (
            "sakura_core/tests1.vcxproj",
            "sakura_core/tests1.vcxproj.filters",
            "src/test/cmake/tests1.cmake",
        ):
            with self.subTest(projection=relative):
                self.assertNotIn(removed_source, (REPO_ROOT / relative).read_text(encoding="utf-8-sig"))

        inventory = json.loads((REPO_ROOT / "src/test/test-inventory.json").read_text(encoding="utf-8"))
        serialization_tests = [
            item for item in inventory["tests"]
            if item["test_id"].startswith("tests1:JsoncDocument.")
        ]
        self.assertEqual(4, len(serialization_tests))
        self.assertEqual(
            {"sakura_serialization_tests"},
            {item["runtime"]["runner_id"] for item in serialization_tests},
        )

    def test_filesystem_tests_are_owned_only_by_the_split_runner(self) -> None:
        removed_sources = ("FileServiceTest.cpp", "Win32FileSystemProviderTest.cpp")
        for removed_source in removed_sources:
            self.assertFalse(REPO_ROOT.joinpath("src/test/cpp/tests1/platform", removed_source).exists())
        for relative in (
            "sakura_core/tests1.vcxproj",
            "sakura_core/tests1.vcxproj.filters",
            "src/test/cmake/tests1.cmake",
        ):
            with self.subTest(projection=relative):
                projection = (REPO_ROOT / relative).read_text(encoding="utf-8-sig")
                for removed_source in removed_sources:
                    self.assertNotIn(removed_source, projection)

        inventory = json.loads((REPO_ROOT / "src/test/test-inventory.json").read_text(encoding="utf-8"))
        filesystem_tests = [
            item
            for item in inventory["tests"]
            if item["test_id"].startswith(("tests1:FileService.", "tests1:Win32FileSystemProvider."))
        ]
        self.assertEqual(13, len(filesystem_tests))
        self.assertEqual(
            {"sakura_filesystem_tests"},
            {item["runtime"]["runner_id"] for item in filesystem_tests},
        )

    def test_filesystem_consumer_projects_receive_only_the_narrow_public_include_root(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        filesystem = graph.components["sakura_filesystem"]
        self.assertEqual(("sakura_core/include",), filesystem.public_include_roots)
        self.assertEqual(("sakura_core/platform/filesystem",), filesystem.private_include_roots)

        provider_text = (
            REPO_ROOT / "src/main/modules/generated/msbuild/projects/sakura_filesystem.vcxproj"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\include", provider_text)
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\platform\filesystem", provider_text)

        for relative in (
            "src/main/modules/generated/msbuild/projects/sakura_filesystem_tests.vcxproj",
            "src/main/modules/generated/msbuild/consumers/sakura_app.props",
            "src/main/modules/generated/msbuild/consumers/tests1.props",
        ):
            with self.subTest(projection=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                include_directories = ";".join(
                    node.text or ""
                    for node in project.findall(f".//{MSBUILD_NS}AdditionalIncludeDirectories")
                )
                self.assertIn("include", include_directories)
                self.assertNotIn(r"platform\filesystem", include_directories)

        cmake_test = (
            REPO_ROOT
            / "src/main/modules/generated/cmake/projects/cmake-msvc-x64-debug/sakura_filesystem_tests/CMakeLists.txt"
        ).read_text(encoding="utf-8-sig")
        test_section = cmake_test.split("add_executable(sakura_filesystem_tests", 1)[-1]
        self.assertIn("sakura_core/include", test_section)
        self.assertNotIn("sakura_core/platform/filesystem", test_section)

    def test_storage_type_contract_tests_are_owned_only_by_the_split_runner(self) -> None:
        legacy_test = REPO_ROOT / "src/test/cpp/tests1/platform/StorageServiceTest.cpp"
        self.assertNotIn("TEST(StorageAddress,", legacy_test.read_text(encoding="utf-8-sig"))

        inventory = json.loads((REPO_ROOT / "src/test/test-inventory.json").read_text(encoding="utf-8"))
        storage_tests = [
            item
            for item in inventory["tests"]
            if item["test_id"].startswith("tests1:StorageAddress.")
        ]
        self.assertEqual(2, len(storage_tests))
        self.assertEqual(
            {"sakura_storage_tests"},
            {item["runtime"]["runner_id"] for item in storage_tests},
        )

    def test_storage_consumer_projects_receive_only_the_public_contract_root(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        storage = graph.components["sakura_storage"]
        self.assertEqual(("sakura_core/include",), storage.public_include_roots)
        self.assertEqual(("sakura_core/platform/storage",), storage.private_include_roots)

        provider_text = (
            REPO_ROOT / "src/main/modules/generated/msbuild/projects/sakura_storage.vcxproj"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\include", provider_text)
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\platform\storage", provider_text)

        for relative in (
            "src/main/modules/generated/msbuild/projects/sakura_storage_tests.vcxproj",
            "src/main/modules/generated/msbuild/consumers/sakura_app.props",
            "src/main/modules/generated/msbuild/consumers/tests1.props",
        ):
            with self.subTest(projection=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                include_directories = ";".join(
                    node.text or ""
                    for node in project.findall(f".//{MSBUILD_NS}AdditionalIncludeDirectories")
                )
                self.assertIn("include", include_directories)
                self.assertNotIn(r"platform\storage", include_directories)

        cmake_test = (
            REPO_ROOT
            / "src/main/modules/generated/cmake/projects/cmake-msvc-x64-debug/sakura_storage_tests/CMakeLists.txt"
        ).read_text(encoding="utf-8-sig")
        test_section = cmake_test.split("add_executable(sakura_storage_tests", 1)[-1]
        self.assertIn("sakura_core/include", test_section)
        self.assertNotIn("sakura_core/platform/storage", test_section)

    def test_serialization_consumer_projects_receive_only_the_narrow_public_include_root(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        serialization = graph.components["sakura_serialization"]
        self.assertEqual(("sakura_core/include",), serialization.public_include_roots)
        self.assertEqual((), serialization.private_include_roots)

        provider_text = (
            REPO_ROOT / "src/main/modules/generated/msbuild/projects/sakura_serialization.vcxproj"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\include", provider_text)
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\platform\serialization", provider_text)

        for relative in (
            "src/main/modules/generated/msbuild/projects/sakura_serialization_tests.vcxproj",
            "src/main/modules/generated/msbuild/consumers/sakura_app.props",
            "src/main/modules/generated/msbuild/consumers/tests1.props",
        ):
            with self.subTest(projection=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                include_directories = ";".join(
                    node.text or ""
                    for node in project.findall(f".//{MSBUILD_NS}AdditionalIncludeDirectories")
                )
                self.assertIn("include", include_directories)
                self.assertNotIn(r"platform\serialization", include_directories)

        cmake_test = (
            REPO_ROOT
            / "src/main/modules/generated/cmake/projects/cmake-msvc-x64-debug/sakura_serialization_tests/CMakeLists.txt"
        ).read_text(encoding="utf-8-sig")
        test_section = cmake_test.split("add_executable(sakura_serialization_tests", 1)[-1]
        self.assertIn("sakura_core/include", test_section)
        self.assertNotIn("sakura_core/platform/serialization", test_section)

    def test_uri_consumer_projects_receive_only_the_narrow_public_include_root(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        uri = graph.components["sakura_uri"]
        self.assertEqual(("sakura_core/include",), uri.public_include_roots)
        self.assertEqual(("sakura_core/platform/uri",), uri.private_include_roots)

        provider_text = (
            REPO_ROOT / "src/main/modules/generated/msbuild/projects/sakura_uri.vcxproj"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\include", provider_text)
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\platform\uri", provider_text)

        for relative in (
            "src/main/modules/generated/msbuild/projects/sakura_uri_tests.vcxproj",
            "src/main/modules/generated/msbuild/consumers/sakura_app.props",
            "src/main/modules/generated/msbuild/consumers/tests1.props",
        ):
            with self.subTest(projection=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                include_directories = ";".join(
                    node.text or ""
                    for node in project.findall(f".//{MSBUILD_NS}AdditionalIncludeDirectories")
                )
                self.assertIn("include", include_directories)
                self.assertNotIn(r"platform\uri", include_directories)

        cmake_test = (
            REPO_ROOT
            / "src/main/modules/generated/cmake/projects/cmake-msvc-x64-debug/sakura_uri_tests/CMakeLists.txt"
        ).read_text(encoding="utf-8-sig")
        test_section = cmake_test.split("add_executable(sakura_uri_tests", 1)[-1]
        self.assertIn("sakura_core/include", test_section)
        self.assertNotIn("sakura_core/platform/uri", test_section)

    def test_request_contract_runner_and_private_boundary(self) -> None:
        for private_header in (
            "RequestService.h",
            "win32/WinHttpRequestRuntime.h",
            "win32/WinHttpSystemProxyResolver.h",
        ):
            self.assertFalse((REPO_ROOT / "sakura_core/platform/request" / private_header).exists())

        for public_header in (
            "sakura_core/include/sakura/request/RequestService.h",
            "sakura_core/include/sakura/request/win32/WinHttpRequestRuntime.h",
            "sakura_core/include/sakura/request/win32/WinHttpSystemProxyResolver.h",
        ):
            self.assertTrue((REPO_ROOT / public_header).exists())

        inventory = json.loads((REPO_ROOT / "src/test/test-inventory.json").read_text(encoding="utf-8"))
        request_tests = [item for item in inventory["tests"] if item["test_id"].startswith("sakura_request_tests:")]
        self.assertEqual(5, len(request_tests))
        self.assertEqual({"sakura_request_tests"}, {item["runtime"]["runner_id"] for item in request_tests})

        for relative in (
            "sakura_core/sakura.vcxproj",
            "sakura_core/sakura.vcxproj.filters",
            "src/test/cmake/tests1.cmake",
        ):
            with self.subTest(projection=relative):
                projection = (REPO_ROOT / relative).read_text(encoding="utf-8-sig")
                self.assertNotIn("platform\\request\\RequestService.cpp", projection)
                self.assertNotIn("platform\\request\\win32\\WinHttpRequestRuntime.cpp", projection)
                self.assertNotIn("platform\\request\\win32\\WinHttpSystemProxyResolver.cpp", projection)

    def test_request_consumer_projects_receive_only_the_narrow_public_include_root(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        request = graph.components["sakura_request"]
        self.assertEqual(("sakura_core/include",), request.public_include_roots)
        self.assertEqual((), request.private_include_roots)
        self.assertEqual(("winhttp",), request.system_libraries)

        provider_text = (
            REPO_ROOT / "src/main/modules/generated/msbuild/projects/sakura_request.vcxproj"
        ).read_text(encoding="utf-8-sig")
        self.assertIn(r"$(SakuraRepoRoot)\sakura_core\include", provider_text)

        for relative in (
            "src/main/modules/generated/msbuild/projects/sakura_request_tests.vcxproj",
            "src/main/modules/generated/msbuild/consumers/sakura_app.props",
            "src/main/modules/generated/msbuild/consumers/tests1.props",
        ):
            with self.subTest(projection=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                include_directories = ";".join(
                    node.text or ""
                    for node in project.findall(f".//{MSBUILD_NS}AdditionalIncludeDirectories")
                )
                self.assertIn("include", include_directories)
                self.assertNotIn(r"platform\request", include_directories)

        cmake_test = (
            REPO_ROOT
            / "src/main/modules/generated/cmake/projects/cmake-msvc-x64-debug/sakura_request_tests/CMakeLists.txt"
        ).read_text(encoding="utf-8-sig")
        test_section = cmake_test.split("add_executable(sakura_request_tests", 1)[-1]
        self.assertIn("sakura_core/include", test_section)
        self.assertNotIn("sakura_core/platform/request", test_section)

    def test_solution_contains_generated_providers_for_legacy_products(self) -> None:
        graph = load_semantic_graph(REPO_ROOT)
        solution = (REPO_ROOT / "sakura.sln").read_text(encoding="utf-8-sig")
        product_ids = {"sakura_app", "tests1"}
        requirements: set[tuple[str, str]] = set()

        for context in graph.contexts.values():
            if context.backend != "msbuild":
                continue
            for edge in graph.active_edges(context.id):
                if edge.source not in product_ids or "link" not in edge.phases:
                    continue
                provider = graph.components.get(edge.target)
                if provider is None or provider.build_definition != "generated":
                    continue
                requirements.add((edge.source, edge.target))

        self.assertTrue(requirements)
        for consumer_id, provider_id in sorted(requirements):
            with self.subTest(consumer=consumer_id, provider=provider_id):
                consumer = graph.components[consumer_id]
                provider = graph.components[provider_id]
                self.assertEqual(1, len(consumer.backend_targets["msbuild"]))
                self.assertEqual(1, len(provider.backend_targets["msbuild"]))

                consumer_path = consumer.backend_targets["msbuild"][0].replace("/", "\\")
                provider_path = provider.backend_targets["msbuild"][0].replace("/", "\\")
                provider_project = ET.parse(REPO_ROOT / provider.backend_targets["msbuild"][0]).getroot()
                provider_guid_node = provider_project.find(f".//{MSBUILD_NS}ProjectGuid")
                self.assertIsNotNone(provider_guid_node)
                provider_guid = provider_guid_node.text
                self.assertIsNotNone(provider_guid)

                provider_marker = f', "{provider_path}", "{provider_guid}"'
                self.assertIn(provider_marker, solution)

                consumer_marker = f', "{consumer_path}", '
                consumer_start = solution.index(consumer_marker)
                consumer_end = solution.index("\nEndProject", consumer_start)
                consumer_block = solution[consumer_start:consumer_end]
                dependency = f"\t\t{provider_guid} = {provider_guid}"
                self.assertIn(dependency, consumer_block)

                for context in graph.contexts.values():
                    if context.backend != "msbuild" or context.id not in provider.supported_contexts:
                        continue
                    solution_configuration = f"{context.configuration}|{context.platform}"
                    self.assertIn(
                        f"{provider_guid}.{solution_configuration}.ActiveCfg = {solution_configuration}",
                        solution,
                    )
                    self.assertIn(
                        f"{provider_guid}.{solution_configuration}.Build.0 = {solution_configuration}",
                        solution,
                    )

    def test_shared_cmake_workspace_clean_is_explicit_opt_in(self) -> None:
        props = ET.parse(REPO_ROOT / "src/main/msbuild/cmake.props").getroot()
        clean_property = props.find(f".//{MSBUILD_NS}SakuraCleanCMakeToolsBuildDir")
        self.assertIsNotNone(clean_property)
        self.assertEqual("false", clean_property.text)
        self.assertEqual(
            "'$(SakuraCleanCMakeToolsBuildDir)'==''",
            clean_property.attrib.get("Condition"),
        )

        targets = ET.parse(REPO_ROOT / "src/main/msbuild/cmake.targets").getroot()
        clean_targets = [
            target
            for target in targets.findall(f"{MSBUILD_NS}Target")
            if target.find(f"{MSBUILD_NS}RemoveDir") is not None
            and target.find(f"{MSBUILD_NS}RemoveDir").attrib.get("Directories")
            == "$(CMakeToolsBuildDir)"
        ]
        self.assertEqual(1, len(clean_targets))
        self.assertEqual(
            "'$(SakuraCleanCMakeToolsBuildDir)'=='true'",
            clean_targets[0].attrib.get("Condition"),
        )
        self.assertEqual("CoreClean", clean_targets[0].attrib.get("BeforeTargets"))

    def test_language_projects_have_no_product_project_reference(self) -> None:
        for relative in (
            "sakura_lang/sakura_lang_en_US.vcxproj",
            "sakura_lang/sakura_lang_zh_CN.vcxproj",
        ):
            with self.subTest(project=relative):
                project = ET.parse(REPO_ROOT / relative).getroot()
                references = project.findall(f".//{MSBUILD_NS}ProjectReference")
                self.assertEqual([], references)

    def test_only_explicit_native_rebuild_observation_requests_shared_clean(self) -> None:
        build = _native_observation_command(["msbuild", "/t:Build"], "Build")
        rebuild = _native_observation_command(["msbuild", "/t:Rebuild"], "Rebuild")

        self.assertNotIn("/p:SakuraCleanCMakeToolsBuildDir=true", build)
        self.assertEqual(
            ["msbuild", "/t:Rebuild", "/p:SakuraCleanCMakeToolsBuildDir=true"],
            rebuild,
        )


if __name__ == "__main__":
    unittest.main()
