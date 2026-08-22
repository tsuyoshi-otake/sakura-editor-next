/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameCoordinatorRuntime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace workbench::rendering {
namespace {

using namespace std::chrono_literals;

FrameSurfaceRequest RequestFor(
	const FrameSurfaceId surfaceId,
	const std::uint64_t requestId,
	const std::uint64_t lifetimeEpoch = 1,
	const std::uint64_t layoutEpoch = 1,
	const std::uint64_t deviceEpoch = 1 )
{
	return {
		.surfaceId = surfaceId,
		.surfaceLifetimeEpoch = lifetimeEpoch,
		.requestId = requestId,
		.contentGeneration = requestId,
		.layoutEpoch = layoutEpoch,
		.deviceEpoch = deviceEpoch,
		.workClass = EFrameWorkClass::Visible,
		.visible = true,
	};
}

template<class Value, class Poll>
Value WaitForValue( Poll&& poll )
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	for( ;; ) {
		if( auto value = poll() ) return *value;
		if( std::chrono::steady_clock::now() >= deadline ) return {};
		std::this_thread::sleep_for(1ms);
	}
}

FrameWorkTicket TakeCpu( FrameCoordinatorRuntime& runtime )
{
	return WaitForValue<FrameWorkTicket>([&] { return runtime.TakeCpuWork(); });
}

std::shared_ptr<const FrameCommitCohort> TakePublication( FrameCoordinatorRuntime& runtime )
{
	return WaitForValue<std::shared_ptr<const FrameCommitCohort>>([&]()
		-> std::optional<std::shared_ptr<const FrameCommitCohort>> {
		auto value = runtime.TakePublication();
		if( value ) return value;
		return std::nullopt;
	});
}

TEST(FrameCoordinatorRuntime, PublishesThroughPullMailboxesWithoutOwnerCallbacks)
{
	FrameCoordinatorRuntime runtime;
	ASSERT_TRUE(runtime.RegisterSurface(10, 1).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(10, 1)).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(10, 2)).Accepted());

	const auto ticket = TakeCpu(runtime);
	ASSERT_TRUE(ticket.IsValid());
	EXPECT_EQ(2u, ticket.requestId);
	ASSERT_TRUE(runtime.SubmitCpuCompletion(ticket).Accepted());
	const auto publication = TakePublication(runtime);
	ASSERT_TRUE(publication);
	ASSERT_EQ(1u, publication->publications.size());
	EXPECT_EQ(2u, publication->publications.front().ticket.requestId);
	EXPECT_GE(runtime.Telemetry().publishedSurfaces, 1u);

	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorRuntime, SlowConsumerCannotBlockOwnerOrUnboundCpuMailbox)
{
	FrameCoordinatorRuntimeOptions options;
	options.maxControlQueueDepth = 4;
	options.maxCpuWorkQueueDepth = 1;
	FrameCoordinatorRuntime runtime(options);
	ASSERT_TRUE(runtime.RegisterSurface(20, 1).Accepted());
	ASSERT_TRUE(runtime.RegisterSurface(21, 1).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(20, 1)).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(21, 1)).Accepted());

	const auto first = TakeCpu(runtime);
	ASSERT_TRUE(first.IsValid());
	EXPECT_LE(runtime.Snapshot().cpuWorkQueueDepth, 1u);
	const auto started = std::chrono::steady_clock::now();
	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Closed,
		runtime.Request(RequestFor(20, 2)).status);
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
	const auto stopped = runtime.Snapshot();
	EXPECT_EQ(EFrameCoordinatorRuntimeState::Stopped, stopped.state);
	EXPECT_FALSE(stopped.ownerThreadRunning);
	EXPECT_EQ(0u, stopped.controlQueueDepth);
	EXPECT_EQ(0u, stopped.cpuWorkQueueDepth);
	EXPECT_FALSE(stopped.publicationPending);
	EXPECT_FALSE(stopped.tickPending);
	for (const FrameSurfaceId surfaceId : { 20u, 21u }) {
		const auto surface = runtime.SurfaceSnapshot(surfaceId);
		ASSERT_TRUE(surface.has_value());
		EXPECT_EQ(EFrameSurfacePhase::Closed, surface->phase);
		EXPECT_EQ(0u, surface->pendingDepth);
		EXPECT_EQ(0u, surface->activeRequestId);
		EXPECT_FALSE(surface->closeRequested);
	}
}

