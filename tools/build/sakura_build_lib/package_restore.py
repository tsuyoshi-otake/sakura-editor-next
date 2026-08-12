"""Explicit, content-addressed vcpkg package restoration.

Normal native project evaluation must consume a completed package root; it must
never be responsible for discovering a manifest or starting a restore.  This
module is the sole restore owner.  It derives the package closure from the
semantic graph, hashes every declared package input, restores into a private
temporary directory under ``build/packages``, and atomically publishes the
completed root only after vcpkg and the declared package directories validate.

The cache is deliberately repository-local.  It makes a pull with unchanged
package inputs a reuse operation instead of another package build, while a
changed manifest, registry, overlay, triplet, or vcpkg tool gets a distinct
immutable entry.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence
from xml.sax.saxutils import escape as xml_escape

from .model import Artifact, SemanticGraph
from .runner import BuildError, EventWriter


# The active receipt describes an immutable package closure.  Version 2 keys
# that closure by tracked vcpkg source metadata rather than by the bootstrapped
# vcpkg.exe bytes.  Bootstrap output is host-local and may be replaced without
# changing the declared package tool source, which must not make a completed
# closure look stale while MSBuild is starting.
PACKAGE_RESTORE_SCHEMA_VERSION = 2
_PACKAGE_PHASES = (
    "generate",
    "compile",
    "link",
    "stage",
    "test",
    "runtime",
    "lifecycle",
)
# vcpkg itself creates deeply nested build and installed paths. Keep the
# content-addressed cache compact enough for ordinary Win32 path handling even
# when the checkout sits below a developer directory.
_PACKAGE_ROOT_RELATIVE = Path("build/pkg/v")
_LOCK_POLL_SECONDS = 0.1
_DEFAULT_CACHE_MAX_BYTES = 8 * 1024 * 1024 * 1024
# When a port build fails, vcpkg appends a ready-to-paste GitHub issue report.
# Its trailing section is only the manifest this module already knows, and it is
# longer than any fixed tail window, so a plain tail slice keeps the boilerplate
# and drops the cause. Cut the section, and lift the diagnostic lines out of the
# whole stream so a cause above the window survives regardless of length.
_VCPKG_REPORT_TAIL_MARKER = "**Additional context**"
_VCPKG_DIAGNOSTIC_MARKERS = (
    "error:",
    "CMake Error",
    "fatal error",
    "failed with:",
    "See logs for more information",
)
_VCPKG_DIAGNOSTIC_LINES = 12
_VCPKG_TAIL_LINES = 80


def _canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path, code: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise BuildError(code, f"could not hash {path}: {error}", 5) from error
    return "sha256:" + digest.hexdigest()


def _repo_relative(repo_root: Path, path: Path, code: str) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError as error:
        raise BuildError(code, f"path escapes repository: {path}", 2) from error


def _read_json(path: Path, code: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except FileNotFoundError as error:
        raise BuildError(code, f"missing file: {path}", 4) from error
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(code, f"could not read {path}: {error}", 5) from error
    if not isinstance(value, dict):
        raise BuildError(code, f"expected JSON object: {path}", 5)
    return value


def _manifest_dependency_names(repo_root: Path) -> tuple[str, ...]:
    manifest = _read_json(repo_root / "vcpkg.json", "PACKAGE_MANIFEST_INVALID")
    dependencies = manifest.get("dependencies", [])
    if not isinstance(dependencies, list):
        raise BuildError("PACKAGE_MANIFEST_INVALID", "vcpkg.json dependencies must be an array", 5)
    names: set[str] = set()
    for dependency in dependencies:
        if isinstance(dependency, str) and dependency:
            names.add(dependency)
        elif isinstance(dependency, dict) and isinstance(dependency.get("name"), str) and dependency["name"]:
            names.add(dependency["name"])
        else:
            raise BuildError("PACKAGE_MANIFEST_INVALID", f"unsupported vcpkg dependency declaration: {dependency!r}", 5)
    return tuple(sorted(names))


def _package_output_names(artifacts: Iterable[Artifact]) -> tuple[str, ...]:
    names: set[str] = set()
    for artifact in artifacts:
        for output in artifact.outputs:
            if output.lower().startswith("vcpkg:"):
                name = output.split(":", 1)[1].strip()
                if not name:
                    raise BuildError("PACKAGE_DECLARATION_INVALID", f"{artifact.id} declares an empty vcpkg package", 5)
                names.add(name)
    return tuple(sorted(names))


def _digest_declared_input(repo_root: Path, relative: str) -> dict[str, object]:
    path = (repo_root / relative).resolve()
    _repo_relative(repo_root, path, "PACKAGE_INPUT_ESCAPE")
    if path.is_file():
        return {
            "path": relative.replace("\\", "/"),
            "kind": "file",
            "sha256": _sha256_file(path, "PACKAGE_INPUT_HASH"),
        }
    if not path.is_dir():
        raise BuildError("PACKAGE_INPUT_MISSING", f"declared package input does not exist: {relative}", 4)

    entries: list[dict[str, str]] = []
    for current_root, directories, files in os.walk(path, topdown=True, followlinks=False):
        directories[:] = sorted(
            directory for directory in directories
            if not (Path(current_root) / directory).is_symlink()
        )
        current = Path(current_root)
        directory_relative = current.relative_to(path).as_posix()
        entries.append({"kind": "directory", "path": directory_relative})
        for filename in sorted(files):
            candidate = current / filename
            if candidate.is_symlink():
                raise BuildError("PACKAGE_INPUT_SYMLINK", f"package input must not contain a symlink: {candidate}", 5)
            entries.append({
                "kind": "file",
                "path": candidate.relative_to(path).as_posix(),
                "sha256": _sha256_file(candidate, "PACKAGE_INPUT_HASH"),
            })
    return {
        "path": relative.replace("\\", "/"),
        "kind": "directory",
        "entry_count": len(entries),
        "sha256": _sha256_bytes(_canonical_json(entries).encode("utf-8")),
    }


def _vcpkg_root(repo_root: Path, environment: Mapping[str, str] | None = None) -> Path:
    env = dict(environment or os.environ)
    candidates: list[Path] = []
    value = env.get("VCPKG_ROOT")
    if value:
        candidates.append(Path(value))
    candidates.extend((repo_root.parent / "vcpkg", repo_root / "tools/vcpkg"))
    for candidate in candidates:
        resolved = candidate.resolve()
        executable = resolved / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
        if executable.is_file() and (resolved / "scripts/buildsystems/vcpkg.cmake").is_file():
            return resolved
    raise BuildError("TOOL_VCPKG_NOT_FOUND", "vcpkg was not found; set VCPKG_ROOT or provide tools/vcpkg", 3)


def _triplets(graph: SemanticGraph, context_id: str) -> tuple[str, str]:
    context = graph.context(context_id)
    if context.arch != "x64":
        raise BuildError("PACKAGE_CONTEXT_UNSUPPORTED", f"package restore supports x64 contexts only: {context_id}", 2)
    if context.toolchain == "mingw":
        return "x64-mingw-static", "x64-windows"
    if context.toolchain == "msvc":
        return "x64-windows-static", "x64-windows"
    raise BuildError("PACKAGE_CONTEXT_UNSUPPORTED", f"package restore has no triplet mapping for {context_id}", 2)


def _triplet_file(vcpkg_root: Path, triplet: str) -> Path:
    """Resolve both built-in and vcpkg community triplets for plan hashing."""

    for candidate in (
        vcpkg_root / "triplets" / f"{triplet}.cmake",
        vcpkg_root / "triplets" / "community" / f"{triplet}.cmake",
    ):
        if candidate.is_file():
            return candidate
    raise BuildError("PACKAGE_TRIPLET_MISSING", f"vcpkg triplet is missing: {triplet}", 3)


def plan_package_restore(
    graph: SemanticGraph,
    roots: Iterable[str],
    context_id: str,
    *,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Create the deterministic package closure plan for one semantic root set."""

    root_ids = tuple(sorted(set(roots)))
    intent = graph.build_intent(root_ids, context_id, _PACKAGE_PHASES)
    package_ids = tuple(str(value) for value in intent["package_sets"])
    if not package_ids:
        return {
            "schema_version": PACKAGE_RESTORE_SCHEMA_VERSION,
            "required": False,
            "context_id": context_id,
            "roots": list(root_ids),
            "package_set_ids": [],
            "package_names": [],
            "plan_hash": None,
        }

    artifacts: list[Artifact] = []
    for package_id in package_ids:
        artifact = graph.artifacts.get(package_id)
        if artifact is None or artifact.artifact_kind != "package_set":
            raise BuildError("PACKAGE_SET_UNKNOWN", f"build intent names a missing package set: {package_id}", 5)
        artifacts.append(artifact)
    package_names = _package_output_names(artifacts)
    manifest_names = _manifest_dependency_names(graph.repo_root)
    if package_names != manifest_names:
        missing = sorted(set(manifest_names) - set(package_names))
        undeclared = sorted(set(package_names) - set(manifest_names))
        detail: list[str] = []
        if missing:
            detail.append("manifest packages missing from declared closure: " + ", ".join(missing))
        if undeclared:
            detail.append("declared packages absent from manifest: " + ", ".join(undeclared))
        raise BuildError("PACKAGE_CLOSURE_MISMATCH", "; ".join(detail), 5)

    target_triplet, host_triplet = _triplets(graph, context_id)
    vcpkg_root = _vcpkg_root(graph.repo_root, environment)
    executable = vcpkg_root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    declared_inputs = sorted({value for artifact in artifacts for value in artifact.inputs})
    input_digests = [_digest_declared_input(graph.repo_root, value) for value in declared_inputs]
    triplet_hashes: dict[str, str] = {}
    for triplet in (target_triplet, host_triplet):
        candidate = _triplet_file(vcpkg_root, triplet)
        triplet_hashes[triplet] = _sha256_file(candidate, "PACKAGE_TRIPLET_HASH")
    toolchain = vcpkg_root / "scripts/buildsystems/vcpkg.cmake"
    tool_metadata = vcpkg_root / "scripts/vcpkg-tool-metadata.txt"
    payload = {
        "schema_version": PACKAGE_RESTORE_SCHEMA_VERSION,
        "semantic_graph_hash": graph.semantic_graph_hash,
        "package_set_ids": list(package_ids),
        "package_names": list(package_names),
        "declared_inputs": input_digests,
        "target_triplet": target_triplet,
        "host_triplet": host_triplet,
        "vcpkg": {
            "tool_metadata_hash": _sha256_file(tool_metadata, "TOOL_VCPKG_METADATA_HASH"),
            "toolchain_hash": _sha256_file(toolchain, "TOOL_VCPKG_TOOLCHAIN_HASH"),
            "triplet_hashes": dict(sorted(triplet_hashes.items())),
        },
    }
    plan_hash = _sha256_bytes(_canonical_json(payload).encode("utf-8"))
    cache_root = graph.repo_root / _PACKAGE_ROOT_RELATIVE
    entry_name = plan_hash.split(":", 1)[1]
    entry_root = cache_root / "e" / entry_name
    active_root = cache_root / "a"
    return {
        "required": True,
        "repo_root": graph.repo_root,
        "context_id": context_id,
        "roots": list(root_ids),
        "plan_hash": plan_hash,
        "plan": payload,
        "cache_root": cache_root,
        "entry_root": entry_root,
        "installed_root": entry_root / "installed",
        "active_root": active_root,
        "active_metadata": active_root / f"{target_triplet}.json",
        "active_props": active_root / f"{target_triplet}.props",
        "active_cmake": active_root / f"{target_triplet}.cmake",
        # Dry-run callers need the same deterministic configuration locations
        # as a completed restore.  Keeping these in the plan avoids a special
        # case that could accidentally make a dry-run depend on an old active
        # package root.
        "active_props_relative": _repo_relative(
            graph.repo_root,
            active_root / f"{target_triplet}.props",
            "PACKAGE_PLAN_PATH",
        ),
        "active_cmake_relative": _repo_relative(
            graph.repo_root,
            active_root / f"{target_triplet}.cmake",
            "PACKAGE_PLAN_PATH",
        ),
        "target_triplet": target_triplet,
        "host_triplet": host_triplet,
        "package_set_ids": list(package_ids),
        "package_names": list(package_names),
        "vcpkg_root": vcpkg_root,
        "vcpkg_executable": executable,
    }


