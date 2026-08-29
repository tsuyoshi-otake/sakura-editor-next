"""Prove staged runtime files match installer and ZIP payloads by SHA-256.

Issue #183: a green compile does not prove the tested bregonig.dll / migemo.dll
is the DLL that packaging ships. This tool hashes the staged files, extracts
packaging outputs into a fresh directory, and compares the bytes. Issue #260
extends that same fail-closed contract to the two required SENP executables,
and Issue #277 extends it to the three scoped terminal orchestration tools.

It is invoked from build-installer.bat and zipArtifacts.bat. A source-level
script inspection is not a substitute for this comparison.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


RUNTIME_NAMES = (
    "bregonig.dll",
    "migemo.dll",
    "sakura-senp-tool.exe",
    "sakura-senp-host.exe",
    "sakura-tmux.exe",
    "tmux.exe",
    "sakura-harness.exe",
)
CHUNK = 1024 * 1024


class IdentityError(RuntimeError):
    pass


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(CHUNK)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _one_file(root: Path, name: str) -> Path:
    matches = [path for path in root.rglob(name) if path.is_file()]
    if not matches:
        raise IdentityError(f"{name} was not found under {root}")
    if len(matches) > 1:
        found = ", ".join(sorted(path.as_posix() for path in matches))
        raise IdentityError(f"{name} has more than one copy under {root}: {found}")
    return matches[0]


def _hash_named(root: Path) -> dict[str, str]:
    return {name: _sha256_file(_one_file(root, name)) for name in RUNTIME_NAMES}


def _reset_dir(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def _extract_zip(archive: Path, destination: Path) -> None:
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(destination)


def _extract_with_7z(seven_zip: Path, archive: Path, destination: Path) -> None:
    result = subprocess.run(
        [str(seven_zip), "x", str(archive), f"-o{destination}", "-y"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "7z failed").strip()
        raise IdentityError(f"could not extract {archive}: {detail}")


def _innounp_path() -> Path | None:
    bundled = Path(__file__).resolve().parent / "innounp" / "innounp.exe"
    if bundled.is_file():
        return bundled
    found = shutil.which("innounp")
    return Path(found) if found else None


def _extract_installer(archive: Path, destination: Path, seven_zip: Path | None) -> None:
    innounp = _innounp_path()
    if innounp is not None:
        result = subprocess.run(
            [str(innounp), "-x", "-y", "-q", f"-d{destination}", str(archive)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "innounp failed").strip()
            raise IdentityError(f"could not extract {archive}: {detail}")
        return
    if seven_zip is None:
        raise IdentityError(
            "extracting an installer requires tools/innounp/innounp.exe, innounp on PATH, or --seven-zip"
        )
    _extract_with_7z(seven_zip, archive, destination)


def _find_setup_exe(root: Path) -> Path:
    matches = [
        path
        for path in root.rglob("*.exe")
        if path.is_file() and "install" in path.name.lower()
    ]
    if not matches:
        matches = [path for path in root.rglob("*.exe") if path.is_file()]
    if len(matches) != 1:
        found = ", ".join(sorted(path.name for path in matches)) or "(none)"
        raise IdentityError(f"expected one installer exe under {root}, found: {found}")
    return matches[0]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", required=True, type=Path)
    parser.add_argument("--installer-work", type=Path)
    parser.add_argument("--installer-exe", type=Path)
    parser.add_argument("--installer-zip", type=Path)
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--seven-zip", type=Path)
    parser.add_argument("--clean-extract", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args(argv)


def collect(args: argparse.Namespace) -> dict[str, object]:
    staged_dir = args.staged.resolve()
    if not staged_dir.is_dir():
        raise IdentityError(f"staged directory is missing: {staged_dir}")
    sources: dict[str, dict[str, str]] = {"staged": _hash_named(staged_dir)}
    extract_root = _reset_dir(args.clean_extract.resolve())

    if args.installer_work is not None:
        sources["installerWork"] = _hash_named(args.installer_work.resolve())

    if args.zip is not None:
        zip_dir = _reset_dir(extract_root / "zip")
        _extract_zip(args.zip.resolve(), zip_dir)
        sources["zip"] = _hash_named(zip_dir)

    if args.installer_exe is not None or args.installer_zip is not None:
        if args.seven_zip is None and _innounp_path() is None:
            raise IdentityError(
                "extracting an installer requires tools/innounp/innounp.exe, innounp on PATH, or --seven-zip"
            )
        setup = args.installer_exe
        if args.installer_zip is not None:
            installer_zip_dir = _reset_dir(extract_root / "installer-zip")
            _extract_zip(args.installer_zip.resolve(), installer_zip_dir)
            setup = _find_setup_exe(installer_zip_dir)
        assert setup is not None
        payload = _reset_dir(extract_root / "installer-payload")
        _extract_installer(setup.resolve(), payload, args.seven_zip.resolve() if args.seven_zip else None)
        sources["installer"] = _hash_named(payload)

    mismatches: list[str] = []
    staged = sources["staged"]
    for label, hashes in sources.items():
        if label == "staged":
            continue
        for name in RUNTIME_NAMES:
            if hashes[name] != staged[name]:
                mismatches.append(
                    f"{name}: staged {staged[name]} != {label} {hashes[name]}"
                )

    return {
        "ok": not mismatches,
        "mismatches": mismatches,
        "sources": sources,
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = collect(args)
    except IdentityError as error:
        report = {"ok": False, "error": str(error)}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if not report.get("ok"):
        sys.stderr.write(json.dumps(report, indent=2, sort_keys=True) + os.linesep)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
