#!/usr/bin/env python3
"""Bounded, hash-pinned TLC checks, including intentional counterexamples (#290)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

TOOL_SHA256 = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
# Official v1.7.4 asset (2024-08-05), verified against the installed TLC 2.19.
TOOL_URL = "https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"
CASES = ("SearchRequestLifecycle", "SearchRequestLifecycle_NoEmptyInvalidation",
         "SearchRequestLifecycle_NoGenerationCheck")


def accepted_result(returncode: int, output: str, negative: bool) -> bool:
    if negative:
        return returncode == 12 and "Error: Invariant CurrentResults is violated." in output
    return (returncode == 0 and "Model checking completed. No error has been found." in output
            and "0 states left on queue." in output)


def main() -> int:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--jar", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    jar = args.jar.resolve()
    digest = hashlib.sha256(jar.read_bytes()).hexdigest()
    if digest != TOOL_SHA256:
        raise SystemExit("TLC_TOOL_HASH_MISMATCH")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    models = root / "docs/formal"
    source_sha = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True, timeout=10).strip()
    evidence = {"source_commit": source_sha, "tool_sha256": digest, "cases": []}
    success = True
    for index, case in enumerate(CASES):
        config = models / f"{case}.cfg"
        model = models / "SearchRequestLifecycle.tla"
        command = ["java", "-Xmx512m", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC",
                   "-workers", "2", "-metadir", str(output / case),
                   "-config", config.name, model.name]
        try:
            result = subprocess.run(command, cwd=models, text=True, encoding="utf-8",
                                    errors="replace", stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=60, check=False)
            code, log = result.returncode, result.stdout
            passed = accepted_result(code, log, index != 0)
        except (OSError, subprocess.TimeoutExpired) as error:
            # subprocess.run kills and waits for its direct Java child on timeout.
            code, log, passed = None, type(error).__name__, False
        (output / f"{case}.log").write_text(log, encoding="utf-8")
        evidence["cases"].append({"name": case, "exit_code": code, "passed": passed,
                                  "model_sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                                  "config_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
                                  "command": command})
        success = success and passed
        print(f"{case}: {'PASS' if passed else 'FAIL'} (exit={code})")
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