def _entry_metadata_path(entry_root: Path) -> Path:
    return entry_root / "complete.json"


def _entry_valid(plan: Mapping[str, object], *, require_metadata: bool = True) -> tuple[bool, dict[str, object] | None, list[str]]:
    entry_root = Path(plan["entry_root"])
    metadata_path = _entry_metadata_path(entry_root)
    if not metadata_path.is_file():
        return False, None, ["completion metadata is missing"]
    try:
        metadata = _read_json(metadata_path, "PACKAGE_CACHE_METADATA_INVALID")
    except BuildError as error:
        return False, None, [str(error)]
    failures: list[str] = []
    if metadata.get("schema_version") != PACKAGE_RESTORE_SCHEMA_VERSION:
        failures.append("completion metadata schema differs")
    if metadata.get("plan_hash") != plan.get("plan_hash"):
        failures.append("completion metadata plan hash differs")
    if metadata.get("plan") != plan.get("plan"):
        failures.append("completion metadata plan payload differs")
    installed_root = entry_root / "installed"
    triplet = str(plan["target_triplet"])
    if not (installed_root / triplet).is_dir():
        failures.append(f"installed triplet root is missing: {triplet}")
    for package in plan.get("package_names", []):
        if not (installed_root / triplet / "share" / str(package)).is_dir():
            failures.append(f"installed package metadata is missing: {package}")
    return not failures, metadata if not failures or require_metadata else None, failures


