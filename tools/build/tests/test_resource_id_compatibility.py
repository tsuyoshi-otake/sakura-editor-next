from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_BUILD = Path(__file__).resolve().parents[1]
if str(TOOLS_BUILD) not in sys.path:
    sys.path.insert(0, str(TOOLS_BUILD))

from sakura_build_lib.resource_id_compatibility import (  # noqa: E402
    ResourceIdContractBuilder,
    build_resource_id_baseline,
    collect_resource_source_contract,
    evaluate_resource_id_compatibility,
    load_resource_id_baseline,
    parse_resource_header,
    require_resource_id_baseline_version_advance,
    validate_resource_id_compatibility_inputs,
    write_resource_id_baseline,
)
from sakura_build_lib.runner import BuildError  # noqa: E402


def _utf16z(value: str) -> bytes:
    return value.encode("utf-16-le") + b"\0\0"


def _standard_dialog(control_id: int = 1001) -> bytes:
    header = struct.pack("<IIHhhhh", 0, 0, 1, 0, 0, 100, 50)
    variable = struct.pack("<HHH", 0, 0, 0)
    prefix = header + variable
    prefix += b"\0" * ((4 - len(prefix) % 4) % 4)
    item = struct.pack("<IIhhhhH", 0, 0, 1, 2, 20, 10, control_id)
    item += struct.pack("<HH", 0xFFFF, 0x0080)
    item += struct.pack("<H", 0)
    item += struct.pack("<H", 0)
    return prefix + item


def _standard_menu(command_id: int = 40001) -> bytes:
    return struct.pack("<HHHH", 0, 0, 0x80, command_id) + _utf16z("Open")


def _extended_menu(popup_id: int = 41000, command_id: int = 41001) -> bytes:
    header = struct.pack("<HHI", 1, 4, 0)
    popup = struct.pack("<IIIH", 0, 0, popup_id, 0x81) + _utf16z("File")
    popup += b"\0" * ((4 - (len(header) + len(popup)) % 4) % 4)
    popup += struct.pack("<I", 0)
    command = struct.pack("<IIIH", 0, 0, command_id, 0x80) + _utf16z("Open")
    return header + popup + command


def _accelerator(command_id: int = 40001) -> bytes:
    return struct.pack("<HHHH", 0x80, ord("O"), command_id, 0)


def _string_block(index: int = 7) -> bytes:
    values = bytearray()
    for current in range(16):
        text = "Hello" if current == index else ""
        encoded = text.encode("utf-16-le")
        values += struct.pack("<H", len(text)) + encoded
    return bytes(values)


def _compiled_contract(command_id: int = 40001, control_id: int = 1001) -> dict[str, object]:
    builder = ResourceIdContractBuilder()
    builder.observe({"kind": "id", "value": 5}, {"kind": "id", "value": 100}, 1041, _standard_dialog(control_id))
    builder.observe({"kind": "id", "value": 4}, {"kind": "id", "value": 101}, 1041, _standard_menu(command_id))
    builder.observe({"kind": "id", "value": 9}, {"kind": "id", "value": 102}, 1041, _accelerator(command_id))
    builder.observe({"kind": "id", "value": 6}, {"kind": "id", "value": 13}, 1041, _string_block())
    return builder.finish()


def _write_fixture(root: Path) -> tuple[Path, Path, Path]:
    header = root / "src/main/resources/sakura_rc.h"
    source = root / "sakura_core/sakura_rc.rc"
    image = root / "x64/Debug/sakura.exe"
    header.parent.mkdir(parents=True)
    source.parent.mkdir(parents=True)
    image.parent.mkdir(parents=True)
    header.write_text(
        "\n".join((
            "#define IDD_MAIN 100",
            "#define IDC_OK 1001",
            "#define IDM_OPEN 40001",
            "#define IDS_HELLO 199",
            "#define IDC_STATIC -1",
            "",
        )),
        encoding="utf-8",
    )
    source.write_text(
        """
// IDC_STATIC IDM_OPEN must not count here
IDD_MAIN DIALOGEX 0, 0, 10, 10
BEGIN
    PUSHBUTTON "IDS_HELLO", IDC_OK, 0, 0, 5, 5
END
IDM_OPEN MENU
BEGIN
    MENUITEM "Open", IDM_OPEN
END
/* IDS_HELLO */
STRINGTABLE
BEGIN
    IDS_HELLO "Hello"
END
""",
        encoding="utf-8",
    )
    image.write_bytes(b"fixture-image")
    return header, source, image


