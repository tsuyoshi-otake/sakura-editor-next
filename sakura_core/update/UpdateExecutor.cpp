/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateExecutor.h"

#include <utility>

namespace update {

UpdateWorkerExecutor::UpdateWorkerExecutor(std::size_t maximumQueuedItems)
	: m_maximumQueuedItems(maximumQueuedItems == 0 ? 1 : maximumQueuedItems)
{
	m_worker = std::thread([this] { Run(); });
}

UpdateWorkerExecutor::~UpdateWorkerExecutor()
{
	Stop();
}

void UpdateWorkerExecutor::Post(std::function<void()> work)
{
	if (!work) return;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_stopping || m_queue.size() >= m_maximumQueuedItems) return;
		m_queue.push_back(std::move(work));
	}
	m_posted.notify_one();
}

void UpdateWorkerExecutor::Stop() noexcept
{
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_stopping) return;
		m_stopping = true;
		// Queued-but-not-started work is abandoned rather than run during
		// shutdown: it would only reach for services the owner is already
		// tearing down.
		m_queue.clear();
	}
	m_posted.notify_all();
	m_drained.notify_all();
	if (m_worker.joinable()) m_worker.join();
}

void UpdateWorkerExecutor::Drain()
{
	std::unique_lock<std::mutex> guard(m_mutex);
	m_drained.wait(guard, [this] { return m_stopping || (m_queue.empty() && !m_executing); });
}

void UpdateWorkerExecutor::Run()
{
	for (;;) {
		std::function<void()> work;
		{
			std::unique_lock<std::mutex> guard(m_mutex);
			m_posted.wait(guard, [this] { return m_stopping || !m_queue.empty(); });
			if (m_stopping) return;
			work = std::move(m_queue.front());
			m_queue.pop_front();
			m_executing = true;
		}

		// An update operation must not be able to take the process down; the
		// state machine's own failure paths already publish a diagnostic, and a
		// throw that escaped one of them is a bug to survive, not to propagate
		// out of a detached worker thread.
		try {
			work();
		} catch (...) {
		}

		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_executing = false;
		}
		m_drained.notify_all();
	}
}

} // namespace update
