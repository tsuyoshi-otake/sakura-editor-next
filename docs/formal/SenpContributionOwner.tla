----------------------- MODULE SenpContributionOwner -----------------------
EXTENDS Naturals
CONSTANTS CheckGeneration, ClearOnRevoke, MaxGeneration
VARIABLES generation, phase, staged, registered, grant, running, mailbox, visible
vars == <<generation, phase, staged, registered, grant, running, mailbox, visible>>
Live == phase \in {"active", "preparing"}
Init ==
    /\ generation = 1 /\ phase = "active" /\ staged = 0
    /\ registered = 2 /\ grant = TRUE
    /\ running = 0 /\ mailbox = 0 /\ visible = 0
Prepare ==
    /\ phase = "active" /\ generation < MaxGeneration
    /\ phase' = "preparing" /\ staged' = 0
    /\ UNCHANGED <<generation, registered, grant, running, mailbox, visible>>
BuildStage ==
    /\ phase = "preparing" /\ staged < 2 /\ staged' = staged + 1
    /\ UNCHANGED <<generation, phase, registered, grant, running, mailbox, visible>>
CommitUpdate ==
    /\ phase = "preparing" /\ staged = 2
    /\ generation' = generation + 1 /\ phase' = "active"
    /\ staged' = 0 /\ visible' = 0
    /\ UNCHANGED <<registered, grant, running, mailbox>>
AbortUpdate ==
    /\ phase = "preparing" /\ phase' = "active" /\ staged' = 0
    /\ UNCHANGED <<generation, registered, grant, running, mailbox, visible>>
StartRead ==
    /\ Live /\ grant /\ running = 0 /\ mailbox = 0
    /\ running' = generation
    /\ UNCHANGED <<generation, phase, staged, registered, grant, mailbox, visible>>
FinishRead ==
    /\ running # 0 /\ running' = 0 /\ mailbox' = running
    /\ UNCHANGED <<generation, phase, staged, registered, grant, visible>>
Apply ==
    /\ Live /\ grant /\ mailbox # 0
    /\ visible' = IF CheckGeneration /\ mailbox # generation THEN visible ELSE mailbox
    /\ mailbox' = 0
    /\ UNCHANGED <<generation, phase, staged, registered, grant, running>>
Revoke ==
    /\ Live /\ phase' = "draining" /\ grant' = FALSE /\ staged' = 0
    /\ visible' = IF ClearOnRevoke THEN 0 ELSE visible
    /\ UNCHANGED <<generation, registered, running, mailbox>>
Drain ==
    /\ phase = "draining" /\ running = 0
    /\ phase' = "stopped" /\ registered' = 0 /\ mailbox' = 0
    /\ UNCHANGED <<generation, staged, grant, running, visible>>
Next == Prepare \/ BuildStage \/ CommitUpdate \/ AbortUpdate \/ StartRead
        \/ FinishRead \/ Apply \/ Revoke \/ Drain
AdvanceUpdate == BuildStage \/ CommitUpdate \/ AbortUpdate
Spec == Init /\ [][Next]_vars /\ WF_vars(FinishRead) /\ WF_vars(Drain)
        /\ WF_vars(AdvanceUpdate)
TypeOK ==
    /\ generation \in 1..MaxGeneration
    /\ phase \in {"active", "preparing", "draining", "stopped"}
    /\ staged \in 0..2 /\ registered \in 0..2 /\ grant \in BOOLEAN
    /\ running \in 0..MaxGeneration /\ mailbox \in 0..MaxGeneration
    /\ visible \in 0..MaxGeneration
AtomicContributions == registered \in {0, 2} /\ (Live => registered = 2)
RevokedGrant == ~Live => ~grant
NoRevokedView == ~Live => visible = 0
CurrentView == visible = 0 \/ visible = generation
StoppedOwnsNothing == phase = "stopped" =>
    (registered = 0 /\ running = 0 /\ mailbox = 0 /\ ~grant)
RetirementTerminates == (phase = "draining") ~> (phase = "stopped")
PreparationTerminates == (phase = "preparing") ~> (phase # "preparing")
=============================================================================
