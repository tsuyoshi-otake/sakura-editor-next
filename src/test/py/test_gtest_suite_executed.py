"""Contracts for the MinGW Output suite execution evidence (#435)."""

import re
import subprocess
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
TOOL = REPO_ROOT / "tools/verify-gtest-suite-executed.py"
WORKFLOW = REPO_ROOT / ".github/workflows/build-on-msys2.yml"
CONFORMANCE = (
    REPO_ROOT / "src/test/cpp/tests1/workbench/OutputServiceProviderConformanceTest.cpp"
)
SUITE = "OutputServiceProviderConformance"
EXECUTED_CASE = '<testcase name="First" status="run" result="completed"/>'


def suite_xml(cases: str, name: str = SUITE) -> str:
    return f'<testsuite name="{name}">{cases}</testsuite>'


def invoke(
    tmp_path: Path, contents: str | None, *args: str
) -> subprocess.CompletedProcess[str]:
    report = tmp_path / "tests1-googletest.xml"
    if contents is not None:
        report.write_text(contents, encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(TOOL), "--xml", str(report), "--suite", SUITE, *args],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )


def test_reports_counts_and_test_names(tmp_path: Path) -> None:
    body = EXECUTED_CASE + '<testcase name="Second" status="run" result="completed"/>'
    unrelated = suite_xml('<testcase name="Other" status="notrun"/>', "OtherSuite")
    result = invoke(tmp_path, f"<testsuites>{unrelated}{suite_xml(body)}</testsuites>")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [f"gtest suite {SUITE}: 2 executed, 0 failed", "First", "Second"]


@pytest.mark.parametrize(
    ("contents", "diagnostic"),
    [
        (suite_xml(EXECUTED_CASE, "OtherSuite"), "suite missing"),
        (suite_xml(""), "no tests executed"),
        (
            suite_xml('<testcase name="Fail" status="run" result="completed">'
                      '<failure>private payload</failure></testcase>'),
            "1 failed",
        ),
        (
            suite_xml('<testcase name="Error" status="run" result="completed">'
                      '<error>private payload</error></testcase>'),
            "1 failed",
        ),
        (
            suite_xml('<testcase name="Dormant" status="notrun" result="completed"/>'),
            "1 not executed",
        ),
        (
            suite_xml(
                '<testcase name="Deferred" status="run" result="' + "ski" + "pped" + '"/>'
            ),
            "1 not executed",
        ),
    ],
)
def test_rejects_incomplete_suite(tmp_path: Path, contents: str, diagnostic: str) -> None:
    result = invoke(tmp_path, contents)
    assert result.returncode == 1
    assert diagnostic in result.stderr
    assert "private payload" not in result.stdout + result.stderr


def test_enforces_minimum(tmp_path: Path) -> None:
    result = invoke(tmp_path, suite_xml(EXECUTED_CASE), "--min-tests", "2")
    assert result.returncode == 1
    assert "expected at least 2" in result.stderr


@pytest.mark.parametrize(
    ("contents", "diagnostic"),
    [("<testsuites>", "malformed GoogleTest XML"), ("", "malformed GoogleTest XML"),
     (None, "cannot read GoogleTest XML")],
)
def test_rejects_invalid_report(tmp_path: Path, contents: str | None, diagnostic: str) -> None:
    result = invoke(tmp_path, contents)
    assert result.returncode == 2
    assert diagnostic in result.stderr


def test_rejects_invalid_minimum(tmp_path: Path) -> None:
    result = invoke(tmp_path, suite_xml(EXECUTED_CASE), "--min-tests", "0")
    assert result.returncode == 2
    assert "--min-tests must be at least 1" in result.stderr


def test_checks_every_matching_suite(tmp_path: Path) -> None:
    first = suite_xml(EXECUTED_CASE)
    second = suite_xml('<testcase name="Second" status="run" result="completed"/>')
    result = invoke(tmp_path, f"<testsuites>{first}{second}</testsuites>", "--min-tests", "2")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[1:] == ["First", "Second"]

    dormant = suite_xml('<testcase name="Dormant" status="notrun"/>')
    result = invoke(tmp_path, f"<testsuites>{first}{dormant}</testsuites>")
    assert result.returncode == 1
    assert "1 not executed" in result.stderr


def test_mingw_verifies_the_suite_immediately_after_unit_tests() -> None:
    text = WORKFLOW.read_text(encoding="utf-8-sig")
    mingw_job = text.split("\n  mingw:\n", 1)[1]
    steps = re.split(r"(?m)^      - (?=(?:name|uses|run):)", mingw_job)
    unit_tests = next(
        index for index, step in enumerate(steps)
        if step.startswith("name: Run unit tests\n")
    )
    evidence = steps[unit_tests + 1]
    assert evidence.startswith("name: Verify Output provider conformance execution\n")
    assert re.search(
        rf"(?m)^        run: py -3 tools/verify-gtest-suite-executed\.py "
        rf"--xml tests1-googletest\.xml --suite {SUITE}$",
        evidence,
    )
    assert not re.search(r"(?m)^        (?:continue-on-error|if):", evidence)


def test_conformance_source_declares_the_expected_suite() -> None:
    text = CONFORMANCE.read_text(encoding="utf-8-sig")
    assert f"TEST({SUITE}," in text
