#!/usr/bin/env python3
"""Turn Microsoft's bundled Seti icon theme into this repository's first-party form.

`vs-seti` is VS Code's *default* file icon theme. This product retired the extension
host, so it cannot load a contributed icon theme at runtime; it bundles the same
artwork as a built-in instead. Two upstream artifacts are converted here:

  font   seti.woff -> seti.ttf
         WOFF1 is an sfnt whose tables are individually zlib-compressed. Neither GDI
         nor DirectWrite reads WOFF, so the container is removed. The subcommand
         asserts the transform is lossless: every sfnt table must come out byte for
         byte identical except `head`, which may differ only in checkSumAdjustment.

  table  vs-seti-icon-theme.json + extensions/*/package.json -> SetiIconThemeTable.h
         VS Code resolves a file icon through CSS selectors whose specificity fixes
         the order fileNames > fileExtensions (longest dotted suffix first) >
         languageIds > the default `file` icon
         (src/vs/workbench/services/themes/browser/fileIconThemeData.ts,
         `collectSelectors`). This product has no language registry, so the
         languageIds layer is folded into the extension/name tables at generation
         time, weakest layer first, which preserves that order.

See sakura_core/workbench/icons/SETI-ATTRIBUTION.md for the pinned upstream commit,
how to obtain the inputs, and the exact invocations.

Requires fontTools for `font` only; `table` uses the standard library.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

INHERIT = 0xFFFFFFFF
#: `head.checkSumAdjustment` is computed over the whole sfnt, so a WOFF container and
#: the sfnt unpacked from it necessarily disagree here and nowhere else.
HEAD_CHECKSUM_ADJUSTMENT_RANGE = range(8, 12)


def convert_font(source: str, destination: str) -> int:
    from fontTools.ttLib import TTFont

    font = TTFont(source, lazy=True, recalcTimestamp=False)
    if font.flavor != "woff":
        print("error: {0} is not WOFF1 (flavor={1!r})".format(source, font.flavor), file=sys.stderr)
        return 1
    font.flavor = None
    font.save(destination)

    before, after = TTFont(source, lazy=True), TTFont(destination, lazy=True)
    if set(before.reader.keys()) != set(after.reader.keys()):
        print("error: the sfnt table set changed", file=sys.stderr)
        return 1
    for tag in sorted(before.reader.keys()):
        source_bytes, result_bytes = before.reader[tag], after.reader[tag]
        if source_bytes == result_bytes:
            continue
        differing = [i for i, (a, b) in enumerate(zip(source_bytes, result_bytes)) if a != b]
        if (tag != "head" or len(source_bytes) != len(result_bytes)
                or any(offset not in HEAD_CHECKSUM_ADJUSTMENT_RANGE for offset in differing)):
            print("error: table {0!r} is not preserved (offsets {1})".format(tag, differing[:8]),
                  file=sys.stderr)
            return 1
        print("  head differs only at checkSumAdjustment offsets {0}".format(differing))
    print("wrote {0} ({1} bytes), {2} tables preserved".format(
        destination, os.path.getsize(destination), len(before.reader.keys())))
    return 0


def read_language_associations(extensions_dir):
    """Map extension and file name to the language id that claims it, from `contributes.languages`."""
    extensions = {}
    names = {}
    conflicts = []
    for path in sorted(glob.glob(os.path.join(extensions_dir, "*.json"))):
        with open(path, encoding="utf-8") as handle:
            package = json.load(handle)
        for language in package.get("contributes", {}).get("languages", []) or []:
            language_id = language.get("id")
            if language_id is None:
                continue
            for target, keys, strip in ((extensions, language.get("extensions"), True),
                                        (names, language.get("filenames"), False)):
                for raw in keys or []:
                    key = (raw.lstrip(".") if strip else raw).lower()
                    if not key:
                        continue
                    owner = target.setdefault(key, language_id)
                    if owner != language_id:
                        conflicts.append(("extension" if strip else "filename", key, owner, language_id))
    return extensions, names, conflicts


def generate_table(theme_path: str, extensions_dir: str, out_path: str) -> int:
    with open(theme_path, encoding="utf-8") as handle:
        theme = json.load(handle)
    definitions = theme["iconDefinitions"]

    def style_of(association, key, dark_id):
        """Resolve one association into (glyph, dark color, light color).

        Upstream emits the `light` section under the `.vs` body class only, so a key the
        light section omits keeps the base definition in every theme kind.
        """
        light_id = (association or {}).get(key, dark_id)
        dark, light_definition = definitions[dark_id], definitions[light_id]
        character = dark["fontCharacter"]
        if not character.startswith("\\") or light_definition["fontCharacter"] != character:
            raise ValueError("{0!r}: the light variant is not the same glyph".format(key))

        def color(entry):
            value = entry.get("fontColor")
            if value is None:
                return INHERIT
            if not value.startswith("#") or len(value) != 7:
                raise ValueError("{0!r}: unsupported fontColor {1!r}".format(key, value))
            return int(value[1:], 16)

        return int(character.lstrip("\\"), 16), color(dark), color(light_definition)

    language_extensions, language_names, conflicts = read_language_associations(extensions_dir)

    # Upstream: `if (!languageIds.jsonc && languageIds.json) { languageIds.jsonc = languageIds.json; }`
    language_ids = dict(theme["languageIds"])
    light_language_ids = dict(theme.get("light", {}).get("languageIds", {}))
    if "jsonc" not in language_ids and "json" in language_ids:
        language_ids["jsonc"] = language_ids["json"]
        if "json" in light_language_ids:
            light_language_ids["jsonc"] = light_language_ids["json"]

    # Fold the weakest layer first so a stronger layer overwrites the same key.
    extensions = {}
    names = {}
    for target, language_map in ((extensions, language_extensions), (names, language_names)):
        for key, language_id in sorted(language_map.items()):
            if language_id in language_ids:
                target[key] = style_of(light_language_ids, language_id, language_ids[language_id])
    light = theme.get("light", {})
    for target, section in ((extensions, "fileExtensions"), (names, "fileNames")):
        for key, dark_id in theme[section].items():
            target[key.lower()] = style_of(light.get(section), key, dark_id)

    styles = []
    style_index = {}

    def intern(style):
        if style not in style_index:
            style_index[style] = len(styles)
            styles.append(style)
        return style_index[style]

    default_style = intern(style_of(light, "file", theme["file"]))
    name_rows = sorted((key, intern(style)) for key, style in names.items())
    extension_rows = sorted((key, intern(style)) for key, style in extensions.items())
    for key, _ in name_rows + extension_rows:
        if not all(0x20 <= ord(character) < 0x7F for character in key) or '"' in key or "\\" in key:
            raise ValueError("association key {0!r} is not plain ASCII".format(key))

    lines = []

    def emit(text=""):
        lines.append(text)

    emit("/*! @file")
    emit("\t@brief Bundled Seti file icon theme table, generated by tools/generate-seti-icon-theme.py")
    emit("")
    emit("\tDo not edit by hand. Regenerate from VS Code's own")
    emit("\t`extensions/theme-seti/icons/vs-seti-icon-theme.json` and its built-in language")
    emit("\tcontributions; SETI-ATTRIBUTION.md beside this file pins the upstream commit,")
    emit("\tthe inputs, and the invocation.")
    emit("")
    emit("\tThe language layer is already folded in. VS Code's `languageIds` section only")
    emit("\tmatches through its language registry, which this product does not have, and CSS")
    emit("\tspecificity makes it the weakest of the three association layers, so a language")
    emit("\tassociation appears here as the extensions and file names that language claims,")
    emit("\toverwritten wherever the theme names the same key directly.")
    emit("*/")
    emit("/*")
    emit("\tCopyright (C) 2026, Sakura Editor Organization")
    emit("")
    emit("\tSPDX-License-Identifier: Zlib")
    emit("*/")
    emit("#pragma once")
    emit("")
    emit("#include <cstdint>")
    emit("#include <string_view>")
    emit("")
    emit("namespace workbench::icons::seti {")
    emit()
    emit("//! One icon: its glyph in seti.ttf and its color in each theme kind, as 0x00RRGGBB.")
    emit("struct SIconStyle {")
    emit("\tconst wchar_t character{};")
    emit("\tconst std::uint32_t darkColor{};")
    emit("\tconst std::uint32_t lightColor{};")
    emit("};")
    emit()
    emit("//! One association key and the style it selects.")
    emit("struct SAssociation {")
    emit("\tconst std::wstring_view key{}; //!< Lowercase ASCII, as VS Code lowercases the file name first.")
    emit("\tconst std::uint16_t style{};   //!< Index into kIconStyles.")
    emit("};")
    emit()
    emit("//! Upstream's `_todo` carries no fontColor, so its rows keep the view's own text color.")
    emit("inline constexpr std::uint32_t kInheritColor = 0xFFFFFFFFu;")
    emit()
    emit("//! The {0} distinct styles the associations below resolve to.".format(len(styles)))
    emit("inline constexpr SIconStyle kIconStyles[] = {")
    for character, dark, light_color in styles:
        dark_text = "kInheritColor" if dark == INHERIT else "0x{0:06X}u".format(dark)
        light_text = "kInheritColor" if light_color == INHERIT else "0x{0:06X}u".format(light_color)
        emit("\t{{ L'\\u{0:04X}', {1}, {2} }},".format(character, dark_text, light_text))
    emit("};")
    emit()
    emit("//! Upstream's `file` association, used when no key below matches.")
    emit("inline constexpr std::uint16_t kDefaultStyle = {0};".format(default_style))
    emit()
    emit("//! Whole file names, ascending so a binary search can find one. {0} rows.".format(len(name_rows)))
    emit("inline constexpr SAssociation kFileNames[] = {")
    for key, index in name_rows:
        emit('\t{{ L"{0}", {1} }},'.format(key, index))
    emit("};")
    emit()
    emit("//! Dotted extension suffixes, ascending, without the leading dot. {0} rows.".format(
        len(extension_rows)))
    emit("inline constexpr SAssociation kFileExtensions[] = {")
    for key, index in extension_rows:
        emit('\t{{ L"{0}", {1} }},'.format(key, index))
    emit("};")
    emit()
    emit("} // namespace workbench::icons::seti")
    emit()

    with open(out_path, "w", encoding="ascii", newline="\r\n") as handle:
        handle.write("\n".join(lines))

    print("wrote {0}".format(out_path))
    print("  styles          {0}".format(len(styles)))
    print("  file names      {0} ({1} from the theme)".format(len(name_rows), len(theme["fileNames"])))
    print("  file extensions {0} ({1} from the theme)".format(
        len(extension_rows), len(theme["fileExtensions"])))
    print("  language ids    {0} folded into the two tables above".format(len(language_ids)))
    contested = [row for row in conflicts if row[2] in language_ids and row[3] in language_ids]
    print("  keys claimed by two icon-carrying languages: {0}".format(len(contested)))
    for row in contested:
        print("    {0}".format(row))
    return 1 if contested else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subcommands = parser.add_subparsers(dest="command", required=True)
    font = subcommands.add_parser("font", help="decontainerise seti.woff into seti.ttf")
    font.add_argument("source", help="upstream extensions/theme-seti/icons/seti.woff")
    font.add_argument("destination", help="sakura_core/workbench/icons/seti.ttf")
    table = subcommands.add_parser("table", help="generate SetiIconThemeTable.h")
    table.add_argument("theme", help="upstream extensions/theme-seti/icons/vs-seti-icon-theme.json")
    table.add_argument("extensions", help="directory of upstream extensions/*/package.json copies")
    table.add_argument("destination", help="sakura_core/workbench/icons/SetiIconThemeTable.h")
    arguments = parser.parse_args()
    if arguments.command == "font":
        return convert_font(arguments.source, arguments.destination)
    return generate_table(arguments.theme, arguments.extensions, arguments.destination)


if __name__ == "__main__":
    sys.exit(main())