class ResourceIdCompatibilityTests(unittest.TestCase):
    def test_nested_contract_decodes_dialog_menu_accelerator_and_strings(self) -> None:
        contract = _compiled_contract()

        self.assertEqual(4, contract["counts"]["top_level"])
        self.assertEqual(1, contract["counts"]["dialogs"])
        self.assertEqual([1001], contract["dialogs"][0]["control_ids"])
        self.assertEqual([40001], contract["menus"][0]["command_ids"])
        self.assertEqual([40001], contract["menus"][0]["item_ids"])
        self.assertEqual([40001], contract["accelerators"][0]["command_ids"])
        self.assertEqual([199], contract["string_blocks"][0]["string_ids"])

    def test_extended_menu_distinguishes_popup_and_command_ids(self) -> None:
        builder = ResourceIdContractBuilder()
        builder.observe(
            {"kind": "id", "value": 4},
            {"kind": "id", "value": 101},
            1041,
            _extended_menu(),
        )
        contract = builder.finish()

        self.assertEqual("extended", contract["menus"][0]["format"])
        self.assertEqual([41001], contract["menus"][0]["command_ids"])
        self.assertEqual([41000, 41001], contract["menus"][0]["item_ids"])
        self.assertEqual(1, contract["counts"]["menu_commands"])
        self.assertEqual(2, contract["counts"]["menu_items"])

    def test_truncated_nested_template_has_typed_terminal_failure(self) -> None:
        builder = ResourceIdContractBuilder()
        with self.assertRaises(BuildError) as caught:
            builder.observe(
                {"kind": "id", "value": 5},
                {"kind": "id", "value": 100},
                1041,
                _standard_dialog()[:-1],
            )

        self.assertEqual("RESOURCE_ID_TEMPLATE_PARSE", caught.exception.code)
        self.assertEqual(5, caught.exception.exit_code)

    def test_header_preserves_aliases_and_rejects_non_numeric_defines(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = root / "resource.h"
            header.write_text("#define FIRST 7\n#define SECOND 7\n", encoding="utf-8")
            parsed = parse_resource_header(root, header)
            header.write_text("#define FIRST other\n", encoding="utf-8")
            with self.assertRaises(BuildError) as caught:
                parse_resource_header(root, header)

        self.assertEqual(2, parsed["definition_count"])
        self.assertEqual({"FIRST": 7, "SECOND": 7}, parsed["definitions"])
        self.assertEqual("RESOURCE_ID_HEADER_UNPARSED_DEFINE", caught.exception.code)

    def test_source_contract_ignores_translatable_text_and_comments(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header, source, _image = _write_fixture(root)
            definitions = parse_resource_header(root, header)["definitions"]
            before = collect_resource_source_contract(root, "ja-JP", (source,), definitions)
            source.write_text(source.read_text(encoding="utf-8").replace("Hello", "Translated"), encoding="utf-8")
            after = collect_resource_source_contract(root, "ja-JP", (source,), definitions)

        self.assertEqual(before["symbols"], after["symbols"])
        self.assertEqual(before["contract_hash"], after["contract_hash"])
        counts = before["symbols"]
        self.assertEqual(1, counts["IDC_OK"])
        self.assertEqual(2, counts["IDM_OPEN"])
        self.assertEqual(1, counts["IDS_HELLO"])

    def test_baseline_evaluation_detects_each_numeric_contract_layer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header, source, image = _write_fixture(root)
            contract = _compiled_contract()
            baseline = build_resource_id_baseline(
                root,
                header,
                {"ja-JP": (source,)},
                {"ja-JP": contract},
            )
            baseline_path = root / "tools/build/baselines/sakura_resource_ids.json"
            write_resource_id_baseline(baseline_path, baseline)
            old_time = 1_700_000_000_000_000_000
            os.utime(baseline_path, ns=(old_time, old_time))
            write_resource_id_baseline(baseline_path, baseline)
            unchanged_mtime = baseline_path.stat().st_mtime_ns

            exact = evaluate_resource_id_compatibility(
                root,
                baseline_path,
                {"ja-JP": image},
                lambda _path: contract,
            )

            source.write_text(source.read_text(encoding="utf-8").replace("IDC_OK", "IDM_OPEN"), encoding="utf-8")
            source_changed = evaluate_resource_id_compatibility(
                root,
                baseline_path,
                {"ja-JP": image},
                lambda _path: contract,
            )
            source.write_text(source.read_text(encoding="utf-8").replace("IDM_OPEN, 0, 0, 5, 5", "IDC_OK, 0, 0, 5, 5"), encoding="utf-8")

            header.write_text(header.read_text(encoding="utf-8").replace("IDC_OK 1001", "IDC_OK 1002"), encoding="utf-8")
            header_changed = evaluate_resource_id_compatibility(
                root,
                baseline_path,
                {"ja-JP": image},
                lambda _path: contract,
            )
            header.write_text(header.read_text(encoding="utf-8").replace("IDC_OK 1002", "IDC_OK 1001"), encoding="utf-8")

            image_changed = evaluate_resource_id_compatibility(
                root,
                baseline_path,
                {"ja-JP": image},
                lambda _path: _compiled_contract(command_id=40002),
            )

        self.assertEqual(old_time, unchanged_mtime)
        self.assertTrue(exact["observed"])
        self.assertIn("RESOURCE_ID_SOURCE_CONTRACT_CHANGED", {item["code"] for item in source_changed["failures"]})
        self.assertIn("RESOURCE_ID_HEADER_MAPPING_CHANGED", {item["code"] for item in header_changed["failures"]})
        self.assertIn("RESOURCE_ID_IMAGE_CONTRACT_CHANGED", {item["code"] for item in image_changed["failures"]})

    def test_baseline_and_input_tamper_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header, source, image = _write_fixture(root)
            contract = _compiled_contract()
            baseline = build_resource_id_baseline(
                root,
                header,
                {"ja-JP": (source,)},
                {"ja-JP": contract},
            )
            baseline_path = root / "tools/build/baselines/sakura_resource_ids.json"
            write_resource_id_baseline(baseline_path, baseline)
            compatibility = evaluate_resource_id_compatibility(
                root,
                baseline_path,
                {"ja-JP": image},
                lambda _path: contract,
            )
            image.write_bytes(b"changed-image")
            input_failures = validate_resource_id_compatibility_inputs(root, compatibility)

            tampered = json.loads(baseline_path.read_text(encoding="utf-8"))
            tampered["header"]["definitions"]["IDC_OK"] = 999
            baseline_path.write_text(json.dumps(tampered), encoding="utf-8")
            with self.assertRaises(BuildError) as caught:
                load_resource_id_baseline(root, baseline_path)

        self.assertIn("RESOURCE_ID_INPUT_CHANGED", {item["code"] for item in input_failures})
        self.assertEqual("RESOURCE_ID_BASELINE_HASH", caught.exception.code)

    def test_changed_baseline_requires_compatibility_version_advance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header, source, _image = _write_fixture(root)
            baseline_path = root / "tools/build/baselines/sakura_resource_ids.json"
            original = build_resource_id_baseline(
                root,
                header,
                {"ja-JP": (source,)},
                {"ja-JP": _compiled_contract()},
                compatibility_version=3,
            )
            write_resource_id_baseline(baseline_path, original)
            changed_same_version = build_resource_id_baseline(
                root,
                header,
                {"ja-JP": (source,)},
                {"ja-JP": _compiled_contract(command_id=40002)},
                compatibility_version=3,
            )
            with self.assertRaises(BuildError) as caught:
                require_resource_id_baseline_version_advance(
                    root,
                    baseline_path,
                    changed_same_version,
                )
            changed_new_version = build_resource_id_baseline(
                root,
                header,
                {"ja-JP": (source,)},
                {"ja-JP": _compiled_contract(command_id=40002)},
                compatibility_version=4,
            )
            require_resource_id_baseline_version_advance(
                root,
                baseline_path,
                changed_new_version,
            )

        self.assertEqual("RESOURCE_ID_BASELINE_VERSION_NOT_ADVANCED", caught.exception.code)


if __name__ == "__main__":
    unittest.main()
