"""Contracts for checking the UTF-16 backend before packaging sakura.exe."""

from __future__ import annotations

import importlib.util
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools/verify_utf16_package_backend.py"
MARKER = "SakuraUtf16Backend"

PE_OFFSET = 0x80
OPTIONAL_OFFSET = PE_OFFSET + 24
SECTION_OFFSET = 0x200
SECTION_RVA = 0x1000
RESOURCE_PREFIX = 0x20
RESOURCE_OFFSET = SECTION_OFFSET + RESOURCE_PREFIX
RESOURCE_RVA = SECTION_RVA + RESOURCE_PREFIX
DIRECTORY_BIT = 0x80000000


def _load_tool():
    spec = importlib.util.spec_from_file_location("verify_utf16_package_backend", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def checker():
    return _load_tool()


def _alignment(size: int, boundary: int) -> int:
    return -size % boundary


def _wide(text: str) -> bytes:
    return (text + "\0").encode("utf-16-le")


def _block(
    key: str, *, kind: int = 1, value: bytes = b"", children: tuple[bytes, ...] = ()
) -> bytes:
    body = bytearray(b"\0" * 6 + _wide(key))
    if value:
        body.extend(b"\0" * _alignment(len(body), 4))
        body.extend(value)
    for child in children:
        body.extend(b"\0" * _alignment(len(body), 4))
        body.extend(child)
    struct.pack_into("<HHH", body, 0, len(body), len(value) // (2 if kind else 1), kind)
    return bytes(body)


def _version(*markers: str | None) -> bytes:
    tables = []
    for index, value in enumerate(markers):
        entries = [_block("FileDescription", value=_wide("Sakura Editor NEXT"))]
        if value is not None:
            entries.append(_block(MARKER, value=_wide(value)))
        tables.append(_block(f"{0x411 + index:04x}04b0", children=tuple(entries)))
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x10000, *([0] * 11))
    translation = _block("Translation", kind=0, value=struct.pack("<HH", 0x411, 1200))
    return _block(
        "VS_VERSION_INFO",
        kind=0,
        value=fixed,
        children=(
            _block("StringFileInfo", children=tuple(tables)),
            _block("VarFileInfo", children=(translation,)),
        ),
    )


def _resource_tree(versions: tuple[bytes, ...]) -> bytes:
    # Directories at 0, 24, and 48, followed by the language data entries.
    tree = bytearray(64 + len(versions) * 24)
    struct.pack_into("<HII", tree, 14, 1, 16, DIRECTORY_BIT | 24)
    struct.pack_into("<HII", tree, 38, 1, 1, DIRECTORY_BIT | 48)
    struct.pack_into("<H", tree, 62, len(versions))
    for index, version in enumerate(versions):
        data_entry = 64 + len(versions) * 8 + index * 16
        struct.pack_into("<II", tree, 64 + index * 8, 0x411 + index, data_entry)
        tree.extend(b"\0" * _alignment(len(tree), 4))
        struct.pack_into("<IIII", tree, data_entry, RESOURCE_RVA + len(tree), len(version), 0, 0)
        tree.extend(version)
    return bytes(tree)


def _pe(*versions: bytes, pe_plus: bool = True) -> bytes:
    directory_offset = 112 if pe_plus else 96
    optional_size = directory_offset + 16 * 8
    image = bytearray(SECTION_OFFSET)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, PE_OFFSET)
    struct.pack_into(
        "<4sHHIIIHH", image, PE_OFFSET, b"PE\0\0",
        0x8664 if pe_plus else 0x14C, 1, 0, 0, 0, optional_size, 2,
    )
    struct.pack_into("<H", image, OPTIONAL_OFFSET, 0x20B if pe_plus else 0x10B)
    struct.pack_into("<I", image, OPTIONAL_OFFSET + directory_offset - 4, 16)
    if versions:
        tree = _resource_tree(versions)
        struct.pack_into(
            "<II", image, OPTIONAL_OFFSET + directory_offset + 2 * 8, RESOURCE_RVA, len(tree)
        )
        name, contents = b".rsrc", b"\0" * RESOURCE_PREFIX + tree
    else:
        name, contents = b".text", b"\xc3"
    raw_size = len(contents) + _alignment(len(contents), 0x200)
    struct.pack_into(
        "<8sIIII", image, OPTIONAL_OFFSET + optional_size,
        name, len(contents), SECTION_RVA, raw_size, SECTION_OFFSET,
    )
    image.extend(contents)
    image.extend(b"\0" * (raw_size - len(contents)))
    return bytes(image)


