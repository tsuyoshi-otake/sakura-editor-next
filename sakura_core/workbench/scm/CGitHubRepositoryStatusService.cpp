/*! @file
 * @brief Single-flight, cancellable background resolution of the GitHub repository status.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/CGitHubRepositoryStatusService.h"

#include <utility>

namespace workbench::scm {

CGitHubRepositoryStatusService::CGitHubRepositoryStatusService(ChangedCallback changed)
	: m_changed(std::move(changed))
	, m_stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
	m_worker = std::thread([this] { Run(); });
}

CGitHubRepositoryStatusService::~CGitHubRepositoryStatusService()
{
	Shutdown();
	if (m_stop != nullptr) ::CloseHandle(m_stop);
}

void CGitHubRepositoryStatusService::Shutdown() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		m_shutdown = true;
		m_changed = nullptr;
		if (m_stop != nullptr) ::SetEvent(m_stop);
	}
	m_wake.notify_all();
	if (m_worker.joinable()) m_worker.join();
}

void CGitHubRepositoryStatusService::SetWorkspaceRoot(std::wstring root)
{
	ChangedCallback changed;
	{
		std::lock_guard lock(m_mutex);
		if (m_shutdown || root == m_root) return;
		m_root = std::move(root);
		if (m_status.repository.has_value()) changed = m_changed;
		m_status = {};
		StartProbeLocked();
	}
	m_wake.notify_all();
	if (changed) changed();
}

void CGitHubRepositoryStatusService::Refresh()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_shutdown || m_root.empty() || m_probePending) return;
		if (m_lastProbeStart
			&& std::chrono::steady_clock::now() - *m_lastProbeStart < kMinimumRefreshInterval) {
			return;
		}
		StartProbeLocked();
	}
	m_wake.notify_all();
}

GitHubRepositoryStatus CGitHubRepositoryStatusService::Current() const
{
	std::lock_guard lock(m_mutex);
	return m_status;
}

void CGitHubRepositoryStatusService::StartProbeLocked()
{
	++m_generation;
	m_probePending = true;
	// Terminates a probe still running for the previous generation. The worker
	// resets the event only when it picks the new probe up.
	if (m_stop != nullptr) ::SetEvent(m_stop);
}

void CGitHubRepositoryStatusService::Publish(const std::uint64_t generation, const GitHubRepositoryStatus& status)
{
	ChangedCallback changed;
	{
		std::lock_guard lock(m_mutex);
		if (m_shutdown || generation != m_generation || m_status == status) return;
		m_status = status;
		changed = m_changed;
	}
	if (changed) changed();
}

void CGitHubRepositoryStatusService::Run()
{
	for (;;) {
		std::wstring root;
		std::uint64_t generation = 0;
		{
			std::unique_lock lock(m_mutex);
			m_wake.wait(lock, [this] { return m_shutdown || m_probePending; });
			if (m_shutdown) return;
			m_probePending = false;
			root = m_root;
			generation = m_generation;
			m_lastProbeStart = std::chrono::steady_clock::now();
			::ResetEvent(m_stop);
		}

		GitHubRepositoryStatus status;
		status.repository = QueryGitHubRepositoryRef(root, m_stop);
		if (!status.repository) {
			Publish(generation, status);
			continue;
		}
		// The name is known before gh answers, so show it immediately.
		Publish(generation, status);
		status.visibility = QueryGitHubRepositoryVisibility(*status.repository, root, m_stop);
		Publish(generation, status);
	}
}

} // namespace workbench::scm
