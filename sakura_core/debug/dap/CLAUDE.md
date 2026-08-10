# Phase 6 Debug Adapter Protocol Guidance

## Codec Boundary

`CDapProtocolCodec` owns only bounded incremental DAP framing and strict message
validation. It has no process, pipe, socket, request table, timeout, breakpoint,
stack, variable, evaluation, Debug Console, or window responsibility.

- Accept only canonical `Content-Length` framing with CRLF delimiters. Bound
  headers, bodies, JSON nesting, and completed messages before retaining them.
- Validate UTF-8 and strict JSON before validating the DAP envelope. Do not
  accept JSONC comments/trailing commas, duplicate members, unknown framing
  headers, LF-only delimiters, or incomplete envelopes.
- A malformed stream enters `Failed` and never scans ahead for a later header.
  Recovery requires an explicit `Reset` for a new transport stream.
- Completed messages preceding a later bad frame remain valid. An invalid
  partial message is never appended.
- `Stop` discards partial input and disables Feed/Encode until explicit Reset.

## Session Boundary

A DAP session layer must inject and exclusively own one byte transport, assign
monotonic outgoing sequence numbers, bound and finalize every pending request,
correlate responses exactly once, and expose server requests/events through
fault-contained callbacks outside locks. Caller-driven timeout/cancellation is
preferred over hidden polling/retry loops. Transport/codec failure, Close, and
Stop must each finalize all pending requests and close the transport once.
Serialize physical transport sends even when callers are concurrent. Sequence
exhaustion and saturated advisory-drop accounting are explicit terminal/bounded
conditions, not wraparound.

An external Stop waits for active callbacks to drain. A callback-originated
Stop returns a deferred result and is finalized at the safe outer delivery
boundary. Callbacks borrow the session; destroying it inside one of those
callbacks is unsupported.

The Debug Console is a view over session evaluate/output events and never owns
the adapter or transport. Stale session generations cannot append to a newer
console.

## Verified Checkpoint

`DapProtocolCodec.*` passes 10/10 and `DapSession.*` passes 17/17. The session
checkpoint proves monotonic client sequence allocation, bounded pending and
completed request state, exact response correlation, one-response tombstones
for cancelled/expired requests, server request/event delivery, caller-driven
cancel/expiry, transport/codec terminal failure, listener fault containment,
serialized physical sends, sequence exhaustion, callback-draining Stop,
destructor finalization without borrowed-listener dispatch, and once-only
transport close through an injected fake transport.

The production adapter process/pipe transport, initialize/launch sequencing,
server-request replies, breakpoint/stack/variable models, evaluation adapter,
runtime ownership, and native Debug Console remain incomplete. Do not describe
the pure session as a launchable debugger.
