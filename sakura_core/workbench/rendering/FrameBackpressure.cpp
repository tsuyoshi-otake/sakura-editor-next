#include "StdAfx.h"
#include "workbench/rendering/FrameBackpressure.h"

#include <algorithm>

namespace workbench::rendering {

FrameBackpressureController::FrameBackpressureController(
	FrameBackpressureOptions options) noexcept
	: m_options(options)
{
	m_options.generalCapacity = std::max<std::size_t>(m_options.generalCapacity, 1);
	m_options.reservedEditorCapacity = std::max<std::size_t>(
		m_options.reservedEditorCapacity, 1);
}

FrameBackpressureResult FrameBackpressureController::Result(
	const EFrameBackpressureStatus status, const std::uint64_t requestId) const noexcept
{
	return { .status = status, .requestId = requestId };
}

FrameBackpressureController::SurfaceSlot* FrameBackpressureController::Find(
	const FrameSurfaceId surfaceId) noexcept
{
	if (surfaceId == 0) return nullptr;
	const auto start = static_cast<std::size_t>(surfaceId)
		% kFrameBackpressureSurfaceSlotCount;
	for (std::size_t offset = 0; offset < kFrameBackpressureSurfaceSlotCount; ++offset) {
		auto& slot = m_surfaces[(start + offset) % kFrameBackpressureSurfaceSlotCount];
		if (slot.surfaceId == surfaceId) return &slot;
		if (slot.surfaceId == 0) {
			slot.surfaceId = surfaceId;
			return &slot;
		}
	}
	return nullptr;
}

const FrameBackpressureController::SurfaceSlot* FrameBackpressureController::Find(
	const FrameSurfaceId surfaceId) const noexcept
{
	if (surfaceId == 0) return nullptr;
	const auto start = static_cast<std::size_t>(surfaceId)
		% kFrameBackpressureSurfaceSlotCount;
	for (std::size_t offset = 0; offset < kFrameBackpressureSurfaceSlotCount; ++offset) {
		const auto& slot = m_surfaces[(start + offset) % kFrameBackpressureSurfaceSlotCount];
		if (slot.surfaceId == surfaceId) return &slot;
		if (slot.surfaceId == 0) return nullptr;
	}
	return nullptr;
}

FrameBackpressureResult FrameBackpressureController::RegisterSurface(
	const FrameSurfaceId surfaceId, const bool editorReserved) noexcept
{
	if (surfaceId == 0) return Result(EFrameBackpressureStatus::Invalid);
	auto* slot = Find(surfaceId);
	if (slot == nullptr) return Result(EFrameBackpressureStatus::SkippedSaturated);
	if (slot->closed) return Result(EFrameBackpressureStatus::Closed);
	if (slot->registered) {
		// A request may reveal that an existing surface is the reserved Editor
		// lane. Upgrading the lane is monotonic and does not move an already
		// admitted item between depth counters.
		if (editorReserved) slot->editorReserved = true;
		return Result(EFrameBackpressureStatus::Replaced);
	}
	if (slot->pending) return Result(EFrameBackpressureStatus::Replaced);
	slot->editorReserved = editorReserved;
	slot->registered = true;
	return Result(EFrameBackpressureStatus::Accepted);
}

FrameBackpressureResult FrameBackpressureController::CloseSurface(
	const FrameSurfaceId surfaceId) noexcept
{
	auto* slot = Find(surfaceId);
	if (slot == nullptr) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (!slot->registered) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (slot->closed) return Result(EFrameBackpressureStatus::Closed);
	if (slot->pending) {
		if (slot->editorReserved) {
			if (m_editorDepth != 0) --m_editorDepth;
		} else if (m_generalDepth != 0) {
			--m_generalDepth;
		}
		slot->pending = false;
	}
	slot->closed = true;
	return Result(EFrameBackpressureStatus::Accepted);
}

FrameBackpressureResult FrameBackpressureController::Submit(
	const FrameSurfaceId surfaceId, const std::uint64_t requestId) noexcept
{
	if (requestId == 0) return Result(EFrameBackpressureStatus::Invalid);
	auto* slot = Find(surfaceId);
	if (slot == nullptr) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (!slot->registered) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (slot->closed) return Result(EFrameBackpressureStatus::Closed);
	if (slot->pending) {
		if (requestId <= slot->latestRequestId) {
			return Result(EFrameBackpressureStatus::SkippedSaturated, slot->latestRequestId);
		}
		slot->latestRequestId = requestId;
		return Result(EFrameBackpressureStatus::Replaced, requestId);
	}
	const auto capacity = slot->editorReserved
		? m_options.reservedEditorCapacity
		: m_options.generalCapacity;
	auto& depth = slot->editorReserved ? m_editorDepth : m_generalDepth;
	if (depth >= capacity) {
		++m_saturationEvents;
		return Result(EFrameBackpressureStatus::SkippedSaturated);
	}
	++depth;
	slot->latestRequestId = requestId;
	slot->pending = true;
	return Result(EFrameBackpressureStatus::Accepted, requestId);
}

FrameBackpressureResult FrameBackpressureController::Complete(
	const FrameSurfaceId surfaceId, const std::uint64_t requestId) noexcept
{
	auto* slot = Find(surfaceId);
	if (slot == nullptr) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (!slot->registered) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (slot->closed) return Result(EFrameBackpressureStatus::Closed);
	if (!slot->pending || slot->latestRequestId != requestId) {
		return Result(EFrameBackpressureStatus::Empty);
	}
	if (slot->editorReserved) {
		if (m_editorDepth != 0) --m_editorDepth;
	} else if (m_generalDepth != 0) {
		--m_generalDepth;
	}
	slot->pending = false;
	return Result(EFrameBackpressureStatus::Accepted, requestId);
}

FrameBackpressureResult FrameBackpressureController::TryPresent(
	const FrameSurfaceId surfaceId, const bool compositorReady) noexcept
{
	auto* slot = Find(surfaceId);
	if (slot == nullptr) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (!slot->registered) return Result(EFrameBackpressureStatus::UnknownSurface);
	if (slot->closed) return Result(EFrameBackpressureStatus::Closed);
	if (!slot->pending) return Result(EFrameBackpressureStatus::Empty);
	if (!compositorReady) {
		++m_backpressureSkips;
		++slot->backpressureSkips;
		++slot->skippedPresentations;
		return Result(EFrameBackpressureStatus::SkippedBackpressure,
			slot->latestRequestId);
	}
	const auto requestId = slot->latestRequestId;
	(void)Complete(surfaceId, requestId);
	slot->presentedRequestId = requestId;
	++m_presentedFrames;
	return Result(EFrameBackpressureStatus::Presented, requestId);
}

FrameBackpressureSnapshot FrameBackpressureController::Snapshot() const noexcept
{
	FrameBackpressureSnapshot snapshot;
	snapshot.generalDepth = m_generalDepth;
	snapshot.generalCapacity = m_options.generalCapacity;
	snapshot.editorDepth = m_editorDepth;
	snapshot.editorCapacity = m_options.reservedEditorCapacity;
	snapshot.saturationEvents = m_saturationEvents;
	snapshot.backpressureSkips = m_backpressureSkips;
	snapshot.presentedFrames = m_presentedFrames;
	for (const auto& source : m_surfaces) {
		if (source.surfaceId == 0) continue;
		auto& target = snapshot.surfaces[snapshot.surfaceCount++];
		target.surfaceId = source.surfaceId;
		target.latestRequestId = source.latestRequestId;
		target.presentedRequestId = source.presentedRequestId;
		target.skippedPresentations = source.skippedPresentations;
		target.backpressureSkips = source.backpressureSkips;
		target.editorReserved = source.editorReserved;
		target.closed = source.closed;
		target.pending = source.pending;
	}
	return snapshot;
}

} // namespace workbench::rendering
