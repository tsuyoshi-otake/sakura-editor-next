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
