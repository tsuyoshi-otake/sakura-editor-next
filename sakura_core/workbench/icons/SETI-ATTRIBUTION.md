# Seti Icon Theme Attribution

`vs-seti` is the file icon theme Visual Studio Code selects when the user has
chosen none. This product bundles the same artwork and the same associations as
a first-party built-in, because "the default file icons look like VS Code's" is
not the goal — *being* VS Code's default is.

Two upstream artifacts are redistributed here, both derived from
[`jesseweed/seti-ui`](https://github.com/jesseweed/seti-ui) and both **MIT**,
Copyright (c) 2014 Jesse Weed. Microsoft redistributes the same artwork under
the same notice in `extensions/theme-seti/ThirdPartyNotices.txt` and records the
provenance in `extensions/theme-seti/cgmanifest.json`. The full permission text
is beside this file as `SETI-LICENSE` and is installed as
`license\seti\SETI-LICENSE`.

Seti includes marks that belong to their owners (Python, Docker, React, and
others). MIT licenses copyright, not trademarks, so the glyphs ship unmodified
and attributed, exactly as upstream ships them. Do not redraw or re-colour one.

## Upstream pin

| Item | Value |
| --- | --- |
| Source project | [`jesseweed/seti-ui`](https://github.com/jesseweed/seti-ui) @ [`2d6c5e68b4ded73c92dac291845ee44e1182d511`](https://github.com/jesseweed/seti-ui/commit/2d6c5e68b4ded73c92dac291845ee44e1182d511) |
| Redistributed through | [`microsoft/vscode`](https://github.com/microsoft/vscode) @ `a1c7d1be7ebeddac39ee87a311d940b04b2e5da2`, `extensions/theme-seti/` |
| License | MIT, Copyright (c) 2014 Jesse Weed |

The commit above is the value of the theme document's own `version` field, which
is how `extensions/theme-seti/build/update-icon-theme.js` records the seti-ui
revision it generated the document from.

## Bundled `seti.ttf`

| Item | Value |
| --- | --- |
| File taken | `extensions/theme-seti/icons/seti.woff` |
| Upstream size | 37,284 bytes |
| Upstream SHA-256 | `b127762058f89b37b08d76185e3b0558f5c28859e39a3840a27cb28b9739d6e2` |
| Local path | `sakura_core/workbench/icons/seti.ttf` |
| Size | 56,592 bytes |
| SHA-256 | `8a24a705f11a671bbc50d0957309faf9ea5f24c2e0155cce8adbf0032e0da812` |
| Family name (`name` nameID 1) | `seti` |
| Modification | **Container only.** No glyph, metric, colour, or name is changed. |

Upstream ships WOFF1, and neither GDI nor DirectWrite reads that container. WOFF1
is an sfnt whose tables are individually zlib-compressed, so removing the
container is lossless and reversible:

```
py -3 tools/generate-seti-icon-theme.py font <seti.woff> sakura_core/workbench/icons/seti.ttf
```

The subcommand refuses to write a file it cannot vouch for. It compares every
sfnt table before and after and requires them to be byte-for-byte identical,
with one allowance: `head` may differ **only** at offsets 8–11, its
`checkSumAdjustment`, which is computed over the whole sfnt and therefore cannot
survive a container change. The observed result is 11 tables preserved
(`GSUB`, `OS/2`, `cmap`, `glyf`, `head`, `hhea`, `hmtx`, `loca`, `maxp`, `name`,
`post`), 162 glyphs, 1000 units per em, and `head` differing at offsets 9, 10,
and 11 alone. Re-running the command reproduces the committed bytes exactly.

The font is embedded in the executable as a named `RCDATA` resource (`SETIFONT`,
declared in `sakura_core/sakura_rc.rc2`) and registered process-privately with
`AddFontMemResourceEx` by `CSetiFont.cpp`. It is never installed into the system
font collection and never written to disk. If registration fails, the Explorer
falls back to the first-party Codicon association table rather than drawing Seti
code points in some unrelated face.

## Generated `SetiIconThemeTable.h`

| Item | Value |
| --- | --- |
| File taken | `extensions/theme-seti/icons/vs-seti-icon-theme.json` |
| Size | 54,732 bytes |
| SHA-256 | `2088843e6f00a81870ed2890132fb658cf33406177a5dd58c9c7001e5184ca2a` |
| Also read | `contributes.languages` from 96 built-in `extensions/*/package.json` |

```
py -3 tools/generate-seti-icon-theme.py table <vs-seti-icon-theme.json> <extensions-dir> sakura_core/workbench/icons/SetiIconThemeTable.h
```

The header contains icon *code points*, *colours*, and *association keys* — the
data of the theme document, not artwork. The upstream document has 383
`iconDefinitions`, 238 `fileExtensions`, 101 `fileNames`, 83 `languageIds`, one
`file` default, and a `light` section overriding colours only. The generated
header has 191 distinct styles, 168 file names, and 512 file extensions; the
growth comes from folding the language layer in, described below.

VS Code resolves a file icon through CSS selectors, and the precedence is a
consequence of their specificity rather than an explicit list
(`src/vs/workbench/services/themes/browser/fileIconThemeData.ts`,
`collectSelectors`): whole file name, then the longest dotted extension suffix,
then the language id, then the default `file` icon. This product retired the
extension host and so has no language registry to match `languageIds` against.
The generator therefore folds each Seti-known language into the extensions and
file names that language claims, weakest layer first, so a directly named key
still wins. Without that fold almost none of this repository's own file types
would resolve: the theme's `fileExtensions` section names none of `cpp`, `h`,
`md`, `json`, `py`, `ps1`, `bat`, `ts`, `yml`, or `xml`. The generator fails if
two icon-carrying languages claim the same key; the pinned inputs produce zero
such conflicts.

The `light` section is emitted under the `.vs` body class alone, and Seti
contributes no `highContrast` section, so High Contrast **and** High Contrast
Light both use the base section. `EIconVariant::Light` therefore means exactly
`ColorThemeKind.Light`, not "the background is bright".

`src/test/cpp/tests1/test-csetifont.cpp` re-verifies on every test run that each
style in the generated header has a real glyph in the actually-embedded bytes,
so a table generated from a different version of the theme fails loudly instead
of drawing `.notdef` in every Explorer row.
