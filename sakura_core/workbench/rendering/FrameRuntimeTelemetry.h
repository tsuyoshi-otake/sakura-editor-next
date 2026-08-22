/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCoordinatorModel.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace workbench::rendering {

//! Fixed bins keep telemetry bounded and make percentile reporting stable.
constexpr std::size_t kFrameTelemetryHistogramBinCount = 16;
constexpr std::size_t kFrameTelemetrySurfaceSlotCount = 64;

enum class EFrameTelemetryMailbox : std::uint8_t {
	Control,
	CpuWork,
	Publication,
	Surface,
	Count,
};

//! Outcome observed at the native Present1(DXGI_PRESENT_DO_NOT_WAIT)
//! boundary.  The values describe the boundary result, not a scheduler
//! decision made by the CPU publication mailbox.
enum class EFrameNativePresentOutcome : std::uint8_t {
	Presented,
	Backpressured,
	NotReady,
	DeviceLost,
	Failed,
	SoftwareFallback,
};

//! A nonblocking readback observation supplied by a native/test adapter.
//!
//! The coordinator never performs a synchronous GPU readback.  An adapter may
//! report a completed staging/readback result (or the software/GDI commit
//! boundary) through this value, which keeps readback evidence epoch-fenced
//! without adding a wait to the presentation owner.
struct FrameReadbackObservation final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t displayEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint64_t byteCount = 0;
	bool completed = false;
	bool matchedLastGood = false;
};

struct FrameTelemetryHistogramSnapshot final {
	std::array<std::uint64_t, kFrameTelemetryHistogramBinCount> bins{};
	std::uint64_t samples = 0;
	std::uint64_t totalMicroseconds = 0;
	std::uint64_t maximumMicroseconds = 0;

	[[nodiscard]] std::uint64_t PercentileUpperBound(
		std::uint32_t percentile) const noexcept;
};

struct FrameMailboxTelemetrySnapshot final {
	std::uint64_t accepted = 0;
	std::uint64_t replaced = 0;
	std::uint64_t saturationEvents = 0;
	std::uint64_t maximumDepth = 0;
	std::uint64_t capacity = 0;
};

struct FrameSurfaceTelemetrySnapshot final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t acceptedRequests = 0;
	std::uint64_t publishedFrames = 0;
	std::uint64_t skippedPresentations = 0;
	std::uint64_t backpressureEvents = 0;
	std::uint64_t latestRequestId = 0;
	std::uint64_t latestPublishedRequestId = 0;
	std::uint64_t latestRequestTimestampMicroseconds = 0;
	std::uint64_t latestPublicationTimestampMicroseconds = 0;
	std::uint64_t nativePresentCount = 0;
	std::uint64_t nativePresentBackpressureEvents = 0;
	std::uint64_t latestNativePresentRequestId = 0;
	std::uint64_t latestNativePresentDurationMicroseconds = 0;
	std::uint64_t readbackObservations = 0;
	std::uint64_t readbackCompletions = 0;
	std::uint64_t latestReadbackRequestId = 0;
};

struct FrameRuntimeTelemetrySnapshot final {
	std::uint64_t requestCount = 0;
	std::uint64_t publicationCount = 0;
	std::uint64_t skippedPresentations = 0;
	std::uint64_t backpressureEvents = 0;
	std::uint64_t mailboxSaturationEvents = 0;
	std::uint64_t surfaceSlotSaturationEvents = 0;
	std::uint64_t displayEpoch = 0;
	std::uint32_t effectiveRefreshRateHz = 0;
	std::uint64_t refreshIntervalMicroseconds = 0;
	std::uint64_t nativePresentAttempts = 0;
	std::uint64_t nativePresentSuccesses = 0;
	std::uint64_t nativePresentBackpressureSkips = 0;
	std::uint64_t nativePresentNotReadySkips = 0;
	std::uint64_t nativePresentDeviceLosses = 0;
	std::uint64_t nativePresentFailures = 0;
	std::int64_t nativePresentLastResult = 0;
	std::uint64_t nativePresentLastDurationMicroseconds = 0;
	std::uint64_t nativePresentMaximumDurationMicroseconds = 0;
	std::uint64_t readbackObservations = 0;
	std::uint64_t readbackCompletions = 0;
	std::uint64_t readbackBytes = 0;
	std::uint64_t lastReadbackRequestId = 0;
	FrameTelemetryHistogramSnapshot requestToPublicationLatency;
	FrameTelemetryHistogramSnapshot uiHandlerDuration;
	FrameTelemetryHistogramSnapshot lockWaitDuration;
	FrameTelemetryHistogramSnapshot nativePresentDuration;
	std::array<FrameMailboxTelemetrySnapshot,
		static_cast<std::size_t>(EFrameTelemetryMailbox::Count)> mailboxes{};
	std::array<FrameSurfaceTelemetrySnapshot, kFrameTelemetrySurfaceSlotCount> surfaces{};
	std::size_t surfaceCount = 0;
};

