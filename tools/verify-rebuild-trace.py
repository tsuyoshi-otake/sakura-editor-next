#!/usr/bin/env python3
"""Record which build outputs change when one source file is touched.

The native Rust archive feeds the product and test executables. After touching one
`rust/native` source (modification time only), the archive and both executables must be
rebuilt and nothing unrelated may move; a rebuild that changes nothing must leave every
tracked output alone. This tool takes the snapshots and judges them; the caller runs the
rebuilds. Run it from the repository root.

  snapshot --out FILE --track NAME=PATTERN [--track ...] [--scan DIR ...]
      Record size, modification time and SHA-256 of every tracked output (each pattern must
      match exactly one file) and size and modification time of every .a/.dll/.exe/.lib file
      below the scan directories (CMakeFiles directories are not entered).
  touch PATH
      Advance the modification time of PATH without changing its content.
  report BEFORE NOOP TOUCHED --expect-changed NAMES [--expect-unchanged NAMES]
      Judge three snapshots. Phase 1 (BEFORE -> NOOP, a rebuild after no source change):
      every tracked output must keep its modification time and SHA-256 (the digest covers the
      size). Phase 2 (NOOP -> TOUCHED, a rebuild after the touch): each --expect-changed output
      must have a later modification time (a relink that reproduces identical bytes counts as
      rebuilt), and each --expect-unchanged output must keep its modification time and SHA-256.
      Other tracked outputs are printed without a verdict. The binary scan is informational.

Exit status: 0 expectations met, 1 an expectation was violated, 2 usage or input error.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import sys
from pathlib import Path

NAME_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
SCHEMA_VERSION = 1
SCAN_SUFFIXES = (".a", ".dll", ".exe", ".lib")
IGNORED_DIRECTORIES = frozenset({"CMakeFiles"})
MAX_LISTED = 40
NS_PER_SECOND = 1_000_000_000


class InputError(Exception):
    """A usage or input problem; reported with exit status 2."""


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def posix(path: str) -> str:
    return path.replace("\\", "/")


def resolve_tracked(spec: str) -> tuple[str, str]:
    name, separator, pattern = spec.partition("=")
    if not separator or not name or not pattern:
        raise InputError(f"--track expects NAME=PATTERN, got: {spec}")
    if not NAME_PATTERN.fullmatch(name):
        raise InputError(f"tracked name '{name}' must match {NAME_PATTERN.pattern} (it is listed with commas)")
    matches = sorted(match for match in glob.glob(pattern) if os.path.isfile(match))
    if len(matches) != 1:
        parent = os.path.dirname(pattern) or "."
        listing = ", ".join(sorted(os.listdir(parent))[:20]) if os.path.isdir(parent) else "(directory missing)"
        raise InputError(
            f"tracked output '{name}': pattern {pattern} matches {len(matches)} files, expected exactly one "
            f"(directory holds: {listing})"
        )
    return name, matches[0]


def scan_directory(root: str) -> dict[str, list[int]]:
    if not os.path.isdir(root):
        raise InputError(f"--scan directory does not exist: {root}")
    found: dict[str, list[int]] = {}
    for current, directories, files in os.walk(root):
        directories[:] = sorted(item for item in directories if item not in IGNORED_DIRECTORIES)
        for item in files:
            if item.lower().endswith(SCAN_SUFFIXES):
                full = os.path.join(current, item)
                info = os.stat(full)
                found[posix(full)] = [info.st_size, info.st_mtime_ns]
    return found


def command_snapshot(args: argparse.Namespace) -> int:
    tracked: dict[str, dict[str, object]] = {}
    for spec in args.track:
        name, path = resolve_tracked(spec)
        if name in tracked:
            raise InputError(f"tracked name given twice: {name}")
        info = os.stat(path)
        tracked[name] = {
            "path": posix(path),
            "size": info.st_size,
            "mtime_ns": info.st_mtime_ns,
            "sha256": sha256_of(Path(path)),
        }
    scanned: dict[str, list[int]] = {}
    for root in args.scan or []:
        scanned.update(scan_directory(root))
    document = {"schema": SCHEMA_VERSION, "tracked": tracked, "scan": scanned}
    Path(args.out).write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"snapshot {args.out}: {len(tracked)} tracked outputs, {len(scanned)} scanned files")
    return 0


def command_touch(args: argparse.Namespace) -> int:
    path = Path(args.path)
    if not path.is_file():
        raise InputError(f"touch target is not a file: {args.path}")
    mtime_before = path.stat().st_mtime_ns
    os.utime(path, None)
    mtime_after = path.stat().st_mtime_ns
    if mtime_after <= mtime_before:
        raise InputError(f"touching {args.path} did not advance its modification time")
    print(f"touched {posix(args.path)}: modification time advanced by {(mtime_after - mtime_before) / NS_PER_SECOND:.3f} s")
    return 0


def load_snapshot(path: str) -> dict[str, object]:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise InputError(f"cannot read snapshot {path}: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("schema") != SCHEMA_VERSION
        or not isinstance(document.get("tracked"), dict)
        or not isinstance(document.get("scan"), dict)
    ):
        raise InputError(f"{path} is not a schema {SCHEMA_VERSION} snapshot")
    for name, entry in document["tracked"].items():
        if not isinstance(entry, dict) or not {"path", "size", "mtime_ns", "sha256"} <= set(entry):
            raise InputError(f"{path}: malformed tracked entry '{name}'")
    return document


def differs(before: dict[str, object], after: dict[str, object]) -> bool:
    return any(before[key] != after[key] for key in ("mtime_ns", "sha256"))


def advanced(before: dict[str, object], after: dict[str, object]) -> bool:
    return int(after["mtime_ns"]) > int(before["mtime_ns"])  # type: ignore[call-overload]


def describe(before: dict[str, object], after: dict[str, object]) -> str:
    delta = (int(after["mtime_ns"]) - int(before["mtime_ns"])) / NS_PER_SECOND  # type: ignore[call-overload]
    content = "different" if before["sha256"] != after["sha256"] else "same"
    return (
        f"mtime {delta:+.3f} s, content {content}, "
        f"sha256 {str(before['sha256'])[:12]} -> {str(after['sha256'])[:12]}"
    )


def split_names(text: str) -> list[str]:
    return [item for item in (part.strip() for part in text.split(",")) if item]


def scan_changes(before: dict[str, list[int]], after: dict[str, list[int]]) -> list[str]:
    lines = []
    for path in sorted(set(before) | set(after)):
        if path not in before:
            lines.append(f"+ {path}")
        elif path not in after:
            lines.append(f"- {path}")
        elif before[path] != after[path]:
            lines.append(f"~ {path}")
    return lines


def print_scan(title: str, lines: list[str]) -> None:
    print(f"  scan {title}: {len(lines)} changed .a/.dll/.exe/.lib files")
    for line in lines[:MAX_LISTED]:
        print(f"    {line}")
    if len(lines) > MAX_LISTED:
        print(f"    ... {len(lines) - MAX_LISTED} more")


def command_report(args: argparse.Namespace) -> int:
    before = load_snapshot(args.before)
    noop = load_snapshot(args.noop)
    touched = load_snapshot(args.touched)
    expect_changed = split_names(args.expect_changed)
    expect_unchanged = split_names(args.expect_unchanged)
    if not expect_changed:
        raise InputError("--expect-changed must name at least one tracked output")
    overlap = sorted(set(expect_changed) & set(expect_unchanged))
    if overlap:
        raise InputError(f"named both changed and unchanged: {', '.join(overlap)}")
    names = sorted(before["tracked"])  # type: ignore[arg-type]
    for snapshot, label in ((noop, args.noop), (touched, args.touched)):
        if sorted(snapshot["tracked"]) != names:  # type: ignore[arg-type]
            raise InputError(f"{label} tracks different outputs than {args.before}")
    for name in expect_changed + expect_unchanged:
        if name not in names:
            raise InputError(f"expected output '{name}' is not tracked")

    violations: list[str] = []

    print("phase 1 - rebuild with no source change (before -> no-op)")
    for name in names:
        old, new = before["tracked"][name], noop["tracked"][name]  # type: ignore[index]
        moved = differs(old, new)
        print(f"  {name:<14} {'CHANGED  ' if moved else 'unchanged'} {describe(old, new)}")
        if moved:
            violations.append(f"{name} changed during the rebuild that followed no source change")
    print_scan("no-op", scan_changes(before["scan"], noop["scan"]))  # type: ignore[arg-type]

    print("phase 2 - rebuild after touching one source (no-op -> touched)")
    for name in names:
        old, new = noop["tracked"][name], touched["tracked"][name]  # type: ignore[index]
        if name in expect_changed:
            ok = advanced(old, new)
            verdict = "rebuilt  " if ok else "NOT REBUILT"
            note = "expected rebuilt: " + ("ok" if ok else "violated")
            if not ok:
                violations.append(f"{name} was not rebuilt after the touch")
        elif name in expect_unchanged:
            ok = not differs(old, new)
            verdict = "unchanged" if ok else "CHANGED  "
            note = "expected unchanged: " + ("ok" if ok else "violated")
            if not ok:
                violations.append(f"{name} changed although it does not depend on the touched source")
        else:
            verdict = "changed  " if differs(old, new) else "unchanged"
            note = "no expectation"
        print(f"  {name:<14} {verdict} {describe(old, new)} ({note})")
    print_scan("touched", scan_changes(noop["scan"], touched["scan"]))  # type: ignore[arg-type]

    if violations:
        print(f"rebuild trace: FAIL ({len(violations)} violation{'s' if len(violations) != 1 else ''})")
        for item in violations:
            print(f"  - {item}")
        return 1
    print("rebuild trace: PASS")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    snapshot = commands.add_parser("snapshot", help="record tracked outputs and a scan of binaries")
    snapshot.add_argument("--out", required=True, help="snapshot file to write")
    snapshot.add_argument("--track", action="append", required=True, metavar="NAME=PATTERN")
    snapshot.add_argument("--scan", action="append", metavar="DIR", help="directory scanned for .a/.dll/.exe/.lib files")
    snapshot.set_defaults(handler=command_snapshot)

    touch = commands.add_parser("touch", help="advance a file's modification time, content untouched")
    touch.add_argument("path")
    touch.set_defaults(handler=command_touch)

    report = commands.add_parser("report", help="judge the snapshots")
    report.add_argument("before")
    report.add_argument("noop")
    report.add_argument("touched")
    report.add_argument("--expect-changed", required=True, metavar="NAMES", help="comma separated tracked names")
    report.add_argument("--expect-unchanged", default="", metavar="NAMES", help="comma separated tracked names")
    report.set_defaults(handler=command_report)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        return arguments.handler(arguments)
    except InputError as error:
        print(f"verify-rebuild-trace: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
