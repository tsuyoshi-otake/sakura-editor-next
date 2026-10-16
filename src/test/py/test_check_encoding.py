"""Integration tests for the dependency-free PowerShell encoding checker."""

import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
CHECKER = REPO_ROOT / "src/main/ps1/check-encoding.ps1"
WORKFLOW = REPO_ROOT / ".github/workflows/build-sakura.yml"
BATCH_WRAPPER = REPO_ROOT / "checkEncoding.bat"


def _run_raw(*args: str, cwd: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(CHECKER),
            *args,
        ],
        cwd=cwd,
        capture_output=True,
        check=False,
    )


def _run(*args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    # The checker's redirected output is UTF-8 whatever the console code page
    # is (#326). Decoding here, strictly, names the offending bytes instead of
    # leaving a stream None when a reader thread fails to decode it.
    raw = _run_raw(*args, cwd=cwd)
    return subprocess.CompletedProcess(
        raw.args,
        raw.returncode,
        raw.stdout.decode("utf-8"),
        raw.stderr.decode("utf-8"),
    )


def _git(repository: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=repository, text=True, encoding="utf-8"
    ).strip()


def _initialize_repository(path: Path) -> str:
    _git(path, "init")
    _git(path, "config", "user.email", "encoding-test@example.invalid")
    _git(path, "config", "user.name", "Encoding Test")
    (path / "baseline.cpp").write_bytes(b"int baseline;\r\n")
    _git(path, "add", "baseline.cpp")
    _git(path, "commit", "-m", "baseline")
    return _git(path, "rev-parse", "HEAD")


def _combined_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def test_all_accepts_every_supported_encoding(tmp_path: Path):
    (tmp_path / "ascii.cpp").write_bytes(b"int value;\r\n")
    (tmp_path / "utf8.h").write_bytes(b"\xef\xbb\xbf" + "// 日本語\n".encode())
    (tmp_path / "little.rc").write_bytes("STRINGTABLE\r\n".encode("utf-16"))
    (tmp_path / "big.rc2").write_bytes(
        b"\xfe\xff" + "STRINGTABLE\r\n".encode("utf-16-be")
    )

    result = _run("all", "-RepositoryRoot", str(tmp_path), cwd=tmp_path)

    assert result.returncode == 0, _combined_output(result)
    assert "Encoding check passed for 4 file(s)." in result.stdout


@pytest.mark.parametrize(
    ("name", "contents"),
    [
        ("no-bom.cpp", "// 日本語\n".encode()),
        ("invalid.h", b"\xef\xbb\xbf\xff"),
        ("no-bom.rc", "STRINGTABLE\n".encode("utf-8")),
        ("truncated.rc2", b"\xff\xfeA"),
    ],
)
def test_all_rejects_unsupported_or_malformed_encoding(
    tmp_path: Path, name: str, contents: bytes
):
    (tmp_path / name).write_bytes(contents)

    result = _run("all", "-RepositoryRoot", str(tmp_path), cwd=tmp_path)

    assert result.returncode == 1
    assert f"NG {tmp_path / name}" in result.stderr
    assert "1 file(s) have an unsupported encoding." in result.stderr


def test_diff_checks_changed_targets_and_ignores_deleted_files(tmp_path: Path):
    _initialize_repository(tmp_path)
    deleted = tmp_path / "deleted.h"
    deleted.write_bytes(b"int deleted;\n")
    _git(tmp_path, "add", "deleted.h")
    _git(tmp_path, "commit", "-m", "add deleted target")
    base_sha = _git(tmp_path, "rev-parse", "HEAD")
    deleted.unlink()
    (tmp_path / "baseline.cpp").write_bytes(
        b"\xef\xbb\xbf" + "// 変更\n".encode()
    )
    (tmp_path / "ignored.md").write_text("ignored", encoding="utf-8")
    _git(tmp_path, "add", "baseline.cpp", "ignored.md")
    _git(tmp_path, "commit", "-m", "change tracked files")

    result = _run(
        "diff", "-BaseSha", base_sha, "-RepositoryRoot", str(tmp_path), cwd=tmp_path
    )

    assert result.returncode == 0, _combined_output(result)
    assert "checking baseline.cpp" in result.stdout
    assert "deleted.h" not in result.stdout
    assert "ignored.md" not in result.stdout
    assert "Encoding check passed for 1 file(s)." in result.stdout


def test_all_skips_owned_snapshot_sources(tmp_path: Path):
    owned = tmp_path / "third_party" / "owned" / "vendor"
    owned.mkdir(parents=True)
    (owned / "legacy.cpp").write_bytes("// 日本語\n".encode("cp932"))
    (tmp_path / "product.cpp").write_bytes(b"int ok;\r\n")

    result = _run("all", "-RepositoryRoot", str(tmp_path), cwd=tmp_path)

    assert result.returncode == 0, _combined_output(result)
    assert "Encoding check passed for 1 file(s)." in result.stdout
    assert "legacy.cpp" not in result.stdout


@pytest.mark.parametrize("base_sha", ["", "0" * 40])
def test_diff_treats_empty_or_all_zero_base_as_branch_creation_noop(
    tmp_path: Path, base_sha: str
):
    _initialize_repository(tmp_path)
    (tmp_path / "invalid.cpp").write_bytes("// 日本語\n".encode())

    result = _run(
        "diff", "-BaseSha", base_sha, "-RepositoryRoot", str(tmp_path), cwd=tmp_path
    )

    assert result.returncode == 0, _combined_output(result)
    assert "branch creation has no diff" in result.stdout
    assert "Encoding check passed for 0 file(s)." in result.stdout


def test_diff_rejects_an_invalid_base_commit(tmp_path: Path):
    _initialize_repository(tmp_path)

    result = _run(
        "diff",
        "-BaseSha",
        "not-a-commit",
        "-RepositoryRoot",
        str(tmp_path),
        cwd=tmp_path,
    )

    assert result.returncode != 0
    assert "git rev-parse failed" in _combined_output(result)


def test_an_operational_failure_is_reported_as_plain_prefixed_lines(tmp_path: Path):
    _initialize_repository(tmp_path)

    result = _run_raw(
        "diff",
        "-BaseSha",
        "not-a-commit",
        "-RepositoryRoot",
        str(tmp_path),
        cwd=tmp_path,
    )

    # The checker owns this report. It must not be the host's error view,
    # whose colors, echoed source line and width-truncated ellipsis depend on
    # the terminal and the console code page (#326).
    assert result.returncode == 1
    assert b"\x1b" not in result.stdout + result.stderr
    lines = result.stderr.decode("utf-8").splitlines()
    assert lines, "expected a diagnostic on stderr"
    assert all(line.startswith("check-encoding: ") for line in lines), lines
    assert "git rev-parse failed with exit code 128" in lines[0]


def test_non_ascii_file_names_are_reported_in_utf8(tmp_path: Path):
    accepted = tmp_path / "正常.cpp"
    rejected = tmp_path / "不正.h"
    accepted.write_bytes(b"int ok;\r\n")
    rejected.write_bytes("// 日本語\n".encode())

    result = _run("all", "-RepositoryRoot", str(tmp_path), cwd=tmp_path)

    assert result.returncode == 1
    assert f"OK {accepted}" in result.stdout
    assert f"NG {rejected}" in result.stderr


def test_diff_reads_non_ascii_paths_from_git(tmp_path: Path):
    base_sha = _initialize_repository(tmp_path)
    (tmp_path / "変更.cpp").write_bytes(b"\xef\xbb\xbf" + "// 変更\n".encode())
    _git(tmp_path, "add", "変更.cpp")
    _git(tmp_path, "commit", "-m", "add a non-ASCII path")

    result = _run(
        "diff", "-BaseSha", base_sha, "-RepositoryRoot", str(tmp_path), cwd=tmp_path
    )

    # git writes -z paths as UTF-8 bytes; decoding them with the console code
    # page turns the name into one that does not exist on disk.
    assert result.returncode == 0, _combined_output(result)
    assert "OK 変更.cpp" in result.stdout
    assert "Encoding check passed for 1 file(s)." in result.stdout


def test_batch_wrapper_only_forwards_to_the_powershell_checker():
    wrapper = BATCH_WRAPPER.read_text(encoding="utf-8")

    assert "src\\main\\ps1\\check-encoding.ps1" in wrapper
    assert "%*" in wrapper
    assert "exit /b %ERRORLEVEL%" in wrapper
    assert "python" not in wrapper.lower()


def test_encoding_job_does_not_install_python_or_uv():
    workflow = WORKFLOW.read_text(encoding="utf-8")
    encoding_job = workflow.split("  check-encoding:", 1)[1].split(
        "\n  build-vcpkg-msvc:", 1
    )[0]

    assert "checkEncoding.bat" in encoding_job
    assert "setup-python" not in encoding_job
    assert "setup-uv" not in encoding_job
    assert "uv pip install" not in encoding_job
