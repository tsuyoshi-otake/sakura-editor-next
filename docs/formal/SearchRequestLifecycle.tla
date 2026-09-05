----------------------- MODULE SearchRequestLifecycle -----------------------
EXTENDS Naturals
CONSTANTS InvalidateEmpty, CheckGeneration
VARIABLES revision, generation, empty, pending, running, mailbox, visible, closed,
          unsafeReplace
vars == <<revision, generation, empty, pending, running, mailbox, visible,
          closed, unsafeReplace>>
None == [revision |-> 0, generation |-> 0]
Init == /\ revision = 1 /\ generation = 1 /\ empty = FALSE
        /\ pending = None /\ running = None /\ mailbox = None
        /\ visible = 0 /\ closed = FALSE /\ unsafeReplace = FALSE
Edit(isEmpty) ==
    /\ ~closed /\ revision < 3
    /\ revision' = revision + 1
    /\ generation' = IF ~isEmpty \/ InvalidateEmpty THEN generation + 1 ELSE generation
    /\ empty' = isEmpty /\ pending' = None /\ visible' = 0
    /\ UNCHANGED <<running, mailbox, closed, unsafeReplace>>
Debounce ==
    /\ ~closed /\ ~empty /\ pending = None
    /\ pending' = [revision |-> revision, generation |-> generation]
    /\ UNCHANGED <<revision, generation, empty, running, mailbox, visible, closed, unsafeReplace>>
Start ==
    /\ ~closed /\ pending # None /\ running = None
    /\ running' = pending /\ pending' = None
    /\ UNCHANGED <<revision, generation, empty, mailbox, visible, closed, unsafeReplace>>
Finish ==
    /\ running # None
    /\ mailbox' = IF ~closed /\ running.generation = generation THEN running ELSE mailbox
    /\ running' = None
    /\ UNCHANGED <<revision, generation, empty, pending, visible, closed, unsafeReplace>>
Apply ==
    /\ ~closed /\ mailbox # None
    /\ visible' = IF ~CheckGeneration \/ mailbox.generation = generation
                   THEN mailbox.revision ELSE visible
    /\ mailbox' = None
    /\ UNCHANGED <<revision, generation, empty, pending, running, closed, unsafeReplace>>
Replace ==
    /\ ~closed /\ ~empty /\ visible # 0
    /\ unsafeReplace' = unsafeReplace \/ visible # revision
    /\ UNCHANGED <<revision, generation, empty, pending, running, mailbox, visible, closed>>
Close ==
    /\ ~closed /\ closed' = TRUE /\ visible' = 0 /\ pending' = None /\ mailbox' = None
    /\ UNCHANGED <<revision, generation, empty, running, unsafeReplace>>
Next == Edit(TRUE) \/ Edit(FALSE) \/ Debounce \/ Start \/ Finish \/ Apply \/ Replace \/ Close
Spec == Init /\ [][Next]_vars
CurrentResults == visible = 0 \/ (~closed /\ ~empty /\ visible = revision)
NoStaleReplace == ~unsafeReplace
=============================================================================