"""Checkout-line-ending invariance checks for architecture-gate inputs."""

from __future__ import annotations

import hashlib
from pathlib import Path

from . import semantic_inventory
from .generator import stale_outputs
from .model import SemanticGraph, load_semantic_graph


SCHEMA_PATH = Path("src/main/modules/schema-v4.json")


def _lf_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _lf_bytes(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def _scanner_version_for_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(_lf_bytes(data)).hexdigest()


def _relative_path(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path.resolve())


def verify_checkout_invariance(
    repo_root: Path,
    manifest_path: Path | None = None,
    *,
    current_graph: SemanticGraph | None = None,
) -> dict[str, object]:
    """Prove that LF/CRLF checkouts keep gate inputs and projections equivalent.

    The alternate inputs are supplied only to the model; the checkout is never
    written. A distinct schema/scanner content variant must also alter its
    version, so this lint cannot pass when an input is omitted from its hash.
    """

    root = repo_root.resolve()
    graph = current_graph or load_semantic_graph(root, manifest_path)
    schema_path = root / SCHEMA_PATH
    schema_text = schema_path.read_text(encoding="utf-8")
    schema_lf = _lf_text(schema_text)
    schema_crlf = schema_lf.replace("\n", "\r\n")
    schema_content_changed = schema_lf + "\n"

    schema_graphs = {
        "current": graph,
        "lf": load_semantic_graph(root, manifest_path, schema_text=schema_lf),
        "crlf": load_semantic_graph(root, manifest_path, schema_text=schema_crlf),
        "content_changed": load_semantic_graph(root, manifest_path, schema_text=schema_content_changed),
    }
    graph_hashes = {name: item.semantic_graph_hash for name, item in schema_graphs.items()}
    projection_stale = {name: stale_outputs(item) for name, item in schema_graphs.items() if name != "content_changed"}
    graph_line_endings_invariant = len({graph_hashes[name] for name in ("current", "lf", "crlf")}) == 1
    projection_line_endings_invariant = len({tuple(paths) for paths in projection_stale.values()}) == 1
    schema_content_changes_graph = graph_hashes["content_changed"] != graph_hashes["lf"]

    scanner_path = Path(semantic_inventory.__file__).resolve()
    scanner_bytes = scanner_path.read_bytes()
    scanner_lf = _lf_bytes(scanner_bytes)
    scanner_crlf = scanner_lf.replace(b"\n", b"\r\n")
    scanner_versions = {
        "current": semantic_inventory._scanner_version(),
        "lf": _scanner_version_for_bytes(scanner_lf),
        "crlf": _scanner_version_for_bytes(scanner_crlf),
        "content_changed": _scanner_version_for_bytes(scanner_lf + b"\n"),
    }
    scanner_line_endings_invariant = scanner_versions["lf"] == scanner_versions["crlf"]
    scanner_current_matches_canonical = scanner_versions["current"] == scanner_versions["lf"]
    scanner_content_changes_version = scanner_versions["content_changed"] != scanner_versions["lf"]

    return {
        "ok": (
            graph_line_endings_invariant
            and projection_line_endings_invariant
            and schema_content_changes_graph
            and scanner_line_endings_invariant
            and scanner_current_matches_canonical
            and scanner_content_changes_version
        ),
        "semantic_graph": {
            "schema": _relative_path(root, schema_path),
            "hashes": graph_hashes,
            "line_endings_invariant": graph_line_endings_invariant,
            "content_change_affects_graph": schema_content_changes_graph,
            "projection_stale": projection_stale,
            "projection_staleness_invariant": projection_line_endings_invariant,
        },
        "semantic_inventory_scanner": {
            "source": _relative_path(root, scanner_path),
            "versions": scanner_versions,
            "line_endings_invariant": scanner_line_endings_invariant,
            "current_matches_canonical": scanner_current_matches_canonical,
            "content_change_affects_version": scanner_content_changes_version,
        },
    }
