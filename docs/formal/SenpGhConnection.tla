------------------------- MODULE SenpGhConnection -------------------------
EXTENDS Naturals
CONSTANTS CheckIdentity, CheckGeneration, MaxEpoch
VARIABLES epoch, phase, opEpoch, candidate, verified, connected, grantEpoch,
          grantVerified, requested, terminals, previous, disconnected
vars == <<epoch, phase, opEpoch, candidate, verified, connected, grantEpoch,
          grantVerified, requested, terminals, previous, disconnected>>
None == "none"
Accounts == {"A", "B"}
Init ==
    /\ epoch = 1 /\ phase = "idle" /\ opEpoch = 0
    /\ candidate = None /\ verified = FALSE
    /\ connected \in Accounts \cup {None}
    /\ grantEpoch = IF connected = None THEN 0 ELSE 1
    /\ grantVerified = (connected # None)
    /\ requested = FALSE /\ terminals = 0
    /\ previous = connected /\ disconnected = FALSE

Begin(account) ==
    /\ ~requested /\ phase = "idle"
    /\ candidate' = account /\ opEpoch' = epoch
    /\ previous' = connected /\ requested' = TRUE /\ phase' = "checking"
    /\ UNCHANGED <<epoch, verified, connected, grantEpoch, grantVerified,
                    terminals, disconnected>>
Receive ==
    /\ phase = "checking" /\ phase' = "verifying"
    /\ UNCHANGED <<epoch, opEpoch, candidate, verified, connected, grantEpoch,
                    grantVerified, requested, terminals, previous, disconnected>>
VerifyIdentity ==
    /\ phase = "verifying" /\ ~verified /\ verified' = TRUE
    /\ UNCHANGED <<epoch, phase, opEpoch, candidate, connected, grantEpoch,
                    grantVerified, requested, terminals, previous, disconnected>>
Publish ==
    /\ phase = "verifying" /\ (verified \/ ~CheckIdentity)
    /\ (opEpoch = epoch \/ ~CheckGeneration)
    /\ connected' = candidate /\ grantEpoch' = opEpoch
    /\ grantVerified' = verified /\ phase' = "idle"
    /\ terminals' = terminals + 1
    /\ UNCHANGED <<epoch, opEpoch, candidate, verified, requested, previous, disconnected>>
Abort ==
    /\ phase # "idle" /\ phase' = "idle" /\ terminals' = terminals + 1
    /\ UNCHANGED <<epoch, opEpoch, candidate, verified, connected, grantEpoch,
                    grantVerified, requested, previous, disconnected>>
Disconnect ==
    /\ epoch < MaxEpoch /\ epoch' = epoch + 1
    /\ connected' = None /\ grantEpoch' = 0 /\ grantVerified' = FALSE
    /\ disconnected' = TRUE
    /\ UNCHANGED <<phase, opEpoch, candidate, verified, requested, terminals, previous>>
Internal == Receive \/ VerifyIdentity \/ Publish \/ Abort
Next == (\E account \in Accounts : Begin(account)) \/ Internal \/ Disconnect
Spec == Init /\ [][Next]_vars /\ WF_vars(Internal)
TypeOK ==
    /\ epoch \in 1..MaxEpoch /\ opEpoch \in 0..MaxEpoch
    /\ phase \in {"idle", "checking", "verifying"}
    /\ candidate \in Accounts \cup {None} /\ connected \in Accounts \cup {None}
    /\ previous \in Accounts \cup {None} /\ grantEpoch \in 0..MaxEpoch
    /\ verified \in BOOLEAN /\ requested \in BOOLEAN
    /\ grantVerified \in BOOLEAN /\ disconnected \in BOOLEAN
    /\ terminals \in 0..1
IdentityBeforeGrant == connected = None \/ grantVerified
NoStaleConnection == connected = None \/ grantEpoch = epoch
CandidatePreservesConnection ==
    (phase # "idle" /\ ~disconnected) => connected = previous
OperationTerminates == requested ~> (terminals = 1)
=============================================================================
