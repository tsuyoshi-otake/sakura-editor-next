------------------------- MODULE ControlStartupHandshake -------------------------
(***************************************************************************)
(* Formal model of the control-process singleton startup handshake.       *)
(*                                                                         *)
(* Code under model (develop 36f0c2550):                                   *)
(*   - CProcessFactory::IsExistControlProcess                              *)
(*       sakura_core/_main/CProcessFactory.cpp:163-176                     *)
(*       OpenMutex on GSTR_MUTEX_SAKURA_CP+profile: TRUE only while some   *)
(*       control process holds the named mutex object.                     *)
(*   - CProcessFactory::StartControlProcess                                *)
(*       sakura_core/_main/CProcessFactory.cpp:188-287                     *)
(*       CreateEvent(manual reset) on GSTR_EVENT_SAKURA_CP_INITIALIZED+    *)
(*       profile; ERROR_ALREADY_EXISTS -> "busy" failure. Then             *)
(*       CreateProcess(-NOWIN) and WaitForMultipleObjects({event, child},  *)
(*       15s): only WAIT_OBJECT_0 (the event) is success; child death and  *)
(*       timeout are explicit failures.                                    *)
(*   - CControlProcess::InitializeProcess                                  *)
(*       sakura_core/_main/CControlProcess.cpp:141-274                     *)
(*       OpenEvent (launcher's event, may be absent), CreateMutex CP       *)
(*       (ERROR_ALREADY_EXISTS -> loser exits), shared-data init, platform *)
(*       runtime start (fail closed), tray window creation + hwndTray      *)
(*       publication, and only then SetEvent.                              *)
(*   - CNormalProcess::InitializeProcess                                   *)
(*       sakura_core/_main/CNormalProcess.cpp:264                          *)
(*       IsExistControlProcess() || StartControlProcess(): an editor that  *)
(*       observes the mutex proceeds immediately and never waits for       *)
(*       readiness.                                                        *)
(*                                                                         *)
(* Documented claims this specification checks:                            *)
(*   1. CProcessFactory.cpp:72-77: several control processes may be        *)
(*      launched concurrently, but the one that first acquires the CP      *)
(*      mutex is the only survivor (AtMostOneSurvivor, MuxConsistency).    *)
(*   2. CControlProcess.cpp:195-198: "The launcher-ready event below must  *)
(*      never advertise an intermediate platform state"                    *)
(*      (SignalOnlyAfterTrayPublished, LauncherOkImpliesTray). The         *)
(*      SignalEarly variant removes the ordering and TLC finds the         *)
(*      forbidden advertisement.                                           *)
(*   3. The launcher's bounded wait always terminates (EveryWaitResolves). *)
(*   4. The warm path (mutex observed -> proceed) can run ahead of tray    *)
(*      publication. WarmImpliesSawTray is deliberately violated in the    *)
(*      WarmWindow configuration to certify that this window is reachable  *)
(*      in the production design; the design tolerates it because every    *)
(*      tray-dependent message fails closed and a real editor is gated by  *)
(*      the platform Hello handshake later in startup.                     *)
(*                                                                         *)
(* Named kernel objects are modeled with handle-lifetime semantics: a      *)
(* named object exists exactly while some live process holds a handle to   *)
(* it, and a new CreateEvent after all holders are gone produces a fresh   *)
(* unsignaled object (a new generation).                                   *)
(*                                                                         *)
(* Deliberate abstractions:                                                *)
(*   - The editor-side GSTR_EVENT_SAKURA_EP_INITIALIZED handshake (tray    *)
(*     spawning editors) is a separate protocol and is out of scope.       *)
(*   - Shared-memory creation is first-comer-wins in both processes and    *)
(*     carries no handshake of its own; only the hwndTray publication      *)
(*     matters here and is modeled as the boolean `tray`.                  *)
(*   - Losing controls close their transient CP-mutex handle immediately   *)
(*     (destructor), so the mutex object is modeled as existing exactly    *)
(*     while the winning owner is alive and initialized/running.           *)
(*   - Launcher process death is not modeled; the launcher's failure paths *)
(*     (busy, timeout, child death) already release the event handle.      *)
(*   - The version-mismatch failure of shared-memory attach and the        *)
(*     abandoned initialize-mutex recovery (MYWM_RECOVER_APPNODE) are out  *)
(*     of scope.                                                           *)
(*   - MaxEvGen bounds how many cold-start storms the model explores.      *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Launchers,          \* editor processes taking the startup path
    Controls,           \* pool of potential control processes
    MaxEvGen,           \* bound on init-event object generations
    MaxStandalone,      \* controls started directly with -NOWIN by the user
    SignalAfterReady    \* TRUE = production ordering: SetEvent only after
                        \* platform Running and tray publication

VARIABLES
    ctl,          \* [Controls -> [st, by, sig]]
    ln,           \* [Launchers -> [st, child, saw]]
    evGen,        \* generation of the init-event object, 0 = never created
    evSig,        \* the current event object is signaled
    evHold,       \* processes holding a handle to the current event object
    muxOwner,     \* the control owning the CP mutex, or "none"
    tray,         \* hwndTray has been published to shared memory
    nStandalone   \* standalone -NOWIN controls started so far

vars == <<ctl, ln, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

CtlStates == {"off", "boot", "opened", "won", "shared", "plat", "tray",
              "ready", "exited", "dead"}
LnStates  == {"idle", "cold", "busy", "held", "waiting", "warm",
              "ok", "tmo", "chdead"}

SurvivorStates == {"won", "shared", "plat", "tray", "ready"}

MutexExists == muxOwner # "none"
EvExists    == evHold # {}

TypeOK ==
    /\ ctl \in [Controls -> [st: CtlStates,
                             by: Launchers \cup {"user", "none"},
                             sig: BOOLEAN]]
    /\ ln \in [Launchers -> [st: LnStates,
                             child: Controls \cup {"none"},
                             saw: BOOLEAN]]
    /\ evGen \in 0..MaxEvGen
    /\ evSig \in BOOLEAN
    /\ evHold \subseteq (Launchers \cup Controls)
    /\ muxOwner \in Controls \cup {"none"}
    /\ tray \in BOOLEAN
    /\ nStandalone \in 0..MaxStandalone

Init ==
    /\ ctl = [c \in Controls |-> [st |-> "off", by |-> "none", sig |-> FALSE]]
    /\ ln = [l \in Launchers |-> [st |-> "idle", child |-> "none", saw |-> FALSE]]
    /\ evGen = 0
    /\ evSig = FALSE
    /\ evHold = {}
    /\ muxOwner = "none"
    /\ tray = FALSE
    /\ nStandalone = 0

(***************************************************************************)
(* Launcher (editor process) actions.                                      *)
(***************************************************************************)

\* IsExistControlProcess() returned TRUE: proceed without waiting for
\* readiness. `saw` records whether the tray was already published at that
\* moment; the WarmWindow configuration asserts it always was and TLC
\* refutes that, certifying the tolerated warm-race window.
LWarm(l) ==
    /\ ln[l].st = "idle"
    /\ MutexExists
    /\ ln' = [ln EXCEPT ![l].st = "warm", ![l].saw = tray]
    /\ UNCHANGED <<ctl, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* IsExistControlProcess() returned FALSE: commit to the cold-start path.
\* The code does not recheck the mutex afterwards.
LCold(l) ==
    /\ ln[l].st = "idle"
    /\ ~MutexExists
    /\ ln' = [ln EXCEPT ![l].st = "cold"]
    /\ UNCHANGED <<ctl, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* CreateEvent succeeded: a fresh unsignaled object, this launcher holds it.
LCreateEv(l) ==
    /\ ln[l].st = "cold"
    /\ ~EvExists
    /\ evGen < MaxEvGen
    /\ evGen' = evGen + 1
    /\ evSig' = FALSE
    /\ evHold' = {l}
    /\ ln' = [ln EXCEPT ![l].st = "held"]
    /\ UNCHANGED <<ctl, muxOwner, tray, nStandalone>>

\* CreateEvent hit ERROR_ALREADY_EXISTS: the busy failure at
\* CProcessFactory.cpp:202-206.
LBusy(l) ==
    /\ ln[l].st = "cold"
    /\ EvExists
    /\ ln' = [ln EXCEPT ![l].st = "busy"]
    /\ UNCHANGED <<ctl, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* CreateProcess(-NOWIN) succeeded; start waiting on {event, child}.
LSpawn(l) ==
    /\ ln[l].st = "held"
    /\ \E c \in Controls:
        /\ ctl[c].st = "off"
        /\ ctl' = [ctl EXCEPT ![c].st = "boot", ![c].by = l]
        /\ ln' = [ln EXCEPT ![l].st = "waiting", ![l].child = c]
    /\ UNCHANGED <<evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* WaitForMultipleObjects returned WAIT_OBJECT_0: the ready event fired.
\* WFMO returns the lowest signaled index, so the event takes priority over
\* a simultaneously dead child.
LWakeOk(l) ==
    /\ ln[l].st = "waiting"
    /\ evSig
    /\ ln' = [ln EXCEPT ![l].st = "ok"]
    /\ evHold' = evHold \ {l}
    /\ UNCHANGED <<ctl, evGen, evSig, muxOwner, tray, nStandalone>>

\* The spawned child died before the event fired (WAIT_OBJECT_0+1).
LWakeChildDead(l) ==
    /\ ln[l].st = "waiting"
    /\ ~evSig
    /\ ctl[ln[l].child].st \in {"exited", "dead"}
    /\ ln' = [ln EXCEPT ![l].st = "chdead"]
    /\ evHold' = evHold \ {l}
    /\ UNCHANGED <<ctl, evGen, evSig, muxOwner, tray, nStandalone>>

\* The 15-second timeout elapsed before the event fired.
LWakeTimeout(l) ==
    /\ ln[l].st = "waiting"
    /\ ~evSig
    /\ ln' = [ln EXCEPT ![l].st = "tmo"]
    /\ evHold' = evHold \ {l}
    /\ UNCHANGED <<ctl, evGen, evSig, muxOwner, tray, nStandalone>>

LWake(l) == LWakeOk(l) \/ LWakeChildDead(l) \/ LWakeTimeout(l)

(***************************************************************************)
(* Control-process actions.                                                *)
(***************************************************************************)

\* The user started `sakura -NOWIN` directly: a control with no launcher.
CSpawnUser ==
    /\ nStandalone < MaxStandalone
    /\ \E c \in Controls:
        /\ ctl[c].st = "off"
        /\ ctl' = [ctl EXCEPT ![c].st = "boot", ![c].by = "user"]
    /\ nStandalone' = nStandalone + 1
    /\ UNCHANGED <<ln, evGen, evSig, evHold, muxOwner, tray>>

\* OpenEvent at the top of CControlProcess::InitializeProcess. Joining the
\* holder set keeps the named object alive even after the launcher gives up.
COpenEv(c) ==
    /\ ctl[c].st = "boot"
    /\ ctl' = [ctl EXCEPT ![c].st = "opened"]
    /\ evHold' = IF EvExists THEN evHold \cup {c} ELSE evHold
    /\ UNCHANGED <<ln, evGen, evSig, muxOwner, tray, nStandalone>>

\* CreateMutex CP without ERROR_ALREADY_EXISTS: this control is the winner.
CMuxWin(c) ==
    /\ ctl[c].st = "opened"
    /\ ~MutexExists
    /\ muxOwner' = c
    /\ ctl' = [ctl EXCEPT ![c].st = "won"]
    /\ UNCHANGED <<ln, evGen, evSig, evHold, tray, nStandalone>>

\* ERROR_ALREADY_EXISTS: the loser returns false and exits; its destructor
\* closes every handle. It never signals the event.
CMuxLose(c) ==
    /\ ctl[c].st = "opened"
    /\ MutexExists
    /\ ctl' = [ctl EXCEPT ![c].st = "exited"]
    /\ evHold' = evHold \ {c}
    /\ UNCHANGED <<ln, evGen, evSig, muxOwner, tray, nStandalone>>

\* Shared-memory initialization and settings load completed.
CShared(c) ==
    /\ ctl[c].st = "won"
    /\ ctl' = [ctl EXCEPT ![c].st = "shared"]
    /\ UNCHANGED <<ln, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* StartControlPlatform reached Running.
CPlatOk(c) ==
    /\ ctl[c].st = "shared"
    /\ ctl' = [ctl EXCEPT ![c].st = "plat"]
    /\ UNCHANGED <<ln, evGen, evSig, evHold, muxOwner, tray, nStandalone>>

\* StartControlPlatform failed: fail closed, release everything, exit
\* without ever publishing readiness (CControlProcess.cpp:199-229).
CPlatFail(c) ==
    /\ ctl[c].st = "shared"
    /\ ctl' = [ctl EXCEPT ![c].st = "exited"]
    /\ muxOwner' = "none"
    /\ evHold' = evHold \ {c}
    /\ UNCHANGED <<ln, evGen, evSig, tray, nStandalone>>

\* Tray window created and hwndTray published to shared memory.
CTrayPub(c) ==
    /\ ctl[c].st = "plat"
    /\ ctl' = [ctl EXCEPT ![c].st = "tray"]
    /\ tray' = TRUE
    /\ UNCHANGED <<ln, evGen, evSig, evHold, muxOwner, nStandalone>>

\* SetEvent on the launcher-owned event. In the production ordering
\* (SignalAfterReady = TRUE) this is only reachable after tray publication;
\* the SignalEarly variant allows it from any post-mutex state, which is
\* exactly the "advertise an intermediate platform state" defect.
CSignal(c) ==
    /\ ~ctl[c].sig
    /\ c \in evHold
    /\ IF SignalAfterReady
           THEN ctl[c].st = "tray"
           ELSE ctl[c].st \in {"won", "shared", "plat", "tray"}
    /\ evSig' = TRUE
    /\ ctl' = [ctl EXCEPT ![c].sig = TRUE]
    /\ UNCHANGED <<ln, evGen, evHold, muxOwner, tray, nStandalone>>

\* InitializeProcess returns true; the scoped event handle closes and the
\* control enters its message loop.
CReady(c) ==
    /\ ctl[c].st = "tray"
    /\ (c \in evHold) => ctl[c].sig
    /\ ctl' = [ctl EXCEPT ![c].st = "ready"]
    /\ evHold' = evHold \ {c}
    /\ UNCHANGED <<ln, evGen, evSig, muxOwner, tray, nStandalone>>

\* A control process dies at any live point: the kernel closes its handles,
\* which releases the mutex name and its event handle.
CDie(c) ==
    /\ ctl[c].st \in {"boot", "opened", "won", "shared", "plat", "tray", "ready"}
    /\ ctl' = [ctl EXCEPT ![c].st = "dead"]
    /\ muxOwner' = IF muxOwner = c THEN "none" ELSE muxOwner
    /\ evHold' = evHold \ {c}
    /\ UNCHANGED <<ln, evGen, evSig, tray, nStandalone>>

Next ==
    \/ \E l \in Launchers:
        LWarm(l) \/ LCold(l) \/ LCreateEv(l) \/ LBusy(l) \/ LSpawn(l)
        \/ LWake(l)
    \/ CSpawnUser
    \/ \E c \in Controls:
        COpenEv(c) \/ CMuxWin(c) \/ CMuxLose(c) \/ CShared(c)
        \/ CPlatOk(c) \/ CPlatFail(c) \/ CTrayPub(c) \/ CSignal(c)
        \/ CReady(c) \/ CDie(c)

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ \A l \in Launchers: WF_vars(LWake(l))

(***************************************************************************)
(* Safety properties.                                                      *)
(***************************************************************************)

\* Claim 1 (CProcessFactory.cpp:72-77): the mutex winner is the only
\* control that survives past the CreateMutex race.
AtMostOneSurvivor ==
    Cardinality({c \in Controls: ctl[c].st \in SurvivorStates}) <= 1

\* Every post-mutex control is the registered owner, and the owner is a
\* live post-mutex control.
MuxConsistency ==
    /\ \A c \in Controls: ctl[c].st \in SurvivorStates => muxOwner = c
    /\ MutexExists => ctl[muxOwner].st \in SurvivorStates

\* A signaled or held event object requires a creator generation.
EvConsistency ==
    (EvExists \/ evSig) => evGen > 0

\* Claim 2 (CControlProcess.cpp:195-198): the ready event never advertises
\* an intermediate platform state. Signaling requires that the tray HWND is
\* already published (which itself requires platform Running).
SignalOnlyAfterTrayPublished ==
    [][(~evSig /\ evSig') => tray']_vars

\* Launcher-visible form of claim 2: when StartControlProcess succeeds, the
\* tray HWND is already published in shared memory.
LauncherOkImpliesTray ==
    [][\A l \in Launchers:
        (ln[l].st = "waiting" /\ ln'[l].st = "ok") => tray]_vars

\* Warm-path attribution: an editor that proceeded because the mutex
\* existed had already seen the published tray. This does NOT hold in the
\* production design -- the WarmWindow configuration asserts it so that TLC
\* produces the reachability certificate for the tolerated window.
WarmImpliesSawTray ==
    \A l \in Launchers: ln[l].st = "warm" => ln[l].saw

(***************************************************************************)
(* Liveness.                                                               *)
(***************************************************************************)

\* Claim 3: the bounded wait always resolves. Some wake action is enabled
\* in every waiting state (the timeout needs no cooperation), so weak
\* fairness on LWake suffices.
EveryWaitResolves ==
    \A l \in Launchers:
        (ln[l].st = "waiting") ~> (ln[l].st \in {"ok", "tmo", "chdead"})

===================================================================================
