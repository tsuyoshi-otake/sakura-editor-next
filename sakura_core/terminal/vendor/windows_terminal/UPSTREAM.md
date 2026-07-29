# Windows Terminal upstream provenance

- Upstream repository: https://github.com/microsoft/terminal
- Pinned commit: `cd89e8226b423ee82dc56c3215ae4a43459a32e8`
- Upstream tree: `src/` plus the repository-root `LICENSE`
- License: MIT; the unmodified upstream license text is in `LICENSE`.

Sakura compiles the pinned `CodepointWidthDetector.cpp`, parser state machine, and terminal input keyboard/mouse encoders through private adapters. The ETW `tracing.cpp` snapshot is retained for provenance and hash verification but is not compiled; Sakura supplies a private no-op parser-tracing compatibility implementation instead. The upstream files remain textually unchanged; Sakura-owned compatibility headers provide the narrow Windows Terminal framework boundary. See `DEPENDENCY_GAPS.md` for the exact build boundary.

Scope:

- VT parser state-machine API and implementation.
- Terminal input keyboard and mouse-sequence encoders.
- Unicode codepoint-width and extended-grapheme implementation.
- Direct header-level dependencies needed to inspect/adapt those components.

Local integration files (not copied from upstream):

- `sakura_compat/WindowsTerminalCompat.{h,cpp}`: Sakura-owned compatibility boundary for the selected parser and input translation units.
- `src/terminal/parser/precomp.h` and `src/terminal/input/precomp.h`: Sakura-owned PCH replacements that preserve the unmodified upstream include directives.
- `src/terminal/types/inc/{IInputEvent,utils}.hpp`: Sakura-owned forwarding headers that preserve the pinned upstream source paths.
- `src/types/precomp.h`: minimal standard-library/logging compatibility shim for the Unicode implementation.
- `../../unicode/TerminalGraphemeWidth.{h,cpp}`: Sakura-owned boundary hiding all Windows Terminal types.
- `../../parser/TerminalParser.{h,cpp}` and `../../input/SakuraTerminalInputAdapter.{h,cpp}`: Sakura-owned adapters that keep the imported parser and input APIs out of Sakura's public model.

Resolved third-party dependencies used by the compatibility boundary:

- fmt `10.1.1`
- Microsoft GSL `4.0.0#1`
- WIL `2023-10-28`

Their exact installed vcpkg copyright payloads are staged under `../licenses/{fmt,ms-gsl,wil}/LICENSE` for release packaging.

Excluded by design: XAML, WinUI, renderer, TerminalApp, host, TextBuffer, UI controls, build metadata, tests, fuzzers, and generated-tool projects.
