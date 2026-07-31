/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/IEditorWorkingCopyBackend.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace workbench::editor {

//! Coordinates Working Copy effects around the pure EditorCoreService without acquiring UI or platform dependencies.
class EditorWorkingCopyCoordinator final {
public:
	explicit EditorWorkingCopyCoordinator(EditorCoreService& core, IEditorWorkingCopyBackend& backend,
		std::size_t maxCompletedOperations = 4096);

	[[nodiscard]] EditorWorkingCopyCoordinatorSnapshot Snapshot() const;
	[[nodiscard]] EditorWorkingCopyOperationResult SetDirty(const SetWorkingCopyDirtyRequest& request);
	[[nodiscard]] EditorWorkingCopyOperationResult Save(const SaveWorkingCopyRequest& request);
	[[nodiscard]] EditorWorkingCopyOperationResult SaveAs(const SaveWorkingCopyAsRequest& request);
	[[nodiscard]] EditorWorkingCopyOperationResult Revert(const RevertWorkingCopyRequest& request);
	[[nodiscard]] EditorWorkingCopyOperationResult Close(const CloseWorkingCopyRequest& request);

private:
	struct LocatedInput;
	struct CompletedOperation {
		std::string fingerprint;
		EditorWorkingCopyOperationResult result;
	};
	//! Owns one effectful operation reservation and releases it on every normal return and stack unwind.
	class InFlightEffect final {
	public:
		InFlightEffect(EditorWorkingCopyCoordinator& owner, std::wstring documentKey, std::string operationId) noexcept;
		InFlightEffect(const InFlightEffect&) = delete;
		InFlightEffect& operator=(const InFlightEffect&) = delete;
		InFlightEffect(InFlightEffect&& other) noexcept;
		InFlightEffect& operator=(InFlightEffect&& other) noexcept;
		~InFlightEffect();

	private:
		EditorWorkingCopyCoordinator* m_owner = nullptr;
		std::wstring m_documentKey;
		std::string m_operationId;
	};
	//! Ensures a successfully staged revert has exactly one native terminal owner, including stack unwinding.
	class PreparedRevert final {
	public:
		PreparedRevert(IEditorWorkingCopyBackend& backend, EditorWorkingCopyRevertTransaction transaction) noexcept;
		PreparedRevert(const PreparedRevert&) = delete;
		PreparedRevert& operator=(const PreparedRevert&) = delete;
		~PreparedRevert();

		void Finalize() noexcept;
		void Rollback() noexcept;

	private:
		IEditorWorkingCopyBackend* m_backend = nullptr;
		EditorWorkingCopyRevertTransaction m_transaction;
	};

	[[nodiscard]] EditorWorkingCopyOperationResult SaveOrRevert(const EditorWorkingCopyOperationMetadata& operation,
		const std::string& inputId, const EditorWorkingCopySaveOptions& saveOptions);
	[[nodiscard]] EditorWorkingCopyOperationResult RevertTransactional(const EditorWorkingCopyOperationMetadata& operation,
		const std::string& inputId);
	[[nodiscard]] EditorWorkingCopyOperationResult CommitSuccessfulSave(const LocatedInput& located,
		const std::string& inputId, const EditorWorkingCopyBackendResult& backendResult,
		const std::optional<EditorDocumentIdentity>& requiredIdentity, bool allowIdentityReplacement);
	[[nodiscard]] std::optional<LocatedInput> Locate(const EditorCoreSnapshot& snapshot, const std::string& inputId) const;
	[[nodiscard]] EditorWorkingCopyOperationResult ValidateAndFind(const EditorWorkingCopyOperationMetadata& operation,
		const std::string& inputId, const std::string& fingerprint, std::optional<LocatedInput>& located, bool& handled);
	[[nodiscard]] EditorWorkingCopyOperationResult CheckCompletedOrConflict(const EditorWorkingCopyOperationMetadata& operation,
		const std::string& fingerprint, bool& handled) const;
	[[nodiscard]] std::optional<InFlightEffect> BeginEffect(const LocatedInput& located,
		const EditorWorkingCopyOperationMetadata& operation);
	[[nodiscard]] EditorWorkingCopyOperationResult InFlightConflict(const EditorWorkingCopyOperationMetadata& operation,
		const std::string& fingerprint, const LocatedInput& located);
	void EndEffect(const std::wstring& documentKey, const std::string& operationId) noexcept;
	void RememberCompleted(const std::string& operationId, std::string fingerprint,
		const EditorWorkingCopyOperationResult& result);
	[[nodiscard]] std::string NextCoreOperationId();
	void SetTransientState(const std::wstring& documentKey, EEditorWorkingCopyState state);
	[[nodiscard]] EditorWorkingCopyOperationResult BackendTerminal(const EditorWorkingCopyBackendResult& result,
		std::uint64_t coreRevision) const;
	[[nodiscard]] EditorWorkingCopySnapshot ToWorkingCopySnapshot(const EditorDocumentSnapshot& document,
		const EditorCoreSnapshot& coreSnapshot) const;
	[[nodiscard]] static bool SameDocumentState(const LocatedInput& before, const LocatedInput& after) noexcept;
	[[nodiscard]] static bool SameIdentity(const EditorDocumentIdentity& left, const EditorDocumentIdentity& right) noexcept;
	[[nodiscard]] static bool IsValidOperation(const EditorWorkingCopyOperationMetadata& operation) noexcept;
	[[nodiscard]] static bool IsValidInputId(const std::string& inputId) noexcept;
	[[nodiscard]] static bool IsValidSaveOptions(const EditorWorkingCopySaveOptions& options) noexcept;
	[[nodiscard]] static void AppendSaveOptionsFingerprint(std::string& fingerprint, const EditorWorkingCopySaveOptions& options);
	[[nodiscard]] static std::string Fingerprint(const char* kind, const EditorWorkingCopyOperationMetadata& operation,
		const std::string& inputId);

	EditorCoreService& m_core;
	IEditorWorkingCopyBackend& m_backend;
	const std::size_t m_maxCompletedOperations;
	mutable std::mutex m_mutex;
	std::uint64_t m_nextCoreOperation = 1;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;
	//! Pending/error/conflict state is keyed internally by canonical document key and is never surfaced as a key.
	std::map<std::wstring, EEditorWorkingCopyState> m_transientStates;
	//! A synchronous UI backend can reenter through callbacks. Reserve at most one effectful operation per document.
	std::map<std::wstring, std::string> m_inFlightDocumentOperations;
	//! Operation IDs are global coordinator replay keys, so they must not be concurrently owned by different documents.
	std::map<std::string, std::wstring> m_inFlightOperationIds;
};

} // namespace workbench::editor
