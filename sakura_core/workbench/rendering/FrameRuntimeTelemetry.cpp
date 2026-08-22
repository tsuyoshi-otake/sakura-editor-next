#include "StdAfx.h"
#include "workbench/rendering/FrameRuntimeTelemetry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace workbench::rendering {
namespace {

// Upper bounds are intentionally coarse. They are large enough for the C8
// gates while keeping the report fixed-size and cheap to update.
constexpr std::array<std::uint64_t, kFrameTelemetryHistogramBinCount - 1>
	kHistogramUpperBounds{
		100, 250, 500, 1000, 2000, 4000, 8000, 16000,
		32000, 64000, 128000, 256000, 512000, 1000000, 2000000,
	};

[[nodiscard]] std::size_t HistogramBin(const std::uint64_t value) noexcept
{
	for (std::size_t index = 0; index < kHistogramUpperBounds.size(); ++index) {
		if (value <= kHistogramUpperBounds[index]) return index;
	}
	return kFrameTelemetryHistogramBinCount - 1;
}

[[nodiscard]] std::uint64_t HistogramUpperBound(const std::size_t index,
	const std::uint64_t maximum) noexcept
{
	if (index < kHistogramUpperBounds.size()) return kHistogramUpperBounds[index];
	return maximum;
}

[[nodiscard]] std::size_t MailboxIndex(const EFrameTelemetryMailbox mailbox) noexcept
{
	return static_cast<std::size_t>(mailbox);
}

} // namespace

std::uint64_t FrameTelemetryHistogramSnapshot::PercentileUpperBound(
	const std::uint32_t percentile) const noexcept
{
	if (samples == 0) return 0;
	const auto boundedPercentile = std::min<std::uint32_t>(percentile, 100);
	// Avoid multiplying two unbounded counters. This keeps percentile reads
	// defined even after a very long-running process has wrapped neither
	// samples nor the rank calculation.
	const auto quotient = samples / 100;
	const auto remainder = samples % 100;
	const auto rank = quotient * boundedPercentile
		+ (remainder * boundedPercentile + 99) / 100;
	const auto targetRank = std::max<std::uint64_t>(rank, 1);
	std::uint64_t cumulative = 0;
	for (std::size_t index = 0; index < bins.size(); ++index) {
		cumulative += bins[index];
		if (cumulative >= targetRank) return HistogramUpperBound(index, maximumMicroseconds);
	}
	return maximumMicroseconds;
}

FrameRuntimeTelemetry::FrameRuntimeTelemetry() noexcept
{
	for (auto& mailbox : mailboxes) {
		mailbox.capacity.store(0, std::memory_order_relaxed);
	}
}

std::uint64_t FrameRuntimeTelemetry::NowMicroseconds() noexcept
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void FrameRuntimeTelemetry::UpdateMaximum(std::atomic<std::uint64_t>& target,
	const std::uint64_t value) noexcept
{
	auto current = target.load(std::memory_order_relaxed);
	while (current < value
		&& !target.compare_exchange_weak(current, value,
			std::memory_order_relaxed, std::memory_order_relaxed)) {
	}
}

std::uint64_t FrameRuntimeTelemetry::Read(
	const std::atomic<std::uint64_t>& value) noexcept
{
	return value.load(std::memory_order_relaxed);
}

void FrameRuntimeTelemetry::RecordHistogram(AtomicHistogram& histogram,
	const std::uint64_t valueMicroseconds) noexcept
{
	histogram.bins[HistogramBin(valueMicroseconds)].fetch_add(
		1, std::memory_order_relaxed);
	histogram.samples.fetch_add(1, std::memory_order_relaxed);
	histogram.totalMicroseconds.fetch_add(valueMicroseconds, std::memory_order_relaxed);
	UpdateMaximum(histogram.maximumMicroseconds, valueMicroseconds);
}

FrameRuntimeTelemetry::AtomicSurface* FrameRuntimeTelemetry::FindSurface(
	const FrameSurfaceId surfaceId) noexcept
{
	if (surfaceId == 0) return nullptr;
	const auto start = static_cast<std::size_t>(surfaceId)
		% kFrameTelemetrySurfaceSlotCount;
	for (std::size_t offset = 0; offset < kFrameTelemetrySurfaceSlotCount; ++offset) {
		auto& surface = surfaces[(start + offset) % kFrameTelemetrySurfaceSlotCount];
		const auto current = surface.surfaceId.load(std::memory_order_acquire);
		if (current == surfaceId) return &surface;
		if (current != 0) continue;
		FrameSurfaceId expected = 0;
		if (surface.surfaceId.compare_exchange_strong(expected, surfaceId,
			std::memory_order_acq_rel, std::memory_order_acquire)) {
			return &surface;
		}
		if (expected == surfaceId) return &surface;
	}
	surfaceSlotSaturationEvents.fetch_add(1, std::memory_order_relaxed);
	return nullptr;
}

