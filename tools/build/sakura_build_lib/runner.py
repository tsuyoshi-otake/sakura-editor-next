"""Native backend command construction and execution.

The canonical CLI owns validation and the build intent.  MSBuild and CMake
remain the execution engines and own their native task schedulers.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence, TextIO


COMPONENT_ISOLATED_ENVIRONMENT = (
    "CMAKE_TOOLCHAIN_FILE",
    "CMAKE_PROJECT_TOP_LEVEL_INCLUDES",
    "CMAKE_PREFIX_PATH",
    "CMAKE_USER_MAKE_RULES_OVERRIDE",
    "CMAKE_USER_MAKE_RULES_OVERRIDE_CXX",
    "VCPKG_ROOT",
    "VCPKG_DEFAULT_TRIPLET",
    "VCPKG_TARGET_TRIPLET",
    "VCPKG_FEATURE_FLAGS",
)
_CMAKE_DISCOVERY_ENVIRONMENT_VARIABLES = frozenset(COMPONENT_ISOLATED_ENVIRONMENT)
NATIVE_PATH_IDENTITY_FILE = ".sakura-native-path.json"
MSVC_SHOWINCLUDES_PREFIX = "Note: including file:"


def cmake_component_build_dir(repo_root: Path, component_id: str, context_id: str) -> Path:
    """Return a stable, short CMake build directory for one component closure.

    Generated component roots intentionally live below a descriptive source
    directory, but reproducing that full context/component spelling in the
    native build directory pushes MSVC object and PDB paths past MAX_PATH for
    otherwise valid contract runners.  The generated source path and every
    command record retain the human-readable identity; the disposable native
    cache uses a collision-resistant token solely to keep its path budget
    bounded on normal checkouts as well as cloned rebuild workspaces.
    """

    identity = f"{context_id}\0{component_id}".encode("utf-8")
    token = hashlib.sha256(identity).hexdigest()[:16]
    return repo_root / "build/components/cmake" / token


class BuildError(RuntimeError):
    def __init__(self, code: str, message: str, exit_code: int = 6) -> None:
        super().__init__(message)
        self.code = code
        self.exit_code = exit_code


@dataclass(frozen=True)
class Parallelism:
    budget: int
    projects: int
    compiler_processes: int


def allocate_parallelism(budget: int) -> Parallelism:
    if budget < 1:
        raise BuildError("JOBS_INVALID", "--jobs must be at least 1", 2)
    if budget == 1:
        return Parallelism(1, 1, 1)
    projects = max(1, int(budget**0.5))
    compiler_processes = max(1, budget // projects)
    while projects * compiler_processes > budget:
        compiler_processes -= 1
    return Parallelism(budget, projects, compiler_processes)


class EventWriter:
    def __init__(self, stream: TextIO | None = None) -> None:
        self._stream = stream
        self._command_id = str(uuid.uuid4())

    def emit(self, event: str, **payload: object) -> None:
        if self._stream is None:
            return
        record = {
            "schema_version": 1,
            "timestamp": time.time_ns(),
            "command_id": self._command_id,
            "task_id": payload.pop("task_id", None),
            "phase": event,
            "component_id": payload.pop("component_id", None),
            "context_id": payload.pop("context_id", None),
            "status": payload.pop("status", None),
            "exit_code": payload.pop("exit_code", None),
            "dependency_reason": payload.pop("dependency_reason", None),
            **payload,
        }
        self._stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
        self._stream.flush()


def _is_ascii_path(path: Path) -> bool:
    try:
        str(path).encode("ascii")
    except UnicodeEncodeError:
        return False
    return True


def _native_alias_candidates(repo_root: Path) -> tuple[Path, ...]:
    candidates: list[Path] = []
    home = Path.home().resolve()
    for candidate in repo_root.parents:
        if (
            candidate == Path(candidate.anchor)
            or candidate == home
            or candidate in home.parents
            or not _is_ascii_path(candidate)
        ):
            continue
        candidates.append(candidate)
    temporary = Path(tempfile.gettempdir()).resolve()
    if temporary != Path(temporary.anchor) and _is_ascii_path(temporary):
        candidates.append(temporary)
    return tuple(sorted(dict.fromkeys(candidates), key=lambda item: (len(str(item)), str(item).lower())))


@contextmanager
def native_execution_root(
    repo_root: Path,
    events: EventWriter | None = None,
    *,
    preferred_alias: Path | None = None,
):
    """Yield an ASCII path to the same checkout for fragile Windows tools.

    VS-bundled CMake 3.31 can terminate with ``0xC0000409`` before compiler
    detection when either its source or build tree contains non-ASCII path
    segments.  A short-lived directory junction keeps the checkout and output
    identity unchanged while preventing the native tool from seeing a lossy
    path spelling.  The junction is never a copy and is removed on every exit.
    """
    resolved = repo_root.resolve()
    if os.name != "nt" or _is_ascii_path(resolved):
        yield resolved
        return

    candidates = _native_alias_candidates(resolved)
    if preferred_alias is not None:
        requested = Path(os.path.abspath(preferred_alias))
        allowed_parents = {candidate / ".snp" for candidate in candidates}
        if not _is_ascii_path(requested) or requested.parent not in allowed_parents:
            raise BuildError(
                "NATIVE_PATH_ALIAS_INVALID",
                f"recorded native path alias is outside an allowed ASCII temporary root: {requested}",
                3,
            )
        aliases = (requested,)
    else:
        aliases = tuple(candidate / ".snp" / f"{os.getpid():x}-{uuid.uuid4().hex[:8]}" for candidate in candidates)

    errors: list[str] = []
    alias: Path | None = None
    for current_alias in aliases:
        base = current_alias.parent
        try:
            base.mkdir(parents=True, exist_ok=True)
            if current_alias.exists():
                raise OSError("alias path is already in use")
            completed = subprocess.run(
                [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "mklink", "/J", str(current_alias), str(resolved)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
            if completed.returncode or not current_alias.is_dir():
                detail = (completed.stderr or completed.stdout).strip()
                raise OSError(f"mklink exit {completed.returncode}: {detail}")
            alias = current_alias
            break
        except OSError as error:
            errors.append(f"{current_alias}: {error}")
            try:
                current_alias.rmdir()
            except OSError:
                pass
            try:
                base.rmdir()
            except OSError:
                pass

    if alias is None:
        detail = "; ".join(errors) if errors else "no writable ASCII ancestor or temporary directory"
        raise BuildError("NATIVE_PATH_ALIAS_UNAVAILABLE", f"could not create an ASCII checkout alias for {resolved}: {detail}", 3)

    if events is not None:
        events.emit("native_path_alias_created", cwd=str(resolved), alias=str(alias))
    try:
        yield alias
    finally:
        cleanup_error: OSError | None = None
        try:
            alias.rmdir()
        except OSError as error:
            cleanup_error = error
        try:
            alias.parent.rmdir()
        except OSError:
            # Concurrent aliases legitimately keep the shared parent non-empty.
            pass
        if events is not None:
            events.emit(
                "native_path_alias_removed",
                cwd=str(resolved),
                alias=str(alias),
                status="failed" if cleanup_error else "success",
            )
        if cleanup_error is not None:
            raise BuildError("NATIVE_PATH_ALIAS_CLEANUP_FAILED", f"could not remove native path alias {alias}: {cleanup_error}", 9)


def write_native_path_identity(build_dir: Path, repo_root: Path, execution_root: Path) -> None:
    """Persist the lexical native root so later evidence can recreate it."""
    try:
        same_checkout = os.path.samefile(repo_root, execution_root)
    except OSError as error:
        raise BuildError("NATIVE_PATH_IDENTITY_UNAVAILABLE", f"could not compare {repo_root} and {execution_root}: {error}", 9) from error
    if not same_checkout:
        raise BuildError("NATIVE_PATH_IDENTITY_MISMATCH", f"native path alias does not reference the checkout: {execution_root}", 9)
    payload = {
        "schema_version": 1,
        "repo_root": str(repo_root.resolve()),
        "execution_root": str(execution_root.absolute()),
    }
    destination = build_dir / NATIVE_PATH_IDENTITY_FILE
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f"{destination.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def cleanup_native_aliases_for_process(repo_root: Path, process_id: int) -> tuple[str, ...]:
    """Remove only junctions owned by a terminated canonical CLI process."""
    if os.name != "nt":
        return ()
    resolved = repo_root.resolve()
    removed: list[str] = []
    failures: list[str] = []
    prefix = f"{process_id:x}-"
    for candidate in _native_alias_candidates(resolved):
        base = candidate / ".snp"
        if not base.is_dir():
            continue
        for alias in base.iterdir():
            if not alias.name.startswith(prefix) or not alias.is_dir():
                continue
            try:
                if not os.path.samefile(resolved, alias):
                    continue
                alias.rmdir()
                removed.append(str(alias))
            except OSError as error:
                failures.append(f"{alias}: {error}")
        try:
            base.rmdir()
        except OSError:
            pass
    if failures:
        raise BuildError("NATIVE_PATH_ALIAS_CLEANUP_FAILED", "; ".join(failures), 9)
    return tuple(removed)


def _existing_executable(value: str | None) -> str | None:
    if not value:
        return None
    candidate = Path(value.strip('"'))
    if candidate.is_file():
        return str(candidate)
    located = shutil.which(value)
    return located


def find_msbuild(repo_root: Path, environment: Mapping[str, str] | None = None) -> str:
    env = dict(environment or os.environ)
    explicit = _existing_executable(env.get("CMD_MSBUILD"))
    if explicit:
        return explicit
    direct = shutil.which("MSBuild.exe") or shutil.which("msbuild.exe")
    if direct:
        return direct

    candidates = [
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio/Installer/vswhere.exe",
        repo_root / "tools/vswhere/vswhere.exe",
    ]
    vswhere = next((path for path in candidates if path.is_file()), None)
    if vswhere is None:
        raise BuildError("TOOL_MSBUILD_NOT_FOUND", "MSBuild.exe was not found; set CMD_MSBUILD or install Visual Studio", 3)
    version = env.get("NUM_VSVERSION")
    version_args: list[str] = []
    if version:
        try:
            major = int(version)
        except ValueError as error:
            raise BuildError("VS_VERSION_INVALID", f"NUM_VSVERSION must be an integer, got {version!r}", 2) from error
        version_args = ["-version", f"[{major}.0,{major + 1}.0)"]
    command = [
        str(vswhere),
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.Component.MSBuild",
        *version_args,
        "-find",
        r"MSBuild\**\Bin\MSBuild.exe",
    ]
    result = subprocess.run(command, cwd=repo_root, check=False, capture_output=True, text=True)
    match = next((line.strip() for line in result.stdout.splitlines() if Path(line.strip()).is_file()), None)
    if not match:
        raise BuildError("TOOL_MSBUILD_NOT_FOUND", "vswhere did not find MSBuild.exe", 3)
    return match


def find_cmake_tool(name: str) -> str:
    direct = shutil.which(f"{name}.exe") or shutil.which(name)
    if direct:
        return direct
    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    candidates = [
        program_files / f"Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/{name}.exe",
        program_files / f"Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/{name}.exe",
        program_files / f"Microsoft Visual Studio/2022/Enterprise/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/{name}.exe",
        program_files / f"Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{name}.exe",
        program_files / f"Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{name}.exe",
        program_files / f"Microsoft Visual Studio/2022/Enterprise/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/{name}.exe",
    ]
    match = next((path for path in candidates if path.is_file()), None)
    if match is None:
        raise BuildError("TOOL_CMAKE_NOT_FOUND", f"{name}.exe was not found", 3)
    return str(match)


def msvc_environment(repo_root: Path, environment: Mapping[str, str] | None = None) -> dict[str, str]:
    """Return the x64 native-tools environment without mutating the parent shell."""
    base = dict(environment or os.environ)
    if base.get("VSCMD_VER") and _existing_executable("cl.exe"):
        return base
    msbuild = Path(find_msbuild(repo_root, base)).resolve()
    installation = next(
        (parent for parent in msbuild.parents if (parent / "VC/Auxiliary/Build/vcvarsall.bat").is_file()),
        None,
    )
    if installation is None:
        raise BuildError("TOOL_VCVARS_NOT_FOUND", f"vcvarsall.bat was not found above {msbuild}", 3)
    vcvarsall = installation / "VC/Auxiliary/Build/vcvarsall.bat"
    temporary_root = repo_root / "build/components"
    temporary_root.mkdir(parents=True, exist_ok=True)
    handle, script_name = tempfile.mkstemp(prefix=".sakura-vcvars-", suffix=".cmd", dir=temporary_root)
    script_path = Path(script_name)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\r\n") as stream:
            stream.write(f'@call "{vcvarsall}" x64 >nul\n')
            stream.write("@if errorlevel 1 exit /b %errorlevel%\n")
            stream.write("@set\n")
        command_line = f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /c ""{script_path}""'
        completed = subprocess.run(
            command_line,
            cwd=repo_root,
            env=base,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    finally:
        script_path.unlink(missing_ok=True)
    if completed.returncode:
        detail = (completed.stderr or completed.stdout).strip().splitlines()
        suffix = f": {detail[-1]}" if detail else ""
        raise BuildError("TOOL_VCVARS_FAILED", f"vcvarsall.bat failed with exit code {completed.returncode}{suffix}", 3)
    result = dict(base)
    for line in completed.stdout.splitlines():
        if "=" not in line or line.startswith("="):
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def mingw_environment(environment: Mapping[str, str] | None = None) -> dict[str, str]:
    env = dict(environment or os.environ)
    prefixes = [Path(r"C:\msys64\mingw64\bin"), Path(r"C:\msys64\usr\bin")]
    existing = env.get("PATH", "").split(os.pathsep)
    normalized = {os.path.normcase(os.path.normpath(item)) for item in existing if item}
    additions = [str(path) for path in prefixes if path.is_dir() and os.path.normcase(str(path)) not in normalized]
    return {"PATH": os.pathsep.join(additions + existing)}


_MSBUILD_PERFORMANCE_SUMMARY_TRUE = {"1", "true"}
_MSBUILD_PERFORMANCE_SUMMARY_FALSE = {"0", "false"}


def msbuild_binlog_arguments(env: Mapping[str, str]) -> list[str]:
    """``/bl:`` switch for ``SAKURA_MSBUILD_BINLOG``, a diagnostic-only escape hatch.

    Unset leaves the command unchanged. A set-but-empty value is rejected
    rather than silently treated as unset, since that is almost always a
    shell-quoting mistake, not an intentional choice.
    """
    value = env.get("SAKURA_MSBUILD_BINLOG")
    if value is None:
        return []
    path = value.strip()
    if not path:
        raise BuildError("MSBUILD_BINLOG_INVALID", "SAKURA_MSBUILD_BINLOG must not be empty when set", 2)
    return [f"/bl:{path}"]


def msbuild_performance_summary_arguments(env: Mapping[str, str]) -> list[str]:
    """``/clp:PerformanceSummary`` switch for ``SAKURA_MSBUILD_PERFORMANCE_SUMMARY``.

    Only the ``1``/``true`` and ``0``/``false`` spellings are accepted, matching
    ``SAKURA_GENERATE_ASSEMBLY_LISTINGS``. Any other value fails explicitly
    instead of being treated as falsy, so a typo cannot silently disable the
    diagnostic.
    """
    value = env.get("SAKURA_MSBUILD_PERFORMANCE_SUMMARY")
    if value is None:
        return []
    normalized = value.strip().lower()
    if normalized in _MSBUILD_PERFORMANCE_SUMMARY_TRUE:
        return ["/clp:PerformanceSummary"]
    if normalized in _MSBUILD_PERFORMANCE_SUMMARY_FALSE:
        return []
    raise BuildError(
        "MSBUILD_PERFORMANCE_SUMMARY_INVALID",
        f"SAKURA_MSBUILD_PERFORMANCE_SUMMARY must be 1/true or 0/false, got {value!r}",
        2,
    )


def msbuild_command(
    repo_root: Path,
    target: Path,
    platform: str,
    configuration: str,
    jobs: int,
    *,
    build_target: str = "Build",
    environment: Mapping[str, str] | None = None,
    log_file: Path | None = None,
) -> list[str]:
    parallel = allocate_parallelism(jobs)
    env = dict(environment or os.environ)
    command = [
        find_msbuild(repo_root, env),
        str(target),
        f"/p:Platform={platform}",
        f"/p:Configuration={configuration}",
        f"/t:{build_target}",
        "/nr:false",
        f"/m:{parallel.projects}",
    ]
    command.extend(["/p:MultiProcessorCompilation=true", f"/p:CL_MPCount={parallel.compiler_processes}"])
    if log_file is not None:
        command.extend(msbuild_file_logger_arguments(log_file))
    command.extend(msbuild_binlog_arguments(env))
    command.extend(msbuild_performance_summary_arguments(env))
    return command


def msbuild_log_path(repo_root: Path, platform: str, configuration: str) -> Path:
    """Where ``zipArtifacts.bat`` expects to find the MSBuild log."""
    return repo_root / "build" / "logs" / f"msbuild-{platform}-{configuration}.log"


def msbuild_file_logger_arguments(log_file: Path) -> list[str]:
    """File logger switches that write the log the packaging step requires.

    ``zipArtifacts.bat`` copies ``build/logs/msbuild-<platform>-<configuration>.log``
    as a *required* artifact, so a build that never writes it makes packaging
    fail after an otherwise successful compile.  Only the builds whose output is
    packaged ask for this log; the evidence collectors reuse ``msbuild_command``
    for verification rebuilds and must not overwrite the product build's log.
    MSBuild does not create the directory of ``LogFile``, hence the mkdir here.
    """
    log_file.parent.mkdir(parents=True, exist_ok=True)
    return ["/fileLogger", f"/fileLoggerParameters:LogFile={log_file};Verbosity=normal;Encoding=UTF-8"]


def cmake_commands(
    repo_root: Path,
    configuration: str,
    jobs: int,
    *,
    run_tests: bool,
    package_cmake_config: Path | None = None,
) -> list[list[str]]:
    allocate_parallelism(jobs)
    build_dir = repo_root / "build/MinGW"
    triplet = os.environ.get("VCPKG_TARGET_TRIPLET", "x64-mingw-static")
    toolchain = repo_root / "src/main/cmake/sakura-vcpkg-toolchain.cmake"
    if not toolchain.is_file():
        raise BuildError("PACKAGE_CMAKE_TOOLCHAIN_MISSING", f"explicit package toolchain is missing: {toolchain}", 4)
    active_config = package_cmake_config or (repo_root / "build/pkg/v/a" / f"{triplet}.cmake")
    if not active_config.is_absolute():
        active_config = repo_root / active_config
    cmake = find_cmake_tool("cmake")
    commands = [
        [
            cmake, "-S", str(repo_root), "-B", str(build_dir),
            f"-DCMAKE_BUILD_TYPE={configuration}", "-DBUILD_PLATFORM=MinGW",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
            f"-DSAKURA_PACKAGE_CONFIG={active_config.as_posix()}",
            f"-DVCPKG_TARGET_TRIPLET={triplet}",
        ],
        [cmake, "--build", str(build_dir), "--config", configuration, "--target", "sakura", "--parallel", str(jobs)],
        [cmake, "--build", str(build_dir), "--config", configuration, "--target", "sakura_lang_en_US", "--parallel", str(jobs)],
        [cmake, "--build", str(build_dir), "--config", configuration, "--target", "tests1", "--parallel", str(jobs)],
    ]
    if run_tests:
        commands.append([find_cmake_tool("ctest"), "--test-dir", str(build_dir), "--build-config", configuration, "--parallel", str(jobs), "--output-on-failure"])
    return commands


def cmake_component_commands(
    repo_root: Path,
    component_id: str,
    context_id: str,
    configuration: str,
    toolchain: str,
    jobs: int,
    *,
    configure_if_needed: bool = True,
) -> list[list[str]]:
    """Configure and build one generated component root without root vcpkg state.

    ``configure_if_needed`` is disabled by the rebuild-closure rehearsal after
    its clean phase.  That keeps the no-op and mutation measurements honest:
    an existing native tree must be built as-is, while the normal component
    build path retains automatic configure/reconfigure behavior.
    """
    allocate_parallelism(jobs)
    source_dir = repo_root / f"src/main/modules/generated/cmake/projects/{context_id}/{component_id}"
    if not (source_dir / "CMakeLists.txt").is_file():
        raise BuildError("COMPONENT_CMAKE_PROJECT_MISSING", f"generated CMake project is missing: {source_dir}", 4)
    build_dir = cmake_component_build_dir(repo_root, component_id, context_id)
    component_build_root = (repo_root / "build/components").resolve()
    resolved_build_dir = build_dir.resolve()
    try:
        resolved_build_dir.relative_to(component_build_root)
    except ValueError as error:
        raise BuildError("COMPONENT_BUILD_PATH_ESCAPE", f"component build directory escapes build/components: {build_dir}", 2) from error
    build_dir.mkdir(parents=True, exist_ok=True)
    # CMake's Visual Studio compiler probe creates a temporary vcxproj below
    # the build directory.  Without these nearest sentinels MSBuild walks up to
    # the repository root and imports its global vcpkg manifest settings.
    # That would make the nominally package-free component configure non-hermetic.
    for sentinel_name in ("Directory.Build.props", "Directory.Build.targets"):
        sentinel = build_dir / sentinel_name
        text = "<Project />\n"
        try:
            current = sentinel.read_text(encoding="utf-8")
        except FileNotFoundError:
            current = None
        if current != text:
            sentinel.write_text(text, encoding="utf-8", newline="\n")
    cmake = find_cmake_tool("cmake")
    configure = [cmake, "-S", str(source_dir), "-B", str(build_dir)]
    if toolchain == "msvc":
        generator = "Ninja"
        configure.extend(["-G", generator, f"-DCMAKE_MAKE_PROGRAM={find_cmake_tool('ninja')}", f"-DCMAKE_BUILD_TYPE={configuration}"])
    elif toolchain == "mingw":
        generator = "MinGW Makefiles"
        configure.extend(["-G", generator, f"-DCMAKE_BUILD_TYPE={configuration}"])
    else:
        raise BuildError("COMPONENT_TOOLCHAIN_UNSUPPORTED", f"unsupported component CMake toolchain: {toolchain}", 2)
    build = [cmake, "--build", str(build_dir), "--config", configuration, "--target", component_id, "--parallel", str(jobs)]
    commands: list[list[str]] = []
    if configure_if_needed and _cmake_component_needs_configure(source_dir, build_dir, generator, configuration):
        commands.append(configure)
    commands.append(build)
    return commands


def _cmake_component_needs_configure(
    source_dir: Path,
    build_dir: Path,
    generator: str,
    configuration: str,
) -> bool:
    """Return whether an explicit configure is required for a component tree.

    Once a Ninja/Makefiles tree is valid, the native build graph owns automatic
    reconfigure when one of CMake's declared inputs changes.  Re-running an
    unconditional configure in the canonical no-op path adds a serial process
    without improving correctness.  A moved checkout/alias, generator change,
    configuration change, missing cache, or missing native build file still
    forces an explicit configure.
    """

    cache_path = build_dir / "CMakeCache.txt"
    native_build_file = build_dir / ("build.ninja" if generator == "Ninja" else "Makefile")
    if not cache_path.is_file() or not native_build_file.is_file():
        return True
    entries: dict[str, str] = {}
    try:
        for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line or line.startswith(("#", "//")) or "=" not in line:
                continue
            key_and_type, value = line.split("=", 1)
            key = key_and_type.split(":", 1)[0]
            entries[key] = value
    except OSError:
        return True
    cached_source = entries.get("CMAKE_HOME_DIRECTORY")
    if not cached_source:
        return True
    expected_source = os.path.normcase(os.path.abspath(source_dir))
    actual_source = os.path.normcase(os.path.abspath(cached_source))
    if (
        actual_source != expected_source
        or entries.get("CMAKE_GENERATOR") != generator
        or entries.get("CMAKE_BUILD_TYPE") != configuration
    ):
        return True
    if generator == "Ninja":
        # Existing trees may have been configured before the component
        # environment pinned VSLANG.  Reconfigure those trees so Ninja's
        # MSVC dependency scanner uses a prefix it can match deterministically
        # instead of silently recording zero header dependencies.
        compiler_files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        if compiler_files:
            try:
                compiler_text = compiler_files[0].read_text(encoding="utf-8", errors="replace")
            except OSError:
                return True
            if f'CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "{MSVC_SHOWINCLUDES_PREFIX}' not in compiler_text:
                return True
    return False


def cmake_component_test_commands(
    repo_root: Path,
    component_id: str,
    context_id: str,
    configuration: str,
    jobs: int,
) -> list[list[str]]:
    allocate_parallelism(jobs)
    build_dir = cmake_component_build_dir(repo_root, component_id, context_id)
    return [[
        find_cmake_tool("ctest"),
        "--test-dir",
        str(build_dir),
        "--build-config",
        configuration,
        "--parallel",
        str(jobs),
        "--output-on-failure",
    ]]


def render_command(command: Sequence[str]) -> str:
    return subprocess.list2cmdline(list(command))


def _without_cmake_discovery_environment(environment: Mapping[str, str]) -> dict[str, str]:
    """Remove only ambient package/toolchain discovery inputs for components."""
    blocked = {name.upper() for name in _CMAKE_DISCOVERY_ENVIRONMENT_VARIABLES}
    return {key: value for key, value in environment.items() if key.upper() not in blocked}


def run_commands(
    commands: Iterable[Sequence[str]],
    repo_root: Path,
    *,
    dry_run: bool,
    events: EventWriter,
    environment: Mapping[str, str] | None = None,
    failure_exit_code: int = 6,
    isolate_cmake_environment: bool = False,
) -> int:
    env = dict(os.environ)
    if environment:
        env.update(environment)
    if isolate_cmake_environment:
        env = _without_cmake_discovery_environment(env)
        # CMake's Ninja/MSVC dependency scanner persists the compiler's
        # localized `/showIncludes` prefix in ``rules.ninja``.  A locale
        # dependent prefix can be decoded differently by CMake (for example
        # after a UTF-8 code-page change), leaving Ninja with zero recorded
        # header dependencies and making component-boundary evidence
        # incomplete.  Component builds are hermetic, so force the stable
        # English MSVC diagnostic prefix for both configure and build.
        env["VSLANG"] = "1033"
    env["MSBUILDDISABLENODEREUSE"] = "1"
    for command in commands:
        rendered = render_command(command)
        print(rendered)
        events.emit("command_started", argv=list(command), cwd=str(repo_root), dry_run=dry_run)
        if dry_run:
            events.emit("command_finished", argv=list(command), exit_code=0, dry_run=True)
            continue
        try:
            completed = subprocess.run(list(command), cwd=repo_root, env=env, check=False)
        except KeyboardInterrupt:
            events.emit("command_finished", argv=list(command), exit_code=130, interrupted=True)
            return 8
        events.emit("command_finished", argv=list(command), exit_code=completed.returncode)
        if completed.returncode:
            return failure_exit_code
    return 0


def distribution_commands(repo_root: Path, platform: str, configuration: str, jobs: int) -> list[list[str]]:
    commands = [
        msbuild_command(
            repo_root,
            repo_root / "sakura.sln",
            platform,
            configuration,
            jobs,
            log_file=msbuild_log_path(repo_root, platform, configuration),
        )
    ]
    for script, arguments in (
        ("build-chm.bat", []),
        ("build-installer.bat", [platform, configuration]),
        ("zipArtifacts.bat", [platform, configuration]),
    ):
        commands.append(["cmd.exe", "/d", "/c", str(repo_root / script), *arguments])
    return commands
