/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::editor {

//! A state projection for one canonical document. It is independent of windows and of legacy editor objects.
enum class EEditorWorkingCopyState : std::uint8_t {
	Saved,
	Dirty,
	PendingSave,
	Conflict,
	Orphaned,
	Error,
};

//! Every public coordinator request completes in one of these terminal states.
enum class EEditorWorkingCopyOperationStatus : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
	NotApplicable,
	Conflict,
	Unsupported,
};

//! A value-free diagnostic reason. Never use this type to expose a path, document text, or backend error text.
enum class EEditorWorkingCopyOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	OperationIdConflict,
	InvalidInput,
	InputNotFound,
	RevisionConflict,
	DocumentStateConflict,
	BackendCancelled,
	BackendFailed,
	BackendUnsupported,
	BackendVersionMismatch,
	BackendIdentityMismatch,
	//! A staged native transaction declined its prevalidated swap without changing live native state.
	BackendApplyFailed,
	NoWorkingCopyStateChange,
	//! Another save/revert/close is already executing for this canonical working copy.
	OperationInProgress,
	CoreRejected,
};

//! Bounded external request identity plus optional optimistic core revision.
struct EditorWorkingCopyOperationMetadata {
	std::string operationId;
	std::optional<std::uint64_t> expectedModelRevision;
};

//! A non-sensitive working-copy projection. The canonical document identity is the only document locator.
struct EditorWorkingCopySnapshot {
	EditorDocumentIdentity identity;
	std::uint64_t version = 0;
	EEditorWorkingCopyState state = EEditorWorkingCopyState::Saved;
	std::vector<std::string> inputIds;
};

struct EditorWorkingCopyCoordinatorSnapshot {
	std::uint64_t coreRevision = 0;
	std::vector<EditorWorkingCopySnapshot> workingCopies;
};

struct EditorWorkingCopyOperationResult {
	EEditorWorkingCopyOperationStatus status = EEditorWorkingCopyOperationStatus::Failed;
	EEditorWorkingCopyOperationReason reason = EEditorWorkingCopyOperationReason::None;
	std::uint64_t coreRevision = 0;
	bool replayed = false;
	std::optional<EditorWorkingCopySnapshot> workingCopy;
};

//! Selects whether an ordinary Save may acquire a target for an untitled working copy.
//! Save As always remains an explicit target-acquisition operation.
enum class EEditorWorkingCopySaveTargetPolicy : std::uint8_t {
	AcquireIfMissing,
	ExistingOnly,
};

//! A portable line-ending request. Preserve lets the native adapter retain the document's current representation.
enum class EEditorWorkingCopyLineEnding : std::uint8_t {
	Preserve,
	CrLf,
	Lf,
	Cr,
};

//! Typed, bounded save intent. These values cross the workbench/backend boundary without legacy LPARAMs.
struct EditorWorkingCopySaveOptions {
	EEditorWorkingCopySaveTargetPolicy targetPolicy = EEditorWorkingCopySaveTargetPolicy::AcquireIfMissing;
	//! Suppresses only non-essential native feedback (for example Save All's warning beep).
	bool suppressFeedback = false;
	//! Requests a physical write even when the accepted content version is already clean.
	//! This preserves the legacy unmodified-overwrite setting without weakening normal Save no-op semantics.
	bool forceWrite = false;
	//! UTF-8 dialog seed when Save As must acquire a target. Empty means no seed; the coordinator bounds it at 4096 bytes.
	std::string suggestedTarget;
	//! An absent encoding preserves native/default selection. A present value must be non-empty canonical UTF-8 (<=64 bytes).
	std::optional<std::string> encodingId;
	EEditorWorkingCopyLineEnding lineEnding = EEditorWorkingCopyLineEnding::Preserve;
};

struct SaveWorkingCopyRequest {
	EditorWorkingCopyOperationMetadata operation;
	std::string inputId;
	EditorWorkingCopySaveOptions options;
};

//! A missing target asks the platform adapter to acquire one (for example through the native Save As dialog).
//! When supplied, the backend must either save to the same canonical identity or return an identity mismatch.
struct SaveWorkingCopyAsRequest {
	EditorWorkingCopyOperationMetadata operation;
	std::string inputId;
	std::optional<EditorDocumentIdentity> targetIdentity;
	EditorWorkingCopySaveOptions options;
};

struct RevertWorkingCopyRequest {
	EditorWorkingCopyOperationMetadata operation;
	std::string inputId;
};

//! Specifies what the native document adapter does after the core has accepted an input close.
//! A workbench File Close keeps the legacy editor usable by creating a fresh untitled document;
//! process/window teardown must only deliver the document-close notification and release the old document.
enum class EEditorWorkingCopyCloseDisposition : std::uint8_t {
	InitializeEmptyDocument,
	DisposeWindow,
};

struct CloseWorkingCopyRequest {
	EditorWorkingCopyOperationMetadata operation;
	std::string inputId;
	//! Legacy grep/output shutdown may suppress only the native confirmation UI.
	//! Core validation, durable cleanup, and terminal completion remain mandatory.
	bool suppressConfirmation = false;
	//! The default preserves ordinary File Close behaviour. Window teardown opts into DisposeWindow explicitly.
	EEditorWorkingCopyCloseDisposition disposition = EEditorWorkingCopyCloseDisposition::InitializeEmptyDocument;
};

//! External edit notification. version is the authoritative working-copy version after the edit.
struct SetWorkingCopyDirtyRequest {
	EditorWorkingCopyOperationMetadata operation;
	std::string inputId;
	std::uint64_t version = 0;
};

} // namespace workbench::editor
