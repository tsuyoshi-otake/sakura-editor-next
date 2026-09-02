------------------------------ MODULE ProjectSwitchState ------------------------------
EXTENDS Naturals

(***************************************************************************)
(* Model of an in-process Project switch with separate logical and native   *)
(* projections. Editor and Terminal snapshots are structured so TLC can    *)
(* explore failures between individual native projection steps instead of  *)
(* treating each whole snapshot as one opaque value.                        *)
(***************************************************************************)

CONSTANTS Projects, Layouts, NoState

EditorStates ==
    { [tabOrder |-> "editorTabsA",
       activeTab |-> "editorActiveA",
       groupLayout |-> "editorGroupsA"],
      [tabOrder |-> "editorTabsB",
       activeTab |-> "editorActiveB",
       groupLayout |-> "editorGroupsB"] }

TerminalStates ==
    { [tabOrder |-> "terminalTabsA",
       selectedTab |-> "terminalSelectedA",
       paneLayout |-> "terminalPanesA",
       focusedPane |-> "terminalFocusA",
       defaultCwd |-> "terminalCwdA"],
      [tabOrder |-> "terminalTabsB",
       selectedTab |-> "terminalSelectedB",
       paneLayout |-> "terminalPanesB",
       focusedPane |-> "terminalFocusB",
       defaultCwd |-> "terminalCwdB"] }

VARIABLES current, phase, pending, dirty, catalogReady, globalLayout,
          savedEditor, savedTerminal, savedDirty,
          activeEditor, activeTerminal,
          nativeEditor, nativeTerminal, workspaceProjection,
          captured, staged, lastOutcome, processGeneration,
          explicitNewWindowObserved

vars == <<current, phase, pending, dirty, catalogReady, globalLayout,
          savedEditor, savedTerminal, savedDirty,
          activeEditor, activeTerminal,
          nativeEditor, nativeTerminal, workspaceProjection,
          captured, staged, lastOutcome, processGeneration,
          explicitNewWindowObserved>>

ProjectState ==
    [workspace : Projects,
     editor : EditorStates,
     terminal : TerminalStates,
     dirty : BOOLEAN]

Init ==
    /\ current \in Projects
    /\ phase = "idle"
    /\ pending = NoState
    /\ catalogReady = FALSE
    /\ globalLayout \in Layouts
    /\ savedEditor \in [Projects -> EditorStates]
    /\ savedTerminal \in [Projects -> TerminalStates]
    /\ savedDirty \in [Projects -> BOOLEAN]
    /\ activeEditor = savedEditor[current]
    /\ activeTerminal = savedTerminal[current]
    /\ dirty = savedDirty[current]
    /\ nativeEditor = activeEditor
    /\ nativeTerminal = activeTerminal
    /\ workspaceProjection = current
    /\ captured = NoState
    /\ staged = NoState
    /\ lastOutcome = NoState
    /\ processGeneration = 0
    /\ explicitNewWindowObserved = FALSE

LoadCatalog ==
    /\ phase = "idle"
    /\ catalogReady = FALSE
    /\ catalogReady' = TRUE
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, phase, pending, dirty, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, processGeneration,
                    explicitNewWindowObserved>>

RejectCatalogUnavailable(target) ==
    /\ phase = "idle"
    /\ catalogReady = FALSE
    /\ target \in Projects
    /\ target # current
    /\ lastOutcome' = "catalogUnavailable"
    /\ UNCHANGED <<current, phase, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, processGeneration,
                    explicitNewWindowObserved>>

BeginSwitch(target) ==
    /\ phase = "idle"
    /\ catalogReady = TRUE
    /\ dirty = FALSE
    /\ target \in Projects
    /\ target # current
    /\ phase' = "preparing"
    /\ pending' = target
    /\ captured' = [workspace |-> current,
                    editor |-> activeEditor,
                    terminal |-> activeTerminal,
                    dirty |-> dirty]
    /\ staged' = NoState
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, processGeneration,
                    explicitNewWindowObserved>>