def _atomic_write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except FileNotFoundError:
        pass
    # The caller owns an entry lock while writing completion metadata. A compact
    # nonce avoids spending another 40+ characters below a vcpkg cache entry.
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex[:8]}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _entry_relative(plan: Mapping[str, object]) -> str:
    return Path(plan["entry_root"]).relative_to(Path(plan["cache_root"])).as_posix()


def _usage_record_path(plan: Mapping[str, object]) -> Path:
    entry_name = Path(plan["entry_root"]).name
    return Path(plan["cache_root"]) / "u" / f"{entry_name}.json"


def _record_cache_use(plan: Mapping[str, object]) -> None:
    """Track LRU metadata outside the immutable content-addressed entry."""

    payload = {
        "schema_version": PACKAGE_RESTORE_SCHEMA_VERSION,
        "entry_relative": _entry_relative(plan),
        "plan_hash": plan["plan_hash"],
        "last_used_unix_ns": time.time_ns(),
    }
    _atomic_write_text(
        _usage_record_path(plan),
        json.dumps(payload, ensure_ascii=True, sort_keys=True, indent=2) + "\n",
    )


def _write_active_projection(plan: Mapping[str, object], metadata: Mapping[str, object]) -> None:
    cache_root = Path(plan["cache_root"])
    active_metadata = Path(plan["active_metadata"])
    active_props = Path(plan["active_props"])
    active_cmake = Path(plan["active_cmake"])
    installed_root = Path(plan["installed_root"]).resolve()
    entry_root = Path(plan["entry_root"]).resolve()
    for candidate in (installed_root, entry_root):
        try:
            candidate.relative_to(cache_root.resolve())
        except ValueError as error:
            raise BuildError("PACKAGE_ACTIVE_PATH_ESCAPE", f"package cache entry escapes cache root: {candidate}", 5) from error
    active = {
        "schema_version": PACKAGE_RESTORE_SCHEMA_VERSION,
        "plan_hash": plan["plan_hash"],
        "target_triplet": plan["target_triplet"],
        "host_triplet": plan["host_triplet"],
        "package_set_ids": plan["package_set_ids"],
        "package_names": plan["package_names"],
        "entry_relative": _entry_relative(plan),
        "completion_hash": _sha256_bytes(_canonical_json(metadata).encode("utf-8")),
    }
    install_for_msbuild = str(installed_root).replace("/", "\\")
    install_for_cmake = installed_root.as_posix()
    props = (
        "<Project>\n"
        "  <PropertyGroup>\n"
        f"    <SakuraPackagePlanHash>{xml_escape(str(plan['plan_hash']))}</SakuraPackagePlanHash>\n"
        f"    <SakuraPackageTargetTriplet>{xml_escape(str(plan['target_triplet']))}</SakuraPackageTargetTriplet>\n"
        f"    <SakuraPackageInstallRoot>{xml_escape(install_for_msbuild)}\\</SakuraPackageInstallRoot>\n"
        f"    <SakuraPackageActiveMetadata>{xml_escape(str(active_metadata.resolve()))}</SakuraPackageActiveMetadata>\n"
        "    <VcpkgEnableManifest>false</VcpkgEnableManifest>\n"
        "    <VcpkgManifestInstall>false</VcpkgManifestInstall>\n"
        "    <VcpkgAutoBootstrap>false</VcpkgAutoBootstrap>\n"
        "    <VcpkgEnableClassic>true</VcpkgEnableClassic>\n"
        "    <VcpkgInstalledDir>$(SakuraPackageInstallRoot)</VcpkgInstalledDir>\n"
        "    <VcpkgTriplet>$(SakuraPackageTargetTriplet)</VcpkgTriplet>\n"
        "  </PropertyGroup>\n"
        "</Project>\n"
    )
    cmake = (
        f"set(SAKURA_PACKAGE_PLAN_HASH \"{str(plan['plan_hash'])}\" CACHE STRING \"Sakura package plan\" FORCE)\n"
        f"set(SAKURA_PACKAGE_INSTALL_ROOT \"{install_for_cmake}\" CACHE PATH \"Sakura package install root\" FORCE)\n"
        f"set(VCPKG_TARGET_TRIPLET \"{str(plan['target_triplet'])}\" CACHE STRING \"vcpkg target triplet\" FORCE)\n"
        "set(VCPKG_MANIFEST_MODE OFF CACHE BOOL \"Disable implicit vcpkg manifest restore\" FORCE)\n"
        "set(VCPKG_INSTALLED_DIR \"${SAKURA_PACKAGE_INSTALL_ROOT}\" CACHE PATH \"Explicit restored vcpkg root\" FORCE)\n"
    )
    _atomic_write_text(active_metadata, json.dumps(active, ensure_ascii=True, sort_keys=True, indent=2) + "\n")
    _atomic_write_text(active_props, props)
    _atomic_write_text(active_cmake, cmake)


