/*! @file
 * @brief File-backed, cancellable source adapter for workspace artifacts.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/filesystem/IFileService.h>
#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace workbench::workspace {

enum class EWorkspaceArtifactDocumentSourceStatus : std::uint8_t {
	Started,
	Updated,
	Reloaded,
	Stopped,
	InvalidRequest,
	CapacityExceeded,
	StartFailed,
	NotStarted,
	ReentrantStopDenied,
	ReadFailed,
};

struct WorkspaceArtifactDocumentSourceResult final {
	EWorkspaceArtifactDocumentSourceStatus status = EWorkspaceArtifactDocumentSourceStatus::ReadFailed;
	std::optional<platform::filesystem::EFileResultStatus> fileStatus;
	std::vector<WorkspaceArtifactDocumentResult> documents;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkspaceArtifactDocumentSourceStatus::Started
			|| status == EWorkspaceArtifactDocumentSourceStatus::Updated
			|| status == EWorkspaceArtifactDocumentSourceStatus::Reloaded
			|| status == EWorkspaceArtifactDocumentSourceStatus::Stopped;
	}
};

//! Every request is a complete immutable topology. The generation must advance
//! whenever the workspace identity/topology changes; revisions are assigned by
//! this controller for each completed filesystem operation.
struct WorkspaceArtifactDocumentSourceRequest final {
	std::uint64_t generation = 0;
	std::vector<platform::uri::Uri> workspaceFolders;
	std::optional<platform::uri::Uri> workspaceConfiguration;
};

//! Production composition boundary: it borrows `IFileService` and the pure
//! artifact service, reads `.code-workspace` plus each `.vscode/{tasks,launch,
//! extensions}.json`, and owns only cancellable watches/workers. It never owns
//! Task/DAP/extension execution and does not stop the borrowed service.
class CWorkspaceArtifactDocumentSourceController final {
public:
	using ReloadCallback = std::function<void(const WorkspaceArtifactDocumentSourceResult&)>;

	explicit CWorkspaceArtifactDocumentSourceController(
		platform::filesystem::IFileService& fileService,
		CWorkspaceArtifactDocumentService& documentService) noexcept;
	~CWorkspaceArtifactDocumentSourceController();

	CWorkspaceArtifactDocumentSourceController(const CWorkspaceArtifactDocumentSourceController&) = delete;
	CWorkspaceArtifactDocumentSourceController& operator=(const CWorkspaceArtifactDocumentSourceController&) = delete;

	//! Installs watches after an initial bounded snapshot. Watch capability gaps
	//! are best effort; successful reads remain usable without a watch backend.
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult Start(
		WorkspaceArtifactDocumentSourceRequest request, ReloadCallback callback = {}) noexcept;
	//! Replaces the topology and schedules one deduplicated rebuild/reload.
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult Update(WorkspaceArtifactDocumentSourceRequest request) noexcept;
	//! Performs one bounded reload while started. Read/parse failures preserve the
	//! last accepted service document; only NotFound removes a contribution.
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult Reload() noexcept;
	//! Cancels watches, joins every worker/dispatcher, and guarantees no callback
	//! can begin after return. Stop from this adapter callback is rejected.
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult Stop() noexcept;

private:
	struct WatchSlot;
	struct ReloadOneResult;

	[[nodiscard]] static EWorkspaceArtifactDocumentSourceStatus ValidateRequest(
		const WorkspaceArtifactDocumentSourceRequest& request) noexcept;
	void StartWorkersLocked();
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult StopLocked() noexcept;
	void CancelAndJoinWorkers() noexcept;
	[[nodiscard]] bool BeginDocumentOperation() noexcept;
	void EndDocumentOperation() noexcept;
	void RebuildTopology() noexcept;
	void DispatchMain() noexcept;
	void WorkerMain(WatchSlot* slot) noexcept;
	void Queue(bool rebuild) noexcept;
	[[nodiscard]] bool CanDispatch() const noexcept;
	[[nodiscard]] bool IsRelevant(const WatchSlot& slot, std::size_t targetIndex,
		const platform::filesystem::FileWatchEvent& event) const noexcept;
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult ReloadSnapshot() noexcept;
	[[nodiscard]] ReloadOneResult ReloadOne(
		EWorkspaceArtifactDocumentKind kind,
		EWorkspaceArtifactDocumentSource source,
		const std::optional<platform::uri::Uri>& folderUri,
		const platform::uri::Uri& resource,
		std::uint64_t generation) noexcept;
	[[nodiscard]] std::uint64_t NextRevision() noexcept;

	platform::filesystem::IFileService& m_fileService;
	CWorkspaceArtifactDocumentService& m_documentService;
	//! Serializes Start/Stop state transitions, including initial reload and
	//! dispatcher publication. A concurrent Stop waits for Start to publish a
	//! complete lifecycle or roll it back; ordinary Reload reads remain outside
	//! this gate and are fenced immediately before their service mutation.
	std::mutex m_lifecycleMutex;
	mutable std::mutex m_mutex;
	std::condition_variable m_pending;
	std::condition_variable m_documentOperationsComplete;
	std::optional<WorkspaceArtifactDocumentSourceRequest> m_request;
	ReloadCallback m_callback;
	std::vector<std::unique_ptr<WatchSlot>> m_slots;
	std::thread m_dispatcher;
	std::uint64_t m_nextRevision = 1;
	bool m_started = false;
	bool m_stopping = false;
	bool m_rebuildRequested = false;
	bool m_reloadPending = false;
	bool m_rebuilding = false;
	std::size_t m_activeDocumentOperations = 0;
};

} // namespace workbench::workspace
