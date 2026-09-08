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
  external work, and checks owner/workspace/account before Apply. The window-local
  `CSenpOwnerComposition` calls owner Poll first and publication Pump second on
  each bounded UI-thread tick; this ordering prevents provider follow-up admission
  from reentering the owner service. Hide/collapse is not revoke. Join is attempted
  only after worker exit; a failed attempt remains
  in an explicit cleanup-failed slot without an automatic retry loop. Close
  revokes/stops all instances before joining any, may retry a failed join once,
  and reports unconfirmed process exit instead of freeing capacity. Generations
  and digest/scope identity never authorize a retained result unless IsCurrent
  is still true at its publication boundary.
- `CSenpOwnerRequests` is the UI-thread bridge from one current owner to its
  native consumers. It increments the owner-wide request generation only after
  admission, gives derived tool completions a new operation ID under the same
  lineage, and bounds live lineages and queued terminals to 16. Publication
  `Apply` may only enqueue through `Publish`; consumers drain after owner `Poll`
  returns, then either derive more work or explicitly `Finish`. Cancelling a
  lineage suppresses already-queued delivery and cancels every live invocation.
  Close this broker before destroying its borrowed owner service.

- Built-in GitHub guests request only the fixed `github` / `repositoryRead`
  tool operation. The shared Rust `github_client` crate converts bounded DTOs;
  it is statically linked into each guest and owns no account, process, network,
  or cache state. Repository completions carry a JSON `body` plus a broker-
  validated `nextPage` number. Keep pagination even when extension-side filtering
  removes every row, and reject duplicate JSON members and malformed required
  fields before publishing a View page.

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


## Bounded text resource storage (U05, #296)

`SenpTextResourceStore` is a single broker-thread, memory-only owner. Its exact
scope is profile, extension ID, package digest, grant, owner/workspace/account
generation and immutable resource revision. The broker must also prove that the
grant is currently live before every call. Opaque handles are identities, not
bearer credentials; a Wasm label must never reconstruct authorization.

`CSenpToolGrants` is the Control-owned authorization gate behind that live-grant
check. Issuance starts from an OS-observed IPC session ID and peer PID and asks a
trusted package authority for the enabled extension digest, management revision,
and approved capabilities. It never promotes Editor-supplied owner fields into
authority. Every use rechecks the connection, complete owner scope, expiry, and
current trusted authority. Session loss, owner replacement, profile shutdown,
and broker shutdown have explicit revocation paths. The 64-record limit and
five-minute lifetime bound abandoned grants. T03 adds the provider policy; this
registry alone cannot construct an executable, argv, environment, or network
request.

`CGhToolPolicy` is the next closed boundary. It discovers only `gh.exe` through
absolute PATH entries, accepts the measured `2.93.0` contract exactly, and
constructs either `--version` or one fixed `gh api --method GET --include`
invocation. Repository identity and path segments are validated separately;
extensions never provide flags, cwd, stdin, headers, executable paths, or
environment values. Ambient GitHub tokens, repository/host selectors, debug,
pager, browser, editor, forced-TTY, socket, and config-directory overrides are
removed in the bounded child environment. `CGhConnectionLifecycle` owns the
T04 account candidate. An initial snapshot stays Unknown until a complete
`gh auth status --json hosts` check proves authentication is absent. The
selected account is pinned to an explicit profile config directory; its token
travels only through native private pipes and a scrubbed child environment.
The candidate becomes Connected only after the fixed `user` endpoint reports
the same login. Timeout, malformed output, and candidate failure preserve an
existing usable account. Replacement, confirmed reauthentication, disconnect,
and close revoke retained credential leases and every profile grant. A late
candidate cannot cross the connection epoch. T06 adds bounded query and
HTTP-envelope parsing. Do not widen either policy into a generic command
runner.

`CGhLoginSession` owns T05's UI-neutral web-login model. It launches only the
measured `gh auth login --hostname <host> --web --skip-ssh-key` shape with an
explicit profile configuration directory, closed stdin, a five-minute deadline,
bounded output, and the process runner's kill-on-close job. The bounded output
observer publishes device-code and URL text while the process is active; control
bytes or observer failure terminate as UnsupportedInteractiveFlow. Exit zero is
only a candidate: T04 must still re-enumerate and verify the account identity
before the session reports success. Cancel, timeout, UI close, disconnect and
every process failure publish explicit terminals. Disconnect revokes Sakura's
credential lease and grants but never runs `gh auth logout`; the presentation
must disclose that shared gh authentication remains and show the CLI-reported
credential storage source.

