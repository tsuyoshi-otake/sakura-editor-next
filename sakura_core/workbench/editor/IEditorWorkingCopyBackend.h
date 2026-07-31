/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorWorkingCopyTypes.h"

#include <cstdint>

namespace workbench::editor {

//! Backend completion, intentionally value-free. Implementations must not pass UI prompts or filesystem paths through it.
enum class EEditorWorkingCopyBackendStatus : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
	Unsupported,
};

struct EditorWorkingCopyBackendRequest {
	EditorDocumentIdentity identity;
	std::uint64_t version = 0;
	bool dirty = false;
	EditorWorkingCopySaveOptions saveOptions;
	bool suppressCloseConfirmation = false;
	//! Only meaningful for PrepareClose/CommitClose. The default is the normal File Close flow.
	EEditorWorkingCopyCloseDisposition closeDisposition = EEditorWorkingCopyCloseDisposition::InitializeEmptyDocument;
};

//! successfulVersion is the version which may safely be published only after the coordinator revalidates the document.
struct EditorWorkingCopyBackendResult {
	EEditorWorkingCopyBackendStatus status = EEditorWorkingCopyBackendStatus::Failed;
	std::uint64_t successfulVersion = 0;
	//! Set when the save changed identity (notably Untitled Save and Save As).
	//! The coordinator validates this value before publishing an identity replacement.
	std::optional<EditorDocumentIdentity> resultingIdentity;
};

//! Opaque, backend-owned ownership token for one staged native revert.
//!
//! A token is issued only after PrepareRevert has completely staged external
//! content without changing the live native document.  It is single-use: the
//! coordinator must finish it exactly once with FinalizePreparedRevert or
//! RollbackPreparedRevert.  Tokens intentionally carry no path, content, or
//! document identity across the boundary.
struct EditorWorkingCopyRevertTransaction {
	std::uint64_t value = 0;

	[[nodiscard]] bool IsValid() const noexcept { return value != 0; }
};

//! PrepareRevert returns both a normal terminal backend result and, on success only, its native transaction token.
struct EditorWorkingCopyBackendRevertPrepareResult {
	EditorWorkingCopyBackendResult result;
	EditorWorkingCopyRevertTransaction transaction;
};

//! ApplyPreparedRevert is a prevalidated, allocation-free native ownership transfer.
enum class EEditorWorkingCopyBackendRevertApplyStatus : std::uint8_t {
	Applied,
	NotPrepared,
	TargetChanged,
};

struct EditorWorkingCopySaveAsBackendRequest {
	EditorWorkingCopyBackendRequest source;
	//! A missing value lets the adapter obtain a target. A present value is an exact canonical target constraint.
	std::optional<EditorDocumentIdentity> targetIdentity;
};

/*!
	@brief Boundary for imperative save/revert/close work.

	PrepareClose must be non-destructive: a non-success result, a stale completion, or a core close rejection leaves
	the backend's document usable. CommitClose is invoked only after EditorCoreService::CloseInput succeeds and is
	noexcept by contract; implementations must make it infallible (best-effort cleanup belongs inside the backend).

	Revert is deliberately a transaction rather than a one-shot reload:
	- PrepareRevert stages and validates the external contents without changing live native state.
	- ApplyPreparedRevert must either make the retained-rollback native swap or make no change.  It is noexcept and
	  allocation-free after preparation; it must not run UI, plugin, macro, or legacy reload callbacks.
	- The coordinator commits the matching Core state only after ApplyPreparedRevert returns Applied.
	- FinalizePreparedRevert discards retained rollback state after that Core commit.  RollbackPreparedRevert restores
	  the exact pre-apply native state after any Core rejection (and is also the terminal discard operation before apply).
*/
class IEditorWorkingCopyBackend {
public:
	virtual ~IEditorWorkingCopyBackend() = default;
	[[nodiscard]] virtual EditorWorkingCopyBackendResult Save(const EditorWorkingCopyBackendRequest& request) = 0;
	[[nodiscard]] virtual EditorWorkingCopyBackendResult SaveAs(const EditorWorkingCopySaveAsBackendRequest& request) = 0;
	[[nodiscard]] virtual EditorWorkingCopyBackendRevertPrepareResult PrepareRevert(
		const EditorWorkingCopyBackendRequest& request) = 0;
	[[nodiscard]] virtual EEditorWorkingCopyBackendRevertApplyStatus ApplyPreparedRevert(
		EditorWorkingCopyRevertTransaction transaction) noexcept = 0;
	virtual void FinalizePreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept = 0;
	virtual void RollbackPreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept = 0;
	[[nodiscard]] virtual EditorWorkingCopyBackendResult PrepareClose(const EditorWorkingCopyBackendRequest& request) = 0;
	virtual void CommitClose(const EditorWorkingCopyBackendRequest& request) noexcept = 0;
};

} // namespace workbench::editor
