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
  declared; declarative language/grammar and host-View packages contain no
  executable code.
- Built-ins are integrity-pinned; publisher trust is signature-based; unsigned
  developer packages start disabled.
- Verification and extraction consume one bounded in-memory archive snapshot;
  package bytes are never reopened after their digest and trust decision. An
  installed listing re-reads every allowed payload file, requires exact
  checksum coverage and directory shape, rejects reparse points, and recomputes
  each SHA-256 before publishing contributions.
- Runtime descriptors carry the validated lowercase SHA-256 for
  `module/extension.wasm`. The host reads the module once into a bounded byte
  vector, compares that digest, and gives those same bytes to Wasmtime. A path
  checked by management is therefore not reopened directly by the runtime
  engine. The installed checksum document is integrity evidence, not a separate
  authenticated trust anchor against a same-user attacker who can rewrite both
  payload and metadata; do not claim stronger same-user tamper resistance until
  authenticated installed metadata or filesystem ACL ownership is defined.
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
- Declarative `viewsContainers.activitybar` and `views` entries may select only
  product-owned host providers from integrity-pinned built-ins. The package owns
  the stable ViewContainer/View existence and metadata; the host retains Git,
  workspace activation, HWND drawing, focus, and accessibility authority.
  The current native page pool accepts this batch only during window startup,
  so enable/uninstall changes apply to the next window.
- Recorded SENP divergence: an Activity Bar ViewContainer icon is a bounded
  `$(codicon-name)` ThemeIcon instead of VS Code's extension-relative SVG path.
  The native Activity Bar currently renders the bundled codicon font and has no
  safe SVG extension-asset renderer; accepting arbitrary image paths would fake
  capability and widen the package filesystem boundary.
- ZIP paths, counts, sizes, compression ratio, UTF-8, JSON members, ABI,
  capabilities, activation events, and contribution identifiers fail closed.
- No WASI linker is attached in ABI v1. Runtime memory, fuel, elapsed time, IPC
  frames, queue depth, and cached results remain bounded.
- Every host and helper process inherits only its declared standard-stream
  handles and is assigned atomically at creation to a kill-on-close job. Each
  has explicit timeout and cleanup ownership. Paint and input paths never wait
  for the host.