`CGhRepositoryReader` owns T06's one-page response boundary. Query names come
from a closed allowlist, values are percent-encoded, numeric page controls are
bounded, and conditional requests accept only a validated entity tag. The
reader executes the policy-built argv only through a T04 authenticated lease.
It parses the bounded `gh api --include` output as one HTTP envelope, never
follows a Link URL, and exposes only a validated next-page number. A 200 body
must have GitHub JSON content type, strict UTF-8 JSON, and an object or array
root. 304, 401, 403 and 404 remain distinct even when gh exits nonzero; malformed
headers, duplicate relevant headers, scalar JSON, and process terminals fail
closed without publishing a body. Endpoint-specific DTO fields remain owned by
the extension SDK stages after T06.

`CGhReadScheduler` owns T07's Control-wide read admission. Its resource key
includes profile, account generation, host, repository identity, endpoint and
canonical query, while excluding the conditional ETag from single-flight
identity. It permits one running read per account lane, admits at most 64 live
resources and 256 subscriptions, and chooses the oldest eligible queue entry.
Hidden subscribers never create polling work. Visible list and active leases
poll no faster than 60 and 15 seconds respectively. A typed 429 or rate-limited
403 installs the bounded host/account cooldown; an ordinary 403 does not.
Removing the last visible subscriber signals the shared stop event, but the
dispatch remains the broker's cleanup responsibility until `Complete`. Close
has the same contract. Never publish a delayed completion from a cancelled
cycle or use profile/account/repository results across another scope.

`CGhLogResource` owns T08's selected-job transfer. It creates only the closed
`gh api --hostname <host> --method GET` job-log request and executes it through
the verified account credential. The token remains in that credential, while
the CLI-internal short-lived redirect URL never enters the resource, result or
diagnostics. Stream stdout into `SenpTextResourceStore` with a 120-second,
32-MiB stdout and 64-KiB stderr bound. Complete, partial failure, cancellation,
timeout and limit exhaustion are distinct terminals. An RAII completion guard
must finish every created resource on all returns and exceptions. Do not infer
whether a 404 means pending generation, missing permission or deletion; expose
the ambiguous `UnavailableOrNotFound` terminal.

`CGhRepositorySelection` owns E01's workspace-to-GitHub repository snapshot.
It consumes the immutable workspace context rather than SCM HWND or refresh
state and performs only fixed passive `rev-parse` and `remote --verbose` Git
reads. File roots and repository roots are canonicalized independently;
duplicate workspace roots inside one repository retain their root identities
but share one repository candidate. Fork and upstream remotes remain separate
choices, and neither `origin` nor `upstream` has implicit priority. Only a
literal GitHub SSH hostname is resolved: an SSH alias remains a typed choice
that requires explicit host resolution. Empty workspaces, unsupported roots,
no repository, multiple roots, remote removal, and stale workspace generations
must remain observable rather than becoming an empty GitHub view.

Append accepts the next byte offset and at most 64 KiB. Fixed 64 KiB pages bound
one resource to 32 MiB and the Control store to 64 MiB of allocated payload pages,
including unused tails, with at most 64 resource slots. Pages and their index
grow amortized O(B); byte-sized appends do not allocate per-chunk metadata.
Allocation is prepared before committing bytes/counters. Allocation exceptions
leave the previous prefix intact and require the caller to Finish and stop its
producer. Every limit/failure/cancel/expiry requires physical producer cleanup
by the broker; the store never owns or retries a process, download or timer.

Loading, Complete, Partial, Failed and Expired are separate states. A resource
limit retains its accepted prefix with LimitExceeded, without evicting another
reader. Finish is terminal; Expire erases all pages and leaves a tombstone until
Release. Close reclaims every slot and rejects all work. Owners must release
expired handles so tombstones cannot exhaust the bounded registration pool.

`SenpTextResourceDecoder` performs strict incremental UTF-8 decoding in O(B),
retaining at most three bytes. It normalizes CR/LF across chunk boundaries and
makes control, ANSI/OSC and bidi-format input inert. A malformed or truncated
scalar rejects the current chunk and closes the decoder. The consumer owns text
storage and indexing; neither decoder nor store reparses earlier content.

Verify `SenpTextResource.*`, including actual 32/64 MiB allocations and every
authorization dimension. U06 owns native publication; T02/T08 still own live
grant, download and release wiring. This store alone enables no network access.