def _patched_u32(image: bytes, offset: int, value: int) -> bytes:
    patched = bytearray(image)
    struct.pack_into("<I", patched, offset, value)
    return bytes(patched)


@pytest.mark.parametrize("pe_plus", [True, False], ids=["PE32+", "PE32"])
def test_parser_reads_both_optional_header_formats(checker, pe_plus: bool) -> None:
    assert checker.read_markers(_pe(_version("cpp"), pe_plus=pe_plus)) == {"cpp"}


def test_parser_collects_every_string_table_and_version_resource(checker) -> None:
    image = _pe(_version("cpp", "rust"), _version("other"))
    assert checker.read_markers(image) == {"cpp", "rust", "other"}


def test_parser_accepts_a_valid_pe_with_no_resource_section(checker) -> None:
    assert checker.read_markers(_pe()) == set()


@pytest.mark.parametrize(
    ("image", "cause"),
    [
        (b"\x00" * 32, "DOS header"),
        (b"MZ" + b"\x00" * 62, "PE header"),
        (
            _patched_u32(_pe(_version("cpp")), OPTIONAL_OFFSET + 112 + 16, 0x5000),
            "resource directory RVA",
        ),
        (
            _patched_u32(_pe(_version("cpp")), RESOURCE_OFFSET + 20, DIRECTORY_BIT),
            "cyclic resource directory",
        ),
        (_pe(_version("cpp")[:-2]), "version block length"),
    ],
    ids=["short-dos", "mz-without-pe", "bad-rva", "cycle", "short-version"],
)
def test_parser_reports_corruption(checker, image: bytes, cause: str) -> None:
    with pytest.raises(checker.PeFormatError, match=cause):
        checker.read_markers(image)


