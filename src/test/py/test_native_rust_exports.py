"""Native Rust staticlib symbol parsing and fail-closed C ABI checks."""

import re
import runpy
import subprocess
import sys
from codecs import BOM_UTF16_BE, BOM_UTF16_LE
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "tools/verify-native-rust-exports.py"
ALLOWLIST = ROOT / "rust/native/sakura_native_ffi/exported-symbols.txt"
PARSERS = runpy.run_path(str(CHECKER))

# Trimmed from dumpbin /LINKERMEMBER:2 for #429, with the worker ABI v2 name.
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
        A sakura_worker_thread_start_v2
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
    "sakura_worker_thread_start_v2",
    "sqrtf16",
}

NM_OUTPUT = """\
libsakura_native_ffi.a[ffi.o]:
sakura_native_initialize_v1 T 0 20
sakura_worker_thread_start_v2 T 20 18
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


def test_byte_simd_candidates_are_comparison_only():
    exports = tuple(
        f"sakura_byte_find_cr_or_lf_{isa}_candidate_v1"
        for isa in ("avx128", "avx2", "avx512bw")
    )
    adapters = {
        "FindCrOrLfRustAvx128": "avx",
        "FindCrOrLfRustAvx2": "avx2",
        "FindCrOrLfRustAvx512Bw": "avx512",
    }
    accessor = "GetSupportedFindCrOrLfRust"
    references = re.compile(r"\b(?:" + "|".join((*exports, *adapters, accessor)) + r")\b")
    adapter_signatures = {
        f"std::size_t {name}(const char* data, std::size_t length) noexcept"
        for name in adapters
    }
    accessor_signature = f"FindCrOrLfFunction {accessor}(Isa isa) noexcept"
    # Pin individual references, including the adapter and Testing accessor
    # names, so wiring either into production dispatch also fails this check.
    allowed = {
        "util/RustUtf16Scan.h": {f"SakuraStatus {name}(" for name in exports},
        "util/CpuDispatchRust.cpp": {f"return {name}(" for name in exports} | adapter_signatures,
        "util/CpuDispatchInternal.h": {f"{signature};" for signature in adapter_signatures},
        "util/CpuDispatch.h": {f"{accessor_signature};"},
        "util/CpuDispatch.cpp": {accessor_signature} | {
            f"return dispatch.capabilities.{capability} ? Internal::{name} : nullptr;"
            for name, capability in adapters.items()
        },
    }

    core = ROOT / "sakura_core"
    assert core.is_dir()
    source_suffixes = {
        ".c", ".cc", ".cpp", ".cppm", ".cxx", ".h", ".hh", ".hpp", ".hsrc",
        ".hxx", ".inc", ".inl", ".ixx", ".rc", ".rc2",
    }
    unexpected = []
    # Test sources live outside sakura_core/ and can use the candidates.
    for path in sorted(core.rglob("*")):
        if path.suffix.lower() not in source_suffixes or not path.is_file():
            continue
        relative = path.relative_to(core).as_posix()
        permitted = allowed.get(relative, set())
        data = path.read_bytes()
        encoding = "utf-16" if data.startswith((BOM_UTF16_LE, BOM_UTF16_BE)) else "utf-8-sig"
        for number, line in enumerate(data.decode(encoding, errors="replace").splitlines(), 1):
            if not references.search(line):
                continue
            reference = line.strip()
            if reference in permitted:
                permitted.remove(reference)
            else:
                unexpected.append(f"sakura_core/{relative}:{number}: {reference}")
    assert not unexpected, (
        "Rust byte CR/LF candidates require a versioned byte operation policy before "
        "production use:\n" + "\n".join(unexpected)
    )


def test_dumpbin_parser_reads_the_public_symbol_table():
    assert PARSERS["parse_dumpbin"](DUMPBIN_OUTPUT) == DUMPBIN_NAMES


def test_gnu_nm_parser_reads_defined_external_symbols():
    assert PARSERS["parse_nm"](NM_OUTPUT) == {
        "sakura_native_initialize_v1",
        "sakura_worker_thread_start_v2",
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
