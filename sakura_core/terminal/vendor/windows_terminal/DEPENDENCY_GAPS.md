# Dependency gaps and integration boundary

This vendor drop preserves upstream source while excluding the Windows Terminal framework and host layers. Sakura compiles a dependency-closed parser/input subset through Sakura-owned adapters and compatibility code; it does not adopt the upstream application, renderer, or host architecture.

## Integrated dependency-closed subset

- `src/types/CodepointWidthDetector.cpp`, `src/terminal/parser/stateMachine.cpp`, `src/terminal/input/terminalInput.cpp`, and `src/terminal/input/mouseInput.cpp` are compiled without Sakura's `StdAfx` PCH and remain textually identical to the pinned upstream files.
- `src/terminal/parser/tracing.cpp` remains a pinned, hash-tracked source snapshot but is not compiled. Its ETW tracing is not required for terminal operation and is supplied by a Sakura-owned no-op compatibility implementation, avoiding the incompatible Shift-JIS execution-charset and TraceLogging pragma combination.
- `sakura_compat/WindowsTerminalCompat.{h,cpp}` supplies the selected upstream files' private PCH contract: Win32 declarations, fmt, Microsoft GSL, WIL, the required TIL subset, parser logging, and the keypad feature policy.
- `src/terminal/parser/precomp.h` and `src/terminal/input/precomp.h` are Sakura-owned PCH replacements. `src/terminal/types/inc/{IInputEvent,utils}.hpp` are Sakura-owned forwarding headers that satisfy the unchanged upstream relative includes.
- `terminal/unicode/TerminalGraphemeWidth.cpp` is the one-way adapter. Public Sakura model/parser headers do not expose vendored types.
- `TerminalModel` uses this adapter for Unicode 16.0 grapheme boundaries, emoji sequences, and cell width.
- `terminal/parser/TerminalParser.{h,cpp}` adapts `StateMachine` to Sakura's terminal model, and `terminal/input/SakuraTerminalInputAdapter.{h,cpp}` adapts the upstream keyboard, mouse, and focus encoders to Sakura input events.

## Deliberately excluded dependencies

- Sakura consumes the resolved vcpkg packages fmt 10.1.1, Microsoft GSL 4.0.0#1, and WIL 2023-10-28. Their installed `share/{fmt,ms-gsl,wil}/copyright` payloads are staged under `sakura_core/terminal/vendor/licenses/` for release packaging.
- The compatibility boundary implements only the TIL and checked-arithmetic surface required by the selected translation units. It does not import Chromium `base`, the full upstream TIL implementation, or the Windows Terminal feature framework.
- Windows Terminal's ETW `parser/tracing.cpp` is deliberately excluded from compilation. Sakura uses no-op tracing only, and does not import a Windows Terminal telemetry host or TraceLogging provider.
- The remaining Windows Terminal parser/input files, renderer, host, XAML/WinUI, TextBuffer, application layer, build metadata, tests, fuzzers, and generated-tool projects remain excluded.

No imported Microsoft source file has include rewrites, namespace changes, or behavioral changes. The compatibility shims and Sakura adapters are local files and are not represented as upstream imports.
