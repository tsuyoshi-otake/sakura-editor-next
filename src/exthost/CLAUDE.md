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

## Contribution Loading

`contributes.viewsContainers` is an object keyed by location, and VS Code
accepts exactly three keys: `activitybar`, `panel`, `secondarySidebar`. The
loader passes the key through verbatim and drops any group under another key,
the way VS Code treats it as a contribution error. Never fold an unknown key
into a default — that puts a container somewhere the extension never declared.

A container entry's `when` travels with it. It is not a View-only field: an
extension that supports more than one location declares the same container once
per location and gates each copy on a context key, so a dropped clause renders
every alternative at once. The clause is forwarded, never evaluated here; the
native projection owns evaluation
([`../../sakura_core/window/CLAUDE.md`](../../sakura_core/window/CLAUDE.md)).

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
