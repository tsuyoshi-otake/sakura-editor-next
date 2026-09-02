/*! @file
 * @brief Cancellable, HWND-free watch lifecycle for file-backed configuration.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/filesystem/IFileService.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace config {

//! A notification is advisory: its consumer must resnapshot through the
//! existing file-source controller before mutating configuration state.
enum class EConfigurationFileWatchChange : std::uint8_t {
	ProfileSettings,
	WorkspaceFolderSettings,
	WorkspaceConfiguration,
	//! An overflow, rescan, or disposed watch invalidated the topology. The
	//! consumer must resnapshot every configured document.
	FullRescan,
};

enum class EConfigurationFileWatchStatus : std::uint8_t {
	Started,
	Updated,
	Stopped,
	InvalidRequest,
	CapacityExceeded,
	StartFailed,
	NotStarted,
	ReentrantStopDenied,
};

struct ConfigurationFileWatchResult final {
	EConfigurationFileWatchStatus status = EConfigurationFileWatchStatus::StartFailed;
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EConfigurationFileWatchStatus::Started
			|| status == EConfigurationFileWatchStatus::Updated
			|| status == EConfigurationFileWatchStatus::Stopped;
	}
};

//! The explicit resources forming one immutable watch topology. Folder roots,
//! rather than only `.vscode`, deliberately keep initially-missing `.vscode`
//! lifecycle visible through a non-recursive parent watch.
struct ConfigurationFileWatchRequest final {
	platform::uri::Uri profileSettings;
	std::vector<platform::uri::Uri> workspaceFolders;
	std::optional<platform::uri::Uri> workspaceConfiguration;
};

//! Runtime-owned watch lifecycle. It owns cancellation and worker joins, but
//! borrows the filesystem service and never reads, parses, or applies settings.
//! Each requested directory watch is non-recursive. For each workspace folder,
//! the topology is exactly two levels: folder -> `.vscode` lifecycle and, when
//! present, `.vscode` -> `settings.json` membership.
class CConfigurationFileWatchController final {
public:
	using ChangeCallback = std::function<void(EConfigurationFileWatchChange)>;

	explicit CConfigurationFileWatchController(platform::filesystem::IFileService& fileService) noexcept;
	~CConfigurationFileWatchController();

	CConfigurationFileWatchController(const CConfigurationFileWatchController&) = delete;
	CConfigurationFileWatchController& operator=(const CConfigurationFileWatchController&) = delete;

	//! Replaces the complete topology. Watch failures are intentionally
	//! best-effort capability gaps; successful watches remain active.
	[[nodiscard]] ConfigurationFileWatchResult Start(ConfigurationFileWatchRequest request, ChangeCallback callback) noexcept;
	[[nodiscard]] ConfigurationFileWatchResult Update(ConfigurationFileWatchRequest request) noexcept;
	//! Cancels every watch, joins workers and the dispatcher, and guarantees that
	//! no callback can begin after it returns.
	//! Calling Stop from this controller's own callback is explicitly rejected:
	//! another lifecycle owner must finalize after the callback returns, avoiding
	//! a dispatcher self-join and destruction on the active callback stack.
	[[nodiscard]] ConfigurationFileWatchResult Stop() noexcept;

private:
	struct WatchSlot;

	void StartWorkersLocked();
	[[nodiscard]] ConfigurationFileWatchResult StopLocked() noexcept;
	void CancelAndJoinWorkers() noexcept;
	void DispatchMain() noexcept;
	void WorkerMain(WatchSlot* slot) noexcept;
	void Queue(EConfigurationFileWatchChange change, bool rebuild) noexcept;
	void RebuildTopology() noexcept;
	[[nodiscard]] bool CanDispatch() const noexcept;
	[[nodiscard]] bool IsDispatcherThread() const noexcept;
	[[nodiscard]] bool IsRelevant(const WatchSlot& slot, std::size_t targetIndex,
		const platform::filesystem::FileWatchEvent& event) const noexcept;
	[[nodiscard]] static EConfigurationFileWatchStatus ValidateRequest(const ConfigurationFileWatchRequest& request) noexcept;

	platform::filesystem::IFileService& m_fileService;
	//! Serializes ownership-changing Start/Stop operations. State callbacks never
	//! take this lock, so an external Stop can still join the dispatcher.
	mutable std::mutex m_lifecycleMutex;
	mutable std::mutex m_mutex;
	std::condition_variable m_pending;
	std::optional<ConfigurationFileWatchRequest> m_request;
	ChangeCallback m_callback;
	std::vector<std::unique_ptr<WatchSlot>> m_slots;
	std::thread m_dispatcher;
	std::thread::id m_dispatcherThreadId;
	bool m_started = false;
	bool m_stopping = false;
	bool m_rebuildRequested = false;
	bool m_rebuilding = false;
	bool m_profilePending = false;
	bool m_folderPending = false;
	bool m_workspacePending = false;
};

} // namespace config
