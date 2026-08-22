/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FramePresentationOwner.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace workbench::rendering {

//! Terminal outcomes for a retained-paint native surface hook.
enum class EFrameNativeSurfacePayloadStatus : std::uint8_t {
	Registered,
	Updated,
	Captured,
	Submitted,
	Closed,
	AlreadyClosed,
	Hidden,
	Minimized,
	ZeroSized,
	Invalid,
	Stale,
	NotRegistered,
	NoSource,
	NoDirtyPixels,
	NoPending,
	SinkRejected,
	CaptureFailed,
	PayloadTooLarge,
	RequestExhausted,
};

struct FrameNativeSurfacePayloadResult final {
	EFrameNativeSurfacePayloadStatus status = EFrameNativeSurfacePayloadStatus::Invalid;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameNativeSurfacePayloadStatus::Registered
			|| status == EFrameNativeSurfacePayloadStatus::Updated
			|| status == EFrameNativeSurfacePayloadStatus::Captured
			|| status == EFrameNativeSurfacePayloadStatus::Submitted
			|| status == EFrameNativeSurfacePayloadStatus::Closed
			|| status == EFrameNativeSurfacePayloadStatus::AlreadyClosed
			|| status == EFrameNativeSurfacePayloadStatus::Hidden
			|| status == EFrameNativeSurfacePayloadStatus::Minimized
			|| status == EFrameNativeSurfacePayloadStatus::ZeroSized;
	}
};

//! UI-owned projection metadata for one retained paint surface.
//!
//! The target HWND is borrowed and may be null for a fallback-only sink. A
//! zero-sized target is represented explicitly and never reaches the native
//! registration sink. `displayEpoch` fences payloads against a newer display
//! or compositor observation supplied by the caller.
struct FrameNativeSurfacePayloadTarget final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t displayEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	HWND targetWindow = nullptr;
	LONG x = 0;
	LONG y = 0;
	bool visible = false;
	bool minimized = false;

	[[nodiscard]] bool IsValidIdentity() const noexcept
	{
		return surfaceId != 0 && surfaceLifetimeEpoch != 0
			&& deviceEpoch != 0 && displayEpoch != 0 && layoutEpoch != 0;
	}
};

//! Asynchronous presentation callbacks. They must enqueue and return; the
//! retained-paint adapter never calls a GPU API, waits, joins, or owns HWNDs.
struct FrameNativeSurfacePayloadSink final {
	std::function<bool(const FrameNativeSurfaceRegistration&)> registerSurface;
	std::function<bool(const FrameNativeSurfaceRegistration&)> updateSurface;
	std::function<void(FrameSurfaceId, std::uint64_t)> closeSurface;
	std::function<bool(std::shared_ptr<const FrameNativeSurfaceFrame>)> submitFrame;

	[[nodiscard]] bool IsUsable() const noexcept
	{
		return static_cast<bool>(registerSurface)
			&& static_cast<bool>(updateSurface)
			&& static_cast<bool>(closeSurface)
			&& static_cast<bool>(submitFrame);
	}
};

//! Extracts one dirty rectangle from a retained GDI paint boundary.
//!
//! The adapter is UI-thread-owned. It reuses one staging DIB across paints,
//! BitBlt-captures only the requested rectangle, and allocates an immutable
//! compact BGRA vector whose size is O(dirty pixels), not O(surface pixels).
//! CapturePending replaces one unpublished payload at most; PublishPending is
//! the only operation that calls the nonblocking sink. All epoch and visibility
//! checks are local and synchronous; the sink is the only external boundary.
class FrameNativeSurfacePayloadAdapter final {
public:
	FrameNativeSurfacePayloadAdapter() = default;
	explicit FrameNativeSurfacePayloadAdapter(FrameSurfaceId surfaceId) noexcept
		: m_lastSurfaceId(surfaceId)
	{
	}
	~FrameNativeSurfacePayloadAdapter() noexcept;
	FrameNativeSurfacePayloadAdapter(const FrameNativeSurfacePayloadAdapter&) = delete;
	FrameNativeSurfacePayloadAdapter& operator=(const FrameNativeSurfacePayloadAdapter&) = delete;
	FrameNativeSurfacePayloadAdapter(FrameNativeSurfacePayloadAdapter&&) = delete;
	FrameNativeSurfacePayloadAdapter& operator=(FrameNativeSurfacePayloadAdapter&&) = delete;

