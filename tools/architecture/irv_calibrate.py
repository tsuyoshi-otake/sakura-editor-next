#!/usr/bin/env python3
"""Calibrate the static IRV estimate against what agents actually read.

irv.py predicts a required-read set from git history and modules.json. This
tool measures that prediction against the ground truth left behind by real
Claude Code sessions: the JSONL transcripts under

    %USERPROFILE%/.claude/projects/C--Codes-tsuyoshi-otake-sakura-editor-next*/

Each transcript line is one JSON record. Records whose ``message.content[]``
carries ``type == "tool_use"`` tell us what the agent touched:

    Read / Edit / Write   input.file_path        -> a file the agent opened
    Bash                  input.command          -> path-like tokens; agents in
                                                    this repository read a lot
                                                    of files through sed/cat/
                                                    grep rather than Read
    Grep / Glob           input.path / results   -> candidate files, counted
                                                    separately (a grep hit is
                                                    not proof of a read)

The actual-read set is Read + Edit + Write + Bash-derived paths, filtered to
paths that exist in the repository tree. ``--include-grep-results`` adds the
paths listed in Grep/Glob tool results.

Issue scoping
-------------
A single transcript in this repository can be 76 MB and cover a dozen Issues.
Attributing every file such a session ever opened to one Issue makes the recall
denominator meaningless. So the scan is *windowed*: a user message that
references "#N" opens a window for Issue N, and the next user message that
references a different Issue closes it. Only tool calls inside an open window
count as that Issue's reads. ``--whole-session`` restores the unscoped view.
When a session mentions the Issue but no *user* turn ever names it (the Issue
came up only in assistant text or tool output), no window can be opened. Such a
transcript is not attributable and is dropped, because counting all of a 76 MB
multi-Issue session as one Issue's reads is what made the first calibration
meaningless. ``--allow-fallback`` keeps it, scanned whole and flagged.

Commands
    sessions --issue N
    compare  --issue N [--session FILE] [--include-grep-results] [--whole-session] [--json]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import irv

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent
PROJECT_GLOB = "C--Codes-tsuyoshi-otake-sakura-editor-next*"
SESSION_ROOT = Path(os.environ.get("CLAUDE_PROJECTS_DIR",
                                   Path.home() / ".claude" / "projects"))

WORKTREE_RE = re.compile(r"^.*?sakura-editor-next[/\\](?:\.claude[/\\]worktrees[/\\][^/\\]+[/\\])?",
                         re.IGNORECASE)
PATH_TOKEN_RE = re.compile(r"[A-Za-z0-9_.\-/\\]*[A-Za-z0-9_\-][/\\][A-Za-z0-9_.\-/\\]+")
READ_TOOLS = ("Read", "Edit", "Write", "NotebookEdit", "MultiEdit")
INTERESTING_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".rs", ".py", ".md", ".json",
                        ".tla", ".cfg", ".ps1", ".bat", ".vcxproj", ".filters",
                        ".txt", ".cmake", ".yml", ".yaml", ".rc")


# --------------------------------------------------------------- repo tree
_REPO_FILES: set[str] | None = None


def repo_files() -> set[str]:
    global _REPO_FILES
    if _REPO_FILES is None:
        out = subprocess.run(["git", "ls-files"], cwd=REPO_ROOT, capture_output=True,
                             text=True, encoding="utf-8", errors="replace").stdout
        _REPO_FILES = {l.strip().replace("\\", "/") for l in out.splitlines() if l.strip()}
    return _REPO_FILES


def normalise(path: str) -> str | None:
    """Map an absolute / worktree / relative path to a repo-relative path."""
    if not path:
        return None
    p = path.strip().strip('"').strip("'").replace("\\", "/")
    m = WORKTREE_RE.match(p.replace("/", "/"))
    if m:
        p = p[m.end():]
    p = p.lstrip("./")
    if p in repo_files():
        return p
    # last-resort: unique basename match for relative reads done after a cd
    return None


def current_project_dir_name() -> str:
    """Claude Code's project-directory encoding of the current checkout."""
    return re.sub(r"[^A-Za-z0-9]", "-", str(REPO_ROOT))


def session_files(include_current: bool = False) -> list[Path]:
    """Transcripts for this repository.

    The transcript of the session that is *running this tool* lives under the
    current checkout's own project directory and would otherwise be counted as
    evidence for whatever Issue this session happens to discuss, so it is
    excluded unless --include-current is given.
    """
    out: list[Path] = []
    if not SESSION_ROOT.exists():
        return out
    current = current_project_dir_name()
    for d in sorted(SESSION_ROOT.glob(PROJECT_GLOB)):
        if not include_current and d.name == current:
            continue
        out.extend(sorted(d.glob("*.jsonl")))
    return out