const FrameRuntimeTelemetry::AtomicSurface* FrameRuntimeTelemetry::FindSurface(
	const FrameSurfaceId surfaceId) const noexcept
{
	if (surfaceId == 0) return nullptr;
	const auto start = static_cast<std::size_t>(surfaceId)
		% kFrameTelemetrySurfaceSlotCount;
	for (std::size_t offset = 0; offset < kFrameTelemetrySurfaceSlotCount; ++offset) {
		const auto& surface = surfaces[(start + offset) % kFrameTelemetrySurfaceSlotCount];
		if (surface.surfaceId.load(std::memory_order_acquire) == surfaceId) return &surface;
		if (surface.surfaceId.load(std::memory_order_acquire) == 0) return nullptr;
	}
	return nullptr;
}

void FrameRuntimeTelemetry::RecordRequest(const FrameSurfaceId surfaceId,
	const std::uint64_t requestId, std::uint64_t timestampMicroseconds) noexcept
{
	if (requestId == 0) return;
	if (timestampMicroseconds == 0) timestampMicroseconds = NowMicroseconds();
	requestCount.fetch_add(1, std::memory_order_relaxed);
	if (auto* surface = FindSurface(surfaceId)) {
		surface->acceptedRequests.fetch_add(1, std::memory_order_relaxed);
		UpdateMaximum(surface->latestRequestId, requestId);
		surface->latestRequestTimestampMicroseconds.store(
			timestampMicroseconds, std::memory_order_release);
	}
}

void FrameRuntimeTelemetry::RecordPublication(const FrameSurfaceId surfaceId,
	const std::uint64_t requestId, std::uint64_t timestampMicroseconds) noexcept
{
	if (requestId == 0) return;
	if (timestampMicroseconds == 0) timestampMicroseconds = NowMicroseconds();
	publicationCount.fetch_add(1, std::memory_order_relaxed);
	if (auto* surface = FindSurface(surfaceId)) {
		surface->publishedFrames.fetch_add(1, std::memory_order_relaxed);
		UpdateMaximum(surface->latestPublishedRequestId, requestId);
		surface->latestPublicationTimestampMicroseconds.store(
			timestampMicroseconds, std::memory_order_release);
		const auto latestRequestId = surface->latestRequestId.load(std::memory_order_acquire);
		const auto requestTimestamp = surface->latestRequestTimestampMicroseconds.load(
			std::memory_order_acquire);
		if (latestRequestId == requestId && requestTimestamp != 0
			&& timestampMicroseconds >= requestTimestamp) {
			RecordHistogram(requestToPublicationLatency,
				timestampMicroseconds - requestTimestamp);
		}
	}
}

void FrameRuntimeTelemetry::RecordSkippedPresentation(
	const FrameSurfaceId surfaceId, const bool backpressured) noexcept
{
	skippedPresentations.fetch_add(1, std::memory_order_relaxed);
	if (backpressured) backpressureEvents.fetch_add(1, std::memory_order_relaxed);
	if (auto* surface = FindSurface(surfaceId)) {
		surface->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
		if (backpressured) surface->backpressureEvents.fetch_add(1, std::memory_order_relaxed);
	}
}

void FrameRuntimeTelemetry::RecordMailboxAccepted(
	const EFrameTelemetryMailbox mailbox, const std::uint64_t depth,
	const std::uint64_t capacity, const bool replaced) noexcept
{
	const auto index = MailboxIndex(mailbox);
	if (index >= mailboxes.size()) return;
	auto& entry = mailboxes[index];
	entry.accepted.fetch_add(1, std::memory_order_relaxed);
	if (replaced) entry.replaced.fetch_add(1, std::memory_order_relaxed);
	entry.capacity.store(capacity, std::memory_order_relaxed);
	UpdateMaximum(entry.maximumDepth, depth);
}

void FrameRuntimeTelemetry::RecordMailboxSaturation(
	const EFrameTelemetryMailbox mailbox, const std::uint64_t depth,
	const std::uint64_t capacity) noexcept
{
	mailboxSaturationEvents.fetch_add(1, std::memory_order_relaxed);
	const auto index = MailboxIndex(mailbox);
	if (index >= mailboxes.size()) return;
	auto& entry = mailboxes[index];
	entry.saturationEvents.fetch_add(1, std::memory_order_relaxed);
	entry.capacity.store(capacity, std::memory_order_relaxed);
	UpdateMaximum(entry.maximumDepth, depth);
}

void FrameRuntimeTelemetry::RecordUiHandlerDuration(
	const std::uint64_t durationMicroseconds) noexcept
{
	RecordHistogram(uiHandlerDuration, durationMicroseconds);
}

