# Icon Font Guidance (`contributes.icons` and the built-in codicon vocabulary)

## Scope

This directory owns everything that turns a `$(name)` into drawable glyph, in
the `workbench::icons` namespace:

| File | Responsibility |
| --- | --- |
| `CExtensionIconFont.h`/`.cpp` | The `contributes.icons` extension contribution point |
| `CCodiconFont.h`/`.cpp` + `codicon.ttf` + `CodiconGlyphTable.h` | VS Code's built-in codicon vocabulary |
| `ThemeIconResolver.h` | The pure joining step both call sites share |
| `CodiconsActivityIcons.h` | Vector geometry for icons drawn as GDI paths, not text |
| `CFileIconThemeRegistry.h`/`.cpp` | `contributes.iconThemes`, selection lookup, association precedence, and registered file-icon resources |
| `LabelRunPainter.h` | Shared vocabulary for measuring/drawing `$(icon)`-mixed labels; the status bar, the hover widget, the SCM tool, and the banner host share the same implementation instead of each keeping a copy. `CreateLabelRunGlyphFont` fails closed (`nullptr`) on a face name at or past `LF_FACESIZE` rather than truncating it — consolidating the hover widget onto it changed that widget from silent truncation to fail-closed |

`CExtensionIconFont.h`/`.cpp` reads an extension's `package.json`,
decodes the referenced icon font (WOFF1 or a bare TTF/OTF/TTC), registers it
process-privately via `AddFontMemResourceEx`, and resolves `(extensionId,
iconId)` or a bare `iconId` to a `{ faceName, glyph }` pair that a caller can
hand straight to `LOGFONTW` / `DrawTextW`. Most of this file documents that
component; the built-in vocabulary has its own section below.

