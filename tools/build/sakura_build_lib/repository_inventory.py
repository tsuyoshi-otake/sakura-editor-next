"""Root-independent repository dependency/provenance inventory.

The R0 inventory is deliberately observational.  A successful collection does
not imply that the repository is ready to graduate: unclassified edges and
unobserved dependency classes remain explicit blockers in the result.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path
from typing import Mapping

from .model import SemanticGraph
from .product_native_evidence import validate_product_native_evidence
from .resource_native_evidence import validate_resource_native_evidence
from .runner import BuildError


CPP_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})
RESOURCE_SUFFIXES = frozenset({".rc", ".rc2"})
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
_PRAGMA_LIB_RE = re.compile(
    r'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"',
    re.IGNORECASE,
)


def _sha256_json(value: object) -> str:
    serialized = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(serialized.encode("utf-8")).hexdigest()


def _read_text(path: Path, relative: str, code: str) -> str:
    try:
        data = path.read_bytes()
        encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        return data.decode(encoding, errors="replace")
    except OSError as error:
        raise BuildError(code, f"could not read {relative}: {error}", 5) from error


def _relative(repo_root: Path, path: Path) -> str | None:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except (OSError, ValueError):
        return None


def _is_below(relative: str, root: str) -> bool:
    normalized = root.rstrip("/")
    return relative == normalized or relative.startswith(normalized + "/")


def _component_owner(graph: SemanticGraph, relative: str) -> str | None:
    explicit: list[tuple[int, str]] = []
    directory: list[tuple[int, str]] = []
    for component in graph.components.values():
        owned_files = set(component.sources) | set(component.public_headers) | set(component.private_headers)
        if relative in owned_files:
            explicit.append((len(relative), component.id))
        owner = component.owner.rstrip("/")
        if _is_below(relative, owner) and not any(_is_below(relative, item) for item in component.ownership_exclusions):
            directory.append((len(owner), component.id))
    candidates = explicit or directory
    if not candidates:
        return None
    candidates.sort(key=lambda item: (-item[0], item[1]))
    return candidates[0][1]


def _walk_repository_files(graph: SemanticGraph) -> tuple[Path, ...]:
    """Enumerate repository C/C++ and resource inputs once, without output trees."""

    files: dict[str, Path] = {}
    output_roots = {"build", "x64", "win32", "mingw"}
    for current, directories, names in os.walk(graph.repo_root, followlinks=False):
        current_path = Path(current)
        at_root = current_path == graph.repo_root
        directories[:] = sorted(
            name
            for name in directories
            if not Path(current, name).is_symlink()
            and name not in {".git", ".vs", "node_modules", "__pycache__", ".pytest_cache"}
            and not (at_root and name.lower() in output_roots)
            and _relative(graph.repo_root, Path(current, name)) != "tools/vcpkg"
        )
        for name in sorted(names):
            path = Path(current, name)
            if path.suffix.lower() not in CPP_SUFFIXES | RESOURCE_SUFFIXES:
                continue
            relative = _relative(graph.repo_root, path)
            if relative is not None:
                files[relative.lower()] = path
    return tuple(files[key] for key in sorted(files))


def _requires_component_owner(relative: str) -> bool:
    """Limit source ownership graduation to product/test/tool source domains.

    Vendored sources under externals are package inputs, not Sakura components.
    They remain resolvable as include targets and are covered by package
    provenance instead of inflating the component ownership set.
    """

    if relative.startswith("tools/vcpkg/"):
        return False
    return any(
        _is_below(relative, root)
        for root in ("sakura_core", "sakura_lang", "src/main", "src/test", "tools")
    )


def _include_target(
    graph: SemanticGraph,
    source_relative: str,
    include_name: str,
    repo_files: Mapping[str, str],
    suffix_index: Mapping[str, tuple[str, ...]],
) -> str | None:
    raw = include_name.replace("\\", "/")
    normalized = raw[2:] if raw.startswith("./") else raw
    direct_candidates = [
        (Path(source_relative).parent / raw).as_posix(),
        normalized,
        f"sakura_core/{normalized}",
        f"src/{normalized}",
    ]
    for component in graph.components.values():
        for root in (*component.public_include_roots, *component.private_include_roots):
            direct_candidates.append((Path(root) / normalized).as_posix())
    for candidate in direct_candidates:
        match = repo_files.get(candidate.lower())
        if match is not None:
            return match
        candidate_path = graph.repo_root / candidate
        if candidate_path.is_file():
            relative = _relative(graph.repo_root, candidate_path)
            if relative is not None:
                return relative
    matches = suffix_index.get(normalized.lower(), ())
    return matches[0] if len(matches) == 1 else None


def _source_inventory(graph: SemanticGraph) -> dict[str, object]:
    repository_files = _walk_repository_files(graph)
    files: list[Path] = []
    unowned_repo_files: list[str] = []
    repo_files: dict[str, str] = {}
    suffixes: dict[str, set[str]] = defaultdict(set)
    for path in repository_files:
        relative = _relative(graph.repo_root, path)
        if relative is None:
            continue
        repo_files[relative.lower()] = relative
        owner = _component_owner(graph, relative)
        if owner is None and _requires_component_owner(relative):
            unowned_repo_files.append(relative)
        if owner is not None:
            files.append(path)
        parts = relative.split("/")
        for index in range(len(parts)):
            suffixes["/".join(parts[index:]).lower()].add(relative)
    suffix_index = {key: tuple(sorted(values)) for key, values in suffixes.items()}

    observed: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    unowned_repo_includes: list[dict[str, object]] = []
    local_unresolved: list[dict[str, object]] = []
    external_includes: set[str] = set()
    pragma_libraries: list[dict[str, object]] = []
    resource_files: list[str] = []
    include_count = 0
    scanned_cpp = 0

    for path in files:
        relative = _relative(graph.repo_root, path)
        if relative is None:
            continue
        if path.suffix.lower() in RESOURCE_SUFFIXES:
            resource_files.append(relative)
            continue
        scanned_cpp += 1
        owner = _component_owner(graph, relative)
        text = _read_text(path, relative, "INVENTORY_SOURCE_READ")
        for line_number, line in enumerate(text.splitlines(), 1):
            include = _INCLUDE_RE.match(line)
            if include is not None:
                include_count += 1
                delimiter, include_name = include.groups()
                target = _include_target(graph, relative, include_name, repo_files, suffix_index)
                if target is not None:
                    provider = _component_owner(graph, target)
                    if owner is not None and provider is not None and owner != provider:
                        observed[(owner, provider)].append({
                            "source": relative,
                            "line": line_number,
                            "include": include_name,
                            "target": target,
                        })
                    elif owner is not None and provider is None:
                        unowned_repo_includes.append({
                            "consumer": owner,
                            "source": relative,
                            "line": line_number,
                            "include": include_name,
                            "target": target,
                        })
                elif delimiter == '"':
                    local_unresolved.append({"source": relative, "line": line_number, "include": include_name})
                else:
                    external_includes.add(include_name)
            pragma = _PRAGMA_LIB_RE.search(line)
            if pragma is not None:
                pragma_libraries.append({
                    "source": relative,
                    "line": line_number,
                    "library": pragma.group(1),
                    "owner": owner,
                })

    declared_compile_edges = {
        (edge.source, edge.target)
        for edge in graph.edges
        if "compile" in edge.phases
    }
    cross_edges: list[dict[str, object]] = []
    undeclared: list[dict[str, object]] = []
    for (consumer, provider), witnesses in sorted(observed.items()):
        record = {
            "consumer": consumer,
            "provider": provider,
            "declared": (consumer, provider) in declared_compile_edges,
            "witness_count": len(witnesses),
            "witnesses": sorted(witnesses, key=lambda item: (str(item["source"]), int(item["line"]))),
        }
        cross_edges.append(record)
        if not record["declared"]:
            undeclared.append(record)

    return {
        "repository_cpp_resource_files": len(repository_files),
        "scanned_cpp_files": scanned_cpp,
        "include_directive_count": include_count,
        "observed_cross_component_edges": cross_edges,
        "undeclared_cross_component_edges": undeclared,
        "unowned_repo_files": sorted(unowned_repo_files),
        "unowned_repo_includes": sorted(
            unowned_repo_includes,
            key=lambda item: (str(item["consumer"]), str(item["source"]), int(item["line"])),
        ),
        "unresolved_quoted_includes": sorted(local_unresolved, key=lambda item: (str(item["source"]), int(item["line"]))),
        "external_include_names": sorted(external_includes),
        "source_link_directives": sorted(pragma_libraries, key=lambda item: (str(item["source"]), int(item["line"]))),
        "resource_files": sorted(resource_files),
    }


def _msbuild_items(repo_root: Path, project_relative: str, item_name: str) -> tuple[str, ...]:
    result: set[str] = set()
    for item in _msbuild_item_records(repo_root, project_relative, item_name):
        relative = item["literal_repository_path"]
        if relative is not None:
            result.add(str(relative))
    return tuple(sorted(result))


def _parse_msbuild(repo_root: Path, project_relative: str) -> ET.Element:
    project = repo_root / project_relative
    try:
        return ET.parse(project).getroot()
    except (ET.ParseError, OSError) as error:
        raise BuildError("INVENTORY_MSBUILD_PARSE", f"could not parse {project_relative}: {error}", 5) from error


def _msbuild_item_records(repo_root: Path, project_relative: str, item_name: str) -> tuple[dict[str, object], ...]:
    project = repo_root / project_relative
    root = _parse_msbuild(repo_root, project_relative)
    result: list[dict[str, object]] = []
    for item in root.findall(f".//{{*}}{item_name}"):
        include = item.get("Include")
        if not include:
            continue
        literal: str | None = None
        contains_expression = any(marker in include for marker in ("$", "%", "@"))
        if not contains_expression:
            literal = _relative(repo_root, project.parent / Path(include.replace("\\", "/")))
        result.append({
            "project": project_relative,
            "include": include,
            "condition": item.get("Condition"),
            "contains_expression": contains_expression,
            "literal_repository_path": literal,
        })
    return tuple(sorted(result, key=lambda item: (str(item["include"]), str(item["condition"] or ""))))


def _split_msbuild_list(value: str | None) -> list[str]:
    if not value:
        return []
    return [item.strip() for item in value.split(";") if item.strip()]


def _output_basename(value: str) -> str:
    normalized = value.strip().strip('"').replace("\\", "/").rstrip("/")
    return normalized.rsplit("/", 1)[-1].lower()


def _component_msbuild_projects(graph: SemanticGraph) -> tuple[tuple[str, str], ...]:
    projects: set[tuple[str, str]] = set()
    for component in graph.components.values():
        for project in component.backend_targets.get("msbuild", ()):
            if project.lower().endswith(".vcxproj"):
                projects.add((component.id, project))
    return tuple(sorted(projects))


def _literal_msbuild_imports(repo_root: Path, project_relative: str) -> tuple[str, ...]:
    project = repo_root / project_relative
    root = _parse_msbuild(repo_root, project_relative)
    imports: set[str] = set()
    for item in root.findall(".//{*}Import"):
        include = item.get("Project")
        if not include or any(marker in include for marker in ("$", "%", "@")):
            continue
        relative = _relative(repo_root, project.parent / Path(include.replace("\\", "/")))
        if relative is not None:
            imports.add(relative)
    return tuple(sorted(imports))


def _msbuild_target_records(
    repo_root: Path,
    project_relative: str,
    *,
    imported_targets: tuple[str, ...] = (),
) -> tuple[dict[str, object], ...]:
    records: list[dict[str, object]] = []
    for source in (project_relative, *imported_targets):
        path = repo_root / source
        if not path.is_file():
            continue
        root = _parse_msbuild(repo_root, source)
        for target in root.findall(".//{*}Target"):
            name = target.get("Name") or ""
            outputs = _split_msbuild_list(target.get("Outputs"))
            inputs = _split_msbuild_list(target.get("Inputs"))
            exec_commands = [
                command
                for task in target.findall(".//{*}Exec")
                if (command := task.get("Command"))
            ]
            if not outputs and not inputs and not exec_commands and not name.lower().startswith(("generate", "make", "copy")):
                continue
            records.append({
                "source": source,
                "target": name,
                "condition": target.get("Condition"),
                "inputs": inputs,
                "outputs": outputs,
                "before_targets": _split_msbuild_list(target.get("BeforeTargets")),
                "after_targets": _split_msbuild_list(target.get("AfterTargets")),
                "depends_on_targets": _split_msbuild_list(target.get("DependsOnTargets")),
                "exec_commands": exec_commands,
            })
    return tuple(sorted(records, key=lambda item: (str(item["source"]), str(item["target"]))))


def _cmake_command_blocks(text: str, command_name: str) -> tuple[str, ...]:
    blocks: list[str] = []
    pattern = re.compile(rf"\b{re.escape(command_name)}\s*\(", re.IGNORECASE)
    position = 0
    while match := pattern.search(text, position):
        depth = 1
        cursor = match.end()
        quoted = False
        escaped = False
        while cursor < len(text) and depth:
            character = text[cursor]
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = not quoted
            elif not quoted and character == "(":
                depth += 1
            elif not quoted and character == ")":
                depth -= 1
            cursor += 1
        if depth:
            break
        blocks.append(text[match.end():cursor - 1])
        position = cursor
    return tuple(blocks)


def _cmake_section(block: str, section: str) -> list[str]:
    boundary = (
        "OUTPUT|COMMAND|MAIN_DEPENDENCY|DEPENDS|BYPRODUCTS|IMPLICIT_DEPENDS|WORKING_DIRECTORY|"
        "COMMENT|DEPFILE|JOB_POOL|JOB_SERVER_AWARE|VERBATIM|APPEND|USES_TERMINAL|COMMAND_EXPAND_LISTS|CODEGEN"
    )
    match = re.search(
        rf"(?:^|\s){section}\s+(.+?)(?=\s+(?:{boundary})(?:\s|$)|$)",
        block,
        re.IGNORECASE | re.DOTALL,
    )
    if match is None:
        return []
    try:
        return [token for token in shlex.split(match.group(1), posix=True) if token]
    except ValueError:
        return [token.strip('"') for token in match.group(1).split() if token]


def _generated_provenance(graph: SemanticGraph, source: Mapping[str, object]) -> dict[str, object]:
    projects = _component_msbuild_projects(graph)
    generated_headers: list[dict[str, object]] = []
    msbuild_targets: list[dict[str, object]] = []
    for component_id, project in projects:
        for item in _msbuild_item_records(graph.repo_root, project, "ClInclude"):
            include = str(item["include"])
            if item["contains_expression"] or _output_basename(include) in {
                "version.h", "funccode_define.h", "funccode_enum.h", "githash.h"
            }:
                generated_headers.append({"consumer": component_id, **item})
        imported_targets = tuple(
            item for item in _literal_msbuild_imports(graph.repo_root, project) if item.lower().endswith((".targets", ".props"))
        )
        for target in _msbuild_target_records(graph.repo_root, project, imported_targets=imported_targets):
            if target["outputs"] or str(target["target"]).lower().startswith(("generate", "make", "copy")):
                msbuild_targets.append({"consumer": component_id, **target})

    cmake_commands: list[dict[str, object]] = []
    cmake_roots = [graph.repo_root / "CMakeLists.txt", graph.repo_root / "src/main/cmake"]
    for root in cmake_roots:
        paths = [root] if root.is_file() else sorted(root.glob("*.cmake")) if root.is_dir() else []
        for path in paths:
            relative = _relative(graph.repo_root, path)
            if relative is None:
                continue
            text = _read_text(path, relative, "INVENTORY_CMAKE_READ")
            for index, block in enumerate(_cmake_command_blocks(text, "add_custom_command"), 1):
                outputs = _cmake_section(block, "OUTPUT")
                if not outputs:
                    continue
                cmake_commands.append({
                    "source": relative,
                    "ordinal": index,
                    "outputs": outputs,
                    "inputs": _cmake_section(block, "DEPENDS"),
                    "commands": _cmake_section(block, "COMMAND"),
                })

    declared_outputs: dict[str, list[dict[str, object]]] = defaultdict(list)
    for item in generated_headers:
        declared_outputs[_output_basename(str(item["include"]))].append({
            "backend": "msbuild-item", "source": item["project"], "output": item["include"]
        })
    for target in msbuild_targets:
        for output in target["outputs"]:
            declared_outputs[_output_basename(str(output))].append({
                "backend": "msbuild-target", "source": target["source"], "target": target["target"], "output": output
            })
    for command in cmake_commands:
        for output in command["outputs"]:
            declared_outputs[_output_basename(str(output))].append({
                "backend": "cmake", "source": command["source"], "ordinal": command["ordinal"], "output": output
            })

    generated_includes: list[dict[str, object]] = []
    unclassified: list[dict[str, object]] = []
    for include in source["unresolved_quoted_includes"]:
        matches = sorted(
            declared_outputs.get(_output_basename(str(include["include"])), []),
            key=lambda item: (str(item["backend"]), str(item["source"]), str(item["output"])),
        )
        record = {**include, "declared_outputs": matches}
        if matches:
            generated_includes.append(record)
        else:
            unclassified.append(record)

    legacy_scripts: list[dict[str, object]] = []
    for relative in ("tools/githash.bat",):
        path = graph.repo_root / relative
        if not path.is_file():
            continue
        text = _read_text(path, relative, "INVENTORY_GENERATOR_READ")
        outputs = sorted(set(match.group(1) for match in re.finditer(r"([A-Za-z0-9_.-]+\.h)\b", text, re.IGNORECASE)))
        legacy_scripts.append({"source": relative, "declared_header_names": outputs})

    declared_input_gaps = [
        {
            "backend": "cmake",
            "source": command["source"],
            "ordinal": command["ordinal"],
            "outputs": command["outputs"],
        }
        for command in cmake_commands
        if command["outputs"] and not command["inputs"]
    ]
    declared_input_gaps.extend(
        {
            "backend": "msbuild",
            "source": target["source"],
            "target": target["target"],
            "outputs": target["outputs"],
        }
        for target in msbuild_targets
        if target["outputs"] and not target["inputs"]
    )

    return {
        "msbuild_generated_header_items": sorted(generated_headers, key=lambda item: (str(item["consumer"]), str(item["include"]))),
        "msbuild_generation_targets": sorted(msbuild_targets, key=lambda item: (str(item["consumer"]), str(item["source"]), str(item["target"]))),
        "cmake_custom_commands": sorted(cmake_commands, key=lambda item: (str(item["source"]), int(item["ordinal"]))),
        "legacy_generator_scripts": legacy_scripts,
        "classified_unresolved_generated_includes": sorted(generated_includes, key=lambda item: (str(item["source"]), int(item["line"]))),
        "unclassified_unresolved_quoted_includes": sorted(unclassified, key=lambda item: (str(item["source"]), int(item["line"]))),
        "declared_input_gaps": sorted(
            declared_input_gaps,
            key=lambda item: (
                str(item["backend"]),
                str(item["source"]),
                str(item.get("target", "")),
                int(item.get("ordinal", 0)),
            ),
        ),
        "native_execution_observed": False,
    }


def _resource_provenance(graph: SemanticGraph, generated: Mapping[str, object]) -> dict[str, object]:
    repository_files = _walk_repository_files(graph)
    resource_paths = [path for path in repository_files if path.suffix.lower() in RESOURCE_SUFFIXES]
    repo_files: dict[str, str] = {}
    suffixes: dict[str, set[str]] = defaultdict(set)
    for path in repository_files:
        relative = _relative(graph.repo_root, path)
        if relative is None:
            continue
        repo_files[relative.lower()] = relative
        parts = relative.split("/")
        for index in range(len(parts)):
            suffixes["/".join(parts[index:]).lower()].add(relative)
    suffix_index = {key: tuple(sorted(values)) for key, values in suffixes.items()}

    includes: list[dict[str, object]] = []
    unresolved: list[dict[str, object]] = []
    vendored_unresolved: list[dict[str, object]] = []
    generated_includes: list[dict[str, object]] = []
    shared_resource_header_consumers: set[str] = set()
    generated_output_names: set[str] = set()
    for item in generated["msbuild_generated_header_items"]:
        generated_output_names.add(_output_basename(str(item["include"])))
    for target in generated["msbuild_generation_targets"]:
        generated_output_names.update(_output_basename(str(output)) for output in target["outputs"])
    for command in generated["cmake_custom_commands"]:
        generated_output_names.update(_output_basename(str(output)) for output in command["outputs"])
    for path in resource_paths:
        relative = _relative(graph.repo_root, path)
        if relative is None:
            continue
        for line_number, line in enumerate(_read_text(path, relative, "INVENTORY_RESOURCE_READ").splitlines(), 1):
            include = _INCLUDE_RE.match(line)
            if include is None:
                continue
            delimiter, include_name = include.groups()
            target = _include_target(graph, relative, include_name, repo_files, suffix_index)
            record = {
                "consumer_resource": relative,
                "consumer_owner": _component_owner(graph, relative),
                "line": line_number,
                "include": include_name,
                "provider": target,
                "provider_owner": _component_owner(graph, target) if target else None,
            }
            if target is not None:
                includes.append(record)
                if target.lower().endswith("/sakura_rc.h") or target.lower() == "sakura_rc.h":
                    shared_resource_header_consumers.add(relative)
            elif delimiter == '"':
                if not _requires_component_owner(relative):
                    vendored_unresolved.append(record)
                elif _output_basename(include_name) in generated_output_names:
                    generated_includes.append(record)
                else:
                    unresolved.append(record)

    compile_items: list[dict[str, object]] = []
    for component_id, project in _component_msbuild_projects(graph):
        for item in _msbuild_item_records(graph.repo_root, project, "ResourceCompile"):
            compile_items.append({"consumer": component_id, **item})
    return {
        "repository_resource_files": sorted(_relative(graph.repo_root, path) for path in resource_paths if _relative(graph.repo_root, path) is not None),
        "unowned_resource_files": sorted(
            relative
            for path in resource_paths
            if (relative := _relative(graph.repo_root, path)) is not None
            and _requires_component_owner(relative)
            and _component_owner(graph, relative) is None
        ),
        "vendored_resource_files": sorted(
            relative
            for path in resource_paths
            if (relative := _relative(graph.repo_root, path)) is not None and not _requires_component_owner(relative)
        ),
        "msbuild_resource_compile_items": sorted(compile_items, key=lambda item: (str(item["consumer"]), str(item["include"]))),
        "resource_include_edges": sorted(includes, key=lambda item: (str(item["consumer_resource"]), int(item["line"]))),
        "generated_resource_includes": sorted(generated_includes, key=lambda item: (str(item["consumer_resource"]), int(item["line"]))),
        "unresolved_resource_includes": sorted(unresolved, key=lambda item: (str(item["consumer_resource"]), int(item["line"]))),
        "vendored_unresolved_resource_includes": sorted(vendored_unresolved, key=lambda item: (str(item["consumer_resource"]), int(item["line"]))),
        "canonical_sakura_rc_header_consumers": sorted(shared_resource_header_consumers),
        "semantic_resource_artifacts": sorted(artifact.id for artifact in graph.artifacts.values() if artifact.artifact_kind == "resource"),
        "native_resource_table_observed": False,
        "native_resource_id_compatibility_observed": False,
        "native_resource_table": {},
    }


def _product_link_provenance(graph: SemanticGraph) -> dict[str, object]:
    link_dependencies: list[dict[str, object]] = []
    project_references: list[dict[str, object]] = []
    object_aggregation: list[dict[str, object]] = []
    for component_id, project in _component_msbuild_projects(graph):
        root = _parse_msbuild(graph.repo_root, project)
        for link in root.findall(".//{*}Link"):
            for dependencies in link.findall("{*}AdditionalDependencies"):
                raw = dependencies.text or ""
                tokens = _split_msbuild_list(raw)
                link_dependencies.append({
                    "consumer": component_id,
                    "project": project,
                    "condition": link.get("Condition"),
                    "raw": raw,
                    "literal_libraries": sorted(token for token in tokens if token.lower().endswith(".lib") and not any(marker in token for marker in ("$", "%", "@"))),
                    "expression_tokens": sorted(token for token in tokens if any(marker in token for marker in ("$", "%", "@"))),
                })
        for reference in _msbuild_item_records(graph.repo_root, project, "ProjectReference"):
            project_references.append({"consumer": component_id, **reference})
        for target in root.findall(".//{*}Target"):
            body = ET.tostring(target, encoding="unicode")
            links_item_vector = bool(re.search(r"<[^>]*Link\s+Include=\"@\(", body, re.IGNORECASE))
            reads_link_manifest = target.find(".//{*}ReadLinesFromFile") is not None
            production_object_name = "sakura" in (target.get("Name") or "").lower() and "object" in (target.get("Name") or "").lower()
            if links_item_vector and (reads_link_manifest or production_object_name or "SakuraLinkInputForTests1" in body):
                object_aggregation.append({
                    "consumer": component_id,
                    "project": project,
                    "target": target.get("Name") or "",
                    "reads_link_input_manifest": reads_link_manifest,
                    "link_item_vector": True,
                })
    semantic_link_edges = sorted(
        edge.id for edge in graph.edges if "link" in edge.phases
    )
    return {
        "msbuild_additional_dependencies": sorted(link_dependencies, key=lambda item: (str(item["consumer"]), str(item["project"]), str(item["raw"]))),
        "msbuild_project_references": sorted(project_references, key=lambda item: (str(item["consumer"]), str(item["include"]))),
        "test_product_object_aggregation": sorted(object_aggregation, key=lambda item: (str(item["consumer"]), str(item["project"]), str(item["target"]))),
        "semantic_link_edges": semantic_link_edges,
        "native_product_link_closure_observed": False,
    }


def _product_reachability(
    graph: SemanticGraph,
    source_inventory: Mapping[str, object],
    product_id: str,
    provider_id: str,
    context_id: str,
) -> dict[str, object]:
    product = graph.components.get(product_id)
    provider = graph.components.get(provider_id)
    if product is None or provider is None:
        raise BuildError("INVENTORY_COMPONENT", f"unknown product/provider: {product_id} -> {provider_id}", 2)
    context = graph.context(context_id)
    if context.backend != "msbuild":
        raise BuildError("INVENTORY_CONTEXT", "product reachability pilot requires an MSBuild context", 2)
    product_targets = product.backend_targets.get("msbuild", ())
    provider_targets = provider.backend_targets.get("msbuild", ())
    if len(product_targets) != 1 or len(provider_targets) != 1:
        raise BuildError("INVENTORY_TARGET", "product/provider must each have exactly one MSBuild target", 2)

    compiled = set(_msbuild_items(graph.repo_root, product_targets[0], "ClCompile"))
    references = set(_msbuild_items(graph.repo_root, product_targets[0], "ProjectReference"))
    provider_project = provider_targets[0]
    direct_sources = sorted(set(provider.sources) & compiled)
    has_project_reference = provider_project in references
    active = graph.active_edges(context_id)
    declared_edges = [
        edge.id
        for edge in active
        if edge.source == product_id
        and edge.target == provider_id
        and {"compile", "link"}.issubset(set(edge.phases))
    ]
    include_witnesses: list[dict[str, object]] = []
    for edge in source_inventory["observed_cross_component_edges"]:
        if edge["consumer"] == product_id and edge["provider"] == provider_id:
            include_witnesses.extend(edge["witnesses"])

    cmake_globs: list[dict[str, object]] = []
    cmake_candidates = [graph.repo_root / "CMakeLists.txt", graph.repo_root / "src/main/cmake"]
    for candidate in cmake_candidates:
        paths = [candidate] if candidate.is_file() else sorted(candidate.glob("*.cmake")) if candidate.is_dir() else []
        for path in paths:
            relative = _relative(graph.repo_root, path)
            if relative is None:
                continue
            for line_number, line in enumerate(_read_text(path, relative, "INVENTORY_CMAKE_READ").splitlines(), 1):
                if re.search(r"\bGLOB_RECURSE\b", line, re.IGNORECASE):
                    cmake_globs.append({"source": relative, "line": line_number, "text": line.strip()})

    independent = bool(declared_edges) and has_project_reference and not direct_sources and not cmake_globs
    if direct_sources:
        status = "embedded_in_product"
    elif not declared_edges or not has_project_reference:
        status = "not_connected_through_declared_provider"
    elif cmake_globs:
        status = "cmake_source_ownership_opaque"
    else:
        status = "independent_provider"
    return {
        "consumer": product_id,
        "provider": provider_id,
        "context_id": context_id,
        "status": status,
        "independent": independent,
        "declared_compile_link_edges": sorted(declared_edges),
        "include_witnesses": sorted(include_witnesses, key=lambda item: (str(item["source"]), int(item["line"]))),
        "msbuild": {
            "product_project": product_targets[0],
            "provider_project": provider_project,
            "provider_project_reference": has_project_reference,
            "provider_sources_compiled_directly": direct_sources,
        },
        "cmake": {
            "glob_recurse_witnesses": sorted(cmake_globs, key=lambda item: (str(item["source"]), int(item["line"]))),
            "source_ownership_verified": not cmake_globs,
        },
    }


def _package_provenance(graph: SemanticGraph) -> dict[str, object]:
    manifest_path = graph.repo_root / "vcpkg.json"
    packages: list[str] = []
    if manifest_path.is_file():
        try:
            value = json.loads(_read_text(manifest_path, "vcpkg.json", "INVENTORY_PACKAGE_MANIFEST"))
        except json.JSONDecodeError as error:
            raise BuildError(
                "INVENTORY_PACKAGE_MANIFEST",
                f"could not parse vcpkg.json: {error.msg} at line {error.lineno}, column {error.colno}",
                5,
            ) from error
        for dependency in value.get("dependencies", []):
            if isinstance(dependency, str):
                packages.append(dependency)
            elif isinstance(dependency, dict) and isinstance(dependency.get("name"), str):
                packages.append(dependency["name"])
    package_sets = sorted(artifact.id for artifact in graph.artifacts.values() if artifact.artifact_kind == "package_set")
    package_edges = sorted(edge.id for edge in graph.edges if edge.kind == "package")
    package_set_members: dict[str, list[str]] = {}
    classified_package_names: set[str] = set()
    for artifact in graph.artifacts.values():
        if artifact.artifact_kind != "package_set":
            continue
        members = sorted(set(artifact.inputs) | set(artifact.outputs))
        package_set_members[artifact.id] = members
        for member in members:
            normalized = member.lower()
            if normalized.startswith("vcpkg:"):
                normalized = normalized.split(":", 1)[1]
            classified_package_names.add(normalized)
    unclassified_packages = sorted(set(packages) - classified_package_names)
    props_path = graph.repo_root / "Directory.Build.props"
    props_text = _read_text(props_path, "Directory.Build.props", "INVENTORY_PACKAGE_PROPS") if props_path.is_file() else ""
    targets_path = graph.repo_root / "Directory.Build.targets"
    targets_text = _read_text(targets_path, "Directory.Build.targets", "INVENTORY_PACKAGE_TARGETS") if targets_path.is_file() else ""
    manifest_enabled = bool(re.search(r"<VcpkgEnableManifest>\s*true\s*</VcpkgEnableManifest>", props_text, re.IGNORECASE))
    property_values: dict[str, list[str]] = {}
    if props_path.is_file():
        props_root = _parse_msbuild(graph.repo_root, "Directory.Build.props")
        for property_name in ("VcpkgEnableManifest", "VcpkgManifestRoot", "VcpkgInstalledDir", "VcpkgTriplet"):
            property_values[property_name] = sorted({
                (element.text or "").strip()
                for element in props_root.findall(f".//{{*}}{property_name}")
                if (element.text or "").strip()
            })
    imports: list[dict[str, object]] = []
    for relative, text in (("Directory.Build.props", props_text), ("Directory.Build.targets", targets_text)):
        if not text:
            continue
        root = _parse_msbuild(graph.repo_root, relative)
        for imported in root.findall(".//{*}Import"):
            project = imported.get("Project")
            if project and "vcpkg" in project.lower():
                imports.append({"source": relative, "project": project, "condition": imported.get("Condition")})

    manifest_install_targets: list[dict[str, object]] = []
    vendored_targets_relative = "tools/vcpkg/scripts/buildsystems/msbuild/vcpkg.targets"
    vendored_targets = graph.repo_root / vendored_targets_relative
    if vendored_targets.is_file():
        root = _parse_msbuild(graph.repo_root, vendored_targets_relative)
        for target in root.findall(".//{*}Target"):
            before = _split_msbuild_list(target.get("BeforeTargets"))
            if "ClCompile" in before and "manifest" in (target.get("Name") or "").lower():
                manifest_install_targets.append({
                    "source": vendored_targets_relative,
                    "target": target.get("Name") or "",
                    "condition": target.get("Condition"),
                    "before_targets": before,
                })
    global_scope = manifest_enabled and bool(imports)
    isolated = not global_scope and not unclassified_packages
    return {
        "root_manifest": "vcpkg.json" if manifest_path.is_file() else None,
        "root_manifest_packages": sorted(set(packages)),
        "declared_package_sets": package_sets,
        "declared_package_set_members": dict(sorted(package_set_members.items())),
        "declared_package_edges": package_edges,
        "global_msbuild_manifest_enabled": manifest_enabled,
        "directory_build_properties": property_values,
        "directory_build_vcpkg_imports": sorted(imports, key=lambda item: (str(item["source"]), str(item["project"]))),
        "manifest_install_before_compile": sorted(manifest_install_targets, key=lambda item: (str(item["source"]), str(item["target"]))),
        "global_restore_scope": global_scope,
        "component_package_isolation_verified": isolated,
        "root_manifest_unclassified_packages": unclassified_packages,
        "root_manifest_classified": not unclassified_packages,
        "native_restore_observed": False,
    }


def collect_repository_inventory(
    graph: SemanticGraph,
    *,
    product_id: str = "sakura_app",
    provider_id: str = "sakura_uri",
    context_id: str = "msvc-x64-debug",
    native_evidence_path: Path | None = None,
    resource_evidence_path: Path | None = None,
) -> dict[str, object]:
    source = _source_inventory(graph)
    reachability = _product_reachability(graph, source, product_id, provider_id, context_id)
    generated = _generated_provenance(graph, source)
    resources = _resource_provenance(graph, generated)
    product_link = _product_link_provenance(graph)
    packages = _package_provenance(graph)
    native_observation = validate_product_native_evidence(
        graph,
        native_evidence_path,
        product_id,
        context_id,
    )
    resource_observation = validate_resource_native_evidence(
        graph,
        resource_evidence_path,
        native_evidence_path,
        product_id,
        context_id,
    )
    linked_objects = set(native_observation.get("link", {}).get("object_inputs", []))
    observed_native_edges: dict[str, list[dict[str, object]]] = defaultdict(list)
    if native_observation["valid"]:
        for unit in native_observation.get("translation_units", []):
            source_path = str(unit.get("source") or "")
            object_path = unit.get("object")
            provider = _component_owner(graph, source_path) if source_path else None
            if provider is None or provider == product_id or not object_path or object_path not in linked_objects:
                continue
            observed_native_edges[provider].append({"source": source_path, "object": object_path})
    native_observation["consumer_provider_edges"] = [
        {
            "consumer": product_id,
            "provider": dependency,
            "witness_count": len(witnesses),
            "witnesses": sorted(witnesses, key=lambda item: (str(item["source"]), str(item["object"]))),
        }
        for dependency, witnesses in sorted(observed_native_edges.items())
    ]
    native_coverage = native_observation.get("coverage", {})
    generated["native_input_consumption_observed"] = bool(native_coverage.get("generated_input_consumption_observed"))
    generated["native_scheduling_observed"] = bool(native_coverage.get("generator_target_scheduling_observed"))
    generated["native_execution_observed"] = bool(native_coverage.get("generator_execution_observed"))
    generated["native_producer_consumer_correlations"] = native_observation.get("generator_observation", {}).get(
        "producer_consumer_correlations", []
    )
    resources["native_compiler_inputs_observed"] = bool(native_coverage.get("resource_compiler_inputs_observed"))
    resource_coverage = resource_observation.get("coverage", {})
    resources["native_resource_table_observed"] = bool(resource_coverage.get("resource_table_observed"))
    resources["native_resource_id_compatibility_observed"] = bool(
        resource_coverage.get("resource_id_compatibility_observed")
    )
    resources["native_resource_table"] = resource_observation.get("resource_table", {})
    product_link["native_input_set_observed"] = bool(native_coverage.get("link_input_set_observed"))
    product_link["native_selected_archive_members_observed"] = bool(native_coverage.get("selected_archive_members_observed"))
    product_link["native_consumer_provider_edges"] = native_observation["consumer_provider_edges"]
    product_link["native_product_link_closure_observed"] = (
        product_link["native_input_set_observed"]
        and product_link["native_selected_archive_members_observed"]
    )
    artifacts_by_kind = {
        kind: sorted(artifact.id for artifact in graph.artifacts.values() if artifact.artifact_kind == kind)
        for kind in ("generated", "resource", "asset", "package_set", "staging_set", "test_fixture", "product")
    }
    coverage = [
        {"class": "include", "status": "partial", "evidence": "owned C/C++ lexical scan plus native compiler/PCH trace when valid evidence is supplied; public/private and all-context closure pending"},
        {"class": "native_source_ownership", "status": "partial", "evidence": "MSBuild exact; CMake GLOB_RECURSE diagnostic"},
        {"class": "link", "status": "partial", "evidence": "MSBuild definitions plus native product link input set when valid evidence is supplied; selected archive members/link-map closure pending"},
        {"class": "generated", "status": "partial", "evidence": "declared outputs/tools plus native generated-input consumption, target terminal outcomes, and exact producer correlations when valid evidence is supplied; declared-input gaps and all-context closure pending"},
        {"class": "resource", "status": "partial", "evidence": "repository definitions plus native RC input trace and top-level PE resource table when valid evidence is supplied; canonical and nested resource ID compatibility pending"},
        {"class": "package", "status": "partial", "evidence": "root vcpkg manifest and global MSBuild restore definition observed; per-component restore closure and native restore trace pending"},
        {"class": "runtime_asset", "status": "not_observed", "evidence": "staging/runtime file access inventory pending"},
        {"class": "test_fixture", "status": "partial", "evidence": "stable test inventory exists; fixture/asset edges pending"},
        {"class": "state", "status": "not_observed", "evidence": "environment/registry/shared state runtime inventory pending"},
        {"class": "protocol", "status": "not_observed", "evidence": "IPC/RPC/persistence golden inventory pending"},
    ]

    findings: list[dict[str, object]] = []
    for failure in native_observation["failures"]:
        findings.append({"severity": "blocker", **failure})
    for failure in resource_observation["failures"]:
        findings.append({"severity": "blocker", **failure})
    for edge in source["undeclared_cross_component_edges"]:
        findings.append({
            "code": "UNDECLARED_INCLUDE_EDGE",
            "severity": "blocker",
            "consumer": edge["consumer"],
            "provider": edge["provider"],
            "witness_count": edge["witness_count"],
        })
    if source["unowned_repo_includes"]:
        findings.append({
            "code": "UNOWNED_REPO_INCLUDE",
            "severity": "blocker",
            "witness_count": len(source["unowned_repo_includes"]),
        })
    if source["unowned_repo_files"]:
        findings.append({
            "code": "UNOWNED_REPO_FILE_SET",
            "severity": "blocker",
            "file_count": len(source["unowned_repo_files"]),
        })
    if source["unresolved_quoted_includes"]:
        findings.append({
            "code": "UNRESOLVED_QUOTED_INCLUDE_SET",
            "severity": "blocker",
            "witness_count": len(source["unresolved_quoted_includes"]),
        })
    for directive in source["source_link_directives"]:
        findings.append({"code": "SOURCE_LINK_DIRECTIVE", "severity": "blocker", **directive})
    if reachability["msbuild"]["provider_sources_compiled_directly"]:
        findings.append({
            "code": "PRODUCT_EMBEDS_PROVIDER_SOURCE",
            "severity": "blocker",
            "consumer": product_id,
            "provider": provider_id,
            "sources": reachability["msbuild"]["provider_sources_compiled_directly"],
        })
    if not reachability["declared_compile_link_edges"]:
        findings.append({"code": "PRODUCT_PROVIDER_EDGE_MISSING", "severity": "blocker", "consumer": product_id, "provider": provider_id})
    if reachability["cmake"]["glob_recurse_witnesses"]:
        findings.append({"code": "CMAKE_SOURCE_OWNERSHIP_OPAQUE", "severity": "blocker", "witnesses": reachability["cmake"]["glob_recurse_witnesses"]})
    if generated["unclassified_unresolved_quoted_includes"]:
        findings.append({
            "code": "GENERATED_OR_LOCAL_INCLUDE_UNCLASSIFIED",
            "severity": "blocker",
            "witness_count": len(generated["unclassified_unresolved_quoted_includes"]),
        })
    if generated["declared_input_gaps"]:
        findings.append({
            "code": "GENERATOR_DECLARED_INPUT_GAP",
            "severity": "blocker",
            "witness_count": len(generated["declared_input_gaps"]),
            "witnesses": generated["declared_input_gaps"],
        })
    if not generated["native_scheduling_observed"]:
        findings.append({"code": "NATIVE_GENERATOR_SCHEDULING_UNOBSERVED", "severity": "blocker"})
    if not generated["native_execution_observed"]:
        findings.append({"code": "NATIVE_GENERATOR_EXECUTION_UNOBSERVED", "severity": "blocker"})
    if not generated["native_producer_consumer_correlations"]:
        findings.append({"code": "NATIVE_GENERATOR_CONSUMER_CORRELATION_UNOBSERVED", "severity": "blocker"})
    if resources["unowned_resource_files"]:
        findings.append({
            "code": "RESOURCE_OWNER_MISSING",
            "severity": "blocker",
            "file_count": len(resources["unowned_resource_files"]),
        })
    if resources["unresolved_resource_includes"]:
        findings.append({
            "code": "RESOURCE_INCLUDE_UNRESOLVED",
            "severity": "blocker",
            "witness_count": len(resources["unresolved_resource_includes"]),
        })
    if len(resources["canonical_sakura_rc_header_consumers"]) > 1:
        findings.append({
            "code": "SHARED_RESOURCE_ID_HEADER_COUPLING",
            "severity": "blocker",
            "consumer_count": len(resources["canonical_sakura_rc_header_consumers"]),
            "consumers": resources["canonical_sakura_rc_header_consumers"],
        })
    if not resources["native_resource_table_observed"]:
        findings.append({"code": "NATIVE_RESOURCE_TABLE_UNOBSERVED", "severity": "blocker"})
    if not resources["native_resource_id_compatibility_observed"]:
        findings.append({"code": "RESOURCE_ID_COMPATIBILITY_UNOBSERVED", "severity": "blocker"})
    expression_tokens = sorted({
        str(token)
        for record in product_link["msbuild_additional_dependencies"]
        for token in record["expression_tokens"]
        if str(token) != "%(AdditionalDependencies)"
    })
    if expression_tokens:
        findings.append({
            "code": "LINK_DEPENDENCY_EXPRESSION_UNRESOLVED",
            "severity": "blocker",
            "tokens": expression_tokens,
        })
    if product_link["test_product_object_aggregation"]:
        findings.append({
            "code": "TEST_LINKS_PRODUCT_OBJECTS",
            "severity": "blocker",
            "witnesses": product_link["test_product_object_aggregation"],
        })
    if not product_link["native_product_link_closure_observed"]:
        findings.append({"code": "NATIVE_PRODUCT_LINK_CLOSURE_UNOBSERVED", "severity": "blocker"})
    if packages["global_restore_scope"]:
        findings.append({
            "code": "GLOBAL_VCPKG_RESTORE_SCOPE",
            "severity": "blocker",
            "package_count": len(packages["root_manifest_packages"]),
        })
    if packages["root_manifest_packages"] and not packages["root_manifest_classified"]:
        findings.append({"code": "ROOT_PACKAGE_SET_UNCLASSIFIED", "severity": "blocker", "packages": packages["root_manifest_unclassified_packages"]})
    if not packages["native_restore_observed"]:
        findings.append({"code": "NATIVE_PACKAGE_RESTORE_UNOBSERVED", "severity": "blocker"})
    for item in coverage:
        if item["status"] != "observed":
            findings.append({"code": "DEPENDENCY_CLASS_INCOMPLETE", "severity": "blocker", "class": item["class"], "status": item["status"]})

    stable_payload = {
        "semantic_graph_hash": graph.semantic_graph_hash,
        "source": source,
        "product_reachability": reachability,
        "generated_provenance": generated,
        "resource_provenance": resources,
        "product_link_provenance": product_link,
        "package_provenance": packages,
        "native_product_observation": native_observation,
        "native_resource_observation": resource_observation,
        "artifacts_by_kind": artifacts_by_kind,
        "coverage": coverage,
        "findings": findings,
    }
    return {
        "schema_version": 1,
        "collection_ok": True,
        "graduation_ready": not findings and all(item["status"] == "observed" for item in coverage),
        **stable_payload,
        "hard_evidence_hash": _sha256_json(stable_payload),
    }


def write_repository_inventory(path: Path, inventory: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(inventory, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def repository_inventory_summary(
    inventory: Mapping[str, object],
    *,
    output_path: Path | None = None,
) -> dict[str, object]:
    """Return a bounded CLI view while retaining full witnesses in the evidence file."""

    source = inventory["source"]
    reachability = inventory["product_reachability"]
    generated = inventory["generated_provenance"]
    resources = inventory["resource_provenance"]
    product_link = inventory["product_link_provenance"]
    packages = inventory["package_provenance"]
    native = inventory["native_product_observation"]
    native_resource = inventory["native_resource_observation"]
    findings_by_code: dict[str, int] = defaultdict(int)
    for finding in inventory["findings"]:
        findings_by_code[str(finding["code"])] += 1
    result: dict[str, object] = {
        "collection_ok": inventory["collection_ok"],
        "graduation_ready": inventory["graduation_ready"],
        "semantic_graph_hash": inventory["semantic_graph_hash"],
        "hard_evidence_hash": inventory["hard_evidence_hash"],
        "repository_cpp_resource_files": source["repository_cpp_resource_files"],
        "scanned_cpp_files": source["scanned_cpp_files"],
        "unowned_repo_file_count": len(source["unowned_repo_files"]),
        "include_directive_count": source["include_directive_count"],
        "unowned_repo_include_count": len(source["unowned_repo_includes"]),
        "unresolved_quoted_include_count": len(source["unresolved_quoted_includes"]),
        "source_link_directive_count": len(source["source_link_directives"]),
        "generated_provenance": {
            "msbuild_header_item_count": len(generated["msbuild_generated_header_items"]),
            "msbuild_target_count": len(generated["msbuild_generation_targets"]),
            "cmake_command_count": len(generated["cmake_custom_commands"]),
            "classified_include_count": len(generated["classified_unresolved_generated_includes"]),
            "unclassified_include_count": len(generated["unclassified_unresolved_quoted_includes"]),
            "declared_input_gap_count": len(generated["declared_input_gaps"]),
            "native_scheduling_observed": generated["native_scheduling_observed"],
            "native_execution_observed": generated["native_execution_observed"],
            "native_input_consumption_observed": generated["native_input_consumption_observed"],
            "native_producer_consumer_correlation_count": len(generated["native_producer_consumer_correlations"]),
        },
        "resource_provenance": {
            "resource_file_count": len(resources["repository_resource_files"]),
            "unowned_resource_file_count": len(resources["unowned_resource_files"]),
            "resource_include_edge_count": len(resources["resource_include_edges"]),
            "generated_include_count": len(resources["generated_resource_includes"]),
            "unresolved_include_count": len(resources["unresolved_resource_includes"]),
            "native_resource_table_observed": resources["native_resource_table_observed"],
            "native_resource_id_compatibility_observed": resources["native_resource_id_compatibility_observed"],
            "native_resource_table_entry_count": int(resources["native_resource_table"].get("entry_count", 0)),
            "native_compiler_inputs_observed": resources["native_compiler_inputs_observed"],
        },
        "product_link_provenance": {
            "additional_dependency_group_count": len(product_link["msbuild_additional_dependencies"]),
            "project_reference_count": len(product_link["msbuild_project_references"]),
            "test_product_object_aggregation_count": len(product_link["test_product_object_aggregation"]),
            "native_product_link_closure_observed": product_link["native_product_link_closure_observed"],
            "native_input_set_observed": product_link["native_input_set_observed"],
            "native_selected_archive_members_observed": product_link["native_selected_archive_members_observed"],
        },
        "package_provenance": {
            "root_package_count": len(packages["root_manifest_packages"]),
            "global_restore_scope": packages["global_restore_scope"],
            "component_package_isolation_verified": packages["component_package_isolation_verified"],
            "native_restore_observed": packages["native_restore_observed"],
        },
        "native_product_observation": {
            "status": native["status"],
            "valid": native["valid"],
            "hard_evidence_hash": native.get("hard_evidence_hash"),
            "coverage": native.get("coverage", {}),
            "counts": native.get("counts", {}),
            "consumer_provider_edge_count": len(native.get("consumer_provider_edges", [])),
        },
        "native_resource_observation": {
            "status": native_resource["status"],
            "valid": native_resource["valid"],
            "hard_evidence_hash": native_resource.get("hard_evidence_hash"),
            "coverage": native_resource.get("coverage", {}),
            "resource_table_entry_count": int(native_resource.get("resource_table", {}).get("entry_count", 0)),
        },
        "finding_count": len(inventory["findings"]),
        "findings_by_code": dict(sorted(findings_by_code.items())),
        "coverage": {
            str(item["class"]): str(item["status"])
            for item in inventory["coverage"]
        },
        "product_reachability": {
            "consumer": reachability["consumer"],
            "provider": reachability["provider"],
            "status": reachability["status"],
            "independent": reachability["independent"],
        },
    }
    if output_path is not None:
        result["output"] = str(output_path)
    return result
