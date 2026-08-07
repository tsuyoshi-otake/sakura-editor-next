# P0/P2 Extension Host Guidance

## Process Contract

The Node host is an untrusted extension execution boundary. Native code remains
authoritative for profile/workspace/window identity, storage, secrets,
configuration, filesystem, trust, and process backends.

Installed extensions within one shared Node host are mutually trusted code, as
in the VS Code extension-host model; they are not separate OS security
principals. `ExtensionContext.secrets` supplies a logical extension namespace,
but hostile cross-extension isolation requires dedicated authenticated
processes and is not claimed by this shared-host architecture.

Handshake/session messages include profile, window, workspace, logical session,
protocol/capability revision, and generation. On reconnect, initialize from an
authoritative snapshot before applying deltas and reject stale generations.
Every async request has bounded cancellation/timeout ownership and one terminal
response.

Activation asks the workbench for nothing. VS Code gates code execution with
Workspace Trust — a per-workspace restricted mode — and has no per-extension
"may this run?" confirmation anywhere in its model. An installed extension is
therefore activated directly; do not reintroduce a permission round trip on the
activation path under any name.

## Extension Registry

`vscode.extensions` is served from the loader's own record set, which is the
only authority for what is registered in this host. `ExtensionLoader` exposes it
as a narrow registry port so the API session can project records without
reaching into loader internals, and one record always maps to one frozen
`Extension` object, so identity comparisons behave as upstream.

`extensionKind` is `ExtensionKind.UI` for every extension. VS Code documents
that value as the answer when no remote extension host exists, and this product
has no remote extension host at all; it is the upstream-correct result, not a
placeholder. `isActive` and `exports` are live getters over the record, so an
extension observed before its own activation reports `false`/`undefined` rather
than a stale snapshot. `onDidChange` fires when the registered set changes.

`window.setStatusBarMessage` is a stack, not a single slot: the newest message
renders, and disposing it reveals the message beneath. One left-aligned entry at
`Number.MIN_VALUE` backs the whole stack, matching upstream's `StatusBarMessage`
so an ordinary left item never sorts behind it.

A `StatusBarItem.tooltip` set to a `vscode.MarkdownString` puts its
`supportThemeIcons` flag on the wire as `tooltipSupportsThemeIcons`, alongside
the Markdown source. `serializeThemeValue` must not flatten a `MarkdownString`
to a bare string: that flag is the only signal distinguishing "the extension
meant a codicon" from "the extension literally typed `$(name)`", and native code
cannot recover it from the text. A plain-string tooltip carries `false`, which is
also upstream's answer — `$(name)` in a plain string is literal text. The native
consumer is
[`../../sakura_core/workbench/hover/CLAUDE.md`](../../sakura_core/workbench/hover/CLAUDE.md).

## API Compatibility

Do not implement an API as successful until a native service owns its real
lifecycle. Register contributions by extension owner and dispose all of them on
disable/update/uninstall. `globalState` and `workspaceState` use native
revisioned storage; JavaScript files are not an independent authoritative
database. SecretStorage binds only after authenticated native Hello, exposes
real `get`/`store`/`delete` plus value-free `onDidChange`, and rejects `keys()`
locally as `UnsupportedCapability` without sending an RPC. Secret values never
appear in trace output or profile exports.

Every Output create/append/replace/clear/show/hide/dispose notification carries
one bounded, session-scoped, non-wrapping operation ID. Allocate it once when
the logical mutation is created and retain the identical payload and ID for a
transport replay. Exhaustion fails explicitly; it never wraps or silently
reuses an earlier ID.

Run Node tests for protocol/API changes and the native integration suite for
wire changes. The current API cohort passes 15/15. Confirm the Node host and
Sakura test processes exit afterward.

## Terminal API: unsupported, and unsupported out loud (2026-08-07, #31)

This host owns no extension-facing terminal. The native terminal under
`sakura_core/terminal/` is real — a ConPTY backend, VT parser, and renderer —
but its instance authority lives inside the UI tab that draws it
(`TerminalTabManager::Impl::Tab` co-owns the input adapter, model, parser, and
`CTerminalSession`), so no service exists that could answer an extension's
question about a terminal. Until a runtime-owned terminal-instance authority
exists, every member of the Terminal API reports `UnsupportedCapability`.

Three consequences are deliberate divergences from upstream, and each has a
reason:

- **`window.terminals` and `window.activeTerminal` throw rather than answering
  `[]` and `undefined`.** VS Code never throws here. But native terminals do
  exist in this product, so an empty answer is not the truthful "there are none"
  it would be for notebooks — it is a false statement about the world, and an
  extension would take the wrong branch on it. Compare `visibleNotebookEditors`,
  which returns `[]` correctly because notebooks genuinely do not exist here.
- **Terminal events throw at subscription instead of returning a listener that
  never fires.** Handing back a live-looking subscription for an event this host
  can never raise is the "approximate a capability" pattern the root
  `CLAUDE.md` forbids. `noOpEvent` remains correct only where the underlying
  concept is genuinely absent.
`window.registerTerminalProfileProvider` is the exception and keeps the
`registerWebviewViewProvider` treatment: it notifies and returns a `Disposable`
instead of throwing. Registering a provider is not a claim that a terminal
exists — it is a callback for a user action, and upstream also never invokes the
provider until the user picks that profile, so an extension cannot tell the two
apart. Throwing would kill activation over a capability the extension may never
have reached. The native dispatcher routes `workbench/terminal/` by prefix into
`DispatchUnsupportedCapability`, so the gap is reported to the user in the
Extension Compatibility output channel; the notification now carries its own
`error.capability` so that report no longer depends on the dispatcher inferring
a name from the method prefix.

