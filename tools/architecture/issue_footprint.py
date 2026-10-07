#!/usr/bin/env python3
"""Measure the context footprint an agent needed to resolve one Issue.

For a tracking Issue number N the tool resolves the Issue's commits through
issue_commits.resolve_issue_commits (GitHub PR association first, message grep
restricted to post-fork history second), then reports, measured at the last of
those commits (so the numbers do not drift as HEAD moves):

    commits                  number of commits referencing the Issue
    changed_files            distinct paths changed by those commits
    changed_subdirs          distinct owning directories (two path levels,
                             three under sakura_core/workbench, terminal,
                             platform, senp)
    build_definition_files   changed .vcxproj / .filters / CMake / modules.json
    read_footprint_lines     line count, at the last commit, of every changed
                             .cpp/.h/.hpp/.inl/.rs/.py/.md/.tla/.cfg/.ps1 file
                             (generated projections and externals excluded)
    claude_md_bytes          bytes of every CLAUDE.md on the ancestor path of a
                             changed file (root CLAUDE.md included), at the
                             last commit
    test_files               changed files under src/test/ or tools/build/pilots/

Commands
    measure --issue N [--json]
    table --issues N,M,... [--markdown]

Only git and the standard library are used. Nothing is written.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import issue_commits as ic

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent
BUILD_DEF_RE = re.compile(r"(\.vcxproj|\.vcxproj\.filters|\.cmake|CMakeLists\.txt|src/main/modules/modules\.json)$")
READ_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".rs", ".py", ".md", ".tla", ".cfg", ".ps1")
READ_EXCLUDE_PREFIXES = ("src/main/modules/generated/", "externals/")
DEEP_PREFIXES = ("sakura_core/workbench/", "sakura_core/terminal/", "sakura_core/platform/", "sakura_core/senp/", "src/test/cpp/tests1/")


def git(*args: str) -> str:
    proc = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout


def issue_commits(issue: int, rev: str) -> list:
    """Commits associated with the Issue.

    Delegates to issue_commits.resolve_issue_commits, which prefers GitHub PR
    association over message grep and drops pre-fork upstream matches, instead
    of the old bare `git log --grep '#N'` that also matched 2012-2018 upstream
    patch numbers.
    """
    resolved = ic.resolve_issue_commits(issue, rev)
    return [c["sha"] for c in resolved["commits"]]


def changed_paths(commit: str) -> list:
    out = git("show", "--pretty=format:", "--name-only", "--no-renames", commit)
    return [line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()]


def owning_dir(path: str) -> str:
    parts = path.split("/")
    depth = 3 if path.startswith(DEEP_PREFIXES) else 2
    if len(parts) <= depth:
        return "/".join(parts[:-1]) or "."
    return "/".join(parts[:depth])


def blob_lines(commit: str, path: str):
    proc = subprocess.run(["git", "show", f"{commit}:{path}"], cwd=REPO_ROOT, capture_output=True)
    if proc.returncode != 0:
        return None
    data = proc.stdout
    if not data:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def blob_size(commit: str, path: str):
    proc = subprocess.run(["git", "cat-file", "-s", f"{commit}:{path}"], cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    return int(proc.stdout.strip())


def measure(issue: int, rev: str = "HEAD") -> dict:
    resolved = ic.resolve_issue_commits(issue, rev)
    commits = [c["sha"] for c in resolved["commits"]]
    association = resolved["association"]
    contamination = ic.contamination_short(resolved["contamination"])
    if not commits:
        return {"issue": issue, "commits": 0, "error": "no commits reference this issue",
                "association": association, "contamination": contamination}
    last = commits[0]  # git log lists newest first
    files = set()
    for commit in commits:
        files.update(changed_paths(commit))
    files = sorted(files)
    read_lines = 0
    read_files = 0
    for path in files:
        if path.endswith(READ_SUFFIXES) and not path.startswith(READ_EXCLUDE_PREFIXES):
            lines = blob_lines(last, path)
            if lines is not None:
                read_lines += lines
                read_files += 1
    claude_paths = set()
    for path in files:
        parts = path.split("/")
        for depth in range(0, len(parts)):
            candidate = "/".join(parts[:depth] + ["CLAUDE.md"])
            claude_paths.add(candidate)
    claude_bytes = 0
    claude_found = []
    for candidate in sorted(claude_paths):
        size = blob_size(last, candidate)
        if size is not None:
            claude_bytes += size
            claude_found.append(candidate)
    return {
        "issue": issue,
        "commits": len(commits),
        "last_commit": last[:9],
        "first_commit": commits[-1][:9],
        "changed_files": len(files),
        "changed_subdirs": len({owning_dir(p) for p in files}),
        "build_definition_files": sum(1 for p in files if BUILD_DEF_RE.search(p)),
        "read_footprint_lines": read_lines,
        "read_footprint_files": read_files,
        "claude_md_bytes": claude_bytes,
        "claude_md_files": len(claude_found),
        "test_files": sum(1 for p in files if p.startswith(("src/test/", "tools/build/pilots/"))),
        "contamination": contamination,
        "association": association,
    }


def cmd_measure(args: argparse.Namespace) -> int:
    result = measure(args.issue, args.rev)
    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
    else:
        for key, value in result.items():
            print(f"{key}: {value}")
    return 0 if "error" not in result else 1


def cmd_table(args: argparse.Namespace) -> int:
    issues = [int(x) for x in args.issues.split(",") if x.strip()]
    rows = [measure(n, args.rev) for n in issues]
    if args.json:
        json.dump(rows, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0
    print("| Issue | コミット数 | 変更ファイル | 変更サブディレクトリ | ビルド定義ファイル | 読解行数 | CLAUDE.md バイト | テストファイル | contamination | association |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---|---|")
    for row in rows:
        if "error" in row:
            print(f"| #{row['issue']} | 0 | - | - | - | - | - | - | "
                  f"{row.get('contamination', '-')} | {row.get('association', '-')} |")
            continue
        print(
            f"| #{row['issue']} | {row['commits']} | {row['changed_files']} | {row['changed_subdirs']} | "
            f"{row['build_definition_files']} | {row['read_footprint_lines']:,} | {row['claude_md_bytes']:,} | {row['test_files']} | "
            f"{row['contamination']} | {row['association']} |"
        )
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rev", default="HEAD", help="branch/revision whose history is scanned (default HEAD)")
    sub = parser.add_subparsers(dest="command", required=True)
    p_measure = sub.add_parser("measure")
    p_measure.add_argument("--issue", type=int, required=True)
    p_measure.add_argument("--json", action="store_true")
    p_measure.set_defaults(func=cmd_measure)
    p_table = sub.add_parser("table")
    p_table.add_argument("--issues", required=True, help="comma separated issue numbers")
    p_table.add_argument("--json", action="store_true")
    p_table.set_defaults(func=cmd_table)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
