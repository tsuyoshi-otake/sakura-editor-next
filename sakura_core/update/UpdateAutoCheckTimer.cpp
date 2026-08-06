/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateAutoCheckTimer.h"

#include <utility>

namespace update {

UpdateAutoCheckTimer::UpdateAutoCheckTimer()
{
	m_worker = std::thread([this] { Run(); });
}

UpdateAutoCheckTimer::~UpdateAutoCheckTimer()
{
	Stop();
}

void UpdateAutoCheckTimer::PostDelayed(std::chrono::milliseconds delay, std::function<void()> work)
{
	if (!work) return;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_stopping) return;
		m_pending = std::move(work);
		m_dueTime = std::chrono::steady_clock::now() + delay;
		++m_generation;
	}
	m_wake.notify_one();
}

void UpdateAutoCheckTimer::Stop() noexcept
{
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_stopping) return;
		m_stopping = true;
		// A pending item that never fired is abandoned rather than run during
		// shutdown: it would only reach for services the owner is already
		// tearing down.
		m_pending = nullptr;
		++m_generation;
	}
	m_wake.notify_all();
	if (m_worker.joinable()) m_worker.join();
}

void UpdateAutoCheckTimer::Run()
{
	for (;;) {
		std::function<void()> due;
		{
			std::unique_lock<std::mutex> guard(m_mutex);
			m_wake.wait(guard, [this] { return m_stopping || m_pending != nullptr; });
			if (m_stopping) return;

			const std::uint64_t generation = m_generation;
			const auto until = m_dueTime;
			// Wait until the item is due. A spurious wake, or one caused by a
			// replacement/cancellation bumping the generation, sends this straight
			// back to the top of the loop instead of firing the wrong item.
			m_wake.wait_until(guard, until, [this, generation] { return m_stopping || m_generation != generation; });
			if (m_stopping) return;
			if (m_generation != generation) continue;

			due = std::move(m_pending);
			m_pending = nullptr;
		}

		// Automatic-check work must not be able to take the process down; the
		// state machine's own failure paths already publish a diagnostic, and a
		// throw that escaped one of them is a bug to survive, not to propagate
		// out of a detached timer thread.
		if (due) {
			try {
				due();
			} catch (...) {
			}
		}
	}
}

} // namespace update
