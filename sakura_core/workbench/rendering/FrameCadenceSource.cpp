/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameCadenceSource.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace workbench::rendering {
namespace {

constexpr UINT kDisplayConfigQueryFlags = QDC_ONLY_ACTIVE_PATHS;

[[nodiscard]] bool SameRateOrUnknown(
	const std::uint32_t previous, const std::uint32_t current) noexcept
{
	// A transient query failure must not manufacture a new epoch or throw away
	// a known clock. A real changed non-zero rate is an epoch boundary.
	return current == 0 || previous == current;
}

[[nodiscard]] std::uint64_t QueryQpc() noexcept
{
	LARGE_INTEGER value{};
	return ::QueryPerformanceCounter(&value)
		? static_cast<std::uint64_t>(value.QuadPart) : 0;
}

} // namespace

std::uint32_t FrameCadenceSource::RationalToRefreshRate(
	const std::uint32_t numerator, const std::uint32_t denominator) noexcept
{
	if (numerator == 0 || denominator == 0) return 0;
	const auto rate = static_cast<long double>(numerator)
		/ static_cast<long double>(denominator);
	if (!(rate > 0.0L)
		|| rate > static_cast<long double>((std::numeric_limits<std::uint32_t>::max)())) {
		return 0;
	}
	return static_cast<std::uint32_t>(std::llround(rate));
}

bool FrameCadenceSource::ReadMonitorDeviceName(
	const HWND window, std::wstring& deviceName) noexcept
{
	try {
		deviceName.clear();
		if (window == nullptr || !::IsWindow(window)) return false;
		const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
		if (monitor == nullptr) return false;
		MONITORINFOEXW info{};
		info.cbSize = sizeof(info);
		if (!::GetMonitorInfoW(monitor, &info)) return false;
		if (info.szDevice[0] == L'\0') return false;
		deviceName.assign(info.szDevice);
		return true;
	} catch (...) {
		deviceName.clear();
		return false;
	}
}

std::uint32_t FrameCadenceSource::ReadDisplayRefreshRate(
	const std::wstring_view deviceName) noexcept
{
	try {
		if (deviceName.empty()) return 0;
		UINT pathCount = 0;
		UINT modeCount = 0;
		if (::GetDisplayConfigBufferSizes(kDisplayConfigQueryFlags,
			&pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
			return 0;
		}

		// The topology can change between sizing and querying. Refresh both
		// counts before the one permitted retry; reusing stale counts can turn a
		// display-change event into a false zero-rate observation.
		for (int attempt = 0; attempt != 2; ++attempt) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
			const LONG result = ::QueryDisplayConfig(kDisplayConfigQueryFlags,
				&pathCount, paths.data(), &modeCount, modes.data(), nullptr);
			if (result == ERROR_INSUFFICIENT_BUFFER && attempt == 0) {
				if (::GetDisplayConfigBufferSizes(kDisplayConfigQueryFlags,
					&pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0) {
					return 0;
				}
				continue;
			}
			if (result != ERROR_SUCCESS) return 0;

			for (UINT index = 0; index < pathCount; ++index) {
				const auto& path = paths[index];
				DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
				source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				source.header.size = sizeof(source);
				source.header.adapterId = path.sourceInfo.adapterId;
				source.header.id = path.sourceInfo.id;
				if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
				if (deviceName != std::wstring_view(source.viewGdiDeviceName)) continue;
				// The active path carries the desktop presentation rate directly.
				// DISPLAYCONFIG_TARGET_MODE contains the richer signal details, but
				// its mode record is not guaranteed to be present for every active
				// path.  The path refresh rate is the stable acquisition contract.
				return RationalToRefreshRate(
					path.targetInfo.refreshRate.Numerator,
					path.targetInfo.refreshRate.Denominator);
			}
			return 0;
		}
		return 0;
	} catch (...) {
		return 0;
	}
}

std::uint32_t FrameCadenceSource::ReadCompositorRefreshRate(
	const HWND window, std::uint64_t& observedQpc) noexcept
{
	observedQpc = QueryQpc();
	if (window == nullptr || !::IsWindow(window)) return 0;
	DWM_TIMING_INFO timing{};
	timing.cbSize = sizeof(timing);
	if (FAILED(::DwmGetCompositionTimingInfo(window, &timing))
		|| timing.qpcRefreshPeriod <= 0) {
		return 0;
	}
	LARGE_INTEGER frequency{};
	if (!::QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
		return 0;
	}
	const auto period = static_cast<long double>(timing.qpcRefreshPeriod);
	const auto qpcFrequency = static_cast<long double>(frequency.QuadPart);
	const auto rate = qpcFrequency / period;
	if (!(rate > 0.0L)) return 0;
	return static_cast<std::uint32_t>(std::llround(rate));
}

FrameCadenceObservation FrameCadenceSource::Observe(const HWND window) noexcept
{
	FrameCadenceObservation observation;
	try {
		std::wstring monitorDeviceName;
		const bool monitorObserved = ReadMonitorDeviceName(window, monitorDeviceName);
		std::uint64_t observedQpc = 0;
		const auto displayRate = monitorObserved
			? ReadDisplayRefreshRate(monitorDeviceName) : 0;
		const auto compositorRate = ReadCompositorRefreshRate(window, observedQpc);

		const bool monitorChanged = monitorObserved
			&& monitorDeviceName != m_monitorDeviceName;
		const bool displayChanged = !SameRateOrUnknown(
			m_lastDisplayRefreshRateHz, displayRate);
		const bool compositorChanged = !SameRateOrUnknown(
			m_lastCompositorRefreshRateHz, compositorRate);
		if (m_displayEpoch == 0 || monitorChanged || displayChanged || compositorChanged) {
			m_displayEpoch = m_displayEpoch == (std::numeric_limits<std::uint64_t>::max)()
				? m_displayEpoch : m_displayEpoch + 1;
		}
		if (monitorObserved) m_monitorDeviceName = std::move(monitorDeviceName);
		if (displayRate != 0) m_lastDisplayRefreshRateHz = displayRate;
		if (compositorRate != 0) m_lastCompositorRefreshRateHz = compositorRate;

		observation.input.displayEpoch = m_displayEpoch;
		observation.input.displayRefreshRateHz = m_lastDisplayRefreshRateHz;
		observation.input.compositorRefreshRateHz = m_lastCompositorRefreshRateHz;
		observation.observedQpc = observedQpc;
		observation.displayRateObserved = displayRate != 0;
		observation.compositorRateObserved = compositorRate != 0;
		observation.monitorObserved = monitorObserved;
		return observation;
	} catch (...) {
		// The cadence boundary is advisory. A failed system query must leave a
		// valid epoch and the last known rates for the explicit 60 Hz fallback.
		if (m_displayEpoch == 0) m_displayEpoch = 1;
		observation.input.displayEpoch = m_displayEpoch;
		observation.input.displayRefreshRateHz = m_lastDisplayRefreshRateHz;
		observation.input.compositorRefreshRateHz = m_lastCompositorRefreshRateHz;
		return observation;
	}
}

void FrameCadenceSource::Invalidate() noexcept
{
	m_monitorDeviceName.clear();
	m_lastDisplayRefreshRateHz = 0;
	m_lastCompositorRefreshRateHz = 0;
	if (m_displayEpoch == (std::numeric_limits<std::uint64_t>::max)()) return;
	++m_displayEpoch;
}

} // namespace workbench::rendering
