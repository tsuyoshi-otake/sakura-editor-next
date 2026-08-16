#!/usr/bin/env python3
"""Verify owned runtime-source snapshots.

Canonical forks live under tsuyoshi-otake/*. The product build consumes a
verified tree in third_party/owned/<id>/ when a ledger row says so. This
module does not fetch the network. Re-import is a maintainer operation;
normal CI only checks that an existing snapshot still matches SNAPSHOT.json.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

SNAPSHOT_NAME = "SNAPSHOT.json"
REQUIRED_KEYS = (
    "id",
    "canonicalRepository",
    "commit",
    "sourceTreeSha256",
    "license",
    "localModificationAllowed",
)


def posix_relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def iter_snapshot_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        parts = path.relative_to(root).parts
        if ".git" in parts:
            continue
        if path.name == SNAPSHOT_NAME and path.parent == root:
            continue
        files.append(path)
    return files


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compute_source_tree_sha256(root: Path) -> str:
    lines: list[str] = []
    for path in iter_snapshot_files(root):
        lines.append(f"{file_sha256(path)}  {posix_relative(path, root)}")
    payload = ("\n".join(lines) + ("\n" if lines else "")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def load_manifest(snapshot_dir: Path) -> dict:
    manifest_path = snapshot_dir / SNAPSHOT_NAME
    if not manifest_path.is_file():
        raise ValueError(f"missing {SNAPSHOT_NAME} in {snapshot_dir}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if not isinstance(manifest, dict):
        raise ValueError(f"{manifest_path} must be a JSON object")
    missing = [key for key in REQUIRED_KEYS if key not in manifest]
    if missing:
        raise ValueError(f"{manifest_path} is missing {missing}")
    if manifest.get("localModificationAllowed") is not False:
        raise ValueError(f"{manifest_path} must set localModificationAllowed to false")
    return manifest


def verify_snapshot(snapshot_dir: Path) -> dict:
    """Fail closed when the tree bytes no longer match the frozen manifest."""

    if not snapshot_dir.is_dir():
        raise ValueError(f"owned snapshot is missing: {snapshot_dir}")
    manifest = load_manifest(snapshot_dir)
    actual = compute_source_tree_sha256(snapshot_dir)
    expected = str(manifest["sourceTreeSha256"])
    if actual != expected:
        raise ValueError(
            f"{snapshot_dir} sourceTreeSha256 mismatch: "
            f"manifest {expected}, tree {actual}"
        )
    return manifest