//! Lock-free, fixed-capacity observations for the frame owner boundary.
//!
//! The producer paths only perform bounded atomic operations. There are no
//! callbacks, allocations, mutexes, waits, or unbounded sample buffers. A
//! surface id that cannot fit in the fixed table is counted globally and does
//! not affect the progress of surfaces already admitted.
class FrameRuntimeTelemetry final {
public:
	FrameRuntimeTelemetry() noexcept;

	FrameRuntimeTelemetry(const FrameRuntimeTelemetry&) = delete;
	FrameRuntimeTelemetry& operator=(const FrameRuntimeTelemetry&) = delete;

	static std::uint64_t NowMicroseconds() noexcept;

	void RecordRequest(FrameSurfaceId surfaceId, std::uint64_t requestId,
		std::uint64_t timestampMicroseconds = 0) noexcept;
	void RecordPublication(FrameSurfaceId surfaceId, std::uint64_t requestId,
		std::uint64_t timestampMicroseconds = 0) noexcept;
	void RecordSkippedPresentation(FrameSurfaceId surfaceId = 0,
		bool backpressured = false) noexcept;
	void RecordMailboxAccepted(EFrameTelemetryMailbox mailbox,
		std::uint64_t depth, std::uint64_t capacity,
		bool replaced = false) noexcept;
	void RecordMailboxSaturation(EFrameTelemetryMailbox mailbox,
		std::uint64_t depth, std::uint64_t capacity) noexcept;
	void RecordUiHandlerDuration(std::uint64_t durationMicroseconds) noexcept;
	void RecordLockWaitDuration(std::uint64_t durationMicroseconds) noexcept;
	//! Records one actual owner-thread Present1 result. This is separate from
	//! publication mailbox acceptance and is the only source of compositor
	//! backpressure telemetry.
	void RecordNativePresent(EFrameNativePresentOutcome outcome,
		std::int64_t resultCode, std::uint64_t durationMicroseconds,
		FrameSurfaceId surfaceId = 0, std::uint64_t requestId = 0) noexcept;
	//! Records a completed nonblocking readback/software observation. No GPU
	//! wait or copy is performed by this method.
	void RecordReadbackObservation(
		const FrameReadbackObservation& observation) noexcept;
	//! Stores the event-source cadence selected by the owner. This is a
	//! configuration observation only; it never creates a timer or waits.
	void ConfigureCadence(std::uint32_t effectiveRefreshRateHz,
		std::uint64_t refreshIntervalMicroseconds,
		std::uint64_t displayEpoch = 0) noexcept;

	[[nodiscard]] FrameRuntimeTelemetrySnapshot Snapshot() const noexcept;

private:
	struct AtomicHistogram final {
		std::array<std::atomic<std::uint64_t>, kFrameTelemetryHistogramBinCount> bins{};
		std::atomic<std::uint64_t> samples{ 0 };
		std::atomic<std::uint64_t> totalMicroseconds{ 0 };
		std::atomic<std::uint64_t> maximumMicroseconds{ 0 };
	};

	struct AtomicMailbox final {
		std::atomic<std::uint64_t> accepted{ 0 };
		std::atomic<std::uint64_t> replaced{ 0 };
		std::atomic<std::uint64_t> saturationEvents{ 0 };
		std::atomic<std::uint64_t> maximumDepth{ 0 };
		std::atomic<std::uint64_t> capacity{ 0 };
	};

