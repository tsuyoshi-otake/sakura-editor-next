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

//! What the worker body records. It is held separately from the owner so a
//! test can still read it once the owner, and the handle it held, are gone.
struct Record final {
	std::mutex mutex;
	std::condition_variable changed;
	bool running = false;
	bool stop = false;
	int completions = 0;
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
	~Owner() { RequestStop(); }

	Owner(const Owner&) = delete;
	Owner& operator=(const Owner&) = delete;

	void Start() { m_worker = CNativeWorkerThread::Start<Owner, &Owner::Run>(this); }
	//! Hands the join to the caller, leaving this owner holding nothing.
	[[nodiscard]] CNativeWorkerThread Release() noexcept { return std::move(m_worker); }
	[[nodiscard]] bool Started() const noexcept { return m_worker.Started(); }

	void RequestStop() noexcept
	{
		{
			std::lock_guard lock(m_record->mutex);
			m_record->stop = true;
		}
		m_record->changed.notify_all();
	}
private:
	void Run()
	{
		{
			std::lock_guard lock(m_record->mutex);
			m_record->running = true;
		}
		m_record->changed.notify_all();
		std::unique_lock lock(m_record->mutex);
		m_record->changed.wait(lock, [this]() { return m_record->stop; });
		++m_record->completions;
	}

	std::shared_ptr<Record> m_record;
	//! Declared last, so it is joined before the state its body reads is gone.
	CNativeWorkerThread m_worker;
};

//! Proves the body actually ran, so a later assertion about the join is about
//! a worker that existed rather than one that never started.
void WaitUntilRunning(Record& record)
{
	std::unique_lock lock(record.mutex);
	ASSERT_TRUE(record.changed.wait_for(lock, std::chrono::seconds(5),
		[&record]() { return record.running; }));
}

[[nodiscard]] int Completions(Record& record)
{
	std::lock_guard lock(record.mutex);
	return record.completions;
}

TEST(NativeWorkerThread, JoinsTheWorkerWhenTheOwnerHoldingItIsDestroyed)
{
	auto record = std::make_shared<Record>();
	{
		Owner owner(record);
		owner.Start();
		ASSERT_NO_FATAL_FAILURE(WaitUntilRunning(*record));
		EXPECT_TRUE(owner.Started());
		// No join is written here. The owner's destructor raises the stop and
		// the handle's destructor waits, which is the whole of the teardown.
	}
	EXPECT_EQ(1, Completions(*record));
}

TEST(NativeWorkerThread, MovesTheJoinWithTheHandleAndPerformsItExactlyOnce)
{
	auto record = std::make_shared<Record>();
	Owner owner(record);
	owner.Start();
	ASSERT_NO_FATAL_FAILURE(WaitUntilRunning(*record));

	CNativeWorkerThread moved = owner.Release();
	EXPECT_FALSE(owner.Started());
	EXPECT_TRUE(moved.Started());

	owner.RequestStop();
	moved.Join();
	EXPECT_FALSE(moved.Started());
	// Joining again, and the destructor after it, each find nothing left to
	// wait for: the handle forgot the worker as it joined it.
	moved.Join();
	EXPECT_EQ(1, Completions(*record));
}

TEST(NativeWorkerThread, JoinsTheWorkerItHeldBeforeAdoptingAnother)
{
	auto first = std::make_shared<Record>();
	auto second = std::make_shared<Record>();
	Owner firstOwner(first);
	Owner secondOwner(second);
	firstOwner.Start();
	secondOwner.Start();
	ASSERT_NO_FATAL_FAILURE(WaitUntilRunning(*first));
	ASSERT_NO_FATAL_FAILURE(WaitUntilRunning(*second));

	CNativeWorkerThread held = firstOwner.Release();
	firstOwner.RequestStop();
	// Adopting the second worker is also the release of the first: the
	// assignment joins what it is about to stop owning.
	held = secondOwner.Release();
	EXPECT_EQ(1, Completions(*first));
	EXPECT_EQ(0, Completions(*second));

	secondOwner.RequestStop();
	held.Join();
	EXPECT_EQ(1, Completions(*second));
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