void FrameRuntimeTelemetry::RecordLockWaitDuration(
	const std::uint64_t durationMicroseconds) noexcept
{
	RecordHistogram(lockWaitDuration, durationMicroseconds);
}

void FrameRuntimeTelemetry::RecordNativePresent(
	const EFrameNativePresentOutcome outcome, const std::int64_t resultCode,
	const std::uint64_t durationMicroseconds, const FrameSurfaceId surfaceId,
	const std::uint64_t requestId) noexcept
{
	if (outcome == EFrameNativePresentOutcome::SoftwareFallback) return;
	nativePresentAttempts.fetch_add(1, std::memory_order_relaxed);
	nativePresentLastResult.store(resultCode, std::memory_order_release);
	nativePresentLastDurationMicroseconds.store(
		durationMicroseconds, std::memory_order_release);
	UpdateMaximum(nativePresentMaximumDurationMicroseconds, durationMicroseconds);
	RecordHistogram(nativePresentDuration, durationMicroseconds);

	switch (outcome) {
	case EFrameNativePresentOutcome::Presented:
		nativePresentSuccesses.fetch_add(1, std::memory_order_relaxed);
		break;
	case EFrameNativePresentOutcome::Backpressured:
		nativePresentBackpressureSkips.fetch_add(1, std::memory_order_relaxed);
		break;
	case EFrameNativePresentOutcome::NotReady:
		nativePresentNotReadySkips.fetch_add(1, std::memory_order_relaxed);
		break;
	case EFrameNativePresentOutcome::DeviceLost:
		nativePresentDeviceLosses.fetch_add(1, std::memory_order_relaxed);
		break;
	case EFrameNativePresentOutcome::Failed:
		nativePresentFailures.fetch_add(1, std::memory_order_relaxed);
		break;
	case EFrameNativePresentOutcome::SoftwareFallback:
		break;
	}

	if (auto* surface = FindSurface(surfaceId)) {
		surface->nativePresentCount.fetch_add(1, std::memory_order_relaxed);
		if (outcome == EFrameNativePresentOutcome::Backpressured) {
			surface->nativePresentBackpressureEvents.fetch_add(
				1, std::memory_order_relaxed);
		}
		if (requestId != 0) {
			UpdateMaximum(surface->latestNativePresentRequestId, requestId);
		}
		surface->latestNativePresentDurationMicroseconds.store(
			durationMicroseconds, std::memory_order_release);
	}
}

void FrameRuntimeTelemetry::RecordReadbackObservation(
	const FrameReadbackObservation& observation) noexcept
{
	if (observation.surfaceId == 0 || observation.surfaceLifetimeEpoch == 0
		|| observation.deviceEpoch == 0 || observation.displayEpoch == 0
		|| observation.layoutEpoch == 0 || observation.requestId == 0
		|| observation.width == 0 || observation.height == 0) {
		return;
	}
	readbackObservations.fetch_add(1, std::memory_order_relaxed);
	if (observation.completed) {
		readbackCompletions.fetch_add(1, std::memory_order_relaxed);
	}
	readbackBytes.fetch_add(observation.byteCount, std::memory_order_relaxed);
	UpdateMaximum(lastReadbackRequestId, observation.requestId);
	if (auto* surface = FindSurface(observation.surfaceId)) {
		surface->readbackObservations.fetch_add(1, std::memory_order_relaxed);
		if (observation.completed) {
			surface->readbackCompletions.fetch_add(1, std::memory_order_relaxed);
		}
		UpdateMaximum(surface->latestReadbackRequestId, observation.requestId);
	}
}

void FrameRuntimeTelemetry::ConfigureCadence(
	const std::uint32_t effectiveRefreshRateHz,
	const std::uint64_t refreshIntervalMicroseconds,
	const std::uint64_t displayEpoch) noexcept
{
	this->effectiveRefreshRateHz.store(
		effectiveRefreshRateHz, std::memory_order_relaxed);
	this->refreshIntervalMicroseconds.store(
		refreshIntervalMicroseconds, std::memory_order_relaxed);
	this->displayEpoch.store(displayEpoch, std::memory_order_release);
}