RejectDirtySwitch(target) ==
    /\ phase = "idle"
    /\ catalogReady = TRUE
    /\ dirty = TRUE
    /\ target \in Projects
    /\ target # current
    /\ lastOutcome' = "dirtyRejected"
    /\ UNCHANGED <<current, phase, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, processGeneration,
                    explicitNewWindowObserved>>

OpenInNewWindow(target) ==
    /\ phase = "idle"
    /\ catalogReady = TRUE
    /\ explicitNewWindowObserved = FALSE
    /\ target \in Projects
    /\ target # current
    /\ explicitNewWindowObserved' = TRUE
    /\ lastOutcome' = "newWindow"
    /\ UNCHANGED <<current, phase, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, processGeneration>>

FailBeforePrepare ==
    /\ phase = "preparing"
    /\ phase' = "idle"
    /\ pending' = NoState
    /\ captured' = NoState
    /\ staged' = NoState
    /\ lastOutcome' = "aborted"
    /\ UNCHANGED <<current, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, processGeneration,
                    explicitNewWindowObserved>>

PrepareTarget ==
    /\ phase = "preparing"
    /\ pending \in Projects
    /\ captured \in ProjectState
    /\ phase' = "committing"
    /\ staged' = [workspace |-> pending,
                  editor |-> savedEditor[pending],
                  terminal |-> savedTerminal[pending],
                  dirty |-> savedDirty[pending]]
    /\ savedEditor' = [savedEditor EXCEPT ![captured.workspace] = captured.editor]
    /\ savedTerminal' = [savedTerminal EXCEPT ![captured.workspace] = captured.terminal]
    /\ savedDirty' = [savedDirty EXCEPT ![captured.workspace] = captured.dirty]
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, lastOutcome, processGeneration,
                    explicitNewWindowObserved>>

CommitWorkspace ==
    /\ phase = "committing"
    /\ staged \in ProjectState
    /\ current' = staged.workspace
    /\ phase' = "projectingEditorTabs"
    /\ UNCHANGED <<pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectEditorTabs ==
    /\ phase = "projectingEditorTabs"
    /\ staged \in ProjectState
    /\ nativeEditor' = [nativeEditor EXCEPT !.tabOrder = staged.editor.tabOrder]
    /\ phase' = "projectingEditorSelection"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeTerminal,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectEditorSelection ==
    /\ phase = "projectingEditorSelection"
    /\ staged \in ProjectState
    /\ nativeEditor' = [nativeEditor EXCEPT !.activeTab = staged.editor.activeTab]
    /\ phase' = "projectingEditorGroups"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeTerminal,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectEditorGroups ==
    /\ phase = "projectingEditorGroups"
    /\ staged \in ProjectState
    /\ nativeEditor' = [nativeEditor EXCEPT !.groupLayout = staged.editor.groupLayout]
    /\ phase' = "projectingTerminalTabs"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeTerminal,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectTerminalTabs ==
    /\ phase = "projectingTerminalTabs"
    /\ staged \in ProjectState
    /\ nativeTerminal' = [nativeTerminal EXCEPT !.tabOrder = staged.terminal.tabOrder]
    /\ phase' = "projectingTerminalSelection"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectTerminalSelection ==
    /\ phase = "projectingTerminalSelection"
    /\ staged \in ProjectState
    /\ nativeTerminal' = [nativeTerminal EXCEPT !.selectedTab = staged.terminal.selectedTab]
    /\ phase' = "projectingTerminalPanes"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectTerminalPanes ==
    /\ phase = "projectingTerminalPanes"
    /\ staged \in ProjectState
    /\ nativeTerminal' = [nativeTerminal EXCEPT !.paneLayout = staged.terminal.paneLayout]
    /\ phase' = "projectingTerminalFocus"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectTerminalFocus ==
    /\ phase = "projectingTerminalFocus"
    /\ staged \in ProjectState
    /\ nativeTerminal' = [nativeTerminal EXCEPT !.focusedPane = staged.terminal.focusedPane]
    /\ phase' = "projectingTerminalCwd"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectTerminalCwd ==
    /\ phase = "projectingTerminalCwd"
    /\ staged \in ProjectState
    /\ nativeTerminal' = [nativeTerminal EXCEPT !.defaultCwd = staged.terminal.defaultCwd]
    /\ phase' = "projectingWorkspace"
    /\ UNCHANGED <<current, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor,
                    workspaceProjection, captured, staged, lastOutcome,
                    processGeneration, explicitNewWindowObserved>>

