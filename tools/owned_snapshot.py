#!/usr/bin/env python3
"""Verify owned runtime-source snapshots.

Canonical forks live under tsuyoshi-otake/*. The product build consumes a
verified tree in third_party/owned/<id>/ when a ledger row says so. This
module does not fetch the network. Re-import is a maintainer operation;
normal CI only checks that an existing snapshot still matches SNAPSHOT.json.

Tree digests are taken from git blob bytes when possible so checkout-time
eol / working-tree-encoding filters cannot make Linux and Windows disagree.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
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
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        parts = path.relative_to(root).parts
        if ".git" in parts:
            continue
        if path.name == SNAPSHOT_NAME and path.parent == root:
            continue
        files.append(path)
    # Path sorting is case-insensitive on Windows; digest order must be
    # platform-stable, so sort by the posix relative path string.
    return sorted(files, key=lambda path: posix_relative(path, root))


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _digest_lines(entries: list[tuple[str, str]]) -> str:
    payload = ("\n".join(f"{digest}  {rel}" for digest, rel in entries) + ("\n" if entries else "")).encode(
        "utf-8"
    )
    return hashlib.sha256(payload).hexdigest()


def compute_source_tree_sha256_from_workdir(root: Path) -> str:
    entries = [
        (file_sha256(path), posix_relative(path, root)) for path in iter_snapshot_files(root)
    ]
    return _digest_lines(entries)


def _git_repo_root(start: Path) -> Path | None:
    try:
        output = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=start,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return Path(output.strip())


def compute_source_tree_sha256_from_git(snapshot_dir: Path) -> str | None:
    """Hash tracked blob bytes for the snapshot directory.

    Returns None when git is unavailable or the directory is not tracked.
    """

    resolved = snapshot_dir.resolve()
    repo_root = _git_repo_root(resolved)
    if repo_root is None:
        return None
    try:
        relative = resolved.relative_to(repo_root).as_posix()
    except ValueError:
        return None

    try:
        listed = subprocess.check_output(
            ["git", "ls-files", "-z", "--", relative],
            cwd=repo_root,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return None

    rel_paths = [item.decode("utf-8") for item in listed.split(b"\0") if item]
    if not rel_paths:
        return None

    prefix = relative.rstrip("/") + "/"
    entries: list[tuple[str, str]] = []
    for repo_rel in sorted(rel_paths):
        if not repo_rel.startswith(prefix) and repo_rel != relative:
            continue
        if repo_rel == f"{relative}/{SNAPSHOT_NAME}":
            continue
        snapshot_rel = repo_rel[len(prefix) :] if repo_rel.startswith(prefix) else Path(repo_rel).name
        try:
            blob = subprocess.check_output(
                ["git", "cat-file", "blob", f":{repo_rel}"],
                cwd=repo_root,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.CalledProcessError):
            return None
        entries.append((hashlib.sha256(blob).hexdigest(), snapshot_rel))
    return _digest_lines(entries)


def compute_source_tree_sha256(root: Path) -> str:
    from_git = compute_source_tree_sha256_from_git(root)
    if from_git is not None:
        return from_git
    return compute_source_tree_sha256_from_workdir(root)


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
