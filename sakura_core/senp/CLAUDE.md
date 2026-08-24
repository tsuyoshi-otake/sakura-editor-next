# SENP Subsystem Guidance

## Responsibilities

- `sakura_senp` owns the `.senp` archive schema, strict validation, trust
  policies, immutable installation, and profile enablement state.
- `CWin32SenpManagementService` owns profile-scoped discovery and invokes the
  package tool. It never loads or executes extension code.
- `CSenpRuntimeService` owns extension-host processes, bounded IPC, caching,
  cancellation, and the projection of enabled contributions. It consumes only
  paths already approved by the management service.
- `CExtensionsWorkbenchTool` is a View inside the Extensions ViewContainer in
  the Primary Side Bar. It owns HWND presentation and explicit user decisions,
  not package validation or extension execution.
- Editor painting consumes presentation-neutral decoration slots. It must never
  synchronously wait for extension code.

Dependencies point from UI and editor integration toward these stable service
interfaces. Package management and runtime execution remain separate even when
they are composed in the same editor process.

## Compatibility boundary

Use VS Code's stable workbench identifiers and ViewContainer/View lifetimes for
the Extensions surface. SENP itself is a deliberate product boundary, not an
implementation of VSIX, the VS Code Extension API, or OpenVSX. Do not accept a
`.vsix`, reinterpret an OpenVSX manifest, or expose an API merely because it can
make the surface look compatible.

Each editor process owns the runtime host for its visible document data. This
differs from a single cross-window shared host because Sakura Editor NEXT keeps
documents and native editor windows in separate editor processes; moving visible
text across the control process would broaden authority and add a shared failure
domain. Package state remains profile-scoped and content-addressed.

The current SENP artifact/resource pipeline is supported by the primary x64
MSBuild build. CMake/MinGW builds do not embed built-in SENP resources or publish
the Wasmtime sidecars yet; the management service therefore reports an explicit
unavailable state and executes nothing on those builds. Do not replace that
failure with a visual placeholder or an unrelated legacy plugin path.

## Package and runtime invariants

- `README.md`, `senp.json`, `LICENSE`, and complete SHA-256 coverage are
  mandatory. `module/extension.wasm` is mandatory only when `runtime` is
  declared; declarative language/grammar packages contain no executable code.
- Built-ins are integrity-pinned; publisher trust is signature-based; unsigned
  developer packages start disabled.
- `kBuiltInResources` declares whether each embedded package is installed by
  default. Startup reconciles missing or outdated default packages before the
  runtime starts. Reinstallation preserves a user's disabled state for the same
  trust class; a product update must never silently re-enable that extension.
  An explicit built-in uninstall publishes a profile tombstone before removing
  the active profile state. Startup must honor that tombstone across product
  updates, while explicit reinstall clears it. Embedded package resources and
  immutable content cache remain available for an offline reinstall.
- Declarative `languages` and `grammars` use the VS Code contribution shape and
  are loaded in-process by `ISenpLanguageService`. Package management chooses
  enabled assets; the language service selects a grammar; the TextMate engine
  tokenizes; the Editor and theme service own final presentation.
- ZIP paths, counts, sizes, compression ratio, UTF-8, JSON members, ABI,
  capabilities, activation events, and contribution identifiers fail closed.
- No WASI linker is attached in ABI v1. Runtime memory, fuel, elapsed time, IPC
  frames, queue depth, and cached results remain bounded.
- Every host and helper process has explicit timeout and job-object cleanup
  ownership. Paint and input paths never wait for the host.
