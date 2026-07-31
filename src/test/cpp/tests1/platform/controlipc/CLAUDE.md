# Control IPC Test Guidance

Keep codec, security, endpoint, gateway, and shutdown tests deterministic and
independent of the editor UI. Use unique test-owned endpoint names and temporary
profile roots. Assert fragmented/coalesced frames, hostile lengths and fields,
version and generation mismatch, current-user ACLs, peer rejection, exact
operation replay, lost-response recovery, cancellation/deadline races, bounded
overload, and one observable terminal result per accepted request.

