"""Require a C++ UTF-16 backend marker in a PE version resource."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Iterator, NamedTuple


MARKER = "SakuraUtf16Backend"
RT_VERSION = 16
RESOURCE_DIRECTORY = 2
RESOURCE_FLAG = 0x80000000
RESOURCE_OFFSET = RESOURCE_FLAG - 1
MAX_SECTIONS = 96
MAX_ENTRIES = 65536


class PeFormatError(ValueError):
    """The PE or version resource has an invalid layout."""


class _Section(NamedTuple):
    rva: int
    raw_offset: int
    raw_size: int


class _VersionBlock(NamedTuple):
    key: str
    kind: int
    value: memoryview
    children_offset: int
    end: int


class _EntryBudget:
    """Bound total traversal even when multiple entries point to one block."""

    def __init__(self) -> None:
        self.remaining = MAX_ENTRIES

    def take(self, count: int) -> None:
        self.remaining -= count
        if self.remaining < 0:
            raise PeFormatError("resource entry limit exceeded")


def _span(data: memoryview, offset: int, size: int, what: str) -> memoryview:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise PeFormatError(f"{what} lies outside its containing data")
    return data[offset:offset + size]


def _resource_location(image: memoryview) -> tuple[list[_Section], int, int]:
    dos = _span(image, 0, 64, "DOS header")
    if dos[:2] != b"MZ":
        raise PeFormatError("missing MZ signature")
    pe_offset, = struct.unpack_from("<I", dos, 0x3C)
    if pe_offset < len(dos):
        raise PeFormatError("PE header overlaps the DOS header")

    header = _span(image, pe_offset, 24, "PE and COFF headers")
    if header[:4] != b"PE\0\0":
        raise PeFormatError("missing PE signature")
    section_count, = struct.unpack_from("<H", header, 6)
    optional_size, = struct.unpack_from("<H", header, 20)
    if section_count > MAX_SECTIONS:
        raise PeFormatError("PE section count exceeds the limit")

    optional_offset = pe_offset + len(header)
    optional = _span(image, optional_offset, optional_size, "optional header")
    magic, = struct.unpack("<H", _span(optional, 0, 2, "optional header magic"))
    if magic == 0x10B:
        directory_offset = 96
    elif magic == 0x20B:
        directory_offset = 112
    else:
        raise PeFormatError("optional header is neither PE32 nor PE32+")
    directory_count, = struct.unpack(
        "<I", _span(optional, directory_offset - 4, 4, "data directory count")
    )
    _span(optional, directory_offset, directory_count * 8, "data directory table")

    table_offset = optional_offset + optional_size
    table = _span(image, table_offset, section_count * 40, "section table")
    sections = []
    for entry in range(section_count):
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
            "<IIII", table, entry * 40 + 8
        )
        if rva + max(virtual_size, raw_size) > 1 << 32:
            raise PeFormatError("section RVA range overflows")
        if raw_size:
            if raw_offset < table_offset + len(table):
                raise PeFormatError("section data overlaps the PE headers")
            _span(image, raw_offset, raw_size, "section data")
            sections.append(_Section(rva, raw_offset, raw_size))

    if directory_count <= RESOURCE_DIRECTORY:
        return sections, 0, 0
    rva, size = struct.unpack_from("<II", optional, directory_offset + RESOURCE_DIRECTORY * 8)
    return sections, rva, size


def _rva_span(
    image: memoryview, sections: list[_Section], rva: int, size: int, what: str
) -> memoryview:
    offset = None
    for section in sections:
        delta = rva - section.rva
        if 0 <= delta < section.raw_size and delta + size <= section.raw_size:
            if offset is not None:
                raise PeFormatError(f"{what} RVA maps to more than one section")
            offset = section.raw_offset + delta
    if offset is None:
        raise PeFormatError(f"{what} RVA lies outside file-backed section data")
    return _span(image, offset, size, what)


def _version_resources(
    image: memoryview,
    sections: list[_Section],
    directory: memoryview,
    budget: _EntryBudget,
    offset: int = 0,
    ancestors: tuple[int, ...] = (),
    is_version: bool = False,
) -> Iterator[memoryview]:
    if offset in ancestors:
        raise PeFormatError("cyclic resource directory")
    depth = len(ancestors)
    # A PE image has three directory levels: type, name, and language.
    if depth >= 3:
        raise PeFormatError("resource directory depth exceeds the limit")

    header = _span(directory, offset, 16, "resource directory header")
    named, numeric = struct.unpack_from("<HH", header, 12)
    budget.take(named + numeric)
    entries = _span(directory, offset + 16, (named + numeric) * 8, "resource entries")
    for index, (name, target) in enumerate(struct.iter_unpack("<II", entries)):
        if bool(name & RESOURCE_FLAG) != (index < named):
            raise PeFormatError("resource name disagrees with the directory counts")
        if name & RESOURCE_FLAG:
            name_offset = name & RESOURCE_OFFSET
            words, = struct.unpack("<H", _span(directory, name_offset, 2, "resource name length"))
            _span(directory, name_offset + 2, words * 2, "resource name")

        version_entry = name == RT_VERSION if depth == 0 else is_version
        if target & RESOURCE_FLAG:
            yield from _version_resources(
                image, sections, directory, budget,
                target & RESOURCE_OFFSET, ancestors + (offset,), version_entry,
            )
        else:
            if depth != 2:
                raise PeFormatError("resource data entry is outside the language level")
            entry = _span(directory, target, 16, "resource data entry")
            data_rva, data_size = struct.unpack_from("<II", entry)
            if data_size or version_entry:
                payload = _rva_span(image, sections, data_rva, data_size, "resource data")
                if version_entry:
                    yield payload


def _aligned(offset: int) -> int:
    return (offset + 3) & ~3


def _utf16(data: memoryview, what: str) -> str:
    try:
        return data.tobytes().decode("utf-16-le")
    except UnicodeDecodeError as error:
        raise PeFormatError(f"invalid UTF-16 in {what}") from error


def _version_block(data: memoryview, offset: int, limit: int) -> _VersionBlock:
    length, value_length, kind = struct.unpack(
        "<HHH", _span(data[:limit], offset, 6, "version block header")
    )
    end = offset + length
    if length < 8 or length % 2 or end > limit or kind not in (0, 1):
        raise PeFormatError("invalid version block length or type")

    key_start = offset + 6
    for cursor in range(key_start, end - 1, 2):
        if data[cursor] == 0 and data[cursor + 1] == 0:
            key = _utf16(data[key_start:cursor], "version key")
            key_end = cursor + 2
            break
    else:
        raise PeFormatError("unterminated version key")

    # End-of-block padding is optional when there is no value or child.
    if key_end == end and value_length == 0:
        return _VersionBlock(key, kind, data[end:end], end, end)
    value_offset = _aligned(key_end)
    value_size = value_length * 2 if kind == 1 else value_length
    value = _span(data[:end], value_offset, value_size, "version value")
    return _VersionBlock(key, kind, value, value_offset + value_size, end)


def _children(
    data: memoryview, parent: _VersionBlock, budget: _EntryBudget
) -> Iterator[_VersionBlock]:
    offset = parent.children_offset
    while offset < parent.end:
        aligned = _aligned(offset)
        if any(data[offset:min(aligned, parent.end)]):
            raise PeFormatError("nonzero padding between version blocks")
        if aligned >= parent.end:
            break
        budget.take(1)
        child = _version_block(data, aligned, parent.end)
        yield child
        offset = child.end


def _version_markers(data: memoryview, budget: _EntryBudget) -> Iterator[str]:
    root = _version_block(data, 0, len(data))
    if root.key != "VS_VERSION_INFO" or root.kind != 0:
        raise PeFormatError("invalid VS_VERSION_INFO root")
    if root.value and (
        len(root.value) != 52 or struct.unpack_from("<I", root.value)[0] != 0xFEEF04BD
    ):
        raise PeFormatError("invalid VS_FIXEDFILEINFO value")
    if len(data) > _aligned(root.end) or any(data[root.end:]):
        raise PeFormatError("unexpected data after VS_VERSION_INFO")

    for info in _children(data, root, budget):
        if info.value or info.key not in ("StringFileInfo", "VarFileInfo"):
            raise PeFormatError("invalid VS_VERSION_INFO child")
        for table in _children(data, info, budget):
            if info.key == "VarFileInfo":
                if table.kind != 0 or len(table.value) % 4 or table.children_offset != table.end:
                    raise PeFormatError("invalid VarFileInfo entry")
                continue
            if table.value or len(table.key) != 8 or any(
                digit not in "0123456789abcdefABCDEF" for digit in table.key
            ):
                raise PeFormatError("invalid version StringTable")
            for entry in _children(data, table, budget):
                if entry.kind != 1 or (
                    entry.end > _aligned(entry.children_offset)
                    or any(data[entry.children_offset:entry.end])
                ):
                    raise PeFormatError("invalid version string")
                if entry.key == MARKER:
                    value = _utf16(entry.value, MARKER)
                    if value and not value.endswith("\0"):
                        raise PeFormatError(f"unterminated {MARKER} value")
                    yield value.rstrip("\0")


def read_markers(image: bytes) -> set[str]:
    """Return the distinct marker values across all version resource leaves."""
    data = memoryview(image)
    sections, resource_rva, resource_size = _resource_location(data)
    if resource_rva == resource_size == 0:
        return set()
    if not resource_rva or not resource_size:
        raise PeFormatError("incomplete resource data directory")
    directory = _rva_span(data, sections, resource_rva, resource_size, "resource directory")
    budget = _EntryBudget()
    markers = set()
    for payload in _version_resources(data, sections, directory, budget):
        markers.update(_version_markers(payload, budget))
    return markers


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="path to sakura.exe")
    args = parser.parse_args(argv)
    try:
        markers = read_markers(args.exe.read_bytes())
    except OSError as error:
        print(f"{args.exe}: cannot read executable: {error}", file=sys.stderr)
        return 2
    except PeFormatError as error:
        print(f"{args.exe}: invalid PE version resource: {error}", file=sys.stderr)
        return 2

    if not markers:
        cause = f"missing {MARKER} marker"
    elif len(markers) > 1:
        cause = f"conflicting {MARKER} markers"
    elif markers == {"rust"}:
        cause = f"{MARKER} is rust; packaging requires cpp"
    elif markers != {"cpp"}:
        value, = markers
        cause = f"unexpected {MARKER} value {value[:80]!r}; packaging requires cpp"
    else:
        print("sakura.exe UTF-16 backend marker: cpp")
        return 0
    print(f"{args.exe}: {cause}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