ProjectWorkspace ==
    /\ phase = "projectingWorkspace"
    /\ staged \in ProjectState
    /\ activeEditor' = staged.editor
    /\ activeTerminal' = staged.terminal
    /\ dirty' = staged.dirty
    /\ workspaceProjection' = staged.workspace
    /\ phase' = "idle"
    /\ pending' = NoState
    /\ captured' = NoState
    /\ staged' = NoState
    /\ lastOutcome' = "succeeded"
    /\ UNCHANGED <<current, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    nativeEditor, nativeTerminal, processGeneration,
                    explicitNewWindowObserved>>

RollbackAfterPrepare ==
    /\ phase \in {"committing", "projectingEditorTabs",
                   "projectingEditorSelection", "projectingEditorGroups",
                   "projectingTerminalTabs", "projectingTerminalSelection",
                   "projectingTerminalPanes", "projectingTerminalFocus",
                   "projectingTerminalCwd", "projectingWorkspace"}
    /\ captured \in ProjectState
    /\ current' = captured.workspace
    /\ activeEditor' = captured.editor
    /\ activeTerminal' = captured.terminal
    /\ nativeEditor' = captured.editor
    /\ nativeTerminal' = captured.terminal
    /\ workspaceProjection' = captured.workspace
    /\ dirty' = captured.dirty
    /\ phase' = "idle"
    /\ pending' = NoState
    /\ captured' = NoState
    /\ staged' = NoState
    /\ lastOutcome' = "aborted"
    /\ UNCHANGED <<catalogReady, globalLayout, savedEditor, savedTerminal,
                    savedDirty, processGeneration, explicitNewWindowObserved>>

ChangeEditor ==
    /\ phase = "idle"
    /\ activeEditor' \in EditorStates
    /\ nativeEditor' = activeEditor'
    /\ dirty' = TRUE
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, phase, pending, catalogReady, globalLayout,
                    savedEditor, savedTerminal, savedDirty,
                    activeTerminal, nativeTerminal, workspaceProjection,
                    captured, staged, processGeneration,
                    explicitNewWindowObserved>>

SaveDocument ==
    /\ phase = "idle"
    /\ savedEditor' = [savedEditor EXCEPT ![current] = activeEditor]
    /\ savedDirty' = [savedDirty EXCEPT ![current] = FALSE]
    /\ dirty' = FALSE
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, phase, pending, catalogReady, globalLayout,
                    savedTerminal, activeEditor, activeTerminal,
                    nativeEditor, nativeTerminal, workspaceProjection,
                    captured, staged, processGeneration,
                    explicitNewWindowObserved>>

ChangeTerminal ==
    /\ phase = "idle"
    /\ activeTerminal' \in TerminalStates
    /\ nativeTerminal' = activeTerminal'
    /\ savedTerminal' = [savedTerminal EXCEPT ![current] = activeTerminal']
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, phase, pending, dirty, catalogReady, globalLayout,
                    savedEditor, savedDirty, activeEditor, nativeEditor,
                    workspaceProjection, captured, staged, processGeneration,
                    explicitNewWindowObserved>>

ChangeGlobalLayout ==
    /\ phase = "idle"
    /\ globalLayout' \in Layouts
    /\ lastOutcome' = NoState
    /\ UNCHANGED <<current, phase, pending, dirty, catalogReady,
                    savedEditor, savedTerminal, savedDirty,
                    activeEditor, activeTerminal, nativeEditor, nativeTerminal,
                    workspaceProjection, captured, staged, processGeneration,
                    explicitNewWindowObserved>>