@pytest.mark.parametrize(
    ("image", "code", "evidence"),
    [
        (_pe(_version("cpp")), 0, "sakura.exe UTF-16 backend marker: cpp"),
        (_pe(_version("cpp", "cpp"), pe_plus=False), 0, "sakura.exe UTF-16 backend marker: cpp"),
        (_pe(_version("rust")), 1, "is rust"),
        (_pe(_version("auto")), 1, "unexpected SakuraUtf16Backend value"),
        (_pe(_version(None)), 1, "missing SakuraUtf16Backend marker"),
        (_pe(), 1, "missing SakuraUtf16Backend marker"),
        (_pe(_version("cpp", "rust")), 1, "conflicting SakuraUtf16Backend markers"),
        (_pe(_version("cpp"), _version("rust")), 1, "conflicting SakuraUtf16Backend markers"),
        (b"MZ", 2, "invalid PE version resource"),
        (None, 2, "cannot read executable"),
    ],
    ids=[
        "cpp-pe32-plus", "cpp-pe32-repeated", "rust", "unknown", "unmarked",
        "no-resource", "conflicting-tables", "conflicting-resources", "corrupt", "missing-file",
    ],
)
def test_checker_cli(tmp_path: Path, image: bytes | None, code: int, evidence: str) -> None:
    exe = tmp_path / "sakura.exe"
    if image is not None:
        exe.write_bytes(image)
    result = subprocess.run(
        [sys.executable, "-I", str(TOOL), "--exe", str(exe)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert result.returncode == code, result.stderr
    if code == 0:
        assert result.stdout.splitlines() == [evidence]
        assert not result.stderr
    else:
        assert evidence in result.stderr
        assert not result.stdout


@pytest.mark.parametrize("script_name", ["build-installer.bat", "zipArtifacts.bat"])
def test_packager_checks_the_binary_before_filesystem_mutation(script_name: str) -> None:
    script = (ROOT / script_name).read_text(encoding="utf-8-sig")
    executable = r"%platform%\%configuration%\sakura.exe"
    invocation = f'py -3 -I "%~dp0tools\\verify_utf16_package_backend.py" --exe "{executable}"'
    position = script.index(invocation)
    assert script.index("Production packaging requires SAKURA_OUTPUT_BACKEND=") < position
    mutation = re.search(
        r'(?im)^[ \t]*@?(?:if[ \t]+(?:not[ \t]+)?exist[ \t]+(?:"[^"\n]*"|\S+)[ \t]+)?'
        r'(?:mkdir|rmdir|copy|del|xcopy|robocopy)\b',
        script,
    )
    assert mutation is not None and position < mutation.start()
    failure_path = script[position + len(invocation):mutation.start()]
    assert re.match(
        r'\nset "UTF16_PACKAGE_BACKEND_EXIT=%ERRORLEVEL%"\n'
        r'if not "%UTF16_PACKAGE_BACKEND_EXIT%" == "0" \(\n'
        rf'[ \t]+echo [^\n]*{re.escape(executable)}[^\n]*\n'
        r'[ \t]+exit /b %UTF16_PACKAGE_BACKEND_EXIT%\n\)',
        failure_path,
    )


def test_version_resource_uses_the_resolved_backend_token() -> None:
    resource = (ROOT / "sakura_core/sakura_rc.rc2").read_text(encoding="utf-16")
    after_table = resource.split('BLOCK "041104b0"', 1)[1]
    table = re.split(r"(?m)^[ \t]+END[ \t]*$", after_table, maxsplit=1)[0]
    assert re.search(
        r'(?m)^#if defined\(SAKURA_UTF16_BACKEND_RUST\)\n'
        r'[ \t]+VALUE "SakuraUtf16Backend", "rust\\0"\n'
        r'#else\n[ \t]+VALUE "SakuraUtf16Backend", "cpp\\0"\n#endif$',
        table,
    )


def test_msbuild_uses_the_same_rust_condition_for_cpp_and_resources() -> None:
    targets = ET.parse(ROOT / "src/main/msbuild/sakura-rust-core.targets").getroot()
    namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    definitions = [
        (
            group.get("Condition"),
            group.findtext("m:ClCompile/m:PreprocessorDefinitions", namespaces=namespace),
            group.findtext("m:ResourceCompile/m:PreprocessorDefinitions", namespaces=namespace),
        )
        for group in targets.findall("m:ItemDefinitionGroup", namespace)
    ]
    token = "SAKURA_UTF16_BACKEND_RUST"
    token_definitions = [
        (condition, cpp, rc)
        for condition, cpp, rc in definitions
        if token in (cpp or "").split(";") or token in (rc or "").split(";")
    ]
    assert token_definitions == [
        ("'$(SakuraUtf16BackendUpper)'=='RUST'",
         f"{token};%(PreprocessorDefinitions)", f"{token};%(PreprocessorDefinitions)")
    ]


def test_cmake_defines_the_rust_token_for_the_resource_owner() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    assert re.search(
        r'(?m)^target_sources\(\$\{PROJECT_NAME\} PRIVATE \$\{SAKURA_LEGACY_RESOURCE_SCRIPTS\}\)\n'
        r'(?:#[^\n]*\n)*'
        r'if\(SAKURA_UTF16_BACKEND STREQUAL "rust"\)\n'
        r'[ \t]+target_compile_definitions\(\$\{PROJECT_NAME\} PRIVATE SAKURA_UTF16_BACKEND_RUST\)\n'
        r'endif\(\)$',
        cmake,
    )
    core = (ROOT / "src/main/cmake/sakura.cmake").read_text(encoding="utf-8-sig")
    assert re.search(
        r'(?m)^set\(RESOURCE_SCRIPTS\n[ \t]+\$\{CMAKE_SOURCE_DIR\}/sakura_core/sakura_rc\.rc$',
        core,
    )
    assert "set(SAKURA_LEGACY_RESOURCE_SCRIPTS ${RESOURCE_SCRIPTS})" in core
