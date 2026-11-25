"""Contracts for the MinGW Output, worker-thread, encoded EOL, and UTF-8 codec suite evidence (#435, #441, #443)."""

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
WORKER_THREAD_TEST = (
    REPO_ROOT / "src/test/cpp/tests1/platform/foundation/NativeWorkerThreadTest.cpp"
)
ENCODED_EOL_CONTRACT_TEST = (
    REPO_ROOT / "src/test/cpp/tests1/charset/EncodedEolContractTest.cpp"
)
ENCODED_EOL_CANDIDATE_TEST = (
    REPO_ROOT / "src/test/cpp/tests1/charset/EncodedEolRustCandidateTest.cpp"
)
UTF8_CODEC_CONTRACT_TEST = (
    REPO_ROOT / "src/test/cpp/tests1/charset/Utf8CodecContractTest.cpp"
)
UTF8_CODEC_CANDIDATE_TEST = (
    REPO_ROOT / "src/test/cpp/tests1/charset/Utf8CodecRustCandidateTest.cpp"
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


def mingw_workflow_steps() -> list[str]:
    text = WORKFLOW.read_text(encoding="utf-8-sig")
    mingw_job = text.split("\n  mingw:\n", 1)[1]
    return re.split(r"(?m)^      - (?=(?:name|uses|run):)", mingw_job)


def test_mingw_verifies_the_suite_immediately_after_unit_tests() -> None:
    steps = mingw_workflow_steps()
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


def test_mingw_verifies_native_worker_threads_immediately_after_output() -> None:
    steps = mingw_workflow_steps()
    output = next(
        index for index, step in enumerate(steps)
        if step.startswith("name: Verify Output provider conformance execution\n")
    )
    evidence = steps[output + 1]
    assert evidence.startswith("name: Verify native worker thread execution\n")
    assert (
        "        run: py -3 tools/verify-gtest-suite-executed.py "
        "--xml tests1-googletest.xml --suite NativeWorkerThread --min-tests 8"
    ) in evidence.splitlines()
    assert not re.search(r"(?m)^        (?:continue-on-error|if):", evidence)


def test_native_worker_thread_source_declares_at_least_eight_cases() -> None:
    text = WORKER_THREAD_TEST.read_text(encoding="utf-8-sig")
    assert len(re.findall(r"(?m)^TEST\(NativeWorkerThread,", text)) >= 8


def test_conformance_source_declares_the_expected_suite() -> None:
    text = CONFORMANCE.read_text(encoding="utf-8-sig")
    assert f"TEST({SUITE}," in text


ENCODED_EOL_STEPS = (
    ("Verify encoded EOL contract execution", "EncodedEolContract", 7),
    ("Verify encoded EOL candidate execution", "EncodedEolRustCandidate", 10),
)


def test_mingw_verifies_the_encoded_eol_suites_immediately_after_native_worker_threads() -> None:
    steps = mingw_workflow_steps()
    worker = next(
        index for index, step in enumerate(steps)
        if step.startswith("name: Verify native worker thread execution\n")
    )
    for offset, (name, suite, minimum) in enumerate(ENCODED_EOL_STEPS, start=1):
        evidence = steps[worker + offset]
        assert evidence.startswith(f"name: {name}\n")
        assert (
            "        run: py -3 tools/verify-gtest-suite-executed.py "
            f"--xml tests1-googletest.xml --suite {suite} --min-tests {minimum}"
        ) in evidence.splitlines()
        assert not re.search(r"(?m)^        (?:continue-on-error|if):", evidence)


def test_encoded_eol_contract_source_declares_at_least_seven_cases() -> None:
    text = ENCODED_EOL_CONTRACT_TEST.read_text(encoding="utf-8-sig")
    assert len(re.findall(r"(?m)^TEST\(EncodedEolContract,", text)) >= 7


def test_encoded_eol_candidate_source_declares_ten_cases_in_the_linked_branch() -> None:
    # Only the branch compiled with the Rust archive counts: the single unlinked
    # alternative after `#else` must never satisfy the minimum the workflow enforces.
    text = ENCODED_EOL_CANDIDATE_TEST.read_text(encoding="utf-8-sig")
    linked = text.partition("\n#else\n")[0]
    assert len(re.findall(r"(?m)^TEST\(EncodedEolRustCandidate,", linked)) >= 10


UTF8_CODEC_STEPS = (
    ("Verify UTF-8 codec contract execution", "Utf8CodecContract", 7),
    ("Verify UTF-8 codec candidate execution", "Utf8CodecRustCandidate", 15),
)


def test_mingw_verifies_the_utf8_codec_suites_immediately_after_the_encoded_eol_suites() -> None:
    steps = mingw_workflow_steps()
    eol_candidate = next(
        index for index, step in enumerate(steps)
        if step.startswith("name: Verify encoded EOL candidate execution\n")
    )
    for offset, (name, suite, minimum) in enumerate(UTF8_CODEC_STEPS, start=1):
        evidence = steps[eol_candidate + offset]
        assert evidence.startswith(f"name: {name}\n")
        assert (
            "        run: py -3 tools/verify-gtest-suite-executed.py "
            f"--xml tests1-googletest.xml --suite {suite} --min-tests {minimum}"
        ) in evidence.splitlines()
        assert not re.search(r"(?m)^        (?:continue-on-error|if):", evidence)


def test_utf8_codec_contract_source_declares_at_least_seven_cases() -> None:
    text = UTF8_CODEC_CONTRACT_TEST.read_text(encoding="utf-8-sig")
    assert len(re.findall(r"(?m)^TEST\(Utf8CodecContract,", text)) >= 7


def test_utf8_codec_candidate_source_declares_fifteen_cases_in_the_linked_branch() -> None:
    # Same rule as the encoded EOL candidate: the one unlinked alternative after
    # `#else` must never satisfy the minimum the workflow enforces.
    text = UTF8_CODEC_CANDIDATE_TEST.read_text(encoding="utf-8-sig")
    linked = text.partition("\n#else\n")[0]
    assert len(re.findall(r"(?m)^TEST\(Utf8CodecRustCandidate,", linked)) >= 15
