# TextMate Grammar Engine Foundation (P4)

## Scope

`sakura_core/textmate/` implements the offline stages of a TextMate grammar
engine: a format-neutral grammar model (`TextMateGrammarModel.h`), a JSON/plist
compiler (`TextMateGrammarCompiler.*`, `TextMateJsonGrammarLoader.*`,
`TextMatePlistGrammarLoader.*`), a thin Onigmo regex wrapper
(`OnigmoRegexEngine.*`), and a line-by-line tokenizer
(`TextMateTokenizer.*`). None of this is wired into rendering yet; that is a
separate, larger effort. This directory has no dependency on `sakura_core/theme`
or on the workbench; `theme::TextMateScopeColorResolver`
(`sakura_core/theme/TextMateScopeColorResolver.h/.cpp`) depends on this
directory's *output shape* (`std::vector<std::wstring>` scope paths) only by
convention, not by `#include`, to keep `theme/` free of a compile dependency on
`textmate/` (see that header's own comment).

## Status summary

| Stage | State | Notes |
|---|---|---|
| 1. Onigmo usable from `sakura_core` | Built and verified (MSVC) | Option A below, applied to both `sakura.vcxproj` and `sakura.cmake` on 2026-08-06. Onigmo's `.c` sources now compile into `sakura_core`, and `TextMateTokenizerTest` links and passes against the real engine. |
| 2. `.tmLanguage.json` / `.tmLanguage` (plist) parsing | Implemented | `TextMateGrammarCompiler` + both loaders. See "Known gaps" for exact divergences from vscode-textmate. |
| 3. Line tokenizer | Implemented | `TextMateTokenizer::TokenizeLine`. It calls `OnigmoPattern::Compile`/`Search`, so it could not be build-verified until stage 1 landed; it now is. |
| 4. Scope-to-color boundary | Boundary defined, not wired to rendering | `theme::TextMateScopeColorResolver`. No production caller passes real `TextMateToken::scopes` into it yet. |

## Onigmo build integration (stage 1 — applied 2026-08-06)

**Option A below is what shipped**, in both `sakura_core/sakura.vcxproj`
(+`.filters`) and `src/main/cmake/sakura.cmake`. `CMakeLists.txt`'s `project()`
call had to gain the `C` language, because until Onigmo arrived the CMake build
compiled no C translation unit at all. Verified on 2026-08-06 with
`build-sln.bat x64 Debug`: 0 warnings, 0 errors, a clean no-op rebuild, and
3066 tests passing under the unattended filter (the 41 new TextMate tests
included, up from a 3025-test baseline). Two things are still unverified: the
MinGW/CMake path, and `externals/Onigmo/win32/config.h`'s portability to GCC.

Two details are load-bearing and easy to undo by accident:

- **`ONIG_EXTERN=extern` must hold at the C++ include site too**, not only on
  Onigmo's own C files. `onigmo.h` otherwise defaults it to
  `__declspec(dllimport) extern` on MSVC and the link emits nine `LNK4217`
  warnings for locally-defined-but-imported symbols. `OnigmoRegexEngine.cpp`
  therefore defines it itself immediately before the include, which is why the
  per-file project definitions do not need to be duplicated onto that file in
  two project files and CMake.
- **The Onigmo `.c` entries carry per-item include directories and defines**
  (`externals/Onigmo/win32`, `externals/Onigmo/enc/unicode`, `HAVE_CONFIG_H`,
  `ONIG_EXTERN=extern`) rather than project-wide settings, so nothing leaks
  into unrelated translation units.

The original analysis follows, since it still documents why Option A was
preferred over the dormant vcpkg port.

### Original analysis

Onigmo is vendored as a Git submodule at `externals/Onigmo/` and is a
source-compatible fork of the Oniguruma engine vscode-oniguruma binds, so
patterns written for VS Code's TextMate grammars should compile and match the
same way through it. `OnigmoRegexEngine.cpp` already does
`#include "Onigmo/onigmo.h"`. That resolves today with **no additional include
path** because `..\externals` is already in `sakura.vcxproj`'s
`AdditionalIncludeDirectories` (line 53) and `${CMAKE_SOURCE_DIR}/externals` is
already in `sakura.cmake`'s `target_include_directories` (line 685) — so
`Onigmo/onigmo.h` already finds `externals/Onigmo/onigmo.h` in both build
systems. **The only missing piece is compiling Onigmo's own `.c` sources and
linking the result into `sakura_core` (and, through `tests1.vcxproj`'s existing
`ProjectReference` + `CollectSakuraObjectsForTests1` object-collection target,
into `tests1` automatically — `tests1.vcxproj` does not recompile
`sakura_core` sources itself, so it needs no separate Onigmo entries.)**

There is a **dormant local vcpkg port** at
`tools/vcpkg-local-registry/ports/onigmo/` already registered in the root
`vcpkg-configuration.json` filesystem registry, but it is not consumed
anywhere (`onigmo` is absent from root `vcpkg.json`'s `dependencies`, from
`sakura.cmake`'s `find_package`/`target_link_libraries` calls, and from
`sakura.vcxproj`'s `AdditionalDependencies`). Two integration paths were
evaluated; **Option A is recommended**.

### Option A (recommended): compile Onigmo's own sources directly into `sakura_core`

Add Onigmo's `.c` files as ordinary `sakura_core` translation units, the same
way any other vendored-but-uncompiled C library would be absorbed. This needs
no vcpkg wiring at all, works uniformly for MSVC and MinGW (it is portable C,
not a separate nmake/vswhere toolchain invocation), and matches how the header
is already being consumed straight from `externals/Onigmo`.

- **Source files** (mirrors the already-vetted, currently-dead
  `tools/vcpkg-local-registry/ports/onigmo/CMakeLists.txt`'s `ONIGMO_SOURCES`
  list): `regcomp.c`, `regenc.c`, `regerror.c`, `regexec.c`, `regext.c`,
  `reggnu.c`, `regparse.c`, `regposerr.c`, `regposix.c`, `regsyntax.c`,
  `regtrav.c`, `regversion.c`, `st.c`, plus every `enc/*.c` file: `ascii.c`,
  `big5.c`, `euc_jp.c`, `euc_kr.c`, `euc_tw.c`, `gb18030.c`, `iso_8859_1.c`
  through `iso_8859_11.c`, `iso_8859_13.c` through `iso_8859_16.c`,
  `koi8_r.c`, `koi8_u.c`, `shift_jis.c`, `unicode.c`, `utf_8.c`, `utf_16be.c`,
  `utf_16le.c`, `utf_32be.c`, `utf_32le.c`, `windows_1250.c`,
  `windows_1251.c`, `windows_1252.c`, `windows_1253.c`, `windows_1254.c`,
  `windows_1257.c`, `windows_31j.c`. (Do **not** add `testc.c`,
  `test_enc_utf8.c`, or `testu.c` — those are Onigmo's own test executables.)
- **Extra include directories**: `externals/Onigmo/win32` (Onigmo already
  ships a Windows-targeted `config.h` there — the same one its own nmake build
  uses — so `HAVE_CONFIG_H` + this directory avoids writing a new config
  header) and `externals/Onigmo/enc/unicode` (holds `casefold.h` /
  `name2ctype.h`, `#include`d unqualified from `enc/unicode.c`).
- **Preprocessor definitions**, scoped to only these translation units (not
  project-wide, to avoid leaking `ONIG_EXTERN` into unrelated code):
  `HAVE_CONFIG_H` and `ONIG_EXTERN=extern`.
- **MSBuild**: add the file list above as `<ClCompile>` entries in
  `sakura_core/sakura.vcxproj` (mirrored in `sakura_core/sakura.vcxproj.filters`
  under a new `Cpp Source Files\externals\Onigmo` filter, following the
  existing nested-directory-reuses-parent-filter convention), with a
  per-item `<AdditionalIncludeDirectories>` / `<PreprocessorDefinitions>`
  override adding the two directories and two defines above to
  `%(AdditionalIncludeDirectories)` / `%(PreprocessorDefinitions)`.
  `tests1.vcxproj` needs no separate entries (see above).
- **CMake**: `sakura.cmake`'s C++ source discovery is scoped to `sakura_core`'s
  own tree (per `src/main/CLAUDE.md`: "CMake currently discovers C++ files
  recursively" — but only within that tree), so `externals/Onigmo/*.c` will
  **not** be auto-discovered. Add the same file list explicitly via
  `target_sources(sakura_core PRIVATE ...)`, add the two include directories
  via `target_include_directories(sakura_core PRIVATE ...)`, and scope the two
  preprocessor definitions to just these files via
  `set_source_files_properties(<the Onigmo .c files> PROPERTIES
  COMPILE_DEFINITIONS "HAVE_CONFIG_H;ONIG_EXTERN=extern")` (or an `OBJECT`
  library target, if isolating Onigmo's build settings more cleanly is
  preferred).
- **Caution**: none of this has been build-verified (running a build was out
  of scope for this work). `externals/Onigmo/win32/config.h`'s portability to
  MinGW/GCC has not been independently confirmed — the root `CLAUDE.md`
  already documents MinGW support as experimental, so treat this as a
  plausible-but-unverified path there specifically.

### Option B: fix and adopt the dormant vcpkg port

`tools/vcpkg-local-registry/ports/onigmo/portfile.cmake` is hardwired to
MSVC-only tooling: it resolves `vswhere`/`VsDevCmd.bat` and invokes Onigmo's
own `win32/build_nmake.cmd` via `nmake` for both Release and Debug, with **no
MinGW/GCC branch at all** (`FATAL_ERROR` for any `VCPKG_TARGET_ARCHITECTURE`
other than `x64`/`x86`). Every sibling local-registry port's `sakura.cmake`
`find_package`/`target_link_libraries` call is unconditional (not gated behind
`if(MSVC)`), so this port is the odd one out. The port's **own**
`CMakeLists.txt` and `onigmo-config.cmake.in` are already portable and
compiler-agnostic — they use the same `vcpkg_cmake_configure` /
`vcpkg_cmake_install` / `vcpkg_cmake_config_fixup` pattern the working
`darkmodelib` port uses — but `portfile.cmake` never invokes them; they are
dead code today. There is also a target-name mismatch to fix while at it: the
dead `CMakeLists.txt` aliases `onigmo::onigmo` (lowercase), but the real
nmake-path `portfile.cmake` inline-writes `add_library(Onigmo::onigmo STATIC
IMPORTED)` (capital `O`) — CMake target names are case-sensitive, so any
consumer must use `Onigmo::onigmo`.

If this path is chosen instead of Option A: rewrite `portfile.cmake` to call
`vcpkg_cmake_configure`/`vcpkg_cmake_install`/`vcpkg_cmake_config_fixup`
against the port's own `CMakeLists.txt` (small, surgical — the `CMakeLists.txt`
needs no changes, only actually invoking it); add `"onigmo"` to root
`vcpkg.json`'s `dependencies`; in `sakura.cmake` add
`find_package(onigmo CONFIG REQUIRED)` and
`target_link_libraries(sakura_core PUBLIC Onigmo::onigmo)` — the **static-lib**
pattern (like `darkmodelib`/`fmt`/`Microsoft.GSL`/`WIL`), not the
DLL-staging pattern (`bregonig`/`cmigemo`), since `OnigmoRegexEngine.cpp` links
the C API in statically. In `sakura.vcxproj`, MSBuild's vcpkg manifest-mode
integration is believed — **not build-verified** — to auto-link every static
`.lib` installed under the triplet's `lib`/`debug\lib` directories without an
explicit `AdditionalDependencies` entry (inferred from `darkmodelib` /
`bregonig` / `cmigemo` / `ppa-stub` / `dll-plugin1` all being absent from the
single existing `AdditionalDependencies` line at `sakura.vcxproj:75` despite
being consumed elsewhere in the codebase); if that hypothesis is wrong, add
`onigmo.lib;` to that line as a fallback. Even after this fix, the *installed*
headers (a flat `include/onigmo.h`, not `include/Onigmo/onigmo.h`) would remain
irrelevant to `sakura_core`'s own `#include "Onigmo/onigmo.h"`, which already
resolves via `externals/Onigmo` directly — only the compiled `.lib` would
matter. The CRT/runtime-library linkage of the nmake-built `onigmo_s.lib`
relative to the rest of the statically-linked `x64-windows-static` vcpkg
dependency set has not been verified either.

### Recommendation

Prefer **Option A**. Onigmo is a submodule-pinned vendored copy, not a
vcpkg-versioned dependency, so there is no version-pinning benefit to routing
it through vcpkg, and Option A avoids both the MinGW gap and the unverified
CRT-linkage question in Option B. Whichever option is chosen, an actual build
must confirm it — this document records analysis, not a verified outcome.

## Known gaps versus vscode-textmate (stage 2/3)

- **Nested `repository` is not merged.** A `repository` dict declared inside a
  non-root rule (legal-but-rare TextMate shape) is never merged into
  `Grammar::repository`; only root-level repository entries resolve. See
  `Grammar::repository`'s doc comment in `TextMateGrammarModel.h` and
  `TextMateGrammarCompiler::Compile`.
- **`$base` == `$self`.** Both resolve to the grammar's own root rule, since
  grammar injection is not implemented. See `EIncludeKind`'s doc comment and
  `ResolveInclude` in `TextMateGrammarCompiler.cpp`.
- **A capture's nested `patterns` are compiled but never tokenized.**
  `captures.<n>.patterns` is compiled into an `IncludeOnly` rule
  (`CaptureRule::nestedPatternsRuleId`), but `TextMateTokenizer.cpp`'s
  `EmitCaptureTokens` only ever applies `capture.name` to the capture's range;
  it never re-tokenizes that range using `nestedPatternsRuleId`. A capture with
  nested patterns behaves identically to one without today.
- **Capture group 0 is never emitted as an extra token scope.**
  `EmitCaptureTokens` unconditionally skips `capture.groupIndex <= 0` ("group 0
  == whole match == baseScopePath already"). Real vscode-textmate does apply an
  explicit group-0 capture's `name` as an extra scope even when it duplicates
  the rule's own `name`. Discovered while writing
  `src/test/cpp/tests1/textmate/TextMateTokenizerTest.cpp`; no test currently
  encodes group-0 capture naming as supported.
- **The compiled root rule never carries the grammar's `scopeName` as its own
  `name`.** `TextMateGrammarCompiler::Compile` leaves the root `IncludeOnly`
  rule's `name` at its default-empty value, so a `TextMateToken`'s scope list
  never contains the grammar's own top-level scope (e.g. `"source.js"`) the way
  real vscode-textmate always includes it as the outermost scope of every
  token. `TextMateTokenizerTest.cpp`'s
  `TokenizeLine_MatchRule_ProducesKeywordTokenAndPlainRemainder` test asserts
  this directly: the keyword token's scope list is exactly
  `["keyword.control.demo"]`, not `["source.demo", "keyword.control.demo"]`.
- **`TextMateRuleStackFrame` does not track its owning `Grammar`.** This
  matters once real cross-grammar `source.foo` / `source.foo#name` includes are
  exercised with state that must be interpreted against the foreign grammar
  rather than the home grammar. Documented directly on the struct in
  `TextMateTokenizer.h`.
- **JSON object member order is not preserved end-to-end.**
  `platform::serialization::JsoncValue::Object` is a sorted
  `std::map<std::wstring, JsoncValue, std::less<>>`, so
  `TextMateJsonGrammarLoader`'s conversion into the order-preserving
  `TextMateGrammarValue::Object` (`std::vector<std::pair<...>>`) cannot recover
  the source file's textual member order — that information is already gone by
  the time `JsoncDocument` hands back its parsed tree. This has no observed
  functional effect: grammar semantics never depend on object member order,
  only on `repository` *name* lookups and `patterns` *array position*, and both
  of those survive intact (arrays are order-preserving on both sides). Routing
  through a different parser to recover object order was not an option — the
  mandated `platform::serialization::JsoncDocument` boundary must be used for
  JSON, and it is key-sorted by design.
- **The plist (`.tmLanguage`) loader is intentionally narrow.** It supports
  only `dict`/`array`/`key`/`string`/`integer`/`real`/`true`/`false`
  elements, with `data`/`date` passed through as raw, uninterpreted strings (no
  real `.tmLanguage.json`/`.tmLanguage` grammar element this loader understands
  ever needs either type decoded). It is not a general-purpose XML parser: no
  XInclude, no external DTD/entity resolution of any kind. That absence is also
  the loader's XXE defense — see `TextMatePlistGrammarLoader.h`'s class comment
  and `src/test/cpp/tests1/textmate/TextMatePlistGrammarLoaderTest.cpp`'s
  `Parse_ReferencingDeclaredDoctypeEntity_FailsAsUnknownEntityReference` test,
  which proves a `<!ENTITY xxe SYSTEM "...">` declared in a `<!DOCTYPE>`
  internal subset is never registered: referencing it via `&xxe;` fails closed
  as an *unknown* entity reference, identically to any other unrecognized
  `&name;`.
- **`TextMateScopeColorResolver` supports only whitespace-separated
  descendant-combinator selectors** with dot-segment specificity scoring. The
  `>` direct-child combinator, `-` exclusion, and `|`/`,` grouping combinators
  are not recognized and fail closed (a selector using them simply never
  matches anything, rather than being silently misinterpreted as a more
  permissive combinator). See that class's own header doc comment for the
  precise list; it is intentionally the same boundary `theme/CLAUDE.md`
  already documents for the wider color-theme projection.

## Stage 4: scope-to-color boundary

`theme::TextMateScopeColorResolver::Resolve` takes a scope path
(`std::vector<std::wstring>`, outermost to innermost — the same shape as
`textmate::TextMateToken::scopes`) and a theme's `ThemeTokenColorRule` list
(already parsed by `CColorThemeRegistry`) and returns the highest-specificity
matching rule's foreground/background/`fontStyle`, using VS Code's own
specificity ranking (sum of dot-separated segments across every matched
selector part; later rule wins an exact tie). It has no `#include` dependency
on `sakura_core/textmate` by design, so an integration layer that already
depends on both `textmate::TextMateToken` and `theme::ColorThemeSnapshot` is
expected to pass `token.scopes` straight through. **No production caller wires
this into rendering yet** — that is real remaining work, not merely omitted
documentation.

## Tests

Added under `src/test/cpp/tests1/textmate/` (see `src/test/CLAUDE.md` for the
directory's own conventions). All five files are registered in
`tests1.vcxproj`(+`.filters`) and all 41 tests pass as of 2026-08-06:

- `TextMateGrammarCompilerTest.cpp` — `TextMateGrammarCompiler::Compile`
  directly against hand-built `TextMateGrammarValue` trees: match/begin-end/
  begin-while rule shapes, `beginCaptures`/`endCaptures`/shared `captures`
  fallback semantics, `applyEndPatternLast` default/override, `disabled`
  pattern skipping, `#name`/`$self`/`$base` include resolution (including the
  documented `$base`==`$self` gap), nested-capture-`patterns` allocation, and
  the nested-`repository`-not-merged gap.
- `TextMateJsonGrammarLoaderTest.cpp` — the same shapes through the real JSON
  text path (`platform::serialization::JsoncDocument` → `TextMateGrammarValue`
  → `Compile`), plus `patterns` array order preservation and JSON
  parse-failure/missing-`scopeName` diagnostics.
- `TextMatePlistGrammarLoaderTest.cpp` — the same shapes through the plist/XML
  path, plus the XXE-defense test described above, predefined/numeric XML
  entity decoding, a self-closing `<plist/>` root, and a mismatched end-tag
  malformed-XML diagnostic.
- `TextMateScopeColorResolverTest.cpp` — `MatchSelectorForTesting`'s
  dot-segment prefix matching, ancestor-subsequence ordering, specificity
  scoring, and `Resolve`'s empty-`scopes`-as-universal-default and
  equal-specificity later-rule-wins tie-break.
- `TextMateTokenizerTest.cpp` — `TextMateTokenizer::TokenizeLine` end to end
  against a JSON-loaded grammar: a `match` rule producing a keyword token plus
  a plain-text remainder, and a `begin`/`end` string rule whose state (open
  frame, `beginCaptures` scope) is carried across two separate `TokenizeLine`
  calls via `RuleStackHandle`, closing correctly on the second line and popping
  back to root. This is the only test file in this directory that reaches the
  regex engine (through `TextMateTokenizer.cpp`'s
  `OnigmoPattern::Compile`/`Search`; the compiler/loader tests never do), so it
  is the one that actually proves the stage 1 Onigmo wiring works rather than
  merely compiling.

### `.vcxproj` registration (done)

All of the following are registered as of 2026-08-06. Keep both halves of each
pair in sync — MSBuild source lists are explicit, so a file added to a
`.vcxproj` without its `.vcxproj.filters` entry builds but disappears from the
Solution Explorer tree.

- **`sakura_core/sakura.vcxproj`(+`.filters`)** — every `textmate\*.cpp/.h`
  plus `theme\TextMateScopeColorResolver.{cpp,h}` under
  `Cpp Source Files\textmate`, and the full Onigmo `.c` list under
  `Cpp Source Files\externals\Onigmo`.
- **`sakura_core/tests1.vcxproj`(+`.filters`)** — the five
  `..\src\test\cpp\tests1\textmate\*Test.cpp` files under `Test Files\textmate`.
  `tests1` does not recompile `sakura_core` sources; it picks Onigmo up through
  the existing `ProjectReference` + `CollectSakuraObjectsForTests1` object
  collection, so it needs no Onigmo entries of its own.
- **`src/main/cmake/sakura.cmake`** — `ONIGMO_SOURCES` appended to `SOURCES`,
  with the include directories and per-source defines applied there. The
  `textmate\*.cpp` files need no CMake entry: that glob is recursive within
  `sakura_core`. `externals/` is outside it, which is why only Onigmo is listed.