TEST(FrameCoordinatorRuntime, CloseDiscardsPendingPublicationAndClosesSurface)
{
	FrameCoordinatorRuntime runtime;
	ASSERT_TRUE(runtime.RegisterSurface(22, 1).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(22, 1)).Accepted());
	const auto ticket = TakeCpu(runtime);
	ASSERT_TRUE(ticket.IsValid());
	ASSERT_TRUE(runtime.SubmitCpuCompletion(ticket).Accepted());
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (!runtime.Snapshot().publicationPending
		&& std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
	ASSERT_TRUE(runtime.Snapshot().publicationPending);

	ASSERT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
	EXPECT_FALSE(runtime.Snapshot().publicationPending);
	EXPECT_EQ(nullptr, runtime.TakePublication());
	ASSERT_TRUE(runtime.SurfaceSnapshot(22).has_value());
	EXPECT_EQ(EFrameSurfacePhase::Closed, runtime.SurfaceSnapshot(22)->phase);
}

TEST(FrameCoordinatorRuntime, PublicationMailboxIsLatestOnlyAndBounded)
{
	FrameCoordinatorRuntime runtime;
	ASSERT_TRUE(runtime.RegisterSurface(30, 1).Accepted());
	for( std::uint64_t requestId = 1; requestId <= 2; ++requestId ) {
		ASSERT_TRUE(runtime.Request(RequestFor(30, requestId)).Accepted());
		const auto ticket = TakeCpu(runtime);
		ASSERT_TRUE(ticket.IsValid());
		ASSERT_TRUE(runtime.SubmitCpuCompletion(ticket).Accepted());
		const auto deadline = std::chrono::steady_clock::now() + 2s;
		while( (!runtime.Snapshot().publicationPending
			|| (requestId == 2 && runtime.Snapshot().replacedPublications == 0))
			&& std::chrono::steady_clock::now() < deadline ) std::this_thread::sleep_for(1ms);
	}
	const auto publication = TakePublication(runtime);
	ASSERT_TRUE(publication);
	ASSERT_EQ(1u, publication->publications.size());
	EXPECT_EQ(2u, publication->publications.front().ticket.requestId);
	EXPECT_GE(runtime.Snapshot().replacedPublications, 1u);

	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorRuntime, DeviceResetFencesOldRequestsBeforeRepublish)
{
	FrameCoordinatorRuntime runtime;
	ASSERT_TRUE(runtime.RegisterSurface(40, 1).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(40, 1, 1, 1, 1)).Accepted());
	const auto oldTicket = TakeCpu(runtime);
	ASSERT_TRUE(oldTicket.IsValid());
	ASSERT_TRUE(runtime.ResetDevice(2).Accepted());
	EXPECT_TRUE(runtime.SubmitCpuCompletion(oldTicket).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(40, 2, 1, 1, 2)).Accepted());
	const auto currentTicket = TakeCpu(runtime);
	ASSERT_TRUE(currentTicket.IsValid());
	EXPECT_EQ(2u, currentTicket.deviceEpoch);
	ASSERT_TRUE(runtime.SubmitCpuCompletion(currentTicket).Accepted());
	const auto publication = TakePublication(runtime);
	ASSERT_TRUE(publication);
	EXPECT_EQ(2u, publication->publications.front().ticket.deviceEpoch);

	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorRuntime, WaitRequiresCloseAndTickIsCoalesced)
{
	FrameCoordinatorRuntime runtime;
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Busy, runtime.Wait().status);
	EXPECT_TRUE(runtime.Tick().Accepted());
	const auto second = runtime.Tick();
	EXPECT_TRUE(second.status == EFrameCoordinatorRuntimeStatus::Replaced || second.Accepted());
	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
	EXPECT_EQ(EFrameCoordinatorRuntimeState::Stopped, runtime.Snapshot().state);
	EXPECT_FALSE(runtime.Snapshot().ownerThreadRunning);
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Closed, runtime.BeginClose().status);
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
	EXPECT_EQ(EFrameCoordinatorRuntimeState::Stopped, runtime.Snapshot().state);
}