FrameRuntimeTelemetrySnapshot FrameRuntimeTelemetry::Snapshot() const noexcept
{
	FrameRuntimeTelemetrySnapshot snapshot;
	snapshot.requestCount = Read(requestCount);
	snapshot.publicationCount = Read(publicationCount);
	snapshot.skippedPresentations = Read(skippedPresentations);
	snapshot.backpressureEvents = Read(backpressureEvents);
	snapshot.mailboxSaturationEvents = Read(mailboxSaturationEvents);
	snapshot.surfaceSlotSaturationEvents = Read(surfaceSlotSaturationEvents);
	snapshot.displayEpoch = Read(displayEpoch);
	snapshot.effectiveRefreshRateHz = effectiveRefreshRateHz.load(
		std::memory_order_relaxed);
	snapshot.refreshIntervalMicroseconds = Read(refreshIntervalMicroseconds);
	snapshot.nativePresentAttempts = Read(nativePresentAttempts);
	snapshot.nativePresentSuccesses = Read(nativePresentSuccesses);
	snapshot.nativePresentBackpressureSkips = Read(nativePresentBackpressureSkips);
	snapshot.nativePresentNotReadySkips = Read(nativePresentNotReadySkips);
	snapshot.nativePresentDeviceLosses = Read(nativePresentDeviceLosses);
	snapshot.nativePresentFailures = Read(nativePresentFailures);
	snapshot.nativePresentLastResult = nativePresentLastResult.load(
		std::memory_order_acquire);
	snapshot.nativePresentLastDurationMicroseconds = Read(
		nativePresentLastDurationMicroseconds);
	snapshot.nativePresentMaximumDurationMicroseconds = Read(
		nativePresentMaximumDurationMicroseconds);
	snapshot.readbackObservations = Read(readbackObservations);
	snapshot.readbackCompletions = Read(readbackCompletions);
	snapshot.readbackBytes = Read(readbackBytes);
	snapshot.lastReadbackRequestId = Read(lastReadbackRequestId);
	const auto copyHistogram = [](const AtomicHistogram& source,
		FrameTelemetryHistogramSnapshot& target) {
		for (std::size_t index = 0; index < target.bins.size(); ++index) {
			target.bins[index] = source.bins[index].load(std::memory_order_relaxed);
		}
		target.samples = source.samples.load(std::memory_order_relaxed);
		target.totalMicroseconds = source.totalMicroseconds.load(std::memory_order_relaxed);
		target.maximumMicroseconds = source.maximumMicroseconds.load(std::memory_order_relaxed);
	};
	copyHistogram(requestToPublicationLatency, snapshot.requestToPublicationLatency);
	copyHistogram(uiHandlerDuration, snapshot.uiHandlerDuration);
	copyHistogram(lockWaitDuration, snapshot.lockWaitDuration);
	copyHistogram(nativePresentDuration, snapshot.nativePresentDuration);
	for (std::size_t index = 0; index < mailboxes.size(); ++index) {
		const auto& source = mailboxes[index];
		auto& target = snapshot.mailboxes[index];
		target.accepted = Read(source.accepted);
		target.replaced = Read(source.replaced);
		target.saturationEvents = Read(source.saturationEvents);
		target.maximumDepth = Read(source.maximumDepth);
		target.capacity = Read(source.capacity);
	}
	for (std::size_t index = 0; index < surfaces.size(); ++index) {
		const auto& source = surfaces[index];
		const auto id = source.surfaceId.load(std::memory_order_acquire);
		if (id == 0) continue;
		auto& target = snapshot.surfaces[snapshot.surfaceCount++];
		target.surfaceId = id;
		target.acceptedRequests = Read(source.acceptedRequests);
		target.publishedFrames = Read(source.publishedFrames);
		target.skippedPresentations = Read(source.skippedPresentations);
		target.backpressureEvents = Read(source.backpressureEvents);
		target.latestRequestId = Read(source.latestRequestId);
		target.latestPublishedRequestId = Read(source.latestPublishedRequestId);
		target.latestRequestTimestampMicroseconds = Read(
			source.latestRequestTimestampMicroseconds);
		target.latestPublicationTimestampMicroseconds = Read(
			source.latestPublicationTimestampMicroseconds);
		target.nativePresentCount = Read(source.nativePresentCount);
		target.nativePresentBackpressureEvents = Read(
			source.nativePresentBackpressureEvents);
		target.latestNativePresentRequestId = Read(
			source.latestNativePresentRequestId);
		target.latestNativePresentDurationMicroseconds = Read(
			source.latestNativePresentDurationMicroseconds);
		target.readbackObservations = Read(source.readbackObservations);
		target.readbackCompletions = Read(source.readbackCompletions);
		target.latestReadbackRequestId = Read(source.latestReadbackRequestId);
	}
	return snapshot;
}

FrameUiHandlerTimingScope::FrameUiHandlerTimingScope(
	FrameRuntimeTelemetry& telemetry) noexcept
	: m_telemetry(&telemetry)
	, m_startedMicroseconds(FrameRuntimeTelemetry::NowMicroseconds())
{
}

FrameUiHandlerTimingScope::~FrameUiHandlerTimingScope() noexcept
{
	if (m_telemetry == nullptr) return;
	const auto finished = FrameRuntimeTelemetry::NowMicroseconds();
	m_telemetry->RecordUiHandlerDuration(
		finished >= m_startedMicroseconds ? finished - m_startedMicroseconds : 0);
}

} // namespace workbench::rendering
