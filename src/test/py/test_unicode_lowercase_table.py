"""Verify the checked-in Unicode lowercase tables for URI identity (#436)."""

import hashlib
import importlib.util
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR = REPO_ROOT / "tools/generate-unicode-lowercase-table.py"
HEADER = REPO_ROOT / "sakura_core/platform/uri/UnicodeLowercaseData.h"


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_unicode_lowercase_table", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def table_rows(body: str, name: str, width: int) -> list[tuple]:
    prefix = f"inline constexpr std::uint32_t {name}[][{width}] = {{\n"
    rows_text = body.split(prefix, 1)[1].split("};", 1)[0]
    rows = []
    for line in rows_text.splitlines():
        values = re.findall(r"0x[0-9A-F]+", line)
        assert len(values) == width
        rows.append(tuple(int(value, 16) for value in values))
    assert rows
    assert all(left[0] < right[0] for left, right in zip(rows, rows[1:]))
    return rows


def test_table_metadata_and_contents() -> None:
    generator = load_generator()
    header = HEADER.read_text(encoding="ascii")
    assert generator.UNICODE_VERSION == "17.0.0"
    assert 'inline constexpr char kUnicodeVersion[] = "17.0.0";' in header
    sources = dict(re.findall(r"^// (\w+\.txt) sha256:([0-9a-f]{64})$", header, re.MULTILINE))
    assert sources == generator.PINNED_SHA256

    body = header.split("// BEGIN GENERATED TABLES\n", 1)[1].split("// END GENERATED TABLES\n", 1)[0]
    body_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()
    assert f"// Table body sha256:{body_hash}\n" in header

    simple = dict(table_rows(body, "kSimpleLowercase", 2))
    special = table_rows(body, "kSpecialLowercase", 4)
    assert special == [(0x0130, 2, 0x0069, 0x0307)]
    assert simple.keys().isdisjoint(row[0] for row in special)
    assert all(scalar != lower for scalar, lower in simple.items())
    for scalar, lower in {
        0x00C9: 0x00E9,
        0x212A: 0x006B,
        0x2126: 0x03C9,
        0x10400: 0x10428,
        0x1E9E: 0x00DF,
        0x1F88: 0x1F80,
        0x13A0: 0xAB70,
        0x01C5: 0x01C6,
    }.items():
        assert simple[scalar] == lower

    cased = table_rows(body, "kCased", 2)
    ignorable = table_rows(body, "kCaseIgnorable", 2)
    for ranges in (cased, ignorable):
        assert all(first <= last for first, last in ranges)
        assert all(left[1] + 1 < right[0] for left, right in zip(ranges, ranges[1:]))
        assert not any(first <= 0x002F <= last for first, last in ranges)
    assert any(first <= 0x03A3 <= last for first, last in cased)
    assert not any(first <= 0x03A3 <= last for first, last in ignorable)
    for scalar in (0x0027, 0x002E, 0x0301):
        assert any(first <= scalar <= last for first, last in ignorable)


def test_generator_rejects_a_digest_mismatch(tmp_path: Path) -> None:
    sample = bytearray(b"0041;LATIN CAPITAL LETTER A;Lu;0;L;;;;;N;;;;0061;\n")
    sample[0] ^= 1
    (tmp_path / "UnicodeData.txt").write_bytes(sample)
    output = tmp_path / "UnicodeLowercaseData.h"
    completed = subprocess.run(
        [sys.executable, "-I", str(GENERATOR), "--ucd-dir", str(tmp_path), "--output", str(output)],
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    assert completed.returncode == 1
    assert "UnicodeData.txt: SHA-256 mismatch" in completed.stderr
    assert not output.exists()
