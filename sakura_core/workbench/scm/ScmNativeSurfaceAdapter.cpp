/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/ScmNativeSurfaceAdapter.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::scm {
namespace {

constexpr std::uint32_t kMaximumSurfaceDimension = 32768;
constexpr std::size_t kMaximumPayloadBytes = 64u * 1024u * 1024u;

[[nodiscard]] std::uint64_t NextEpoch(const std::uint64_t value) noexcept
{
	return value == (std::numeric_limits<std::uint64_t>::max)() ? value : value + 1;
}

[[nodiscard]] bool SameRegistration(
	const ScmNativeSurfaceRegistration& left,
	const ScmNativeSurfaceRegistration& right) noexcept
{
	return left.surfaceId == right.surfaceId
		&& left.surfaceLifetimeEpoch == right.surfaceLifetimeEpoch
		&& left.deviceEpoch == right.deviceEpoch
		&& left.layoutEpoch == right.layoutEpoch
		&& left.width == right.width
		&& left.height == right.height
		&& left.targetWindow == right.targetWindow
		&& left.x == right.x
		&& left.y == right.y
		&& left.visible == right.visible;
}

} // namespace

bool ScmNativeSurfaceFrame::IsValid() const noexcept
{
	if (surfaceId == 0 || surfaceLifetimeEpoch == 0 || deviceEpoch == 0
		|| layoutEpoch == 0 || requestId == 0 || width == 0 || height == 0
		|| width > kMaximumSurfaceDimension || height > kMaximumSurfaceDimension
		|| !pixels || payloadWidth == 0 || payloadHeight == 0
		|| payloadWidth > kMaximumSurfaceDimension || payloadHeight > kMaximumSurfaceDimension
		|| payloadWidth > (std::numeric_limits<std::uint32_t>::max)() / 4u
		|| payloadPitch < payloadWidth * 4u) {
		return false;
	}
	const RECT full{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	if (dirtyRect.left < full.left || dirtyRect.top < full.top
		|| dirtyRect.right > full.right || dirtyRect.bottom > full.bottom
		|| dirtyRect.left >= dirtyRect.right || dirtyRect.top >= dirtyRect.bottom) {
		return false;
	}
	if (static_cast<std::uint32_t>(dirtyRect.right - dirtyRect.left) != payloadWidth
		|| static_cast<std::uint32_t>(dirtyRect.bottom - dirtyRect.top) != payloadHeight) {
		return false;
	}
	if (payloadHeight > (std::numeric_limits<std::size_t>::max)() / payloadPitch) {
		return false;
	}
	return static_cast<std::size_t>(payloadPitch) * payloadHeight <= pixels->size()
		&& static_cast<std::size_t>(payloadPitch) * payloadHeight <= kMaximumPayloadBytes;
}

ScmNativeSurfacePayloadAdapter::~ScmNativeSurfacePayloadAdapter() noexcept
{
	ClearTarget();
	ResetStaging();
}

void ScmNativeSurfacePayloadAdapter::SetSink(ScmNativeSurfaceSink sink) noexcept
{
	CloseRegistered();
	try {
		m_sink = std::move(sink);
	}
	catch (...) {
		m_sink = {};
	}
}

bool ScmNativeSurfacePayloadAdapter::SetTarget(
	const ScmNativeSurfaceTarget& target) noexcept
{
	if (target.surfaceId == 0 || target.surfaceLifetimeEpoch == 0) {
		ClearTarget();
		return false;
	}

	const bool sameLifetime = m_hasTarget
		&& m_target.surfaceId == target.surfaceId
		&& m_target.surfaceLifetimeEpoch == target.surfaceLifetimeEpoch;
	if (!sameLifetime && target.surfaceId == m_lastSurfaceId
		&& target.surfaceLifetimeEpoch <= m_lastLifetimeEpoch) {
		return false;
	}
	if (!sameLifetime) CloseRegistered();

	m_target = target;
	m_target.deviceEpoch = std::max<std::uint64_t>(1, m_target.deviceEpoch);
	m_target.layoutEpoch = std::max<std::uint64_t>(1, m_target.layoutEpoch);
	m_hasTarget = true;
	m_lastSurfaceId = m_target.surfaceId;
	m_lastLifetimeEpoch = m_target.surfaceLifetimeEpoch;
	m_layoutEpoch = std::max(m_layoutEpoch, m_target.layoutEpoch);

	// A hidden target is still registered as a logical surface, but its native
	// HWND is withdrawn by the update. This lets the presentation owner release
	// the visual without reusing a lifetime epoch.
	if (m_registered && !m_target.visible) HideRegisteredTarget();
	return true;
}

void ScmNativeSurfacePayloadAdapter::ClearTarget() noexcept
{
	CloseRegistered();
	m_hasTarget = false;
	m_target = {};
}

void ScmNativeSurfacePayloadAdapter::CloseRegistered() noexcept
{
	if (!m_registered) return;
	try {
		if (m_sink.closeSurface) {
			m_sink.closeSurface(m_registration.surfaceId,
				m_registration.surfaceLifetimeEpoch);
		}
	}
	catch (...) {
		// A sink owns its asynchronous boundary. The UI-side lifetime is still
		// closed locally even when that boundary reports an exception.
	}
	m_registered = false;
	m_registration = {};
}

void ScmNativeSurfacePayloadAdapter::HideRegisteredTarget() noexcept
{
	if (!m_registered || !m_sink.updateSurface) return;
	ScmNativeSurfaceRegistration hidden = m_registration;
	hidden.targetWindow = nullptr;
	hidden.visible = false;
	try {
		if (m_sink.updateSurface(hidden)) m_registration = hidden;
	}
	catch (...) {
	}
}

void ScmNativeSurfacePayloadAdapter::ResetStaging() noexcept
{
	if (m_stagingDc != nullptr && m_stagingOriginalBitmap != nullptr) {
		(void)::SelectObject(m_stagingDc, m_stagingOriginalBitmap);
	}
	if (m_stagingBitmap != nullptr) ::DeleteObject(m_stagingBitmap);
	if (m_stagingDc != nullptr) ::DeleteDC(m_stagingDc);
	m_stagingDc = nullptr;
	m_stagingBitmap = nullptr;
	m_stagingOriginalBitmap = nullptr;
	m_stagingBits = nullptr;
	m_stagingWidth = 0;
	m_stagingHeight = 0;
	m_stagingPitch = 0;
}

bool ScmNativeSurfacePayloadAdapter::EnsureStaging(
	const std::uint32_t width, const std::uint32_t height) noexcept
{
	if (width == 0 || height == 0 || width > kMaximumSurfaceDimension
		|| height > kMaximumSurfaceDimension
		|| width > (std::numeric_limits<std::uint32_t>::max)() / 4u) {
		return false;
	}
	if (m_stagingDc != nullptr && m_stagingBitmap != nullptr
		&& width <= m_stagingWidth && height <= m_stagingHeight) {
		return true;
	}

	const std::uint32_t nextWidth = std::max(width, m_stagingWidth);
	const std::uint32_t nextHeight = std::max(height, m_stagingHeight);
	if (nextHeight > (std::numeric_limits<std::size_t>::max)()
		/ (static_cast<std::size_t>(nextWidth) * 4u)) {
		return false;
	}
	HDC dc = m_stagingDc;
	if (dc == nullptr) dc = ::CreateCompatibleDC(nullptr);
	if (dc == nullptr) return false;

	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = static_cast<LONG>(nextWidth);
	info.bmiHeader.biHeight = -static_cast<LONG>(nextHeight);
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	const HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS,
		&bits, nullptr, 0);
	if (bitmap == nullptr || bits == nullptr) {
		if (m_stagingDc == nullptr) ::DeleteDC(dc);
		return false;
	}
	const HGDIOBJ previous = ::SelectObject(dc, bitmap);
	if (previous == nullptr || previous == HGDI_ERROR) {
		::DeleteObject(bitmap);
		if (m_stagingDc == nullptr) ::DeleteDC(dc);
		return false;
	}
	if (m_stagingDc == nullptr) {
		m_stagingDc = dc;
		m_stagingOriginalBitmap = previous;
	}
	else if (m_stagingBitmap != nullptr) {
		::DeleteObject(m_stagingBitmap);
	}
	m_stagingBitmap = bitmap;
	m_stagingBits = bits;
	m_stagingWidth = nextWidth;
	m_stagingHeight = nextHeight;
	m_stagingPitch = nextWidth * 4u;
	return true;
}

bool ScmNativeSurfacePayloadAdapter::ResolveSourceSize(
	HWND sourceWindow, const std::uint32_t explicitWidth,
	const std::uint32_t explicitHeight, std::uint32_t& width,
	std::uint32_t& height) const noexcept
{
	if (explicitWidth != 0 || explicitHeight != 0) {
		width = explicitWidth;
		height = explicitHeight;
	}
	else {
		if (sourceWindow == nullptr || !::IsWindow(sourceWindow)) return false;
		RECT client{};
		if (!::GetClientRect(sourceWindow, &client)) return false;
		width = static_cast<std::uint32_t>(std::max<LONG>(0, client.right - client.left));
		height = static_cast<std::uint32_t>(std::max<LONG>(0, client.bottom - client.top));
	}
	return width != 0 && height != 0 && width <= kMaximumSurfaceDimension
		&& height <= kMaximumSurfaceDimension;
}

bool ScmNativeSurfacePayloadAdapter::ResolveDirtyRect(
	const RECT& requested, const std::uint32_t width,
	const std::uint32_t height, RECT& dirty) const noexcept
{
	const RECT full{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	if (requested.left == 0 && requested.top == 0
		&& requested.right == 0 && requested.bottom == 0) {
		dirty = full;
		return true;
	}
	dirty.left = std::clamp(requested.left, full.left, full.right);
	dirty.top = std::clamp(requested.top, full.top, full.bottom);
	dirty.right = std::clamp(requested.right, full.left, full.right);
	dirty.bottom = std::clamp(requested.bottom, full.top, full.bottom);
	return dirty.left < dirty.right && dirty.top < dirty.bottom;
}

bool ScmNativeSurfacePayloadAdapter::ResolveRegistration(
	HWND sourceWindow, const std::uint32_t width, const std::uint32_t height,
	ScmNativeSurfaceRegistration& registration) noexcept
{
	registration = {};
	registration.surfaceId = m_target.surfaceId;
	registration.surfaceLifetimeEpoch = m_target.surfaceLifetimeEpoch;
	registration.deviceEpoch = std::max<std::uint64_t>(1, m_target.deviceEpoch);
	registration.width = width;
	registration.height = height;
	registration.visible = m_target.visible;
	registration.targetWindow = m_target.visible ? m_target.targetWindow : nullptr;
	registration.x = m_target.x;
	registration.y = m_target.y;
	if (sourceWindow != nullptr && registration.targetWindow != nullptr
		&& ::IsWindow(sourceWindow) && ::IsWindow(registration.targetWindow)) {
		POINT origin{ 0, 0 };
		if (::MapWindowPoints(sourceWindow, registration.targetWindow, &origin, 1) != 0) {
			registration.x += origin.x;
			registration.y += origin.y;
		}
	}

	const std::uint64_t requestedLayout = std::max<std::uint64_t>(1, m_target.layoutEpoch);
	if (!m_registered) {
		m_layoutEpoch = std::max(m_layoutEpoch, requestedLayout);
	}
	else {
		const bool geometryChanged = m_registration.width != registration.width
			|| m_registration.height != registration.height
			|| m_registration.targetWindow != registration.targetWindow
			|| m_registration.x != registration.x || m_registration.y != registration.y
			|| m_registration.deviceEpoch != registration.deviceEpoch
			|| m_registration.visible != registration.visible;
		m_layoutEpoch = std::max(m_layoutEpoch, requestedLayout);
		if (geometryChanged && m_layoutEpoch <= m_registration.layoutEpoch) {
			m_layoutEpoch = NextEpoch(m_registration.layoutEpoch);
		}
		else {
			m_layoutEpoch = std::max(m_layoutEpoch, m_registration.layoutEpoch);
		}
	}
	registration.layoutEpoch = std::max<std::uint64_t>(1, m_layoutEpoch);
	return registration.IsValid();
}

bool ScmNativeSurfacePayloadAdapter::EnsureRegistration(
	const ScmNativeSurfaceRegistration& registration) noexcept
{
	if (!registration.IsValid() || !m_sink.IsUsable()) return false;
	try {
		if (!m_registered) {
			if (!m_sink.registerSurface(registration)) return false;
			m_registration = registration;
			m_registered = true;
			return true;
		}
		if (SameRegistration(m_registration, registration)) return true;
		if (!m_sink.updateSurface(registration)) return false;
		m_registration = registration;
		return true;
	}
	catch (...) {
		return false;
	}
}

void ScmNativeSurfacePayloadAdapter::CaptureAndSubmit(
	HWND sourceWindow, HDC sourceDc, const RECT& dirtyRect,
	const std::uint32_t explicitWidth, const std::uint32_t explicitHeight) noexcept
{
	if (!m_hasTarget || !m_target.visible || sourceDc == nullptr
		|| !m_sink.IsUsable()) return;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	if (!ResolveSourceSize(sourceWindow, explicitWidth, explicitHeight, width, height)) return;
	RECT dirty{};
	if (!ResolveDirtyRect(dirtyRect, width, height, dirty)) return;
	if (!EnsureStaging(width, height)) return;
	const int dirtyWidth = dirty.right - dirty.left;
	const int dirtyHeight = dirty.bottom - dirty.top;
	if (dirtyWidth <= 0 || dirtyHeight <= 0
		|| static_cast<std::size_t>(dirtyWidth) > kMaximumPayloadBytes
		/ (static_cast<std::size_t>(dirtyHeight) * 4u)) return;
	if (!::BitBlt(m_stagingDc, dirty.left, dirty.top, dirtyWidth, dirtyHeight,
		sourceDc, dirty.left, dirty.top, SRCCOPY)) return;

	ScmNativeSurfaceRegistration registration{};
	if (!ResolveRegistration(sourceWindow, width, height, registration)
		|| !EnsureRegistration(registration)) return;

	const std::uint32_t payloadWidth = static_cast<std::uint32_t>(dirtyWidth);
	const std::uint32_t payloadHeight = static_cast<std::uint32_t>(dirtyHeight);
	const std::uint32_t payloadPitch = payloadWidth * 4u;
	const std::size_t payloadBytes = static_cast<std::size_t>(payloadPitch) * payloadHeight;
	std::shared_ptr<std::vector<std::uint8_t>> payload;
	try {
		payload = std::make_shared<std::vector<std::uint8_t>>(payloadBytes);
	}
	catch (...) {
		return;
	}
	const auto* sourcePixels = static_cast<const std::uint8_t*>(m_stagingBits);
	for (std::uint32_t row = 0; row < payloadHeight; ++row) {
		const auto* source = sourcePixels
			+ static_cast<std::size_t>(dirty.top + static_cast<LONG>(row)) * m_stagingPitch
			+ static_cast<std::size_t>(dirty.left) * 4u;
		auto* destination = payload->data() + static_cast<std::size_t>(row) * payloadPitch;
		std::copy_n(source, payloadPitch, destination);
		for (std::uint32_t pixel = 0; pixel < payloadWidth; ++pixel) {
			destination[pixel * 4u + 3u] = 0xff;
		}
	}

	if (m_requestId == (std::numeric_limits<std::uint64_t>::max)()) return;
	ScmNativeSurfaceFrame frame{};
	frame.surfaceId = registration.surfaceId;
	frame.surfaceLifetimeEpoch = registration.surfaceLifetimeEpoch;
	frame.deviceEpoch = registration.deviceEpoch;
	frame.layoutEpoch = registration.layoutEpoch;
	frame.requestId = ++m_requestId;
	frame.width = width;
	frame.height = height;
	frame.dirtyRect = dirty;
	frame.payloadWidth = payloadWidth;
	frame.payloadHeight = payloadHeight;
	frame.payloadPitch = payloadPitch;
	frame.pixels = std::move(payload);
	if (!frame.IsValid()) return;
	try {
		(void)m_sink.submitFrame(std::make_shared<const ScmNativeSurfaceFrame>(std::move(frame)));
	}
	catch (...) {
	}
}

} // namespace workbench::scm
