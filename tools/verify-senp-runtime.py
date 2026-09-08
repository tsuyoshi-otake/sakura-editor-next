#!/usr/bin/env python3
"""Build isolated SENP v2 peers, then run the native lifecycle acceptance suites.

Prerequisite: build-sln.bat x64 Debug. No fixture is installed as a package or
copied into a distribution directory. Every child has a timeout and cleanup owner.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def matching_processes(paths: list[Path]) -> list[dict]:
    # Executable identities only, never an arbitrary name-wide process kill.
    literals = ",".join("'" + str(path.resolve()).replace("'", "''") + "'" for path in paths)
    script = (
        "$expected=@(" + literals + "); "
        "$matches=@(Get-CimInstance Win32_Process | Where-Object { "
        "$_.ExecutablePath -and $expected -contains $_.ExecutablePath } | "
        "Select-Object ProcessId,ParentProcessId,ExecutablePath); "
        "ConvertTo-Json -InputObject $matches -Compress"
    )
    result = subprocess.run(["pwsh", "-NoProfile", "-Command", script],
                            capture_output=True, text=True, check=True, timeout=20,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    return json.loads(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path.home() / "tmp" / "senp-runtime-fixtures")
    parser.add_argument("--tests1", type=Path, default=ROOT / "x64/Debug/tests1.exe")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--prepare-only", action="store_true", help="build fixtures without claiming a test pass")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("the native v2 process acceptance path requires Windows")
    output = args.output_dir.resolve()
    if output == Path.home().resolve() or output == Path(output.anchor):
        parser.error("output-dir must be a dedicated work-artifact directory")
    output.mkdir(parents=True, exist_ok=True)
    tests = args.tests1.resolve()
    if not tests.is_file() and not args.prepare_only:
        parser.error("tests1 does not exist; run build-sln.bat x64 Debug first")
    target = ROOT / "rust/senp/target"
    executables = [output / "sakura-senp-host.exe", output / "senp-host-fixture.exe", tests]
    existing = matching_processes(executables)
    if existing:
        raise RuntimeError(f"matching processes already running; not owned by this run: {existing}")
    steps: list[dict] = []

    def run(name: str, command: list[str], cwd: Path, timeout: int, env=None) -> None:
        started = time.monotonic()
        log = output / f"{name}.log"
        print(f"{name}: {' '.join(command)}", flush=True)
        with log.open("w", encoding="utf-8") as stream:
            child = subprocess.Popen(command, cwd=cwd, env=env, stdout=stream,
                                     stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = child.wait(timeout=timeout)
            except BaseException as error:
                subprocess.run(["taskkill.exe", "/PID", str(child.pid), "/T", "/F"],
                               capture_output=True, timeout=20,
                               creationflags=subprocess.CREATE_NO_WINDOW)
                child.wait(timeout=20)
                code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
        steps.append({"name": name, "pid": child.pid, "exited": child.poll() is not None,
                      "exitCode": code, "seconds": round(time.monotonic() - started, 3),
                      "log": str(log), "logSha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        print(log.read_text(encoding="utf-8", errors="replace"), flush=True)
        if code:
            raise RuntimeError(f"{name} exited {code}")

    status = "failed"
    try:
        cargo = ["cargo", "build", "--locked", "--target-dir", str(target)]
        if args.offline:
            cargo.append("--offline")
        run("build-peers", cargo + ["-p", "sakura-senp-host", "-p", "sakura-senp-tool", "--features", "test-fixtures"],
            ROOT / "rust/senp", 600)
        run("build-guest", cargo + ["-p", "senp-effect-guest-fixture", "--target", "wasm32-unknown-unknown"],
            ROOT / "rust/senp", 600)
        run("build-sample", cargo + ["-p", "sakura-senp-sample", "--target", "wasm32-unknown-unknown"],
            ROOT / "rust/senp", 600)
        run("build-github-pull-requests", cargo + ["-p", "sakura-github-pull-requests", "--target", "wasm32-unknown-unknown"],
            ROOT / "rust/senp", 600)
        run("build-github-actions", cargo + ["-p", "sakura-github-actions", "--target", "wasm32-unknown-unknown"],
            ROOT / "rust/senp", 600)
        for name in ("sakura-senp-host.exe", "senp-host-fixture.exe"):
            shutil.copy2(target / "debug" / name, output / name)
        run("componentize", [str(target / "debug/sakura-senp-tool.exe"), "componentize",
            str(target / "wasm32-unknown-unknown/debug/senp_effect_guest_fixture.wasm"), str(output / "extension.wasm")], ROOT, 30)
        (output / "extension.sha256").write_text(hashlib.sha256((output / "extension.wasm").read_bytes()).hexdigest() + "\n", encoding="ascii")
        run("componentize-sample", [str(target / "debug/sakura-senp-tool.exe"), "componentize",
            str(target / "wasm32-unknown-unknown/debug/sakura_senp_sample.wasm"), str(output / "sample-extension.wasm")], ROOT, 30)
        (output / "sample-extension.sha256").write_text(
            hashlib.sha256((output / "sample-extension.wasm").read_bytes()).hexdigest() + "\n", encoding="ascii")
        run("componentize-github-pull-requests", [str(target / "debug/sakura-senp-tool.exe"), "componentize",
            str(target / "wasm32-unknown-unknown/debug/sakura_github_pull_requests.wasm"),
            str(output / "github-pull-requests-extension.wasm")], ROOT, 30)
        (output / "github-pull-requests-extension.sha256").write_text(
            hashlib.sha256((output / "github-pull-requests-extension.wasm").read_bytes()).hexdigest() + "\n", encoding="ascii")
        run("componentize-github-actions", [str(target / "debug/sakura-senp-tool.exe"), "componentize",
            str(target / "wasm32-unknown-unknown/debug/sakura_github_actions.wasm"),
            str(output / "github-actions-extension.wasm")], ROOT, 30)
        (output / "github-actions-extension.sha256").write_text(
            hashlib.sha256((output / "github-actions-extension.wasm").read_bytes()).hexdigest() + "\n", encoding="ascii")
        for scenario in ("echo", "blocked-read", "blocked-write", "partial", "oversized", "crash", "memory-limit"):
            (output / f"{scenario}.wasm").write_bytes(b"native pipe test scenario; not a Wasm component")
        if args.prepare_only:
            status = "prepared"
        else:
            env = dict(os.environ, SAKURA_SENP_RUNTIME_FIXTURES=str(output))
            rust_peer, cpp_peer = output / "protocol-rust.jsonl", output / "protocol-cpp.jsonl"
            env.pop("SENP_PROTOCOL_PEER_FILE", None)
            env["SENP_PROTOCOL_OUTPUT"] = str(rust_peer)
            cargo_test = ["cargo", "test", "--locked", "--target-dir", str(target), "-p", "sakura-senp-host"]
            if args.offline:
                cargo_test.append("--offline")
            run("rust-codec", cargo_test + ["--test", "effect_protocol"], ROOT / "rust/senp", 600, env)
            env["SENP_PROTOCOL_PEER_FILE"] = str(rust_peer)
            env["SENP_PROTOCOL_OUTPUT"] = str(cpp_peer)
            xml = output / "native-results.xml"
            run("native-tests", [str(tests), "--gtest_filter=SenpRuntimeLifecycle.*:SenpRuntimeProcess.*:SenpEffectProtocol.*:SenpViewLifecycle.*:SenpOwnerComposition.*",
                f"--gtest_output=xml:{xml}"], ROOT, 120, env)
            report = ET.parse(xml).getroot()
            cases = report.findall(".//testcase")
            process_cases = [case for case in cases if case.get("classname") == "SenpRuntimeProcess"]
            owner_cases = [case for case in cases if case.get("classname") == "SenpViewLifecycle"]
            composition_cases = {case.get("name") for case in cases
                                 if case.get("classname") == "SenpOwnerComposition"}
            # Named, not counted: the hermetic cases in this suite grow, and a
            # count that drifts turns the real-runtime gate into a pass for a
            # run that never activated a component.
            required_compositions = {
                "RealSamplePublishesTwoTreesAndStructuredDocument",
                "RealGithubIssuesAndPullRequestsReachNativeProviders",
                "RealGithubActionsReachNativeProviders",
            }
            missing = sorted(required_compositions - composition_cases)
            if len(process_cases) != 7 or len(owner_cases) < 15 or missing \
                    or any(case.find("skipped") is not None or case.find("failure") is not None for case in cases):
                raise RuntimeError(
                    "required process cases are missing, skipped or failed"
                    + (f"; absent real-runtime cases: {missing}" if missing else ""))
            env["SENP_PROTOCOL_PEER_FILE"] = str(cpp_peer)
            env["SENP_PROTOCOL_OUTPUT"] = str(rust_peer)
            v1 = ROOT / "build/x64/Debug/rust/senp/stage/sakura-indent-rainbow/module/extension.wasm"
            if not v1.is_file():
                raise RuntimeError("the Debug v1 component is missing; rebuild the Debug solution")
            env["SAKURA_SENP_TEST_COMPONENT"] = str(v1)
            run("rust-host", cargo_test + ["--", "--include-ignored"], ROOT / "rust/senp", 600, env)
            if rust_peer.read_bytes() != cpp_peer.read_bytes():
                raise RuntimeError("C++ and Rust canonical fixture encodings disagree")
            status = "pass"
    finally:
        survivors = matching_processes(executables)
        if survivors:
            # The initial snapshot proved these exact executable identities
            # were absent. Kill roots first; the test-owned job closes children.
            ids = {item["ProcessId"] for item in survivors}
            for item in sorted(survivors, key=lambda value: value["ParentProcessId"] in ids):
                subprocess.run(["taskkill.exe", "/PID", str(item["ProcessId"]), "/T", "/F"],
                               capture_output=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
            survivors = matching_processes(executables)
            status = "failed"
        receipt = {"status": status, "steps": steps, "survivors": survivors}
        (output / "evidence.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt), flush=True)
    return 0 if status in ("pass", "prepared") else 1


if __name__ == "__main__":
    sys.exit(main())
