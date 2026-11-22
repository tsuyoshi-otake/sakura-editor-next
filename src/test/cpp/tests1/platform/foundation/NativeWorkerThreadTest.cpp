/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <gtest/gtest.h>

#include "platform/foundation/NativeWorkerThread.h"

#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
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

struct ThrowingWorker {
	void ThrowStandardException() { throw std::runtime_error("worker failure"); }
	void ThrowUnknownException() { throw 1; }
};

struct SelfReleasingWorker {
	void Run()
	{
		// Wait for the caller to install the handle and transfer this owner.
		auto self = ownership.get();
		auto completion = std::move(self->released);
		self.reset();
		completion.set_value();
	}

	std::future<std::unique_ptr<SelfReleasingWorker>> ownership;
	std::promise<void> released;
	CNativeWorkerThread worker;
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
	const auto result = moved.Join();
	EXPECT_EQ(SakuraWorkerThreadStatus::Ok, result.status);
	EXPECT_EQ(SakuraWorkerBodyOutcome::Completed, result.outcome);
	EXPECT_FALSE(moved.Started());
	// Joining again, and the destructor after it, each find nothing left to
	// wait for: the handle forgot the worker as it joined it.
	EXPECT_EQ(SakuraWorkerThreadStatus::InvalidArgument, moved.Join().status);
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

TEST(NativeWorkerThread, ReportsStandardExceptionsAsRedactedOutcomes)
{
	ThrowingWorker owner;
	auto worker = CNativeWorkerThread::Start<ThrowingWorker, &ThrowingWorker::ThrowStandardException>(&owner);
	const auto result = worker.Join();
	EXPECT_EQ(SakuraWorkerThreadStatus::Ok, result.status);
	EXPECT_EQ(SakuraWorkerBodyOutcome::ThrewException, result.outcome);
}

TEST(NativeWorkerThread, ReportsUnknownExceptionsAsRedactedOutcomes)
{
	ThrowingWorker owner;
	auto worker = CNativeWorkerThread::Start<ThrowingWorker, &ThrowingWorker::ThrowUnknownException>(&owner);
	const auto result = worker.Join();
	EXPECT_EQ(SakuraWorkerThreadStatus::Ok, result.status);
	EXPECT_EQ(SakuraWorkerBodyOutcome::ThrewUnknown, result.outcome);
}

TEST(NativeWorkerThread, CanDestroyItsHandleFromTheWorkerThread)
{
	std::promise<std::unique_ptr<SelfReleasingWorker>> transfer;
	auto owner = std::make_unique<SelfReleasingWorker>();
	owner->ownership = transfer.get_future();
	auto released = owner->released.get_future();
	owner->worker = CNativeWorkerThread::Start<SelfReleasingWorker, &SelfReleasingWorker::Run>(owner.get());
	transfer.set_value(std::move(owner));

	ASSERT_EQ(std::future_status::ready, released.wait_for(std::chrono::seconds(5)));
	released.get();
}

TEST(NativeWorkerThread, ConcurrentJoinsConsumeOnlyOneTokenCopy)
{
	using Status = SakuraWorkerThreadStatus;
	using Outcome = SakuraWorkerBodyOutcome;
	constexpr int callerCount = 4;
	struct Attempt {
		std::uint64_t token;
		Status status;
		Outcome outcome;
	};

	ThrowingWorker owner;
	std::uint64_t issued = 0;
	ASSERT_EQ(Status::Ok, ::sakura_worker_thread_start_v2(
		&detail::InvokeWorkerBody<ThrowingWorker, &ThrowingWorker::ThrowStandardException>, &owner, &issued));
	std::barrier gate(callerCount);
	std::array<Attempt, callerCount> attempts;
	{
		std::array<std::jthread, callerCount> callers;
		for (std::size_t i = 0; i < callers.size(); ++i) {
			callers[i] = std::jthread([&, i] {
				auto& attempt = attempts[i];
				attempt.token = issued;
				attempt.outcome = Outcome::Completed;
				gate.arrive_and_wait();
				attempt.status = ::sakura_worker_thread_join_v2(&attempt.token, &attempt.outcome);
			});
		}
	}

	int successes = 0;
	for (const auto& attempt : attempts) {
		if (attempt.status == Status::Ok) {
			++successes;
			EXPECT_EQ(0u, attempt.token);
			EXPECT_EQ(Outcome::ThrewException, attempt.outcome);
		} else {
			EXPECT_EQ(Status::NotFound, attempt.status);
			EXPECT_EQ(issued, attempt.token);
			EXPECT_EQ(Outcome::Completed, attempt.outcome);
		}
	}
	EXPECT_EQ(1, successes);
}

} // namespace
} // namespace platform::foundation
