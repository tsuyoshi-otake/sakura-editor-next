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

- Package parsing selects `schemaVersion` only after the entire JSON passes
  duplicate-member and trailing-input checks. Schema 1 retains its existing
  strict field/capability validation. Schema 2 with
  `sakura:senp/extension@2.0.0` is recognized but returns the typed
  `UnsupportedRuntime` boundary until v2 owner/contribution/capability validation is wired;
  mismatched ABI/module pairs return `AbiMismatch`, unknown schemas return
  `UnsupportedSchema`. Pack, archive verification, and installed-content
  discovery use the same dispatch and must never fall back to v1 or publish
  v2 contributions early (#296, G01).

- The v2 event/effect wire contract is `SenpEffectProtocol` and
  `sakura_senp_host::effect_protocol`, with separate WIT bindings under
  `rust/senp/wit/v2/`. All records require every named member; variants use
  `type`/`data` and reject unknown cases. The transport uses strict JSON, not
  configuration JSONC. Frames are at most 1 MiB and 65,536 JSON nodes; counters
  are exact nonnegative integers up to `INT64_MAX`, with positive sequence,
  session generation and owner generation. Account generation zero means no
  adopted account yet, not proof of a signed-out user. A batch rejects duplicate
  read IDs and command completions. Session replay, sequence ordering and ack
  ownership belong to the session state machine, not the stateless codec.
- Validate aggregate output budgets while traversing/serializing the typed
  graph. Do not first clone it or build an unbounded intermediate JSON tree.
  The shared accepted/rejected fixtures and bidirectional exchange procedure
  live in `rust/senp/fixtures/README.md`. G02 adds the contract; G03 dispatches it
  through the explicit low-level v2 host. Package publication remains gated on
  the native projection and capability-validation stages.

- G03a's `CSenpRuntimeSession` and Rust `effect_session::Session` own explicit
  invocation terminals, ordered sequences, bounded ack receipts and replay.
  The native owner mints monotonic `s<session>:o<ticket>` IDs independently of
  transport sequence, which is assigned only on send; dropping an unsent
  cancellation must leave no sequence gap. A returned effect batch completes
  a Wasm invocation, not its tool reads or user command. Pending plus undrained
  outcomes is bounded to 16. Revocation and protocol/transport failure also
  clear effects from already-completed outcomes waiting for their consumer.
  Consumers that already took a result still require the contribution-owner
  generation fence. See `docs/senp-v2-runtime.md` for the full contract.

- G03b's `CSenpEffectRuntime` owns one worker and one atomically assigned job;
  only that worker performs host I/O. Both pipe directions use OVERLAPPED I/O
  under one absolute frame deadline; cancellation drains kernel completion
  before freeing its buffers. The job enforces a 512 MiB process memory cap
  in addition to Wasm Store limits, active-process limit 1 and kill-on-close.
  Do not reuse v1's synchronous writer for v2. Stop requests cancellation;
  Join serializes the sole thread join and also requests Stop. Process/worker
  exit observations are distinct from a requested stop. No automatic restart.
  `tools/verify-senp-runtime.py` owns the adversarial native peer and actual
  Wasm component fixtures; they must never enter the package/distribution catalog.

- G04's `CSenpContributionOwners` owns preparation, activation, replacement,
  revocation and retirement of at most four runtime instances, counting every
  preparing/retiring/failed-cleanup instance. It reserves one of 16 terminal
  transition receipts before starting a host. Preparation failure preserves the
  old owner. Revoke removes the owner from the current set, clears that exact
  publication's authority/content/subscribers, then requests host Stop.
- `ISenpOwnerPublication` is a mandatory native transaction, not a Wasm import.
  Preparation must validate every descriptor/page/command before Commit; false
  Commit leaves the previous publication unchanged. Revoke owns termination of
  higher-level tool/UI requests. An absent native adapter is Unsupported before
  any host launch. The G04 catalog/runtime tests do not enable package-derived
  pages: native pages, commands and broker grants are wired by U01/U06/T02.
- Poll performs at most 16 completion reads per instance, never waits or starts
  external work, and checks owner/workspace/account before Apply. Composition
  must schedule bounded drains while it owns instances; hide/collapse is not
  revoke. Join is attempted only after worker exit; a failed attempt remains
  in an explicit cleanup-failed slot without an automatic retry loop. Close
  revokes/stops all instances before joining any, may retry a failed join once,
  and reports unconfirmed process exit instead of freeing capacity. Generations
  and digest/scope identity never authorize a retained result unless IsCurrent
  is still true at its publication boundary.

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
