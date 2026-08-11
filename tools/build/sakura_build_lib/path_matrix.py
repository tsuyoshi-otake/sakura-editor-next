"""Basic Windows checkout-path matrix and pilot revert rehearsal."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .model import load_semantic_graph
from .runner import BuildError, cleanup_native_aliases_for_process


DEFAULT_TIMEOUT_SECONDS = 300
_MATRIX_FILES = (
    "CMakeLists.txt",
    "src/main/modules/modules.json",
    "src/main/modules/schema-v3.json",
    "src/main/modules/compile-profiles.json",
    "sakura_core/sakura.vcxproj.filters",
)


@dataclass(frozen=True)
class ChildResult:
    status: str
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    """Terminate only the child tree started by this matrix invocation."""
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            text=True,
            check=False,
        )
    else:
        process.kill()


def _run_child(
    command: Sequence[str],
    cwd: Path,
    *,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
) -> ChildResult:
    started = time.perf_counter()
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    process = subprocess.Popen(
        list(command),
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=creationflags,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        _terminate_process_tree(process)
        try:
            stdout, stderr = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
        try:
            cleanup_native_aliases_for_process(cwd, process.pid)
        except BuildError as error:
            return ChildResult(
                status="cleanup_failed",
                returncode=9,
                stdout=stdout,
                stderr=f"{stderr}\n{error.code}: {error}".strip(),
                elapsed_ms=(time.perf_counter() - started) * 1000,
            )
        return ChildResult(
            status="timeout",
            returncode=8,
            stdout=stdout,
            stderr=stderr,
            elapsed_ms=(time.perf_counter() - started) * 1000,
        )
    return ChildResult(
        status="success" if process.returncode == 0 else "failed",
        returncode=process.returncode,
        stdout=stdout,
        stderr=stderr,
        elapsed_ms=(time.perf_counter() - started) * 1000,
    )


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _safe_manifest_relative(value: object, location: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"path-matrix manifest path must be a non-empty string at {location}")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"path-matrix manifest path escapes the repository at {location}: {value}")
    return path


def _manifest_copy_entries(source: Path) -> tuple[set[Path], set[Path]]:
    manifest_path = source / "src/main/modules/modules.json"
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    files: set[Path] = set()
    directories: set[Path] = set()

    for component_index, component in enumerate(data.get("components", [])):
        for value_index, value in enumerate(component.get("sources", [])):
            location = f"components[{component_index}].sources[{value_index}]"
            relative = _safe_manifest_relative(value, location)
            source_path = source / relative
            if source_path.is_file():
                files.add(relative)
            elif source_path.is_dir():
                directories.add(relative)
            else:
                raise FileNotFoundError(f"path-matrix manifest input is missing at {location}: {relative.as_posix()}")
        for field in ("public_headers", "private_headers"):
            for value_index, value in enumerate(component.get(field, [])):
                location = f"components[{component_index}].{field}[{value_index}]"
                relative = _safe_manifest_relative(value, location)
                if not (source / relative).is_file():
                    raise FileNotFoundError(
                        f"path-matrix manifest header is missing at {location}: {relative.as_posix()}"
                    )
                files.add(relative)
        for backend, targets in component.get("backend_targets", {}).items():
            for target_index, value in enumerate(targets):
                location = f"components[{component_index}].backend_targets.{backend}[{target_index}]"
                relative = _safe_manifest_relative(
                    value,
                    location,
                )
                source_path = source / relative
                if source_path.is_file():
                    files.add(relative)
                elif len(relative.parts) > 1 or relative.suffix:
                    raise FileNotFoundError(
                        f"path-matrix backend target is missing at {location}: {relative.as_posix()}"
                    )

    for edge_index, edge in enumerate(data.get("edges", [])):
        for witness_index, witness in enumerate(edge.get("witnesses", [])):
            relative = _safe_manifest_relative(
                witness.get("probe"),
                f"edges[{edge_index}].witnesses[{witness_index}].probe",
            )
            if not (source / relative).is_file():
                raise FileNotFoundError(f"path-matrix edge witness is missing: {relative.as_posix()}")
            files.add(relative)

    for artifact_index, artifact in enumerate(data.get("artifacts", [])):
        for input_index, value in enumerate(artifact.get("inputs", [])):
            relative = _safe_manifest_relative(value, f"artifacts[{artifact_index}].inputs[{input_index}]")
            source_path = source / relative
            if source_path.is_file():
                files.add(relative)
            elif source_path.is_dir():
                directories.add(relative)
            else:
                raise FileNotFoundError(f"path-matrix artifact input is missing: {relative.as_posix()}")

    return files, directories


def _copy_minimal_repository(source: Path, destination: Path) -> None:
    manifest_files, manifest_directories = _manifest_copy_entries(source)
    files = {Path(value) for value in _MATRIX_FILES} | manifest_files
    for relative in sorted(files, key=lambda item: item.as_posix()):
        source_file = source / relative
        if not source_file.is_file():
            raise FileNotFoundError(f"path-matrix input is missing: {relative.as_posix()}")
        destination_file = destination / relative
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_file, destination_file)
    for relative in sorted(manifest_directories, key=lambda item: item.as_posix()):
        (destination / relative).mkdir(parents=True, exist_ok=True)


def _cli_command(script: Path, repo: Path, *arguments: str) -> list[str]:
    return [
        sys.executable,
        str(script),
        "--repo-root",
        str(repo),
        "--format",
        "json",
        *arguments,
    ]


def _run_step(
    script: Path,
    repo: Path,
    label: str,
    arguments: Sequence[str],
    records: list[dict[str, object]],
    *,
    timeout_seconds: float,
    parse_json: bool = False,
) -> dict[str, object] | None:
    result = _run_child(
        _cli_command(script, repo, *arguments),
        repo,
        timeout_seconds=timeout_seconds,
    )
    records.append({
        "step": label,
        "status": result.status,
        "exit_code": result.returncode,
        "elapsed_ms": round(result.elapsed_ms, 3),
    })
    if result.status != "success":
        detail = (result.stdout + "\n" + result.stderr).strip().splitlines()
        tail = "\n".join(detail[-40:]) if detail else "no output"
        raise RuntimeError(f"{label} {result.status} (exit {result.returncode}):\n{tail}")
    if not parse_json:
        return None
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{label} did not return JSON: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{label} returned a non-object JSON result")
    return value


def _run_case(
    script: Path,
    repo: Path,
    case_id: str,
    contexts: Sequence[str],
    jobs: int,
    *,
    generated: bool,
    timeout_seconds: float,
) -> dict[str, object]:
    records: list[dict[str, object]] = []
    _run_step(
        script,
        repo,
        "generate" if generated else "generate-check",
        ("generate",) if generated else ("generate", "--check"),
        records,
        timeout_seconds=timeout_seconds,
        parse_json=True,
    )
    manifest = _run_step(
        script,
        repo,
        "manifest-check",
        ("manifest", "check"),
        records,
        timeout_seconds=timeout_seconds,
        parse_json=True,
    )
    _run_step(
        script,
        repo,
        "graph-check",
        ("graph", "check", "--all-contexts"),
        records,
        timeout_seconds=timeout_seconds,
        parse_json=True,
    )
    evidence: dict[str, dict[str, object]] = {}
    for context_id in contexts:
        _run_step(
            script,
            repo,
            f"test:{context_id}",
            ("test", "component", "sakura_uri_tests", "--context", context_id, "--jobs", str(jobs)),
            records,
            timeout_seconds=timeout_seconds,
        )
        value = _run_step(
            script,
            repo,
            f"evidence:{context_id}",
            ("verify", "component-boundary", "sakura_uri_tests", "--context", context_id),
            records,
            timeout_seconds=timeout_seconds,
            parse_json=True,
        )
        assert value is not None
        evidence[context_id] = value
    assert manifest is not None
    return {
        "id": case_id,
        "status": "success",
        "semantic_graph_hash": manifest["graph_hash"],
        "hard_evidence_hashes": {
            context_id: value["hard_evidence_hash"] for context_id, value in sorted(evidence.items())
        },
        "evidence_failures": {
            context_id: value["failures"] for context_id, value in sorted(evidence.items())
        },
        "steps": records,
    }


def _standalone_revert_rehearsal(
    script: Path,
    repo: Path,
    component_id: str,
    *,
    timeout_seconds: float,
) -> dict[str, object]:
    graph = load_semantic_graph(repo)
    removed_ids = set(graph.final_link_closure((component_id,), "msvc-x64-debug"))
    removed_ids.update(graph.closure((component_id,), "msvc-x64-debug", ("compile", "link", "test")))
    manifest_path = repo / "src/main/modules/modules.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    removed_components = [item for item in manifest["components"] if item["id"] in removed_ids]
    removed_owners = {item["owner"] for item in removed_components}
    legacy_files = (
        repo / "CMakeLists.txt",
        repo / "sakura_core/sakura.vcxproj",
        repo / "sakura_core/sakura.vcxproj.filters",
        repo / "sakura_core/tests1.vcxproj",
    )
    before_hashes = {path.relative_to(repo).as_posix(): _sha256(path) for path in legacy_files}

    remaining_components = []
    for item in manifest["components"]:
        if item["id"] in removed_ids:
            continue
        copied = dict(item)
        copied["ownership_exclusions"] = [
            value for value in item["ownership_exclusions"] if value not in removed_owners
        ]
        remaining_components.append(copied)
    manifest["components"] = remaining_components
    manifest["edges"] = [
        item for item in manifest["edges"] if item["from"] not in removed_ids and item["to"] not in removed_ids
    ]
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    records: list[dict[str, object]] = []
    _run_step(script, repo, "revert-generate", ("generate",), records, timeout_seconds=timeout_seconds, parse_json=True)
    _run_step(script, repo, "revert-manifest", ("manifest", "check"), records, timeout_seconds=timeout_seconds, parse_json=True)
    graph_result = _run_step(
        script,
        repo,
        "revert-graph",
        ("graph", "check", "--all-contexts"),
        records,
        timeout_seconds=timeout_seconds,
        parse_json=True,
    )
    after_hashes = {path.relative_to(repo).as_posix(): _sha256(path) for path in legacy_files}
    source_paths = [repo / source for item in removed_components for source in item["sources"]]
    implementation_sources = [
        Path(source).name.lower()
        for item in removed_components
        if item["kind"] == "implementation"
        for source in item["sources"]
    ]
    legacy_project_text = "\n".join(path.read_text(encoding="utf-8", errors="replace").lower() for path in legacy_files)
    generated_targets = [
        repo / target
        for item in removed_components
        for targets in item["backend_targets"].values()
        for target in targets
        if "/" in target or "\\" in target
    ]
    generated_targets.extend(
        repo / f"src/main/modules/generated/cmake/projects/{context_id}/{item['id']}/CMakeLists.txt"
        for item in removed_components
        for context_id in item["supported_contexts"]
        if graph.contexts[context_id].backend == "cmake"
    )
    checks = {
        "legacy_files_unchanged": before_hashes == after_hashes,
        "sources_remain": all(path.is_file() for path in source_paths),
        "legacy_project_references_remain": all(name in legacy_project_text for name in implementation_sources),
        "generated_pilot_projects_removed": all(not path.exists() for path in generated_targets),
        "graph_failures": [] if graph_result is None else graph_result.get("failures", []),
    }
    ok = all(value is True or value == [] for value in checks.values())
    return {
        "status": "success" if ok else "failed",
        "removed_components": sorted(removed_ids),
        "checks": checks,
        "steps": records,
    }


def run_basic_path_matrix(
    repo_root: Path,
    component_id: str,
    contexts: Sequence[str],
    jobs: int,
    *,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    workspace_root: Path | None = None,
) -> dict[str, object]:
    if component_id != "sakura_uri_tests":
        return {"ok": False, "status": "failed", "failure": "R1a basic matrix currently supports sakura_uri_tests only"}
    script = Path(__file__).resolve().parents[1] / "sakura_build.py"
    matrix_base = (
        workspace_root
        or (Path(os.environ["SAKURA_PATH_MATRIX_ROOT"]) if os.environ.get("SAKURA_PATH_MATRIX_ROOT") else None)
        or (Path.home() / "tmp/sakura-path-matrix")
    ).resolve()
    if matrix_base == Path.home().resolve() or matrix_base.parent == matrix_base:
        return {"ok": False, "status": "failed", "failure": "unsafe path-matrix workspace root"}
    matrix_base.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix="issue15-r1a-", dir=matrix_base)
    work_root = Path(temporary.name)
    try:
        work_root.resolve().relative_to(matrix_base.resolve())
    except ValueError:
        temporary.cleanup()
        return {"ok": False, "status": "failed", "failure": "temporary matrix root escaped its workspace root"}

    report: dict[str, object] = {
        "schema_version": 1,
        "component_id": component_id,
        "contexts": list(contexts),
        "cases": [],
        "standalone_revert": None,
        "status": "running",
    }
    cleanup_error: str | None = None
    try:
        case_roots = (
            ("normal", repo_root, False),
            ("ascii-space", work_root / "ascii space/repo", True),
            ("japanese", work_root / "日本語/repo", True),
        )
        for case_id, case_root, copied in case_roots:
            if copied:
                _copy_minimal_repository(repo_root, case_root)
            try:
                case = _run_case(
                    script,
                    case_root,
                    case_id,
                    contexts,
                    jobs,
                    generated=copied,
                    timeout_seconds=timeout_seconds,
                )
            except RuntimeError as error:
                report["cases"].append({"id": case_id, "status": "failed", "failure": str(error)})
                raise
            report["cases"].append(case)

        cases = report["cases"]
        assert isinstance(cases, list)
        semantic_hashes = {case["semantic_graph_hash"] for case in cases}
        evidence_hashes_match = all(
            len({case["hard_evidence_hashes"][context_id] for case in cases}) == 1
            for context_id in contexts
        )
        report["semantic_graph_hashes_match"] = len(semantic_hashes) == 1
        report["hard_evidence_hashes_match"] = evidence_hashes_match
        japanese_root = work_root / "日本語/repo"
        report["standalone_revert"] = _standalone_revert_rehearsal(
            script,
            japanese_root,
            component_id,
            timeout_seconds=timeout_seconds,
        )
        revert = report["standalone_revert"]
        assert isinstance(revert, dict)
        report["ok"] = (
            report["semantic_graph_hashes_match"] is True
            and report["hard_evidence_hashes_match"] is True
            and revert["status"] == "success"
        )
        report["status"] = "success" if report["ok"] else "failed"
    except (OSError, RuntimeError, KeyError, json.JSONDecodeError) as error:
        report["ok"] = False
        report["status"] = "failed"
        report["failure"] = str(error)
    finally:
        try:
            temporary.cleanup()
        except OSError as error:
            cleanup_error = str(error)
        try:
            matrix_base.rmdir()
        except OSError:
            # A concurrent matrix or a caller-owned workspace may keep it.
            pass
    report["workspace_cleaned"] = not work_root.exists()
    if cleanup_error is not None or not report["workspace_cleaned"]:
        report["ok"] = False
        report["status"] = "cleanup_failed"
        report["cleanup_error"] = cleanup_error or "temporary matrix root still exists"
    return report