TEST(FrameCoordinatorRuntime, TracksUiOwnedGdiFallbackOnTheOwnerThread)
{
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.forceSoftware = true;
	FrameCoordinatorRuntime runtime(options);
	FramePresentationSurfaceSpec spec{
		.surfaceId = 50,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 1,
		.layoutEpoch = 1,
		.width = 320,
		.height = 480,
		.visible = true,
	};
	ASSERT_TRUE(runtime.RegisterPresentedSurface(spec).Accepted());
	ASSERT_TRUE(runtime.RecordGdiFallback(FrameGdiSurfaceCommit{
		.surfaceId = 50,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 1,
		.layoutEpoch = 2,
		.requestId = 1,
		.width = 640,
		.height = 480,
		.visible = true,
	}).Accepted());

	const auto snapshot = WaitForValue<FramePresentationSurfaceSnapshot>([&]()
		-> std::optional<FramePresentationSurfaceSnapshot> {
		auto value = runtime.PresentationSurfaceSnapshot(50);
		if (value && value->state == EFramePresentationSurfaceState::GdiFallback) return value;
		return std::nullopt;
	});
	EXPECT_EQ(EFramePresentationSurfaceState::GdiFallback, snapshot.state);
	EXPECT_EQ(640u, snapshot.width);
	EXPECT_EQ(480u, snapshot.height);
	EXPECT_EQ(2u, snapshot.layoutEpoch);
	EXPECT_EQ(1u, snapshot.lastPresentedRequestId);
	EXPECT_TRUE(snapshot.hasLastGoodContent);

	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
	const auto closed = runtime.PresentationSurfaceSnapshot(50);
	ASSERT_TRUE(closed.has_value());
	EXPECT_EQ(EFramePresentationSurfaceState::Closed, closed->state);
	EXPECT_EQ(EFramePresentationOwnerState::Closed,
		runtime.Snapshot().presentationState);
}

