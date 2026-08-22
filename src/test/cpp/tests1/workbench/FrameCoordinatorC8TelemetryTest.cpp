/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameBackpressure.h"
#include "workbench/rendering/FrameCadence.h"
#include "workbench/rendering/FrameCadenceSource.h"
#include "workbench/rendering/FrameCoordinatorModel.h"
#include "workbench/rendering/FrameCoordinatorRuntime.h"
#include "workbench/rendering/FrameFaultModel.h"
#include "workbench/rendering/FrameRuntimeTelemetry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <thread>

namespace workbench::rendering {
namespace {

using namespace std::chrono_literals;

FrameSurfaceRequest RequestFor(const FrameSurfaceId surfaceId,
	const std::uint64_t requestId, const EFrameWorkClass workClass,
	const bool visible = true)
{
	return {
		.surfaceId = surfaceId,
		.surfaceLifetimeEpoch = 1,
		.requestId = requestId,
		.contentGeneration = requestId,
		.layoutEpoch = 1,
		.deviceEpoch = 1,
		.workClass = workClass,
		.visible = visible,
	};
}

template<class Value, class Poll>
Value WaitForValue(Poll&& poll)
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	for (;;) {
		if (auto value = poll()) return *value;
		if (std::chrono::steady_clock::now() >= deadline) return {};
		std::this_thread::sleep_for(1ms);
	}
}

std::shared_ptr<const FrameCommitCohort> TakePublication(
	FrameCoordinatorRuntime& runtime)
{
	return WaitForValue<std::shared_ptr<const FrameCommitCohort>>([&]()
		-> std::optional<std::shared_ptr<const FrameCommitCohort>> {
		auto publication = runtime.TakePublication();
		if (publication != nullptr) return publication;
		return std::nullopt;
	});
}

void WriteProductionC8Evidence(
	const FrameCadenceObservation& observation,
	const FrameCadenceResult& cadence,
	const FrameCoordinatorRuntimeSnapshot& initial,
	const FrameCoordinatorRuntimeSnapshot& idle,
	const FrameRuntimeTelemetrySnapshot& telemetry)
{
	const char* path = std::getenv("SAKURA_C8_TELEMETRY_OUTPUT");
	if (path == nullptr || *path == '\0') return;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return;
	const auto refreshInterval = std::max<std::int64_t>(
		cadence.refreshInterval.count(), 1);
	const auto p95Visible = telemetry.requestToPublicationLatency
		.PercentileUpperBound(95);
	const auto p99Visible = telemetry.requestToPublicationLatency
		.PercentileUpperBound(99);
	const auto intervalCount = [refreshInterval, &telemetry](const std::uint64_t value) {
		if (telemetry.requestToPublicationLatency.samples == 0) return std::uint64_t{ 0 };
		return (value + static_cast<std::uint64_t>(refreshInterval) - 1)
			/ static_cast<std::uint64_t>(refreshInterval);
	};
	output << "{\n"
		<< "  \"schemaVersion\": 1,\n"
		<< "  \"productionRuntime\": true,\n"
		<< "  \"cadenceSource\": {\n"
		<< "    \"monitorObserved\": " << (observation.monitorObserved ? "true" : "false") << ",\n"
		<< "    \"displayRateObserved\": " << (observation.displayRateObserved ? "true" : "false") << ",\n"
		<< "    \"compositorRateObserved\": " << (observation.compositorRateObserved ? "true" : "false") << ",\n"
		<< "    \"displayRefreshRateHz\": " << observation.input.displayRefreshRateHz << ",\n"
		<< "    \"compositorRefreshRateHz\": " << observation.input.compositorRefreshRateHz << ",\n"
		<< "    \"displayEpoch\": " << observation.input.displayEpoch << ",\n"
		<< "    \"observedQpc\": " << observation.observedQpc << "\n"
		<< "  },\n"
		<< "  \"cadence\": {\n"
		<< "    \"effectiveRefreshRateHz\": " << cadence.effectiveRefreshRateHz << ",\n"
		<< "    \"refreshIntervalMicroseconds\": " << cadence.refreshInterval.count() << "\n"
		<< "  },\n"
		<< "  \"cleanIdle\": {\n"
		<< "    \"seconds\": 10,\n"
		<< "    \"requestedTickCountBefore\": " << initial.requestedTickCount << ",\n"
		<< "    \"requestedTickCountAfter\": " << idle.requestedTickCount << ",\n"
		<< "    \"processedTickCountBefore\": " << initial.processedTickCount << ",\n"
		<< "    \"processedTickCountAfter\": " << idle.processedTickCount << "\n"
		<< "  },\n"
		<< "  \"controlQueueDepth\": " << idle.controlQueueDepth << ",\n"
		<< "  \"maxControlQueueDepth\": "
		<< telemetry.mailboxes[static_cast<std::size_t>(EFrameTelemetryMailbox::Control)].capacity << ",\n"
		<< "  \"uiHandlerP99Milliseconds\": "
		<< telemetry.uiHandlerDuration.PercentileUpperBound(99) / 1000.0 << ",\n"
		<< "  \"uiHandlerMaximumMilliseconds\": "
		<< telemetry.uiHandlerDuration.maximumMicroseconds / 1000.0 << ",\n"
		<< "  \"lockP99Microseconds\": "
		<< telemetry.lockWaitDuration.PercentileUpperBound(99) << ",\n"
		<< "  \"lockMaximumMicroseconds\": "
		<< telemetry.lockWaitDuration.maximumMicroseconds << ",\n"
		<< "  \"inputToVisibleP95Intervals\": " << intervalCount(p95Visible) << ",\n"
		<< "  \"inputToVisibleP99Intervals\": " << intervalCount(p99Visible) << ",\n"
		<< "  \"inputToVisibleDefinition\": \"request-to-publication; physical visibility is not captured by this software fallback trial\",\n"
		<< "  \"physicalVisibilityCaptured\": false,\n"
		<< "  \"inputToPublicationSamples\": "
		<< telemetry.requestToPublicationLatency.samples << ",\n"
		<< "  \"inputToPublicationMaximumMicroseconds\": "
		<< telemetry.requestToPublicationLatency.maximumMicroseconds << ",\n"
		<< "  \"nativePresentAttempts\": " << telemetry.nativePresentAttempts << ",\n"
		<< "  \"nativePresentBackpressureSkips\": " << telemetry.nativePresentBackpressureSkips << ",\n"
		<< "  \"readbackObservations\": " << telemetry.readbackObservations << ",\n"
		<< "  \"readbackCompletions\": " << telemetry.readbackCompletions << "\n"
		<< "}\n";
}

TEST(FrameCoordinatorC8Telemetry, StalledSurfaceDoesNotBlockReadySurface)
{
	FrameCoordinatorModel model;
	ASSERT_TRUE(model.RegisterSurface(1, 1).Accepted());
	ASSERT_TRUE(model.RegisterSurface(2, 1).Accepted());
	ASSERT_TRUE(model.Request(RequestFor(1, 1, EFrameWorkClass::Interactive)).Accepted());
	ASSERT_TRUE(model.Request(RequestFor(2, 1, EFrameWorkClass::Visible)).Accepted());

	const auto editor = model.TakeNextCpuWork();
	ASSERT_TRUE(editor.has_value());
	ASSERT_EQ(1u, editor->surfaceId);
	ASSERT_TRUE(model.CompleteCpu(*editor).Accepted());
	ASSERT_TRUE(model.QueueGpu(*editor).Accepted());
	ASSERT_TRUE(model.BeginGpu(*editor).Accepted());
	ASSERT_TRUE(model.CompleteGpu(*editor).Accepted());
	const auto stalled = model.TakeNextCpuWork();
	ASSERT_TRUE(stalled.has_value());
	ASSERT_EQ(2u, stalled->surfaceId);

	const auto cohort = model.AssembleCommit(1, 1, 1);
	ASSERT_EQ(1u, cohort.publications.size());
	EXPECT_EQ(1u, cohort.publications.front().ticket.surfaceId);
	ASSERT_EQ(1u, cohort.lateSurfaces.size());
	EXPECT_EQ(2u, cohort.lateSurfaces.front().surfaceId);
}

TEST(FrameCoordinatorC8Telemetry, GeneralSaturationLeavesReservedEditorProgress)
{
	FrameBackpressureController controller({ .generalCapacity = 2,
		.reservedEditorCapacity = 1 });
	ASSERT_TRUE(controller.RegisterSurface(10, false).Accepted());
	ASSERT_TRUE(controller.RegisterSurface(11, false).Accepted());
	ASSERT_TRUE(controller.RegisterSurface(13, false).Accepted());
	ASSERT_TRUE(controller.RegisterSurface(12, true).Accepted());
	EXPECT_TRUE(controller.Submit(10, 1).Accepted());
	EXPECT_TRUE(controller.Submit(11, 1).Accepted());
	EXPECT_EQ(EFrameBackpressureStatus::SkippedSaturated,
		controller.Submit(13, 1).status);
	EXPECT_TRUE(controller.Submit(12, 1).Accepted());

	const auto snapshot = controller.Snapshot();
	EXPECT_EQ(2u, snapshot.generalDepth);
	EXPECT_EQ(1u, snapshot.editorDepth);
	EXPECT_EQ(1u, snapshot.saturationEvents);
	EXPECT_EQ(EFrameBackpressureStatus::Presented,
		controller.TryPresent(12, true).status);
	EXPECT_EQ(2u, controller.Snapshot().generalDepth);
}

TEST(FrameCoordinatorC8Telemetry, PresentBackpressureSkipsWithoutBusyRetry)
{
	FrameBackpressureController controller({ .generalCapacity = 2,
		.reservedEditorCapacity = 1 });
	ASSERT_TRUE(controller.RegisterSurface(20, true).Accepted());
	ASSERT_TRUE(controller.Submit(20, 1).Accepted());
	EXPECT_EQ(EFrameBackpressureStatus::SkippedBackpressure,
		controller.TryPresent(20, false).status);
	const auto skipped = controller.Snapshot();
	ASSERT_EQ(1u, skipped.surfaceCount);
	EXPECT_TRUE(skipped.surfaces.front().pending);
	EXPECT_EQ(1u, skipped.backpressureSkips);

	EXPECT_EQ(EFrameBackpressureStatus::Replaced,
		controller.Submit(20, 2).status);
	EXPECT_EQ(EFrameBackpressureStatus::Presented,
		controller.TryPresent(20, true).status);
	const auto presented = controller.Snapshot();
	EXPECT_EQ(2u, presented.surfaces.front().presentedRequestId);
	EXPECT_FALSE(presented.surfaces.front().pending);
}

TEST(FrameCoordinatorC8Telemetry, CadenceUsesRefreshRateAndDoesNotBusyRetry)
{
	const auto sixty = FrameCadence::Calculate({ .displayRefreshRateHz = 60 });
	const auto oneTwenty = FrameCadence::Calculate({ .displayRefreshRateHz = 120 });
	const auto oneFortyFour = FrameCadence::Calculate({ .displayRefreshRateHz = 144 });
	const auto mixed = FrameCadence::Calculate({ .displayRefreshRateHz = 60,
		.compositorRefreshRateHz = 144 });
	EXPECT_EQ(16667, sixty.refreshInterval.count());
	EXPECT_EQ(8334, oneTwenty.refreshInterval.count());
	EXPECT_EQ(6945, oneFortyFour.refreshInterval.count());
	EXPECT_EQ(144u, mixed.effectiveRefreshRateHz);
	EXPECT_TRUE(FrameCadence::IsDue(oneTwenty.refreshInterval, oneTwenty.refreshInterval));
	EXPECT_EQ(std::chrono::microseconds(8332),
		FrameCadence::NextInterval(std::chrono::microseconds(2), oneTwenty.refreshInterval));
	EXPECT_EQ(std::chrono::microseconds(1),
		FrameCadence::NextInterval(std::chrono::microseconds(1), {}));
}

TEST(FrameCoordinatorC8Telemetry, TelemetryKeepsBoundedLatencyAndSurfaceProgress)
{
	FrameRuntimeTelemetry telemetry;
	telemetry.RecordRequest(30, 1, 1000);
	telemetry.RecordPublication(30, 1, 3000);
	telemetry.RecordRequest(31, 1, 1000);
	telemetry.RecordSkippedPresentation(31, true);
	telemetry.RecordMailboxAccepted(EFrameTelemetryMailbox::Control, 2, 2, true);
	telemetry.RecordMailboxSaturation(EFrameTelemetryMailbox::Control, 2, 2);
	telemetry.RecordUiHandlerDuration(1900);
	telemetry.RecordLockWaitDuration(70);

	const auto snapshot = telemetry.Snapshot();
	EXPECT_EQ(2u, snapshot.requestCount);
	EXPECT_EQ(1u, snapshot.publicationCount);
	EXPECT_EQ(1u, snapshot.skippedPresentations);
	EXPECT_EQ(1u, snapshot.backpressureEvents);
	EXPECT_EQ(1u, snapshot.mailboxSaturationEvents);
	EXPECT_EQ(2000u, snapshot.requestToPublicationLatency.PercentileUpperBound(95));
	EXPECT_EQ(2000u, snapshot.uiHandlerDuration.PercentileUpperBound(99));
	EXPECT_EQ(100u, snapshot.lockWaitDuration.PercentileUpperBound(99));
	ASSERT_EQ(2u, snapshot.surfaceCount);
	for (const auto& surface : snapshot.surfaces) {
		if (surface.surfaceId == 30) {
			EXPECT_EQ(1u, surface.publishedFrames);
			EXPECT_EQ(1u, surface.latestPublishedRequestId);
		}
		if (surface.surfaceId == 31) {
			EXPECT_EQ(1u, surface.skippedPresentations);
			EXPECT_EQ(1u, surface.backpressureEvents);
		}
	}
}

TEST(FrameCoordinatorC8Telemetry, DeviceLossFaultHasExplicitRecoveryAndClose)
{
	FrameFaultModel fault(7);
	EXPECT_EQ(EFrameFaultStatus::Accepted,
		fault.Inject(EFrameFaultBoundary::Present, -1).status);
	EXPECT_EQ(EFrameFaultState::DeviceLost, fault.Snapshot().state);
	EXPECT_EQ(EFrameFaultStatus::Accepted, fault.Recover(false, false).status);
	EXPECT_EQ(EFrameFaultState::SoftwareOnly, fault.Snapshot().state);
	EXPECT_EQ(8u, fault.Snapshot().deviceEpoch);
	EXPECT_EQ(EFrameFaultStatus::AlreadyApplied,
		fault.Recover(false, false).status);
	EXPECT_TRUE(fault.Close().Accepted());
	EXPECT_EQ(EFrameFaultState::Closed, fault.Snapshot().state);
	EXPECT_EQ(EFrameFaultStatus::Closed,
		fault.Inject(EFrameFaultBoundary::Resize, -2).status);
}

TEST(FrameCoordinatorC8Telemetry,
	RuntimeWiresRefreshCadenceAndLatestOnlyPublication)
{
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.forceSoftware = true;
	options.cadence.displayRefreshRateHz = 120;
	options.backpressure.generalCapacity = 1;
	options.backpressure.reservedEditorCapacity = 1;
	FrameCoordinatorRuntime runtime(options);

	const auto cadence = runtime.Cadence();
	EXPECT_TRUE(cadence.valid);
	EXPECT_EQ(120u, cadence.effectiveRefreshRateHz);
	EXPECT_EQ(8334, cadence.refreshInterval.count());
	EXPECT_EQ(120u, runtime.RuntimeTelemetry().effectiveRefreshRateHz);

	ASSERT_TRUE(runtime.RegisterSurface(90, 1).Accepted());
	ASSERT_TRUE(runtime.Request(RequestFor(90, 1, EFrameWorkClass::Visible)).Accepted());
	const auto firstTicket = WaitForValue<std::optional<FrameWorkTicket>>([&]() {
		return runtime.TakeCpuWork();
	});
	ASSERT_TRUE(firstTicket.has_value());
	ASSERT_TRUE(runtime.SubmitCpuCompletion(*firstTicket).Accepted());
	const auto firstPublicationReady = WaitForValue<std::optional<bool>>([&]()
		-> std::optional<bool> {
		if (runtime.Snapshot().publicationPending) return true;
		return std::nullopt;
	});
	ASSERT_TRUE(firstPublicationReady.has_value());

	ASSERT_TRUE(runtime.Request(RequestFor(90, 2, EFrameWorkClass::Visible)).Accepted());
	const auto secondTicket = WaitForValue<std::optional<FrameWorkTicket>>([&]() {
		return runtime.TakeCpuWork();
	});
	ASSERT_TRUE(secondTicket.has_value());
	ASSERT_TRUE(runtime.SubmitCpuCompletion(*secondTicket).Accepted());

	const auto replacement = WaitForValue<std::optional<bool>>([&]()
		-> std::optional<bool> {
		if (runtime.Snapshot().replacedPublications != 0) return true;
		return std::nullopt;
	});
	ASSERT_TRUE(replacement.has_value());

	// The CPU publication mailbox is depth one and latest-only. It is not a
	// substitute for swap-chain readiness, which the presentation owner checks
	// independently without waiting or busy retrying.
	const auto latestPublication = TakePublication(runtime);
	ASSERT_TRUE(latestPublication);
	ASSERT_EQ(2u, latestPublication->publications.front().ticket.requestId);

	ASSERT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorC8Telemetry,
	RuntimeConsumesExplicitNativeReadinessCadenceAndReadbackBoundaries)
{
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.forceSoftware = true;
	options.cadence.displayEpoch = 1;
	FrameCoordinatorRuntime runtime(options);

	ASSERT_TRUE(runtime.RegisterPresentedSurface(FramePresentationSurfaceSpec{
		.surfaceId = 91,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 2,
		.layoutEpoch = 1,
		.width = 320,
		.height = 240,
		.visible = true,
	}).Accepted());
	ASSERT_TRUE(runtime.UpdateCadence(FrameCadenceInput{
		.displayEpoch = 2,
		.displayRefreshRateHz = 120,
	}).Accepted());
	ASSERT_TRUE(runtime.RecordGdiFallback(FrameGdiSurfaceCommit{
		.surfaceId = 91,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 2,
		.layoutEpoch = 2,
		.requestId = 1,
		.width = 320,
		.height = 240,
		.visible = true,
	}).Accepted());
	ASSERT_TRUE(runtime.PresentSurface(FrameNativePresentationRequest{
		.surfaceId = 91,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 2,
		.displayEpoch = 2,
		.layoutEpoch = 2,
		.requestId = 2,
	}).Accepted());
	ASSERT_TRUE(runtime.RecordReadbackObservation(FrameReadbackObservation{
		.surfaceId = 91,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 2,
		.displayEpoch = 2,
		.layoutEpoch = 2,
		.requestId = 2,
		.width = 320,
		.height = 240,
		.byteCount = 320u * 240u * 4u,
		.completed = true,
		.matchedLastGood = true,
	}).Accepted());

	const auto telemetry = WaitForValue<FrameRuntimeTelemetrySnapshot>([&]()
		-> std::optional<FrameRuntimeTelemetrySnapshot> {
		const auto value = runtime.RuntimeTelemetry();
		if (value.nativePresentAttempts >= 1
			&& value.nativePresentNotReadySkips >= 1
			&& value.readbackObservations >= 1) return value;
		return std::nullopt;
	});
	const auto runtimeSnapshot = runtime.Snapshot();
	EXPECT_NE(EFrameCoordinatorRuntimeState::Failed, runtimeSnapshot.state);
	EXPECT_GT(runtime.RuntimeTelemetry().mailboxes[
		static_cast<std::size_t>(EFrameTelemetryMailbox::Control)].accepted, 0u);
	const auto presentationSnapshot = runtime.PresentationSurfaceSnapshot(91);
	ASSERT_TRUE(presentationSnapshot.has_value());
	EXPECT_EQ(2u, presentationSnapshot->deviceEpoch);
	EXPECT_EQ(2u, presentationSnapshot->layoutEpoch);
	EXPECT_EQ(1u, presentationSnapshot->lastPresentedRequestId);
	EXPECT_EQ(1u, runtime.PresentationTelemetry().presentNotReadySkips);
	EXPECT_EQ(2u, telemetry.displayEpoch);
	EXPECT_EQ(1u, telemetry.nativePresentAttempts);
	EXPECT_EQ(1u, telemetry.nativePresentNotReadySkips);
	EXPECT_EQ(1u, telemetry.readbackObservations);
	EXPECT_EQ(1u, telemetry.readbackCompletions);
	EXPECT_EQ(320u * 240u * 4u, telemetry.readbackBytes);

	ASSERT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

TEST(FrameCoordinatorC8Telemetry,
	ProductionRuntimeCleanIdleTenSecondsHasNoImplicitTimer)
{
	// Use the same event-source observation that the window integration supplies
	// instead of inventing a refresh rate in this production-runtime trial.
	FrameCadenceSource cadenceSource;
	const auto cadenceObservation = cadenceSource.Observe(::GetDesktopWindow());
	FrameCoordinatorRuntimeOptions options;
	options.presentationOwner.forceSoftware = true;
	options.presentationOwner.allowHardware = false;
	options.presentationOwner.allowWarp = false;
	options.cadence = cadenceObservation.input;
	FrameCoordinatorRuntime runtime(options);

	const auto running = WaitForValue<FrameCoordinatorRuntimeSnapshot>([&]()
		-> std::optional<FrameCoordinatorRuntimeSnapshot> {
		const auto snapshot = runtime.Snapshot();
		if (snapshot.state == EFrameCoordinatorRuntimeState::Running) return snapshot;
		return std::nullopt;
	});
	ASSERT_EQ(EFrameCoordinatorRuntimeState::Running, running.state);

	// Produce one real request/completion/publication sample before the idle
	// window. This keeps latency evidence honest: a zero-sample histogram must
	// never be reported as a one-refresh input-to-visible interval.
	ASSERT_TRUE(runtime.RegisterSurface(901, 1).Accepted());
	ASSERT_TRUE(runtime.Request(
		RequestFor(901, 1, EFrameWorkClass::Visible)).Accepted());
	const auto ticket = WaitForValue<FrameWorkTicket>([&]()
		-> std::optional<FrameWorkTicket> {
		auto value = runtime.TakeCpuWork();
		if (value.has_value()) return value;
		return std::nullopt;
	});
	ASSERT_TRUE(ticket.IsValid());
	ASSERT_TRUE(runtime.SubmitCpuCompletion(ticket).Accepted());
	ASSERT_TRUE(TakePublication(runtime) != nullptr);

	const auto initialCadence = runtime.Cadence();
	std::uint64_t lastSettledProcessedTickCount = (std::numeric_limits<std::uint64_t>::max)();
	std::uint64_t lastSettledRequestedTickCount = (std::numeric_limits<std::uint64_t>::max)();
	unsigned stablePolls = 0;
	const auto initial = WaitForValue<FrameCoordinatorRuntimeSnapshot>([&]()
		-> std::optional<FrameCoordinatorRuntimeSnapshot> {
		const auto snapshot = runtime.Snapshot();
		const auto telemetry = runtime.RuntimeTelemetry();
		const auto surface = runtime.SurfaceSnapshot(901);
		if (snapshot.processedTickCount != lastSettledProcessedTickCount
			|| snapshot.requestedTickCount != lastSettledRequestedTickCount) {
			lastSettledProcessedTickCount = snapshot.processedTickCount;
			lastSettledRequestedTickCount = snapshot.requestedTickCount;
			stablePolls = 0;
			return std::nullopt;
		}
		if (!snapshot.tickPending
			&& snapshot.controlQueueDepth == 0
			&& snapshot.cpuWorkQueueDepth == 0
			&& !snapshot.publicationPending
			&& snapshot.processedTickCount != 0
			&& telemetry.publicationCount >= 1
			&& surface.has_value()
			&& surface->publishedRequestId == 1) {
			if (++stablePolls < 5) return std::nullopt;
			return snapshot;
		}
		stablePolls = 0;
		return std::nullopt;
	});
	ASSERT_GT(initial.processedTickCount, 0u);
	const auto initialTelemetry = runtime.RuntimeTelemetry();

	// A production runtime is event-driven. Ten seconds with no input, cadence,
	// publication, or compositor wake must not synthesize a retry tick.
	std::this_thread::sleep_for(10s);
	const auto idle = runtime.Snapshot();
	const auto idleTelemetry = runtime.RuntimeTelemetry();
	EXPECT_EQ(initial.requestedTickCount, idle.requestedTickCount);
	EXPECT_EQ(initial.processedTickCount, idle.processedTickCount);
	EXPECT_EQ(initial.controlQueueDepth, idle.controlQueueDepth);
	EXPECT_EQ(initial.cpuWorkQueueDepth, idle.cpuWorkQueueDepth);
	EXPECT_EQ(initialTelemetry.nativePresentAttempts,
		idleTelemetry.nativePresentAttempts);
	EXPECT_EQ(initialTelemetry.readbackObservations,
		idleTelemetry.readbackObservations);
	EXPECT_EQ(initialCadence.displayEpoch, idleTelemetry.displayEpoch);
	EXPECT_GT(initialCadence.effectiveRefreshRateHz, 0u);
	EXPECT_GT(initialCadence.refreshInterval.count(), 0);
	WriteProductionC8Evidence(
		cadenceObservation,
		initialCadence,
		initial,
		idle,
		idleTelemetry);

	ASSERT_TRUE(runtime.BeginClose().Accepted());
	EXPECT_EQ(EFrameCoordinatorRuntimeStatus::Succeeded, runtime.Wait().status);
}

} // namespace
} // namespace workbench::rendering
