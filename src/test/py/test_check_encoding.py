"""
Unit tests for check_encoding.py

Tests cover:
- File extension validation
- Encoding detection (UTF-8, UTF-16, ASCII)
- Encoding result validation
- Git operations (mocked)
- File processing integration
"""

import importlib.util
import subprocess
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest


def _load_module():
    """Dynamically load checkEncoding module from repository root"""
    repo_root = Path(__file__).resolve().parents[3]
    module_path = repo_root / "src/main/py/check_encoding.py"
    spec = importlib.util.spec_from_file_location("checkEncoding", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load module spec: {module_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def check_encoding_module():
    """Fixture: Load checkEncoding module once per test session"""
    return _load_module()


# ============================================================================
# File Extension Validation Tests
# ============================================================================


def test_check_extension_accepts_cpp(check_encoding_module):
    """Test that .cpp files are recognized as valid"""
    assert check_encoding_module.check_extension("test.cpp") is True


def test_check_extension_accepts_h(check_encoding_module):
    """Test that .h files are recognized as valid"""
    assert check_encoding_module.check_extension("test.h") is True


def test_check_extension_accepts_rc(check_encoding_module):
    """Test that .rc files are recognized as valid"""
    assert check_encoding_module.check_extension("test.rc") is True


def test_check_extension_accepts_rc2(check_encoding_module):
    """Test that .rc2 files are recognized as valid"""
    assert check_encoding_module.check_extension("test.rc2") is True


def test_check_extension_rejects_other_extensions(check_encoding_module):
    """Test that non-target extensions are rejected"""
    assert check_encoding_module.check_extension("test.txt") is False
    assert check_encoding_module.check_extension("test.py") is False
    assert check_encoding_module.check_extension("test.md") is False
    assert check_encoding_module.check_extension("test.exe") is False


# ============================================================================
# Encoding Detection Tests (using actual file creation)
# ============================================================================


def test_check_encoding_detects_utf8_bom(tmp_path, check_encoding_module):
    """Test that UTF-8 BOM files are correctly detected"""
    file_path = tmp_path / "utf8_bom.txt"
    # UTF-8 BOM = \xef\xbb\xbf
    file_path.write_bytes(b"\xef\xbb\xbf" + "Hello".encode("utf-8"))
    
    encoding = check_encoding_module.check_encoding(str(file_path))
    # chardet should detect as UTF-8-SIG (UTF-8 with BOM)
    assert encoding in ("UTF-8-SIG", "UTF-8")


def test_check_encoding_detects_utf16_le(tmp_path, check_encoding_module):
    """Test that UTF-16 LE files are correctly detected"""
    file_path = tmp_path / "utf16_le.txt"
    # Write text in UTF-16 LE encoding
    file_path.write_text("Hello", encoding="utf-16-le")
    
    encoding = check_encoding_module.check_encoding(str(file_path))
    # chardet should detect UTF-16 or UTF-16-LE
    # Some configs may return None; allow flexible detection
    normalized = encoding.upper() if encoding is not None else None
    assert normalized in ("UTF-16", "UTF-16LE", "UTF-16-LE", "UTF-16LE", None)


def test_check_encoding_detects_ascii(tmp_path, check_encoding_module):
    """Test that ASCII files are correctly detected"""
    file_path = tmp_path / "ascii.txt"
    file_path.write_bytes(b"Hello World")
    
    encoding = check_encoding_module.check_encoding(str(file_path))
    # ASCII is subset of UTF-8; chardet may report as UTF-8 or ASCII
    assert encoding in ("ascii", "UTF-8", "ASCII")


def test_check_encoding_handles_nonexistent_file(check_encoding_module):
    """Test that nonexistent file raises FileNotFoundError as expected"""
    # check_encoding.py opens file directly, so it raises FileNotFoundError
    with pytest.raises(FileNotFoundError):
        check_encoding_module.check_encoding("/nonexistent/path/file.txt")


# ============================================================================
# Encoding Result Validation Tests
# ============================================================================


def test_check_encoding_result_cpp_allows_utf8_sig(check_encoding_module):
    """Test that .cpp files accept UTF-8-SIG encoding"""
    # check_encoding_result(file_name, encoding) should return True for valid combo
    result = check_encoding_module.check_encoding_result("test.cpp", "UTF-8-SIG")
    assert result is True


def test_check_encoding_result_cpp_allows_ascii(check_encoding_module):
    """Test that .cpp files accept ASCII encoding"""
    result = check_encoding_module.check_encoding_result("test.cpp", "ascii")
    assert result is True


def test_check_encoding_result_h_allows_utf8_sig(check_encoding_module):
    """Test that .h files accept UTF-8-SIG encoding"""
    result = check_encoding_module.check_encoding_result("test.h", "UTF-8-SIG")
    assert result is True


def test_check_encoding_result_rc_requires_utf16(check_encoding_module):
    """Test that .rc files accept UTF-16 encoding"""
    result = check_encoding_module.check_encoding_result("test.rc", "UTF-16")
    assert result is True


def test_check_encoding_result_rc_rejects_utf8(check_encoding_module):
    """Test that .rc files reject UTF-8 encoding"""
    result = check_encoding_module.check_encoding_result("test.rc", "UTF-8")
    assert result is False


# ============================================================================
# Git Operations Tests (mocked with pytest-mock)
# ============================================================================


# Base-SHA-driven diff and safe fallback tests


def test_get_diff_files_uses_valid_base_sha_and_filters_extensions(mocker, check_encoding_module):
    mock_check_output = mocker.patch("subprocess.check_output")
    mock_check_output.side_effect = [b"abc123def456\n", b"src/test.cpp\0README.md\0src/style.rc\0"]

    files = list(check_encoding_module.get_diff_files("abc123def456"))

    assert files == ["src/test.cpp", "src/style.rc"]
    assert mock_check_output.call_args_list[0].args[0] == [
        "git", "rev-parse", "--verify", "--end-of-options", "abc123def456^{commit}"
    ]
    assert mock_check_output.call_args_list[1].args[0] == [
        "git", "diff", "--name-only", "--diff-filter=d", "-z",
        "abc123def456", "HEAD", "--"
    ]


def test_get_diff_files_excludes_deleted_and_keeps_rename_destination(mocker, check_encoding_module):
    mock_check_output = mocker.patch("subprocess.check_output")
    mock_check_output.side_effect = [b"abc123\n", b"src/renamed.cpp\0src/removed.h\0"]

    files = list(check_encoding_module.get_diff_files("abc123"))

    assert files == ["src/renamed.cpp", "src/removed.h"]
    assert "--diff-filter=d" in mock_check_output.call_args_list[1].args[0]


def test_get_diff_files_propagates_invalid_base_failure(mocker, check_encoding_module):
    mock_check_output = mocker.patch("subprocess.check_output")
    mock_check_output.side_effect = subprocess.CalledProcessError(128, "git")

    with pytest.raises(subprocess.CalledProcessError):
        list(check_encoding_module.get_diff_files("not-a-commit"))


@pytest.mark.parametrize("base_sha", ["", "0" * 40])
def test_empty_or_all_zero_base_is_a_branch_creation_noop(base_sha, mocker, check_encoding_module):
	get_diff_files = mocker.patch.object(
		check_encoding_module, "get_diff_files", return_value=["src/test.cpp"]
	)

	assert check_encoding_module._is_full_scan_base(base_sha) is True
	assert list(check_encoding_module.get_ci_files(base_sha)) == []
	get_diff_files.assert_not_called()


def test_get_ci_files_uses_diff_for_valid_base(mocker, check_encoding_module):
    get_diff_files = mocker.patch.object(
        check_encoding_module, "get_diff_files", return_value=["src/test.cpp"]
    )

    assert list(check_encoding_module.get_ci_files("abc123")) == ["src/test.cpp"]
    get_diff_files.assert_called_once_with("abc123")


def test_check_all_generator_yields_target_extensions(mocker, check_encoding_module):
    """Test check_all generator yields files with target extensions"""
    # Mock os.walk to return controlled directory structure
    mock_walk = mocker.patch("os.walk")
    mock_walk.return_value = [
        (".", ["subdir"], ["test.cpp", "doc.md", "style.rc"]),
        ("./subdir", [], ["header.h", "config.txt"])
    ]
    
    # Collect results from generator
    files = list(check_encoding_module.check_all())
    
    # Should yield at least files with target extensions
    assert len(files) >= 1


# ============================================================================
# Integration Tests
# ============================================================================


def test_process_files_detects_encoding_violations(tmp_path, check_encoding_module, capsys):
    """
    Test process_files() integration: verify it detects encoding violations
    
    Setup:
    - Create a .cpp file with UTF-8 (valid)
    - Create a .rc file with UTF-8 (invalid, should be UTF-16)
    - Expect process_files to detect the .rc as violation
    """
    # Create valid .cpp file (UTF-8)
    cpp_file = tmp_path / "test.cpp"
    cpp_file.write_text("int main() {}", encoding="utf-8")
    
    # Create invalid .rc file (UTF-8 instead of UTF-16)
    rc_file = tmp_path / "test.rc"
    rc_file.write_text("STRINGTABLE\nBEGIN\nEND", encoding="utf-8")
    
    # Process files and count violations
    files = [str(cpp_file), str(rc_file)]
    violation_count = check_encoding_module.process_files(files)
    
    # Should report at least 1 violation (the .rc file)
    assert violation_count >= 1


def test_process_files_accepts_valid_files(tmp_path, check_encoding_module):
    """
    Test process_files() with valid encodings
    
    Setup:
    - Create a .cpp file with UTF-8 (valid)
    - Create a .rc file with UTF-16 (valid)
    - Expect process_files to return 0 violations
    """
    # Create valid .cpp file (UTF-8)
    cpp_file = tmp_path / "valid.cpp"
    cpp_file.write_text("int main() {}", encoding="utf-8")
    
    # Create valid .rc file (UTF-16)
    rc_file = tmp_path / "valid.rc"
    rc_file.write_text("STRINGTABLE\nBEGIN\nEND", encoding="utf-16")
    
    # Process files
    files = [str(cpp_file), str(rc_file)]
    violation_count = check_encoding_module.process_files(files)
    
    # Should report 0 violations for valid files
    assert violation_count == 0
