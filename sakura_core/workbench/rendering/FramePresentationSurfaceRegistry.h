/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCoordinatorModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace workbench::rendering {

// One entry represents one independently paced composition island. A window
// may own many entries; this registry deliberately does not collapse them
// into one giant framebuffer or one global backpressure bit.
enum class EFramePresentationSurfaceState : std::uint8_t {
	Closed,
	Ready,
	Backpressured,
	DeviceLost,
	GdiFallback,
	SoftwareOnly,
	Failed,
};

struct FramePresentationSurfaceSpec final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t layoutEpoch = 1;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	bool visible = false;
};

struct FramePresentationSurfaceSnapshot final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t layoutEpoch = 1;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	EFramePresentationSurfaceState state = EFramePresentationSurfaceState::Closed;
	std::uint64_t lastPresentedRequestId = 0;
	bool visible = false;
	bool hasLastGoodContent = false;
};

enum class EFramePresentationSurfaceStatus : std::uint8_t {
	Succeeded,
	Stale,
	Invalid,
	UnknownSurface,
	Busy,
	Closed,
	Exhausted,
	Full,
};

struct FramePresentationSurfaceResult final {
	EFramePresentationSurfaceStatus status = EFramePresentationSurfaceStatus::Invalid;
	EFramePresentationSurfaceState state = EFramePresentationSurfaceState::Closed;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFramePresentationSurfaceStatus::Succeeded;
	}
};

//! Owner-thread-only surface registry for per-island DComp/swapchain state.
//!
//! This is a native-resource-free boundary: FramePresentationOwner uses it to
//! keep stable surface identity, lifetime/device fencing, visibility, and
//! last-good state independent for every island. The registry has no HWND,
//! mutex, callback, wait, or worker. A later implementation can attach one
//! swapchain/visual to each entry without changing the epoch contract.
class FramePresentationSurfaceRegistry final {
public:
	explicit FramePresentationSurfaceRegistry(std::size_t maximumSurfaceCount = 256) noexcept;

	[[nodiscard]] FramePresentationSurfaceResult Register(
		const FramePresentationSurfaceSpec& spec);
	[[nodiscard]] FramePresentationSurfaceResult Close(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult Resize(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint32_t width, std::uint32_t height) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult MarkPresented(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch, std::uint64_t requestId) noexcept;
	//! Layout-aware presentation fence used by the native owner. The legacy
	//! overload above remains for adapters whose layout epoch is unchanged.
	[[nodiscard]] FramePresentationSurfaceResult MarkPresented(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch, std::uint64_t layoutEpoch,
		std::uint64_t requestId) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult MarkBackpressure(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult MarkDeviceLost(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult MarkSoftwareOnly(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch) noexcept;
	//! Reprojects one live island after the window device domain was replaced.
	//! GDI-authoritative pixels remain valid; GPU islands return to Ready or the
	//! coherent software fallback without discarding their last-good metadata.
	[[nodiscard]] FramePresentationSurfaceResult ReprojectDevice(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch, bool softwareOnly) noexcept;
	//! Records a frame already committed by the UI-owned GDI back buffer.
	//!
	//! This is intentionally distinct from SoftwareOnly: the presentation owner
	//! does not own or copy the GDI pixels, but it still fences the committed
	//! request by lifetime, device, and layout epoch.
	[[nodiscard]] FramePresentationSurfaceResult MarkGdiFallback(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		std::uint64_t deviceEpoch, std::uint64_t layoutEpoch,
		std::uint64_t requestId, bool visible) noexcept;
	[[nodiscard]] FramePresentationSurfaceResult MarkFailed(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch) noexcept;

	[[nodiscard]] std::optional<FramePresentationSurfaceSnapshot> Snapshot(
		FrameSurfaceId surfaceId) const;
	[[nodiscard]] std::size_t Size() const noexcept { return m_surfaces.size(); }
	[[nodiscard]] std::size_t MaximumSurfaceCount() const noexcept
	{
		return m_maximumSurfaceCount;
	}

private:
	[[nodiscard]] FramePresentationSurfaceResult Result(
		EFramePresentationSurfaceStatus status,
		EFramePresentationSurfaceState state) const noexcept;
	[[nodiscard]] FramePresentationSurfaceResult FindCurrent(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch,
		FramePresentationSurfaceSnapshot*& snapshot) noexcept;
	[[nodiscard]] static bool IsValidSpec(
		const FramePresentationSurfaceSpec& spec) noexcept;

	std::size_t m_maximumSurfaceCount = 1;
	std::unordered_map<FrameSurfaceId, FramePresentationSurfaceSnapshot> m_surfaces;
};

} // namespace workbench::rendering
