"""Native MSVC fixtures proving C++ ABI mismatch enforcement."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from .abi_contract import render_detect_mismatch_header
from .runner import BuildError, EventWriter, msvc_environment, render_command


ABI_FIXTURES = (
    "abi-pack-mismatch",
    "abi-iterator-mismatch",
    "abi-opaque-compatible",
)


@dataclass(frozen=True)
class _FixtureSpec:
    mismatch_field: str | None
    provider_value: object
    consumer_value: object
    provider_options: tuple[str, ...]
    consumer_options: tuple[str, ...]
    expected_link: str


_FIXTURES = {
    "abi-pack-mismatch": _FixtureSpec(
        "default_pack",
        8,
        1,
        ("/Zp8", "/DABI_FIXTURE_PACK_VALUE=1"),
        ("/Zp1", "/DABI_FIXTURE_PACK_VALUE=1"),
        "failure",
    ),
    "abi-iterator-mismatch": _FixtureSpec(
        "iterator_debug_level",
        2,
        0,
        ("/Zp8", "/D_ITERATOR_DEBUG_LEVEL=2", "/DABI_FIXTURE_STL_VALUE=1"),
        ("/Zp8", "/D_ITERATOR_DEBUG_LEVEL=0", "/DABI_FIXTURE_STL_VALUE=1"),
        "failure",
    ),
    # Packing deliberately differs, but an opaque C handle contract only
    # stamps abi_family and arch. This must remain link-compatible.
    "abi-opaque-compatible": _FixtureSpec(None, 8, 1, ("/Zp8",), ("/Zp1",), "success"),
}


@dataclass(frozen=True)
class _CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float


def _terminate_process_tree(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            check=False,
            text=True,
        )
    else:
        process.kill()


def _run(
    argv: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
    events: EventWriter,
) -> _CommandResult:
    started = time.perf_counter()
    events.emit("command_started", argv=list(argv), cwd=str(cwd), fixture=True)
    process = subprocess.Popen(
        list(argv),
        cwd=cwd,
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        _terminate_process_tree(process)
        stdout, stderr = process.communicate()
        events.emit("command_finished", argv=list(argv), exit_code=8, status="timeout", fixture=True)
        raise BuildError("ABI_FIXTURE_TIMEOUT", f"command exceeded {timeout_seconds}s: {render_command(argv)}", 8) from error
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    events.emit("command_finished", argv=list(argv), exit_code=process.returncode, status="success" if process.returncode == 0 else "failure", fixture=True)
    return _CommandResult(tuple(argv), process.returncode, stdout, stderr, elapsed_ms)


def _write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _command_record(result: _CommandResult) -> dict[str, object]:
    diagnostic = (result.stdout + "\n" + result.stderr).strip()
    return {
        "argv": list(result.argv),
        "returncode": result.returncode,
        "elapsed_ms": round(result.elapsed_ms, 3),
        "diagnostic_tail": diagnostic.splitlines()[-40:],
    }


def run_abi_fixture(
    repo_root: Path,
    fixture_name: str,
    context_id: str,
    timeout_seconds: int,
    events: EventWriter,
) -> dict[str, object]:
    if fixture_name not in _FIXTURES:
        raise BuildError("ABI_FIXTURE_UNKNOWN", f"unknown ABI fixture: {fixture_name}", 2)
    if context_id != "msvc-x64-debug":
        raise BuildError("ABI_FIXTURE_CONTEXT", f"ABI fixtures currently require msvc-x64-debug, got {context_id}", 2)
    if timeout_seconds < 1:
        raise BuildError("TIMEOUT_INVALID", "--timeout-seconds must be at least 1", 2)

    spec = _FIXTURES[fixture_name]
    environment = msvc_environment(repo_root)
    cl = shutil.which("cl.exe", path=environment.get("PATH"))
    linker = shutil.which("link.exe", path=environment.get("PATH"))
    if cl is None or linker is None:
        raise BuildError("TOOL_MSVC_NOT_FOUND", "cl.exe and link.exe are required for ABI fixtures", 3)

    work = repo_root / "build/evidence/r1a/abi" / fixture_name
    work.mkdir(parents=True, exist_ok=True)
    edge_key = "sakura.edge.abi-fixture"
    if spec.mismatch_field is None:
        provider_stamps = ((f"{edge_key}.abi_family", "msvc"), (f"{edge_key}.arch", "x64"))
        consumer_stamps = provider_stamps
    else:
        provider_stamps = ((f"{edge_key}.{spec.mismatch_field}", spec.provider_value),)
        consumer_stamps = ((f"{edge_key}.{spec.mismatch_field}", spec.consumer_value),)
    provider_header = work / "provider.contract.h"
    consumer_header = work / "consumer.contract.h"
    _write_text(provider_header, render_detect_mismatch_header(provider_stamps))
    _write_text(consumer_header, render_detect_mismatch_header(consumer_stamps))

    source_root = repo_root / "tools/build/fixtures/abi"
    provider_obj = work / "provider.obj"
    consumer_obj = work / "consumer.obj"
    executable = work / f"{fixture_name}.exe"
    common = ("/nologo", "/c", "/EHsc", "/std:c++20", "/MTd")
    commands = [
        [cl, *common, *spec.provider_options, f"/FI{provider_header}", f"/Fo{provider_obj}", str(source_root / "provider.cpp")],
        [cl, *common, *spec.consumer_options, f"/FI{consumer_header}", f"/Fo{consumer_obj}", str(source_root / "consumer.cpp")],
        [linker, "/nologo", f"/OUT:{executable}", str(consumer_obj), str(provider_obj)],
    ]

    results: list[_CommandResult] = []
    for command in commands[:2]:
        result = _run(command, work, environment, timeout_seconds, events)
        results.append(result)
        if result.returncode:
            raise BuildError("ABI_FIXTURE_COMPILE_FAILED", f"fixture compile failed: {render_command(command)}", 6)
    link_result = _run(commands[2], work, environment, timeout_seconds, events)
    results.append(link_result)
    link_diagnostic = link_result.stdout + "\n" + link_result.stderr

    if spec.expected_link == "failure":
        expected_key = f"{edge_key}.{spec.mismatch_field}"
        ok = link_result.returncode != 0 and "LNK2038" in link_diagnostic and expected_key in link_diagnostic
        terminal = "expected_failure" if ok else "unexpected_link_result"
    else:
        expected_key = None
        ok = link_result.returncode == 0
        terminal = "linked" if ok else "unexpected_link_result"
        if ok:
            execution = _run([str(executable)], work, environment, timeout_seconds, events)
            results.append(execution)
            ok = execution.returncode == 0
            terminal = "success" if ok else "runtime_failure"

    evidence = {
        "schema_version": 1,
        "ok": ok,
        "fixture": fixture_name,
        "context_id": context_id,
        "expected_link": spec.expected_link,
        "expected_mismatch_field": spec.mismatch_field,
        "expected_mismatch_key": expected_key,
        "terminal_state": terminal,
        "commands": [_command_record(result) for result in results],
    }
    evidence_path = work / "evidence.json"
    _write_text(evidence_path, json.dumps(evidence, ensure_ascii=False, sort_keys=True, indent=2) + "\n")
    return {**evidence, "evidence": str(evidence_path.relative_to(repo_root).as_posix())}
