#!/usr/bin/env python3
"""Bounded positive/liveness and intentional-counterexample TLC gates (#296)."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

TOOL_SHA256 = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
# Existing official v1.7.4 (2024-08-05); no new dependency adoption.
TOOL_URL = "https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"


@dataclass(frozen=True)
class Case:
    group: str
    model: str
    config: str
    invariant: str | None = None
    temporal: str | None = None


CASES = (
    Case("connection", "SenpGhConnection", "SenpGhConnection"),
    Case("connection", "SenpGhConnection", "SenpGhConnection_NoIdentity", "IdentityBeforeGrant"),
    Case("connection", "SenpGhConnection", "SenpGhConnection_NoGeneration", "NoStaleConnection"),
    Case("owner", "SenpContributionOwner", "SenpContributionOwner"),
    Case("owner", "SenpContributionOwner", "SenpContributionOwner_NoGeneration", "CurrentView"),
    Case("owner", "SenpContributionOwner", "SenpContributionOwner_NoClear", "NoRevokedView"),
    Case("requests", "SenpGhRequests", "SenpGhRequests"),
    Case("requests", "SenpGhRequests", "SenpGhRequests_NoSingleFlight", "SingleFlight"),
    Case("requests", "SenpGhRequests", "SenpGhRequests_NoLastSubscriber", "SharedLeasePreserved"),
    Case("requests", "SenpGhRequests", "SenpGhRequests_NoCooldown", "RespectRateWindow"),
    Case("requests", "SenpGhRequests", "SenpGhRequests_NoReap", "TerminalOwnsNoProcess"),
    Case("requests", "SenpGhRequests", "SenpGhRequests_NoCleanupFairness",
         temporal="EveryAcceptedTerminates"),
)


def exploration(output: str) -> dict[str, int]:
    found = re.search(r"([\d,]+) states generated, ([\d,]+) distinct states found, "
                      r"([\d,]+) states left on queue\.", output)
    depth = re.search(r"depth of the complete state graph search is (\d+)", output)
    result = {}
    if found:
        result.update(zip(("generated", "distinct", "queued"),
                          (int(v.replace(",", "")) for v in found.groups())))
    if depth:
        result["depth"] = int(depth[1])
    return result


def sole_temporal_property(config: str, expected: str) -> bool:
    # TLC 2.19 does not print a violated temporal property's name. Only accept
    # its generic failure when the config checks exactly the expected property.
    return re.findall(r"^PROPERT(?:Y|IES)\b[^\n]*", config, re.MULTILINE) == [f"PROPERTIES {expected}"]


def accepted_result(code: int | None, output: str, invariant: str | None,
                    temporal: str | None = None) -> bool:
    if temporal is not None:
        return (code == 13 and "Error: Temporal properties were violated." in output
                and "The following behavior constitutes a counter-example:" in output
                and re.search(r"State 1:", output) is not None
                and re.search(r"Stuttering|Back to state", output) is not None)
    if invariant is not None:
        return (code == 12 and f"Error: Invariant {invariant} is violated." in output
                and "The behavior up to this point is:" in output
                and re.search(r"State 1:", output) is not None)
    states = exploration(output)
    return (code == 0 and "Model checking completed. No error has been found." in output
            and "Finished checking temporal properties" in output
            and states.get("queued") == 0 and states.get("distinct", 0) > 0
            and states.get("depth", 0) > 0)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_case(command: list[str], cwd: Path, timeout: float) -> tuple[int | None, str, dict]:
    """Only a direct Java child is launched; no shell or child-spawning wrapper."""
    started = time.monotonic()
    process = None
    timed_out = False
    try:
        process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   encoding="utf-8", errors="replace")
        try:
            log, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            log, _ = process.communicate(timeout=10)
        return (None if timed_out else process.returncode), log, {
            "pid": process.pid, "exited": process.poll() is not None,
            "timed_out": timed_out, "elapsed_seconds": round(time.monotonic() - started, 3)}
    except OSError as error:
        return None, f"TLC_LAUNCH_FAILED: {error}", {"exited": process is None}
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)


def main() -> int:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--jar", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--model", choices=sorted({case.group for case in CASES}))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    jar = args.jar.resolve()
    if not jar.is_file() or sha256(jar) != TOOL_SHA256:
        raise SystemExit("TLC_TOOL_MISSING_OR_HASH_MISMATCH")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    run_dir = output / str(uuid.uuid4())
    run_dir.mkdir()
    evidence = {
        "source_commit": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                                 text=True, timeout=10).strip(),
        "runner_sha256": sha256(Path(__file__)), "tool_sha256": TOOL_SHA256,
        "tool_url": TOOL_URL, "run_directory": str(run_dir), "cases": [],
    }
    for case in CASES:
        if args.model is not None and case.group != args.model:
            continue
        case_dir = run_dir / case.config
        case_dir.mkdir()
        source_hashes = {}
        for filename in (f"{case.model}.tla", f"{case.config}.cfg"):
            source = root / "docs/formal" / filename
            data = source.read_bytes()
            (case_dir / filename).write_bytes(data)
            source_hashes[filename] = hashlib.sha256(data).hexdigest()
        command = ["java", "-Xmx512m", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC",
                   "-workers", "2", "-metadir", str(case_dir / "states"),
                   "-config", f"{case.config}.cfg", f"{case.model}.tla"]
        config = (case_dir / f"{case.config}.cfg").read_text(encoding="utf-8")
        if case.temporal is not None and not sole_temporal_property(config, case.temporal):
            code, log, lifecycle = None, "TLC_TEMPORAL_CONFIG_MISMATCH", {"exited": True}
        else:
            code, log, lifecycle = run_case(command, case_dir, 60)
        log_path = case_dir / "tlc.log"
        log_path.write_text(log, encoding="utf-8")
        unchanged = all(sha256(root / "docs/formal" / filename) == digest
                        for filename, digest in source_hashes.items())
        passed = (accepted_result(code, log, case.invariant, case.temporal)
                  and lifecycle["exited"] and unchanged)
        evidence["cases"].append({
            "name": case.config, "expected_invariant": case.invariant, "passed": passed,
            "expected_temporal": case.temporal,
            "exit_code": code, "source_hashes": source_hashes, "sources_unchanged": unchanged,
            "command": command, "log": str(log_path), "log_sha256": sha256(log_path),
            "exploration": exploration(log), **lifecycle,
        })
        print(f"{case.config}: {'PASS' if passed else 'FAIL'} (exit={code}, "
              f"states={exploration(log).get('distinct', '?')})", flush=True)
        # Retain completed evidence even if the following case cannot start.
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return 0 if evidence["cases"] and all(case["passed"] for case in evidence["cases"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
