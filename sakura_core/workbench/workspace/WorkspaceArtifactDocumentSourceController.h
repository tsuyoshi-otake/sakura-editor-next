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
#include "workbench/WorkerRetirementService.h"

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
	RetirementUnavailable,
	NotStarted,
	ReentrantStopDenied,
	ReadFailed,
};

struct WorkspaceArtifactDocumentSourceResult final {
	EWorkspaceArtifactDocumentSourceStatus status = EWorkspaceArtifactDocumentSourceStatus::ReadFailed;
	std::optional<platform::filesystem::EFileResultStatus> fileStatus;
	std::vector<WorkspaceArtifactDocumentResult> documents;
	//! Stop returns after handing live threads to the bounded reaper. These
	//! fields expose that finalization is pending without making the caller wait.
	bool retirementPending = false;
	bool retirementFinalized = false;

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

//! Production composition boundary: it retains shared ownership of the
//! `IFileService` and pure artifact service while retired work is pending,
//! reads `.code-workspace` plus each `.vscode/{tasks,launch,extensions}.json`,
//! and owns only cancellable watches/workers. It never owns Task/DAP/extension
//! execution and does not stop either shared service.
class CWorkspaceArtifactDocumentSourceController final {
public:
	using ReloadCallback = std::function<void(const WorkspaceArtifactDocumentSourceResult&)>;

	explicit CWorkspaceArtifactDocumentSourceController(
		std::shared_ptr<platform::filesystem::IFileService> fileService,
		std::shared_ptr<CWorkspaceArtifactDocumentService> documentService) noexcept;
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
	//! Cancels watches, wakes workers, and transfers every worker/dispatcher to
	//! the bounded retirement service. Return is non-blocking with respect to
	//! worker completion; the result exposes pending versus finalized retirement.
	//! Stop from this adapter callback is rejected.
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult Stop() noexcept;
	//! Non-blocking observation of the last lifecycle's retirement terminal state.
	[[nodiscard]] bool IsRetirementFinalized() const noexcept;

private:
	struct SharedState;
	struct WatchSlot;
	struct ReloadOneResult;

	[[nodiscard]] static EWorkspaceArtifactDocumentSourceStatus ValidateRequest(
		const WorkspaceArtifactDocumentSourceRequest& request) noexcept;
	[[nodiscard]] bool StartWorkersLocked(const std::shared_ptr<SharedState>& state);
	[[nodiscard]] WorkspaceArtifactDocumentSourceResult StopLocked() noexcept;
	static void CancelAndRetireWorkers(const std::shared_ptr<SharedState>& state) noexcept;
	static void MarkThreadFinished(const std::shared_ptr<SharedState>& state) noexcept;
	[[nodiscard]] static bool BeginDocumentOperation(const std::shared_ptr<SharedState>& state) noexcept;
	static void EndDocumentOperation(const std::shared_ptr<SharedState>& state) noexcept;
	static void RebuildTopology(const std::shared_ptr<SharedState>& state) noexcept;
	static void DispatchMain(const std::shared_ptr<SharedState>& state) noexcept;
	static void WorkerMain(const std::shared_ptr<SharedState>& state,
		const std::shared_ptr<WatchSlot>& slot) noexcept;
	static void Queue(const std::shared_ptr<SharedState>& state, bool rebuild) noexcept;
	[[nodiscard]] static bool CanDispatch(const std::shared_ptr<SharedState>& state) noexcept;
	[[nodiscard]] static bool IsRelevant(const WatchSlot& slot, std::size_t targetIndex,
		const platform::filesystem::FileWatchEvent& event) noexcept;
	[[nodiscard]] static WorkspaceArtifactDocumentSourceResult ReloadSnapshot(
		const std::shared_ptr<SharedState>& state) noexcept;
	[[nodiscard]] static ReloadOneResult ReloadOne(
		const std::shared_ptr<SharedState>& state,
		EWorkspaceArtifactDocumentKind kind,
		EWorkspaceArtifactDocumentSource source,
		const std::optional<platform::uri::Uri>& folderUri,
		const platform::uri::Uri& resource,
		std::uint64_t generation) noexcept;
	[[nodiscard]] static std::uint64_t NextRevision(const std::shared_ptr<SharedState>& state) noexcept;

	std::shared_ptr<platform::filesystem::IFileService> m_fileService;
	std::shared_ptr<CWorkspaceArtifactDocumentService> m_documentService;
	//! Serializes Start/Stop state transitions, including initial reload and
	//! dispatcher publication. A concurrent Stop waits for Start to publish a
	//! complete lifecycle or roll it back; ordinary Reload reads remain outside
	//! this gate and are fenced immediately before their service mutation.
	std::mutex m_lifecycleMutex;
	std::shared_ptr<SharedState> m_state;
};

} // namespace workbench::workspace
