/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "update/IUpdateService.h"

namespace update {

//! One worker thread for every update side effect.
//!
//! The request service is synchronous, so a check or a download would otherwise
//! block the UI thread for as long as GitHub takes to answer. Exactly one thread
//! is used, deliberately: update work is inherently sequential, and a pool would
//! only make two downloads race for the same staging directory.
class UpdateWorkerExecutor final : public IUpdateExecutor {
public:
	explicit UpdateWorkerExecutor(std::size_t maximumQueuedItems = 32);
	~UpdateWorkerExecutor() override;

	UpdateWorkerExecutor(const UpdateWorkerExecutor&) = delete;
	UpdateWorkerExecutor& operator=(const UpdateWorkerExecutor&) = delete;

	//! Drops the work when the queue bound is reached or the executor is already
	//! stopping. Dropping is correct here: every posted item is a whole update
	//! operation that the state machine will re-derive on the next check, and an
	//! unbounded queue would let a stuck network turn repeated polls into
	//! unbounded memory.
	void Post(std::function<void()> work) override;

	//! Stops accepting work and joins the worker. Idempotent, and safe to call
	//! from the destructor of an owner that is shutting a window down.
	void Stop() noexcept;

	//! Blocks until the queue is empty and no item is executing. Present for
	//! tests and for orderly shutdown; production code never needs to wait.
	void Drain();

private:
	void Run();

	mutable std::mutex m_mutex;
	std::condition_variable m_posted;
	std::condition_variable m_drained;
	std::deque<std::function<void()>> m_queue;
	std::size_t m_maximumQueuedItems;
	bool m_stopping = false;
	bool m_executing = false;
	std::thread m_worker;
};

//! Runs the work on the calling thread, immediately. Used by tests so every
//! transition the state machine makes is observable synchronously, and by any
//! caller that is already on a thread where blocking is acceptable.
class InlineUpdateExecutor final : public IUpdateExecutor {
public:
	void Post(std::function<void()> work) override
	{
		if (work) work();
	}
};

} // namespace update