	void SetSink(FrameNativeSurfacePayloadSink sink) noexcept;
	[[nodiscard]] FrameNativeSurfacePayloadResult Register(
		const FrameNativeSurfacePayloadTarget& target) noexcept;
	[[nodiscard]] FrameNativeSurfacePayloadResult Update(
		const FrameNativeSurfacePayloadTarget& target) noexcept;
	//! Captures one dirty rectangle into the bounded latest-wins mailbox.
	//! No sink callback is made by this method.
	[[nodiscard]] FrameNativeSurfacePayloadResult CapturePending(
		HDC sourceDc, const RECT& dirtyRect) noexcept;
	//! Publishes the most recently captured immutable payload. The sink must be
	//! nonblocking; a rejected payload remains available for a later retry.
	[[nodiscard]] FrameNativeSurfacePayloadResult PublishPending() noexcept;
	[[nodiscard]] FrameNativeSurfacePayloadResult Submit(
		HDC sourceDc, const RECT& dirtyRect) noexcept;
	[[nodiscard]] FrameNativeSurfacePayloadResult Close() noexcept;
	//! Discards the currently captured payload without calling the sink.
	void DiscardPending() noexcept
	{
		m_pendingFrame.reset();
	}
	[[nodiscard]] bool HasPending() const noexcept { return m_pendingFrame != nullptr; }

	[[nodiscard]] bool HasTarget() const noexcept { return m_hasTarget; }
	[[nodiscard]] bool IsRegistered() const noexcept { return m_registered; }
	[[nodiscard]] std::uint64_t NextRequestId() const noexcept
	{
		return m_requestId == (std::numeric_limits<std::uint64_t>::max)()
			? m_requestId : m_requestId + 1;
	}

private:
	static constexpr std::uint32_t kMaximumSurfaceDimension =
		kFrameMaximumNativeSurfaceDimension;
	static constexpr std::size_t kMaximumPayloadBytes = 64u * 1024u * 1024u;

	void CloseRegistered() noexcept;
	void ResetStaging() noexcept;
	[[nodiscard]] bool EnsureStaging(std::uint32_t width, std::uint32_t height) noexcept;
	[[nodiscard]] bool ResolveDirtyRect(const RECT& requested,
		std::uint32_t width, std::uint32_t height, RECT& dirty) const noexcept;
	[[nodiscard]] FrameNativeSurfaceRegistration MakeRegistration(
		const FrameNativeSurfacePayloadTarget& target) const noexcept;
	[[nodiscard]] bool EnsureRegistration(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	[[nodiscard]] static bool SameRegistration(
		const FrameNativeSurfaceRegistration& left,
		const FrameNativeSurfaceRegistration& right) noexcept;
	[[nodiscard]] static EFrameNativeSurfacePayloadStatus VisibilityStatus(
		const FrameNativeSurfacePayloadTarget& target,
		EFrameNativeSurfacePayloadStatus visibleStatus) noexcept;

	FrameNativeSurfacePayloadSink m_sink;
	FrameNativeSurfacePayloadTarget m_target{};
	bool m_hasTarget = false;
	bool m_registered = false;
	FrameNativeSurfaceRegistration m_registration{};
	FrameSurfaceId m_lastSurfaceId = 0;
	std::uint64_t m_lastLifetimeEpoch = 0;
	std::uint64_t m_requestId = 0;
	std::shared_ptr<const FrameNativeSurfaceFrame> m_pendingFrame;
	HDC m_stagingDc = nullptr;
	HBITMAP m_stagingBitmap = nullptr;
	HGDIOBJ m_stagingOriginalBitmap = nullptr;
	void* m_stagingBits = nullptr;
	std::uint32_t m_stagingWidth = 0;
	std::uint32_t m_stagingHeight = 0;
	std::uint32_t m_stagingPitch = 0;
};

} // namespace workbench::rendering