Next ==
    \/ LoadCatalog
    \/ \E target \in Projects : RejectCatalogUnavailable(target)
    \/ \E target \in Projects : BeginSwitch(target)
    \/ \E target \in Projects : RejectDirtySwitch(target)
    \/ \E target \in Projects : OpenInNewWindow(target)
    \/ FailBeforePrepare
    \/ PrepareTarget
    \/ CommitWorkspace
    \/ ProjectEditorTabs
    \/ ProjectEditorSelection
    \/ ProjectEditorGroups
    \/ ProjectTerminalTabs
    \/ ProjectTerminalSelection
    \/ ProjectTerminalPanes
    \/ ProjectTerminalFocus
    \/ ProjectTerminalCwd
    \/ ProjectWorkspace
    \/ RollbackAfterPrepare
    \/ ChangeEditor
    \/ SaveDocument
    \/ ChangeTerminal
    \/ ChangeGlobalLayout

AdvanceSwitch ==
    \/ FailBeforePrepare
    \/ PrepareTarget
    \/ CommitWorkspace
    \/ ProjectEditorTabs
    \/ ProjectEditorSelection
    \/ ProjectEditorGroups
    \/ ProjectTerminalTabs
    \/ ProjectTerminalSelection
    \/ ProjectTerminalPanes
    \/ ProjectTerminalFocus
    \/ ProjectTerminalCwd
    \/ ProjectWorkspace
    \/ RollbackAfterPrepare

(* Once BeginSwitch has accepted a request, the transaction owns an explicit *)
(* success or rollback terminal. User actions before BeginSwitch stay unfair. *)
Spec == Init /\ [][Next]_vars /\ WF_vars(AdvanceSwitch)

TypeOK ==
    /\ current \in Projects
    /\ phase \in {"idle", "preparing", "committing",
                    "projectingEditorTabs", "projectingEditorSelection",
                    "projectingEditorGroups", "projectingTerminalTabs",
                    "projectingTerminalSelection", "projectingTerminalPanes",
                    "projectingTerminalFocus", "projectingTerminalCwd",
                    "projectingWorkspace"}
    /\ pending \in Projects \cup {NoState}
    /\ dirty \in BOOLEAN
    /\ catalogReady \in BOOLEAN
    /\ globalLayout \in Layouts
    /\ savedEditor \in [Projects -> EditorStates]
    /\ savedTerminal \in [Projects -> TerminalStates]
    /\ savedDirty \in [Projects -> BOOLEAN]
    /\ activeEditor \in EditorStates
    /\ activeTerminal \in TerminalStates
    /\ nativeEditor \in [tabOrder : {"editorTabsA", "editorTabsB"},
                          activeTab : {"editorActiveA", "editorActiveB"},
                          groupLayout : {"editorGroupsA", "editorGroupsB"}]
    /\ nativeTerminal \in [tabOrder : {"terminalTabsA", "terminalTabsB"},
                            selectedTab : {"terminalSelectedA", "terminalSelectedB"},
                            paneLayout : {"terminalPanesA", "terminalPanesB"},
                            focusedPane : {"terminalFocusA", "terminalFocusB"},
                            defaultCwd : {"terminalCwdA", "terminalCwdB"}]
    /\ workspaceProjection \in Projects
    /\ captured = NoState \/ captured \in ProjectState
    /\ staged = NoState \/ staged \in ProjectState
    /\ lastOutcome \in {NoState, "catalogUnavailable", "dirtyRejected",
                         "aborted", "succeeded", "newWindow"}
    /\ processGeneration = 0
    /\ explicitNewWindowObserved \in BOOLEAN

NoProcessRestart == processGeneration = 0