@contextmanager
def _restore_lock(cache_root: Path, target_triplet: str, plan_hash: str, timeout_seconds: int) -> Iterator[Path]:
    if timeout_seconds < 1:
        raise BuildError("PACKAGE_LOCK_TIMEOUT_INVALID", "package lock timeout must be at least one second", 2)
    lock_root = cache_root / "l"
    lock_root.mkdir(parents=True, exist_ok=True)
    lock_path = lock_root / f"{target_triplet}-{plan_hash.split(':', 1)[1]}.lock"
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            lock_path.mkdir()
            break
        except FileExistsError:
            if time.monotonic() >= deadline:
                raise BuildError(
                    "PACKAGE_RESTORE_LOCK_TIMEOUT",
                    f"timed out waiting for package restore owner lock: {lock_path}",
                    8,
                )
            time.sleep(_LOCK_POLL_SECONDS)
        except OSError as error:
            raise BuildError("PACKAGE_RESTORE_LOCK_FAILED", f"could not acquire package restore lock {lock_path}: {error}", 5) from error
    try:
        _atomic_write_text(lock_path / "owner.json", json.dumps({"pid": os.getpid(), "plan_hash": plan_hash}, sort_keys=True) + "\n")
        yield lock_path
    finally:
        try:
            (lock_path / "owner.json").unlink(missing_ok=True)
            lock_path.rmdir()
        except OSError as error:
            raise BuildError("PACKAGE_RESTORE_LOCK_RELEASE_FAILED", f"could not release package restore lock {lock_path}: {error}", 9) from error


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, text=True, check=False)
    else:
        process.kill()


