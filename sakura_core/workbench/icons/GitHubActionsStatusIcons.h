/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib AND MIT
*/
#pragma once

// The Sakura GDI wrapper is Zlib; the imported GitHub Actions status icon
// geometry and colours are MIT.
// Imported from github/vscode-github-actions at 45e962b4439e6476d67937206566c0470743c660.
// See GITHUB-ACTIONS-ATTRIBUTION.md beside this file for the source SVGs, the
// license, and the local adaptations.

#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <Windows.h>

#include <span>
#include <string_view>

namespace workbench::icons::github_actions {

//! Upstream ships one SVG per colour theme kind. A layer that exists in only
//! one of them is drawn only for that kind.
enum class ThemeKind : unsigned char {
	Both,
	LightOnly,
	DarkOnly,
};

//! One filled SVG path of a status icon, with its light and dark fill.
struct StatusIconLayer {
	int viewBox = 16;
	bool evenOdd = false;
	std::string_view path;
	COLORREF light = 0;
	COLORREF dark = 0;
	//! SVG fill-opacity as a constant 0..255 alpha. 255 is an ordinary fill.
	BYTE alpha = 255;
	ThemeKind kind = ThemeKind::Both;
};

//! A path an extension names in TreeItem.icon, and the layers it draws.
struct StatusIconEntry {
	std::wstring_view name;
	std::span<const StatusIconLayer> layers;
};

namespace detail {

[[nodiscard]] constexpr COLORREF Hex(unsigned int rgb) noexcept
{
	return RGB((rgb >> 16) & 0xFFU, (rgb >> 8) & 0xFFU, rgb & 0xFFU);
}

constexpr COLORREF kLightMuted = Hex(0x57606A);
constexpr COLORREF kDarkMuted = Hex(0x8B949E);
constexpr COLORREF kAttention = Hex(0xBF8700);

constexpr std::string_view kSuccessPath = R"svg(M8 15C11.866 15 15 11.866 15 8C15 4.134 11.866 1 8 1C4.134 1 1 4.134 1 8C1 11.866 4.134 15 8 15ZM11.3078 6.49529C11.564 6.23901 11.564 5.82349 11.3078 5.56721C11.0515 5.31093 10.636 5.31093 10.3797 5.56721L6.90625 9.04067L5.62029 7.75471C5.36401 7.49843 4.94849 7.49843 4.69221 7.75471C4.43593 8.01099 4.43593 8.42651 4.69221 8.68279L6.44221 10.4328C6.69849 10.689 7.11401 10.689 7.37029 10.4328L11.3078 6.49529Z)svg";
constexpr std::string_view kFailurePath = R"svg(M3.05025 12.9497C0.316583 10.2161 0.316583 5.78392 3.05025 3.05025C5.78392 0.316583 10.2161 0.316583 12.9497 3.05025C15.6834 5.78392 15.6834 10.2161 12.9497 12.9497C10.2161 15.6834 5.78392 15.6834 3.05025 12.9497ZM6.27653 5.34846C6.02025 5.09218 5.60474 5.09218 5.34845 5.34846C5.09217 5.60474 5.09217 6.02026 5.34845 6.27654L7.07192 8L5.34845 9.72346C5.09217 9.97974 5.09217 10.3953 5.34845 10.6515C5.60474 10.9078 6.02025 10.9078 6.27653 10.6515L7.99999 8.92808L9.72345 10.6515C9.97973 10.9078 10.3952 10.9078 10.6515 10.6515C10.9078 10.3953 10.9078 9.97974 10.6515 9.72346L8.92807 8L10.6515 6.27654C10.9078 6.02026 10.9078 5.60475 10.6515 5.34847C10.3953 5.09219 9.97973 5.09219 9.72345 5.34847L7.99999 7.07192L6.27653 5.34846Z)svg";
constexpr std::string_view kCancelledPath = R"svg(M4.91096 1.19221C5.03403 1.06914 5.20095 1 5.375 1H10.625C10.799 1 10.966 1.06914 11.089 1.19221L14.8078 4.91096C14.9309 5.03403 15 5.20095 15 5.375V10.625C15 10.799 14.9309 10.966 14.8078 11.089L11.089 14.8078C10.966 14.9309 10.799 15 10.625 15H5.375C5.20095 15 5.03403 14.9309 4.91096 14.8078L1.19221 11.089C1.06914 10.966 1 10.799 1 10.625V5.375C1 5.20095 1.06914 5.03403 1.19221 4.91096L4.91096 1.19221ZM5.64683 2.3125L2.3125 5.64683V10.3531L5.64683 13.6875H10.3531L13.6875 10.3531V5.64683L10.3531 2.3125H5.64683ZM8 4.5C8.36243 4.5 8.65625 4.79382 8.65625 5.15625V8.21875C8.65625 8.58118 8.36243 8.875 8 8.875C7.63757 8.875 7.34375 8.58118 7.34375 8.21875V5.15625C7.34375 4.79382 7.63757 4.5 8 4.5ZM8 11.5C8.48325 11.5 8.875 11.1083 8.875 10.625C8.875 10.1417 8.48325 9.75 8 9.75C7.51676 9.75 7.125 10.1417 7.125 10.625C7.125 11.1083 7.51676 11.5 8 11.5Z)svg";
constexpr std::string_view kPendingRingPath = R"svg(M3.97833 3.97833C1.75722 6.19944 1.75722 9.80056 3.97833 12.0217C6.19944 14.2428 9.80056 14.2428 12.0217 12.0217C14.2428 9.80056 14.2428 6.19944 12.0217 3.97833C9.80056 1.75722 6.19944 1.75722 3.97833 3.97833ZM3.05025 12.9497C0.316583 10.2161 0.316583 5.78392 3.05025 3.05025C5.78392 0.316583 10.2161 0.316583 12.9497 3.05025C15.6834 5.78392 15.6834 10.2161 12.9497 12.9497C10.2161 15.6834 5.78392 15.6834 3.05025 12.9497Z)svg";
constexpr std::string_view kStepQueuedLightRingPath = R"svg(M3.40381 3.40381C0.865398 5.94221 0.865398 10.0578 3.40381 12.5962C5.94221 15.1346 10.0578 15.1346 12.5962 12.5962C15.1346 10.0578 15.1346 5.94221 12.5962 3.40381C10.0578 0.865398 5.94221 0.865398 3.40381 3.40381ZM2.34315 13.6569C-0.781049 10.5327 -0.781049 5.46734 2.34315 2.34315C5.46734 -0.781049 10.5327 -0.781049 13.6569 2.34315C16.781 5.46734 16.781 10.5327 13.6569 13.6569C10.5327 16.781 5.46734 16.781 2.34315 13.6569Z)svg";
constexpr std::string_view kQueuedPath = R"svg(M8 12C10.2091 12 12 10.2091 12 8C12 5.79086 10.2091 4 8 4C5.79086 4 4 5.79086 4 8C4 10.2091 5.79086 12 8 12Z)svg";
constexpr std::string_view kRunSkippedPath = R"svg(M3.3125 9C3.3125 5.85888 5.85888 3.3125 9 3.3125C12.1412 3.3125 14.6875 5.85888 14.6875 9C14.6875 12.1412 12.1412 14.6875 9 14.6875C5.85888 14.6875 3.3125 12.1412 3.3125 9ZM9 2C5.134 2 2 5.134 2 9C2 12.866 5.134 16 9 16C12.866 16 16 12.866 16 9C16 5.134 12.866 2 9 2ZM11.8703 7.05779C12.1265 6.80151 12.1265 6.38599 11.8703 6.12971C11.614 5.87343 11.1985 5.87343 10.9422 6.12971L6.12971 10.9422C5.87343 11.1985 5.87343 11.614 6.12971 11.8703C6.38599 12.1265 6.80151 12.1265 7.05779 11.8703L11.8703 7.05779Z)svg";
constexpr std::string_view kStepSkippedPath = R"svg(M2.3125 8C2.3125 4.85888 4.85888 2.3125 8 2.3125C11.1412 2.3125 13.6875 4.85888 13.6875 8C13.6875 11.1412 11.1412 13.6875 8 13.6875C4.85888 13.6875 2.3125 11.1412 2.3125 8ZM8 1C4.134 1 1 4.134 1 8C1 11.866 4.134 15 8 15C11.866 15 15 11.866 15 8C15 4.134 11.866 1 8 1ZM10.8703 6.05779C11.1265 5.80151 11.1265 5.38599 10.8703 5.12971C10.614 4.87343 10.1985 4.87343 9.94224 5.12971L5.12971 9.94224C4.87343 10.1985 4.87343 10.614 5.12971 10.8703C5.38599 11.1265 5.80151 11.1265 6.05779 10.8703L10.8703 6.05779Z)svg";
constexpr std::string_view kWaitingPath = R"svg(M2.3125 8C2.3125 6.49158 2.91172 5.04494 3.97833 3.97833C5.04494 2.91172 6.49158 2.3125 8 2.3125C9.50842 2.3125 10.9551 2.91172 12.0217 3.97833C13.0883 5.04494 13.6875 6.49158 13.6875 8C13.6875 9.50842 13.0883 10.9551 12.0217 12.0217C10.9551 13.0883 9.50842 13.6875 8 13.6875C6.49158 13.6875 5.04494 13.0883 3.97833 12.0217C2.91172 10.9551 2.3125 9.50842 2.3125 8ZM8 1C6.14349 1 4.36301 1.7375 3.05026 3.05026C1.7375 4.36301 1 6.14349 1 8C1 9.85649 1.7375 11.637 3.05026 12.9498C4.36301 14.2625 6.14349 15 8 15C9.85649 15 11.637 14.2625 12.9498 12.9498C14.2625 11.637 15 9.85649 15 8C15 6.14349 14.2625 4.36301 12.9498 3.05026C11.637 1.7375 9.85649 1 8 1ZM8.4375 5.15625C8.4375 4.9822 8.36836 4.81528 8.24529 4.69221C8.12222 4.56914 7.9553 4.5 7.78125 4.5C7.6072 4.5 7.44028 4.56914 7.31721 4.69221C7.19414 4.81528 7.125 4.9822 7.125 5.15625V8.21875C7.12503 8.34981 7.16431 8.47785 7.23775 8.5864C7.31121 8.69493 7.41547 8.779 7.53713 8.82775L9.72462 9.70275C9.88484 9.76129 10.0616 9.75507 10.2173 9.68554C10.3731 9.61598 10.4957 9.48851 10.559 9.33012C10.6224 9.17175 10.6216 8.99492 10.5568 8.8371C10.4921 8.6793 10.3684 8.55292 10.212 8.48475L8.4375 7.77425V5.15625Z)svg";
constexpr std::string_view kInProgressRingPath = R"svg(M8 15C11.866 15 15 11.866 15 8C15 4.13401 11.866 1 8 1C4.13401 1 1 4.13401 1 8C1 11.866 4.13401 15 8 15ZM8 13.25C10.8995 13.25 13.25 10.8995 13.25 8C13.25 5.1005 10.8995 2.75 8 2.75C5.1005 2.75 2.75 5.1005 2.75 8C2.75 10.8995 5.1005 13.25 8 13.25Z)svg";
constexpr std::string_view kInProgressArcPath = R"svg(M15 8C15 8.29633 14.9816 8.58836 14.9458 8.875H13.1774C13.2252 8.59044 13.25 8.29812 13.25 8C13.25 5.1005 10.8995 2.75 8 2.75V1C11.866 1 15 4.13401 15 8Z)svg";
//! `resources/icons/{light,dark}/logs.svg`, the `github-actions.workflow.logs` action.
constexpr std::string_view kLogsPath = R"svg(M2 3H1V4H2V3ZM2 6H1V7H2V6ZM1 9H2V10H1V9ZM2 12H1V13H2V12ZM4 3H15V4H4V3ZM15 6H4V7H15V6ZM4 9H15V10H4V9ZM15 12H4V13H15V12Z)svg";
//! Upstream `<circle cx="8" cy="8" r="3.5">`, written as four cubic arcs.
constexpr std::string_view kInProgressCenterPath = R"svg(M11.5 8C11.5 9.933 9.933 11.5 8 11.5C6.067 11.5 4.5 9.933 4.5 8C4.5 6.067 6.067 4.5 8 4.5C9.933 4.5 11.5 6.067 11.5 8Z)svg";

constexpr StatusIconLayer kRunSuccess[] = { { 16, true, kSuccessPath, Hex(0x1A7F37), Hex(0x3FB950) } };
constexpr StatusIconLayer kFailure[] = { { 16, true, kFailurePath, Hex(0xCF222E), Hex(0xF85149) } };
constexpr StatusIconLayer kCancelled[] = { { 16, true, kCancelledPath, kLightMuted, kDarkMuted } };
constexpr StatusIconLayer kRunSkipped[] = { { 18, true, kRunSkippedPath, kLightMuted, kDarkMuted } };
constexpr StatusIconLayer kRunPending[] = { { 16, true, kPendingRingPath, kLightMuted, kDarkMuted } };
constexpr StatusIconLayer kRunQueued[] = { { 16, false, kQueuedPath, kAttention, Hex(0x9E6A03) } };
constexpr StatusIconLayer kRunWaiting[] = { { 16, true, kWaitingPath, Hex(0x9E6A03), Hex(0xD29922) } };
//! Upstream animates the arc with a CSS rotation; it is drawn here at rest.
constexpr StatusIconLayer kInProgress[] = {
	{ 16, true, kInProgressRingPath, kAttention, kAttention, 128 },
	{ 16, true, kInProgressArcPath, kAttention, kAttention },
	{ 16, false, kInProgressCenterPath, kAttention, Hex(0x9E6A03) },
};
constexpr StatusIconLayer kStepSuccess[] = { { 16, true, kSuccessPath, kLightMuted, kDarkMuted } };
constexpr StatusIconLayer kStepSkipped[] = { { 16, true, kStepSkippedPath, kLightMuted, kDarkMuted } };
//! The light and dark step_queued SVGs draw rings of different radii.
constexpr StatusIconLayer kStepQueued[] = {
	{ 16, true, kStepQueuedLightRingPath, kLightMuted, kLightMuted, 255, ThemeKind::LightOnly },
	{ 16, true, kPendingRingPath, kDarkMuted, kDarkMuted, 255, ThemeKind::DarkOnly },
};

constexpr StatusIconLayer kLogs[] = { { 16, true, kLogsPath, Hex(0x424242), Hex(0xC5C5C5) } };

constexpr StatusIconEntry kStatusIcons[] = {
	{ L"resources/icons/light/logs.svg", kLogs },
	{ L"resources/icons/workflowruns/wr_success.svg", kRunSuccess },
	{ L"resources/icons/workflowruns/wr_failure.svg", kFailure },
	{ L"resources/icons/workflowruns/wr_skipped.svg", kRunSkipped },
	{ L"resources/icons/workflowruns/wr_cancelled.svg", kCancelled },
	{ L"resources/icons/workflowruns/wr_pending.svg", kRunPending },
	{ L"resources/icons/workflowruns/wr_queued.svg", kRunQueued },
	{ L"resources/icons/workflowruns/wr_waiting.svg", kRunWaiting },
	{ L"resources/icons/workflowruns/wr_inprogress.svg", kInProgress },
	{ L"resources/icons/steps/step_success.svg", kStepSuccess },
	{ L"resources/icons/steps/step_failure.svg", kFailure },
	{ L"resources/icons/steps/step_skipped.svg", kStepSkipped },
	{ L"resources/icons/steps/step_cancelled.svg", kCancelled },
	{ L"resources/icons/steps/step_queued.svg", kStepQueued },
	{ L"resources/icons/steps/step_inprogress.svg", kInProgress },
};

} // namespace detail

//! True when TreeItem.icon names an extension-relative image rather than a
//! codicon. Codicon names never contain a path separator.
[[nodiscard]] constexpr bool IsIconPath(std::wstring_view name) noexcept
{
	return name.find(L'/') != std::wstring_view::npos;
}

//! Every icon path this build can draw.
[[nodiscard]] constexpr std::span<const StatusIconEntry> StatusIcons() noexcept
{
	return detail::kStatusIcons;
}

//! The layers of a known status icon path; empty for any other name.
[[nodiscard]] constexpr std::span<const StatusIconLayer> FindStatusIcon(std::wstring_view name) noexcept
{
	for (const auto& entry : detail::kStatusIcons) {
		if (entry.name == name) return entry.layers;
	}
	return {};
}

[[nodiscard]] constexpr bool AppliesTo(const StatusIconLayer& layer, bool lightTheme) noexcept
{
	return layer.kind == ThemeKind::Both || (layer.kind == ThemeKind::LightOnly) == lightTheme;
}

namespace detail {

//! Fills one layer into a square viewport that already preserves the aspect ratio.
[[nodiscard]] inline bool FillLayer(HDC dc, const IconRect& viewport, const StatusIconLayer& layer, COLORREF color) noexcept
{
	if (layer.path.empty() || layer.viewBox <= 0) return false;
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	if (::SetGraphicsMode(dc, GM_ADVANCED) == 0) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetPolyFillMode(dc, layer.evenOdd ? ALTERNATE : WINDING);
	const int logicalExtent = layer.viewBox * codicons::detail::kCoordinateScale;
	const XFORM transform{
		static_cast<float>(viewport.Width()) / logicalExtent, 0.0F,
		0.0F, static_cast<float>(viewport.Height()) / logicalExtent,
		static_cast<float>(viewport.left), static_cast<float>(viewport.top),
	};
	if (::SetWorldTransform(dc, &transform) == FALSE || ::BeginPath(dc) == FALSE
		|| !codicons::detail::AppendSvgPath(dc, layer.path) || ::EndPath(dc) == FALSE) {
		::RestoreDC(dc, saved);
		return false;
	}
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush == nullptr) {
		::RestoreDC(dc, saved);
		return false;
	}
	const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
	const bool drawn = ::FillPath(dc) != FALSE;
	::SelectObject(dc, oldBrush);
	::DeleteObject(brush);
	::RestoreDC(dc, saved);
	return drawn;
}

//! GDI paths have no opacity. Fill a copy of the destination and blend the
//! whole copy back at the layer's constant alpha: unfilled pixels blend with
//! themselves, so only the filled shape changes.
[[nodiscard]] inline bool BlendLayer(HDC dc, const IconRect& viewport, const StatusIconLayer& layer, COLORREF color) noexcept
{
	const int width = viewport.Width();
	const int height = viewport.Height();
	const HDC memory = ::CreateCompatibleDC(dc);
	if (memory == nullptr) return false;
	const HBITMAP bitmap = ::CreateCompatibleBitmap(dc, width, height);
	if (bitmap == nullptr) {
		::DeleteDC(memory);
		return false;
	}
	const HGDIOBJ oldBitmap = ::SelectObject(memory, bitmap);
	bool drawn = ::BitBlt(memory, 0, 0, width, height, dc, viewport.left, viewport.top, SRCCOPY) != FALSE
		&& FillLayer(memory, { 0, 0, width, height }, layer, color);
	if (drawn) {
		const BLENDFUNCTION blend{ AC_SRC_OVER, 0, layer.alpha, 0 };
		drawn = ::AlphaBlend(dc, viewport.left, viewport.top, width, height, memory, 0, 0, width, height, blend) != FALSE;
	}
	::SelectObject(memory, oldBitmap);
	::DeleteObject(bitmap);
	::DeleteDC(memory);
	return drawn;
}

} // namespace detail

//! Draws a known status icon in the colours of the active theme kind.
//! Returns false and draws nothing for an unknown name.
inline bool Draw(HDC dc, const IconRect& box, std::wstring_view name, bool lightTheme) noexcept
{
	const auto layers = FindStatusIcon(name);
	if (dc == nullptr || layers.empty() || box.Width() <= 0 || box.Height() <= 0) return false;
	const IconRect viewport = codicons::detail::SvgIconLetterboxBounds(box);
	if (viewport.Width() <= 0) return false;
	bool drawn = true;
	for (const auto& layer : layers) {
		if (!AppliesTo(layer, lightTheme)) continue;
		const COLORREF color = lightTheme ? layer.light : layer.dark;
		const bool filled = layer.alpha == 255
			? detail::FillLayer(dc, viewport, layer, color)
			: detail::BlendLayer(dc, viewport, layer, color);
		drawn = filled && drawn;
	}
	return drawn;
}

} // namespace workbench::icons::github_actions
