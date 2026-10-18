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
  strict field/capability validation. Schema 2 requires its own
  `runtime` block naming `sakura:senp/extension@3.0.0` and `module/extension.wasm`
  before anything else is read: a v2 manifest that omits `runtime` is
  `InvalidManifest`, not a declarative extension carrying a newer number.
  Mismatched ABI/module pairs return `AbiMismatch`, unknown schemas return
  `UnsupportedSchema`. Pack, archive verification, and installed-content
  discovery use the same dispatch and must never fall back to v1 (#296, G01/R01).

- **The runtime ABI is owned by Rust and moves with the WIT (#299).** The
  schema-2 ABI string is `sakura:senp/extension@<WIT package version>`, defined
  once as `sakura_senp::ABI_V2` and `sakura_senp_host::effect_protocol::ABI`
  (plus the C++ wire `kAbi`, tied to the host through the shared protocol
  fixture). `sakura_senp_host/tests/wit_pin.rs` pins the WIT digest to its
  package version, so any WIT change fails the build until the version and
  every ABI string are bumped. The schema number is unrelated and stays 2. A
  record field added without a bump (9aaed7a9e `ahead`) once let an older
  installed GitHub Actions package pass every manifest check and then die in
  wasmtime as a generic "could not be activated".
- Pack, verification and install admit only the current ABI. Installed-content
  listing (`AbiAdmission::Installed`) also lists a schema-2 package naming
  another well-formed extension ABI and reports `compatible: false`; one such
  package must never abort the whole listing. C++ reads that `compatible` bool
  (required in the installed JSON) into `RuntimeContribution::compatible` and
  never compares ABI strings itself: activation reports Unsupported, the window
  composition refuses it and the package authority grants no tools.
- Startup refreshes every installed built-in whose archive differs from the
  embedded one, including offered (`installedByDefault == false`) built-ins the
  user installed; an offered built-in that is absent is still never installed
  on the user's behalf. Enablement and uninstall tombstones are preserved.
  `senp::StartupInstallsBuiltIn` is that decision, and `SenpBuiltInStartup.*`
  pins it for every embedded ID. No embedded built-in is offered today (#337).
  A same-ID developer or publisher copy of an installed-by-default built-in is
  a different trust class, so startup replaces it with the embedded package;
  give a locally developed build of one a distinct ID.

- The schema-2 admission gate is released. It was held closed only until the
  real application could accept a v2 package, run the GitHub tool, complete a
  command, read a text resource and persist state; those exist now, so
  `parse_manifest` admits v2 rather than returning a typed refusal. Do not
  reintroduce a version-wide refusal here: what the gate protected is the ABI
  pair, and that check is what remains.

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
  in addition to Wasm Store limits, active-process limit 1 and kill-on-close,
  all through the shared `senp::ConfigureChildJob` below.
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
  Admission reserves terminal capacity: `Submit` applies the same
  `invocations + completed < 16` reservation as `SubmitDerived`, so an admitted
  invocation can always be published, and `Publish` checks capacity before it
  mutates (#339). Both this broker and `CSenpContributionOwners` are bound to
  the thread that constructed them: any public entry called from another thread
  refuses with its existing typed refusal and mutates nothing; only teardown
  through the destructor bypasses the check.

- **Failure granularity, smallest first: invocation < owner < package < window
  snapshot (#355, #345, #339).** A non-activation invocation that ends
  `Cancelled`, `TimedOut` or guest `Rejected` is an invocation terminal: the
  consumer finalizes and shows it - tree pages get their existing Retry row,
  documents get `FailDocument`, commands get one status-line message, and a
  workspace event gets no UI (VS Code only logs event-listener errors to the
  extension host log). The owner is never revoked for these. An owner terminal
  revokes the owner with no automatic restart and lets declared Views show
  their existing Failed/Retry: it covers activation failure or timeout,
  `ProtocolError`, `HostUnavailable` (transport failure, process exit, job
  kill), an effect invalid for its request, and a terminal for a context the
  projection never admitted. `CSenpOwnerProjection` records the kind of every
  root event it admits - Command and Workspace, the kinds tree providers and
  the document maps do not already track - bounded by the lineage cap and
  erased when the coordinator releases the lineage, and dispatches these
  terminals from that record; derived tool completions keep their originating
  root's kind, matched by owner plus request generation like documents.
  Package and window-snapshot granularity are the composition's concern
  (#352), not this layer's. `CSenpEffectCoordinator::SubmitVisibility` was
  removed: it had no caller, so the `VisibilityChanged` wire event stays on
  the codec only.

- Built-in GitHub guests request only the fixed `github` / `repositoryRead`
  tool operation. The shared Rust `github_client` crate converts bounded DTOs;
  it is statically linked into each guest and owns no account, process, network,
  or cache state. Repository completions carry a JSON `body` plus a broker-
  validated `nextPage` number. Keep pagination even when extension-side filtering
  removes every row, and reject duplicate JSON members and malformed required
  fields before publishing a View page. A row degrades instead of failing its
  page for data github.com legitimately sends: labels past 32 (or without a
  usable name; a label may be a string or an object) are kept out and counted
  as `+N`, a null or absent `user` (deleted account) is shown as `ghost`, and an
  Actions job may omit `steps`, `labels` or `html_url`. Guests cut row
  `label`/`description` to 1,024 bytes and `tooltip`/field values to 4,096 on a
  character boundary, because the host rejects a whole effect batch for one
  over-limit field. These stay strict on purpose: `https://` URLs (link
  activation policy), required `draft`/`display_title`, the Actions
  `run_attempt` match (#296 requires rejecting another attempt's response), and
  GitHub Enterprise Server is out of scope for the first release.
- A schema-2 tree page's `revision` is the provider's version of ONE parent's
  listing: the first page and every page appended through its cursors publish
  the same value, on success and on failure, and a later listing may keep or
  raise it. Guests publish a page-independent constant (never the page number
  or the workspace revision): invalidation already fences a stale page by
  ticket, and offset pages carry no data version. On append the host drops an
  item that repeats a retained direct child of the same parent (offset-shift
  duplicate, #351); in-page duplicates, ids retained under another parent and
  ancestor/cyclic ids still fail the page atomically.

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
- Archive and package-store hygiene (#353, #342, #362): every ZIP entry is read
  through `declared + 1` bytes and must match its declared size exactly (zip
  5.1.1 does not bound Deflate output), so the declared-size expanded budget
  also bounds the bytes actually read. Exact duplicate entry names are caught by comparing the EOCD
  entry count with what the reader indexed (zip 5.1.1 folds them); ZIP64 is
  rejected. Windows device names (`CON`, `NUL`, `COM0-9`, `LPT0-9`, superscript
  digits, any extension) and trailing dot/space are rejected in entry path
  components, and device names also in package ids (`valid_package_id`; other
  identifiers keep `valid_identifier`). Temporary names carry a per-process
  counter next to the pid and clock.
- Mutators (install, built-in uninstall, set-enabled) serialize across
  processes on `<root>/.senp-root.lock` via `File::try_lock`, polling with
  backoff for at most 10 s and then failing with the typed `Busy` code; readers
  never take it. Under that lock an install whose existing `content/<sha>` fails
  verification renames it aside, publishes the staged verified content and
  removes the quarantine best-effort; a failed rename is `PublicationFailed`.
- `sakura-senp-tool list` emits JSON Lines: one complete JSON object per
  package, `kind:"installed"` or `kind:"invalid"`. A failure confined to
  one profile state entry becomes an invalid row carrying the `SenpError`
  code; root-level failures still fail the command. Each line stays within the
  C++ decoder per-document budget (1 MiB, 65,536 nodes; an over-budget row is
  emitted as invalid instead), and the whole stdout stays within
  `kMaximumToolOutputBytes` (4 MiB; the producer fails with `OutputTooLarge`
  rather than emitting a partial listing). C++ decodes each line independently:
  an invalid or undecodable line becomes a descriptor with `invalidCode` set
  (never enabled, no contributions, no runtime; `invalidCode`/
  `invalidMessage` are untranslated tool tokens -- the Extensions View
  localizes only the status label), and `ReadPipeBounded` records
  overflow so a truncated stream is a typed failure, never parsed.
  `README.md` stays mandatory, UTF-8, checksum-covered package validation but
  left the listing (#356): the future details editor reads it on demand.
- `Start()` reconciles every built-in once per call: one failing install no
  longer stops the loop, failures are collected into the snapshot diagnostic as
  `ReadyWithDiagnostics` naming the failed ids, and a later Start retries
  exactly those ids instead of answering `AlreadyReady` while they exist.
  `LoadBuiltInCatalog` stays all-or-nothing: an embedded package failing
  inspection is a build defect, not a per-package degradation.
- Runtime descriptors carry the validated lowercase SHA-256 for
  `module/extension.wasm`. The host reads the module once into a bounded byte
  vector, compares that digest, and gives those same bytes to Wasmtime. A path
  checked by management is therefore not reopened directly by the runtime
  engine. The installed checksum document is integrity evidence, not a separate
  authenticated trust anchor against a same-user attacker who can rewrite both
  payload and metadata; do not claim stronger same-user tamper resistance until
  authenticated installed metadata or filesystem ACL ownership is defined.
- `kBuiltInResources` declares whether each embedded package is installed by
  default. Startup reconciles missing or outdated default packages, and
  outdated installed offered packages, before the runtime starts. Reinstallation preserves a user's disabled state for the same
  trust class; a product update must never silently re-enable that extension.
  An explicit built-in uninstall publishes a profile tombstone before removing
  the active profile state. Startup must honor that tombstone across product
  updates, while explicit reinstall clears it. Embedded package resources and
  immutable content cache remain available for an offline reinstall.
- Because a missing default package is installed whenever the profile lacks it,
  flipping a built-in to installed-by-default reaches existing profiles on
  their first window after the update, not only fresh profiles. The GitHub
  Pull Requests and GitHub Actions packages were offered until 2026-10-17 and
  are installed by default since, on the owner's decision (#337). A profile
  where the user uninstalled one keeps it uninstalled through its tombstone.
  **Recorded divergence:** VS Code bundles neither extension; a user installs
  both from the Marketplace. SENP has no gallery, so default installation
  replaces that step. The divergence is the default only: the Views, command
  IDs and upstream View identifiers are unchanged. A profile without a GitHub
  connection sees the failed read as a Retry row (today `tool-unavailable`,
  an untranslated failure token), never placeholder data.
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
- An Activity Bar ViewContainer icon is either a bounded `$(codicon-name)`
  ThemeIcon or, as in VS Code, a package-relative image path (`.svg`/`.png`,
  at most 260 bytes of `[A-Za-z0-9._-]` segments, no empty, `.` or `..`
  segment, so no absolute, drive, URL or traversal form). Rust manifest
  validation and `WorkbenchContributionRegistry::ContainerIconName` apply the
  same rule; the path reaches the Activity Bar verbatim.
- Recorded SENP divergence: the host never reads that image from the package.
  The native Activity Bar has no safe SVG extension-asset renderer, so a path
  draws only when it names an entry of the compiled-in vocabulary (today the
  GitHub Actions `resources/icons/light/explorer.svg`, see
  `../workbench/icons/CLAUDE.md`), as the single-colour mask VS Code paints in
  the Activity Bar foreground. Any other path draws the label's initial tile,
  the fallback of an unknown codicon, rather than a substituted glyph.
- Tree item icons follow the same boundary. `TreeItem.icon` may name an
  upstream-relative image path, but only a path in the host's compiled-in
  vocabulary draws (today the GitHub Actions status icons, see
  `../workbench/tree/CLAUDE.md`); the package's own files are never read and an
  unknown path draws nothing.
- ZIP paths, counts, sizes, compression ratio, UTF-8, JSON members, ABI,
  capabilities, activation events, and contribution identifiers fail closed.
- No WASI linker is attached in ABI v1. Runtime memory, fuel, elapsed time, IPC
  frames, queue depth, and cached results remain bounded.
- Every host and helper process inherits only its declared standard-stream
  handles and is assigned atomically at creation to a kill-on-close job. Each
  has explicit timeout and cleanup ownership. Paint and input paths never wait
  for the host.
- All three launchers configure that job through one helper,
  `senp::ConfigureChildJob` (`SenpChildJob.h` / `SenpChildJob.cpp`), which sets
  and then reads back every limit: kill-on-close, active-process 1,
  die-on-unhandled-exception and the full basic UI restriction set (the
  children are console helpers created with `CREATE_NO_WINDOW`; none needs a
  desktop, USER32 handles, global atoms, the clipboard or display settings).
  Any API failure or read-back mismatch is a launch failure, so no unbounded
  child may run. The memory cap differs per child: both host protocols get
  `kExtensionHostJobLimits` = 512 MiB (#340 parity), and `RunToolImpl` gets
  `kPackageToolJobLimits` = 256 MiB, sized from the tool's bounded worst
  case: install holds the 64 MiB archive snapshot, its 64 MiB of verified
  entries and the re-read of the staged copy at once. With a 62 MiB
  incompressible package (the largest the limits admit) install and
  same-digest repair peaked at 187 MiB committed, verify at 125 MiB and list
  at 63 MiB (measured 2026-10-18 under a real 256 MiB job). Raising an archive
  or expansion limit requires re-measuring this cap. A job memory kill reaches the same
  explicit terminal as any other host failure; there is no automatic restart.


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
five-minute lifetime bound abandoned grants.

The broker session tracks grant lifecycle, not only live grants. A grant id the
session minted but the registry no longer holds (pruned after expiry, or
revoked) must answer `Expired` so the editor re-issues; an id it never minted
stays `Unauthorized`. The minted-id history is bounded to the most recent
`4 * MaximumGrants()` ids; an evicted id falls back to `Unauthorized`, which the
editor treats exactly like `Expired` (forget + re-issue). The session keeps only
live records and drops expired ones before the tracked-grant cap check, so one
connection can renew indefinitely while `MaximumGrants()` concurrently live
grants still answer `ResourceExhausted`. Destruction cancels by connection, not
by remembered scope: one `CancelConnection` tears down every executor scope the
connection ever created, because an owner identity is reissued on every
workspace-revision or account change and remembering each would grow without
bound. On the executor side the global `MaximumScopes()` cap likewise bounds
live scopes only: a scope state holding no reads, pending completions,
resources or logs is idle and is reclaimed before a new scope is refused.
That reclaim is only a backstop. The explicit release is `RetireOwner`
(operation 9): when the editor retires an owner identity it sends one
grant-free request whose authority is the connection itself - the broker
builds the scope only from the OS-observed session and peer PID plus the
request's profile and owner, so a peer can only ever release its own
connection's scope. The broker then drops the session's records for that
scope (keeping the minted-id history, so a late request still answers
`Expired`), revokes that owner's grants for that connection only through
`CSenpToolGrantSession::RevokeOwner` - never the registry-wide
`CSenpToolGrants::RevokeOwner`, which would take another window's identical
owner identity with it - and calls `ISenpToolExecutor::ReleaseScope`, which
terminates the scope's reads, pending completions, resources, log reads and
queued/running log jobs before erasing it. The operation is idempotent. On a
mixed build an older Control refuses the unknown operation with a
session-preserving Error frame, so the editor keeps the connection and the
scope is released at connection close, exactly as before `RetireOwner`
existed (#354).
Issue, validate, and remaining-lifetime reads all share one injectable clock so
tests drive expiry without sleeping. T03 adds the provider policy; this
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

The built-in GitHub Issues guest owns E03's endpoint-specific issue and comment
DTOs. An Issue row keeps the database ID and repository Issue number as separate
identity fields. Selection opens `github-issue:<number>` and reads only
`issues/{number}`; expansion pages `issues/{number}/comments`; a comment document
uses its database ID with `issues/comments/{id}`. The guest never embeds a body
in command arguments. It validates the 256-KiB SENP completion envelope, 240-KiB
body bound, required HTTPS URLs and timestamps before publishing. Null Issue
bodies, empty comments, empty pages, partial pages and failed reads are distinct
terminal presentations.

The same guest owns E04's separate pull-request DTOs and `pr:github` tree.
Pull-request identities use `pull:<database-id>:<number>` and must never collide
with Issue identities. List and detail reads use only `pulls` and
`pulls/{number}` in the repository selected by the native broker; ordinary
conversation comments use that repository's `issues/{number}/comments` path.
Base and head repository, branch, and commit values are bounded display metadata.
A head in another fork never changes the selected repository or authorizes an
implicit read from that fork. Draft, closed-unmerged, and merged states remain
distinct in tree and document publication.

The separate Actions guest owns Workflow, Run and Attempt identities. Wrapped
list responses contain `body.workflows` or `body.workflow_runs`, not a bare
array. A run retains its workflow ID, database ID and attempt number. The run
row's ID includes the attempt it was read at, so its jobs and its "Previous
attempts" never silently switch to a later rerun. Current Branch requires a single verified repository branch;
detached, unavailable and ambiguous selections are explicit terminal states.
No run status or null conclusion may be promoted to success. The upstream
`github-actions.workflow.run.open` command deliberately opens a native read-only
attempt document here, as requested for this platform's in-app Actions reading
workflow; upstream opens the browser. The command and View identifiers remain
upstream identifiers. Production workspace events and packaging are R01 gates.

Push detection (#297). `workspaceChanged` carries each repository's `ahead`
(WIT `u32`, both codecs reject larger values). `CEditWnd` publishes it from the
SCM model's `HEAD` ahead count, 0 without an upstream, and a changed count is a
new snapshot. Upstream's Actions extension refreshes CURRENT BRANCH when the
HEAD name changes or when `HEAD.ahead` falls below its recorded value. It
records that value only when it refreshes, so the ordinary
commit-then-push sequence (0, 1, 0) never compares lower and the push is
missed, although its code comment says a changed ahead count "indicates a push".
**Documented divergence:** the guest records `ahead` on every observation, so
any drop invalidates CURRENT BRANCH only; a rise, like a commit upstream, does
nothing. A branch change that also lowers the count is one invalidation, and a
new scope forgets the count. This is the upstream rule applied as its comment
describes, not an extra trigger.

E06 job and step projection uses the database Job ID and each Step's job-scoped
number. A matrix display name is never an identity. The selected Run/Attempt
pair is embedded in the job resource and checked on every list/detail/step
completion. Null runner assignments, absent timestamps, empty steps, unknown
states and absent conclusions remain distinct from success. GitHub can return
skipped jobs without runners or steps; do not reject those valid responses.
The platform-only `sakura.githubActions.openJobDetails` command opens the native
summary and Step table; upstream has no corresponding native summary command,
and the upstream log command must retain its separate log meaning. That command
is upstream's own `github-actions.workflow.logs`, "View job logs", an inline
`view/item/context` action on a job row whose contextValue contains `job` and
`completed` (#297). It receives the job row's ID in place of the job node and
opens the log as a native text document; there is no Log row. Upstream's
`github-actions.step.logs` is omitted: it opens the same job log scrolled to a
step, and a text document has no step-anchored reveal contract here, so a step
row carries no action rather than a look-alike that ignores the step. The log
reads through the `jobLog` tool operation, not through
a repository path, and the extension never holds the bytes: the tool writes them
into the Control text resource store and answers `{"resource","bytes","log"}`,
which the extension turns into a text-resource section. A tree item naming a
command the View did not register rejects the whole page, so a new command must
reach `senp.json` before any item may carry it. The section is published only
once a real handle exists, because the reader starts reading a text section
immediately and does not wait on its status.

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

GitHub pull-request list responses can omit the conversation comment count.
Preserve that count as unknown and allow expansion; only an explicit zero is a
leaf. Detail responses require a count. Draft is independent from open/closed
state, and merged status comes from merged_at; a closed draft must remain closed.

The package preflight validates schema-2 Tree activation, capabilities and
command declarations as part of admission. Its identifier
grammar preserves upstream colon IDs without widening schema 1. Commands are
bounded package declarations, not authorization supplied by a Tree item.
Native package-authority decoding retains schema/ABI, activation, capability and
command metadata in RuntimeContribution for later window composition. Decoding
does not enable a package, publish an owner, or issue a tool grant. Both installed
and available built-in records use the same runtime-metadata decoder.

Schema 2 accepts the VS Code `contributes.menus` shape for two menus only,
`view/title` and `view/item/context`, and only the subset a native View can
present (#297). A command may carry a `$(codicon)` `icon` or a package-relative
path naming a bundled icon; a `view/title` item must
name a declared command that has one, use `group: "navigation"` (the inline
title bar; overflow `...` menus do not exist here), and place itself with
`when` clauses of the form `view == <id>` joined by `||`. Any other context-key
expression is refused rather than evaluated in part. Each View has at most
eight actions and a (command, View) pair appears once. Rust `validate_manifest`
and the native decoder apply the same rule; a menu item's command icon must be
a lower-case `$(codicon)` (`[a-z0-9-]`, the bundled glyph vocabulary) — Rust
refuses an upper-case ThemeIcon at pack/install so a package no window could
compose is never installed (#352) — and the window composition additionally
requires the named View to be declared.
Recorded divergences: upstream GitHub Actions names its refresh icon by a
package-relative `refresh.svg` pair, while the built-in package uses the
equivalent `$(refresh)` codicon because the host never reads package images.
Upstream also activates on `onCommand:` for a title click; SENP activation is
`onView:` only, so a title action reaches only the currently bound runtime and a
click before activation returns false instead of starting the extension. The
refresh action is the sanctioned trigger for the no-polling Tree rule.

A `view/item/context` item is drawn on the row, so only `group: "inline"` is
accepted (a row context menu does not exist here). Its `when` is one View clause
or none, joined by `&&` to `viewItem =~ /token/` and `viewItem == value` terms;
a row shows the action when its contextValue contains every regex token and
equals every value. Any other expression is refused. The icon may be a codicon or
a path, and the path must name an entry of the bundled vocabulary; `view/title`
still requires a codicon. Both menus apply the lower-case codicon rule at
pack/install (`valid_codicon`), and the native decoder enforces it again when
the window composes the package.
The action runs with the row's ID as its only argument. Item actions belong to
the runtime generation, not to the structural View declaration.

GitHub Actions tree divergences (#297). As upstream, a run lists its latest
attempt's jobs followed, for a rerun, by "Previous attempts", which lists every
earlier attempt oldest first. The run row's ID carries the attempt it was read
at, so a rerun arrives as a new row instead of re-pointing the old row's
children. An attempt row shows the `history` codicon rather than its own status
icon, which would cost one read per attempt before the list could show. The
run and workflow contextValues keep only the kind and `completed`: upstream's
rerun/cancel permission and pin tokens serve menus this extension does not
contribute, and inventing them would advertise actions that do not exist.

Native publication targets that retain an owner must be constructed through
the CSenpOwnerComposition publication factory. Contribution ownership allocates
the generation before invoking that synchronous factory; the caller's launch
generation is not authoritative. The factory receives the same identity as the
runtime and accepted receipt. Empty/rejected/throwing factories start no host,
and reentrant activation/close cannot interrupt the outer preparation.

- `CSenpExtensionActivation` consumes bounded authority snapshots without host
  work until a declared View requests its matching `onView:` activation. Native
  catalog/page preparations are serialized: queued admission starts at most one
  candidate per call after the current preparation finishes. Runtime Busy,
  Unsupported and Failed outcomes remain terminal until an explicit retry or
  relevant metadata/scope change; polling and repeated visibility do not retry.
  Disable/removal revokes publication; scope changes allocate a fresh owner.
  Repeated Close retains responsibility for unconfirmed process exit. This is
  window lifecycle infrastructure; declaration shells and CEditWnd scheduling
  still need their own integration before enabling schema-v2 packages.
