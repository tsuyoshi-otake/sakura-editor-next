# Phase 4 Request, Proxy, and WinHTTP Guidance

## Scope

`platform/request/` owns the transport-neutral request contract, bounded retry
orchestration, challenge-scoped credential seam, response cache seam, and Win32
transport adapters. Configuration, OpenVSX, extension UI, and HWND state do not
belong here.

## Dependency and Lifetime Rules

- Consumers depend on `IRequestService`; `RequestService` depends on injected
  transport, proxy, credential, clock, scheduler, jitter, and optional cache
  services. Keep that graph acyclic and destroy consumers before dependencies.
- A detached request context must own every dependency it can outlive. It may
  retain an immutable validated policy snapshot, but never a raw
  `IConfigurationService`, `CConfigurationNetworkPolicy`, workbench runtime, or
  native-window reference.
- `CConfigurationProxyService` has two deliberate forms: a live form that reads
  one coherent configuration snapshot per selection, and a detached form that
  owns one previously validated snapshot. Do not reintroduce multi-key
  `GetValue` reads or borrow policy objects into workers.
- WinHTTP system/PAC resolution remains behind `ISystemProxyResolver`.
  Configuration code must not call PAC, WPAD, WinHTTP, environment discovery,
  or credential APIs directly.

## Stateful Request Rules

- One caller cancellation token and one monotonic deadline cover proxy/PAC
  resolution, credential lookup, transport, redirects, retry delay, and
  response consumption. No stage may reset or extend the budget.
- Deduplicate compatible in-flight requests. Bound redirects and retries,
  respect `Retry-After`, and use exponential backoff with jitter. Every branch
  reaches exactly one typed terminal `RequestResult`.
- Enforce method/header/body/redirect limits before and while consuming input.
  Redirects may not downgrade HTTPS, introduce userinfo, or escape URI
  validation.
- `EProxySupport::Off` performs no proxy lookup. Manual/system selections are
  validated before transport use. Proxy configuration and credentials never
  appear in diagnostics.
- The production WinHTTP transport has no TLS-validation bypass. Compatibility
  settings that request weakened validation terminate as unsupported before
  network work.
- Credentials are requested only after a typed `401`/`407` challenge and are
  scoped to that challenge. They are transient request inputs, never settings
  or caller-injected general headers.

## Phase 4 Production Checkpoint (2026-07-31)

- OpenVSX search, VSIX download, and optional SHA-256 retrieval now use the
  shared `RequestService` graph through `OpenVsxRequestServiceAdapter`.
- `OpenVsxProductionClient` owns its WinHTTP transport, system-proxy resolver,
  immutable configuration-backed proxy service, clock, scheduler, jitter,
  request service, and registry adapter. A detached Extensions-pane job owns
  the resulting client and therefore cannot outlive borrowed configuration or
  workbench objects.
- The production graph currently uses an explicit no-credential adapter.
  Authentication challenges remain typed failures until the control-owned
  Secret Vault and challenge credential adapter are composed. Do not describe
  authenticated proxy/registry access as supported yet.

## Verification

- Keep ordinary tests deterministic and network-free. Cover every terminal
  outcome, cancellation before dispatch and during waits, deadline exhaustion,
  response limits, redirect validation, request deduplication, retry bounds,
  proxy modes, invalid resolver results, and lifetime after configuration
  teardown.
- Live proxy, PAC, TLS, authentication, and registry tests require an isolated
  opt-in environment and must never run as part of the default suite.