TransactionPhaseInvariant ==
    /\ (phase = "idle") =>
        /\ pending = NoState
        /\ captured = NoState
        /\ staged = NoState
        /\ workspaceProjection = current
        /\ nativeEditor = activeEditor
        /\ nativeTerminal = activeTerminal
    /\ (phase = "preparing") =>
        /\ pending \in Projects
        /\ captured \in ProjectState
        /\ staged = NoState
        /\ current = captured.workspace
        /\ workspaceProjection = captured.workspace
        /\ activeEditor = captured.editor
        /\ activeTerminal = captured.terminal
        /\ nativeEditor = captured.editor
        /\ nativeTerminal = captured.terminal
    /\ (phase = "committing") =>
        /\ pending \in Projects
        /\ captured \in ProjectState
        /\ staged \in ProjectState
        /\ staged.workspace = pending
        /\ current = captured.workspace
        /\ workspaceProjection = captured.workspace
        /\ nativeEditor = captured.editor
        /\ nativeTerminal = captured.terminal
    /\ (phase = "projectingEditorTabs") =>
        /\ current = staged.workspace
        /\ workspaceProjection = captured.workspace
        /\ nativeEditor = captured.editor
        /\ nativeTerminal = captured.terminal
    /\ (phase = "projectingEditorSelection") =>
        /\ nativeEditor.tabOrder = staged.editor.tabOrder
        /\ nativeEditor.activeTab = captured.editor.activeTab
        /\ nativeEditor.groupLayout = captured.editor.groupLayout
        /\ nativeTerminal = captured.terminal
    /\ (phase = "projectingEditorGroups") =>
        /\ nativeEditor.tabOrder = staged.editor.tabOrder
        /\ nativeEditor.activeTab = staged.editor.activeTab
        /\ nativeEditor.groupLayout = captured.editor.groupLayout
        /\ nativeTerminal = captured.terminal
    /\ (phase = "projectingTerminalTabs") =>
        /\ nativeEditor = staged.editor
        /\ nativeTerminal = captured.terminal
    /\ (phase = "projectingTerminalSelection") =>
        /\ nativeEditor = staged.editor
        /\ nativeTerminal.tabOrder = staged.terminal.tabOrder
        /\ nativeTerminal.selectedTab = captured.terminal.selectedTab
        /\ nativeTerminal.paneLayout = captured.terminal.paneLayout
        /\ nativeTerminal.focusedPane = captured.terminal.focusedPane
        /\ nativeTerminal.defaultCwd = captured.terminal.defaultCwd
    /\ (phase = "projectingTerminalPanes") =>
        /\ nativeTerminal.tabOrder = staged.terminal.tabOrder
        /\ nativeTerminal.selectedTab = staged.terminal.selectedTab
        /\ nativeTerminal.paneLayout = captured.terminal.paneLayout
        /\ nativeTerminal.focusedPane = captured.terminal.focusedPane
        /\ nativeTerminal.defaultCwd = captured.terminal.defaultCwd
    /\ (phase = "projectingTerminalFocus") =>
        /\ nativeTerminal.tabOrder = staged.terminal.tabOrder
        /\ nativeTerminal.selectedTab = staged.terminal.selectedTab
        /\ nativeTerminal.paneLayout = staged.terminal.paneLayout
        /\ nativeTerminal.focusedPane = captured.terminal.focusedPane
        /\ nativeTerminal.defaultCwd = captured.terminal.defaultCwd
    /\ (phase = "projectingTerminalCwd") =>
        /\ nativeTerminal.tabOrder = staged.terminal.tabOrder
        /\ nativeTerminal.selectedTab = staged.terminal.selectedTab
        /\ nativeTerminal.paneLayout = staged.terminal.paneLayout
        /\ nativeTerminal.focusedPane = staged.terminal.focusedPane
        /\ nativeTerminal.defaultCwd = captured.terminal.defaultCwd
    /\ (phase = "projectingWorkspace") =>
        /\ nativeEditor = staged.editor
        /\ nativeTerminal = staged.terminal