It depends on `CZipFile::InflateZlibStream` (`sakura_core/io/CZipFile.h`) for
WOFF1's zlib-compressed tables, and on nothing else outside the C++ standard
library, picojson, and Win32. It does not depend on
`sakura_core/extension/CExtensionManager` (see "Independence from
CExtensionManager" below), and it does not touch `CMainStatusBar`/`CEditWnd`,
which own calling this registry from the status bar.

## Public Surface

```cpp
bool RegisterExtension(std::wstring_view extensionId, const std::filesystem::path& extensionRoot);
std::optional<SExtensionContributedIcon> Find(std::wstring_view extensionId, std::wstring_view iconId) const;
std::optional<SExtensionContributedIcon> Find(std::wstring_view iconId) const; // グローバル解決
void UnregisterExtension(std::wstring_view extensionId);
void Clear();
```

`extensionId` is an opaque caller-supplied key. `RegisterExtension` never reads
or validates it against the manifest's own `publisher`/`name` fields — it is
used purely as a lookup key for `Find`/`UnregisterExtension`. A
directory-derived id computed elsewhere (e.g. by an extension installer) is a
perfectly valid input, as long as the same string is reused consistently for
the same extension across register/find/unregister calls for its lifetime.

## Global Icon Registry (VS Code `IconRegistry` Compatibility)

Real VS Code's `contributes.icons` does **not** namespace icon ids per
extension: `platform/theme/common/iconRegistry.ts`'s `registerIcon()` inserts
into one process-wide registry keyed by id alone, and `ThemeIcon.fromString
("$(id)")` resolves that id regardless of which extension is rendering it. On
a duplicate id, upstream keeps the **first** registration and logs an error;
it does not overwrite.

`Find(std::wstring_view iconId)` reproduces this:

- Global across every registered extension, keyed by icon id alone.
- Deterministic first-registration-wins on a duplicate id (implemented with a
  monotonic registration-sequence number per candidate; the lowest surviving
  sequence for an id wins).
- If the winning extension is later removed (`UnregisterExtension`/`Clear`),
  the id automatically **transfers** to the next-oldest surviving registrant
  rather than resolving to `nullopt` or a dangling font reference. This is
  exercised by
  `CExtensionIconFontRegistry.GlobalFindTransfersToNextRegistrantWhenWinnerUnregisters`
  in `src/test/cpp/tests1/test-cextensioniconfont.cpp`.

The namespaced `Find(extensionId, iconId)` is a separate, independent API: it
always answers "what did this specific extension declare for this id", is
unaffected by which extension currently wins the global lookup, and keeps
working after another extension's competing declaration is registered or
removed. A caller resolving VS Code's `$(name)` status-bar syntax should use
the global one-argument `Find`; a caller inspecting one extension's own
contribution should use the two-argument one.

**Known edge case, accepted as-is**: `RegisterExtension` re-registering an
*already-registered* `extensionId` first fully tears down that extension's
existing state (including its global candidacies) and then rebuilds it from
scratch with brand-new (larger) sequence numbers — this reload-as-full-rebuild
behavior predates this feature and is shared by the namespaced API too. A
consequence specific to the global registry: if extension A registered id X
first and was winning, and extension B later also declared X (shadowed, not
winning), then **reloading A** (not unregistering it) gives A's candidacy for
X a new, larger sequence number than B's — so B, which never lost its
candidacy, becomes the new winner of X after A's reload. This differs from a
hypothetical "reload preserves original priority" semantics. It is judged
low-risk (native extension manifests are not hot-reloaded during normal
operation) and is intentionally not special-cased; only the
coordinator-specified "winner is unregistered, not reloaded" transfer case is
guaranteed and tested.

## Unsupported: WOFF2

`detail::LoadFontAsSfnt` recognizes the `wOF2` magic and fails closed with the
distinct `EFontDecodeError::UnsupportedWoff2` reason without ever attempting a
decode. WOFF2 (Brotli-compressed) is a different format from WOFF1 (zlib) and
is out of scope for this feature. An extension shipping only a WOFF2 icon font
will have that icon silently skipped (per-icon failure, not extension-wide
failure — see `RegisterExtension`'s per-icon-tolerant contract in
`CExtensionIconFont.h`), and the caller's existing fallback glyph applies.

## File Icon Themes: Supported Boundary

`CFileIconThemeRegistry` implements the file-icon-theme contribution point
(`contributes.iconThemes`) and the `workbench.iconTheme` selection contract.
It discovers JSONC theme documents from enabled extension roots, resolves
`fileNames`, `fileExtensions`, `folderNames`, root-folder associations, light
and high-contrast variants, and keeps registered font resources alive for the
Explorer. The command and picker use the stable VS Code command id
`workbench.action.selectIconTheme`.

The native projection currently supports ordinary raster image files and
registered icon fonts. Unsupported image formats such as SVG, unsupported
font formats such as WOFF2, malformed documents, missing definitions, and
paths outside an extension root fail closed; they do not become placeholder
icons or silently reuse an unrelated icon. Advanced association forms that
are not represented by the current native Explorer contract remain
unsupported until their semantics can be implemented and tested.

## Unsupported: `"default"` as a String Alias

VS Code's `contributes.icons` schema accepts **two** shapes for an icon's
`default`: the font-reference object (`{ "fontPath", "fontCharacter" }`) and a
plain **string** naming another already-registered icon id, which registers the
new id as an alias of that icon rather than as a new glyph.

`RegisterExtension` implements only the object form. An icon whose `default` is
a string is skipped by the `!defaultIt->second.is<picojson::object>()` guard in
`CExtensionIconFont.cpp` — the same per-icon-tolerant path that skips a broken
font, so the rest of the extension's icons still register. The registry then
answers `nullopt` for that id and the caller falls back exactly as it does for
any other unresolved id.

This fails closed rather than approximating: resolving an alias correctly means
following it to its target, which may be another extension's contribution
(registration-order dependent) or a built-in codicon (a vocabulary this registry
deliberately does not know — see the next section). Returning the alias target's
*name* as if it were a face name, or silently substituting an unrelated glyph,
would be exactly the "fake a capability so the pixels look right" failure the
repository's top-level rule forbids. Note that when the alias target happens to
be a built-in codicon name, the caller's own fallback to the bundled
`codicon.ttf` already draws the right glyph — but that is the fallback working
as designed, not alias support.

Implementing this properly requires a resolution pass that runs after all
extensions have registered (so target order stops mattering) and a defined
answer for alias cycles and dangling targets. Do not add a naive
resolve-at-parse-time lookup.

## Built-in Codicon Vocabulary Is a Separate, Owned Boundary

This registry only ever answers for icon ids that were actually declared via
some extension's `contributes.icons`. VS Code's own built-in codicon
vocabulary (`$(check)`, `$(close)`, etc., defined natively in VS Code rather
than contributed by any extension) is resolved by
[`ThemeIconResolver.h`](ThemeIconResolver.h) beside this registry. That header
holds the *combining* step only: given a `$(icon-id)`, it asks this registry
first (matching upstream's global `IconRegistry` lookup) and resolves the
built-in vocabulary otherwise. It is a header of pure functions with
no state of its own, so both call sites that need the vocabulary —
`CMainStatusBar`'s `StatusBarItem.text` renderer and
[`../hover/CHoverWidget.cpp`](../hover/CHoverWidget.cpp)'s inline icon runs —
share one answer instead of each keeping a private copy. Do not attempt to make
this registry itself aware of the built-in codicon set; keep the two
vocabularies (native built-in vs. extension-contributed) in their existing
separate owners, and keep the resolver the only place that joins them.

### The built-in vocabulary is the bundled `codicon.ttf`, as in real VS Code

Real VS Code ships the whole `codicon.ttf` and draws every built-in `$(name)`
as one glyph of that font. This product does the same rather than approximating
the vocabulary with a hand-picked set of vector icons:

- [`codicon.ttf`](codicon.ttf) is npm `@vscode/codicons@0.0.46-24`'s
  `dist/codicon.ttf`, redistributed byte-verbatim. Provenance, size, SHA-256,
  and licensing are in [`CODICONS-ATTRIBUTION.md`](CODICONS-ATTRIBUTION.md).
- It is embedded in the executable as the named `CODICONFONT` `RCDATA` resource
  declared in `sakura_core/sakura_rc.rc2`, not shipped as a loose file. One
  missing file next to the executable must not be able to silently turn every
  built-in icon into something else. `sakura_rc.rc2` is included by
  `sakura_rc.rc`, so both MSBuild and CMake/MinGW compile it with no
  project-file change. **Declare it there and nowhere else** — `tests1.exe`
  links `sakura.vcxproj`'s objects *and* its `.res`, so a second declaration in
  `tests1_rc.rc` fails the link with `CVT1100 重複するリソース` / `LNK1123`.
- [`CCodiconFont.h`](CCodiconFont.h)/`.cpp` is a Meyers singleton that reads
  that resource and registers it process-privately, deliberately reusing this
  component's own `detail::LoadFontAsSfnt` →
  `detail::EnsureUniqueFontIdentifier` → `detail::ExtractFamilyName` →
  `detail::CRegisteredMemoryFont` pipeline instead of adding a second font
  path. The face name is read from the font's own `name` table (measured:
  `codicon`), never hardcoded at the call site.
- [`CodiconGlyphTable.h`](CodiconGlyphTable.h) is generated one-to-one from the
  same package's `dist/codiconsLibrary.ts` — the `register('<name>', 0x<cp>)`
  list upstream's `vs/base/common/codicons.ts` builds `Codicon` from — so all
  746 names resolve, aliases included. Do not hand-edit it, and do not
  regenerate it from `codicon.csv`: the CSV lists 630 primary names only and
  omits aliases such as `zap`, which `odangoo.otak-usage` actually uses.
- `CCodiconFont::FaceName()` is empty when the registration failed. That is the
  only condition under which `ThemeIconResolver` reaches the imported vectors or
  the substitute dot for a built-in `$(name)`; it fails closed rather than
  drawing an unrelated glyph. `CCodiconFont.EveryTableEntryHasARealGlyphInTheRegisteredFont`
  in `src/test/cpp/tests1/test-ccodiconfont.cpp` asserts through GDI that every
  one of the 746 code points has a real (non-`.notdef`) glyph in the bytes that
  actually shipped, so a table/font version drift cannot pass unnoticed.

[`CodiconsActivityIcons.h`](CodiconsActivityIcons.h)'s imported vector paths
remain the GDI fallback for built-in icons when the bundled font cannot be
registered. The Activity Bar and native title bar normally draw their matching
codicon.ttf glyphs, preserving the same anti-aliased text rendering and normal
weight across every built-in icon. The tab strip continues to use its vector
path where that component owns a GDI-specific rendering path.

Verified live on 2026-08-01 with the x64 Debug build and both `odangoo.otak-usage`
and `odangoo.otak-monitor` installed: `$(circle-slash)`, `$(zap)`, `$(copy)`,
`$(gear)`, `$(rocket)`, and `$(list-flat)` all draw their real codicon shapes in
the status bar hovers. Every one of those had previously drawn the substitute dot.

## Divergence: Direct File I/O Instead of `IFileService`

`sakura_core/io/CLAUDE.md` directs new workbench/extension code to consume
`IFileService`/`IFileSystemProvider` rather than local file APIs directly.
This component reads the manifest and font file with `std::ifstream` /
`std::filesystem` directly instead, for two reasons:

1. It follows the existing `CExtensionManager::ReadDisplayName` precedent:
   extensions are always installed to a local, already-resolved directory by
   the extension installer before any of this code runs — there is no virtual
   or remote scheme in play for `contributes.icons` today.
2. The WOFF1 decode / `AddFontMemResourceEx` registration pipeline is
   fundamentally Win32-native binary handling, not a filesystem-provider
   shaped read.

If extensions are ever installed to a non-local, provider-backed scheme, this
divergence should be revisited together with `CExtensionManager`'s own local
I/O.

## Divergence: No Dependency on `CExtensionManager` / `extension/`

This component does not import any type from `sakura_core/extension/`. The
`package.json` key names (`contributes`, `icons`, `default`, `fontPath`,
`fontCharacter`) and the on-disk manifest filename are hardcoded literals in
`CExtensionIconFont.cpp`, duplicating what `CExtensionManager` already knows
about manifest shape, rather than sharing a schema type. This is intentional:
`sakura_core/extension/` is explicitly out of scope for this feature's
changes, and keeping `workbench/` from depending on `extension/` preserves the
acyclic dependency direction (integration-layer code must not become a
dependency of core/workbench abstractions). If `CExtensionManager` grows a
shared manifest-schema type in the future, revisit this duplication.

## Divergence: Simplified Macintosh `name` Table Decoding

`detail::ExtractFamilyName`'s fallback path for `platformID=1` (Macintosh)
`name` records decodes each byte as a Latin-1 code point
(`static_cast<wchar_t>(byte)`), not true Apple Mac Roman encoding (whose high
byte range maps to different Unicode code points than Latin-1). This is
accepted because Windows-platform (`platformID=3`, `encodingID=1`) records are
the overwhelmingly common case for fonts shipped in a Windows-targeted VSIX,
and this fallback only engages when no Windows record exists at all. A font
whose family name relies on non-ASCII Mac Roman high bytes with no Windows
name record would get a mis-decoded family name string.

## Untested Size-Limit Boundaries

`EFontDecodeError::FileTooLarge`, `TableTooLarge`, and
`ReconstructedFontTooLarge` are implemented (fixed byte-count ceilings in
`CExtensionIconFont.cpp`) but are not exercised by dedicated unit tests in
`test-cextensioniconfont.cpp`, because reproducing them requires allocating
tens-of-megabytes-sized buffers per test case. The surrounding numeric-boundary
logic (offset/length range checks, table count checks, header-length checks)
is covered by tests that exercise the same comparison machinery at a much
smaller scale (`TableOutOfRange`, `TableCountOutOfRange`, `HeaderTooShort`).
This is an intentionally scoped test gap, not an oversight.

## Divergence: GDI Requires `name` nameID 3, Chromium Does Not (2026-08-01)

`AddFontMemResourceEx` rejects — returns `0` for — an otherwise valid sfnt whose
`name` table contains no **nameID 3 (Unique font identifier)** record at all.
Measured on this machine (Windows 11 Pro 26200) by controlled bisection: a font
carrying only nameIDs 1/2/4/6 registers with `handle == 0`; adding one nameID 3
record and changing nothing else makes the identical bytes register
successfully, `GetTextFaceW` return the real family name, and `DrawTextW` draw
the real glyph. Three plausible alternative causes were each ruled out by
experiment and are **not** the trigger: `OS/2.fsSelection == 0`, all-zero
`OS/2` Unicode-range and codepage bits, and a stale `head.checkSumAdjustment`.

Chromium — and therefore real VS Code — accepts such a font. This is not an
exotic input: IcoMoon and similar icon-font generators routinely emit only
nameIDs 1/2/4/6, and `odangoo.otak-usage`'s `images/otak-usage-icons.woff` is
exactly that shape. Without a repair, the whole registration fails, the icon is
skipped per the per-icon-tolerant contract, `Find` answers `nullopt`, and
`ThemeIconResolver` falls back to the substitute dot — so an extension whose
icon renders correctly in real VS Code renders as a dot here. That is precisely
the kind of platform-caused divergence this file exists to record.

`detail::EnsureUniqueFontIdentifier` closes it: when no nameID 3 exists, it
clones the font's **own** existing name and re-emits it as nameID 3, then
rebuilds the `name` table and reassembles the sfnt (tags sorted, bodies 4-byte
aligned, that table's checksum and `head.checkSumAdjustment` recomputed).
Constraints that are deliberate, not incidental:

- The source is only ever a record the font already declares, tried in the order
  Windows nameID 1 → 6 → 4, then Macintosh nameID 1 → 6 → 4. Windows first
  because that is the platform record GDI actually reads. A font declaring none
  of those is **skipped**, never given an invented identifier.
- Every other table is copied byte-for-byte. Only `name` is rewritten, and only
  `head.checkSumAdjustment` changes outside it — mandatory, because the table
  offsets moved and a retained value would be self-contradictory.
- `name` records are re-sorted by `(platformID, encodingID, languageID, nameID)`
  as the spec requires; appending the synthesized record without sorting would
  produce a table GDI may reject for a different reason.
- TTC input (`ttcf`), an absent `name` table, and an unparsable `name` table all
  return `Skipped` with the buffer untouched. A collection is a different
  container layout and cannot be reassembled as a single sfnt.

The repair runs in `RegisterExtension`, **between** `detail::LoadFontAsSfnt` and
`detail::ExtractFamilyName` — not inside `LoadFontAsSfnt`, which is contractually
a byte-verbatim passthrough for the TTF/OTF/TTC magics and is tested as such by
`CExtensionIconFontLoadFontAsSfnt.PassesThroughSfntMagicsVerbatim`. The behavior
above is covered by the `CExtensionIconFontEnsureUniqueFontIdentifier` suite in
`src/test/cpp/tests1/test-cextensioniconfont.cpp`, which asserts it purely on
byte layout and never calls GDI.

This is a minimal compatibility repair for one measured GDI requirement, not a
font fixer. Do not grow it into a general "make any rejected font acceptable"
pass: another rejection cause must be measured and bisected the same way before
anything else is synthesized.

## WOFF1 Fields Never Validated

`detail::DecodeWoff1` only reads `flavor` (offset 4) and `numTables` (offset
12) from the 44-byte WOFF1 header. `length`, `totalSfntSize`,
`majorVersion`/`minorVersion`, and the metadata/private-data offsets are never
read or validated. Each table directory entry's `origChecksum` is copied
verbatim into the rebuilt sfnt table directory; it is never recomputed or
checked against the actual table bytes. Structural validity beyond "the
offsets/lengths fit inside the input buffer" is left to `AddFontMemResourceEx`
(TTF/OTF/TTC passthrough) or to GDI's own font parser after registration —
this component's job is bounds-safety against untrusted extension data, not
full WOFF/sfnt conformance checking.