Known gap, deliberately not fixed here: an imperative unsupported member
(`createTerminal`, `createWebviewPanel`, `tasks.executeTask`, `debug.*`,
`SecretStorage.keys`) throws without notifying, so it never reaches the Extension
Compatibility channel — only the extension sees it. That is uniform across every
such member today; fix it for all of them at once or not at all, and do not make
one capability report differently from its siblings.

`ExtensionContext.environmentVariableCollection` is an unsupported namespace
rather than `undefined`, so the failure lands on the mutator the extension
actually called. The `TerminalLocation`, `TerminalExitReason`,
`EnvironmentVariableMutatorType`, and
`TerminalShellExecutionCommandLineConfidence` value tables are exported with
upstream-exact values: they are constants, not capabilities, and their presence
moves an extension's failure from an untyped read at module scope onto the
typed boundary at the call it meant to make.

`workspace.isTrusted` no longer needs fixing before any of this can move: it is
a real, native-backed answer as of "workspace.isTrusted is now a real answer,
and a downgrade raises no event (2026-08-07, #33)" below. That removes the
specific precondition this section used to name, but it does not remove the
gate itself. Terminal creation is process creation, and
a truthful `isTrusted` is not the same thing as a decision to allow it — nothing
in this host yet consumes trust to gate extension-originated process launch.
Implementing the Terminal API therefore remains gated on an extension-origin
process-launch policy, not merely on a native service being ready and not
merely on trust being reported correctly. Shell integration must never be
aliased to `sendText`, and no code path may special-case a particular
extension's ID or command.

## workspace.isTrusted is now a real answer, and a downgrade raises no event (2026-08-07, #33)

`workspace.isTrusted` used to be a hardcoded `true`, which is exactly the
overstatement the Terminal API section above warned against. It is now backed
by `session.workspaceTrusted`, seeded from the `workspaceTrusted` field of the
extension registration payload and kept current afterward by the
`extension/workspace/didChangeTrust` notification.

The read is fail-closed and strictly boolean. `options.workspaceTrusted ===
true` is the only thing that grants trust at construction: absent, `undefined`,
and non-boolean truthy values (the string `'yes'`, for instance) all read as
`false`. The wire notification enforces the identical rule, `params?.trusted
=== true`, so a malformed or missing `trusted` field can never be
misinterpreted as a grant. Both directions are pinned by regression tests in
`test/vscode-api.test.cjs`.

On the native side, `CExtensionService` projects `EWorkspaceTrustState::Trusted`
to `true` and both `Unknown` and `Untrusted` to `false`. That is not a lossy
approximation of three states into two: VS Code's `isTrusted` is a plain
boolean answering "did the user explicitly grant trust," and neither `Unknown`
nor `Untrusted` can answer that with anything but no. The field is always sent
on registration — omitting it would leave the host free to assume trust — and
registration doubles as the baseline every later notification diffs against,
which is exactly what makes a reconnect correct even though the host has no
memory of the pre-reconnect value.

**The documented divergence.** Upstream VS Code restarts the extension host
whenever workspace trust is downgraded, so it never has to express a downgrade
to a still-running host, and consequently upstream has no revoke event at
all — `onDidGrantWorkspaceTrust` is the only workspace-trust event that exists.
This fork instead transitions a window's trust in place, without restarting
the host. The chosen behavior: the wire always carries the current value
rather than a grant signal, the host always updates `isTrusted` to match that
current fact, and it fires `onDidGrantWorkspaceTrust` only on the
untrusted-to-trusted edge. A downgrade therefore flips `isTrusted` to `false`
with zero listener invocations — silent by design, not a missed case. Leaving
`isTrusted` at `true` after a downgrade was rejected because it overstates
trust, which is the dangerous direction of the root `CLAUDE.md`'s "never fake a
capability" rule. Inventing a revoke event to announce the downgrade was also
rejected: the extension-facing public API stays byte-for-byte upstream, with
no new event VS Code doesn't have. A dedicated test pins the zero-fire
demotion.

`onDidGrantWorkspaceTrust` is now a real `EventEmitter.event` returning a real
`Disposable`, where it previously returned a bare `Disposable` that never
fired anything. Its emitter is disposed alongside the session's other
emitters. The field is updated before the emitter fires — the same ordering
`window.state` already uses — so a listener that reads `workspace.isTrusted`
from inside its own handler observes `true`, never the stale value.

Two update paths are required, not one, because `session.options` is a
spread-copy taken at activation time: mutating the loader's options is not
retroactive to a session that already activated. `ExtensionLoader.handleRequest`
therefore updates `this.options.workspaceTrusted` so any extension activated
later starts with the current value, while the same notification also fans out
to every live session so each one updates its own instance field directly.
`mergeSessionOptions` accepts `workspaceTrusted` as a boolean for the same
reason — a registration resync must be able to seed the loader-level value too.

The native push deduplicates: `CExtensionService::SetWorkspaceTrusted` remembers
the last value it sent and skips a resend of an identical value, so a repeated
identical push can never be mistaken downstream for a fresh grant.

The exthost cohort passes 58/58.
