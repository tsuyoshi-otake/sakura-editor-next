"""Bootstrap a signed, offline MSYS2 toolchain with an aged version lock.

The lock records an earlier public CI observation of every package version.
The matching bytes are pinned by SHA-256 and must also carry an aged signature
trusted by the pinned MSYS2 base image. Pacman sees no remote repositories.
"""

from __future__ import annotations

import concurrent.futures
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
from pathlib import Path, PurePosixPath


LOCK = Path(__file__).with_name("packages.lock.json")
OBSERVATION = Path(__file__).with_name("package-observation.txt")
OBSERVATION_SHA256 = "19351a02e1ad40f3fc03bab5a2c8bf9d4de43e04bce7cb18a54a092f7a989650"
MINGW_ROOT = "https://repo.msys2.org/mingw/mingw64/"
MSYS_ROOT = "https://repo.msys2.org/msys/x86_64/"
MINIMUM_AGE = timedelta(hours=72)
MAX_DOWNLOAD_BYTES = 128 * 1024 * 1024
DOWNLOAD_DEADLINE_SECONDS = 180
HASH = re.compile(r"[0-9a-f]{64}\Z")
MINGW_PACKAGE = re.compile(r"mingw-w64-x86_64-[a-z0-9][a-z0-9+._~-]*-any\.pkg\.tar\.zst\Z")
MSYS_PACKAGE = re.compile(r"doxygen-[0-9][a-z0-9+._~-]*-x86_64\.pkg\.tar\.zst\Z")


def require_old_timestamp(value: object, now: datetime) -> None:
    if not isinstance(value, str):
        raise ValueError("age evidence must be a UTC timestamp")
    try:
        observed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError as error:
        raise ValueError(f"invalid age evidence: {value!r}") from error
    if now.tzinfo is None or now.utcoffset() is None or now - observed < MINIMUM_AGE:
        raise ValueError(f"age evidence is less than 72 hours old: {value}")


def _no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate lock key: {key}")
        result[key] = value
    return result


def read_lock(path: Path, now: datetime, observation: Path = OBSERVATION) -> dict[str, object]:
    lock = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicates)
    if not isinstance(lock, dict) or set(lock) != {
        "schema_version", "installer", "packages_observed_at", "observation_urls", "packages", "profiles"
    } or type(lock["schema_version"]) is not int or lock["schema_version"] != 1:
        raise ValueError("invalid MSYS2 lock schema")
    installer = lock["installer"]
    if not isinstance(installer, dict) or set(installer) != {"url", "sha256", "published_at"}:
        raise ValueError("invalid installer lock")
    if not isinstance(installer["url"], str) or not re.fullmatch(
        r"https://github\.com/msys2/msys2-installer/releases/download/[0-9-]+/"
        r"msys2-base-x86_64-[0-9]+\.tar\.zst", installer["url"]
    ):
        raise ValueError("installer must be an official versioned MSYS2 release")
    if not isinstance(installer["sha256"], str) or not HASH.fullmatch(installer["sha256"]):
        raise ValueError("installer SHA-256 is invalid")
    require_old_timestamp(installer["published_at"], now)
    require_old_timestamp(lock["packages_observed_at"], now)
    urls = lock["observation_urls"]
    if not isinstance(urls, list) or not urls or any(
        not isinstance(url, str) or not re.fullmatch(
            r"https://github\.com/tsuyoshi-otake/sakura-editor-next/actions/runs/[1-9][0-9]*", url
        ) for url in urls
    ) or len(set(urls)) != len(urls):
        raise ValueError("package observation URLs are invalid")
    packages = lock["packages"]
    if not isinstance(packages, list) or not packages:
        raise ValueError("package lock must not be empty")
    names: set[str] = set()
    for item in packages:
        if not isinstance(item, dict) or set(item) != {"filename", "sha256"}:
            raise ValueError("invalid package entry")
        filename, digest = item["filename"], item["sha256"]
        if not isinstance(filename, str) or not (MINGW_PACKAGE.fullmatch(filename) or MSYS_PACKAGE.fullmatch(filename)):
            raise ValueError(f"invalid package filename: {filename!r}")
        if filename in names:
            raise ValueError(f"duplicate package: {filename}")
        if not isinstance(digest, str) or not HASH.fullmatch(digest):
            raise ValueError(f"invalid package SHA-256: {filename}")
        names.add(filename)
    profiles = lock["profiles"]
    if not isinstance(profiles, dict) or set(profiles) != {"mingw", "cppcheck", "doxygen"}:
        raise ValueError("invalid toolchain profiles")
    included: set[str] = set()
    for profile, selected in profiles.items():
        if not isinstance(selected, list) or not selected or any(not isinstance(name, str) for name in selected):
            raise ValueError(f"invalid package selection: {profile}")
        if len(set(selected)) != len(selected) or not set(selected) <= names:
            raise ValueError(f"duplicate or unknown package in profile: {profile}")
        included.update(selected)
    if included != names:
        raise ValueError("package lock contains unused packages")
    validate_observation(lock, observation)
    return lock


