"""Contracts for the rebuild trace tool and the MinGW step that runs it (#441).

The tool decides whether a rebuild did what the dependency graph promises: a rebuild after
no source change moves nothing, and a rebuild after one Rust source is touched rebuilds the
archive and both executables while the language DLLs stay as they were. These tests drive it
with fake build outputs whose modification times are set explicitly, so every verdict is
deterministic and no compiler runs. The workflow pins keep the MinGW step wired to the same
paths, names, order and touched crate that the tool is tested with.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL = REPO_ROOT / "tools/verify-rebuild-trace.py"
WORKFLOW = REPO_ROOT / ".github/workflows/build-on-msys2.yml"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
NATIVE_ROOT = REPO_ROOT / "rust/native"
STEP = "Trace the rebuild after touching a native Rust source"

NS = 1_000_000_000
OLD = 1_000_000_000 * NS
BUILT = 1_700_000_000 * NS
FUTURE = 4_100_000_000 * NS
LATER = 60 * NS

OUTPUTS = {
    "archive": "out/libffi.a",
    "sakura": "out/sakura.exe",
    "tests1": "out/tests1.exe",
    "language": "out/lang.dll",
}
REBUILT = ("archive", "sakura", "tests1")

# The lines of the workflow step that make up the trace, by the prefix that identifies them.
TRACE_EVENTS = (
    ('py -3 tools/verify-rebuild-trace.py snapshot --out "${snapshots}/before.json" ', "snapshot before"),
    ('py -3 tools/verify-rebuild-trace.py snapshot --out "${snapshots}/noop.json" ', "snapshot no-op"),
    ('py -3 tools/verify-rebuild-trace.py snapshot --out "${snapshots}/touched.json" ', "snapshot touched"),
    ("py -3 tools/verify-rebuild-trace.py touch ", "touch"),
    ("py -3 tools/verify-rebuild-trace.py report ", "report"),
)


def _load_tool():
    spec = importlib.util.spec_from_file_location("verify_rebuild_trace", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


trace = _load_tool()


class Build:
    """Fake build outputs below a temporary root, addressed relative to it as the CI step does."""

    def __init__(self, root: Path) -> None:
        self.root = root
        (root / "out").mkdir()
        for name in OUTPUTS:
            self.write(name, f"{name} generation 1\n".encode())

    def path(self, name: str) -> Path:
        return self.root / OUTPUTS[name]

    def stamp(self, name: str, at: int) -> None:
        os.utime(self.path(name), ns=(at, at))

    def write(self, name: str, content: bytes, at: int = BUILT) -> None:
        self.path(name).write_bytes(content)
        self.stamp(name, at)

    def relink(self, names: tuple[str, ...] | list[str], at: int) -> None:
        """Rewrite outputs with the bytes they already have, as a relink that reproduces its inputs does."""
        for name in names:
            self.write(name, self.path(name).read_bytes(), at)

    def track_arguments(self) -> list[str]:
        return [argument for name, relative in OUTPUTS.items() for argument in ("--track", f"{name}={relative}")]

    def snapshot(self, label: str, *extra: str) -> int:
        return trace.main(["snapshot", "--out", f"{label}.json", *self.track_arguments(), "--scan", "out", *extra])

    def report(
        self,
        *,
        changed: str = "archive,sakura,tests1",
        unchanged: str = "language",
        labels: tuple[str, str, str] = ("before", "noop", "touched"),
    ) -> int:
        snapshots = [f"{label}.json" for label in labels]
        return trace.main(["report", *snapshots, "--expect-changed", changed, "--expect-unchanged", unchanged])

    def run_trace(self, *, rebuild_without_change=None, rebuild_after_touch=None, **expectations: str) -> int:
        assert self.snapshot("before") == 0
        if rebuild_without_change is not None:
            rebuild_without_change()
        assert self.snapshot("noop") == 0
        if rebuild_after_touch is not None:
            rebuild_after_touch()
        assert self.snapshot("touched") == 0
        return self.report(**expectations)


@pytest.fixture
def build(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Build:
    monkeypatch.chdir(tmp_path)
    return Build(tmp_path)


def output_lines(capsys: pytest.CaptureFixture[str]) -> list[str]:
    return capsys.readouterr().out.splitlines()


def error_text(capsys: pytest.CaptureFixture[str]) -> str:
    return capsys.readouterr().err


def test_passes_when_the_touch_rebuilds_the_archive_and_both_executables(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    status = build.run_trace(rebuild_after_touch=lambda: build.relink(REBUILT, BUILT + LATER))
    lines = output_lines(capsys)
    assert status == 0
    assert lines[-1] == "rebuild trace: PASS"
    for name in REBUILT:
        pattern = (
            rf"^  {name} +rebuilt +mtime \+60\.000 s, content same, "
            r"sha256 [0-9a-f]{12} -> [0-9a-f]{12} \(expected rebuilt: ok\)$"
        )
        assert any(re.match(pattern, line) for line in lines), name
    assert any(
        re.match(r"^  language +unchanged +mtime \+0\.000 s, content same, .*\(expected unchanged: ok\)$", line)
        for line in lines
    )


def test_a_rebuild_may_change_the_bytes_of_the_outputs_it_owns(build: Build) -> None:
    def rebuild() -> None:
        for name in REBUILT:
            build.write(name, f"{name} generation 2, longer\n".encode(), BUILT + LATER)

    assert build.run_trace(rebuild_after_touch=rebuild) == 0


@pytest.mark.parametrize("missed", REBUILT)
def test_fails_when_an_expected_output_is_not_rebuilt(
    build: Build, capsys: pytest.CaptureFixture[str], missed: str
) -> None:
    names = [name for name in REBUILT if name != missed]
    status = build.run_trace(rebuild_after_touch=lambda: build.relink(names, BUILT + LATER))
    lines = output_lines(capsys)
    assert status == 1
    assert f"  - {missed} was not rebuilt after the touch" in lines
    assert "rebuild trace: FAIL (1 violation)" in lines
    assert any(re.match(rf"^  {missed} +NOT REBUILT ", line) for line in lines)


def test_an_output_whose_modification_time_moved_backwards_is_not_rebuilt(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    def rebuild() -> None:
        build.relink(("sakura", "tests1"), BUILT + LATER)
        build.relink(("archive",), BUILT - LATER)

    assert build.run_trace(rebuild_after_touch=rebuild) == 1
    assert "  - archive was not rebuilt after the touch" in output_lines(capsys)


@pytest.mark.parametrize(
    "how",
    ["new bytes, new time", "same bytes, new time", "new bytes, same size, old time", "new bytes, new size, old time"],
)
def test_fails_when_a_control_moves(build: Build, capsys: pytest.CaptureFixture[str], how: str) -> None:
    def rebuild() -> None:
        build.relink(REBUILT, BUILT + LATER)
        if how == "new bytes, new time":
            build.write("language", b"language generation 2\n", BUILT + LATER)
        elif how == "same bytes, new time":
            build.relink(("language",), BUILT + LATER)
        elif how == "new bytes, same size, old time":
            original = build.path("language").read_bytes()
            build.write("language", b"x" * len(original), BUILT)
        else:
            build.write("language", b"language generation 2, longer\n", BUILT)

    status = build.run_trace(rebuild_after_touch=rebuild)
    assert status == 1
    assert "  - language changed although it does not depend on the touched source" in output_lines(capsys)


@pytest.mark.parametrize("noisy", list(OUTPUTS))
@pytest.mark.parametrize("how", ["same bytes, new time", "new bytes, same size, old time"])
def test_fails_when_a_rebuild_after_no_change_moves_an_output(
    build: Build, capsys: pytest.CaptureFixture[str], noisy: str, how: str
) -> None:
    def rebuild_without_change() -> None:
        if how == "same bytes, new time":
            build.relink((noisy,), BUILT + LATER)
        else:
            build.write(noisy, b"x" * len(build.path(noisy).read_bytes()), BUILT)

    status = build.run_trace(
        rebuild_without_change=rebuild_without_change,
        rebuild_after_touch=lambda: build.relink(REBUILT, BUILT + 2 * LATER),
    )
    lines = output_lines(capsys)
    assert status == 1
    assert "rebuild trace: FAIL (1 violation)" in lines
    assert f"  - {noisy} changed during the rebuild that followed no source change" in lines
    phase_one = lines[: lines.index("phase 2 - rebuild after touching one source (no-op -> touched)")]
    assert any(re.match(rf"^  {noisy} +CHANGED ", line) for line in phase_one)


def test_reports_every_violation_instead_of_stopping_at_the_first(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    status = build.run_trace(
        rebuild_without_change=lambda: build.relink(("sakura",), BUILT + LATER),
        rebuild_after_touch=lambda: build.write("language", b"changed\n", BUILT + 2 * LATER),
    )
    lines = output_lines(capsys)
    assert status == 1
    assert "rebuild trace: FAIL (5 violations)" in lines
    assert lines[lines.index("rebuild trace: FAIL (5 violations)") + 1 :] == [
        "  - sakura changed during the rebuild that followed no source change",
        "  - archive was not rebuilt after the touch",
        "  - language changed although it does not depend on the touched source",
        "  - sakura was not rebuilt after the touch",
        "  - tests1 was not rebuilt after the touch",
    ]


def test_an_output_without_an_expectation_is_printed_and_never_judged_in_the_touched_phase(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    (build.root / "out/extra.bin").write_bytes(b"extra\n")
    os.utime(build.root / "out/extra.bin", ns=(BUILT, BUILT))
    extra = ["--track", "extra=out/extra.bin"]

    def snapshot(label: str) -> None:
        assert trace.main(["snapshot", "--out", f"{label}.json", *build.track_arguments(), *extra, "--scan", "out"]) == 0

    snapshot("before")
    snapshot("noop")
    build.relink(REBUILT, BUILT + LATER)
    (build.root / "out/extra.bin").write_bytes(b"extra, rewritten\n")
    snapshot("touched")
    assert build.report() == 0
    assert any(
        re.match(r"^  extra +changed +.*\(no expectation\)$", line) for line in output_lines(capsys)
    )


def test_the_expectation_of_unchanged_outputs_is_optional(build: Build) -> None:
    def rebuild() -> None:
        build.relink(REBUILT, BUILT + LATER)
        build.write("language", b"language generation 2\n", BUILT + LATER)

    assert build.run_trace(rebuild_after_touch=rebuild, unchanged="") == 0


def test_snapshot_records_size_time_and_digest_of_each_tracked_output(build: Build) -> None:
    assert build.snapshot("before") == 0
    document = json.loads((build.root / "before.json").read_text(encoding="utf-8"))
    content = build.path("sakura").read_bytes()
    assert document["schema"] == 1
    assert sorted(document["tracked"]) == sorted(OUTPUTS)
    assert document["tracked"]["sakura"] == {
        "path": "out/sakura.exe",
        "size": len(content),
        "mtime_ns": BUILT,
        "sha256": hashlib.sha256(content).hexdigest(),
    }
    assert document["scan"] == {
        "out/lang.dll": [len(build.path("language").read_bytes()), BUILT],
        "out/libffi.a": [len(build.path("archive").read_bytes()), BUILT],
        "out/sakura.exe": [len(content), BUILT],
        "out/tests1.exe": [len(build.path("tests1").read_bytes()), BUILT],
    }


def test_the_binary_scan_lists_changes_but_never_decides(build: Build, capsys: pytest.CaptureFixture[str]) -> None:
    sub = build.root / "out/sub"
    (sub / "CMakeFiles").mkdir(parents=True)
    inputs = ("out/old.exe", "out/import.lib", "out/sub/lib.a", "out/sub/CMakeFiles/object.a", "out/notes.txt")
    for relative in inputs:
        (build.root / relative).write_bytes(b"generation 1\n")
        os.utime(build.root / relative, ns=(BUILT, BUILT))

    def rebuild() -> None:
        build.relink(REBUILT, BUILT + LATER)
        (build.root / "out/old.exe").unlink()
        (build.root / "out/new.dll").write_bytes(b"new\n")
        for relative in inputs[1:]:
            (build.root / relative).write_bytes(b"generation 2, longer\n")

    assert build.run_trace(rebuild_after_touch=rebuild) == 0
    lines = output_lines(capsys)
    assert "  scan no-op: 0 changed .a/.dll/.exe/.lib files" in lines
    start = lines.index("  scan touched: 7 changed .a/.dll/.exe/.lib files")
    assert lines[start + 1 : start + 8] == [
        "    ~ out/import.lib",
        "    ~ out/libffi.a",
        "    + out/new.dll",
        "    - out/old.exe",
        "    ~ out/sakura.exe",
        "    ~ out/sub/lib.a",
        "    ~ out/tests1.exe",
    ]
    assert lines[start + 8] == "rebuild trace: PASS"
    assert not any("object.a" in line or "notes.txt" in line for line in lines)


def test_a_long_scan_listing_is_capped_and_counts_the_rest(build: Build, capsys: pytest.CaptureFixture[str]) -> None:
    def rebuild() -> None:
        build.relink(REBUILT, BUILT + LATER)
        for index in range(45):
            (build.root / f"out/generated{index:02}.dll").write_bytes(b"new\n")

    assert build.run_trace(rebuild_after_touch=rebuild) == 0
    lines = output_lines(capsys)
    start = lines.index("  scan touched: 48 changed .a/.dll/.exe/.lib files")
    assert lines[start + 1 : start + 41] == [f"    + out/generated{index:02}.dll" for index in range(40)]
    assert lines[start + 41] == "    ... 8 more"
    assert lines[start + 42] == "rebuild trace: PASS"


def test_a_changed_binary_in_the_no_change_rebuild_is_listed_without_failing(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    (build.root / "out/helper.dll").write_bytes(b"helper 1\n")

    def rebuild_without_change() -> None:
        (build.root / "out/helper.dll").write_bytes(b"helper 2, longer\n")

    status = build.run_trace(
        rebuild_without_change=rebuild_without_change,
        rebuild_after_touch=lambda: build.relink(REBUILT, BUILT + LATER),
    )
    lines = output_lines(capsys)
    assert status == 0
    assert "  scan no-op: 1 changed .a/.dll/.exe/.lib files" in lines
    assert "    ~ out/helper.dll" in lines


@pytest.mark.parametrize(
    ("arguments", "message"),
    [
        (["--track", "x=out/missing.bin"], "pattern out/missing.bin matches 0 files, expected exactly one"),
        (["--track", "x=out/*.exe"], "pattern out/*.exe matches 2 files, expected exactly one"),
        (["--track", "x=out/sakura.exe", "--track", "x=out/tests1.exe"], "tracked name given twice: x"),
        (["--track", "no-separator"], "--track expects NAME=PATTERN, got: no-separator"),
        (["--track", "=out/sakura.exe"], "--track expects NAME=PATTERN"),
        (["--track", "a,b=out/sakura.exe"], "tracked name 'a,b' must match"),
        (["--track", "x=out/sakura.exe", "--scan", "missing-directory"], "--scan directory does not exist: missing-directory"),
    ],
)
def test_snapshot_rejects_bad_input_without_writing_a_file(
    build: Build, capsys: pytest.CaptureFixture[str], arguments: list[str], message: str
) -> None:
    assert trace.main(["snapshot", "--out", "bad.json", *arguments]) == 2
    assert message in error_text(capsys)
    assert not (build.root / "bad.json").exists()


def test_snapshot_names_what_the_directory_holds_when_a_pattern_misses(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    assert trace.main(["snapshot", "--out", "bad.json", "--track", "x=out/absent.exe"]) == 2
    assert "directory holds: lang.dll, libffi.a, sakura.exe, tests1.exe" in error_text(capsys)


def make_snapshots(build: Build) -> None:
    assert build.run_trace(rebuild_after_touch=lambda: build.relink(REBUILT, BUILT + LATER)) == 0


@pytest.mark.parametrize(
    ("changed", "unchanged", "message"),
    [
        ("archive,missing", "language", "expected output 'missing' is not tracked"),
        ("archive", "language,missing", "expected output 'missing' is not tracked"),
        ("archive,sakura", "sakura,language", "named both changed and unchanged: sakura"),
        ("", "language", "--expect-changed must name at least one tracked output"),
        (" , ", "language", "--expect-changed must name at least one tracked output"),
    ],
)
def test_report_rejects_expectations_it_cannot_judge(
    build: Build, capsys: pytest.CaptureFixture[str], changed: str, unchanged: str, message: str
) -> None:
    make_snapshots(build)
    capsys.readouterr()
    assert build.report(changed=changed, unchanged=unchanged) == 2
    assert message in error_text(capsys)


def test_report_rejects_snapshots_that_track_different_outputs(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    make_snapshots(build)
    arguments = [argument for name in ("archive", "sakura", "tests1") for argument in ("--track", f"{name}={OUTPUTS[name]}")]
    assert trace.main(["snapshot", "--out", "fewer.json", *arguments]) == 0
    capsys.readouterr()
    assert build.report(labels=("before", "fewer", "touched")) == 2
    assert "fewer.json tracks different outputs than before.json" in error_text(capsys)


@pytest.mark.parametrize(
    ("content", "message"),
    [
        (None, "cannot read snapshot broken.json"),
        ("not json", "cannot read snapshot broken.json"),
        ('{"schema": 2, "tracked": {}, "scan": {}}', "broken.json is not a schema 1 snapshot"),
        ('{"schema": 1, "tracked": [], "scan": {}}', "broken.json is not a schema 1 snapshot"),
        ('{"schema": 1, "tracked": {"x": {"path": "p"}}, "scan": {}}', "broken.json: malformed tracked entry 'x'"),
    ],
)
def test_report_rejects_a_snapshot_it_cannot_read(
    build: Build, capsys: pytest.CaptureFixture[str], content: str | None, message: str
) -> None:
    make_snapshots(build)
    capsys.readouterr()
    if content is not None:
        (build.root / "broken.json").write_text(content, encoding="utf-8")
    assert build.report(labels=("before", "broken", "touched")) == 2
    assert message in error_text(capsys)


def test_touch_advances_the_modification_time_and_keeps_the_bytes(
    build: Build, capsys: pytest.CaptureFixture[str]
) -> None:
    before = build.path("sakura").read_bytes()
    assert trace.main(["touch", "out/sakura.exe"]) == 0
    assert build.path("sakura").read_bytes() == before
    assert build.path("sakura").stat().st_mtime_ns > BUILT
    assert re.match(
        r"^touched out/sakura\.exe: modification time advanced by [0-9.]+ s$", capsys.readouterr().out.strip()
    )


def test_touch_refuses_a_file_it_cannot_move_forward(build: Build, capsys: pytest.CaptureFixture[str]) -> None:
    build.stamp("sakura", FUTURE)
    assert trace.main(["touch", "out/sakura.exe"]) == 2
    assert "touching out/sakura.exe did not advance its modification time" in error_text(capsys)


@pytest.mark.parametrize("target", ["out/absent.rs", "out"])
def test_touch_refuses_a_target_that_is_not_a_file(
    build: Build, capsys: pytest.CaptureFixture[str], target: str
) -> None:
    assert trace.main(["touch", target]) == 2
    assert f"touch target is not a file: {target}" in error_text(capsys)


def test_the_command_line_returns_the_verdict_as_its_exit_status(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    files = {name: out / f"{name}.bin" for name in ("archive", "exe", "control")}
    for name, path in files.items():
        path.write_bytes(f"{name} 1\n".encode())
        os.utime(path, ns=(BUILT, BUILT))

    def cli(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOL), *arguments],
            cwd=tmp_path,
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
        )

    tracked = [argument for name in files for argument in ("--track", f"{name}=out/{name}.bin")]
    expectations = ["--expect-changed", "archive,exe", "--expect-unchanged", "control"]
    for label in ("before", "noop"):
        assert cli("snapshot", "--out", f"{label}.json", *tracked).returncode == 0
    for name in ("archive", "exe"):
        files[name].write_bytes(f"{name} 2\n".encode())
        os.utime(files[name], ns=(BUILT + LATER, BUILT + LATER))
    assert cli("snapshot", "--out", "touched.json", *tracked).returncode == 0
    passed = cli("report", "before.json", "noop.json", "touched.json", *expectations)
    assert (passed.returncode, passed.stdout.splitlines()[-1]) == (0, "rebuild trace: PASS")

    files["control"].write_bytes(b"control 2\n")
    assert cli("snapshot", "--out", "moved.json", *tracked).returncode == 0
    failed = cli("report", "before.json", "noop.json", "moved.json", *expectations)
    assert (failed.returncode, failed.stdout.splitlines()[-1]) == (1, "  - control changed although it does not depend on the touched source")

    invalid = cli("snapshot", "--out", "bad.json", "--track", "x=out/absent.bin")
    assert invalid.returncode == 2
    assert invalid.stderr.startswith("verify-rebuild-trace: tracked output 'x'")
    assert invalid.stdout == ""


def workflow_steps() -> list[str]:
    text = WORKFLOW.read_text(encoding="utf-8-sig").replace("\r\n", "\n")
    job = text[text.index("\n  mingw:\n") :]
    return re.split(r"(?m)^      - (?=name:|uses:|run:)", job)[1:]


def workflow_step(name: str) -> str:
    matches = [step for step in workflow_steps() if step.startswith(f"name: {name}\n")]
    assert len(matches) == 1, f"expected exactly one step named {name!r}"
    return matches[0]


def run_body(name: str) -> str:
    _, separator, script = workflow_step(name).partition("        run: |\n")
    assert separator, f"step {name!r} has no block script"
    return textwrap.dedent(script)


def archive_crates() -> set[str]:
    """The crates the native FFI staticlib builds from: itself and its path dependencies, transitively."""

    def path_dependencies(crate: str) -> list[str]:
        manifest = (NATIVE_ROOT / crate / "Cargo.toml").read_text(encoding="utf-8-sig")
        section = re.search(r"(?ms)^\[dependencies\]\s*\n(.*?)(?=^\[|\Z)", manifest)
        if section is None:
            return []
        return [Path(path).name for path in re.findall(r'path\s*=\s*"([^"]+)"', section.group(1))]

    seen: set[str] = set()
    pending = ["sakura_native_ffi"]
    while pending:
        crate = pending.pop()
        if crate not in seen:
            seen.add(crate)
            pending.extend(path_dependencies(crate))
    return seen


def test_the_trace_runs_after_the_gdb_diagnostics_and_before_cleanup() -> None:
    names = [step.partition("\n")[0] for step in workflow_steps() if step.startswith("name: ")]
    gdb = names.index("name: Try gdb backtrace on test failure")
    traced = names.index(f"name: {STEP}")
    cleanup = names.index("name: Clean up test process survivors")
    assert names.index("name: Verify encoded EOL candidate execution") < gdb < traced < cleanup
    assert cleanup == len(names) - 1


def test_the_trace_step_blocks_and_runs_on_every_leg() -> None:
    step = workflow_step(STEP)
    assert not re.search(r"(?m)^        (?:continue-on-error|if):", step)
    assert re.search(r"(?m)^        timeout-minutes: \d+$", step)


def test_the_trace_runs_in_the_environment_of_the_build_step() -> None:
    cargo_path = 'export PATH="$(cygpath --unix "${SAKURA_CARGO_BIN}"):${PATH}"'
    body = run_body(STEP)
    lines = body.splitlines()
    assert body.startswith("set -eo pipefail\n")
    assert cargo_path in lines
    assert cargo_path in run_body("Build with MinGW-w64-gcc").splitlines()
    assert "MSYS2_PATH_TYPE" not in workflow_step(STEP)
    # A build resolves cargo from the PATH it starts with, so the export comes before the first one.
    assert lines.index(cargo_path) < lines.index("run_build")


def test_the_trace_takes_each_snapshot_around_the_rebuild_it_describes() -> None:
    body = run_body(STEP)
    lines = body.splitlines()
    build_command = "./build-gnu.bat ${{ env.BuildPlatform }} ${{ env.Configuration }} --no-test"
    assert build_command in run_body("Build with MinGW-w64-gcc").splitlines()
    # The step defines the build once, as the command the build step runs, and calls it twice.
    start = lines.index("run_build() {")
    assert lines[start : start + 3] == ["run_build() {", f"  {build_command}", "}"]
    assert body.count("./build-gnu.bat") == 1

    def event(line: str) -> str | None:
        line = line.strip()
        if line == "run_build":
            return "build"
        return next((name for prefix, name in TRACE_EVENTS if line.startswith(prefix)), None)

    sequence = [name for name in map(event, lines) if name is not None]
    assert sequence == ["snapshot before", "build", "snapshot no-op", "touch", "build", "snapshot touched", "report"]
    assert body.count("verify-rebuild-trace.py snapshot") == 3
    joined = body.replace("\\\n", " ")
    assert re.search(
        r'verify-rebuild-trace\.py report +"\$\{snapshots\}/before\.json" +"\$\{snapshots\}/noop\.json" '
        r'+"\$\{snapshots\}/touched\.json" +--expect-changed ',
        joined,
    )


def test_the_touched_file_feeds_the_archive() -> None:
    touched = re.search(r"verify-rebuild-trace\.py touch (\S+)", run_body(STEP))
    assert touched is not None
    path = REPO_ROOT / touched.group(1)
    assert path.is_file()
    assert path.suffix == ".rs"
    assert path.relative_to(NATIVE_ROOT).parts[0] in archive_crates()


def test_the_step_tracks_the_outputs_the_other_steps_name() -> None:
    body = run_body(STEP)
    tracked = dict(re.findall(r'--track "([A-Za-z0-9._-]+)=([^"]+)"', body))
    assert set(tracked) == {"archive", "sakura", "tests1", "language-en-US", "language-zh-CN"}
    archive = re.search(r'^archive="([^"]+)"$', run_body("Verify native Rust C ABI exports (GNU nm)"), re.M)
    assert archive is not None
    assert tracked["archive"] == archive.group(1)
    assert 'output_dir="${BuildPlatform}/${Configuration}"' in body.splitlines()
    assert tracked["sakura"] == "${output_dir}/sakura.exe"
    assert tracked["tests1"] == "${output_dir}/tests1.exe"
    locales = re.findall(r"(?m)^create_language_dll\(([A-Za-z]+-[A-Za-z]+) \d+\)", ROOT_CMAKE.read_text(encoding="utf-8-sig"))
    assert {tracked["language-en-US"], tracked["language-zh-CN"]} == {
        "${output_dir}/sakura_lang_" + locale.replace("-", "_") + ".dll" for locale in locales
    }
    arguments = [line.strip() for line in body.splitlines()]
    assert '--scan "${output_dir}"' in arguments
    assert "--scan build/MinGW" in arguments


def test_the_step_expects_the_archive_and_both_executables_rebuilt_and_the_languages_untouched() -> None:
    body = run_body(STEP)
    changed = re.search(r"--expect-changed (\S+)", body)
    unchanged = re.search(r"--expect-unchanged (\S+)", body)
    assert changed is not None and unchanged is not None
    assert changed.group(1).split(",") == ["archive", "sakura", "tests1"]
    assert unchanged.group(1).split(",") == ["language-en-US", "language-zh-CN"]