NoPartialNativeProjectionWhenIdle ==
    (phase = "idle") =>
        /\ workspaceProjection = current
        /\ nativeEditor = activeEditor
        /\ nativeTerminal = activeTerminal

NoLossOnAbortAction ==
    (phase # "idle" /\ phase' = "idle" /\ lastOutcome' = "aborted") =>
        /\ current' = captured.workspace
        /\ activeEditor' = captured.editor
        /\ activeTerminal' = captured.terminal
        /\ nativeEditor' = captured.editor
        /\ nativeTerminal' = captured.terminal
        /\ workspaceProjection' = captured.workspace
        /\ dirty' = captured.dirty
NoLossOnAbort == []([NoLossOnAbortAction]_vars)

DirtySwitchGuardAction == dirty => current' = current
DirtySwitchGuard == []([DirtySwitchGuardAction]_vars)

CatalogReadinessGuardAction == ~catalogReady => current' = current
CatalogReadinessGuard == []([CatalogReadinessGuardAction]_vars)

NonReentrantSwitchAction ==
    (phase # "idle" /\ phase' # "idle") =>
        /\ pending' = pending
        /\ captured' = captured
NonReentrantSwitch == []([NonReentrantSwitchAction]_vars)

GlobalLayoutStableDuringSwitchAction ==
    phase # "idle" => globalLayout' = globalLayout
GlobalLayoutStableDuringSwitch == []([GlobalLayoutStableDuringSwitchAction]_vars)

OrdinarySwitchDoesNotOpenWindowAction ==
    phase # "idle" => explicitNewWindowObserved' = explicitNewWindowObserved
OrdinarySwitchDoesNotOpenWindow == []([OrdinarySwitchDoesNotOpenWindowAction]_vars)

ExplicitWindowIsolationAction ==
    (~explicitNewWindowObserved /\ explicitNewWindowObserved') =>
        /\ current' = current
        /\ activeEditor' = activeEditor
        /\ activeTerminal' = activeTerminal
        /\ nativeEditor' = nativeEditor
        /\ nativeTerminal' = nativeTerminal
        /\ workspaceProjection' = workspaceProjection
        /\ globalLayout' = globalLayout
ExplicitWindowIsolation == []([ExplicitWindowIsolationAction]_vars)

EditorRestorationAction ==
    (phase = "projectingWorkspace" /\ phase' = "idle"
     /\ lastOutcome' = "succeeded") =>
        /\ activeEditor' = savedEditor'[current']
        /\ nativeEditor' = savedEditor'[current']
        /\ activeEditor'.tabOrder = savedEditor'[current'].tabOrder
        /\ activeEditor'.activeTab = savedEditor'[current'].activeTab
        /\ activeEditor'.groupLayout = savedEditor'[current'].groupLayout
        /\ dirty' = savedDirty'[current']
EditorRestoration == []([EditorRestorationAction]_vars)

TerminalRestorationAction ==
    (phase = "projectingWorkspace" /\ phase' = "idle"
     /\ lastOutcome' = "succeeded") =>
        /\ activeTerminal' = savedTerminal'[current']
        /\ nativeTerminal' = savedTerminal'[current']
        /\ activeTerminal'.tabOrder = savedTerminal'[current'].tabOrder
        /\ activeTerminal'.selectedTab = savedTerminal'[current'].selectedTab
        /\ activeTerminal'.paneLayout = savedTerminal'[current'].paneLayout
        /\ activeTerminal'.focusedPane = savedTerminal'[current'].focusedPane
        /\ activeTerminal'.defaultCwd = savedTerminal'[current'].defaultCwd
TerminalRestoration == []([TerminalRestorationAction]_vars)

WorkspaceProjectionAction ==
    (phase = "projectingWorkspace" /\ phase' = "idle"
     /\ lastOutcome' = "succeeded") => workspaceProjection' = current'
WorkspaceProjectionAfterSuccess == []([WorkspaceProjectionAction]_vars)

TransactionTerminates == (phase # "idle") ~> (phase = "idle")

=============================================================================
