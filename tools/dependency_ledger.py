#!/usr/bin/env python3
"""Validate the Issue #184 dependency ledger and generate NOTICE / SPDX SBOM.

The ledger is the source of truth. Markdown tables are not. Unknown enum
values fail closed. A flat ``class`` field is rejected rather than aliased.
This tool does not use the network.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import owned_snapshot

REPO_ROOT = TOOLS_DIR.parent
LEDGER_PATH = REPO_ROOT / "src/main/dependencies/dependencies.json"
SCHEMA_PATH = REPO_ROOT / "src/main/dependencies/schema.json"
NOTICE_PATH = REPO_ROOT / "src/main/dependencies/NOTICE"
SBOM_PATH = REPO_ROOT / "src/main/dependencies/sbom.spdx.json"
OWNED_ROOT = REPO_ROOT / "third_party/owned"
GITMODULES = REPO_ROOT / ".gitmodules"
VCPKG_MANIFEST = REPO_ROOT / "vcpkg.json"
LOCAL_PORTS = REPO_ROOT / "tools/vcpkg-local-registry/ports"
MODULES = REPO_ROOT / "src/main/modules/modules.json"
SAKURA_CORE = REPO_ROOT / "sakura_core"
ORACLE_PATH = REPO_ROOT / "installer/externals/bregonig/ORACLE.json"
INNOUNP_PIN = REPO_ROOT / "tools/innounp/PIN.json"

KIND = frozenset({"library", "tool", "process", "data", "fixture"})
OWNERSHIP = frozenset({"owned-fork", "owned-implementation", "owned-snapshot", "external"})
LIFECYCLE = frozenset({"keep", "temporary-migration", "remove"})
STATUS = frozenset({"present", "removed"})
SCOPE = frozenset({"configure", "restore", "build", "test", "runtime", "distribution"})
FORBIDDEN_CLASS_ALIASES = frozenset(
    {"class", "absorb", "vendor-compat", "isolated-tool", "isolated-artifact", "test-fixture"}
)
GITMODULE_PATH_RE = re.compile(r"^\s*path\s*=\s*(.+)$", re.MULTILINE)
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
CREATED_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")


class LedgerError(Exception):
    """Fail-closed ledger violation."""


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_ledger(path: Path = LEDGER_PATH) -> dict[str, Any]:
    document = _read_json(path)
    if not isinstance(document, dict):
        raise LedgerError(f"{path} must be a JSON object")
    return document


def load_schema(path: Path = SCHEMA_PATH) -> dict[str, Any]:
    schema = _read_json(path)
    if not isinstance(schema, dict):
        raise LedgerError(f"{path} must be a JSON object")
    return schema


def _dependency_allowed_keys(schema: dict[str, Any]) -> frozenset[str]:
    properties = schema["$defs"]["dependency"]["properties"]
    return frozenset(str(key) for key in properties)


def validate_document(document: dict[str, Any], schema: dict[str, Any]) -> None:
    errors: list[str] = []
    if document.get("schemaVersion") != 1:
        errors.append("schemaVersion must be 1")
    created = document.get("created")
    if not isinstance(created, str) or not CREATED_RE.fullmatch(created):
        errors.append("created must be a UTC timestamp of the form YYYY-MM-DDTHH:MM:SSZ")
    if not str(document.get("policy") or "").strip():
        errors.append("policy must be a non-empty string")
    extra_top = set(document) - {"schemaVersion", "created", "policy", "dependencies"}
    if extra_top:
        errors.append("unknown top-level keys: " + ", ".join(sorted(extra_top)))
    dependencies = document.get("dependencies")
    if not isinstance(dependencies, list) or not dependencies:
        errors.append("dependencies must be a non-empty array")
        raise LedgerError("; ".join(errors))

    allowed = _dependency_allowed_keys(schema)
    seen_ids: set[str] = set()
    for index, entry in enumerate(dependencies):
        prefix = f"dependencies[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{prefix} must be an object")
            continue
        for alias in FORBIDDEN_CLASS_ALIASES:
            if alias == "class" and alias in entry:
                errors.append(f"{prefix} uses forbidden field 'class'; use orthogonal enums")
            elif alias in entry and alias != "class":
                errors.append(f"{prefix} uses retired class alias {alias!r}")
        extra = set(entry) - allowed
        if extra:
            errors.append(f"{prefix} unknown fields: " + ", ".join(sorted(extra)))
        entry_id = entry.get("id")
        if not isinstance(entry_id, str) or not entry_id:
            errors.append(f"{prefix} id is required")
        elif entry_id in seen_ids:
            errors.append(f"duplicate id {entry_id!r}")
        else:
            seen_ids.add(entry_id)
            prefix = entry_id
        for field, allowed_values in (
            ("kind", KIND),
            ("ownership", OWNERSHIP),
            ("lifecycle", LIFECYCLE),
            ("status", STATUS),
        ):
            value = entry.get(field)
            if value not in allowed_values:
                errors.append(f"{prefix}.{field}={value!r} is not in {sorted(allowed_values)}")
        scope = entry.get("scope")
        if not isinstance(scope, list) or not scope:
            errors.append(f"{prefix}.scope must be a non-empty array")
        else:
            if len(scope) != len(set(scope)):
                errors.append(f"{prefix}.scope must not contain duplicates")
            for item in scope:
                if item not in SCOPE:
                    errors.append(f"{prefix}.scope contains unknown value {item!r}")
        planned = entry.get("plannedEndState")
        if planned is not None and planned not in OWNERSHIP:
            errors.append(f"{prefix}.plannedEndState={planned!r} is not an ownership value")
        revision = entry.get("sourceRevision")
        if revision is not None and not (isinstance(revision, str) and GIT_SHA_RE.fullmatch(revision)):
            errors.append(f"{prefix}.sourceRevision must be a 40-character git SHA")
        for artifact in entry.get("artifacts") or []:
            digest = artifact.get("sha256")
            if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
                errors.append(f"{prefix} artifact sha256 is invalid")
        if entry.get("ownership") == "owned-snapshot" and entry.get("snapshot"):
            if entry.get("localModificationAllowed") is not False:
                errors.append(
                    f"{prefix} owned snapshot under third_party/owned must set "
                    "localModificationAllowed to false"
                )
    if errors:
        raise LedgerError("; ".join(errors))


def present_entries(document: dict[str, Any]) -> list[dict[str, Any]]:
    return [entry for entry in document["dependencies"] if entry.get("status") == "present"]


def _gitmodules_paths() -> set[str]:
    text = GITMODULES.read_text(encoding="utf-8-sig")
    return {path.strip().replace("\\", "/") for path in GITMODULE_PATH_RE.findall(text)}


def _vcpkg_names() -> set[str]:
    names: set[str] = set()
    for item in _read_json(VCPKG_MANIFEST)["dependencies"]:
        if isinstance(item, str):
            names.add(item)
        else:
            names.add(str(item["name"]))
    return names


def _local_port_licenses() -> dict[str, str]:
    licenses: dict[str, str] = {}
    for portfile in sorted(LOCAL_PORTS.glob("*/vcpkg.json")):
        document = _read_json(portfile)
        name = str(document["name"])
        license_id = document.get("license")
        if isinstance(license_id, str):
            licenses[name] = license_id
    return licenses


def _package_set_inputs() -> list[str]:
    document = _read_json(MODULES)
    for artifact in document["artifacts"]:
        if artifact.get("id") == "root-vcpkg-package-set":
            return [str(item).replace("\\", "/") for item in artifact["inputs"]]
    raise LedgerError("root-vcpkg-package-set is missing from modules.json")


def _git_ls_tree_commit(path: str) -> str:
    git = shutil_which_git()
    result = subprocess.run(
        [git, "-C", str(REPO_ROOT), "ls-tree", "HEAD", "--", path],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise LedgerError(f"git ls-tree failed for {path}: {(result.stderr or result.stdout).strip()}")
    line = (result.stdout or "").strip()
    if not line:
        raise LedgerError(f"git ls-tree returned no entry for {path}")
    parts = line.split()
    if len(parts) < 3 or parts[1] != "commit" or not GIT_SHA_RE.fullmatch(parts[2]):
        raise LedgerError(f"{path} is not a gitlink commit: {line!r}")
    return parts[2]


def shutil_which_git() -> str:
    import shutil

    git = shutil.which("git")
    if git is None:
        raise LedgerError("git is required to verify sourceRevision gitlinks")
    return git


def _gitlink_path(entry: dict[str, Any]) -> str | None:
    if entry.get("sourceRevision") is None:
        return None
    for key in ("maintainerCheckout", "source"):
        value = entry.get(key)
        if isinstance(value, str) and value and not value.endswith((".zip", ".exe", ".dll")):
            return value.replace("\\", "/")
    return None


def check_coverage(document: dict[str, Any]) -> None:
    errors: list[str] = []
    present = present_entries(document)
    listed_checkouts = {
        str(entry[key]).replace("\\", "/")
        for entry in present
        for key in ("source", "maintainerCheckout")
        if key in entry
    }
    for path in sorted(_gitmodules_paths()):
        if path not in listed_checkouts:
            errors.append(f"git submodule {path} has no present ledger row")
    listed_vcpkg = {str(entry["vcpkg"]) for entry in present if "vcpkg" in entry}
    for name in sorted(_vcpkg_names()):
        if name not in listed_vcpkg:
            errors.append(f"vcpkg.json names {name} but the ledger does not")
    port_licenses = _local_port_licenses()
    for name, license_id in sorted(port_licenses.items()):
        matches = [entry for entry in present if entry.get("vcpkg") == name]
        if not matches:
            errors.append(f"local port {name} has no present ledger row")
            continue
        declared = matches[0].get("license")
        if declared != license_id:
            errors.append(
                f"{matches[0]['id']} license {declared!r} does not match local port {license_id!r}"
            )
    listed_prefixes = listed_checkouts
    for relative in _package_set_inputs():
        if relative in {"vcpkg.json", "vcpkg-configuration.json"}:
            continue
        if any(
            relative == prefix or relative.startswith(prefix.rstrip("/") + "/")
            or prefix.startswith(relative.rstrip("/") + "/")
            for prefix in listed_prefixes
        ):
            continue
        if relative.startswith("tools/vcpkg-local-registry"):
            continue
        if relative.startswith("tools/dll_plugin1") or relative.startswith("tools/ppa_stub"):
            continue
        errors.append(f"package-set input {relative} is not classified in the ledger")
    for entry in present:
        git_path = _gitlink_path(entry)
        revision = entry.get("sourceRevision")
        if git_path is None or revision is None:
            continue
        actual = _git_ls_tree_commit(git_path)
        if actual != revision:
            errors.append(
                f"{entry['id']} sourceRevision {revision} does not match gitlink {actual} at {git_path}"
            )
    if errors:
        raise LedgerError("; ".join(errors))


def check_artifacts(document: dict[str, Any]) -> None:
    errors: list[str] = []
    providers: dict[str, str] = {}
    for entry in present_entries(document):
        for artifact in entry.get("artifacts") or []:
            relative = str(artifact["path"]).replace("\\", "/")
            path = REPO_ROOT / relative
            if not path.is_file():
                errors.append(f"{entry['id']} artifact is missing: {relative}")
                continue
            digest = _sha256_file(path)
            if digest != artifact["sha256"]:
                errors.append(
                    f"{entry['id']} {relative} sha256 {digest} does not match the ledger"
                )
            size = artifact.get("bytes")
            if isinstance(size, int) and path.stat().st_size != size:
                errors.append(f"{entry['id']} {relative} byte length does not match the ledger")
        for runtime in entry.get("runtimeArtifacts") or []:
            name = str(runtime["name"])
            if not runtime.get("productProvider"):
                continue
            owner = providers.get(name)
            if owner is not None:
                errors.append(
                    f"runtime artifact {name} has two product providers: {owner} and {entry['id']}"
                )
            providers[name] = str(entry["id"])
    for required in ("bregonig.dll", "migemo.dll"):
        if required not in providers:
            errors.append(f"runtime artifact {required} has no product provider")
    oracle = _read_json(ORACLE_PATH)
    bron = next(
        artifact
        for entry in document["dependencies"] if entry["id"] == "bron420-zip"
        for artifact in entry["artifacts"]
    )
    if oracle.get("productProvider") is not False:
        errors.append("ORACLE.json must set productProvider to false")
    if oracle.get("sha256") != bron["sha256"]:
        errors.append("ORACLE.json sha256 does not match the bron420-zip ledger row")
    pin = _read_json(INNOUNP_PIN)
    innounp = next(
        artifact
        for entry in document["dependencies"] if entry["id"] == "innounp"
        for artifact in entry["artifacts"]
    )
    if pin.get("productProvider") is not False:
        errors.append("innounp PIN.json must set productProvider to false")
    if pin.get("sha256") != innounp["sha256"]:
        errors.append("innounp PIN.json sha256 does not match the ledger row")
    if errors:
        raise LedgerError("; ".join(errors))


def check_owned_snapshots(document: dict[str, Any]) -> None:
    errors: list[str] = []
    declared: dict[str, dict[str, Any]] = {}
    for entry in present_entries(document):
        snapshot = entry.get("snapshot")
        if not isinstance(snapshot, str) or not snapshot:
            continue
        relative = snapshot.replace("\\", "/")
        if not relative.startswith("third_party/owned/"):
            errors.append(f"{entry['id']} snapshot must live under third_party/owned/")
            continue
        declared[relative] = entry
        try:
            owned_snapshot.verify_snapshot(REPO_ROOT / relative)
        except ValueError as exc:
            errors.append(str(exc))
        if entry.get("localModificationAllowed") is not False:
            errors.append(f"{entry['id']} forbids in-place snapshot edits")
    if OWNED_ROOT.is_dir():
        for child in sorted(path for path in OWNED_ROOT.iterdir() if path.is_dir()):
            relative = child.relative_to(REPO_ROOT).as_posix()
            if relative not in declared:
                errors.append(f"undeclared owned snapshot directory: {relative}")
    if errors:
        raise LedgerError("; ".join(errors))


def _include_claims(document: dict[str, Any]) -> list[tuple[str, str, tuple[str, ...]]]:
    claims: list[tuple[str, str, tuple[str, ...]]] = []
    for entry in present_entries(document):
        for item in entry.get("includePatterns") or []:
            roots = tuple(str(root).replace("\\", "/") for root in item["roots"])
            claims.append((str(entry["id"]), str(item["pattern"]), roots))
    return claims


def check_product_includes(document: dict[str, Any]) -> None:
    errors: list[str] = []
    claims = _include_claims(document)
    if not SAKURA_CORE.is_dir():
        raise LedgerError("sakura_core is missing")
    for path in SAKURA_CORE.rglob("*"):
        if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(REPO_ROOT).as_posix()
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            text = path.read_text(encoding="cp932", errors="replace")
        for include in INCLUDE_RE.findall(text):
            matches = [
                (entry_id, roots)
                for entry_id, pattern, roots in claims
                if include == pattern or include.startswith(pattern)
            ]
            if not matches:
                continue
            allowed = any(
                relative == root.rstrip("/") or relative.startswith(root)
                for _entry_id, roots in matches
                for root in roots
            )
            if not allowed:
                errors.append(
                    f"{relative} includes {include} outside the ledger roots "
                    f"for {sorted({entry_id for entry_id, _roots in matches})}"
                )
    if errors:
        raise LedgerError("; ".join(errors))


def check_ctags_issues(document: dict[str, Any]) -> None:
    errors: list[str] = []
    for entry_id in ("ctags", "diffutils"):
        entry = next((item for item in document["dependencies"] if item["id"] == entry_id), None)
        if entry is None:
            errors.append(f"{entry_id} row is missing")
            continue
        related = set(entry.get("relatedIssues") or [])
        if 125 not in related or 132 not in related:
            errors.append(f"{entry_id} must relate to issues 125 and 132")
    if errors:
        raise LedgerError("; ".join(errors))


def render_notice(document: dict[str, Any]) -> str:
    lines = [
        "Sakura Editor NEXT third-party notices",
        "======================================",
        "",
        "This file is generated from src/main/dependencies/dependencies.json.",
        "Do not edit it by hand. Run: python tools/dependency_ledger.py generate",
        "",
    ]
    for entry in sorted(present_entries(document), key=lambda item: str(item["id"])):
        license_id = entry.get("license")
        if not license_id:
            continue
        lines.append(str(entry["id"]))
        lines.append(
            f"  kind={entry['kind']} ownership={entry['ownership']} "
            f"lifecycle={entry['lifecycle']} status={entry['status']}"
        )
        lines.append(f"  license: {license_id}")
        if entry.get("version"):
            lines.append(f"  version: {entry['version']}")
        if entry.get("canonicalRepository"):
            lines.append(f"  canonical: {entry['canonicalRepository']}")
        if entry.get("source"):
            lines.append(f"  source: {entry['source']}")
        if entry.get("sourceRevision"):
            lines.append(f"  revision: {entry['sourceRevision']}")
        if entry.get("vcpkg"):
            lines.append(f"  vcpkg: {entry['vcpkg']}")
        if entry.get("notes"):
            lines.append(f"  notes: {entry['notes']}")
        lines.append("")
    return "\n".join(lines)


def render_sbom(document: dict[str, Any]) -> str:
    packages = []
    for entry in sorted(present_entries(document), key=lambda item: str(item["id"])):
        license_id = entry.get("license")
        if not license_id:
            continue
        spdx_id = "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]", "-", str(entry["id"]))
        package: dict[str, Any] = {
            "SPDXID": spdx_id,
            "name": entry["id"],
            "licenseDeclared": license_id,
            "downloadLocation": entry.get("canonicalRepository") or "NOASSERTION",
        }
        if entry.get("version"):
            package["versionInfo"] = entry["version"]
        if entry.get("sourceRevision"):
            package["sourceInfo"] = f"git:{entry['sourceRevision']}"
        packages.append(package)
    document_json = {
        "SPDXID": "SPDXRef-DOCUMENT",
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "name": "sakura-editor-next-dependencies",
        "documentNamespace": (
            "https://github.com/tsuyoshi-otake/sakura-editor-next/"
            "src/main/dependencies/sbom.spdx.json"
        ),
        "creationInfo": {
            "created": document["created"],
            "creators": ["Tool: sakura-dependency-ledger-1"],
        },
        "packages": packages,
    }
    return _canonical_json(document_json)


def generate(document: dict[str, Any] | None = None) -> None:
    current = document if document is not None else load_ledger()
    NOTICE_PATH.write_text(render_notice(current), encoding="utf-8", newline="\n")
    SBOM_PATH.write_text(render_sbom(current), encoding="utf-8", newline="\n")


def _normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def check_generated(document: dict[str, Any]) -> None:
    expected_notice = render_notice(document)
    expected_sbom = render_sbom(document)
    if not NOTICE_PATH.is_file() or _normalize_newlines(NOTICE_PATH.read_text(encoding="utf-8")) != expected_notice:
        raise LedgerError("NOTICE is stale; run python tools/dependency_ledger.py generate")
    if not SBOM_PATH.is_file() or _normalize_newlines(SBOM_PATH.read_text(encoding="utf-8")) != expected_sbom:
        raise LedgerError("sbom.spdx.json is stale; run python tools/dependency_ledger.py generate")


def check() -> None:
    schema = load_schema()
    document = load_ledger()
    validate_document(document, schema)
    check_coverage(document)
    check_artifacts(document)
    check_owned_snapshots(document)
    check_product_includes(document)
    check_ctags_issues(document)
    check_generated(document)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", default="check", choices=("check", "generate", "validate"))
    args = parser.parse_args(argv)
    try:
        schema = load_schema()
        document = load_ledger()
        validate_document(document, schema)
        if args.command == "validate":
            return 0
        if args.command == "generate":
            generate(document)
            return 0
        check()
    except LedgerError as exc:
        print(f"dependency ledger: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
