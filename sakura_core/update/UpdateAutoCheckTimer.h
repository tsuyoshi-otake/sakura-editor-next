/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "update/IUpdateService.h"

namespace update {

//! One background thread that runs at most one delayed callback at a time.
//!
//! This is what turns `update.mode`'s policy decisions (`AllowsAutomaticCheck`,
//! `AllowsPeriodicCheck`) into an observed side effect: `UpdateService::Impl`
//! arms this timer instead of ever spinning its own sleep loop, so the state
//! machine stays free of wall-clock waits and remains exercisable through a
//! fake `IUpdateScheduler` in tests. See `sakura_core/update/CLAUDE.md`.
class UpdateAutoCheckTimer final : public IUpdateScheduler {
public:
	UpdateAutoCheckTimer();
	~UpdateAutoCheckTimer() override;

	UpdateAutoCheckTimer(const UpdateAutoCheckTimer&) = delete;
	UpdateAutoCheckTimer& operator=(const UpdateAutoCheckTimer&) = delete;

	//! Replaces any pending item, so `UpdateService` never has to track whether
	//! one is already armed before arming another.
	void PostDelayed(std::chrono::milliseconds delay, std::function<void()> work) override;

	//! Discards the pending item, if any, and joins the worker. Idempotent, and
	//! safe to call from the destructor of an owner that is shutting a window
	//! down — the same contract as `UpdateWorkerExecutor::Stop`.
	void Stop() noexcept;

private:
	void Run();

	mutable std::mutex m_mutex;
	std::condition_variable m_wake;
	std::function<void()> m_pending;
	std::chrono::steady_clock::time_point m_dueTime;
	//! Bumped by every `PostDelayed`/`Stop`, so the worker can distinguish "the
	//! item I was waiting for is still the one that is due" from "something
	//! replaced or cancelled it while I slept".
	std::uint64_t m_generation = 0;
	bool m_stopping = false;
	std::thread m_worker;
};

} // namespace update
