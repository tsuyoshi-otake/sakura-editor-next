#!/usr/bin/env python3
"""IRV - Issue Reading Volume.

IRV is the set of files an AI agent must READ to understand, investigate,
change and verify one Issue safely. It is deliberately *not* the set of files
the Issue changed: a change of twenty lines in one header can require reading
its component, its contract, its tests and every file that includes it.

Every file in the IRV set carries exactly one inclusion reason, taken from a
fixed vocabulary. When a file qualifies under several reasons it keeps the
first one in this precedence order:

    guide > manifest > formal > contract > test > owning_source > dependency_owner

Inclusion reasons
-----------------
guide
    CLAUDE.md files on the ancestor path of any file in the IRV set - not only
    of the changed files - plus the repository root CLAUDE.md, which is always
    included. These are the guides Claude Code auto-loads, so they are read
    whether the agent wants them or not. The closure over the whole IRV set
    (rather than over the changed files alone) is calibrated: sessions for
    #217 and #292 read sakura_core/workbench/CLAUDE.md, its per-view
    CLAUDE.md files and src/test/cpp/tests1/workbench/CLAUDE.md, which a
    changed-file-only closure misses.
owning_source
    Production source (.cpp/.h/.hpp/.inl/.rs) of the owning component of each
    changed production file. Ownership comes from src/main/modules/modules.json
    (components[].sources / public_headers / private_headers, longest segment
    prefix match, with ownership_exclusions carving holes). For a real
    component the reason is `owning_source` and the whole component is
    included, because a component is the unit an agent can hold in its head.
owning_source(dir)
    The monolith rule. `sakura_app` claims all of sakura_core, so including
    "the owning component" would mean including the entire legacy monolith and
    the number would be meaningless. Instead, for a changed file owned by
    `sakura_app`, the IRV set includes only that file's directory siblings -
    the .cpp/.h files sitting in the same directory - and the reason is
    recorded as `owning_source(dir)`. Directory siblings count toward the
    production-source budget exactly like `owning_source`.
contract
    The interface surface the change has to honour: public_headers of each
    owning component, every repository-local header directly #included by a
    changed production file and by those headers in turn (INCLUDE_HOPS = 2
    hops; the compiler's include roots are sakura_core/include and
    sakura_core - a header you include is an interface you depend on), and the
    public headers of any
    modules.json contracts[] entry whose contract_owner is an owning component
    (the contract ids themselves are reported as contract_ids; a contract is a
    manifest record, not a file). The one-hop forward include closure is
    calibrated: #217 changed sakura_core/window/CCustomFrameController.cpp and
    the session then read the workbench, theme, scm, explorer and activity
    headers reachable from that file's includes, none of which any ownership
    rule reaches and most of which are two hops out. The .cpp paired with a
    resolved header is included as `owning_source`, because reading a
    collaborator's interface usually forces reading its implementation.
test
    The tests needed to judge the change safely: test files under src/test/ or
    tools/build/pilots/ that #include a changed header, test files whose name
    mirrors a changed file (test-<name>.cpp, <name>_test.cpp, <name>Test.cpp
    or a src/test/cpp/tests1/ file carrying the changed stem), and the sources
    of the `<component>_tests` twin of each owning component (which is where
    tools/build/pilots/ contract tests live), and the *mirrored test
    directory*: for a changed file under sakura_core/<..>/<leaf>/, the files
    directly in src/test/cpp/tests1/<leaf>/, using the deepest directory
    segment that has a mirror. Calibration evidence: #217 read
    src/test/cpp/tests1/window/ files and #292 read
    src/test/cpp/tests1/workbench/ files that no name-similarity rule reaches.
manifest
    src/main/modules/modules.json when any changed file is component-owned,
    the .vcxproj / .vcxproj.filters / CMakeLists.txt that list a changed file,
    and src/test/test-inventory.json when a test file changed. Two further
    entries are calibrated rather than derived: sakura_core/tests1.vcxproj and
    its .filters whenever the IRV set contains a src/test/ file (a new test
    file has to be registered there, and #217 and #228 both read them), and
    the build driver whenever a build definition changed - tools/build/
    sakura_build.py, its sakura_build_lib/*.py modules and src/main/cmake/
    *.cmake (tools/CLAUDE.md makes `sakura_build.py lint checkout-invariance`
    a mandatory preflight, and #274 read sakura_build_lib/semantic_inventory.py,
    sakura_build_lib/test_inventory.py, src/main/cmake/sakura.cmake and
    src/main/cmake/build-rust-sakura-core.cmake).
formal
    docs/formal/*.tla whose model name appears in the contents of a changed
    file, together with that model's .cfg configurations, plus
    docs/formal/README.md whenever at least one model matched.
dependency_owner
    For a changed header, the *unchanged* files that directly #include it
    (reverse include, one hop). Capped at 30 files in the set; the full count
    is reported separately as dependents_total.

A file that the Issue changed is included only if it falls into one of these
categories - usually `owning_source`. The output always shows the reason per
file, and is always multi-column: there is no single IRV scalar, because an
agent's guide budget, source budget and test budget are spent separately.

Budget (a typical bug fix)
    guides            <= 3 files and <= 450 lines
    production source <= 2500 lines
    tests             <= 10 files

Commands
    estimate --issue N [--commits SHA,SHA] [--json]
    table --issues N,M,...
    explain --issue N
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from component_ownership import (MONOLITH, Ownership, is_production_source,
                                 load_ownership)
import issue_commits as ic

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent

PRODUCTION_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".rs")
SOURCE_REASONS = ("owning_source", "owning_source(dir)")
REASON_ORDER = ("guide", "manifest", "formal", "contract", "test",
                "owning_source", "owning_source(dir)", "dependency_owner")
PUBLIC_INCLUDE_ROOT = "sakura_core/include"
SOURCE_INCLUDE_ROOT = "sakura_core"
INCLUDE_HOPS = 2
BUILD_DEF_RE = re.compile(
    r"(\.vcxproj|\.vcxproj\.filters|\.cmake|CMakeLists\.txt"
    r"|src/main/modules/modules\.json|src/main/dependencies/dependencies\.json)$")
TEST_PATHSPECS = ("src/test/", "tools/build/pilots/")
DEPENDENT_CAP = 30

BUDGET = {
    "guide_files": 3,
    "guide_lines": 450,
    "source_lines": 2500,
    "test_files": 10,
}


def git(*args: str, check: bool = False) -> str:
    proc = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        if check:
            raise RuntimeError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
        return ""
    return proc.stdout


class BlobReader:
    """One `git cat-file --batch` process per commit.

    Reading a few hundred blobs with one subprocess each is the difference
    between two seconds and a minute per Issue on Windows, so every line
    count and content read in this tool goes through here and is memoised.
    """

    def __init__(self, commit: str):
        self.commit = commit
        self.proc = subprocess.Popen(
            ["git", "cat-file", "--batch"], cwd=REPO_ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        self._lines: dict[str, int | None] = {}
        self._text: dict[str, str | None] = {}

    def _fetch(self, path: str) -> bytes | None:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(f"{self.commit}:{path}\n".encode("utf-8", "replace"))
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().decode("utf-8", "replace").strip()
        if not header or header.endswith(("missing", "ambiguous")) or " blob " not in header:
            if header and " " in header and not header.endswith(("missing", "ambiguous")):
                # non-blob (tree/commit): consume its payload
                try:
                    size = int(header.rsplit(" ", 1)[1])
                    self.proc.stdout.read(size + 1)
                except (ValueError, IndexError):
                    pass
            return None
        size = int(header.rsplit(" ", 1)[1])
        data = self.proc.stdout.read(size)
        self.proc.stdout.read(1)  # trailing newline
        return data

    def lines(self, path: str) -> int | None:
        if path not in self._lines:
            data = self._fetch(path)
            if data is None:
                self._lines[path] = None
                self._text[path] = None
            else:
                self._lines[path] = data.count(b"\n") + (0 if data.endswith(b"\n") or not data else 1)
                self._text[path] = data.decode("utf-8", "replace")
        return self._lines[path]

    def text(self, path: str) -> str | None:
        if path not in self._text:
            self.lines(path)
        return self._text[path]

    def exists(self, path: str) -> bool:
        return self.lines(path) is not None

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


_TREE_CACHE: dict[str, list[str]] = {}


def tree_files(commit: str) -> list[str]:
    if commit not in _TREE_CACHE:
        out = git("ls-tree", "-r", "--name-only", commit)
        _TREE_CACHE[commit] = [l.strip().replace("\\", "/") for l in out.splitlines() if l.strip()]
    return _TREE_CACHE[commit]


def git_grep_files(commit: str, pattern: str, pathspecs: list[str] | None = None) -> list[str]:
    args = ["grep", "-l", "-I", "-E", pattern, commit]
    if pathspecs:
        args += ["--", *pathspecs]
    out = git(*args)
    result = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith(commit + ":"):
            result.append(line[len(commit) + 1:].replace("\\", "/"))
    return result


def expand_entry(commit: str, entry: str) -> list[str]:
    """A modules.json entry is either a file or a directory prefix."""
    files = tree_files(commit)
    if entry in files:
        return [entry]
    prefix = entry.rstrip("/") + "/"
    return [f for f in files if f.startswith(prefix)]


# ------------------------------------------------------------------ core
class IrvResult(dict):
    pass


def compute_irv(issue: int, rev: str = "HEAD", commits_override: list[str] | None = None,
                use_gh: bool = True) -> dict:
    own = load_ownership()

    if commits_override:
        shas = []
        for s in commits_override:
            full = git("rev-parse", "--verify", "--quiet", f"{s}^{{commit}}").strip()
            if full:
                shas.append(full)
        assoc = {
            "issue": issue,
            "commits": [{"sha": s, "short": s[:9], "date": ic.commit_meta(s)["date"],
                         "subject": ic.commit_meta(s)["subject"], "source": "explicit"}
                        for s in shas],
            "association": "explicit", "gh_available": False, "contamination": [],
            "history_cutoff": ic.history_cutoff(rev),
        }
    else:
        assoc = ic.resolve_issue_commits(issue, rev, use_gh=use_gh)

    commits = assoc["commits"]
    if not commits:
        return {"issue": issue, "error": "no commits associated with this issue",
                "association": assoc["association"],
                "contamination": assoc["contamination"],
                "history_cutoff": assoc["history_cutoff"], "commit_count": 0}

    last = commits[0]["sha"]
    changed: set[str] = set()
    for c in commits:
        changed.update(ic.commit_paths(c["sha"]))
    changed = {p for p in changed if p}

    reader = BlobReader(last)
    try:
        reasons = _classify(own, reader, last, changed)
        result = _summarise(issue, assoc, commits, changed, reasons, reader)
    finally:
        reader.close()
    return result


def _assign(reasons: dict[str, str], path: str, reason: str) -> None:
    existing = reasons.get(path)
    if existing is None or REASON_ORDER.index(reason) < REASON_ORDER.index(existing):
        reasons[path] = reason


def _classify(own: Ownership, reader: BlobReader, commit: str, changed: set[str]) -> dict[str, str]:
    reasons: dict[str, str] = {}
    changed_present = {p for p in changed if reader.exists(p)}

    # ---------------- guide ----------------
    guides = {"CLAUDE.md"}
    for path in changed:
        parts = path.split("/")
        for depth in range(0, len(parts)):
            guides.add("/".join(parts[:depth] + ["CLAUDE.md"]))
    for g in sorted(guides):
        if reader.exists(g):
            _assign(reasons, g, "guide")

    # ------------- owning components -------------
    changed_production = [p for p in changed if is_production_source(p)]
    owners: dict[str, str | None] = {p: own.owner_of(p) for p in changed_production}
    components = sorted({c for c in owners.values() if c})

    # ---------------- owning_source ----------------
    for comp in components:
        if comp == MONOLITH:
            continue
        for key in ("sources", "public_headers", "private_headers"):
            for entry in own.entries(comp, key):
                for f in expand_entry(commit, entry):
                    if f.endswith(PRODUCTION_SUFFIXES) and reader.exists(f):
                        _assign(reasons, f, "owning_source")
    # monolith rule: directory siblings only
    for path, comp in owners.items():
        if comp != MONOLITH:
            continue
        directory = path.rsplit("/", 1)[0] if "/" in path else ""
        prefix = directory + "/" if directory else ""
        for f in tree_files(commit):
            if not f.startswith(prefix):
                continue
            if "/" in f[len(prefix):]:
                continue
            if f.endswith((".cpp", ".h")):
                _assign(reasons, f, "owning_source(dir)")
    # a changed production file is always at least its own source
    for path in changed_production:
        if path in changed_present:
            _assign(reasons, path, "owning_source" if owners.get(path) != MONOLITH
                    else "owning_source(dir)")

    # ---------------- contract ----------------
    contract_ids: list[str] = []
    for comp in components:
        for h in own.entries(comp, "public_headers"):
            for f in expand_entry(commit, h):
                if reader.exists(f):
                    _assign(reasons, f, "contract")
        for c in own.contracts_owned_by(comp):
            contract_ids.append(c["id"])
            owner_comp = c.get("contract_owner")
            for h in own.entries(owner_comp, "public_headers"):
                for f in expand_entry(commit, h):
                    if reader.exists(f):
                        _assign(reasons, f, "contract")
    # one-hop forward include closure over the changed production files
    inc_re = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')
    by_basename: dict[str, list[str]] = {}
    for f in tree_files(commit):
        if f.endswith((".h", ".hpp", ".inl")) and not f.startswith("externals/"):
            by_basename.setdefault(f.rsplit("/", 1)[-1], []).append(f)
    def resolve_include(inc: str, directory: str) -> str | None:
        inc = inc.replace("\\", "/")
        for candidate in (f"{PUBLIC_INCLUDE_ROOT}/{inc}",
                          f"{directory}/{inc}" if directory else inc,
                          f"{SOURCE_INCLUDE_ROOT}/{inc}",
                          inc):
            if reader.exists(candidate):
                return candidate if not candidate.startswith("externals/") else None
        same = by_basename.get(inc.rsplit("/", 1)[-1], [])
        if len(same) == 1 and not same[0].startswith("externals/"):
            return same[0]
        return None

    resolved_headers: set[str] = set()
    frontier = [p for p in changed_present if p.endswith(PRODUCTION_SUFFIXES)]
    for hop in range(INCLUDE_HOPS):
        next_frontier = []
        for path in frontier:
            directory = path.rsplit("/", 1)[0] if "/" in path else ""
            for inc in inc_re.findall(reader.text(path) or ""):
                target = resolve_include(inc, directory)
                if target and target not in resolved_headers:
                    resolved_headers.add(target)
                    next_frontier.append(target)
        frontier = next_frontier
        if not frontier:
            break
    for header in sorted(resolved_headers):
        _assign(reasons, header, "contract")
        for suffix in (".cpp",):
            paired = header.rsplit(".", 1)[0] + suffix
            if reader.exists(paired):
                _assign(reasons, paired, "owning_source")

    # ---------------- test ----------------
    changed_headers = sorted(p for p in changed_present if p.endswith((".h", ".hpp", ".inl")))
    if changed_headers:
        alt = "|".join(re.escape(p.rsplit("/", 1)[-1]) for p in changed_headers[:60])
        pattern = rf'#\s*include\s*[<"][^">]*({alt})[">]'
        for f in git_grep_files(commit, pattern, list(TEST_PATHSPECS)):
            _assign(reasons, f, "test")
    # name mirror
    test_files_all = [f for f in tree_files(commit) if f.startswith(TEST_PATHSPECS)]
    stems = {p.rsplit("/", 1)[-1].rsplit(".", 1)[0] for p in changed_production}
    stems = {s for s in stems if len(s) >= 4}
    for f in test_files_all:
        base = f.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        for stem in stems:
            if stem.lower() in base.lower():
                if reader.exists(f):
                    _assign(reasons, f, "test")
                break
    # mirrored test directory (calibrated rule, see module docstring)
    mirror_root = "src/test/cpp/tests1/"
    mirror_dirs: set[str] = set()
    for path in changed_production:
        segments = path.split("/")[:-1]
        for seg in reversed(segments):
            candidate = f"{mirror_root}{seg}/"
            if any(f.startswith(candidate) for f in test_files_all):
                mirror_dirs.add(candidate)
                break
    for directory in mirror_dirs:
        for f in test_files_all:
            if f.startswith(directory) and "/" not in f[len(directory):]:
                if reader.exists(f):
                    _assign(reasons, f, "test")

    # *_tests twin components (this is where tools/build/pilots/ lives)
    for comp in components:
        for twin in own.test_components_for(comp):
            for key in ("sources", "public_headers", "private_headers"):
                for entry in own.entries(twin, key):
                    for f in expand_entry(commit, entry):
                        if reader.exists(f):
                            _assign(reasons, f, "test")

    # ---------------- manifest ----------------
    if any(own.is_component_owned(p) for p in changed_production):
        if reader.exists("src/main/modules/modules.json"):
            _assign(reasons, "src/main/modules/modules.json", "manifest")
    if any(p.startswith("src/test/") for p in changed):
        if reader.exists("src/test/test-inventory.json"):
            _assign(reasons, "src/test/test-inventory.json", "manifest")
    if any(p.startswith("src/test/") for p in set(changed) | set(reasons)):
        for f in ("sakura_core/tests1.vcxproj", "sakura_core/tests1.vcxproj.filters"):
            if reader.exists(f):
                _assign(reasons, f, "manifest")
    if any(BUILD_DEF_RE.search(p) for p in changed):
        for f in tree_files(commit):
            if (f == "tools/build/sakura_build.py"
                    or (f.startswith("tools/build/sakura_build_lib/") and f.endswith(".py"))
                    or (f.startswith("src/main/cmake/") and f.endswith(".cmake"))):
                if reader.exists(f):
                    _assign(reasons, f, "manifest")
    basenames = sorted({p.rsplit("/", 1)[-1] for p in changed_production})
    if basenames:
        alt = "|".join(re.escape(b) for b in basenames[:80])
        for f in git_grep_files(commit, rf"({alt})",
                                ["*.vcxproj", "*.vcxproj.filters", "CMakeLists.txt", "*.cmake"]):
            if not f.startswith("src/main/modules/generated/") and reader.exists(f):
                _assign(reasons, f, "manifest")

    # ---------------- formal ----------------
    models = [f for f in tree_files(commit)
              if f.startswith("docs/formal/") and f.endswith(".tla")]
    matched_models = []
    changed_blob = "\n".join((reader.text(p) or "") for p in changed_present
                             if p.endswith(PRODUCTION_SUFFIXES + (".md", ".json")))
    for model in models:
        name = model.rsplit("/", 1)[-1][:-4]
        if name in changed_blob:
            matched_models.append((model, name))
    for model, name in matched_models:
        _assign(reasons, model, "formal")
        for f in tree_files(commit):
            if f.startswith(f"docs/formal/{name}") and f.endswith(".cfg"):
                _assign(reasons, f, "formal")
    if matched_models and reader.exists("docs/formal/README.md"):
        _assign(reasons, "docs/formal/README.md", "formal")

    # ---------------- dependency_owner ----------------
    dependents_total = 0
    if changed_headers:
        alt = "|".join(re.escape(p.rsplit("/", 1)[-1]) for p in changed_headers[:60])
        pattern = rf'#\s*include\s*[<"][^">]*({alt})[">]'
        hits = [f for f in git_grep_files(commit, pattern,
                                          ["*.cpp", "*.h", "*.hpp", "*.inl"])
                if f not in changed and not f.startswith("externals/")]
        dependents_total = len(hits)
        for f in sorted(hits)[:DEPENDENT_CAP]:
            _assign(reasons, f, "dependency_owner")

    # guide closure is re-run over the final IRV set, not only over the
    # changed files; `guide` has the highest precedence so this can only
    # promote a file, never demote one.
    closure_guides = {"CLAUDE.md"}
    for path in set(reasons) | changed:
        parts = path.split("/")
        for depth in range(0, len(parts)):
            closure_guides.add("/".join(parts[:depth] + ["CLAUDE.md"]))
    for g in sorted(closure_guides):
        if reader.exists(g):
            _assign(reasons, g, "guide")

    reasons["__meta__"] = json.dumps({
        "components": components,
        "contract_ids": sorted(set(contract_ids)),
        "dependents_total": dependents_total,
        "formal_models": [m for _, m in matched_models],
    })
    return reasons


def _summarise(issue: int, assoc: dict, commits: list[dict], changed: set[str],
               reasons: dict[str, str], reader: BlobReader) -> dict:
    meta = json.loads(reasons.pop("__meta__"))
    per_file = []
    for path in sorted(reasons):
        per_file.append({"path": path, "reason": reasons[path],
                         "lines": reader.lines(path) or 0})

    def agg(reason_set):
        sel = [f for f in per_file if f["reason"] in reason_set]
        return len(sel), sum(f["lines"] for f in sel)

    guide_files, guide_lines = agg({"guide"})
    source_files, source_lines = agg(set(SOURCE_REASONS))
    test_files, test_lines = agg({"test"})
    contract_files, contract_lines = agg({"contract"})
    formal_files, _ = agg({"formal"})
    manifest_files, _ = agg({"manifest"})
    dep_files, dep_lines = agg({"dependency_owner"})

    guide_ok = guide_files <= BUDGET["guide_files"] and guide_lines <= BUDGET["guide_lines"]
    source_ok = source_lines <= BUDGET["source_lines"]
    test_ok = test_files <= BUDGET["test_files"]

    return {
        "issue": issue,
        "association": assoc["association"],
        "commit_count": len(commits),
        "commits": [c["short"] for c in commits],
        "last_commit": commits[0]["short"],
        "history_cutoff": assoc["history_cutoff"],
        "contamination": assoc["contamination"],
        "contamination_short": ic.contamination_short(assoc["contamination"]),
        "changed_files": len(changed),
        "guide_files": guide_files,
        "guide_lines": guide_lines,
        "source_files": source_files,
        "source_lines": source_lines,
        "test_files": test_files,
        "test_lines": test_lines,
        "contract_files": contract_files,
        "contract_lines": contract_lines,
        "formal_files": formal_files,
        "formal_models": meta["formal_models"],
        "manifest_files": manifest_files,
        "dependency_owner_files": dep_files,
        "dependents_total": meta["dependents_total"],
        "component_count": len(meta["components"]),
        "components": meta["components"],
        "contract_ids": meta["contract_ids"],
        "total_files": len(per_file),
        "total_lines": sum(f["lines"] for f in per_file),
        "guide_ok": guide_ok,
        "source_ok": source_ok,
        "test_ok": test_ok,
        "budget_ok": guide_ok and source_ok and test_ok,
        "files": per_file,
    }


# ----------------------------------------------------------------- CLI
def cmd_estimate(args) -> int:
    overrides = [s.strip() for s in args.commits.split(",")] if args.commits else None
    r = compute_irv(args.issue, args.rev, overrides, use_gh=not args.no_gh)
    if args.json:
        out = dict(r)
        out.pop("files", None)
        json.dump(out, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0 if "error" not in r else 1
    if "error" in r:
        print(f"issue: #{r['issue']}")
        print(f"error: {r['error']}")
        return 1
    keys = ["issue", "association", "commit_count", "changed_files",
            "guide_files", "guide_lines", "source_files", "source_lines",
            "test_files", "test_lines", "contract_files", "formal_files",
            "manifest_files", "dependency_owner_files", "dependents_total",
            "component_count", "total_files", "total_lines",
            "guide_ok", "source_ok", "test_ok", "budget_ok"]
    for k in keys:
        print(f"{k}: {r[k]}")
    print(f"components: {','.join(r['components']) or '-'}")
    print(f"contamination: {r['contamination_short']}")
    return 0


def cmd_table(args) -> int:
    issues = [int(x) for x in args.issues.split(",") if x.strip()]
    rows = [compute_irv(n, args.rev, use_gh=not args.no_gh) for n in issues]
    if args.json:
        for r in rows:
            r.pop("files", None)
        json.dump(rows, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0
    header = ("| Issue | commits | changed_files | guide_files | guide_lines | "
              "source_files | source_lines | test_files | contract_files | "
              "formal_files | manifest_files | components | budget_ok | contamination |")
    print(header)
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|")
    for r in rows:
        if "error" in r:
            print(f"| #{r['issue']} | 0 | - | - | - | - | - | - | - | - | - | - | - | "
                  f"{ic.contamination_short(r.get('contamination', []))} |")
            continue
        print(f"| #{r['issue']} | {r['commit_count']} | {r['changed_files']} | "
              f"{r['guide_files']} | {r['guide_lines']} | {r['source_files']} | "
              f"{r['source_lines']} | {r['test_files']} | {r['contract_files']} | "
              f"{r['formal_files']} | {r['manifest_files']} | {r['component_count']} | "
              f"{'yes' if r['budget_ok'] else 'no'} | {r['contamination_short']} |")
    return 0


def cmd_explain(args) -> int:
    r = compute_irv(args.issue, args.rev, use_gh=not args.no_gh)
    if "error" in r:
        print(f"issue: #{r['issue']}\nerror: {r['error']}")
        return 1
    print(f"# IRV for Issue #{r['issue']}  ({r['commit_count']} commits, "
          f"association={r['association']}, last={r['last_commit']})")
    print(f"components: {','.join(r['components']) or '-'}")
    if r["contract_ids"]:
        print(f"contract_ids: {','.join(r['contract_ids'])}")
    if r["formal_models"]:
        print(f"formal_models: {','.join(r['formal_models'])}")
    print(f"dependents_total: {r['dependents_total']} "
          f"(capped at {DEPENDENT_CAP} in the set)")
    print()
    print(f"{'reason':<20} {'lines':>7}  path")
    for reason in REASON_ORDER:
        sel = [f for f in r["files"] if f["reason"] == reason]
        for f in sorted(sel, key=lambda x: -x["lines"]):
            print(f"{f['reason']:<20} {f['lines']:>7}  {f['path']}")
    print()
    print(f"total: {r['total_files']} files, {r['total_lines']} lines; "
          f"budget_ok={r['budget_ok']} (guide={r['guide_ok']}, "
          f"source={r['source_ok']}, test={r['test_ok']})")
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rev", default="HEAD")
    p.add_argument("--no-gh", action="store_true")
    sub = p.add_subparsers(dest="command", required=True)

    e = sub.add_parser("estimate")
    e.add_argument("--issue", type=int, required=True)
    e.add_argument("--commits", default=None, help="comma separated SHAs overriding association")
    e.add_argument("--json", action="store_true")
    e.set_defaults(func=cmd_estimate)

    t = sub.add_parser("table")
    t.add_argument("--issues", required=True)
    t.add_argument("--json", action="store_true")
    t.set_defaults(func=cmd_table)

    x = sub.add_parser("explain")
    x.add_argument("--issue", type=int, required=True)
    x.set_defaults(func=cmd_explain)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