def _summarize_vcpkg_failure(stdout: str, stderr: str, returncode: int | None) -> str:
    """Build a failure message that keeps the cause instead of vcpkg's boilerplate."""
    lines = (stdout + "\n" + stderr).strip().splitlines()
    marker = next(
        (index for index, line in enumerate(lines) if line.strip().startswith(_VCPKG_REPORT_TAIL_MARKER)),
        None,
    )
    body = lines if marker is None else lines[:marker]

    diagnostics: list[str] = []
    for line in lines:
        if not any(needle in line for needle in _VCPKG_DIAGNOSTIC_MARKERS):
            continue
        stripped = line.strip()
        if stripped and stripped not in diagnostics:
            diagnostics.append(stripped)

    sections = [f"vcpkg exited with {returncode}"]
    if diagnostics:
        omitted = len(diagnostics) - _VCPKG_DIAGNOSTIC_LINES
        kept = diagnostics[:_VCPKG_DIAGNOSTIC_LINES]
        if omitted > 0:
            kept.append(f"... {omitted} further diagnostic line(s) omitted")
        sections.append("\n".join(kept))

    tail = body[-_VCPKG_TAIL_LINES:]
    if tail:
        heading = f"--- vcpkg output, last {len(tail)} of {len(body)} line(s)"
        heading += "; issue-report template removed ---" if marker is not None else " ---"
        sections.append(heading + "\n" + "\n".join(tail))
    return "\n\n".join(sections)


def _run_vcpkg(
    argv: Sequence[str],
    *,
    repo_root: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
) -> tuple[str, str]:
    try:
        process = subprocess.Popen(
            list(argv),
            cwd=repo_root,
            env=dict(environment),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
        )
    except OSError as error:
        raise BuildError("PACKAGE_RESTORE_START_FAILED", f"could not start vcpkg: {error}", 3) from error
    try:
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            _terminate_process_tree(process)
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            raise BuildError("PACKAGE_RESTORE_TIMEOUT", f"vcpkg restore exceeded {timeout_seconds}s", 8) from error
        if process.returncode:
            raise BuildError("PACKAGE_RESTORE_FAILED", _summarize_vcpkg_failure(stdout, stderr, process.returncode), 6)
        return stdout, stderr
    finally:
        if process.poll() is None:
            _terminate_process_tree(process)


