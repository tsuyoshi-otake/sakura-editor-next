/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCoordinatorModel.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace workbench::scm {

//! The physical target for one SCM native projection.
//!
//! The target HWND is borrowed. The adapter never posts to it, paints it, or
//! waits for it. A caller may leave it null while proving the CPU payload hook
//! in a fallback-only configuration.
struct ScmNativeSurfaceTarget final {
	rendering::FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 1;
	std::uint64_t layoutEpoch = 1;
	HWND targetWindow = nullptr;
	LONG x = 0;
	LONG y = 0;
	bool visible = false;
};

//! Registration sent to the asynchronous presentation boundary.
//!
//! Width and height describe the source surface. The frame payload may be a
//! compact dirty rectangle and therefore does not have to be surface-sized.
struct ScmNativeSurfaceRegistration final {
	rendering::FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	HWND targetWindow = nullptr;
	LONG x = 0;
	LONG y = 0;
	bool visible = false;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return surfaceId != 0 && surfaceLifetimeEpoch != 0 && deviceEpoch != 0
			&& layoutEpoch != 0 && width != 0 && height != 0;
	}
};

//! Immutable compact BGRA payload for one visible SCM damage rectangle.
//!
//! `pixels` contains exactly `payloadPitch * payloadHeight` bytes (or more),
//! not a full-surface shadow. `dirtyRect` is in source-surface coordinates and
//! has the same width and height as the compact payload.
struct ScmNativeSurfaceFrame final {
	rendering::FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	RECT dirtyRect{};
	std::uint32_t payloadWidth = 0;
	std::uint32_t payloadHeight = 0;
	std::uint32_t payloadPitch = 0;
	std::shared_ptr<const std::vector<std::uint8_t>> pixels;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! The only side effect boundary used by CScmWorkbenchTool.
//!
//! Implementations should enqueue these operations and return immediately.
//! In particular, Submit must not make the UI thread wait for a compositor or
//! call a GPU API. Empty callbacks make the hook a safe no-op fallback.
struct ScmNativeSurfaceSink final {
	std::function<bool(const ScmNativeSurfaceRegistration&)> registerSurface;
	std::function<bool(const ScmNativeSurfaceRegistration&)> updateSurface;
	std::function<void(rendering::FrameSurfaceId, std::uint64_t)> closeSurface;
	std::function<bool(std::shared_ptr<const ScmNativeSurfaceFrame>)> submitFrame;

	[[nodiscard]] bool IsUsable() const noexcept
	{
		return static_cast<bool>(registerSurface)
			&& static_cast<bool>(updateSurface)
			&& static_cast<bool>(closeSurface)
			&& static_cast<bool>(submitFrame);
	}
};

//! Extracts only the dirty pixels from one retained SCM backbuffer.
//!
//! The adapter is UI-thread-owned. Its staging DIB is reused across frames and
//! is resized only when the source grows; each capture performs one BitBlt and
//! one row copy proportional to the requested dirty rectangle.
class ScmNativeSurfacePayloadAdapter final {
public:
	ScmNativeSurfacePayloadAdapter() = default;
	~ScmNativeSurfacePayloadAdapter() noexcept;
	ScmNativeSurfacePayloadAdapter(const ScmNativeSurfacePayloadAdapter&) = delete;
	ScmNativeSurfacePayloadAdapter& operator=(const ScmNativeSurfacePayloadAdapter&) = delete;

	void SetSink(ScmNativeSurfaceSink sink) noexcept;
	[[nodiscard]] bool SetTarget(const ScmNativeSurfaceTarget& target) noexcept;
	void ClearTarget() noexcept;
	//! Captures one dirty region. Explicit dimensions are used only when the
	//! source HWND is null, which keeps the adapter directly testable with a DIB.
	void CaptureAndSubmit(HWND sourceWindow, HDC sourceDc, const RECT& dirtyRect,
		std::uint32_t sourceWidth = 0, std::uint32_t sourceHeight = 0) noexcept;

	[[nodiscard]] bool HasTarget() const noexcept { return m_hasTarget; }
	[[nodiscard]] bool IsRegistered() const noexcept { return m_registered; }
	[[nodiscard]] std::uint64_t NextRequestId() const noexcept { return m_requestId + 1; }

private:
	void CloseRegistered() noexcept;
	void ResetStaging() noexcept;
	[[nodiscard]] bool EnsureStaging(std::uint32_t width, std::uint32_t height) noexcept;
	[[nodiscard]] bool ResolveSourceSize(HWND sourceWindow, std::uint32_t explicitWidth,
		std::uint32_t explicitHeight, std::uint32_t& width, std::uint32_t& height) const noexcept;
	[[nodiscard]] bool ResolveDirtyRect(const RECT& requested, std::uint32_t width,
		std::uint32_t height, RECT& dirty) const noexcept;
	[[nodiscard]] bool ResolveRegistration(HWND sourceWindow, std::uint32_t width,
		std::uint32_t height, ScmNativeSurfaceRegistration& registration) noexcept;
	[[nodiscard]] bool EnsureRegistration(const ScmNativeSurfaceRegistration& registration) noexcept;
	void HideRegisteredTarget() noexcept;

	ScmNativeSurfaceSink m_sink;
	ScmNativeSurfaceTarget m_target{};
	bool m_hasTarget = false;
	bool m_registered = false;
	ScmNativeSurfaceRegistration m_registration{};
	std::uint64_t m_lastSurfaceId = 0;
	std::uint64_t m_lastLifetimeEpoch = 0;
	std::uint64_t m_layoutEpoch = 1;
	std::uint64_t m_requestId = 0;
	HDC m_stagingDc = nullptr;
	HBITMAP m_stagingBitmap = nullptr;
	HGDIOBJ m_stagingOriginalBitmap = nullptr;
	void* m_stagingBits = nullptr;
	std::uint32_t m_stagingWidth = 0;
	std::uint32_t m_stagingHeight = 0;
	std::uint32_t m_stagingPitch = 0;
};

} // namespace workbench::scm
