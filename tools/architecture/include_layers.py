#!/usr/bin/env python3
"""Measure upward #include edges between architectural layers.

Layer membership is DERIVED from the component manifest
(src/main/modules/modules.json), which is the single source of architecture
truth. tools/architecture/layers.json only says how a component family maps to
a rank, and carries hand-written overrides for the paths that are still inside
the sakura_app monolith. Every override must justify itself with a "reason" and
a "removal_condition"; "check-derivation" fails when an override contradicts
the manifest.

A source file in a lower-rank layer that includes a header owned by a
higher-rank layer is an "upward include" - a dependency pointing away from
stable code.

Commands
    report [--json]                totals and per layer-pair counts
    check-derivation [--strict]    prove overrides still agree with the manifest
    explain --from A --to B        every upward edge between two layers
    file <repo-relative path>      layer and includes of one file

Only the standard library is used. Nothing is written to the repository.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent
DEFAULT_MAP = TOOL_DIR / "layers.json"
DEFAULT_MANIFEST = REPO_ROOT / "src" / "main" / "modules" / "modules.json"
SOURCE_ROOTS = ("sakura_core", "src/main/cpp")
SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".inl")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

KIND_COMPONENT = "derived_component"
KIND_CONTRACT = "derived_contract"
KIND_OVERRIDE = "override_monolith"
KIND_UNCLASSIFIED = "unclassified"


def _rel(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def _norm(entry: str) -> str:
    return entry.replace("\\", "/").strip("/")


def _path_under(rel: str, entry: str) -> bool:
    """True when rel is entry itself or lives inside the directory entry."""
    return rel == entry or rel.startswith(entry + "/")


# --------------------------------------------------------------------------
# model loading
# --------------------------------------------------------------------------


def load_manifest(manifest_path: Path) -> dict:
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def load_model(map_path: Path = DEFAULT_MAP, manifest_path: Path = DEFAULT_MANIFEST) -> dict:
    """Load layers.json plus modules.json and build the derivation indexes."""
    data = json.loads(Path(map_path).read_text(encoding="utf-8"))
    manifest = load_manifest(Path(manifest_path))
    return build_model(data, manifest, map_path=Path(map_path))


def build_model(data: dict, manifest: dict, map_path: Path = DEFAULT_MAP) -> dict:
    layers = data["layers"]
    ranks = {layer["id"]: int(layer["rank"]) for layer in layers}
    order = [layer["id"] for layer in layers]

    layer_of_rank = {}
    for layer in layers:
        rank = int(layer["rank"])
        if layer.get("derived_target") or rank not in layer_of_rank:
            if rank in layer_of_rank and not layer.get("derived_target"):
                continue
            layer_of_rank[rank] = layer["id"]

    derivation = data["derivation"]
    family_rank = {str(k): int(v) for k, v in derivation["family_rank"].items()}
    monolith_families = set(derivation.get("monolith_families", ["legacy"]))
    contract_layer = derivation["contract_layer"]
    contract_prefixes = [_norm(p) for p in derivation.get("contract_prefixes", [])]

    components = {c["id"]: c for c in manifest.get("components", [])}

    # ownership: sources + private_headers of non-monolith components
    owner_table = []  # (entry, component_id)
    contract_paths = set()
    for comp in manifest.get("components", []):
        for header in comp.get("public_headers", []) or []:
            contract_paths.add(_norm(header))
        if comp.get("family") in monolith_families:
            continue
        for key in ("sources", "private_headers"):
            for entry in comp.get(key, []) or []:
                owner_table.append((_norm(entry), comp["id"]))
    owner_table.sort(key=lambda item: -len(item[0]))

    overrides = []
    for item in data.get("monolith_overrides", []):
        overrides.append(
            {
                "prefix": item["prefix"].replace("\\", "/"),
                "layer": item["layer"],
                "reason": (item.get("reason") or "").strip(),
                "removal_condition": (item.get("removal_condition") or "").strip(),
            }
        )
    override_table = sorted(overrides, key=lambda item: -len(item["prefix"]))

    data["_ranks"] = ranks
    data["_order"] = order
    data["_layer_of_rank"] = layer_of_rank
    data["_family_rank"] = family_rank
    data["_monolith_families"] = monolith_families
    data["_contract_layer"] = contract_layer
    data["_contract_prefixes"] = contract_prefixes
    data["_contract_paths"] = contract_paths
    data["_owner_table"] = owner_table
    data["_overrides"] = overrides
    data["_override_table"] = override_table
    data["_components"] = components
    data["_manifest"] = manifest
    data["_map_path"] = Path(map_path)
    return data


# --------------------------------------------------------------------------
# classification
# --------------------------------------------------------------------------


def owner_of(rel_path: str, model: dict):
    """Component id that owns rel_path, or None when it stays in the monolith."""
    for entry, comp_id in model["_owner_table"]:
        if _path_under(rel_path, entry):
            return comp_id
    return None


def override_for(rel_path: str, model: dict):
    for item in model["_override_table"]:
        if rel_path.startswith(item["prefix"]):
            return item
    return None


def _layer_for_rank(model: dict, rank: int):
    return model["_layer_of_rank"].get(rank)


def classify(rel_path: str, model: dict):
    """Return (layer_id, rank, classification_kind) or None when excluded.

    An unclassified file returns (None, None, KIND_UNCLASSIFIED): it is counted
    and reported, never silently dropped.
    """
    rel_path = rel_path.replace("\\", "/")
    for prefix in model.get("exclude_prefixes", []):
        if rel_path.startswith(prefix):
            return None

    # (1) contracts
    if rel_path in model["_contract_paths"] or any(
        _path_under(rel_path, prefix) for prefix in model["_contract_prefixes"]
    ):
        layer = model["_contract_layer"]
        return layer, model["_ranks"][layer], KIND_CONTRACT

    # (2) component ownership
    comp_id = owner_of(rel_path, model)
    if comp_id is not None:
        family = model["_components"][comp_id].get("family")
        rank = model["_family_rank"].get(family)
        if rank is not None:
            layer = _layer_for_rank(model, rank)
            if layer is not None:
                return layer, rank, KIND_COMPONENT

    # (3) monolith overrides
    item = override_for(rel_path, model)
    if item is not None:
        layer = item["layer"]
        return layer, model["_ranks"][layer], KIND_OVERRIDE

    # (4) unclassified
    return None, None, KIND_UNCLASSIFIED


# --------------------------------------------------------------------------
# include scanning
# --------------------------------------------------------------------------


def collect_sources() -> list:
    result = []
    for root in SOURCE_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in {"vendor", ".git"}]
            for name in filenames:
                if name.endswith(SOURCE_SUFFIXES):
                    result.append(Path(dirpath) / name)
    result.sort()
    return result


def build_resolver(sources: list):
    by_base = collections.defaultdict(list)
    rels = {}
    for path in sources:
        rel = _rel(path)
        rels[rel] = path
        by_base[path.name].append(rel)

    def resolve(includer_rel: str, include: str):
        include = include.replace("\\", "/")
        base = include.rsplit("/", 1)[-1]
        candidates = by_base.get(base)
        if not candidates:
            return None
        if len(candidates) == 1:
            return candidates[0]
        includer_dir = includer_rel.rsplit("/", 1)[0]
        same_dir = f"{includer_dir}/{include}"
        if same_dir in rels:
            return same_dir
        for candidate in candidates:
            if candidate.endswith("/" + include):
                return candidate
        for root in SOURCE_ROOTS:
            joined = f"{root}/{include}"
            if joined in rels:
                return joined
        return None

    return resolve


def collect_edges(model: dict):
    sources = collect_sources()
    resolve = build_resolver(sources)
    edges = []  # (src_rel, dst_rel, src_layer, dst_layer, src_rank, dst_rank)
    unresolved = 0
    kind_counts = collections.Counter()
    unclassified_files = []
    for path in sources:
        src_rel = _rel(path)
        src_cls = classify(src_rel, model)
        if src_cls is None:
            continue
        kind_counts[src_cls[2]] += 1
        if src_cls[0] is None:
            unclassified_files.append(src_rel)
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for match in INCLUDE_RE.finditer(text):
            dst_rel = resolve(src_rel, match.group(1))
            if dst_rel is None:
                unresolved += 1
                continue
            dst_cls = classify(dst_rel, model)
            if dst_cls is None or dst_cls[0] is None:
                continue
            edges.append((src_rel, dst_rel, src_cls[0], dst_cls[0], src_cls[1], dst_cls[1]))
    stats = {
        "source_files": len(sources),
        "kind_counts": kind_counts,
        "unclassified_files": unclassified_files,
    }
    return edges, unresolved, stats


def summarize(model: dict) -> dict:
    edges, unresolved, stats = collect_edges(model)
    upward = [e for e in edges if e[5] > e[4]]
    pair_edges = collections.Counter()
    pair_files = collections.defaultdict(set)
    per_file = collections.Counter()
    for src, _dst, sl, dl, _sr, _dr in upward:
        pair_edges[(sl, dl)] += 1
        pair_files[(sl, dl)].add(src)
        per_file[src] += 1
    pairs = [
        {"from": sl, "to": dl, "edges": n, "files": len(pair_files[(sl, dl)])}
        for (sl, dl), n in sorted(pair_edges.items(), key=lambda item: (-item[1], item[0]))
    ]
    layer_counts = collections.Counter()
    for src_rel in {e[0] for e in edges}:
        cls = classify(src_rel, model)
        if cls and cls[0]:
            layer_counts[cls[0]] += 1
    kind_counts = stats["kind_counts"]
    return {
        "schema_version": 2,
        "layer_map": _rel(model["_map_path"]),
        "source_of_truth": model.get("source_of_truth"),
        "layers": [
            {"id": lid, "rank": model["_ranks"][lid], "files_with_includes": layer_counts.get(lid, 0)}
            for lid in model["_order"]
        ],
        "source_files": stats["source_files"],
        "classification_source_counts": {
            KIND_COMPONENT: kind_counts.get(KIND_COMPONENT, 0),
            KIND_CONTRACT: kind_counts.get(KIND_CONTRACT, 0),
            KIND_OVERRIDE: kind_counts.get(KIND_OVERRIDE, 0),
            KIND_UNCLASSIFIED: kind_counts.get(KIND_UNCLASSIFIED, 0),
        },
        "unclassified_files": sorted(stats["unclassified_files"])[:25],
        "override_count": len(model["_overrides"]),
        "include_edges_total": len(edges),
        "unresolved": unresolved,
        "unresolved_includes": unresolved,
        "upward_edges_total": len(upward),
        "upward_files_total": len(per_file),
        "pairs": pairs,
        "top_files": [{"file": f, "upward_edges": n} for f, n in per_file.most_common(15)],
    }


# --------------------------------------------------------------------------
# derivation consistency check
# --------------------------------------------------------------------------


def check_derivation(model: dict, files=None) -> dict:
    """Prove the overrides still agree with the manifest.

    files: repo-relative paths to test for override/ownership rank conflicts.
    Defaults to every scanned source file.
    """
    errors = []
    warnings = []
    overrides = model["_overrides"]
    ranks = model["_ranks"]
    components = model["_components"]
    family_rank = model["_family_rank"]
    monolith_families = model["_monolith_families"]

    # unknown layer ids / missing justification
    for item in overrides:
        if item["layer"] not in ranks:
            errors.append(
                "override '%s' names unknown layer '%s'" % (item["prefix"], item["layer"])
            )
        if not item["reason"]:
            errors.append("override '%s' has an empty reason" % item["prefix"])
        if not item["removal_condition"]:
            errors.append("override '%s' has an empty removal_condition" % item["prefix"])

    # duplicate prefixes with different layers (nesting is legal, longest wins)
    by_prefix = collections.defaultdict(set)
    for item in overrides:
        by_prefix[_norm(item["prefix"])].add(item["layer"])
    for prefix, layer_ids in sorted(by_prefix.items()):
        if len(layer_ids) > 1:
            errors.append(
                "overrides overlap: prefix '%s' is mapped to %s"
                % (prefix, ", ".join(sorted(layer_ids)))
            )

    # an override prefix that is already owned by an extracted component
    for item in overrides:
        comp_id = owner_of(_norm(item["prefix"]), model)
        if comp_id is not None and components[comp_id].get("family") not in monolith_families:
            errors.append(
                "override '%s' is owned by non-legacy component '%s' in the manifest"
                % (item["prefix"], comp_id)
            )

    # an override that would classify an owned file at a different rank
    mismatches = {}
    if files is None:
        files = [_rel(path) for path in collect_sources()]
    for rel in files:
        comp_id = owner_of(rel, model)
        if comp_id is None:
            continue
        family = components[comp_id].get("family")
        if family in monolith_families or family not in family_rank:
            continue
        item = override_for(rel, model)
        if item is None or item["layer"] not in ranks:
            continue
        if ranks[item["layer"]] != family_rank[family]:
            mismatches[(item["prefix"], comp_id)] = (item["layer"], family, rel)
    for (prefix, comp_id), (layer, family, rel) in sorted(mismatches.items()):
        errors.append(
            "override '%s' -> %s (rank %d) contradicts component '%s' family '%s' (rank %d), "
            "e.g. %s" % (prefix, layer, ranks[layer], comp_id, family, family_rank[family], rel)
        )

    # stale overrides: removal_condition names a component that now exists
    for item in overrides:
        for comp_id in components:
            if comp_id in item["removal_condition"] and comp_id != "sakura_app":
                warnings.append(
                    "override '%s' may be stale: removal_condition names existing component '%s'"
                    % (item["prefix"], comp_id)
                )

    # manifest dependency edges that point upward
    manifest_upward = []
    for edge in model["_manifest"].get("edges", []) or []:
        src, dst = edge.get("from"), edge.get("to")
        if src not in components or dst not in components:
            continue
        src_rank = family_rank.get(components[src].get("family"))
        dst_rank = family_rank.get(components[dst].get("family"))
        if src_rank is None or dst_rank is None:
            continue
        if dst_rank > src_rank:
            manifest_upward.append(
                {
                    "from": src,
                    "to": dst,
                    "kind": edge.get("kind"),
                    "from_rank": src_rank,
                    "to_rank": dst_rank,
                }
            )

    return {"errors": errors, "warnings": warnings, "manifest_upward_edges": manifest_upward}


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------


def _model_from_args(args: argparse.Namespace) -> dict:
    return load_model(Path(args.layer_map), Path(args.manifest))


def cmd_report(args: argparse.Namespace) -> int:
    model = _model_from_args(args)
    summary = summarize(model)
    if args.json:
        json.dump(summary, sys.stdout, ensure_ascii=False, indent=2)
        sys.stdout.write("\n")
        return 0
    print(f"source of truth: {summary['source_of_truth']}")
    print(f"source files: {summary['source_files']}")
    counts = summary["classification_source_counts"]
    print(
        "classification: component {0} / contract {1} / override {2} / unclassified {3}"
        " (overrides defined: {4})".format(
            counts[KIND_COMPONENT],
            counts[KIND_CONTRACT],
            counts[KIND_OVERRIDE],
            counts[KIND_UNCLASSIFIED],
            summary["override_count"],
        )
    )
    print(f"include edges: {summary['include_edges_total']} (unresolved {summary['unresolved']})")
    print(f"upward edges: {summary['upward_edges_total']} in {summary['upward_files_total']} files")
    print()
    print("| from | to | edges | files |")
    print("|---|---|---:|---:|")
    for pair in summary["pairs"]:
        print(f"| {pair['from']} | {pair['to']} | {pair['edges']} | {pair['files']} |")
    print()
    print("top files:")
    for item in summary["top_files"]:
        print(f"  {item['upward_edges']:4d}  {item['file']}")
    return 0


def cmd_check_derivation(args: argparse.Namespace) -> int:
    model = _model_from_args(args)
    result = check_derivation(model)
    print(f"layer map: {_rel(model['_map_path'])}")
    print(f"source of truth: {model.get('source_of_truth')}")
    print(f"overrides: {len(model['_overrides'])}")
    print()
    print(f"errors: {len(result['errors'])}")
    for line in result["errors"]:
        print(f"  ERROR {line}")
    print(f"warnings: {len(result['warnings'])}")
    for line in result["warnings"]:
        print(f"  WARN  {line}")
    print(f"manifest_upward_edges: {len(result['manifest_upward_edges'])}")
    for item in result["manifest_upward_edges"]:
        print(
            "  UP    {0} (rank {1}) -> {2} (rank {3}) [{4}]".format(
                item["from"], item["from_rank"], item["to"], item["to_rank"], item["kind"]
            )
        )
    failed = bool(result["errors"])
    if args.strict and result["manifest_upward_edges"]:
        failed = True
    print()
    print("result: FAIL" if failed else "result: OK")
    return 1 if failed else 0


def cmd_explain(args: argparse.Namespace) -> int:
    model = _model_from_args(args)
    edges, _unresolved, _stats = collect_edges(model)
    rows = [e for e in edges if e[5] > e[4] and e[2] == args.from_layer and e[3] == args.to_layer]
    rows.sort()
    for src, dst, *_ in rows:
        print(f"{src} -> {dst}")
    print(f"# {len(rows)} upward edges {args.from_layer} -> {args.to_layer}", file=sys.stderr)
    return 0


def cmd_file(args: argparse.Namespace) -> int:
    model = _model_from_args(args)
    rel = Path(args.path).as_posix()
    cls = classify(rel, model)
    if cls is None:
        print(f"{rel}: (excluded)")
        return 0
    layer_id, _rank, kind = cls
    comp_id = owner_of(rel, model)
    detail = f" via {comp_id}" if comp_id else ""
    print(f"{rel}: {layer_id or '(unclassified)'} [{kind}{detail}]")
    edges, _u, _s = collect_edges(model)
    for src, dst, _sl, dl, sr, dr in edges:
        if src == rel:
            mark = "UP" if dr > sr else "  "
            print(f"  {mark} {dst} [{dl}]")
    return 0


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--layer-map", default=str(DEFAULT_MAP))
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    sub = parser.add_subparsers(dest="command", required=True)
    p_report = sub.add_parser("report")
    p_report.add_argument("--json", action="store_true")
    p_report.set_defaults(func=cmd_report)
    p_check = sub.add_parser("check-derivation")
    p_check.add_argument("--strict", action="store_true")
    p_check.set_defaults(func=cmd_check_derivation)
    p_explain = sub.add_parser("explain")
    p_explain.add_argument("--from", dest="from_layer", required=True)
    p_explain.add_argument("--to", dest="to_layer", required=True)
    p_explain.set_defaults(func=cmd_explain)
    p_file = sub.add_parser("file")
    p_file.add_argument("path")
    p_file.set_defaults(func=cmd_file)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