def _restore_temp_root(plan: Mapping[str, object]) -> Path:
    entry_root = Path(plan["entry_root"])
    parent = entry_root.parent
    parent.mkdir(parents=True, exist_ok=True)
    return parent / f".r-{uuid.uuid4().hex[:12]}"


def _run_restore(plan: Mapping[str, object], *, timeout_seconds: int, environment: Mapping[str, str] | None) -> dict[str, object]:
    repo_root = Path(plan["repo_root"])
    temporary = _restore_temp_root(plan)
    temporary.mkdir(parents=True, exist_ok=False)
    installed_root = temporary / "installed"
    try:
        cache_root = Path(plan["cache_root"])
        downloads = cache_root / "downloads"
        command = [
            str(plan["vcpkg_executable"]),
            "install",
            f"--triplet={plan['target_triplet']}",
            f"--host-triplet={plan['host_triplet']}",
            f"--x-install-root={installed_root}",
            f"--x-buildtrees-root={temporary / 'buildtrees'}",
            f"--x-packages-root={temporary / 'packages'}",
            f"--downloads-root={downloads}",
            "--no-print-usage",
        ]
        env = dict(os.environ)
        if environment:
            env.update(environment)
        env["VCPKG_ROOT"] = str(plan["vcpkg_root"])
        env.pop("VCPKG_DEFAULT_TRIPLET", None)
        _run_vcpkg(command, repo_root=repo_root, environment=env, timeout_seconds=timeout_seconds)
        candidate_plan = dict(plan)
        candidate_plan["entry_root"] = temporary
        candidate_plan["installed_root"] = installed_root
        metadata = {
            "schema_version": PACKAGE_RESTORE_SCHEMA_VERSION,
            "plan_hash": plan["plan_hash"],
            "plan": plan["plan"],
            "target_triplet": plan["target_triplet"],
            "host_triplet": plan["host_triplet"],
            "package_names": plan["package_names"],
        }
        _atomic_write_text(_entry_metadata_path(temporary), json.dumps(metadata, ensure_ascii=True, sort_keys=True, indent=2) + "\n")
        valid, _, failures = _entry_valid(candidate_plan)
        if not valid:
            raise BuildError("PACKAGE_RESTORE_INCOMPLETE", "; ".join(failures), 6)
        destination = Path(plan["entry_root"])
        try:
            temporary.replace(destination)
        except OSError as error:
            raise BuildError("PACKAGE_RESTORE_PUBLISH_FAILED", f"could not atomically publish package root {destination}: {error}", 6) from error
        return metadata
    except BaseException:
        if temporary.exists():
            shutil.rmtree(temporary, ignore_errors=True)
        raise


def _result_from_plan(plan: Mapping[str, object], *, status: str, metadata: Mapping[str, object] | None, valid: bool, failures: Sequence[str] = ()) -> dict[str, object]:
    if not plan.get("required"):
        return {
            "required": False,
            "valid": True,
            "status": "not_required",
            "plan_hash": None,
            "native_restore_execution_observed": False,
            "restore_performed_this_invocation": False,
            "package_closure_validated": True,
            "package_set_ids": [],
            "package_names": [],
        }
    cache_root = Path(plan["cache_root"])
    return {
        "required": True,
        "valid": valid,
        "status": status,
        "plan_hash": plan["plan_hash"],
        "target_triplet": plan["target_triplet"],
        "host_triplet": plan["host_triplet"],
        "package_set_ids": plan["package_set_ids"],
        "package_names": plan["package_names"],
        "entry_relative": _entry_relative(plan),
        "installed_root_relative": _repo_relative(Path(plan["repo_root"]), Path(plan["installed_root"]), "PACKAGE_RESULT_PATH"),
        "active_props_relative": _repo_relative(Path(plan["repo_root"]), Path(plan["active_props"]), "PACKAGE_RESULT_PATH"),
        "active_cmake_relative": _repo_relative(Path(plan["repo_root"]), Path(plan["active_cmake"]), "PACKAGE_RESULT_PATH"),
        "completion_hash": _sha256_bytes(_canonical_json(metadata).encode("utf-8")) if metadata is not None else None,
        # ``reused`` and ``validated`` have a completed cache entry whose
        # immutable completion metadata can only have been written after the
        # explicit vcpkg owner returned successfully.  That is evidence of a
        # restore without falsely claiming that this invocation did work.
        "native_restore_execution_observed": valid and status in {"restored", "reused", "validated"},
        "restore_performed_this_invocation": status == "restored",
        "package_closure_validated": valid,
        "failures": list(failures),
    }


