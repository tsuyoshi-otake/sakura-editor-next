"""Deterministic committed projection generation."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path, PureWindowsPath
from typing import Iterable, Mapping
from xml.sax.saxutils import escape

from .abi_contract import render_detect_mismatch_header
from .model import Component, Context, GENERATOR_VERSION, INTERFACE_COMPONENT_KINDS, ManifestError, SemanticGraph


GENERATED_ROOT = Path("src/main/modules/generated")
ABI_STAMP_ROOT = GENERATED_ROOT / "abi"
MSBUILD_LEGACY_CONSUMER_ROOT = GENERATED_ROOT / "msbuild" / "consumers"
CMAKE_LEGACY_ROOT = GENERATED_ROOT / "cmake" / "legacy"
CMAKE_LEGACY_CONSUMER_ROOT = CMAKE_LEGACY_ROOT / "consumers"
CMAKE_LEGACY_OWNERSHIP_PATH = CMAKE_LEGACY_ROOT / "source-ownership.cmake"


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"


def _project_guid(component_id: str) -> str:
    value = uuid.uuid5(uuid.NAMESPACE_URL, f"https://sakura-editor-next.invalid/components/{component_id}")
    return "{" + str(value).upper() + "}"


def _msbuild_path(relative: str) -> str:
    return "$(SakuraRepoRoot)\\" + relative.replace("/", "\\")


def _component_edges(graph: SemanticGraph, component_id: str, context_id: str):
    """Return direct component edges that require a native link dependency."""
    return tuple(
        edge
        for edge in graph.active_edges(context_id)
        if edge.source == component_id
        and edge.target in graph.components
        and "link" in edge.phases
    )


def _final_link_generated_dependencies(
    graph: SemanticGraph,
    component_id: str,
    context_id: str,
) -> tuple[str, ...]:
    """Return generated archive providers required by a native final link.

    Static-library link requirements do not propagate through every native
    backend in the same way.  In particular, an MSBuild ProjectReference to a
    static library does not reliably carry that library's private archive
    providers into the final executable.  The semantic graph already owns the
    complete private link closure, so project generation must materialize that
    closure at each native final-link root.  This keeps ``propagation=none``
    private for compile/API usage while preventing an unresolved symbol from
    being hidden behind a transitive archive dependency.
    """
    providers: set[str] = set()
    for dependency_id in graph.final_link_closure((component_id,), context_id):
        if dependency_id == component_id or dependency_id not in graph.components:
            continue
        dependency = graph.components[dependency_id]
        if dependency.build_definition == "generated" and dependency.sources:
            providers.add(dependency_id)
    return tuple(sorted(providers))


def _system_libraries_for_link(
    graph: SemanticGraph,
    component_id: str,
    context_id: str,
) -> tuple[str, ...]:
    """Return system libraries required by a component's final static link.

    System libraries are declared by the component that owns the native API
    usage.  A static archive cannot carry those requirements by itself, so the
    executable/legacy consumer receives the deterministic union of its link
    closure.  The same declaration is projected to CMake as a transitive
    PUBLIC dependency below.
    """
    libraries: set[str] = set()
    for current_id in graph.final_link_closure((component_id,), context_id):
        current = graph.components.get(current_id)
        if current is not None:
            libraries.update(current.system_libraries)
    return tuple(sorted(libraries, key=str.casefold))


def _compile_include_roots(graph: SemanticGraph, component_id: str, context_id: str) -> tuple[str, ...]:
    component = graph.components[component_id]
    roots = set(component.public_include_roots) | set(component.private_include_roots)
    for dependency_id in graph.closure((component_id,), context_id, ("compile",)):
        if dependency_id == component_id or dependency_id not in graph.components:
            continue
        roots.update(graph.components[dependency_id].public_include_roots)
    return tuple(sorted(roots))


def _abi_header_path(component_id: str, context_id: str) -> Path:
    return ABI_STAMP_ROOT / context_id / f"{component_id}.h"


def _abi_header_text(graph: SemanticGraph, component_id: str, context_id: str) -> str:
    """Render edge-scoped MSVC link guards without over-constraining C boundaries."""
    stamps: list[tuple[str, str]] = []
    profile = graph.project_profile(component_id, context_id)
    for edge in sorted(graph.active_edges(context_id), key=lambda item: item.id):
        if edge.contract_profile is None or component_id not in {edge.source, edge.target}:
            continue
        if not {"compile", "link"}.intersection(edge.phases):
            continue
        if edge.source not in graph.components or edge.target not in graph.components:
            continue
        policy = graph.compile_profiles.contract_profiles[edge.contract_profile]
        for field in sorted(policy["required_project_fields"]):
            stamps.append((f"sakura.edge.{edge.id}.{field}", profile[field]))
    return render_detect_mismatch_header(stamps)


def _legacy_generated_link_dependencies(
    graph: SemanticGraph,
    component_id: str,
    context_id: str,
) -> tuple[Component, ...]:
    """Return generated providers linked directly by a legacy native target."""
    component = graph.components[component_id]
    if component.build_definition != "legacy":
        return ()
    return tuple(
        graph.components[dependency_id]
        for dependency_id in _final_link_generated_dependencies(graph, component_id, context_id)
    )


def _legacy_consumer_contexts(
    graph: SemanticGraph,
    component: Component,
    backend: str,
) -> tuple[Context, ...]:
    return tuple(
        graph.contexts[context_id]
        for context_id in component.supported_contexts
        if graph.contexts[context_id].backend == backend
        and _legacy_generated_link_dependencies(graph, component.id, context_id)
    )


def _msbuild_condition(contexts: Iterable[Context], all_contexts: Iterable[Context]) -> str:
    selected = tuple(sorted(contexts, key=lambda item: item.id))
    available = tuple(sorted(all_contexts, key=lambda item: item.id))
    if len(selected) == len(available):
        return ""
    terms = [
        f"'$(Configuration)|$(Platform)'=='{item.configuration}|{item.platform}'"
        for item in selected
    ]
    return f' Condition="{escape(" Or ".join(terms))}"'


def _msbuild_source_item_spec(
    graph: SemanticGraph,
    project_path: Path,
    source: str,
) -> str | None:
    """Find the exact handwritten ClCompile identity for a provider source."""
    project_directory = graph.repo_root / project_path.parent
    expected = (graph.repo_root / source).resolve()
    root = ET.parse(graph.repo_root / project_path).getroot()
    namespace = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
    for item in root.findall(".//msbuild:ClCompile", namespace):
        include = item.get("Include")
        if not include or "$" in include or "*" in include or "?" in include:
            continue
        # MSBuild item specs always use Windows separators, including when this
        # generator runs on a POSIX CI host.
        include_path = Path(PureWindowsPath(include).as_posix())
        if (project_directory / include_path).resolve() == expected:
            return include
    return None


def _msbuild_legacy_consumer_projection(graph: SemanticGraph, component: Component) -> str:
    """Project an always-green legacy consumer -> generated provider edge.

    The handwritten project keeps its original source item.  While this props
    file exists, the extracted provider sources are removed and replaced by a
    ProjectReference.  Removing the manifest edge and regenerating deletes this
    file, so the conditional import restores legacy ownership without editing
    the handwritten source list again.
    """
    contexts = _legacy_consumer_contexts(graph, component, "msbuild")
    if not contexts:
        raise ValueError(f"legacy MSBuild consumer has no generated dependency: {component.id}")
    targets = component.backend_targets.get("msbuild", ())
    if len(targets) != 1:
        raise ValueError(f"legacy MSBuild consumer must have exactly one project: {component.id}")
    project_path = Path(targets[0])
    project_directory = graph.repo_root / project_path.parent
    dependency_contexts: dict[str, list[Context]] = {}
    for context in contexts:
        for dependency in _legacy_generated_link_dependencies(graph, component.id, context.id):
            dependency_contexts.setdefault(dependency.id, []).append(context)

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
        '  <!-- Generated by tools/build/sakura_build.py. Do not edit. -->',
        '  <!-- Removing this projection restores the handwritten legacy source ownership. -->',
    ]
    source_records: dict[str, list[Context]] = {}
    for dependency_id, active_contexts in dependency_contexts.items():
        dependency = graph.components[dependency_id]
        for source in dependency.sources:
            source_records.setdefault(source, []).extend(active_contexts)
    if source_records:
        removal_lines: list[str] = []
        for source, active_contexts in sorted(source_records.items()):
            item_spec = _msbuild_source_item_spec(graph, project_path, source)
            if item_spec is None:
                continue
            unique_contexts = {item.id: item for item in active_contexts}.values()
            condition = _msbuild_condition(unique_contexts, contexts)
            removal_lines.append(f'    <ClCompile Remove="{escape(item_spec)}"{condition} />')
        if removal_lines:
            lines.append('  <ItemGroup>')
            lines.extend(removal_lines)
            lines.append('  </ItemGroup>')

    for context in sorted(contexts, key=lambda item: item.id):
        include_roots: set[str] = set()
        for dependency in _legacy_generated_link_dependencies(graph, component.id, context.id):
            include_roots.update(dependency.public_include_roots)
        abi_relative = os.path.relpath(
            graph.repo_root / _abi_header_path(component.id, context.id),
            project_directory,
        ).replace("/", "\\")
        condition = escape(f"'$(Configuration)|$(Platform)'=='{context.configuration}|{context.platform}'")
        system_libraries = _system_libraries_for_link(graph, component.id, context.id)
        lines.extend([
            f'  <ItemDefinitionGroup Condition="{condition}">',
            '    <ClCompile>',
        ])
        if include_roots:
            include_text = ";".join(
                f"$(MSBuildProjectDirectory)\\{os.path.relpath(graph.repo_root / item, project_directory).replace('/', '\\')}"
                for item in sorted(include_roots)
            )
            lines.append(f'      <AdditionalIncludeDirectories>{escape(include_text)};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>')
        lines.extend([
            f'      <ForcedIncludeFiles>$(MSBuildProjectDirectory)\\{escape(abi_relative)};%(ForcedIncludeFiles)</ForcedIncludeFiles>',
            '    </ClCompile>',
        ])
        if system_libraries:
            additional_dependencies = ";".join(
                [*(f"{library}.lib" for library in system_libraries), "%(AdditionalDependencies)"]
            )
            lines.extend([
                '    <Link>',
                f'      <AdditionalDependencies>{escape(additional_dependencies)}</AdditionalDependencies>',
                '    </Link>',
            ])
        lines.append('  </ItemDefinitionGroup>')

    lines.extend([
        "  <ItemDefinitionGroup Condition=\"'$(SakuraNativeProductMapEvidence)'=='true'\">",
        '    <Link>',
        '      <GenerateMapFile>true</GenerateMapFile>',
        '    </Link>',
        '  </ItemDefinitionGroup>',
    ])

    lines.append('  <ItemGroup>')
    for dependency_id, active_contexts in sorted(dependency_contexts.items()):
        dependency = graph.components[dependency_id]
        dependency_targets = dependency.backend_targets.get("msbuild", ())
        generated_targets = [item for item in dependency_targets if item.startswith("src/main/modules/generated/msbuild/projects/")]
        if len(generated_targets) != 1:
            raise ValueError(f"generated component dependency must have one generated MSBuild target: {component.id} -> {dependency_id}")
        relative = os.path.relpath(graph.repo_root / generated_targets[0], project_directory).replace("/", "\\")
        condition = _msbuild_condition(active_contexts, contexts)
        lines.extend([
            f'    <ProjectReference Include="$(MSBuildProjectDirectory)\\{escape(relative)}"{condition}>',
            f'      <Project>{_project_guid(dependency_id)}</Project>',
        ])
        for context in sorted({item.id: item for item in active_contexts}.values(), key=lambda item: item.id):
            metadata_condition = escape(
                f"'$(Configuration)|$(Platform)'=='{context.configuration}|{context.platform}'"
            )
            lines.extend([
                f'      <SetConfiguration Condition="{metadata_condition}">Configuration={escape(context.configuration)}</SetConfiguration>',
                f'      <SetPlatform Condition="{metadata_condition}">Platform={escape(context.platform)}</SetPlatform>',
            ])
        lines.append('    </ProjectReference>')
    lines.extend(['  </ItemGroup>', '</Project>', ''])
    return "\n".join(lines)


def _cmake_config_expression(configuration: str, value: str) -> str:
    return f'"$<$<CONFIG:{configuration}>:{value}>"'


def _cmake_abi_options(
    graph: SemanticGraph,
    component_id: str,
    contexts: Iterable[Context],
    target: str,
) -> list[str]:
    """Render compiler/config-specific ABI enforcement for a native target."""
    by_toolchain: dict[str, dict[str, Context]] = {}
    for context in contexts:
        by_toolchain.setdefault(context.toolchain, {})[context.configuration] = context
    lines: list[str] = []
    if "msvc" in by_toolchain:
        lines.append('if(MSVC)')
        runtime_values: list[str] = []
        option_values: list[str] = []
        definition_values: list[str] = []
        for configuration, context in sorted(by_toolchain["msvc"].items()):
            profile = graph.project_profile(component_id, context.id)
            runtime = {"MTd": "MultiThreadedDebug", "MT": "MultiThreaded"}.get(profile["crt"])
            if runtime is None:
                raise ValueError(f"unsupported generated CMake MSVC CRT profile: {profile['crt']}")
            runtime_values.append(f'$<$<CONFIG:{configuration}>:{runtime}>')
            wchar_option = "/Zc:wchar_t" if profile["wchar_t_builtin"] else "/Zc:wchar_t-"
            abi_header = _abi_header_path(component_id, context.id).as_posix()
            for value in (
                "/source-charset:utf-8",
                "/execution-charset:utf-8",
                f'/Zp{profile["default_pack"]}',
                wchar_option,
                f'/FI${{CMAKE_SOURCE_DIR}}/{abi_header}',
            ):
                option_values.append(_cmake_config_expression(configuration, value))
            definitions = [
                f'_ITERATOR_DEBUG_LEVEL={profile["iterator_debug_level"]}',
                f'_WIN32_WINNT={profile["win32_winnt"]}',
                "NOMINMAX",
            ]
            if profile["unicode"]:
                definitions.extend(("UNICODE", "_UNICODE"))
            definition_values.extend(_cmake_config_expression(configuration, value) for value in definitions)
        lines.append(f'  set_property(TARGET {target} PROPERTY MSVC_RUNTIME_LIBRARY "{"".join(runtime_values)}")')
        lines.append(f'  target_compile_options({target} PRIVATE {" ".join(option_values)})')
        lines.append(f'  target_compile_definitions({target} PRIVATE {" ".join(definition_values)})')
    if "mingw" in by_toolchain:
        lines.append('elseif(MINGW)' if "msvc" in by_toolchain else 'if(MINGW)')
        option_values = []
        definition_values = []
        for configuration, context in sorted(by_toolchain["mingw"].items()):
            profile = graph.project_profile(component_id, context.id)
            abi_header = _abi_header_path(component_id, context.id).as_posix()
            option_values.extend([
                _cmake_config_expression(configuration, f'-fpack-struct={profile["default_pack"]}'),
                _cmake_config_expression(configuration, "-include"),
                _cmake_config_expression(configuration, f'${{CMAKE_SOURCE_DIR}}/{abi_header}'),
            ])
            definitions = [f'_WIN32_WINNT={profile["win32_winnt"]}', "NOMINMAX"]
            if profile["unicode"]:
                definitions.extend(("UNICODE", "_UNICODE"))
            definition_values.extend(_cmake_config_expression(configuration, value) for value in definitions)
        lines.append(f'  target_compile_options({target} PRIVATE {" ".join(option_values)})')
        lines.append(f'  target_compile_definitions({target} PRIVATE {" ".join(definition_values)})')
    if by_toolchain:
        lines.append('endif()')
    return lines


def _cmake_abi_force_include(
    graph: SemanticGraph,
    component_id: str,
    contexts: Iterable[Context],
    target: str,
) -> list[str]:
    """Force only the edge guard into a legacy target; keep its native flags."""
    by_toolchain: dict[str, dict[str, Context]] = {}
    for context in contexts:
        by_toolchain.setdefault(context.toolchain, {})[context.configuration] = context
    lines: list[str] = []
    if "msvc" in by_toolchain:
        values = [
            _cmake_config_expression(
                configuration,
                f'/FI${{CMAKE_SOURCE_DIR}}/{_abi_header_path(component_id, context.id).as_posix()}',
            )
            for configuration, context in sorted(by_toolchain["msvc"].items())
        ]
        lines.extend(['if(MSVC)', f'  target_compile_options({target} PRIVATE {" ".join(values)})'])
    if "mingw" in by_toolchain:
        values: list[str] = []
        for configuration, context in sorted(by_toolchain["mingw"].items()):
            values.extend([
                _cmake_config_expression(configuration, "-include"),
                _cmake_config_expression(
                    configuration,
                    f'${{CMAKE_SOURCE_DIR}}/{_abi_header_path(component_id, context.id).as_posix()}',
                ),
            ])
        lines.extend([
            'elseif(MINGW)' if "msvc" in by_toolchain else 'if(MINGW)',
            f'  target_compile_options({target} PRIVATE {" ".join(values)})',
        ])
    if by_toolchain:
        lines.append('endif()')
    return lines


def _cmake_legacy_providers(graph: SemanticGraph) -> tuple[Component, ...]:
    providers: dict[str, Component] = {}
    for component in graph.components.values():
        if component.build_definition != "legacy":
            continue
        for context_id in component.supported_contexts:
            if graph.contexts[context_id].backend != "cmake":
                continue
            for dependency in _legacy_generated_link_dependencies(graph, component.id, context_id):
                providers[dependency.id] = dependency
    return tuple(providers[item] for item in sorted(providers))


def _cmake_legacy_source_ownership(graph: SemanticGraph) -> str:
    providers = _cmake_legacy_providers(graph)
    if not providers:
        raise ValueError("legacy CMake source ownership has no generated providers")
    lines = [
        '# Generated by tools/build/sakura_build.py. Do not edit.',
        '# Removing this projection restores the legacy GLOB-owned source list.',
    ]
    for provider in providers:
        for source in sorted(provider.sources):
            lines.append(f'list(REMOVE_ITEM SOURCES "${{CMAKE_SOURCE_DIR}}/{source}")')
    lines.append('')
    for provider in providers:
        contexts = tuple(
            graph.contexts[context_id]
            for context_id in provider.supported_contexts
            if graph.contexts[context_id].backend == "cmake"
        )
        target_files = [*provider.sources, *provider.public_headers, *provider.private_headers]
        lines.append(f'if(NOT TARGET {provider.id})')
        lines.append(f'  add_library({provider.id} STATIC')
        lines.extend(f'    "${{CMAKE_SOURCE_DIR}}/{item}"' for item in target_files)
        lines.append('  )')
        if provider.public_include_roots:
            roots = " ".join(f'"${{CMAKE_SOURCE_DIR}}/{item}"' for item in provider.public_include_roots)
            lines.append(f'  target_include_directories({provider.id} PUBLIC {roots})')
        if provider.private_include_roots:
            roots = " ".join(f'"${{CMAKE_SOURCE_DIR}}/{item}"' for item in provider.private_include_roots)
            lines.append(f'  target_include_directories({provider.id} PRIVATE {roots})')
        lines.append(f'  target_compile_features({provider.id} PRIVATE cxx_std_20)')
        if provider.system_libraries:
            lines.append(f'  target_link_libraries({provider.id} PUBLIC {" ".join(provider.system_libraries)})')
        lines.extend(f'  {item}' for item in _cmake_abi_options(graph, provider.id, contexts, provider.id))
        lines.append('endif()')
        lines.append('')
    return "\n".join(lines)


def _cmake_legacy_consumer_projection(graph: SemanticGraph, component: Component) -> str:
    contexts = _legacy_consumer_contexts(graph, component, "cmake")
    if not contexts:
        raise ValueError(f"legacy CMake consumer has no generated dependency: {component.id}")
    dependencies = {
        dependency.id
        for context in contexts
        for dependency in _legacy_generated_link_dependencies(graph, component.id, context.id)
    }
    lines = [
        '# Generated by tools/build/sakura_build.py. Do not edit.',
        'if(NOT DEFINED SAKURA_LEGACY_CONSUMER_COMPILE_TARGET OR',
        '   NOT DEFINED SAKURA_LEGACY_CONSUMER_LINK_TARGET)',
        f'  message(FATAL_ERROR "Legacy component integration targets were not provided for {component.id}")',
        'endif()',
        'if(NOT TARGET "${SAKURA_LEGACY_CONSUMER_COMPILE_TARGET}" OR',
        '   NOT TARGET "${SAKURA_LEGACY_CONSUMER_LINK_TARGET}")',
        f'  message(FATAL_ERROR "Legacy component integration targets do not exist for {component.id}")',
        'endif()',
    ]
    for dependency_id in sorted(dependencies):
        lines.append(f'target_link_libraries("${{SAKURA_LEGACY_CONSUMER_LINK_TARGET}}" PRIVATE {dependency_id})')
    lines.extend(_cmake_abi_force_include(graph, component.id, contexts, '"${SAKURA_LEGACY_CONSUMER_COMPILE_TARGET}"'))
    lines.append('')
    return "\n".join(lines)


def _msbuild_project(graph: SemanticGraph, component: Component, project_path: Path) -> str:
    contexts = [
        graph.contexts[context_id]
        for context_id in component.supported_contexts
        if graph.contexts[context_id].backend == "msbuild"
    ]
    if not contexts:
        raise ValueError(f"generated MSBuild component has no MSBuild context: {component.id}")
    guid = _project_guid(component.id)
    if component.kind in {"test", "executable"}:
        configuration_type = "Application"
    elif component.sources:
        configuration_type = "StaticLibrary"
    else:
        # MSBuild has no C++ INTERFACE target equivalent. Utility carries
        # metadata/references without manufacturing an empty archive.
        configuration_type = "Utility"
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
        '  <!-- Generated by tools/build/sakura_build.py. Do not edit. -->',
        '  <ItemGroup Label="ProjectConfigurations">',
    ]
    for context in sorted(contexts, key=lambda item: item.id):
        lines.extend([
            f'    <ProjectConfiguration Include="{escape(context.configuration)}|{escape(context.platform)}">',
            f'      <Configuration>{escape(context.configuration)}</Configuration>',
            f'      <Platform>{escape(context.platform)}</Platform>',
            '    </ProjectConfiguration>',
        ])
    lines.extend([
        '  </ItemGroup>',
        '  <PropertyGroup Label="Globals">',
        f'    <ProjectGuid>{guid}</ProjectGuid>',
        f'    <RootNamespace>{escape(component.id)}</RootNamespace>',
        f'    <ProjectName>{escape(component.id)}</ProjectName>',
        '    <Keyword>Win32Proj</Keyword>',
        '    <ImportDirectoryBuildProps>false</ImportDirectoryBuildProps>',
        '    <ImportDirectoryBuildTargets>false</ImportDirectoryBuildTargets>',
        "    <SakuraRepoRoot>$([System.IO.Path]::GetFullPath('$(MSBuildThisFileDirectory)..\\..\\..\\..\\..\\..'))</SakuraRepoRoot>",
        '  </PropertyGroup>',
        '  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />',
    ])
    for context in sorted(contexts, key=lambda item: item.id):
        profile = graph.project_profile(component.id, context.id)
        lines.extend([
            f'  <PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'{escape(context.configuration)}|{escape(context.platform)}\'" Label="Configuration">',
            f'    <ConfigurationType>{configuration_type}</ConfigurationType>',
            f'    <UseDebugLibraries>{str(context.configuration == "Debug").lower()}</UseDebugLibraries>',
            f'    <PlatformToolset>{escape(profile["toolset"])}</PlatformToolset>',
            f'    <CharacterSet>{"Unicode" if profile["unicode"] else "NotSet"}</CharacterSet>',
            '    <WholeProgramOptimization>false</WholeProgramOptimization>',
            '  </PropertyGroup>',
        ])
    lines.append('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />')
    for context in sorted(contexts, key=lambda item: item.id):
        lines.extend([
            f'  <ImportGroup Label="PropertySheets" Condition="\'$(Configuration)|$(Platform)\'==\'{escape(context.configuration)}|{escape(context.platform)}\'">',
            f'    <Import Project="$(SakuraRepoRoot)\\src\\main\\modules\\generated\\msbuild\\{escape(context.id)}.props" />',
            '  </ImportGroup>',
        ])
    lines.extend(['  <PropertyGroup Label="UserMacros" />'])
    for context in sorted(contexts, key=lambda item: item.id):
        profile = graph.project_profile(component.id, context.id)
        runtime = {"MTd": "MultiThreadedDebug", "MT": "MultiThreaded"}.get(profile["crt"])
        if runtime is None:
            raise ValueError(f"unsupported generated MSBuild CRT profile: {profile['crt']}")
        include_roots = [_msbuild_path(item) for item in _compile_include_roots(graph, component.id, context.id)]
        includes = ";".join(include_roots + ["%(AdditionalIncludeDirectories)"])
        abi_header = _msbuild_path(_abi_header_path(component.id, context.id).as_posix())
        definitions = [f"_ITERATOR_DEBUG_LEVEL={profile['iterator_debug_level']}", f"_WIN32_WINNT={profile['win32_winnt']}", "NOMINMAX"]
        if profile["unicode"]:
            definitions.extend(("UNICODE", "_UNICODE"))
        definition_text = ";".join((*definitions, "%(PreprocessorDefinitions)"))
        alignment = {1: "1Byte", 2: "2Bytes", 4: "4Bytes", 8: "8Bytes", 16: "16Bytes"}.get(profile["default_pack"])
        if alignment is None:
            raise ValueError(f"unsupported generated MSBuild packing profile: {profile['default_pack']}")
        output_root = f"$(SakuraRepoRoot)\\build\\components\\{context.id}\\{component.id}\\"
        lines.extend([
            f'  <PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'{escape(context.configuration)}|{escape(context.platform)}\'">',
            f'    <OutDir>{escape(output_root)}bin\\</OutDir>',
            f'    <IntDir>{escape(output_root)}obj\\</IntDir>',
            f'    <TargetName>{escape(component.id)}</TargetName>',
            f'    <LinkIncremental>{str(configuration_type == "Application" and graph.compile_profiles.link_profiles[graph.compile_profiles.context_profiles[context.id]["link_profile"]]["incremental_link"]).lower()}</LinkIncremental>',
            '  </PropertyGroup>',
            f'  <ItemDefinitionGroup Condition="\'$(Configuration)|$(Platform)\'==\'{escape(context.configuration)}|{escape(context.platform)}\'">',
        ])
        if component.sources:
            lines.extend([
                '    <ClCompile>',
                '      <PrecompiledHeader>NotUsing</PrecompiledHeader>',
                '      <MultiProcessorCompilation>true</MultiProcessorCompilation>',
                f'      <LanguageStandard>{"stdcpp20" if profile["cpp_standard"] == "c++20" else escape(profile["cpp_standard"])}</LanguageStandard>',
                f'      <RuntimeLibrary>{runtime}</RuntimeLibrary>',
                f'      <TreatWChar_tAsBuiltInType>{str(profile["wchar_t_builtin"]).lower()}</TreatWChar_tAsBuiltInType>',
                f'      <StructMemberAlignment>{alignment}</StructMemberAlignment>',
                f'      <AdditionalIncludeDirectories>{escape(includes)}</AdditionalIncludeDirectories>',
                f'      <PreprocessorDefinitions>{escape(definition_text)}</PreprocessorDefinitions>',
                f'      <ForcedIncludeFiles>{escape(abi_header)};%(ForcedIncludeFiles)</ForcedIncludeFiles>',
                '      <AdditionalOptions>/source-charset:utf-8 /execution-charset:utf-8 /showIncludes %(AdditionalOptions)</AdditionalOptions>',
                '      <ObjectFileName>$(IntDir)%(Filename).obj</ObjectFileName>',
                '      <ProgramDataBaseFileName>$(IntDir)$(ProjectName).compile.pdb</ProgramDataBaseFileName>',
                '      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>',
                '      <SupportJustMyCode>false</SupportJustMyCode>',
                '      <WarningLevel>Level4</WarningLevel>',
                '    </ClCompile>',
            ])
        if configuration_type == "StaticLibrary":
            lines.extend([
                '    <Lib>',
                '      <OutputFile>$(OutDir)$(TargetName).lib</OutputFile>',
                '    </Lib>',
            ])
        elif configuration_type == "Application":
            system_libraries = _system_libraries_for_link(graph, component.id, context.id)
            additional_dependencies = ";".join(
                [*(f"{library}.lib" for library in system_libraries), "%(AdditionalDependencies)"]
            )
            lines.extend([
                '    <Link>',
                f'      <AdditionalDependencies>{escape(additional_dependencies)}</AdditionalDependencies>',
                '      <SubSystem>Console</SubSystem>',
                '      <GenerateDebugInformation>true</GenerateDebugInformation>',
                '      <ProgramDatabaseFile>$(OutDir)$(TargetName).pdb</ProgramDatabaseFile>',
                '      <GenerateMapFile>true</GenerateMapFile>',
                '      <MapFileName>$(OutDir)$(TargetName).map</MapFileName>',
            ])
            if graph.compile_profiles.link_profiles[graph.compile_profiles.context_profiles[context.id]["link_profile"]]["incremental_link"] is False:
                lines.append('      <EnableCOMDATFolding>false</EnableCOMDATFolding>')
            lines.append('    </Link>')
        lines.append('  </ItemDefinitionGroup>')
    if component.public_headers or component.private_headers:
        lines.append('  <ItemGroup>')
        for header in sorted((*component.public_headers, *component.private_headers)):
            lines.append(f'    <ClInclude Include="{escape(_msbuild_path(header))}" />')
        lines.append('  </ItemGroup>')
    if component.sources:
        lines.append('  <ItemGroup>')
        for source in sorted(component.sources):
            lines.append(f'    <ClCompile Include="{escape(_msbuild_path(source))}" />')
        lines.append('  </ItemGroup>')
    dependency_contexts: dict[str, list[Context]] = {}
    for context in contexts:
        for dependency_id in _final_link_generated_dependencies(graph, component.id, context.id):
            dependency_contexts.setdefault(dependency_id, []).append(context)
    dependency_ids = sorted(dependency_contexts)
    if dependency_ids:
        lines.append('  <ItemGroup>')
        for dependency_id in dependency_ids:
            dependency = graph.components[dependency_id]
            dependency_targets = dependency.backend_targets.get("msbuild", ())
            generated_targets = [item for item in dependency_targets if item.startswith("src/main/modules/generated/msbuild/projects/")]
            if len(generated_targets) != 1:
                raise ValueError(f"generated component dependency must have one generated MSBuild target: {component.id} -> {dependency_id}")
            relative = os.path.relpath(graph.repo_root / generated_targets[0], graph.repo_root / project_path.parent).replace("/", "\\")
            active_contexts = dependency_contexts[dependency_id]
            condition = ""
            if len(active_contexts) != len(contexts):
                terms = [
                    f"'$(Configuration)|$(Platform)'=='{item.configuration}|{item.platform}'"
                    for item in sorted(active_contexts, key=lambda value: value.id)
                ]
                condition = f' Condition="{escape(" Or ".join(terms))}"'
            lines.extend([
                f'    <ProjectReference Include="{escape(relative)}"{condition}>',
                f'      <Project>{_project_guid(dependency_id)}</Project>',
            ])
            for active_context in sorted(active_contexts, key=lambda item: item.id):
                metadata_condition = escape(
                    f"'$(Configuration)|$(Platform)'=='{active_context.configuration}|{active_context.platform}'"
                )
                lines.extend([
                    f'      <SetConfiguration Condition="{metadata_condition}">Configuration={escape(active_context.configuration)}</SetConfiguration>',
                    f'      <SetPlatform Condition="{metadata_condition}">Platform={escape(active_context.platform)}</SetPlatform>',
                ])
            lines.append('    </ProjectReference>')
        lines.append('  </ItemGroup>')
    lines.extend([
        '  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />',
        '</Project>',
        '',
    ])
    return "\n".join(lines)


def _cmake_component_order(graph: SemanticGraph, root_id: str, context_id: str) -> tuple[str, ...]:
    visited: set[str] = set()
    ordered: list[str] = []

    def visit(component_id: str) -> None:
        if component_id in visited:
            return
        visited.add(component_id)
        for dependency_id in _final_link_generated_dependencies(graph, component_id, context_id):
            visit(dependency_id)
        ordered.append(component_id)

    visit(root_id)
    return tuple(ordered)


def _cmake_project(graph: SemanticGraph, root: Component, context_id: str) -> str:
    context = graph.contexts[context_id]
    lines = [
        'cmake_minimum_required(VERSION 3.24)',
        f'project({root.id} LANGUAGES CXX)',
        # MSVC emits /showIncludes diagnostics as UTF-8 on this toolchain,
        # while CMake's compiler probe can decode the captured pipe using the
        # active ANSI code page.  Pin the byte prefix consumed by Ninja after
        # project() so localized probe decoding cannot erase header deps.
        'set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "メモ: インクルード ファイル:  ")',
        'set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")',
        '',
        '# Generated by tools/build/sakura_build.py. Do not edit.',
        'get_filename_component(SAKURA_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../.." ABSOLUTE)',
        f'include("${{CMAKE_CURRENT_LIST_DIR}}/../../../{context_id}.cmake")',
        'set(CMAKE_CXX_STANDARD 20)',
        'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
        'set(CMAKE_CXX_EXTENSIONS OFF)',
        # Leave headroom for the real checkout spelling when a short-lived
        # ASCII junction is required for a non-ASCII Windows path.
        'set(CMAKE_OBJECT_PATH_MAX 220)',
    ]
    if context.toolchain == "msvc":
        lines.append('set(CMAKE_CXX_STANDARD_LIBRARIES "")')
    if context.toolchain == "msvc":
        lines.append('file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")')
    lines.append('')
    for component_id in _cmake_component_order(graph, root.id, context_id):
        component = graph.components[component_id]
        source_files = [f'"${{SAKURA_REPO_ROOT}}/{item}"' for item in component.sources]
        header_files = [f'"${{SAKURA_REPO_ROOT}}/{item}"' for item in (*component.public_headers, *component.private_headers)]
        target_files = [*source_files, *header_files]
        has_sources = bool(source_files)
        profile = graph.project_profile(component.id, context_id)
        if component.kind == "test" or component.kind == "executable":
            lines.append(f'add_executable({component.id}')
            lines.extend(f'  {item}' for item in target_files)
            lines.append(')')
        elif has_sources:
            lines.append(f'add_library({component.id} STATIC')
            lines.extend(f'  {item}' for item in target_files)
            lines.append(')')
        else:
            # Header-only contracts and aggregates are metadata-only targets;
            # header files must never become archive members.
            lines.append(f'add_library({component.id} INTERFACE)')
            if header_files:
                lines.append(f'target_sources({component.id} INTERFACE')
                lines.extend(f'  {item}' for item in header_files)
                lines.append(')')
        public_roots = [f'"${{SAKURA_REPO_ROOT}}/{item}"' for item in component.public_include_roots]
        private_roots = [f'"${{SAKURA_REPO_ROOT}}/{item}"' for item in component.private_include_roots]
        own_roots = set(component.public_include_roots) | set(component.private_include_roots)
        compile_dependency_roots = [
            f'"${{SAKURA_REPO_ROOT}}/{item}"'
            for item in _compile_include_roots(graph, component.id, context_id)
            if item not in own_roots
        ]
        if public_roots:
            include_scope = "PUBLIC" if has_sources else "INTERFACE"
            lines.append(f'target_include_directories({component.id} {include_scope} {" ".join(public_roots)})')
        if private_roots and has_sources:
            lines.append(f'target_include_directories({component.id} PRIVATE {" ".join(private_roots)})')
        elif private_roots:
            lines.append(f'target_include_directories({component.id} INTERFACE {" ".join(private_roots)})')
        if compile_dependency_roots:
            # Compile-only providers contribute public include roots only; no
            # native target or archive is introduced by this edge.
            compile_scope = "PRIVATE" if has_sources else "INTERFACE"
            lines.append(f'target_include_directories({component.id} {compile_scope} {" ".join(compile_dependency_roots)})')
        if has_sources:
            lines.append(f'target_compile_features({component.id} PRIVATE cxx_std_20)')
            if context.toolchain == "msvc":
                runtime = {"MTd": "MultiThreadedDebug", "MT": "MultiThreaded"}.get(profile["crt"])
                if runtime is None:
                    raise ValueError(f"unsupported generated CMake MSVC CRT profile: {profile['crt']}")
                wchar_option = "/Zc:wchar_t" if profile["wchar_t_builtin"] else "/Zc:wchar_t-"
                abi_header = _abi_header_path(component.id, context_id).as_posix()
                lines.append(f'set_property(TARGET {component.id} PROPERTY MSVC_RUNTIME_LIBRARY "{runtime}")')
                # CMake's Ninja/MSVC generator otherwise emits `/Fd<dir>\\` for
                # executable targets.  That trailing-directory form is accepted
                # in a normal checkout on some toolchains but is rejected by
                # cl.exe in the short-lived cloned workspaces used by rebuild
                # evidence.  Give every native component an explicit, unique
                # compile/link PDB path so the hermetic path matrix behaves the
                # same as the in-place build.
                lines.append(
                    f'set_target_properties({component.id} PROPERTIES '
                    f'COMPILE_PDB_NAME "{component.id}.compile" '
                    f'COMPILE_PDB_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/pdb" '
                    f'PDB_NAME "{component.id}" '
                    f'PDB_OUTPUT_DIRECTORY "${{CMAKE_BINARY_DIR}}/pdb")'
                )
                lines.append(
                    f'target_compile_options({component.id} PRIVATE /source-charset:utf-8 /execution-charset:utf-8 '
                    f'/Zp{profile["default_pack"]} {wchar_option} "/FI${{SAKURA_REPO_ROOT}}/{abi_header}")'
                )
                definitions = [f"_ITERATOR_DEBUG_LEVEL={profile['iterator_debug_level']}", f"_WIN32_WINNT={profile['win32_winnt']}", "NOMINMAX"]
                if profile["unicode"]:
                    definitions.extend(("UNICODE", "_UNICODE"))
                lines.append(f'target_compile_definitions({component.id} PRIVATE {" ".join(definitions)})')
            elif context.toolchain == "mingw":
                abi_header = _abi_header_path(component.id, context_id).as_posix()
                definitions = [f"_WIN32_WINNT={profile['win32_winnt']}", "NOMINMAX"]
                if profile["unicode"]:
                    definitions.extend(("UNICODE", "_UNICODE"))
                lines.append(f'target_compile_options({component.id} PRIVATE -fpack-struct={profile["default_pack"]} -include "${{SAKURA_REPO_ROOT}}/{abi_header}")')
                lines.append(f'target_compile_definitions({component.id} PRIVATE {" ".join(definitions)})')
        elif component.kind in INTERFACE_COMPONENT_KINDS:
            lines.append(f'target_compile_features({component.id} INTERFACE cxx_std_20)')
        direct_edges = sorted(_component_edges(graph, component.id, context_id), key=lambda item: item.id)
        direct_targets = {edge.target for edge in direct_edges}
        for edge in direct_edges:
            scope = "INTERFACE" if not has_sources else ("PUBLIC" if edge.propagation == "public" else "PRIVATE")
            lines.append(f'target_link_libraries({component.id} {scope} {edge.target})')
        # A private static provider must be repeated at the final native link
        # root.  CMake can carry some PRIVATE link interfaces transitively, but
        # keeping the generated root explicit makes the closure deterministic
        # across CMake and MSBuild and gives the evidence checker a real link
        # provider to inspect.
        if has_sources:
            for dependency_id in _final_link_generated_dependencies(graph, component.id, context_id):
                if dependency_id not in direct_targets:
                    lines.append(f'target_link_libraries({component.id} PRIVATE {dependency_id})')
        if component.system_libraries:
            scope = "INTERFACE" if not has_sources else "PUBLIC"
            lines.append(f'target_link_libraries({component.id} {scope} {" ".join(component.system_libraries)})')
        if component.kind == "test":
            lines.append(f'target_link_options({component.id} PRIVATE /INCREMENTAL:NO "/MAP:${{CMAKE_BINARY_DIR}}/{component.id}.map")')
        lines.append('')
    if root.kind == "test":
        lines.extend([
            'enable_testing()',
            f'add_test(NAME {root.id} COMMAND {root.id})',
            '',
        ])
    return "\n".join(lines)


def _output_hash(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def _manifest_stamp_payload(graph: SemanticGraph, output_hashes: Mapping[str, str]) -> dict[str, object]:
    return {
        "schema_version": graph.schema_version,
        "generator_version": GENERATOR_VERSION,
        "semantic_graph_hash": graph.semantic_graph_hash,
        "contexts": sorted(graph.contexts),
        "output_hashes": dict(sorted(output_hashes.items())),
    }


def _manifest_stamp_text(graph: SemanticGraph, output_hashes: Mapping[str, str]) -> str:
    return _json_text(_manifest_stamp_payload(graph, output_hashes))


def _msbuild_context_text(graph: SemanticGraph, context_id: str) -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
        '  <!-- Generated by tools/build/sakura_build.py. Do not edit. -->\n'
        '  <PropertyGroup>\n'
        f'    <SakuraModulesSchemaVersion>{graph.schema_version}</SakuraModulesSchemaVersion>\n'
        f'    <SakuraModulesGeneratorVersion>{GENERATOR_VERSION}</SakuraModulesGeneratorVersion>\n'
        f'    <SakuraSemanticGraphHash>{graph.semantic_graph_hash}</SakuraSemanticGraphHash>\n'
        f'    <SakuraBuildContextId>{context_id}</SakuraBuildContextId>\n'
        '  </PropertyGroup>\n'
        '</Project>\n'
    )


def _cmake_context_text(graph: SemanticGraph, context_id: str) -> str:
    return (
        '# Generated by tools/build/sakura_build.py. Do not edit.\n'
        f'set(SAKURA_MODULES_SCHEMA_VERSION "{graph.schema_version}")\n'
        f'set(SAKURA_MODULES_GENERATOR_VERSION "{GENERATOR_VERSION}")\n'
        f'set(SAKURA_SEMANTIC_GRAPH_HASH "{graph.semantic_graph_hash}")\n'
        f'set(SAKURA_BUILD_CONTEXT_ID "{context_id}")\n'
    )


def scoped_output_paths(graph: SemanticGraph, component_id: str, context_id: str) -> tuple[Path, ...]:
    """Return files consumed by the selected component's native root.

    The committed stamp owns content hashes. Component builds therefore do not
    regenerate every expected projection merely to prove that a small closure
    is current. Full generation remains the sole owner of unexpected-file and
    repository-wide projection checks.
    """
    context = graph.context(context_id)
    component = graph.components.get(component_id)
    if component is None:
        raise ManifestError("COMPONENT_UNKNOWN", "--component", f"unknown component: {component_id}")
    if context_id not in component.supported_contexts:
        raise ManifestError("COMPONENT_CONTEXT_UNSUPPORTED", "--context", f"component {component_id} does not support {context_id}")
    paths: set[Path] = {GENERATED_ROOT / "manifest.stamp.json"}
    if context.backend == "msbuild":
        paths.add(GENERATED_ROOT / "msbuild" / f"{context_id}.props")
        if component.build_definition == "legacy" and _legacy_generated_link_dependencies(graph, component_id, context_id):
            paths.add(MSBUILD_LEGACY_CONSUMER_ROOT / f"{component_id}.props")
            paths.add(_abi_header_path(component_id, context_id))
        for current_id in graph.final_link_closure((component_id,), context_id):
            current = graph.components.get(current_id)
            if current is None or current.build_definition != "generated":
                continue
            paths.add(_abi_header_path(current.id, context_id))
            targets = current.backend_targets.get("msbuild", ())
            if len(targets) != 1:
                raise ValueError(f"generated component must have exactly one MSBuild target: {current.id}")
            paths.add(Path(targets[0]))
    elif context.backend == "cmake":
        paths.add(GENERATED_ROOT / "cmake" / f"{context_id}.cmake")
        if component.build_definition == "generated":
            paths.add(GENERATED_ROOT / "cmake" / "projects" / context_id / component_id / "CMakeLists.txt")
            for current_id in _cmake_component_order(graph, component_id, context_id):
                current = graph.components[current_id]
                if current.build_definition == "generated":
                    paths.add(_abi_header_path(current.id, context_id))
        elif _legacy_generated_link_dependencies(graph, component_id, context_id):
            paths.add(CMAKE_LEGACY_OWNERSHIP_PATH)
            paths.add(CMAKE_LEGACY_CONSUMER_ROOT / f"{component_id}.cmake")
            paths.add(_abi_header_path(component_id, context_id))
            for current_id in graph.final_link_closure((component_id,), context_id):
                current = graph.components.get(current_id)
                if current is not None and current.build_definition == "generated":
                    paths.add(_abi_header_path(current.id, context_id))
    return tuple(sorted(paths, key=lambda item: item.as_posix()))


def _stale_expected_outputs(graph: SemanticGraph, expected_map: Mapping[Path, str]) -> list[str]:
    stale: list[str] = []
    for relative, expected in expected_map.items():
        path = graph.repo_root / relative
        try:
            actual = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            stale.append(f"missing:{relative.as_posix()}")
            continue
        except UnicodeError:
            stale.append(f"different:{relative.as_posix()}")
            continue
        if actual != expected:
            stale.append(f"different:{relative.as_posix()}")
    return stale


def stale_component_outputs(graph: SemanticGraph, component_id: str, context_id: str) -> list[str]:
    stamp_relative = GENERATED_ROOT / "manifest.stamp.json"
    stamp_path = graph.repo_root / stamp_relative
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return [f"missing:{stamp_relative.as_posix()}"]
    except (UnicodeError, json.JSONDecodeError):
        return [f"different:{stamp_relative.as_posix()}"]
    if not isinstance(stamp, dict):
        return [f"different:{stamp_relative.as_posix()}"]
    output_hashes = stamp.get("output_hashes")
    expected_metadata = _manifest_stamp_payload(graph, output_hashes if isinstance(output_hashes, dict) else {})
    if not isinstance(output_hashes, dict) or stamp != expected_metadata:
        return [f"different:{stamp_relative.as_posix()}"]

    stale: list[str] = []
    for relative in scoped_output_paths(graph, component_id, context_id):
        if relative == stamp_relative:
            continue
        expected_hash = output_hashes.get(relative.as_posix())
        if not isinstance(expected_hash, str):
            stale.append(f"different:{stamp_relative.as_posix()}")
            break
        path = graph.repo_root / relative
        try:
            # Generated projections are text files. Git may materialize them
            # with CRLF even though the canonical generator payload uses LF,
            # so hash the same newline-normalized text used by the full stale
            # check instead of the checkout-specific byte representation.
            actual_hash = _output_hash(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            stale.append(f"missing:{relative.as_posix()}")
            continue
        except UnicodeError:
            stale.append(f"different:{relative.as_posix()}")
            continue
        if actual_hash != expected_hash:
            stale.append(f"different:{relative.as_posix()}")
    return stale


def expected_outputs(graph: SemanticGraph) -> Mapping[Path, str]:
    outputs: dict[Path, str] = {}
    for context_id in sorted(graph.contexts):
        projection = graph.project(context_id)
        outputs[GENERATED_ROOT / "context" / f"{context_id}.json"] = _json_text(projection)
        outputs[GENERATED_ROOT / "msbuild" / f"{context_id}.props"] = _msbuild_context_text(graph, context_id)
        outputs[GENERATED_ROOT / "cmake" / f"{context_id}.cmake"] = _cmake_context_text(graph, context_id)
    for component in sorted(graph.components.values(), key=lambda item: item.id):
        if component.build_definition != "generated":
            continue
        msbuild_targets = component.backend_targets.get("msbuild", ())
        if msbuild_targets:
            if len(msbuild_targets) != 1:
                raise ValueError(f"generated component must have exactly one MSBuild target: {component.id}")
            project_path = Path(msbuild_targets[0])
            outputs[project_path] = _msbuild_project(graph, component, project_path)
        for context_id in sorted(component.supported_contexts):
            outputs[_abi_header_path(component.id, context_id)] = _abi_header_text(graph, component.id, context_id)
            if graph.contexts[context_id].backend == "cmake":
                project_path = GENERATED_ROOT / "cmake" / "projects" / context_id / component.id / "CMakeLists.txt"
                outputs[project_path] = _cmake_project(graph, component, context_id)
    for component in sorted(graph.components.values(), key=lambda item: item.id):
        if component.build_definition != "legacy":
            continue
        msbuild_contexts = _legacy_consumer_contexts(graph, component, "msbuild")
        if msbuild_contexts:
            outputs[MSBUILD_LEGACY_CONSUMER_ROOT / f"{component.id}.props"] = _msbuild_legacy_consumer_projection(graph, component)
            for context in msbuild_contexts:
                outputs[_abi_header_path(component.id, context.id)] = _abi_header_text(graph, component.id, context.id)
        cmake_contexts = _legacy_consumer_contexts(graph, component, "cmake")
        if cmake_contexts:
            outputs[CMAKE_LEGACY_CONSUMER_ROOT / f"{component.id}.cmake"] = _cmake_legacy_consumer_projection(graph, component)
            for context in cmake_contexts:
                outputs[_abi_header_path(component.id, context.id)] = _abi_header_text(graph, component.id, context.id)
    if _cmake_legacy_providers(graph):
        outputs[CMAKE_LEGACY_OWNERSHIP_PATH] = _cmake_legacy_source_ownership(graph)
    output_hashes = {relative.as_posix(): _output_hash(text) for relative, text in outputs.items()}
    outputs[GENERATED_ROOT / "manifest.stamp.json"] = _manifest_stamp_text(graph, output_hashes)
    return dict(sorted(outputs.items(), key=lambda item: item[0].as_posix()))


def stale_outputs(graph: SemanticGraph) -> list[str]:
    expected_map = expected_outputs(graph)
    stale = _stale_expected_outputs(graph, expected_map)
    generated_root = graph.repo_root / GENERATED_ROOT
    if generated_root.is_dir():
        expected_paths = {(graph.repo_root / relative).resolve() for relative in expected_map}
        for path in sorted((item for item in generated_root.rglob("*") if item.is_file()), key=lambda item: item.as_posix()):
            if path.resolve() not in expected_paths:
                stale.append(f"unexpected:{path.relative_to(graph.repo_root).as_posix()}")
    return stale


def _atomic_write_if_different(path: Path, text: str) -> bool:
    try:
        if path.read_text(encoding="utf-8") == text:
            return False
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True


def generate(graph: SemanticGraph) -> list[str]:
    changed: list[str] = []
    expected_map = expected_outputs(graph)
    for relative, text in expected_map.items():
        if _atomic_write_if_different(graph.repo_root / relative, text):
            changed.append(relative.as_posix())
    generated_root = (graph.repo_root / GENERATED_ROOT).resolve()
    expected_paths = {(graph.repo_root / relative).resolve() for relative in expected_map}
    if generated_root.is_dir():
        for path in sorted((item for item in generated_root.rglob("*") if item.is_file()), key=lambda item: item.as_posix()):
            resolved = path.resolve()
            if resolved not in expected_paths:
                resolved.relative_to(generated_root)
                path.unlink()
                changed.append(f"removed:{path.relative_to(graph.repo_root).as_posix()}")
    return changed
