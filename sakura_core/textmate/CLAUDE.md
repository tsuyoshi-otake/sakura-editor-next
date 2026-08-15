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
| 1. Onigmo usable from `sakura_core` | Built (MSVC) | Linked as the vcpkg static library `Onigmo::onigmo` (full encodings). TextMate runtime uses UTF-8 only; `bregonig.dll` keeps CP932 via the same lib. Direct engine coverage is `OnigmoRegexEngineTest`; tokenizer coverage is `TextMateTokenizerTest`. |
| 2. `.tmLanguage.json` / `.tmLanguage` (plist) parsing | Implemented | `TextMateGrammarCompiler` + both loaders. See "Known gaps" for exact divergences from vscode-textmate. |
| 3. Line tokenizer | Implemented | `TextMateTokenizer::TokenizeLine`. It calls `OnigmoPattern::Compile`/`Search`, so it could not be build-verified until stage 1 landed; it now is. |
| 4. Scope-to-color boundary | Boundary defined, not wired to rendering | `theme::TextMateScopeColorResolver`. No production caller passes real `TextMateToken::scopes` into it yet. |

## Onigmo build integration

Onigmo NEXT (`tsuyoshi-otake/onigmo-next`) is vendored as a Git submodule at
`externals/onigmo-next/` so `#include "onigmo-next/onigmo.h"` stays stable.
It is our maintained fork of k-takata/Onigmo, itself a source-compatible
fork of the Oniguruma engine vscode-oniguruma binds. Wrapper type names stay
`OnigmoRegexEngine` / `OnigmoPattern`.

The compiled library is **not** built as ~50 `.c` files inside `sakura.exe`.
The local vcpkg port `tools/vcpkg-local-registry/ports/onigmo-next/`
CMake-builds a static `onigmo.lib` (imported as `Onigmo::onigmo`) from that
submodule. Both build paths consume it:

- **CMake**: `find_package(Onigmo CONFIG REQUIRED)` and
  `target_link_libraries(sakura_core PUBLIC Onigmo::onigmo)` in
  `src/main/cmake/sakura.cmake`. Root `CMakeLists.txt` is
  `project(sakura LANGUAGES CXX)` — the editor graph compiles no C TUs.
- **MSBuild**: vcpkg manifest-mode auto-links `lib/onigmo.lib` (Debug:
  `debug/lib/onigmo.lib`) for `sakura.vcxproj` and `tests1.vcxproj`. Do not
  add Onigmo `.c` files back to either project. `tests1` still picks up
  `OnigmoRegexEngine.obj` through `BuildSakuraTestSupportLibrary`; it needs
  the vcpkg `.lib` at link time for the Onigmo C symbols.

Root `vcpkg.json` lists `onigmo-next` as a direct dependency because sakura
links it. `bregonig` also depends on the same port. The package-set outputs
in `src/main/modules/modules.json` must stay in lockstep with that manifest
(`PACKAGE_CLOSURE_MISMATCH` otherwise).

The port keeps the **full encoding set**, including Shift_JIS and
Windows-31J (`enc/windows_31j.c`, CP932). That is load-bearing for
`bregonig.dll`, which statically links `onigmo.lib` and exposes CP932 to
search/replace. TextMate (`OnigmoRegexEngine.cpp`) still initializes and
compiles only `ONIG_ENCODING_UTF_8`. Do not slim the vcpkg library to
UTF-8-only: vcpkg cannot host two variants in one triplet, and dropping
CP932 would break bregonig.

Headers for sakura still resolve via `externals/` (`..\externals` /
`${CMAKE_SOURCE_DIR}/externals`). The vcpkg install layout is a flat
`include/onigmo.h`, which `bregonig`'s portfile copies and renames the
archive to `onigmo_s.lib` as that Makefile expects.

Fork-side CMake lives in the submodule (`CMakeLists.txt`,
`cmake/OnigmoConfig.cmake.in`, optional `ONIGMO_BUILD_TESTS` →
`test_enc_utf8`). The port invokes that CMake; it no longer wraps
`win32/build_nmake.cmd`.

Two details are load-bearing and easy to undo by accident:

- **`ONIG_EXTERN=extern` must hold at the C++ include site.** `onigmo.h`
  otherwise defaults it to `__declspec(dllimport) extern` on MSVC and the
  link emits `LNK4217`. `OnigmoRegexEngine.cpp` defines it immediately
  before the include. The CMake target also publishes it PUBLIC; MSBuild
  auto-link does not, so the `.cpp` define is not optional.
- **Do not compile Onigmo `.c` into `sakura_core`.** The vcpkg static lib
  is the only compile of those files on the editor/CI path. Re-adding them
  to `sakura.vcxproj` / `sakura.cmake` restores the per-build cost this
  wiring exists to remove, and will duplicate symbols against `onigmo.lib`.

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
directory's own conventions). Six files are registered in
`tests1.vcxproj`(+`.filters`); CMake discovers them via the recursive tests1
glob:

- `OnigmoRegexEngineTest.cpp` — direct `OnigmoPattern::Compile`/`Search`
  coverage: a valid UTF-8 pattern, an invalid `(` (null + error text),
  whole-match and capture UTF-16 offsets on `a(b+)c` vs `xxabbcy`, and a
  no-match `nullopt`. This is the safety net for the vcpkg-lib link switch.
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
  back to root. This also reaches the regex engine through
  `OnigmoPattern::Compile`/`Search`.

### `.vcxproj` registration

Keep both halves of each pair in sync — MSBuild source lists are explicit, so
a file added to a `.vcxproj` without its `.filters` entry builds but
disappears from the Solution Explorer tree.

- **`sakura_core/sakura.vcxproj`(+`.filters`)** — every `textmate\*.cpp/.h`
  plus `theme\TextMateScopeColorResolver.{cpp,h}` under
  `Cpp Source Files\textmate`. No Onigmo `.c` entries.
- **`sakura_core/tests1.vcxproj`(+`.filters`)** — the six
  `..\src\test\cpp\tests1\textmate\*Test.cpp` files under `Test Files\textmate`.
  `tests1` does not recompile `sakura_core` sources; it archives
  `OnigmoRegexEngine.obj` via `BuildSakuraTestSupportLibrary` and links
  `onigmo.lib` through vcpkg auto-link.
- **`src/main/cmake/sakura.cmake`** — `find_package(Onigmo CONFIG REQUIRED)`
  and `target_link_libraries(sakura_core PUBLIC Onigmo::onigmo)`. The
  `textmate\*.cpp` files need no CMake entry: that glob is recursive within
  `sakura_core`.
