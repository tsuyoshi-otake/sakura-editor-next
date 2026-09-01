# Debug Console Pure Model Guidance

`DebugConsoleModel.*` is a bounded, deterministic, thread-safe state model for
the Debug Console.  It owns session-generation fencing, retained transcript and
REPL history, evaluation identity/idempotency, caller-driven expiry, and
listener delivery.  It deliberately has no DAP types, process, pipe, timer,
thread, UI, or extension-host dependency.

Every session-scoped mutation supplies the active generation.  Old generations
must be rejected without modifying the newer session.  An operation identifier
replays only when its active/retained operation has exactly the same generation,
expression, and deadline; a different payload is a conflict.  Terminal evaluations are
retained only to the configured bound, so adapters must not rely on replay after
their completion record has been evicted.

Callbacks run after releasing the mutex, tolerate reentrancy, and contain
exceptions. All capacity eviction and snapshot ordering must stay
deterministic. An external Stop waits for callbacks to drain; a
callback-originated Stop returns the typed deferred result and is finalized by
the safe outer delivery boundary. Callbacks borrow the model, so destroying it
inside one is unsupported. Destructor fallback clears listeners before Stop and
never dispatches borrowed callbacks; explicit Stop owns terminal delivery.

Identifiers and sequence values never wrap; exhaustion is explicit and advisory
drop counters saturate. Resource-exhausted mutations are transactional.
`DisposeSession` retains the active session and unprocessed pending evaluations
when terminalization cannot be retained, so a later retry owns completion.
`Stop` and `DisposeSession` finalize pending work explicitly; neither destruction
nor a UI disappearance is a substitute for that lifecycle signal.

Verified scope: `DebugConsoleModel.*` passes 14/14, proving strict UTF-8 and enum
validation, session-generation fencing, bounded transcript/evaluation state,
transactional exhaustion, caller-driven expiry, saturated drops, and
external/reentrant Stop behavior plus destructor fallback without listener
delivery. Residual production work: a DAP/session
adapter must forward requests/results and issue cancellation, the runtime must
own the model, and native UI/view integration must render snapshots and own user
interaction. None is implemented or implied here; the current Debug Console
contribution is descriptor-only.
