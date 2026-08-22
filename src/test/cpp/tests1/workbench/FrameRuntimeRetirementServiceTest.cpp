/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameRuntimeRetirementService.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace workbench::rendering {
namespace {

using namespace std::chrono_literals;

template<class Poll>
bool WaitUntil(Poll&& poll)
{
	const auto deadline = std::chrono::steady_clock::now() + 3s;
	for (;;) {
		if (poll()) return true;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(1ms);
	}
}

TEST(FrameRuntimeRetirementService, ReservesCapacityBeforeHandoff)
{
	auto& service = FrameRuntimeRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));

	std::array<std::optional<FrameRuntimeRetirementService::Reservation>,
		FrameRuntimeRetirementService::kMaximumSlots> reservations;
	for (auto& reservation : reservations) {
		reservation = service.TryReserve();
		ASSERT_TRUE(reservation.has_value());
	}

	EXPECT_EQ(
		FrameRuntimeRetirementService::kMaximumSlots,
		service.ReservedOrPendingCount());
	EXPECT_FALSE(service.TryReserve().has_value());

	for (auto& reservation : reservations) reservation.reset();
	EXPECT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
}

TEST(FrameRuntimeRetirementService, HandoffStatusRemainsObservableUntilWaitCompletes)
{
	auto& service = FrameRuntimeRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));

	auto reservation = service.TryReserve();
	ASSERT_TRUE(reservation.has_value());
	auto runtime = std::make_unique<FrameCoordinatorRuntime>();
	const auto started = std::chrono::steady_clock::now();
	const auto result = service.Retire(runtime, std::move(*reservation));
	reservation.reset();

	EXPECT_TRUE(result.Accepted());
	EXPECT_EQ(EFrameRuntimeRetirementStatus::Retired, result.status);
	EXPECT_EQ(nullptr, runtime);
	EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
	ASSERT_TRUE(result.observation);

	EXPECT_TRUE(WaitUntil([&] {
		return result.observation->Snapshot().phase
			== EFrameRuntimeRetirementPhase::Completed;
	}));
	const auto snapshot = result.observation->Snapshot();
	EXPECT_EQ(EFrameRuntimeRetirementStatus::Retired, snapshot.status);
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, snapshot.waitStatus);
	EXPECT_EQ(EFrameCoordinatorRuntimeState::Stopped, snapshot.runtimeState);
}

TEST(FrameRuntimeRetirementService, LeaseScopeExitAlwaysTransfersAStartedRuntime)
{
	auto& service = FrameRuntimeRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));

	std::shared_ptr<FrameRuntimeRetirementObservation> observation;
	{
		auto lease = FrameRuntimeRetirementLease::TryCreate();
		ASSERT_TRUE(lease);
		ASSERT_NE(nullptr, lease->Runtime());
		observation = lease->Observation();
	}

	ASSERT_TRUE(observation);
	EXPECT_TRUE(WaitUntil([&] {
		return observation->Snapshot().phase == EFrameRuntimeRetirementPhase::Completed;
	}));
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded,
		observation->Snapshot().waitStatus);
	EXPECT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
}

TEST(FrameRuntimeRetirementService, AStalledReaperDoesNotBlockAnotherSlot)
{
	auto& service = FrameRuntimeRetirementService::Instance();
	ASSERT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));

	struct Gate final {
		std::mutex mutex;
		std::condition_variable wake;
		bool release = false;
		std::atomic<bool> started = false;
	};
	auto gate = std::make_shared<Gate>();
	std::thread stalled([gate] {
		gate->started.store(true, std::memory_order_release);
		std::unique_lock lock(gate->mutex);
		gate->wake.wait(lock, [&] { return gate->release; });
	});
	ASSERT_TRUE(WaitUntil([&] { return gate->started.load(std::memory_order_acquire); }));

	auto stalledReservation = service.TryReserve();
	ASSERT_TRUE(stalledReservation.has_value());
	const auto stalledResult = service.RetireWorker(
		stalled,
		std::move(*stalledReservation),
		std::static_pointer_cast<void>(gate));
	stalledReservation.reset();
	ASSERT_TRUE(stalledResult.Accepted());
	ASSERT_TRUE(stalledResult.observation);
	ASSERT_TRUE(WaitUntil([&] {
		return stalledResult.observation->Snapshot().phase
			== EFrameRuntimeRetirementPhase::Joining;
	}));

	std::atomic<bool> otherCompleted = false;
	std::thread other([&] { otherCompleted.store(true, std::memory_order_release); });
	auto otherReservation = service.TryReserve();
	ASSERT_TRUE(otherReservation.has_value());
	const auto otherResult = service.RetireWorker(other, std::move(*otherReservation));
	otherReservation.reset();
	ASSERT_TRUE(otherResult.Accepted());
	ASSERT_TRUE(otherResult.observation);
	EXPECT_TRUE(WaitUntil([&] {
		return otherCompleted.load(std::memory_order_acquire)
			&& otherResult.observation->Snapshot().phase
			== EFrameRuntimeRetirementPhase::Completed;
	}));
	EXPECT_EQ(EFrameRuntimeRetirementPhase::Joining,
		stalledResult.observation->Snapshot().phase);

	{
		std::lock_guard lock(gate->mutex);
		gate->release = true;
	}
	gate->wake.notify_all();
	EXPECT_TRUE(WaitUntil([&] {
		return stalledResult.observation->Snapshot().phase
			== EFrameRuntimeRetirementPhase::Completed;
	}));
	EXPECT_TRUE(WaitUntil([&] { return service.ReservedOrPendingCount() == 0; }));
}

} // namespace
} // namespace workbench::rendering
