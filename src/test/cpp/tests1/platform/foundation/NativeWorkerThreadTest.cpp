/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <gtest/gtest.h>

#include "platform/foundation/NativeWorkerThread.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace platform::foundation {
namespace {

/*!
	What the worker body records, behind the lock that guards it.

	It is held separately from the owner, so an assertion can outlive the owner
	and the handle it held and still read what the worker did. Every field is
	private: a test that reached for one directly would be reading it without
	the lock the worker writes it under.
*/
class Record final {
public:
	//! Announces that the body is running, so a later assertion about the join
	//! is about a worker that existed rather than one that never started.
	void MarkRunning()
	{
		{
			std::lock_guard lock(m_mutex);
			m_running = true;
		}
		m_changed.notify_all();
	}

	//! Holds the body until its owner asks it to stop, then counts the return.
	void AwaitStopAndComplete()
	{
		std::unique_lock lock(m_mutex);
		m_changed.wait(lock, [this]() { return m_stop; });
		++m_completions;
	}

	void RequestStop()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_changed.notify_all();
	}

	//! False when the body did not start within the timeout, which is a failure
	//! of the test's premise rather than of the property under test.
	[[nodiscard]] bool WaitUntilRunning()
	{
		std::unique_lock lock(m_mutex);
		return m_changed.wait_for(lock, std::chrono::seconds(5), [this]() { return m_running; });
	}

	[[nodiscard]] int Completions()
	{
		std::lock_guard lock(m_mutex);
		return m_completions;
	}
private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_running = false;
	bool m_stop = false;
	int m_completions = 0;
};

/*!
	An owner shaped like the production ones.

	It starts its worker from inside itself, which is where the pointer to a
	private member function may be formed, and it raises its stop in its own
	destructor - before the handle declared after it is destroyed and waits.
*/
class Owner final {
public:
	explicit Owner(std::shared_ptr<Record> record) noexcept : m_record(std::move(record)) {}
	~Owner() { m_record->RequestStop(); }

	Owner(const Owner&) = delete;
	Owner& operator=(const Owner&) = delete;

	void Start() { m_worker = CNativeWorkerThread::Start<Owner, &Owner::Run>(this); }
	//! Hands the join to the caller, leaving this owner holding nothing.
	[[nodiscard]] CNativeWorkerThread Release() noexcept { return std::move(m_worker); }
	[[nodiscard]] bool Started() const noexcept { return m_worker.Started(); }
private:
	void Run()
	{
		m_record->MarkRunning();
		m_record->AwaitStopAndComplete();
	}

	std::shared_ptr<Record> m_record;
	//! Declared last, so it is joined before the state its body reads is gone.
	CNativeWorkerThread m_worker;
};

TEST(NativeWorkerThread, JoinsTheWorkerWhenTheOwnerHoldingItIsDestroyed)
{
	auto record = std::make_shared<Record>();
	{
		Owner owner(record);
		owner.Start();
		ASSERT_TRUE(record->WaitUntilRunning());
		EXPECT_TRUE(owner.Started());
		// No join is written here. The owner's destructor raises the stop and
		// the handle's destructor waits, which is the whole of the teardown.
	}
	EXPECT_EQ(1, record->Completions());
}

TEST(NativeWorkerThread, MovesTheJoinWithTheHandleAndPerformsItExactlyOnce)
{
	auto record = std::make_shared<Record>();
	Owner owner(record);
	owner.Start();
	ASSERT_TRUE(record->WaitUntilRunning());

	CNativeWorkerThread moved = owner.Release();
	EXPECT_FALSE(owner.Started());
	EXPECT_TRUE(moved.Started());

	record->RequestStop();
	moved.Join();
	EXPECT_FALSE(moved.Started());
	// Joining again, and the destructor after it, each find nothing left to
	// wait for: the handle forgot the worker as it joined it.
	moved.Join();
	EXPECT_EQ(1, record->Completions());
}

TEST(NativeWorkerThread, JoinsTheWorkerItHeldBeforeAdoptingAnother)
{
	auto first = std::make_shared<Record>();
	auto second = std::make_shared<Record>();
	Owner firstOwner(first);
	Owner secondOwner(second);
	firstOwner.Start();
	secondOwner.Start();
	ASSERT_TRUE(first->WaitUntilRunning());
	ASSERT_TRUE(second->WaitUntilRunning());

	CNativeWorkerThread held = firstOwner.Release();
	first->RequestStop();
	// Adopting the second worker is also the release of the first: the
	// assignment joins what it is about to stop owning.
	held = secondOwner.Release();
	EXPECT_EQ(1, first->Completions());
	EXPECT_EQ(0, second->Completions());

	second->RequestStop();
	held.Join();
	EXPECT_EQ(1, second->Completions());
}

TEST(NativeWorkerThread, OwnsNothingWhenItWasNeverStarted)
{
	CNativeWorkerThread idle;
	EXPECT_FALSE(idle.Started());
	idle.Join();
	EXPECT_FALSE(idle.Started());

	const CNativeWorkerThread adopted = std::move(idle);
	EXPECT_FALSE(adopted.Started());
}

} // namespace
} // namespace platform::foundation
