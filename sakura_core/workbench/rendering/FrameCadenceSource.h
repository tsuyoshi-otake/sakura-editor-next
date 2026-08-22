/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCadence.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace workbench::rendering {

//! A cadence sample acquired from the actual output/compositor associated with
//! one editor window. The source does not create a timer and never waits for a
//! vblank; it only reads bounded Win32/DWM observations at an explicit event
//! boundary (window move, DPI/display change, or a caller-requested refresh).
struct FrameCadenceObservation final {
	FrameCadenceInput input{};
	std::uint64_t observedQpc = 0;
	bool displayRateObserved = false;
	bool compositorRateObserved = false;
	bool monitorObserved = false;

	[[nodiscard]] bool HasExternalClock() const noexcept
	{
		return displayRateObserved || compositorRateObserved;
	}
};

//! Owner-thread source for the production cadence boundary.
//!
//! `DwmGetCompositionTimingInfo` supplies the compositor's QPC refresh period
//! for the window, while `QueryDisplayConfig` supplies the active target's
//! vSync rate. They are kept separate because a Win11 compositor can run at a
//! different cadence from the panel and because two monitors may be mixed-rate.
//! The object is intentionally single-writer and has no lock or callback.
class FrameCadenceSource final {
public:
	FrameCadenceSource() noexcept = default;
	FrameCadenceSource(const FrameCadenceSource&) = delete;
	FrameCadenceSource& operator=(const FrameCadenceSource&) = delete;

	//! Reads the current monitor/compositor sample for `window`.
	//!
	//! A failed optional Win32 query is represented by a zero raw rate; the
	//! cadence math then uses the last known rate or its explicit 60 Hz fallback.
	//! The method is bounded and nonblocking. It is valid only on the owning UI
	//! thread, matching the HWND event source that invokes it.
	[[nodiscard]] FrameCadenceObservation Observe(HWND window) noexcept;

	//! Clears the monitor identity and forces the next observation to publish a
	//! new display epoch. This is used after a display topology notification.
	void Invalidate() noexcept;

	[[nodiscard]] std::uint64_t DisplayEpoch() const noexcept
	{
		return m_displayEpoch;
	}

private:
	[[nodiscard]] static bool ReadMonitorDeviceName(
		HWND window, std::wstring& deviceName) noexcept;
	[[nodiscard]] static std::uint32_t ReadDisplayRefreshRate(
		std::wstring_view deviceName) noexcept;
	[[nodiscard]] static std::uint32_t ReadCompositorRefreshRate(
		HWND window, std::uint64_t& observedQpc) noexcept;
	[[nodiscard]] static std::uint32_t RationalToRefreshRate(
		std::uint32_t numerator, std::uint32_t denominator) noexcept;

	std::uint64_t m_displayEpoch = 0;
	std::wstring m_monitorDeviceName;
	std::uint32_t m_lastDisplayRefreshRateHz = 0;
	std::uint32_t m_lastCompositorRefreshRateHz = 0;
};

} // namespace workbench::rendering
