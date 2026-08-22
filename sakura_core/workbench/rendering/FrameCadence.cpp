#include "StdAfx.h"
#include "workbench/rendering/FrameCadence.h"

#include <algorithm>
#include <limits>

namespace workbench::rendering {

FrameCadenceResult FrameCadence::Calculate(const FrameCadenceInput& input) noexcept
{
	const auto minimum = std::max<std::uint32_t>(input.minimumRefreshRateHz, 1);
	const auto maximum = std::max(input.maximumRefreshRateHz, minimum);
	std::uint32_t selected = input.compositorRefreshRateHz != 0
		? input.compositorRefreshRateHz
		: input.displayRefreshRateHz;
	if (selected == 0) selected = 60;
	selected = std::clamp(selected, minimum, maximum);
	const auto interval = static_cast<std::uint64_t>(1000000u + selected - 1)
		/ selected;
	return {
		.displayEpoch = input.displayEpoch,
		.effectiveRefreshRateHz = selected,
		.refreshInterval = std::chrono::microseconds(interval),
		.valid = true,
	};
}

bool FrameCadence::IsDue(const std::chrono::microseconds elapsed,
	const std::chrono::microseconds interval) noexcept
{
	return interval.count() > 0 && elapsed >= interval;
}

std::chrono::microseconds FrameCadence::NextInterval(
	const std::chrono::microseconds elapsed,
	const std::chrono::microseconds interval) noexcept
{
	if (interval.count() <= 0) return std::chrono::microseconds(1);
	if (elapsed.count() < 0) return interval;
	const auto elapsedCount = elapsed.count();
	const auto intervalCount = interval.count();
	const auto remainder = elapsedCount % intervalCount;
	return std::chrono::microseconds(remainder == 0
		? intervalCount
		: intervalCount - remainder);
}

} // namespace workbench::rendering
