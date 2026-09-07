-------------------------- MODULE SenpGhRequests --------------------------
EXTENDS Naturals, FiniteSets
CONSTANTS Clients, Deadline, CheckSingleFlight, CheckLastSubscriber,
          CheckCooldown, ReapBeforeTerminal
VARIABLES phase, accepted, subscribers, terminals, workers, now, cooldown,
          retried, reason, earlyStop, earlyDispatch
vars == <<phase, accepted, subscribers, terminals, workers, now, cooldown,
          retried, reason, earlyStop, earlyDispatch>>
Init ==
    /\ phase = "idle" /\ accepted = {} /\ subscribers = {} /\ workers = {}
    /\ terminals = [c \in Clients |-> 0] /\ now = 0 /\ cooldown = 0
    /\ retried = FALSE /\ reason = "none"
    /\ earlyStop = FALSE /\ earlyDispatch = FALSE
Subscribe(c) ==
    /\ c \notin accepted /\ now < Deadline
    /\ phase \in {"idle", "queued", "running"}
    /\ accepted' = accepted \cup {c} /\ subscribers' = subscribers \cup {c}
    /\ phase' = IF phase = "idle" THEN "queued" ELSE phase
    /\ workers' = IF phase = "running" /\ ~CheckSingleFlight
                   THEN workers \cup {c} ELSE workers
    /\ UNCHANGED <<terminals, now, cooldown, retried, reason, earlyStop, earlyDispatch>>
Dispatch ==
    /\ phase = "queued" /\ subscribers # {} /\ now < Deadline
    /\ (~CheckCooldown \/ now >= cooldown)
    /\ phase' = "running" /\ workers' = {CHOOSE c \in subscribers : TRUE}
    /\ earlyDispatch' = (earlyDispatch \/ now < cooldown)
    /\ UNCHANGED <<accepted, subscribers, terminals, now, cooldown, retried, reason, earlyStop>>
Unsubscribe(c) ==
    /\ phase \in {"queued", "running"} /\ c \in subscribers
    /\ LET remaining == subscribers \ {c}
           stop == remaining = {} \/ ~CheckLastSubscriber
       IN /\ subscribers' = remaining
          /\ phase' = IF stop THEN "cleaning" ELSE phase
          /\ reason' = IF stop THEN "cancel" ELSE reason
          /\ terminals' = IF remaining # {} THEN [terminals EXCEPT ![c] = @ + 1]
                          ELSE terminals
          /\ earlyStop' = (earlyStop \/ (stop /\ remaining # {}))
    /\ UNCHANGED <<accepted, workers, now, cooldown, retried, earlyDispatch>>
ReceiveSuccess ==
    /\ phase = "running" /\ phase' = "cleaning" /\ reason' = "success"
    /\ UNCHANGED <<accepted, subscribers, terminals, workers, now, cooldown,
                   retried, earlyStop, earlyDispatch>>
ReceiveRateLimit ==
    /\ phase = "running" /\ ~retried
    /\ phase' = "cleaning" /\ reason' = "retry" /\ retried' = TRUE
    /\ cooldown' = IF now + 2 < Deadline THEN now + 2 ELSE Deadline
    /\ UNCHANGED <<accepted, subscribers, terminals, workers, now, earlyStop, earlyDispatch>>
Tick ==
    /\ phase # "done" /\ now < Deadline /\ now' = now + 1
    /\ UNCHANGED <<phase, accepted, subscribers, terminals, workers, cooldown,
                   retried, reason, earlyStop, earlyDispatch>>
Expire ==
    /\ phase \in {"queued", "running"} /\ now = Deadline
    /\ phase' = "cleaning" /\ reason' = "timeout"
    /\ UNCHANGED <<accepted, subscribers, terminals, workers, now, cooldown,
                   retried, earlyStop, earlyDispatch>>
Cleanup ==
    /\ phase = "cleaning"
    /\ LET retry == reason = "retry" /\ now < Deadline /\ subscribers # {}
       IN /\ phase' = IF retry THEN "queued" ELSE "done"
          /\ subscribers' = IF retry THEN subscribers ELSE {}
          /\ terminals' = IF retry THEN terminals ELSE
              [c \in Clients |-> IF c \in accepted /\ terminals[c] = 0
                                 THEN terminals[c] + 1 ELSE terminals[c]]
    /\ workers' = IF ReapBeforeTerminal THEN {} ELSE workers
    /\ reason' = "none"
    /\ UNCHANGED <<accepted, now, cooldown, retried, earlyStop, earlyDispatch>>
Next == (\E c \in Clients : Subscribe(c) \/ Unsubscribe(c)) \/ Dispatch
        \/ ReceiveSuccess \/ ReceiveRateLimit \/ Tick \/ Expire \/ Cleanup
Spec == Init /\ [][Next]_vars /\ WF_vars(Tick) /\ WF_vars(Expire) /\ WF_vars(Cleanup)
SpecNoCleanupFairness == Init /\ [][Next]_vars /\ WF_vars(Tick) /\ WF_vars(Expire)
TypeOK ==
    /\ phase \in {"idle", "queued", "running", "cleaning", "done"}
    /\ accepted \subseteq Clients /\ subscribers \subseteq accepted /\ workers \subseteq Clients
    /\ terminals \in [Clients -> 0..2] /\ now \in 0..Deadline /\ cooldown \in 0..Deadline
    /\ retried \in BOOLEAN /\ reason \in {"none", "success", "retry", "cancel", "timeout"}
    /\ earlyStop \in BOOLEAN /\ earlyDispatch \in BOOLEAN
SingleFlight == Cardinality(workers) <= 1
SharedLeasePreserved == ~earlyStop
RespectRateWindow == ~earlyDispatch
TerminalOwnsNoProcess == phase = "done" => workers = {}
AtMostOnce == \A c \in Clients : terminals[c] <= 1
PendingHasOwner == \A c \in accepted : terminals[c] = 0 =>
    (c \in subscribers \/ phase = "cleaning")
EveryAcceptedTerminates == \A c \in Clients : (c \in accepted) ~> (terminals[c] = 1)
=============================================================================
