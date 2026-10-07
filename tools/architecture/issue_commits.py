#!/usr/bin/env python3
"""Resolve which commits actually belong to a tracking Issue.

Why this module exists
----------------------
``git log --grep '#N'`` is not a safe Issue->commit association in this
repository. The history starts at the 2012 upstream sakura-editor project, so
"#291" matches an upstream 2012 patch number as readily as this fork's Issue
291, and a single fork commit often references several Issues at once.

Resolution sources, in priority order (the winning source is recorded per
commit in ``CommitRef.source``):

1. ``manual``   - tools/architecture/issue_commits.json, an optional override
                  file of the shape {"289": {"include": [...], "exclude": [...]}}.
2. ``pr``       - GitHub association through ``gh``: the Issue timeline's
                  ``committed`` events (commits of the linked pull request),
                  its ``referenced``/``merged`` commit ids, and the merge
                  commits of merged PRs found by ``gh pr list --search N``.
                  Results are cached under tools/architecture/.cache/.
3. ``grep``     - ``git log <rev> --grep '(^|[^0-9])#N([^0-9]|$)'`` restricted
                  to commits at or after the fork history cutoff.

When ``gh`` is unavailable or fails, the resolver degrades gracefully and the
association is reported as ``grep_only``.

Fork history cutoff
-------------------
The git history of this checkout begins in 2012 (upstream) and upstream
authors keep appearing through 2026, so "first commit dated >= 2026-01-01" is
*not* the fork point. The cutoff is therefore the first commit authored by
``tsuyoshi-otake``, which is where this fork's own history begins. It is
computed at run time and reported as ``history_cutoff``.

Contamination
-------------
``multi_issue``     commit message references two or more distinct "#N"
``unrelated_paths`` more than half of the commit's paths are owned by a
                    component other than the Issue's majority component
``upstream_match``  grep matched a commit older than the fork cutoff; such
                    commits are excluded from the set but counted here
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from component_ownership import load_ownership

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent
CACHE_DIR = TOOL_DIR / ".cache"
MAPPING_FILE = TOOL_DIR / "issue_commits.json"
FORK_AUTHOR = "tsuyoshi-otake"
GH_REPO = "tsuyoshi-otake/sakura-editor-next"
ISSUE_REF_RE = re.compile(r"(?<![0-9A-Za-z_])#([0-9]{1,5})(?![0-9])")


def git(*args: str, check: bool = True) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace",
    )
    if proc.returncode != 0:
        if check:
            raise RuntimeError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
        return ""
    return proc.stdout


# ---------------------------------------------------------------- cutoff
_CUTOFF_CACHE: dict | None = None


def history_cutoff(rev: str = "HEAD") -> dict:
    """First commit authored by the fork owner: the start of fork history."""
    global _CUTOFF_CACHE
    if _CUTOFF_CACHE is not None:
        return _CUTOFF_CACHE
    out = git("log", rev, "--reverse", "--format=%H|%ad", "--date=short",
              f"--author={FORK_AUTHOR}", check=False)
    line = next((l for l in out.splitlines() if l.strip()), "")
    if not line:
        _CUTOFF_CACHE = {"sha": None, "date": None, "rule": "fork author not found"}
        return _CUTOFF_CACHE
    sha, date = line.split("|", 1)
    _CUTOFF_CACHE = {
        "sha": sha,
        "short": sha[:9],
        "date": date,
        "rule": f"first commit authored by {FORK_AUTHOR}",
    }
    return _CUTOFF_CACHE


# ------------------------------------------------------------------ gh
def _cache_path(name: str) -> Path:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    return CACHE_DIR / name


def _gh_json(args: list[str], cache_name: str, timeout: int = 45):
    path = _cache_path(cache_name)
    if path.exists() and os.environ.get("IRV_NO_CACHE") != "1":
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            pass
    try:
        proc = subprocess.run(["gh", *args], cwd=REPO_ROOT, capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None
    try:
        path.write_text(json.dumps(data), encoding="utf-8")
    except OSError:
        pass
    return data


def gh_commits(issue: int) -> tuple[list[str], bool]:
    """Commit SHAs associated with the Issue through GitHub. (shas, gh_ok)"""
    timeline = _gh_json(
        ["api", f"repos/{GH_REPO}/issues/{issue}/timeline?per_page=100", "--paginate"],
        f"timeline-{issue}.json",
    )
    shas: list[str] = []
    gh_ok = timeline is not None
    if isinstance(timeline, list):
        for ev in timeline:
            name = ev.get("event")
            if name is None and ev.get("sha"):      # raw commit entry
                shas.append(ev["sha"])
            elif name == "committed" and ev.get("sha"):
                shas.append(ev["sha"])
            elif name in ("referenced", "merged", "closed") and ev.get("commit_id"):
                shas.append(ev["commit_id"])
    prs = _gh_json(
        ["pr", "list", "--repo", GH_REPO, "--state", "merged", "--search", str(issue),
         "--json", "number,mergeCommit,title,body", "--limit", "20"],
        f"prlist-{issue}.json",
    )
    if isinstance(prs, list):
        gh_ok = True
        for pr in prs:
            mc = (pr.get("mergeCommit") or {}).get("oid")
            text = f"{pr.get('title', '')}\n{pr.get('body', '')}"
            if mc and (str(issue) in ISSUE_REF_RE.findall(text) or pr.get("number") == issue):
                shas.append(mc)
    # keep only SHAs that exist in this checkout
    resolved = []
    for sha in shas:
        out = git("rev-parse", "--verify", "--quiet", f"{sha}^{{commit}}", check=False)
        if out.strip():
            resolved.append(out.strip())
    return resolved, gh_ok


# ---------------------------------------------------------------- grep
def grep_commits(issue: int, rev: str) -> list[str]:
    pattern = rf"(^|[^0-9A-Za-z_])#{issue}([^0-9]|$)"
    out = git("log", rev, "--format=%H", "-E", f"--grep={pattern}", check=False)
    return [l.strip() for l in out.splitlines() if l.strip()]


# ------------------------------------------------------------- metadata
_META: dict[str, dict] = {}


def commit_meta(sha: str) -> dict:
    if sha in _META:
        return _META[sha]
    out = git("show", "-s", "--format=%H%x01%ad%x01%an%x01%B", "--date=short", sha, check=False)
    if not out.strip():
        meta = {"sha": sha, "date": "", "author": "", "message": "", "subject": ""}
    else:
        full, date, author, body = out.split("\x01", 3)
        meta = {
            "sha": full.strip(), "date": date, "author": author,
            "message": body, "subject": body.strip().splitlines()[0] if body.strip() else "",
        }
    _META[sha] = meta
    return meta


def commit_paths(sha: str) -> list[str]:
    out = git("show", "--pretty=format:", "--name-only", "--no-renames", sha, check=False)
    return [l.strip().replace("\\", "/") for l in out.splitlines() if l.strip()]


# ------------------------------------------------------------- resolver
def load_mapping() -> dict:
    if not MAPPING_FILE.exists():
        return {}
    try:
        data = json.loads(MAPPING_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return {k: v for k, v in data.items() if not k.startswith("_")}


def resolve_issue_commits(issue: int, rev: str = "HEAD", use_gh: bool = True) -> dict:
    """Return the association result for one Issue.

    {issue, commits: [{sha, short, date, subject, source}], association,
     contamination: [...], history_cutoff: {...}, sources: {...}}
    """
    cutoff = history_cutoff(rev)
    cutoff_date = cutoff.get("date") or "0000-00-00"

    mapping = load_mapping().get(str(issue), {})
    manual_include = [s for s in mapping.get("include", [])]
    manual_exclude = {s.lower() for s in mapping.get("exclude", [])}

    source_of: dict[str, str] = {}

    for sha in manual_include:
        full = git("rev-parse", "--verify", "--quiet", f"{sha}^{{commit}}", check=False).strip()
        if full:
            source_of[full] = "manual"

    gh_ok = False
    if use_gh:
        gh_shas, gh_ok = gh_commits(issue)
        for sha in gh_shas:
            source_of.setdefault(sha, "pr")

    grep_shas = grep_commits(issue, rev)
    upstream = []
    for sha in grep_shas:
        meta = commit_meta(sha)
        if meta["date"] and meta["date"] < cutoff_date:
            upstream.append(sha)
            continue
        source_of.setdefault(sha, "grep")

    for sha in list(source_of):
        if sha[:9].lower() in manual_exclude or sha.lower() in manual_exclude:
            del source_of[sha]

    # order newest first
    ordered = sorted(source_of, key=lambda s: (commit_meta(s)["date"], s), reverse=True)
    commits = []
    for sha in ordered:
        meta = commit_meta(sha)
        commits.append({
            "sha": meta["sha"], "short": meta["sha"][:9], "date": meta["date"],
            "subject": meta["subject"], "source": source_of[sha],
        })

    used = {c["source"] for c in commits}
    if not commits:
        association = "none"
    elif "manual" in used:
        association = "manual" + ("+pr" if "pr" in used else "") + ("+grep" if "grep" in used else "")
    elif not gh_ok:
        association = "grep_only"
    elif used == {"pr"}:
        association = "pr"
    elif used == {"grep"}:
        association = "pr_empty+grep"
    else:
        association = "pr+grep"

    contamination = _contamination(issue, commits, upstream)
    return {
        "issue": issue,
        "commits": commits,
        "association": association,
        "gh_available": gh_ok,
        "contamination": contamination,
        "history_cutoff": cutoff,
    }


def _contamination(issue: int, commits: list[dict], upstream: list[str]) -> list[dict]:
    own = load_ownership()
    out: list[dict] = []

    multi = []
    for c in commits:
        refs = {int(x) for x in ISSUE_REF_RE.findall(commit_meta(c["sha"])["message"])}
        if len(refs) >= 2:
            multi.append(c["short"])
    if multi:
        out.append({"kind": "multi_issue", "count": len(multi), "shas": multi})

    # majority component across the whole set
    tally: dict[str, int] = {}
    per_commit: dict[str, list[str]] = {}
    for c in commits:
        owners = [own.owner_of(p) or "(unowned)" for p in commit_paths(c["sha"])]
        per_commit[c["short"]] = owners
        for o in owners:
            tally[o] = tally.get(o, 0) + 1
    majority = max(tally, key=lambda k: tally[k]) if tally else None
    unrelated = []
    if majority:
        for short, owners in per_commit.items():
            if not owners:
                continue
            foreign = sum(1 for o in owners if o != majority)
            if foreign > len(owners) * 0.5:
                unrelated.append(short)
    if unrelated:
        out.append({"kind": "unrelated_paths", "count": len(unrelated),
                    "majority_component": majority, "shas": unrelated})

    if upstream:
        out.append({"kind": "upstream_match", "count": len(upstream),
                    "shas": [s[:9] for s in upstream],
                    "note": "matched by grep but older than the fork cutoff; excluded"})
    return out


def contamination_short(contamination: list[dict]) -> str:
    if not contamination:
        return "-"
    abbrev = {"multi_issue": "multi", "unrelated_paths": "unrelated", "upstream_match": "upstream"}
    return ",".join(f"{abbrev[c['kind']]}:{c['count']}" for c in contamination)


# ----------------------------------------------------------------- CLI
def cmd_resolve(args) -> int:
    result = resolve_issue_commits(args.issue, args.rev, use_gh=not args.no_gh)
    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0
    print(f"issue: #{result['issue']}")
    print(f"association: {result['association']} (gh_available={result['gh_available']})")
    cut = result["history_cutoff"]
    print(f"history_cutoff: {cut.get('short')} {cut.get('date')} ({cut.get('rule')})")
    print(f"commits: {len(result['commits'])}")
    for c in result["commits"]:
        print(f"  {c['short']} {c['date']} [{c['source']}] {c['subject'][:70]}")
    print(f"contamination: {contamination_short(result['contamination'])}")
    for c in result["contamination"]:
        print(f"  {c['kind']}: {', '.join(c['shas'])}")
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--no-gh", action="store_true", help="skip GitHub association")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("resolve")
    p.add_argument("--issue", type=int, required=True)
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_resolve)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.path.insert(0, str(TOOL_DIR))
    sys.exit(main())
