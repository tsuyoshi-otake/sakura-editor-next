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

Before any of this becomes supported, `workspace.isTrusted` must stop being a
hardcoded `true`. Terminal creation is process creation, and exposing it under
an unconditional trust answer would let any installed extension spawn a process
with no user gesture. Implementing the Terminal API is therefore gated on an
extension-origin process-launch policy, not merely on a native service being
ready. Shell integration must never be aliased to `sendText`, and no code path
may special-case a particular extension's ID or command.
