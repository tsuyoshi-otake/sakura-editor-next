/*! @file
 * @brief Single-flight, cancellable background resolution of the GitHub repository status.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitHubRepositoryStatus.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace workbench::scm {

//!
//! @brief Owns the one worker that resolves `GitHubRepositoryStatus` for a workspace root.
//!
//! Load rules: at most one probe runs at a time; a new root cancels the running
//! probe (the child process is terminated through the stop event) and the stale
//! result is discarded by a generation fence; `Refresh` for the same root is
//! ignored until `kMinimumRefreshInterval` has passed since the last probe began,
//! so a caller that fires on every SCM publication cannot become a gh polling loop.
//! A failed probe is not retried on its own.
//!
//! `changed` runs on the worker thread and must only post to the UI thread; the
//! UI thread then reads `Current()`.
//!
class CGitHubRepositoryStatusService final {
public:
	using ChangedCallback = std::function<void()>;
	static constexpr std::chrono::seconds kMinimumRefreshInterval{ 120 };

	explicit CGitHubRepositoryStatusService(ChangedCallback changed);
	~CGitHubRepositoryStatusService();
	CGitHubRepositoryStatusService(const CGitHubRepositoryStatusService&) = delete;
	CGitHubRepositoryStatusService& operator=(const CGitHubRepositoryStatusService&) = delete;

	//! Probe a new workspace root now. The previous status is cleared at once.
	void SetWorkspaceRoot(std::wstring root);
	//! Probe the current root again, no more often than `kMinimumRefreshInterval`.
	void Refresh();
	[[nodiscard]] GitHubRepositoryStatus Current() const;
	//! Cancels any probe and joins the worker. Idempotent; no callback runs afterwards.
	void Shutdown() noexcept;

private:
	void Run();
	void StartProbeLocked();
	void Publish(std::uint64_t generation, const GitHubRepositoryStatus& status);

	ChangedCallback m_changed;
	mutable std::mutex m_mutex;
	std::condition_variable m_wake;
	HANDLE m_stop = nullptr;
	std::wstring m_root;
	std::uint64_t m_generation = 0;
	bool m_probePending = false;
	bool m_shutdown = false;
	std::optional<std::chrono::steady_clock::time_point> m_lastProbeStart;
	GitHubRepositoryStatus m_status;
	std::thread m_worker;
};

} // namespace workbench::scm