	struct AtomicSurface final {
		std::atomic<FrameSurfaceId> surfaceId{ 0 };
		std::atomic<std::uint64_t> acceptedRequests{ 0 };
		std::atomic<std::uint64_t> publishedFrames{ 0 };
		std::atomic<std::uint64_t> skippedPresentations{ 0 };
		std::atomic<std::uint64_t> backpressureEvents{ 0 };
		std::atomic<std::uint64_t> latestRequestId{ 0 };
		std::atomic<std::uint64_t> latestPublishedRequestId{ 0 };
		std::atomic<std::uint64_t> latestRequestTimestampMicroseconds{ 0 };
		std::atomic<std::uint64_t> latestPublicationTimestampMicroseconds{ 0 };
		std::atomic<std::uint64_t> nativePresentCount{ 0 };
		std::atomic<std::uint64_t> nativePresentBackpressureEvents{ 0 };
		std::atomic<std::uint64_t> latestNativePresentRequestId{ 0 };
		std::atomic<std::uint64_t> latestNativePresentDurationMicroseconds{ 0 };
		std::atomic<std::uint64_t> readbackObservations{ 0 };
		std::atomic<std::uint64_t> readbackCompletions{ 0 };
		std::atomic<std::uint64_t> latestReadbackRequestId{ 0 };
	};

	[[nodiscard]] AtomicSurface* FindSurface(FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] const AtomicSurface* FindSurface(FrameSurfaceId surfaceId) const noexcept;
	static void RecordHistogram(AtomicHistogram& histogram,
		std::uint64_t valueMicroseconds) noexcept;
	static void UpdateMaximum(std::atomic<std::uint64_t>& target,
		std::uint64_t value) noexcept;
	static std::uint64_t Read(const std::atomic<std::uint64_t>& value) noexcept;

	std::atomic<std::uint64_t> requestCount{ 0 };
	std::atomic<std::uint64_t> publicationCount{ 0 };
	std::atomic<std::uint64_t> skippedPresentations{ 0 };
	std::atomic<std::uint64_t> backpressureEvents{ 0 };
	std::atomic<std::uint64_t> mailboxSaturationEvents{ 0 };
	std::atomic<std::uint64_t> surfaceSlotSaturationEvents{ 0 };
	std::atomic<std::uint64_t> displayEpoch{ 0 };
	std::atomic<std::uint32_t> effectiveRefreshRateHz{ 0 };
	std::atomic<std::uint64_t> refreshIntervalMicroseconds{ 0 };
	std::atomic<std::uint64_t> nativePresentAttempts{ 0 };
	std::atomic<std::uint64_t> nativePresentSuccesses{ 0 };
	std::atomic<std::uint64_t> nativePresentBackpressureSkips{ 0 };
	std::atomic<std::uint64_t> nativePresentNotReadySkips{ 0 };
	std::atomic<std::uint64_t> nativePresentDeviceLosses{ 0 };
	std::atomic<std::uint64_t> nativePresentFailures{ 0 };
	std::atomic<std::int64_t> nativePresentLastResult{ 0 };
	std::atomic<std::uint64_t> nativePresentLastDurationMicroseconds{ 0 };
	std::atomic<std::uint64_t> nativePresentMaximumDurationMicroseconds{ 0 };
	std::atomic<std::uint64_t> readbackObservations{ 0 };
	std::atomic<std::uint64_t> readbackCompletions{ 0 };
	std::atomic<std::uint64_t> readbackBytes{ 0 };
	std::atomic<std::uint64_t> lastReadbackRequestId{ 0 };
	AtomicHistogram requestToPublicationLatency;
	AtomicHistogram uiHandlerDuration;
	AtomicHistogram lockWaitDuration;
	AtomicHistogram nativePresentDuration;
	std::array<AtomicMailbox,
		static_cast<std::size_t>(EFrameTelemetryMailbox::Count)> mailboxes;
	std::array<AtomicSurface, kFrameTelemetrySurfaceSlotCount> surfaces;
};

//! Measures a UI handler without registering a callback or retaining state.
class FrameUiHandlerTimingScope final {
public:
	explicit FrameUiHandlerTimingScope(FrameRuntimeTelemetry& telemetry) noexcept;
	~FrameUiHandlerTimingScope() noexcept;

	FrameUiHandlerTimingScope(const FrameUiHandlerTimingScope&) = delete;
	FrameUiHandlerTimingScope& operator=(const FrameUiHandlerTimingScope&) = delete;

private:
	FrameRuntimeTelemetry* m_telemetry = nullptr;
	std::uint64_t m_startedMicroseconds = 0;
};

} // namespace workbench::rendering
