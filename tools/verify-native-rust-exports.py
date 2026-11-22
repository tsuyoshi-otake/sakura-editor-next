"""Compare the native Rust staticlib's public symbols with its C ABI allowlist."""

import argparse
import re
import subprocess
import sys
from pathlib import Path


ALLOWLIST = (
    Path(__file__).resolve().parents[1]
    / "rust/native/sakura_native_ffi/exported-symbols.txt"
)

# Every sakura_* name is an ABI candidate. Other public names must match a
# known toolchain symbol: mangling, COFF metadata, Rust/Win32 runtime, or
# compiler-builtins/libm. Match whole names so new C exports fail closed.
RUNTIME_SYMBOLS = tuple(map(re.compile, (
    # Rust v0 and legacy mangling, and MSVC decorated names.
    r"_R(?:[BIMNXY][A-Za-z0-9_]*)?C(?:s[A-Za-z0-9]+_)?u?[0-9]+[A-Za-z_][A-Za-z0-9_.$]*",
    r"_ZN[0-9].*E(?:\.llvm\.[0-9]+)?",
    r"\?\S+",
    # LLVM/COFF metadata and MSVC/MinGW import records.
    r"anon\.[0-9a-f]+\.[0-9]+\.llvm\.[0-9]+",
    r"\.weak\.\S+\.default(?:\.\S+)?",
    r"\.refptr\.\S+",
    r"__(?:real|xmm|ymm|zmm)@[0-9a-f]+",
    r"__(?:imp_|(?:NULL_)?IMPORT_DESCRIPTOR_)\S+",
    r"\x7f[A-Za-z0-9_-]+_NULL_THUNK_DATA",
    r"(?:_head_D__|__D__)[A-Za-z0-9_]+_dll_imports_lib(?:_iname)?",
    # Unmangled platform runtime, compiler-builtins, and libm entry points.
    r"ProcessPrng|WaitOnAddress|WakeByAddress(?:All|Single)",
    r"rust_eh_personality|___chkstk_ms|__compilerrt_abort_impl",
    r"__rust_[iu]128_(?:addo?|mulo|subo?)",
    r"__gnu_(?:f2h|h2f)_ieee",
    r"__(?:abs|add|mul|neg|sub)v(?:si|di|ti)[234]",
    r"__(?:ashl|ashr|bswap|clz|ctz|cmp|div|divmod|ffs|lshr|mod|mul|mulo|neg|"
    r"parity|popcount|ucmp|udiv|udivmod|umod)(?:si|di|ti)[234]",
    r"__(?:add|div|eq|ge|gt|le|lt|mul|ne|neg|powi|sub|unord)(?:hf|sf|df|tf|sc|dc)[23]",
    r"__(?:extend|trunc)(?:hf|sf|df|tf){2}2",
    r"__fix(?:uns)?(?:hf|sf|df|tf)(?:si|di|ti)",
    r"__float(?:un)?(?:si|di|ti)(?:hf|sf|df|tf)",
    r"(?:ceil|copysign|fabs|fdim|floor|fma|fmax|fmin|fmod|rint|round|sqrt|trunc)f(?:16|128)",
    r"(?:fmaximum(?:_num)?|fminimum(?:_num)?|roundeven)(?:f(?:16|128)?)?",
    r"lgammaf?_r",
)))


def parse_dumpbin(output: str) -> set[str]:
    lines = iter(output.splitlines())
    for line in lines:
        header = re.fullmatch(r"\s*([0-9]+) public symbols\s*", line)
        if header:
            break
    else:
        raise ValueError("dumpbin output is missing its public symbol table")

    expected_count = int(header[1])
    if expected_count == 0:
        return set()

    names = []
    for line in lines:
        if not line.strip():
            if names:
                break
            continue
        entry = re.fullmatch(r"\s*[0-9A-Fa-f]+\s+(\S+)\s*", line)
        if not entry:
            raise ValueError(f"invalid dumpbin public symbol row: {line!r}")
        names.append(entry[1])
    if len(names) != expected_count:
        raise ValueError(f"dumpbin listed {len(names)} of {expected_count} public symbols")
    return set(names)


def parse_nm(output: str) -> set[str]:
    names = set()
    for line in output.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        entry = re.fullmatch(r"(\S+)\s+([A-Za-z?])(?:\s+([0-9A-Fa-f]+)(?:\s+[0-9A-Fa-f]+)?)?", line)
        if not entry:
            raise ValueError(f"invalid GNU nm POSIX symbol row: {line!r}")
        name, kind, address = entry.groups()
        # GNU nm uses lowercase u for unique globals and i for indirect functions.
        if address is not None and kind != "U" and (not kind.islower() or kind in "iu"):
            names.add(name)
    return names


def read_allowlist() -> list[str]:
    names = [
        name
        for line in ALLOWLIST.read_text(encoding="ascii").splitlines()
        if (name := line.partition("#")[0].strip())
    ]
    if (
        not names
        or names != sorted(set(names))
        or any(not re.fullmatch(r"sakura_[A-Za-z0-9_]+", name) for name in names)
    ):
        raise ValueError(f"{ALLOWLIST} must contain sorted, unique sakura_* names")
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format", choices=("dumpbin", "nm"), required=True)
    parser.add_argument("--tool", help="path to dumpbin or GNU nm (defaults to the tool on PATH)")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--archive", type=Path, help="path to the built staticlib")
    source.add_argument("--symbols-from", type=Path, help="captured dumpbin or GNU nm POSIX output")
    args = parser.parse_args()

    if args.format == "dumpbin":
        flags = ("/NOLOGO", "/LINKERMEMBER:2")
        parse_symbols = parse_dumpbin
    else:
        flags = ("--defined-only", "--extern-only", "--format=posix", "--no-demangle")
        parse_symbols = parse_nm

    try:
        if args.symbols_from is not None:
            output = args.symbols_from.read_text(encoding="utf-8-sig")
        else:
            result = subprocess.run(
                (args.tool or args.format, *flags, str(args.archive.resolve())),
                capture_output=True, encoding="utf-8", errors="replace", check=True, timeout=60,
            )
            output = result.stdout
        symbols = parse_symbols(output)
        expected = set(read_allowlist())
    except subprocess.CalledProcessError as error:
        print(f"{error}\n{error.stderr.strip() or error.stdout.strip()}", file=sys.stderr)
        return 1
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1

    exports = {name for name in symbols if name.startswith("sakura_")}
    unknown = {
        name for name in symbols - exports
        if not any(pattern.fullmatch(name) for pattern in RUNTIME_SYMBOLS)
    }
    missing = expected - exports
    extra = exports - expected
    for label, names in (
        ("Missing export", missing),
        ("Unexpected export", extra),
        ("Unknown public symbol", unknown),
    ):
        for name in sorted(names):
            print(f"{label}: {name}", file=sys.stderr)
    if missing or extra or unknown:
        return 1
    print(f"Native Rust C ABI matches the allowlist ({len(exports)} symbols).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
