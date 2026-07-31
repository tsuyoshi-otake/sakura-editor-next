# Phase 6 PORTS Pure-State Guidance

`PortForwardingService` is a process-local, pure model of the VS Code PORTS
surface. It owns discovered port metadata, an explicit forwarding lifecycle,
and a copied snapshot only. It must never bind sockets, open tunnels, launch a
process, poll, retry, or infer backend success from endpoint data.

- A producer is identified by `PortOwner { id, generation }`. A newer
  generation atomically evicts the prior generation's ports; an exact owner
  disposal creates a tombstone that fences late work. Do not weaken this to an
  owner-ID-only operation. Owner identities, including tombstones, are bounded
  by `maximumOwners`; capacity failure is explicit and tombstones are never
  silently evicted in a way that could revive stale producers.
- Match VS Code's address direction: the discovered identity is the remote
  host/port, while a successful tunnel supplies a local address and optional
  local port. Preserve protocol, privacy, source description, process
  description, label, and closeability as projection metadata.
- A host/backend adapter explicitly drives `Discovered -> Forwarding ->
  Forwarded|Failed` and `Forwarding|Forwarded -> Stopping -> Stopped`. Terminal
  states remain observable in snapshots until owner disposal or service Stop.
- Every accepted mutation has an operation ID and optional expected revision.
  Remembered exact replay returns `Replayed`; altered reuse conflicts. Revision
  never wraps and exhaustion is explicit.
- Snapshot order is stable port-ID order. Notifications are queued in committed
  revision order, drained outside the lock, and listener exceptions are
  contained. A bounded notification queue may drop advisory delivery; snapshots
  remain authoritative and expose the drop count.
- An external `Stop` waits for active listener callbacks to drain. A
  callback-originated Stop returns a typed deferred result and is finalized by
  the safe outer delivery boundary. Destruction from inside a callback is
  unsupported because the callback still borrows the service. Terminal Stop
  clears ports, operation history, and listeners. There is no autonomous retry
  or background worker to join.

Adapters may own real backend handles, policy, and user-visible error mapping,
but they must dispose those handles before asking the pure model to forget the
owner. Native PORTS projections and extension RPC bridges remain separate Phase
6 work.

The focused `PortForwardingService.*` suite passes 12/12. It proves bounded
owner/tombstone lifetime, remote-to-local projection metadata, deterministic
state transitions, saturated advisory-drop accounting, and callback-draining
Stop behavior. No forwarding backend, runtime owner, native PORTS renderer, or
extension RPC bridge is implied; the current PORTS contribution remains
descriptor-only.