TEST(FrameCoordinatorRuntime, NativeSurfacePayloadMailboxIsBoundedLatestWins)
{
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.forceSoftware = true;
	options.presentationOwner.allowHardware = false;
	options.presentationOwner.allowWarp = false;
	options.maxControlQueueDepth = 2;
	FrameCoordinatorRuntime runtime(options);

	ASSERT_TRUE(runtime.RegisterPresentedSurface(FrameNativeSurfaceRegistration{
		.presentation = FramePresentationSurfaceSpec{
			.surfaceId = 501,
			.surfaceLifetimeEpoch = 1,
			.deviceEpoch = 2,
			.layoutEpoch = 1,
			.width = 2,
			.height = 2,
			.visible = true,
		},
	}).Accepted());

	const auto makeFrame = [](const std::uint64_t requestId) {
		return FrameNativeSurfaceFrame{
			.surfaceId = 501,
			.surfaceLifetimeEpoch = 1,
			.deviceEpoch = 2,
			.displayEpoch = 1,
			.layoutEpoch = 1,
			.requestId = requestId,
			.width = 2,
			.height = 2,
			.pitch = 8,
			.pixels = std::make_shared<const std::vector<std::uint8_t>>(
				std::vector<std::uint8_t>(16, static_cast<std::uint8_t>(requestId))),
		};
	};
	EXPECT_TRUE(runtime.SubmitNativeSurfaceFrame(makeFrame(1)).Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Replaced,
		runtime.SubmitNativeSurfaceFrame(makeFrame(2)).status);

	const auto presentation = WaitForValue<FramePresentationSurfaceSnapshot>([&]()
		-> std::optional<FramePresentationSurfaceSnapshot> {
		const auto value = runtime.PresentationSurfaceSnapshot(501);
		if (value && runtime.PresentationTelemetry().presentNotReadySkips >= 1) {
			return value;
		}
		return std::nullopt;
	});
	EXPECT_EQ(EFramePresentationSurfaceState::SoftwareOnly, presentation.state);
	EXPECT_EQ(0u, runtime.PresentationTelemetry().nativeSurfaceUploadCalls);
	EXPECT_EQ(1u, runtime.RuntimeTelemetry().nativePresentAttempts);

	ASSERT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorRuntime, RecoversInjectedDeviceLossOnOwnerThreadAndReprojectsSurfaces)
{
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.allowHardware = false;
	options.presentationOwner.allowWarp = true;
	options.presentationOwner.allowHardwareReprobe = false;
	FrameCoordinatorRuntime runtime(options);

	const auto initialDeviceEpoch = WaitForValue<std::uint64_t>([&]()
		-> std::optional<std::uint64_t> {
		const auto snapshot = runtime.Snapshot();
		if (snapshot.state == EFrameCoordinatorRuntimeState::Running
			&& snapshot.presentationState == EFramePresentationOwnerState::WarpReady
			&& snapshot.presentationDeviceEpoch != 0) {
			return snapshot.presentationDeviceEpoch;
		}
		return std::nullopt;
	});
	ASSERT_NE(0u, initialDeviceEpoch);

	ASSERT_TRUE(runtime.RegisterPresentedSurface({
		.surfaceId = 60,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = initialDeviceEpoch,
		.layoutEpoch = 1,
		.width = 800,
		.height = 600,
		.visible = true,
	}).Accepted());
	ASSERT_TRUE(runtime.RecordGdiFallback({
		.surfaceId = 60,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = initialDeviceEpoch,
		.layoutEpoch = 2,
		.requestId = 1,
		.width = 800,
		.height = 600,
		.visible = true,
	}).Accepted());
	(void)WaitForValue<FramePresentationSurfaceSnapshot>([&]()
		-> std::optional<FramePresentationSurfaceSnapshot> {
		auto value = runtime.PresentationSurfaceSnapshot(60);
		if (value && value->state == EFramePresentationSurfaceState::GdiFallback) {
			return value;
		}
		return std::nullopt;
	});

	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Invalid,
		runtime.InjectPresentationFailure(
			static_cast<EFrameDeviceFailureBoundary>(255), -1).status);
	ASSERT_TRUE(runtime.InjectPresentationFailure(
		EFrameDeviceFailureBoundary::Present,
		static_cast<long>(DXGI_ERROR_DEVICE_REMOVED)).Accepted());

	const auto recoveredDeviceEpoch = WaitForValue<std::uint64_t>([&]()
		-> std::optional<std::uint64_t> {
		const auto snapshot = runtime.Snapshot();
		if (snapshot.processedDeviceFailures == 1
			&& snapshot.presentationDeviceEpoch > initialDeviceEpoch) {
			return snapshot.presentationDeviceEpoch;
		}
		return std::nullopt;
	});
	ASSERT_GT(recoveredDeviceEpoch, initialDeviceEpoch);
	const auto surface = runtime.PresentationSurfaceSnapshot(60);
	ASSERT_TRUE(surface.has_value());
	EXPECT_EQ(recoveredDeviceEpoch, surface->deviceEpoch);
	EXPECT_EQ(EFramePresentationSurfaceState::GdiFallback, surface->state);
	EXPECT_EQ(1u, surface->lastPresentedRequestId);
	EXPECT_TRUE(surface->hasLastGoodContent);
	EXPECT_GE(runtime.PresentationTelemetry().deviceLossRecoveries, 2u);
	EXPECT_GE(runtime.PresentationDeviceTelemetry().lossDetections, 2u);
	EXPECT_EQ(1u, runtime.Telemetry().deviceResets);

	EXPECT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

} // namespace
} // namespace workbench::rendering
