---------------------------- MODULE ExtensionHostLease ----------------------------
(***************************************************************************)
(* Formal model (TLA+ / TLC) of the extension-host editor lease protocol  *)
(* owned by the hidden control process, as of develop 36f0c2550:          *)
(*                                                                        *)
(*   - CControlTray MYWM_EXTENSION_HOST_ACQUIRE / _RELEASE handlers       *)
(*     (sakura_core/_main/CControlTray.cpp)                               *)
(*   - CExtensionHostController lease tracking: SYNCHRONIZE process       *)
(*     handle pinning, per-owner nesting, rollback, and the periodic      *)
(*     ReclaimTerminatedEditorLeases tick                                 *)
(*     (sakura_core/extension/CExtensionHostController.cpp)               *)
(*                                                                        *)
(* Concurrency model. The tray runs a single-threaded window-message      *)
(* loop, so one message handler never interleaves with another handler or *)
(* with the WM_TIMER tick; controller calls made inside one handler are   *)
(* therefore modeled as atomic steps. What DOES interleave with every     *)
(* machine instruction is the OS: an editor process can die at any point, *)
(* and its PID can be recycled by an unrelated new process. The acquire   *)
(* handler is therefore split into three steps with OS interleaving       *)
(* between them, exactly matching the code's own race windows:            *)
(*                                                                        *)
(*   BeginAcquire     first IsRegisteredEditorLeaseOwner check            *)
(*   CtrlAcquire*     CExtensionHostController::AcquireLease (atomic:     *)
(*                    OpenProcess pin, dead check, count, vault/broker    *)
(*                    acquisition with rollback on refusal)               *)
(*   Recheck*         second IsRegisteredEditorLeaseOwner check; a        *)
(*                    failure rolls the just-acquired lease back          *)
(*                                                                        *)
(* Process generations. proc[p].gen is the incarnation counter of PID     *)
(* slot p; Recycle increments it, modeling PID reuse. A SYNCHRONIZE       *)
(* handle opened while generation g owned the PID keeps designating       *)
(* generation g forever - that is the entire point of pinning. The        *)
(* PinHandles constant selects between:                                   *)
(*   TRUE  - current design: the dead-owner test asks the pinned          *)
(*           generation's process object (handle semantics), and          *)
(*   FALSE - a hypothetical legacy design that re-derives liveness from   *)
(*           the numeric PID, which is exactly the ABA defect the         *)
(*           production code guards against.                              *)
(*                                                                        *)
(* Abstractions (deliberate, documented):                                 *)
(*   - Broker host state machine (Starting/Ready/quiesce/retry) and the   *)
(*     Secret Vault activation CAS are folded into one nondeterministic   *)
(*     accept/reject outcome per acquisition; every reject exercises the  *)
(*     RollbackAcquiredLease path.                                        *)
(*   - kMaximumTrackedEditorLeaseOwners (256) is unreachable with the     *)
(*     small PID set and is not modeled; kMaximumEditorLeasesPerOwner is  *)
(*     modeled as CapPerOwner.                                            *)
(*   - Controller Shutdown() and handshake/host-lost flows are out of     *)
(*     scope; the model covers the lease lifecycle only.                  *)
(*   - Message transport is the tray's FIFO queue. An editor blocks in    *)
(*     SendMessage, so each live generation has at most one in-flight     *)
(*     message; a message survives its sender's death (conservative).    *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    PIDS,            \* symbolic PID slots, e.g. {p1, p2}
    MaxGen,          \* incarnations per PID slot (bounds recycling)
    MaxNested,       \* nested leases one editor generation requests
    CapPerOwner,     \* kMaximumEditorLeasesPerOwner (fail-closed cap)
    QCap,            \* tray queue bound
    PinHandles,      \* TRUE: dead check uses the pinned generation (current design)
    RecheckAfterPin, \* TRUE: tray re-validates HWND/PID after AcquireLease (current design)
    ValidateSender   \* TRUE: tray validates HWND/PID before forwarding (current design)

ASSUME /\ MaxGen \in Nat \ {0}
       /\ MaxNested \in Nat \ {0}
       /\ CapPerOwner \in Nat \ {0}
       /\ QCap \in Nat \ {0}
       /\ PinHandles \in BOOLEAN
       /\ RecheckAfterPin \in BOOLEAN
       /\ ValidateSender \in BOOLEAN

Gens == 1..MaxGen
Msg == [kind: {"acq", "rel"}, pid: PIDS, gen: Gens]
NoMsg == [kind |-> "none"]
IdleTray == [phase |-> "Idle", msg |-> NoMsg]

VARIABLES
    proc,     \* [PIDS -> [gen: Gens, alive: BOOLEAN]] OS process table
    queue,    \* Seq(Msg): tray message queue (FIFO)
    tray,     \* [phase: {"Idle","Validated","Pinned"}, msg: Msg \cup {NoMsg}]
    count,    \* m_editorLeases[pid].leaseCount (0 = untracked)
    pinnedG,  \* generation the SYNCHRONIZE handle designates (bookkeeping when PinHandles=FALSE)
    brokerL,  \* broker-side lease count per pid
    vaultL,   \* Secret Vault lease count per pid
    ledger,   \* [PIDS -> [Gens -> 0..1]] open process handles (exactly-once discipline)
    held      \* [PIDS -> [Gens -> Nat]] grants an editor generation received and has not released

vars == <<proc, queue, tray, count, pinnedG, brokerL, vaultL, ledger, held>>

(***************************************************************************)
(* Helpers                                                                 *)
(***************************************************************************)

\* IsEditorProcessTerminated on the stored handle. With pinning, the handle
\* designates generation pinnedG[p]; it is signaled once that generation is
\* gone (superseded or dead). Without pinning, liveness is re-derived from
\* the numeric PID: the current occupant's liveness - the ABA mistake.
PinDead(p) ==
    IF PinHandles
    THEN proc[p].gen > pinnedG[p] \/ (proc[p].gen = pinnedG[p] /\ ~proc[p].alive)
    ELSE ~proc[p].alive

\* IsRegisteredEditorLeaseOwner: the registered editor HWND is live and
\* GetWindowThreadProcessId reports this PID, i.e. the sender generation is
\* still the live current occupant of the PID slot.
Registered(p, g) == proc[p].alive /\ proc[p].gen = g

InFlight(p, g) ==
    \/ \E i \in 1..Len(queue): queue[i].pid = p /\ queue[i].gen = g
    \/ tray.phase # "Idle" /\ tray.msg.pid = p /\ tray.msg.gen = g

DeadOwners == {p \in PIDS: count[p] > 0 /\ PinDead(p)}

\* ReleaseTrackedLease: decrement one lease, close the pinned handle when the
\* owner entry disappears; an untracked pid is a complete no-op (the code
\* returns before touching broker or vault).
ReleaseOne(p) ==
    IF count[p] = 0
    THEN UNCHANGED <<count, pinnedG, brokerL, vaultL, ledger>>
    ELSE /\ count'   = [count   EXCEPT ![p] = @ - 1]
         /\ brokerL' = [brokerL EXCEPT ![p] = @ - 1]
         /\ vaultL'  = [vaultL  EXCEPT ![p] = @ - 1]
         /\ ledger'  = IF count[p] = 1
                       THEN [ledger EXCEPT ![p][pinnedG[p]] = 0]
                       ELSE ledger
         /\ pinnedG' = pinnedG

\* ReclaimTerminatedEditorLeases: release every lease of every owner whose
\* pinned process object is signaled, closing each handle exactly once.
Reclaim(S) ==
    /\ count'   = [p \in PIDS |-> IF p \in S THEN 0 ELSE count[p]]
    /\ brokerL' = [p \in PIDS |-> IF p \in S THEN 0 ELSE brokerL[p]]
    /\ vaultL'  = [p \in PIDS |-> IF p \in S THEN 0 ELSE vaultL[p]]
    /\ ledger'  = [p \in PIDS |-> IF p \in S THEN [g \in Gens |-> 0] ELSE ledger[p]]
    /\ pinnedG' = pinnedG

(***************************************************************************)
(* OS actions                                                              *)
(***************************************************************************)

Die(p) ==
    /\ proc[p].alive
    /\ proc' = [proc EXCEPT ![p].alive = FALSE]
    /\ UNCHANGED <<queue, tray, count, pinnedG, brokerL, vaultL, ledger, held>>

\* PID reuse: a new, unrelated process (itself an editor in the worst case)
\* is created with the same numeric PID.
Recycle(p) ==
    /\ ~proc[p].alive
    /\ proc[p].gen < MaxGen
    /\ proc' = [proc EXCEPT ![p].gen = @ + 1, ![p].alive = TRUE]
    /\ UNCHANGED <<queue, tray, count, pinnedG, brokerL, vaultL, ledger, held>>

(***************************************************************************)
(* Editor actions. SendMessage blocks the sender, so one generation has at *)
(* most one in-flight message; held is decremented at release send time    *)
(* because the editor relinquishes the lease when it asks.                 *)
(***************************************************************************)

SendAcquire(p) ==
    /\ proc[p].alive
    /\ held[p][proc[p].gen] < MaxNested
    /\ ~InFlight(p, proc[p].gen)
    /\ Len(queue) < QCap
    /\ queue' = Append(queue, [kind |-> "acq", pid |-> p, gen |-> proc[p].gen])
    /\ UNCHANGED <<proc, tray, count, pinnedG, brokerL, vaultL, ledger, held>>

SendRelease(p) ==
    /\ proc[p].alive
    /\ held[p][proc[p].gen] > 0
    /\ ~InFlight(p, proc[p].gen)
    /\ Len(queue) < QCap
    /\ queue' = Append(queue, [kind |-> "rel", pid |-> p, gen |-> proc[p].gen])
    /\ held' = [held EXCEPT ![p][proc[p].gen] = @ - 1]
    /\ UNCHANGED <<proc, tray, count, pinnedG, brokerL, vaultL, ledger>>

(***************************************************************************)
(* Tray: MYWM_EXTENSION_HOST_ACQUIRE, step 1 - first                       *)
(* IsRegisteredEditorLeaseOwner check. A failed check replies 0 without    *)
(* touching the controller.                                                *)
(***************************************************************************)

BeginAcquire ==
    /\ tray.phase = "Idle"
    /\ queue # <<>>
    /\ Head(queue).kind = "acq"
    /\ queue' = Tail(queue)
    /\ LET m == Head(queue) IN
        IF ~ValidateSender \/ Registered(m.pid, m.gen)
        THEN tray' = [phase |-> "Validated", msg |-> m]
        ELSE tray' = tray
    /\ UNCHANGED <<proc, count, pinnedG, brokerL, vaultL, ledger, held>>

(***************************************************************************)
(* Controller: AcquireLease, atomic. OpenProcess pins whatever process     *)
(* CURRENTLY occupies the PID - which is not necessarily the sender.       *)
(***************************************************************************)

\* Untracked owner, OpenProcess fails or the occupant is already terminated.
CtrlAcquireUntrackedFail ==
    /\ tray.phase = "Validated"
    /\ count[tray.msg.pid] = 0
    /\ ~proc[tray.msg.pid].alive
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, count, pinnedG, brokerL, vaultL, ledger, held>>

\* Untracked owner, pinned successfully, but the Secret Vault or the broker
\* refused: RollbackAcquiredLease erases the fresh entry and closes the
\* handle it just opened, so the step nets to no controller-state change.
CtrlAcquireUntrackedReject ==
    /\ tray.phase = "Validated"
    /\ count[tray.msg.pid] = 0
    /\ proc[tray.msg.pid].alive
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, count, pinnedG, brokerL, vaultL, ledger, held>>

\* Untracked owner, pinned and accepted by vault and broker.
CtrlAcquireUntrackedOk ==
    /\ tray.phase = "Validated"
    /\ LET p == tray.msg.pid IN
        /\ count[p] = 0
        /\ proc[p].alive
        /\ count'   = [count   EXCEPT ![p] = 1]
        /\ pinnedG' = [pinnedG EXCEPT ![p] = proc[p].gen]
        /\ ledger'  = [ledger  EXCEPT ![p][proc[p].gen] = 1]
        /\ brokerL' = [brokerL EXCEPT ![p] = 1]
        /\ vaultL'  = [vaultL  EXCEPT ![p] = 1]
        /\ IF RecheckAfterPin
           THEN /\ tray' = [phase |-> "Pinned", msg |-> tray.msg]
                /\ held' = held
           ELSE /\ tray' = IdleTray
                /\ held' = [held EXCEPT ![tray.msg.pid][tray.msg.gen] = @ + 1]
    /\ UNCHANGED <<proc, queue>>

\* Tracked owner whose pinned process object is signaled: never extend a
\* dead lease - run the full reclaim, then reject this acquisition.
CtrlAcquireTrackedDead ==
    /\ tray.phase = "Validated"
    /\ count[tray.msg.pid] > 0
    /\ PinDead(tray.msg.pid)
    /\ Reclaim(DeadOwners)
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, held>>

\* Tracked owner at the per-owner cap: fail closed.
CtrlAcquireTrackedCap ==
    /\ tray.phase = "Validated"
    /\ count[tray.msg.pid] >= CapPerOwner
    /\ ~PinDead(tray.msg.pid)
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, count, pinnedG, brokerL, vaultL, ledger, held>>

\* Tracked live owner, but the vault or broker refused: rollback decrements
\* the count it just incremented; the pinned handle stays (count > 0).
CtrlAcquireTrackedReject ==
    /\ tray.phase = "Validated"
    /\ count[tray.msg.pid] > 0
    /\ count[tray.msg.pid] < CapPerOwner
    /\ ~PinDead(tray.msg.pid)
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, count, pinnedG, brokerL, vaultL, ledger, held>>

\* Tracked live owner, nested acquisition accepted.
CtrlAcquireTrackedOk ==
    /\ tray.phase = "Validated"
    /\ LET p == tray.msg.pid IN
        /\ count[p] > 0
        /\ count[p] < CapPerOwner
        /\ ~PinDead(p)
        /\ count'   = [count   EXCEPT ![p] = @ + 1]
        /\ brokerL' = [brokerL EXCEPT ![p] = @ + 1]
        /\ vaultL'  = [vaultL  EXCEPT ![p] = @ + 1]
        /\ IF RecheckAfterPin
           THEN /\ tray' = [phase |-> "Pinned", msg |-> tray.msg]
                /\ held' = held
           ELSE /\ tray' = IdleTray
                /\ held' = [held EXCEPT ![tray.msg.pid][tray.msg.gen] = @ + 1]
    /\ UNCHANGED <<proc, queue, pinnedG, ledger>>

(***************************************************************************)
(* Tray: MYWM_EXTENSION_HOST_ACQUIRE, step 3 - second                      *)
(* IsRegisteredEditorLeaseOwner check. "Pinning the process handle closes  *)
(* the PID-reuse race only if the registered HWND still identifies that    *)
(* PID after OpenProcess." A failure releases the just-acquired lease.     *)
(***************************************************************************)

RecheckPass ==
    /\ tray.phase = "Pinned"
    /\ Registered(tray.msg.pid, tray.msg.gen)
    /\ held' = [held EXCEPT ![tray.msg.pid][tray.msg.gen] = @ + 1]
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, count, pinnedG, brokerL, vaultL, ledger>>

RecheckFail ==
    /\ tray.phase = "Pinned"
    /\ ~Registered(tray.msg.pid, tray.msg.gen)
    /\ ReleaseOne(tray.msg.pid)
    /\ tray' = IdleTray
    /\ UNCHANGED <<proc, queue, held>>

(***************************************************************************)
(* Tray: MYWM_EXTENSION_HOST_RELEASE - validated, then one decrement.      *)
(***************************************************************************)

ProcessRelease ==
    /\ tray.phase = "Idle"
    /\ queue # <<>>
    /\ Head(queue).kind = "rel"
    /\ queue' = Tail(queue)
    /\ LET m == Head(queue) IN
        IF ~ValidateSender \/ Registered(m.pid, m.gen)
        THEN ReleaseOne(m.pid)
        ELSE UNCHANGED <<count, pinnedG, brokerL, vaultL, ledger>>
    /\ UNCHANGED <<proc, tray, held>>

(***************************************************************************)
(* WM_TIMER tick: ReclaimTerminatedEditorLeases. The 2-second interval is  *)
(* abstracted away; fairness below guarantees the tick keeps firing.       *)
(***************************************************************************)

Tick ==
    /\ tray.phase = "Idle"
    /\ Reclaim(DeadOwners)
    /\ UNCHANGED <<proc, queue, tray, held>>

(***************************************************************************)
(* Specification                                                           *)
(***************************************************************************)

Init ==
    /\ proc = [p \in PIDS |-> [gen |-> 1, alive |-> TRUE]]
    /\ queue = <<>>
    /\ tray = IdleTray
    /\ count = [p \in PIDS |-> 0]
    /\ pinnedG = [p \in PIDS |-> 0]
    /\ brokerL = [p \in PIDS |-> 0]
    /\ vaultL = [p \in PIDS |-> 0]
    /\ ledger = [p \in PIDS |-> [g \in Gens |-> 0]]
    /\ held = [p \in PIDS |-> [g \in Gens |-> 0]]

ProcessQueue == BeginAcquire \/ ProcessRelease

HandleValidated ==
    \/ CtrlAcquireUntrackedFail
    \/ CtrlAcquireUntrackedReject
    \/ CtrlAcquireUntrackedOk
    \/ CtrlAcquireTrackedDead
    \/ CtrlAcquireTrackedCap
    \/ CtrlAcquireTrackedReject
    \/ CtrlAcquireTrackedOk

HandleRecheck == RecheckPass \/ RecheckFail

Next ==
    \/ \E p \in PIDS: Die(p) \/ Recycle(p) \/ SendAcquire(p) \/ SendRelease(p)
    \/ ProcessQueue
    \/ HandleValidated
    \/ HandleRecheck
    \/ Tick

\* The tray message pump always drains its queue and finishes a started
\* handler (weak fairness); the WM_TIMER tick fires whenever the pump is
\* idle, and idleness recurs between messages, so the tick gets strong
\* fairness to survive schedules that interleave it with message bursts.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(ProcessQueue)
    /\ WF_vars(HandleValidated)
    /\ WF_vars(HandleRecheck)
    /\ SF_vars(Tick)

(***************************************************************************)
(* Safety invariants                                                       *)
(***************************************************************************)

TypeOK ==
    /\ proc \in [PIDS -> [gen: Gens, alive: BOOLEAN]]
    /\ queue \in Seq(Msg)
    /\ Len(queue) <= QCap
    /\ tray \in [phase: {"Idle", "Validated", "Pinned"}, msg: Msg \cup {NoMsg}]
    /\ (tray.phase = "Idle") <=> (tray.msg = NoMsg)
    /\ tray.phase # "Idle" => tray.msg.kind = "acq"
    /\ count \in [PIDS -> 0..CapPerOwner]
    /\ pinnedG \in [PIDS -> 0..MaxGen]
    /\ brokerL \in [PIDS -> 0..CapPerOwner]
    /\ vaultL \in [PIDS -> 0..CapPerOwner]
    /\ ledger \in [PIDS -> [Gens -> 0..1]]
    /\ held \in [PIDS -> [Gens -> 0..MaxNested]]

\* Broker lease count, Secret Vault lease count, and the controller's
\* tracked count never diverge - every rollback branch restores all three.
ComponentBalance ==
    \A p \in PIDS: brokerL[p] = count[p] /\ vaultL[p] = count[p]

\* Exactly one SYNCHRONIZE handle is open per tracked owner, it designates
\* the pinned generation, and it is closed exactly when tracking ends.
HandleLedger ==
    \A p \in PIDS: \A g \in Gens:
        ledger[p][g] = (IF count[p] > 0 /\ pinnedG[p] = g THEN 1 ELSE 0)

PendingRel(p, g) ==
    IF \E i \in 1..Len(queue):
        queue[i].kind = "rel" /\ queue[i].pid = p /\ queue[i].gen = g
    THEN 1 ELSE 0

\* Whenever the tray is idle (no acquire transaction in flight) and the
\* pinned owner is still the live occupant of its PID, every tracked lease
\* is attributable to that generation: the count equals the grants it
\* received and has not yet released (a release may still be queued). A
\* recycled foreign process can never be the live pinned owner of leases it
\* did not request. A DEAD pinned owner may leave a residual count behind
\* (its queued release is correctly dropped by the tray validation); that
\* residue is covered by DeadOwnerLeasesReclaimed, not by this invariant.
QuiescentAttribution ==
    tray.phase = "Idle" =>
        \A p \in PIDS:
            \/ count[p] = 0
            \/ PinDead(p)
            \/ count[p] = held[p][pinnedG[p]] + PendingRel(p, pinnedG[p])

(***************************************************************************)
(* Action properties (the PID-reuse theorems)                              *)
(***************************************************************************)

\* An already-tracked owner's lease count grows only for a request made by
\* the generation the SYNCHRONIZE handle pins: PID reuse cannot extend an
\* old grant.
NoForeignLeaseExtension ==
    [][\A p \in PIDS:
        (count[p] > 0 /\ count'[p] > count[p]) => tray.msg.gen = pinnedG[p]]_vars

\* A completed grant (successful reply to the editor) always leaves the
\* lease pinned to the requesting generation itself: PID reuse cannot
\* transfer a fresh grant to a bystander process either.
GrantsGoToPinnedGeneration ==
    [][\A p \in PIDS: \A g \in Gens:
        held'[p][g] > held[p][g] => count'[p] > 0 /\ pinnedG'[p] = g]_vars

(***************************************************************************)
(* Liveness                                                                *)
(***************************************************************************)

\* Once a tracked owner's pinned process object is signaled, the periodic
\* reclaim eventually releases every lease it held.
DeadOwnerLeasesReclaimed ==
    \A p \in PIDS: (count[p] > 0 /\ PinDead(p)) ~> count[p] = 0

====================================================================================
