/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCoordinatorModel.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace workbench::rendering {

constexpr std::size_t kFrameBackpressureSurfaceSlotCount = 64;

enum class EFrameBackpressureStatus : std::uint8_t {
	Accepted,
	Replaced,
	SkippedSaturated,
	SkippedBackpressure,
	Presented,
	Empty,
	UnknownSurface,
	Closed,
	Invalid,
};

struct FrameBackpressureOptions final {
	std::size_t generalCapacity = 8;
	std::size_t reservedEditorCapacity = 1;
};

struct FrameBackpressureResult final {
	EFrameBackpressureStatus status = EFrameBackpressureStatus::Invalid;
	std::uint64_t requestId = 0;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameBackpressureStatus::Accepted
			|| status == EFrameBackpressureStatus::Replaced;
	}
};

struct FrameBackpressureSurfaceSnapshot final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t latestRequestId = 0;
	std::uint64_t presentedRequestId = 0;
	std::uint64_t skippedPresentations = 0;
	std::uint64_t backpressureSkips = 0;
	bool editorReserved = false;
	bool pending = false;
	bool closed = false;
};

struct FrameBackpressureSnapshot final {
	std::size_t generalDepth = 0;
	std::size_t generalCapacity = 0;
	std::size_t editorDepth = 0;
	std::size_t editorCapacity = 0;
	std::uint64_t saturationEvents = 0;
	std::uint64_t backpressureSkips = 0;
	std::uint64_t presentedFrames = 0;
	std::array<FrameBackpressureSurfaceSnapshot,
		kFrameBackpressureSurfaceSlotCount> surfaces{};
	std::size_t surfaceCount = 0;
};

//! Owner-thread model for bounded work admission and no-wait presentation.
//!
//! Editor surfaces have a reserved admission lane. A stalled or saturated
//! general surface therefore cannot consume the Editor lane, and a failed
//! Present attempt leaves the latest request pending for the next explicit
//! cadence tick instead of retrying in a busy loop.
class FrameBackpressureController final {
public:
	explicit FrameBackpressureController(FrameBackpressureOptions options = {}) noexcept;

	[[nodiscard]] FrameBackpressureResult RegisterSurface(
		FrameSurfaceId surfaceId, bool editorReserved) noexcept;
	[[nodiscard]] FrameBackpressureResult CloseSurface(FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] FrameBackpressureResult Submit(
		FrameSurfaceId surfaceId, std::uint64_t requestId) noexcept;
	[[nodiscard]] FrameBackpressureResult Complete(
		FrameSurfaceId surfaceId, std::uint64_t requestId) noexcept;
	//! `compositorReady == false` means Present would return
	//! DXGI_ERROR_WAS_STILL_DRAWING. No wait or retry occurs here.
	[[nodiscard]] FrameBackpressureResult TryPresent(
		FrameSurfaceId surfaceId, bool compositorReady) noexcept;

	[[nodiscard]] FrameBackpressureSnapshot Snapshot() const noexcept;

private:
	struct SurfaceSlot final {
		FrameSurfaceId surfaceId = 0;
		std::uint64_t latestRequestId = 0;
		std::uint64_t presentedRequestId = 0;
		std::uint64_t skippedPresentations = 0;
		std::uint64_t backpressureSkips = 0;
		bool editorReserved = false;
		bool registered = false;
		bool pending = false;
		bool closed = false;
	};

	[[nodiscard]] SurfaceSlot* Find(FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] const SurfaceSlot* Find(FrameSurfaceId surfaceId) const noexcept;
	[[nodiscard]] FrameBackpressureResult Result(
		EFrameBackpressureStatus status, std::uint64_t requestId = 0) const noexcept;

	FrameBackpressureOptions m_options;
	std::array<SurfaceSlot, kFrameBackpressureSurfaceSlotCount> m_surfaces{};
	std::size_t m_generalDepth = 0;
	std::size_t m_editorDepth = 0;
	std::uint64_t m_saturationEvents = 0;
	std::uint64_t m_backpressureSkips = 0;
	std::uint64_t m_presentedFrames = 0;
};

} // namespace workbench::rendering
