#!/usr/bin/env python3
"""Component ownership resolution backed by src/main/modules/modules.json.

This module is the single place that answers "which component owns this
repository path?". It is imported by issue_commits.py (contamination
detection) and irv.py (Issue Reading Volume estimation), so the dependency
direction is ownership <- issue_commits <- irv / issue_footprint and stays
acyclic.

Ownership rule
--------------
Every component declares ``sources``, ``public_headers`` and
``private_headers``. Each entry is either a concrete file path or a directory
prefix. A path is matched against every entry with segment-aware longest
prefix matching, and the component with the longest match wins.

``ownership_exclusions`` carve holes out of a component: if an exclusion entry
of a component matches the path with a longer prefix than the component's own
best match, that component is disqualified. This is how the legacy monolith
``sakura_app`` (which claims the whole of ``sakura_core``) gives up the
directories that have been extracted into real components.

If nothing matches, production C++/Rust sources fall back to the monolith
``sakura_app``; every other path is reported as unowned (None).
"""

from __future__ import annotations

import json
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent
MODULES_JSON = "src/main/modules/modules.json"

MONOLITH = "sakura_app"
PRODUCTION_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".rs")
NON_PRODUCTION_PREFIXES = (
    "src/test/",
    "tools/",
    "docs/",
    "externals/",
    "src/main/modules/generated/",
)


def _norm(path: str) -> str:
    return path.replace("\\", "/").strip().lstrip("./")


def _prefix_len(entry: str, path: str) -> int:
    """Segment-aware prefix match length, or -1 when the entry does not match."""
    if path == entry:
        return len(entry)
    if path.startswith(entry + "/"):
        return len(entry)
    return -1


class Ownership:
    def __init__(self, manifest: dict):
        self.manifest = manifest
        self.components = {c["id"]: c for c in manifest.get("components", [])}
        self._owns: dict[str, list[str]] = {}
        self._excl: dict[str, list[str]] = {}
        # A component's "owner" directory is a weak ownership prefix used for
        # files that live in a component's directory but are not (yet) listed
        # in sources/headers. It is ignored when several components declare the
        # same owner directory (tools/build/pilots is shared by every *_tests
        # component), because it cannot disambiguate there.
        owner_dirs: dict[str, list[str]] = {}
        for cid, comp in self.components.items():
            owner = _norm(comp.get("owner") or "")
            if owner:
                owner_dirs.setdefault(owner, []).append(cid)
        shared_owner_dirs = {d for d, ids in owner_dirs.items() if len(ids) > 1}
        for cid, comp in self.components.items():
            entries = []
            for key in ("sources", "public_headers", "private_headers"):
                entries.extend(_norm(e) for e in comp.get(key) or [])
            owner = _norm(comp.get("owner") or "")
            if owner and owner not in shared_owner_dirs:
                entries.append(owner)
            self._owns[cid] = entries
            self._excl[cid] = [_norm(e) for e in comp.get("ownership_exclusions") or []]
        self._cache: dict[str, str | None] = {}
        self.contracts = manifest.get("contracts", [])

    # -- ownership -----------------------------------------------------
    def owner_of(self, path: str) -> str | None:
        path = _norm(path)
        if path in self._cache:
            return self._cache[path]
        best_id: str | None = None
        best_len = -1
        monolith_disqualified = False
        for cid, entries in self._owns.items():
            match = max((_prefix_len(e, path) for e in entries), default=-1)
            if match < 0:
                continue
            excl = max((_prefix_len(e, path) for e in self._excl[cid]), default=-1)
            if excl > match:
                if cid == MONOLITH:
                    monolith_disqualified = True
                continue
            if match > best_len:
                best_id, best_len = cid, match
        if best_id is None and not monolith_disqualified and is_production_source(path):
            best_id = MONOLITH
        self._cache[path] = best_id
        return best_id

    def is_component_owned(self, path: str) -> bool:
        owner = self.owner_of(path)
        return owner is not None and owner != MONOLITH

    # -- component data ------------------------------------------------
    def component(self, cid: str) -> dict:
        return self.components.get(cid, {})

    def entries(self, cid: str, key: str) -> list[str]:
        return [_norm(e) for e in (self.components.get(cid, {}).get(key) or [])]

    def test_components_for(self, cid: str) -> list[str]:
        """Components that look like the test twin of ``cid`` (``<cid>_tests``)."""
        out = []
        for other in self.components:
            if other == cid:
                continue
            if other == cid + "_tests" or other.replace("_tests", "") == cid:
                if other.endswith("_tests"):
                    out.append(other)
        return sorted(set(out))

    def contracts_owned_by(self, cid: str) -> list[dict]:
        return [c for c in self.contracts if c.get("contract_owner") == cid]


def is_production_source(path: str) -> bool:
    path = _norm(path)
    if path.startswith(NON_PRODUCTION_PREFIXES):
        return False
    return path.endswith(PRODUCTION_SUFFIXES)


def load_ownership(text: str | None = None) -> Ownership:
    if text is None:
        text = (REPO_ROOT / MODULES_JSON).read_text(encoding="utf-8")
    return Ownership(json.loads(text))


if __name__ == "__main__":
    import sys

    own = load_ownership()
    for arg in sys.argv[1:]:
        print(f"{arg}: {own.owner_of(arg)}")