def restore_package_closure(
    graph: SemanticGraph,
    roots: Iterable[str],
    context_id: str,
    *,
    timeout_seconds: int = 1800,
    events: EventWriter | None = None,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Restore or reuse one graph-declared package closure and publish its active root."""

    plan = plan_package_restore(graph, roots, context_id, environment=environment)
    if not plan.get("required"):
        return _result_from_plan(plan, status="not_required", metadata=None, valid=True)
    cache_root = Path(plan["cache_root"])
    if events is not None:
        events.emit("package_restore_started", context_id=context_id, status="started", plan_hash=plan["plan_hash"])
    try:
        with _restore_lock(cache_root, str(plan["target_triplet"]), str(plan["plan_hash"]), timeout_seconds):
            valid, metadata, _ = _entry_valid(plan)
            if valid and metadata is not None:
                _write_active_projection(plan, metadata)
                _record_cache_use(plan)
                result = _result_from_plan(plan, status="reused", metadata=metadata, valid=True)
            else:
                metadata = _run_restore(plan, timeout_seconds=timeout_seconds, environment=environment)
                valid, checked_metadata, failures = _entry_valid(plan)
                if not valid or checked_metadata is None:
                    raise BuildError("PACKAGE_RESTORE_PUBLISH_INCOMPLETE", "; ".join(failures), 6)
                _write_active_projection(plan, checked_metadata)
                _record_cache_use(plan)
                result = _result_from_plan(plan, status="restored", metadata=checked_metadata, valid=True)
        if events is not None:
            events.emit(
                "package_restore_finished",
                context_id=context_id,
                status=str(result["status"]),
                plan_hash=plan["plan_hash"],
                restored=bool(result["native_restore_execution_observed"]),
            )
        return result
    except BuildError:
        if events is not None:
            events.emit("package_restore_finished", context_id=context_id, status="failed", plan_hash=plan["plan_hash"], exit_code=6)
        raise


def validate_package_restore(
    graph: SemanticGraph,
    roots: Iterable[str],
    context_id: str,
    *,
    environment: Mapping[str, str] | None = None,
) -> dict[str, object]:
    """Validate the active strict root without creating or mutating a package cache."""

    plan = plan_package_restore(graph, roots, context_id, environment=environment)
    if not plan.get("required"):
        return _result_from_plan(plan, status="not_required", metadata=None, valid=True)
    active_metadata = Path(plan["active_metadata"])
    failures: list[str] = []
    metadata: dict[str, object] | None = None
    if not active_metadata.is_file():
        failures.append(f"active package metadata is missing: {active_metadata}")
    else:
        try:
            active = _read_json(active_metadata, "PACKAGE_ACTIVE_METADATA_INVALID")
        except BuildError as error:
            failures.append(str(error))
        else:
            if active.get("plan_hash") != plan.get("plan_hash"):
                failures.append("active package plan hash differs")
            if active.get("entry_relative") != _entry_relative(plan):
                failures.append("active package entry differs")
            if active.get("target_triplet") != plan.get("target_triplet"):
                failures.append("active package triplet differs")
    valid, metadata, entry_failures = _entry_valid(plan)
    failures.extend(entry_failures)
    if valid and not failures and metadata is not None:
        expected_hash = _sha256_bytes(_canonical_json(metadata).encode("utf-8"))
        try:
            active = _read_json(active_metadata, "PACKAGE_ACTIVE_METADATA_INVALID")
        except BuildError:
            pass
        else:
            if active.get("completion_hash") != expected_hash:
                failures.append("active package completion hash differs")
    result = _result_from_plan(plan, status="validated" if not failures else "stale_or_missing", metadata=metadata, valid=not failures, failures=failures)
    return result


def package_gc(
    repo_root: Path,
    *,
    keep: int = 3,
    max_bytes: int | None = _DEFAULT_CACHE_MAX_BYTES,
    apply: bool = False,
) -> dict[str, object]:
    """Apply LRU/count/capacity policy only to inactive, unlocked cache entries."""

    if keep < 0:
        raise BuildError("PACKAGE_GC_KEEP_INVALID", "--keep must be zero or greater", 2)
    if max_bytes is not None and max_bytes < 0:
        raise BuildError("PACKAGE_GC_MAX_BYTES_INVALID", "--max-bytes must be zero or greater", 2)
    cache_root = repo_root / _PACKAGE_ROOT_RELATIVE
    entries_root = cache_root / "e"
    active_root = cache_root / "a"
    usage_root = cache_root / "u"
    locked = {
        path.stem.rsplit("-", 1)[-1]
        for path in (cache_root / "l").glob("*.lock")
        if path.is_dir()
    } if (cache_root / "l").is_dir() else set()
    active_entries: set[str] = set()
    if active_root.is_dir():
        for metadata_path in active_root.glob("*.json"):
            try:
                metadata = _read_json(metadata_path, "PACKAGE_ACTIVE_METADATA_INVALID")
            except BuildError:
                continue
            entry = metadata.get("entry_relative")
            if isinstance(entry, str):
                active_entries.add(entry.replace("\\", "/"))
    def entry_size(entry: Path) -> int:
        total = 0
        try:
            for current, directories, files in os.walk(entry, topdown=True, followlinks=False):
                directories[:] = [name for name in directories if not Path(current, name).is_symlink()]
                for name in files:
                    candidate = Path(current, name)
                    if candidate.is_symlink():
                        continue
                    try:
                        total += candidate.stat().st_size
                    except FileNotFoundError:
                        continue
        except OSError as error:
            raise BuildError("PACKAGE_GC_SIZE_FAILED", f"could not inspect cache entry {entry}: {error}", 5) from error
        return total

    def last_used(entry: Path) -> int:
        usage_path = usage_root / f"{entry.name}.json"
        if usage_path.is_file():
            try:
                metadata = _read_json(usage_path, "PACKAGE_USAGE_METADATA_INVALID")
            except BuildError:
                metadata = {}
            value = metadata.get("last_used_unix_ns")
            if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
                return value
        try:
            return entry.stat().st_mtime_ns
        except OSError as error:
            raise BuildError("PACKAGE_GC_STAT_FAILED", f"could not inspect cache entry {entry}: {error}", 5) from error

    records: list[dict[str, object]] = []
    if entries_root.is_dir():
        for entry in entries_root.iterdir():
            if not entry.is_dir() or entry.name.startswith("."):
                continue
            relative = entry.relative_to(cache_root).as_posix()
            records.append({
                "path": entry,
                "relative": relative,
                "size_bytes": entry_size(entry),
                "last_used_unix_ns": last_used(entry),
                "active": relative in active_entries,
                "locked": entry.name in locked,
            })
    records.sort(key=lambda item: (int(item["last_used_unix_ns"]), str(item["relative"])), reverse=True)
    protected = [item for item in records if bool(item["active"]) or bool(item["locked"])]
    eligible = [item for item in records if not bool(item["active"]) and not bool(item["locked"])]
    retained_by_count = eligible[:max(0, keep - len(protected))]
    candidates: list[dict[str, object]] = []
    for record in eligible[len(retained_by_count):]:
        candidates.append({**record, "reasons": ["retention_count"]})
    current_size = sum(int(item["size_bytes"]) for item in records)
    planned_size = current_size - sum(int(item["size_bytes"]) for item in candidates)
    if max_bytes is not None and planned_size > max_bytes:
        for record in sorted(retained_by_count, key=lambda item: (int(item["last_used_unix_ns"]), str(item["relative"]))):
            if planned_size <= max_bytes:
                break
            candidates.append({**record, "reasons": ["capacity"]})
            planned_size -= int(record["size_bytes"])
    candidate_paths = {Path(item["path"]) for item in candidates}
    preserved = [
        str(record["relative"])
        for record in records
        if Path(record["path"]) not in candidate_paths
    ]
    removed: list[str] = []
    if apply:
        for candidate in candidates:
            entry = Path(candidate["path"])
            relative = str(candidate["relative"])
            try:
                entry.relative_to(entries_root)
            except ValueError as error:
                raise BuildError("PACKAGE_GC_PATH_ESCAPE", f"refusing to remove outside package entries: {entry}", 5) from error
            shutil.rmtree(entry)
            _usage_record_path({"entry_root": entry, "cache_root": cache_root}).unlink(missing_ok=True)
            removed.append(relative)
    return {
        "cache_root": _repo_relative(repo_root, cache_root, "PACKAGE_GC_PATH"),
        "keep": keep,
        "max_bytes": max_bytes,
        "applied": apply,
        "entry_count": len(records),
        "total_size_bytes": current_size,
        "projected_size_bytes": planned_size,
        "capacity_satisfied": max_bytes is None or planned_size <= max_bytes,
        "candidate_entries": [str(item["relative"]) for item in candidates],
        "candidate_reasons": {str(item["relative"]): list(item["reasons"]) for item in candidates},
        "removed_entries": removed,
        "preserved_entries": sorted(preserved),
    }