def validate_observation(lock: dict[str, object], path: Path) -> None:
    # Git may check text files out with CRLF on Windows; fingerprint the
    # canonical LF representation without weakening the content check.
    content = path.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(content).hexdigest() != OBSERVATION_SHA256:
        raise ValueError("package observation fingerprint changed")
    observed_versions: set[str] = set()
    timestamps: list[datetime] = []
    for line in content.decode("utf-8").splitlines():
        match = re.fullmatch(r"(\S+Z) Packages \(([0-9]+)\) (.+)", line)
        if not match:
            raise ValueError("malformed package observation")
        versions = match.group(3).split()
        if len(versions) != int(match.group(2)):
            raise ValueError("package observation count differs")
        timestamps.append(datetime.fromisoformat(match.group(1).replace("Z", "+00:00")))
        observed_versions.update(versions)
    recorded = datetime.strptime(lock["packages_observed_at"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    if not timestamps or max(timestamps) > recorded:
        raise ValueError("package observation occurred after the recorded time")
    for item in lock["packages"]:
        filename = item["filename"]
        suffix = "-any.pkg.tar.zst" if MINGW_PACKAGE.fullmatch(filename) else "-x86_64.pkg.tar.zst"
        if filename[:-len(suffix)] not in observed_versions:
            raise ValueError(f"package version has no matching historical observation: {filename}")


def _retry_after(error: urllib.error.HTTPError) -> float:
    value = error.headers.get("Retry-After")
    if not value:
        return 0.0
    try:
        seconds = float(value)
    except ValueError:
        try:
            date = parsedate_to_datetime(value)
            if date.tzinfo is None or date.utcoffset() is None:
                raise ValueError("Retry-After date must include a timezone")
            seconds = (date - datetime.now(timezone.utc)).total_seconds()
        except (TypeError, ValueError, OverflowError) as cause:
            raise ValueError("invalid Retry-After header") from cause
    if not math.isfinite(seconds):
        raise ValueError("invalid Retry-After header")
    return max(0.0, seconds)


def download(url: str, destination: Path, digest: str | None = None) -> None:
    last_error: Exception | None = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=45) as response:
                deadline = time.monotonic() + DOWNLOAD_DEADLINE_SECONDS
                chunks = []
                size = 0
                while True:
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"download exceeded {DOWNLOAD_DEADLINE_SECONDS}s")
                    chunk = response.read1(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > MAX_DOWNLOAD_BYTES:
                        raise ValueError(f"download exceeds {MAX_DOWNLOAD_BYTES} bytes")
                    chunks.append(chunk)
                content = b"".join(chunks)
            if digest is not None and hashlib.sha256(content).hexdigest() != digest:
                raise ValueError(f"SHA-256 mismatch for {destination.name}")
            destination.write_bytes(content)
            return
        except (OSError, urllib.error.URLError) as error:
            last_error = error
            if attempt == 2:
                break
            delay = (2 ** attempt) + random.random()
            if isinstance(error, urllib.error.HTTPError):
                delay = max(delay, _retry_after(error))
            if delay > 30:
                break  # Fail instead of retrying before a longer provider window.
            time.sleep(delay)
    raise RuntimeError(f"could not download {url}: {last_error}") from last_error


def run(command: list[str], *, timeout: int) -> str:
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process.communicate()
        raise RuntimeError(f"command timed out after {timeout}s: {command[0]}") from error
    text = output.decode("utf-8", errors="replace")
    if process.returncode:
        raise RuntimeError(f"command failed ({process.returncode}): {command[0]}\n{text[-8000:]}")
    return text


def validate_archive_listing(listing: str) -> None:
    names = listing.splitlines()
    if not names:
        raise ValueError("empty installer archive")
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name or not path.parts or path.parts[0] != "msys64":
            raise ValueError(f"unsafe installer archive entry: {name!r}")


BOOTSTRAP = r"""#!/usr/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
trap 'gpgconf --homedir /etc/pacman.d/gnupg --kill gpg-agent || true' EXIT
pacman-key --init
pacman-key --populate msys2
now=$(date +%s)
count=0
declare -A seen_names=()
for package in /var/cache/sakura-packages/*.pkg.tar.zst; do
  status=$(gpg --batch --status-fd 1 --homedir /etc/pacman.d/gnupg --verify "$package.sig" "$package" 2>/dev/null) || { printf 'signature verification failed: %s\n' "$package"; exit 1; }
  stamp=$(printf '%s\n' "$status" | awk '/^\[GNUPG:\] VALIDSIG / {print $5}')
  [[ "$stamp" =~ ^[0-9]+$ ]] || { printf 'signature timestamp missing: %s\n' "$package"; exit 1; }
  printf '%s\n' "$status" | grep -Eq '^\[GNUPG:\] TRUST_(FULLY|ULTIMATE) ' || { printf 'untrusted signature: %s\n' "$package"; exit 1; }
  (( now - stamp >= 259200 )) || { printf 'signature younger than 72 hours: %s\n' "$package"; exit 1; }
  metadata=$(bsdtar -xOf "$package" .PKGINFO)
  name=$(printf '%s\n' "$metadata" | awk '$1 == "pkgname" && $2 == "=" {print $3}')
  version=$(printf '%s\n' "$metadata" | awk '$1 == "pkgver" && $2 == "=" {print $3}')
  arch=$(printf '%s\n' "$metadata" | awk '$1 == "arch" && $2 == "=" {print $3}')
  [[ "$arch" == any || "$arch" == x86_64 ]] || { printf 'unsupported package architecture: %s\n' "$package"; exit 1; }
  [[ "$(basename "$package")" == "$name-$version-$arch.pkg.tar.zst" ]] || { printf 'package metadata mismatch: %s\n' "$package"; exit 1; }
  [[ -z "${seen_names[$name]+present}" ]] || { printf 'duplicate package name: %s\n' "$name"; exit 1; }
  seen_names[$name]=1
  count=$((count + 1))
done
[[ "$count" -eq "$1" ]] || { printf 'unexpected package count\n'; exit 1; }
pacman --config /etc/sakura-offline.conf --noconfirm --upgrade /var/cache/sakura-packages/*.pkg.tar.zst
"""


def main() -> int:
    now = datetime.now(timezone.utc)
    lock = read_lock(LOCK, now)  # Reject young or malformed evidence before any download.
    profile = os.environ.get("SAKURA_MSYS2_PROFILE", "mingw")
    if profile not in lock["profiles"]:
        raise ValueError(f"unknown MSYS2 profile: {profile}")
    selection = set(lock["profiles"][profile])
    selected_packages = [item for item in lock["packages"] if item["filename"] in selection]
    runner_temp = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    work = runner_temp / "sakura-locked-msys2"
    work.mkdir(exist_ok=False)
    try:
        installer = lock["installer"]
        archive = work / "msys2-base.tar.zst"
        download(installer["url"], archive, installer["sha256"])
        listing = run(["tar", "-tf", str(archive)], timeout=120)
        validate_archive_listing(listing)
        run(["tar", "-xf", str(archive), "-C", str(work)], timeout=240)
        image = work / "msys64"
        bash = image / "usr/bin/bash.exe"
        if not bash.is_file():
            raise RuntimeError("installer did not contain the expected MSYS2 shell")
        packages = image / "var/cache/sakura-packages"
        packages.mkdir(exist_ok=False)

        def fetch(item: dict[str, str]) -> None:
            filename = item["filename"]
            root = MINGW_ROOT if MINGW_PACKAGE.fullmatch(filename) else MSYS_ROOT
            download(root + filename, packages / filename, item["sha256"])
            download(root + filename + ".sig", packages / (filename + ".sig"))

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(fetch, selected_packages))
        expected = {item["filename"] for item in selected_packages}
        actual = {path.name for path in packages.iterdir()}
        if actual != expected | {name + ".sig" for name in expected}:
            raise RuntimeError("downloaded package set differs from the lock")
        (image / "etc/sakura-offline.conf").write_text(
            "[options]\nArchitecture = auto\nSigLevel = Required\nLocalFileSigLevel = Required\n",
            encoding="ascii", newline="\n",
        )
        (image / "etc/sakura-bootstrap.sh").write_text(BOOTSTRAP, encoding="ascii", newline="\n")
        print(run([str(bash), "--noprofile", "--norc", "/etc/sakura-bootstrap.sh", str(len(expected))], timeout=600))

        wrapper_dir = work / "wrapper"
        wrapper_dir.mkdir()
        wrapper = wrapper_dir / "msys2.cmd"
        wrapper.write_text(
            '@echo off\r\nsetlocal\r\n'
            'IF NOT DEFINED MSYSTEM set MSYSTEM=MINGW64\r\n'
            'IF NOT DEFINED MSYS2_PATH_TYPE set MSYS2_PATH_TYPE=minimal\r\n'
            'set CHERE_INVOKING=1\r\n'
            f'"{bash}" -leo pipefail %*\r\n', encoding="ascii", newline="",
        )
        with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8", newline="\n") as stream:
            stream.write(str(wrapper_dir) + "\n")
        with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8", newline="\n") as stream:
            stream.write("MSYSTEM=MINGW64\nMSYS2_PATH_TYPE=minimal\n")
        print(f"Installed {len(expected)} verified MSYS2 packages without remote dependency resolution")
        return 0
    except Exception:
        # An externally killed or timed-out bootstrap may bypass the shell trap.
        # Stop only the agent belonging to this isolated image before removing it.
        bash = work / "msys64/usr/bin/bash.exe"
        if bash.is_file():
            try:
                run([str(bash), "--noprofile", "--norc", "-c",
                     "export PATH=/usr/bin:/bin; gpgconf --homedir /etc/pacman.d/gnupg --kill gpg-agent"],
                    timeout=15)
            except Exception as cleanup_error:
                print(f"Could not stop the isolated GPG agent: {cleanup_error}", file=sys.stderr)
        if work.resolve().is_relative_to(runner_temp) and work.name == "sakura-locked-msys2":
            shutil.rmtree(work, ignore_errors=False)
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"MSYS2 setup failed: {error}", file=sys.stderr)
        raise SystemExit(1)