# ----------------------------------------------------------- transcript scan
def issue_patterns(issue: int) -> re.Pattern:
    n = issue
    return re.compile(rf"(?:#{n}(?![0-9])|\bIssue[ _-]?{n}(?![0-9])|\bissue[ _-]?{n}(?![0-9]))")


def _text_of(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for it in content:
            if isinstance(it, dict):
                if it.get("type") == "text":
                    parts.append(it.get("text") or "")
                elif it.get("type") == "tool_result":
                    pass
        return "\n".join(parts)
    return ""


ISSUE_REF_RE = re.compile(r"(?<![0-9A-Za-z_])#([0-9]{2,5})(?![0-9])")


def _user_text(rec: dict) -> str | None:
    """Text of a real user turn (a tool result is not a user turn)."""
    if rec.get("type") != "user":
        return None
    msg = rec.get("message") or {}
    content = msg.get("content")
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return None
    parts = [it.get("text") or "" for it in content
             if isinstance(it, dict) and it.get("type") == "text"]
    return "\n".join(parts) if parts else None


def scan_session(path: Path, issue: int | None, whole_session: bool = False,
                 allow_fallback: bool = False):
    info = _scan_once(path, issue, whole_session)
    info["fallback"] = False
    if info["windowed"] and info["window_turns"] == 0 and info["mentions"] > 0 and allow_fallback:
        info = _scan_once(path, issue, True)
        info["fallback"] = True
    return info


def _scan_once(path: Path, issue: int | None, whole_session: bool = False):
    """One streaming pass. Returns mention counts, tool counts and path sets."""
    pat = issue_patterns(issue) if issue is not None else None
    mentions = 0
    windowed = not (whole_session or issue is None)
    active = not windowed
    window_turns = 0
    first_ts = last_ts = None
    counts = {"Read": 0, "Edit": 0, "Write": 0, "Bash": 0, "Grep": 0, "Glob": 0}
    read_paths: set[str] = set()
    bash_paths: set[str] = set()
    grep_result_paths: set[str] = set()
    pending: dict[str, str] = {}          # tool_use_id -> tool name (Grep/Glob only)

    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line.strip():
                continue
            if pat is not None and pat.search(line):
                mentions += 1
            try:
                rec = json.loads(line)
            except (json.JSONDecodeError, ValueError):
                continue
            ts = rec.get("timestamp")
            if ts:
                first_ts = first_ts or ts
                last_ts = ts
            if windowed:
                utext = _user_text(rec)
                if utext is not None:
                    refs = {int(x) for x in ISSUE_REF_RE.findall(utext)}
                    if issue in refs or (pat is not None and pat.search(utext)):
                        active = True
                        window_turns += 1
                    elif refs:
                        active = False
            msg = rec.get("message") or {}
            content = msg.get("content")
            if not isinstance(content, list):
                continue
            if not active:
                continue
            for item in content:
                if not isinstance(item, dict):
                    continue
                kind = item.get("type")
                if kind == "tool_use":
                    name = item.get("name")
                    inp = item.get("input") or {}
                    if name in counts:
                        counts[name] += 1
                    if name in READ_TOOLS:
                        norm = normalise(inp.get("file_path") or inp.get("notebook_path") or "")
                        if norm:
                            read_paths.add(norm)
                    elif name == "Bash":
                        for tok in PATH_TOKEN_RE.findall(inp.get("command") or ""):
                            if not tok.endswith(INTERESTING_SUFFIXES):
                                continue
                            norm = normalise(tok)
                            if norm:
                                bash_paths.add(norm)
                    elif name in ("Grep", "Glob"):
                        pending[item.get("id") or ""] = name
                elif kind == "tool_result":
                    if item.get("tool_use_id") in pending:
                        body = item.get("content")
                        text = body if isinstance(body, str) else _join_result(body)
                        for tok in PATH_TOKEN_RE.findall(text or ""):
                            norm = normalise(tok)
                            if norm:
                                grep_result_paths.add(norm)
    return {
        "session": str(path), "mentions": mentions, "window_turns": window_turns,
        "windowed": windowed, "first_ts": first_ts,
        "last_ts": last_ts, "tool_counts": counts, "read_paths": read_paths,
        "bash_paths": bash_paths, "grep_result_paths": grep_result_paths,
    }


def _join_result(body) -> str:
    if isinstance(body, list):
        return "\n".join(it.get("text", "") for it in body
                         if isinstance(it, dict) and it.get("type") == "text")
    return ""


# ---------------------------------------------------------------- category
def category_of(path: str) -> str:
    base = path.rsplit("/", 1)[-1]
    if base == "CLAUDE.md":
        return "guide"
    if path.startswith(".claude/") or path.startswith("docs/") or base == "AGENTS.md":
        return "guide/doc"
    if path.startswith(("src/test/", "tools/build/pilots/")):
        return "test"
    if path.startswith("sakura_core/include/sakura/"):
        return "contract"
    if path.endswith((".vcxproj", ".filters", ".json", ".cmake")) or base == "CMakeLists.txt":
        return "manifest"
    if path.endswith((".cpp", ".h", ".hpp", ".inl", ".rs")):
        return "source"
    if path.startswith("tools/"):
        return "tooling"
    return "other"


# ----------------------------------------------------------------- commands
def cmd_sessions(args) -> int:
    rows = []
    files = session_files(args.include_current)
    for f in files:
        info = scan_session(f, args.issue, getattr(args, "whole_session", False))
        if info["mentions"] > 0:
            rows.append(info)
    rows.sort(key=lambda r: -r["mentions"])
    print(f"sessions mentioning issue #{args.issue}: {len(rows)} "
          f"(scanned {len(files)} transcripts)")
    for r in rows:
        c = r["tool_counts"]
        print(f"  {Path(r['session']).name}  mentions={r['mentions']:<5} "
              f"first={r['first_ts']}  read={c['Read']} bash={c['Bash']} "
              f"grep={c['Grep']} glob={c['Glob']} edit={c['Edit']} write={c['Write']}  "
              f"paths={len(r['read_paths'] | r['bash_paths'])}")
        print(f"      {r['session']}")
    return 0 if rows else 1


def compare_issue(args, issue: int) -> dict:
    """Compute the static-vs-actual comparison for one Issue.

    Returns a dict with either an ``error`` key or the full comparison
    (``result`` summary, ``missing`` list, ``infos``, ``dropped``, ``static_set``,
    ``reason_of``, ``actual``).
    """
    if args.session:
        infos = [scan_session(Path(args.session), issue, args.whole_session,
                              args.allow_fallback)]
    else:
        infos = [i for i in (scan_session(f, issue, args.whole_session,
                                          args.allow_fallback)
                             for f in session_files(args.include_current))
                 if i["mentions"] >= args.min_mentions]
    dropped = [Path(i["session"]).name for i in infos
               if i["windowed"] and i["window_turns"] == 0 and not i["fallback"]]
    infos = [i for i in infos
             if not (i["windowed"] and i["window_turns"] == 0 and not i["fallback"])]
    if not infos:
        return {"error": f"no attributable session for issue #{issue}"
                + (f" (dropped, no user turn named it: {', '.join(dropped)})" if dropped else "")}

    actual: set[str] = set()
    grep_results: set[str] = set()
    counts = {"Read": 0, "Edit": 0, "Write": 0, "Bash": 0, "Grep": 0, "Glob": 0}
    for i in infos:
        actual |= i["read_paths"] | i["bash_paths"]
        grep_results |= i["grep_result_paths"]
        for k, v in i["tool_counts"].items():
            counts[k] += v
    if args.include_grep_results:
        actual |= grep_results

    static = irv.compute_irv(issue, args.rev, use_gh=not args.no_gh)
    if "error" in static:
        return {"error": f"static IRV failed: {static['error']}"}
    static_set = {f["path"] for f in static["files"]}
    reason_of = {f["path"]: f["reason"] for f in static["files"]}

    inter = static_set & actual
    recall = len(inter) / len(actual) if actual else 0.0
    precision = len(inter) / len(static_set) if static_set else 0.0
    missing = sorted(actual - static_set)

    result = {
        "issue": issue,
        "sessions": [Path(i["session"]).name for i in infos],
        "tool_counts": counts,
        "static_files": len(static_set),
        "actual_read_files": len(actual),
        "grep_result_files": len(grep_results),
        "intersection": len(inter),
        "recall": round(recall, 3),
        "precision": round(precision, 3),
        "missing_total": len(missing),
    }
    return {"result": result, "missing": missing, "infos": infos, "dropped": dropped,
            "static_set": static_set, "reason_of": reason_of, "actual": actual,
            "grep_results": grep_results, "counts": counts}


def _missing_by_dir(missing: list[str]) -> list[tuple[str, list[str]]]:
    by_dir: dict[str, list[str]] = {}
    for p in missing:
        by_dir.setdefault(p.rsplit("/", 1)[0] if "/" in p else ".", []).append(p)
    return sorted(by_dir.items(), key=lambda kv: (-len(kv[1]), kv[0]))


def cmd_compare(args) -> int:
    issues = args.issue
    if args.table:
        # One markdown row per Issue; the columns are the calibration summary
        # that the planning document quotes. Session ids are shortened to the
        # first 8 characters so the row stays readable.
        header = ["Issue", "sessions", "window_turns", "tool_calls", "static_files",
                  "actual_read_files", "intersection", "recall", "precision",
                  "missing_total", "top_missing_dir"]
        print("| " + " | ".join(header) + " |")
        print("|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|")
        rc = 0
        for issue in issues:
            cmp = compare_issue(args, issue)
            if "error" in cmp:
                print(f"| #{issue} | (none) | 0 | - | - | - | - | - | - | - | {cmp['error']} |")
                rc = 1
                continue
            r = cmp["result"]
            sessions = ",".join(n[:8] for n in r["sessions"])
            turns = sum(i["window_turns"] for i in cmp["infos"])
            calls = "R" + str(cmp["counts"]["Read"]) + "/B" + str(cmp["counts"]["Bash"]) \
                + "/G" + str(cmp["counts"]["Grep"])
            ranked = _missing_by_dir(cmp["missing"])
            top = f"{ranked[0][0]}/ ({len(ranked[0][1])})" if ranked else "-"
            print(f"| #{issue} | {sessions} | {turns} | {calls} | {r['static_files']} | "
                  f"{r['actual_read_files']} | {r['intersection']} | {r['recall']:.3f} | "
                  f"{r['precision']:.3f} | {r['missing_total']} | {top} |")
        return rc

    if len(issues) != 1:
        print("compare without --table takes exactly one --issue")
        return 2
    cmp = compare_issue(args, issues[0])
    if "error" in cmp:
        print(cmp["error"])
        return 1
    result = cmp["result"]
    missing = cmp["missing"]
    infos = cmp["infos"]
    dropped = cmp["dropped"]
    static_set = cmp["static_set"]
    reason_of = cmp["reason_of"]
    actual = cmp["actual"]
    counts = cmp["counts"]
    if args.json:
        result["missing_top"] = missing[:20]
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0

    print(f"# calibration for Issue #{result['issue']}")
    print(f"sessions: {', '.join(result['sessions'])}")
    fb = [Path(i['session']).name for i in infos if i.get('fallback')]
    print(f"scoping: {'whole-session' if args.whole_session else 'issue-windowed'} "
          f"({sum(i['window_turns'] for i in infos)} user turns opened a window"
          + (f"; whole-session fallback for {', '.join(fb)}" if fb else "")
          + (f"; dropped unattributable {', '.join(dropped)}" if dropped else "") + ")")
    print(f"tool calls: " + ", ".join(f"{k}={v}" for k, v in counts.items()))
    print(f"static_files:      {result['static_files']}")
    print(f"actual_read_files: {result['actual_read_files']}"
          f"{' (incl. grep results)' if args.include_grep_results else ''}")
    print(f"grep_result_files: {result['grep_result_files']} (not counted unless --include-grep-results)")
    print(f"intersection:      {result['intersection']}")
    print(f"recall:            {result['recall']}   (|static & actual| / |actual|)")
    print(f"precision:         {result['precision']}   (|static & actual| / |static|)")
    print()
    # group the misses by directory
    ranked = _missing_by_dir(missing)
    print(f"top actual-read files missing from the static set ({len(missing)} total), "
          f"grouped by directory:")
    shown = 0
    for directory, paths in ranked:
        if shown >= 20:
            break
        print(f"  {directory}/  ({len(paths)} files)")
        for p in paths:
            if shown >= 20:
                break
            print(f"      [{category_of(p):<10}] {p}")
            shown += 1
    if not missing:
        print("  (none)")
    print()
    print("static files never touched (top 10 by reason):")
    unused = sorted(static_set - actual, key=lambda p: reason_of[p])
    for p in unused[:10]:
        print(f"  [{reason_of[p]:<18}] {p}")
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rev", default="HEAD")
    p.add_argument("--no-gh", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("sessions")
    s.add_argument("--issue", type=int, required=True)
    s.add_argument("--include-current", action="store_true",
                   help="also scan this session's own transcript")
    s.add_argument("--whole-session", action="store_true")
    s.set_defaults(func=cmd_sessions)

    c = sub.add_parser("compare")
    c.add_argument("--issue", type=int, required=True, action="append",
                   help="Issue number; repeatable with --table")
    c.add_argument("--table", action="store_true",
                   help="print one markdown row per --issue instead of the report")
    c.add_argument("--session", default=None)
    c.add_argument("--include-grep-results", action="store_true")
    c.add_argument("--min-mentions", type=int, default=2)
    c.add_argument("--include-current", action="store_true")
    c.add_argument("--allow-fallback", action="store_true",
                   help="keep sessions where no user turn named the Issue, scanned whole")
    c.add_argument("--whole-session", action="store_true",
                   help="count every file the session touched, not only the Issue windows")
    c.add_argument("--json", action="store_true")
    c.set_defaults(func=cmd_compare)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
