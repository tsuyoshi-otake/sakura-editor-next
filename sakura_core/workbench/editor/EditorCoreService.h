/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/DocumentRegistry.h"
#include "workbench/editor/EditorGroupModel.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace workbench::editor {

struct EditorCoreSubscriptionState;

/*!
	@brief Authoritative pure-model facade for the first editor workbench slice.

	The facade has one editor group, a distinct shared-document registry, bounded
	operation replay, and revisioned post-commit notifications. It intentionally
	has no resolver implementation, save adapter, HWND, legacy document, INI, or
	transport dependency.
*/
class EditorCoreService final {
public:
	explicit EditorCoreService(std::uint64_t generation = 1, std::size_t maxCompletedOperations = 4096);
	~EditorCoreService();

	[[nodiscard]] EditorCoreSnapshot Snapshot() const;
	[[nodiscard]] EditorOperationResult ResolveDocument(const ResolveDocumentRequest& request);
	[[nodiscard]] EditorOperationResult ReleaseDocument(const ReleaseDocumentRequest& request);
	[[nodiscard]] EditorOperationResult OpenResolvedInput(const OpenResolvedInputRequest& request);
	[[nodiscard]] EditorOperationResult ShowInput(const ShowInputRequest& request);
	[[nodiscard]] EditorOperationResult SetDocumentState(const SetDocumentStateRequest& request);
	//! Semantic first-slice spelling for callers that only change dirty/revision document state.
	[[nodiscard]] EditorOperationResult SetDirty(const SetDocumentStateRequest& request)
	{
		return SetDocumentState(request);
	}
	[[nodiscard]] EditorOperationResult ReplaceInputDocument(const ReplaceInputDocumentRequest& request);
	[[nodiscard]] EditorOperationResult CloseInput(const CloseInputRequest& request);
	[[nodiscard]] std::unique_ptr<IEditorCoreSubscription> Subscribe(EditorCoreChangeCallback callback);

private:
	struct CompletedOperation {
		std::string fingerprint;
		EditorOperationResult result;
	};

	[[nodiscard]] static std::string Fingerprint(const OpenResolvedInputRequest& request);
	[[nodiscard]] static std::string Fingerprint(const ResolveDocumentRequest& request);
	[[nodiscard]] static std::string Fingerprint(const ReleaseDocumentRequest& request);
	[[nodiscard]] static std::string Fingerprint(const ShowInputRequest& request);
	[[nodiscard]] static std::string Fingerprint(const SetDocumentStateRequest& request);
	[[nodiscard]] static std::string Fingerprint(const ReplaceInputDocumentRequest& request);
	[[nodiscard]] static std::string Fingerprint(const CloseInputRequest& request);
	[[nodiscard]] EditorOperationResult CheckCompletedOrConflict(
		const EditorOperationMetadata& operation, const std::string& fingerprint, bool& handled) const;
	[[nodiscard]] EditorOperationResult RevisionConflictResult() const;
	[[nodiscard]] EditorCoreChangeBatch Commit(std::vector<EditorCoreChange> changes);
	void RememberCompleted(const std::string& operationId, std::string fingerprint, const EditorOperationResult& result);
	//! Called while m_mutex is held. Returns true only to the caller elected to drain after unlocking it.
	[[nodiscard]] bool EnqueueNotification(const EditorCoreChangeBatch& batch);
	void DrainNotifications();

	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 1;
	std::uint64_t m_revision = 0;
	std::size_t m_maxCompletedOperations = 4096;
	DocumentRegistry m_documents;
	EditorGroupModel m_group;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;

	std::shared_ptr<EditorCoreSubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<EditorCoreChangeBatch> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

} // namespace workbench::editor
