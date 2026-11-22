"""Native Rust staticlib symbol parsing and fail-closed C ABI checks."""

import runpy
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "tools/verify-native-rust-exports.py"
ALLOWLIST = ROOT / "rust/native/sakura_native_ffi/exported-symbols.txt"
PARSERS = runpy.run_path(str(CHECKER))

# Trimmed from dumpbin /LINKERMEMBER:2 on the Release archive for #429.
DUMPBIN_OUTPUT = """\
Dump of file sakura_native_ffi.lib

File Type: LIBRARY

    302 offsets

        1    2C10C
       12A   40FE24

    8 public symbols

       9C .weak.__adddf3.default
       14 ProcessPrng
        7 _RINvMNtCs9N2lWLRSIT9_4core3stre18trim_start_matchesReECs3A4XqWyy34_14rustc_demangle
       9C __adddf3
       14 __imp_ProcessPrng
        A sakura_native_initialize_v1
        A sakura_worker_thread_start_v1
       2C sqrtf16

Archive member name at 2548E: //
"""
DUMPBIN_NAMES = {
    ".weak.__adddf3.default",
    "ProcessPrng",
    "_RINvMNtCs9N2lWLRSIT9_4core3stre18trim_start_matchesReECs3A4XqWyy34_14rustc_demangle",
    "__adddf3",
    "__imp_ProcessPrng",
    "sakura_native_initialize_v1",
    "sakura_worker_thread_start_v1",
    "sqrtf16",
}

NM_OUTPUT = """\
libsakura_native_ffi.a[ffi.o]:
sakura_native_initialize_v1 T 0 20
sakura_worker_thread_start_v1 T 20 18
local_helper t 38 4
missing_import U

libsakura_native_ffi.a[compiler_builtins.o]:
___chkstk_ms T 0
__adddf3 W 10 20
"""


def allowlist_names() -> list[str]:
    return [
        name for line in ALLOWLIST.read_text(encoding="ascii").splitlines()
        if (name := line.partition("#")[0].strip())
    ]


def test_allowlist_is_sorted_unique_and_nonempty():
    names = allowlist_names()
    assert names
    assert names == sorted(set(names))


def test_dumpbin_parser_reads_the_public_symbol_table():
    assert PARSERS["parse_dumpbin"](DUMPBIN_OUTPUT) == DUMPBIN_NAMES


def test_gnu_nm_parser_reads_defined_external_symbols():
    assert PARSERS["parse_nm"](NM_OUTPUT) == {
        "sakura_native_initialize_v1",
        "sakura_worker_thread_start_v1",
        "___chkstk_ms",
        "__adddf3",
    }


@pytest.mark.parametrize(
    "added, removed, diagnostic",
    [
        (None, None, None),
        ("sakura_probe_v1", None, "Unexpected export: sakura_probe_v1"),
        (None, "sakura_native_initialize_v1", "Missing export: sakura_native_initialize_v1"),
        ("external_probe_v1", None, "Unknown public symbol: external_probe_v1"),
    ],
)
def test_captured_symbol_check(tmp_path, added, removed, diagnostic):
    exports = set(allowlist_names())
    names = exports | {name for name in DUMPBIN_NAMES if not name.startswith("sakura_")}
    if added:
        names.add(added)
    if removed:
        names.remove(removed)
    capture = tmp_path / "dumpbin.txt"
    capture.write_text(
        f"    {len(names)} public symbols\n\n"
        + "".join(f"        A {name}\n" for name in sorted(names)),
        encoding="ascii",
    )
    result = subprocess.run(
        (sys.executable, str(CHECKER), "--format", "dumpbin", "--symbols-from", str(capture)),
        capture_output=True, text=True, timeout=15, check=False,
    )
    if diagnostic is None:
        assert result.returncode == 0, result.stderr
        assert f"({len(exports)} symbols)" in result.stdout
    else:
        assert result.returncode != 0
        assert result.stderr.splitlines() == [diagnostic]
