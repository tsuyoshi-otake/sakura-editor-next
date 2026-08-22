/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <cstdint>

namespace workbench::rendering {

struct FrameCadenceInput final {
	// A display epoch is supplied by the output/display event source.  It is
	// deliberately independent from the refresh-rate value: a monitor can keep
	// the same nominal rate while its output, VRR policy, or compositor timing
	// domain changes.  Zero means that the source has not published an epoch.
	std::uint64_t displayEpoch = 0;
	// Zero means that the compositor rate is not available. The display rate is
	// then used, followed by the explicit 60 Hz default.
	std::uint32_t displayRefreshRateHz = 0;
	std::uint32_t compositorRefreshRateHz = 0;
	std::uint32_t minimumRefreshRateHz = 1;
	std::uint32_t maximumRefreshRateHz = 1000;
};

struct FrameCadenceResult final {
	std::uint64_t displayEpoch = 0;
	std::uint32_t effectiveRefreshRateHz = 0;
	std::chrono::microseconds refreshInterval{ 0 };
	bool valid = false;
};

//! Explicit cadence math. This type never creates a timer or waits.
class FrameCadence final {
public:
	[[nodiscard]] static FrameCadenceResult Calculate(
		const FrameCadenceInput& input) noexcept;

	[[nodiscard]] static bool IsDue(
		std::chrono::microseconds elapsed,
		std::chrono::microseconds interval) noexcept;

	[[nodiscard]] static std::chrono::microseconds NextInterval(
		std::chrono::microseconds elapsed,
		std::chrono::microseconds interval) noexcept;
};

} // namespace workbench::rendering
