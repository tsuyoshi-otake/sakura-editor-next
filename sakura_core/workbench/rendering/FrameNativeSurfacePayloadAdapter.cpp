/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameNativeSurfacePayloadAdapter.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::rendering {

FrameNativeSurfacePayloadAdapter::~FrameNativeSurfacePayloadAdapter() noexcept
{
	(void)Close();
	ResetStaging();
}

void FrameNativeSurfacePayloadAdapter::SetSink(
	FrameNativeSurfacePayloadSink sink) noexcept
{
	CloseRegistered();
	try {
		m_sink = std::move(sink);
	}
	catch (...) {
		m_sink = {};
	}
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::Register(
	const FrameNativeSurfacePayloadTarget& target) noexcept
{
	if (!target.IsValidIdentity()
		|| target.width > kMaximumSurfaceDimension
		|| target.height > kMaximumSurfaceDimension) {
		return { EFrameNativeSurfacePayloadStatus::Invalid };
	}

	const bool sameLifetime = m_hasTarget
		&& m_target.surfaceId == target.surfaceId
		&& m_target.surfaceLifetimeEpoch == target.surfaceLifetimeEpoch;
	if (sameLifetime) {
		// A zero-sized update is an explicit close of the physical registration.
		// Reopening it with the old lifetime would let a late payload from the
		// previous HWND/layout generation become current again.
		if (!m_registered && (m_target.width == 0 || m_target.height == 0)) {
			return { EFrameNativeSurfacePayloadStatus::Stale };
		}
		return Update(target);
	}
	if (target.surfaceId == m_lastSurfaceId
		&& target.surfaceLifetimeEpoch <= m_lastLifetimeEpoch) {
		return { EFrameNativeSurfacePayloadStatus::Stale };
	}

	CloseRegistered();
	m_target = target;
	m_hasTarget = true;
	m_lastSurfaceId = target.surfaceId;
	m_lastLifetimeEpoch = target.surfaceLifetimeEpoch;
	m_requestId = 0;
	m_registration = {};
	if (target.width == 0 || target.height == 0) {
		return { EFrameNativeSurfacePayloadStatus::ZeroSized };
	}
	if (!m_sink.IsUsable()) return { EFrameNativeSurfacePayloadStatus::SinkRejected };

	const auto registration = MakeRegistration(target);
	if (!EnsureRegistration(registration)) {
		return { EFrameNativeSurfacePayloadStatus::SinkRejected };
	}
	return { VisibilityStatus(target, EFrameNativeSurfacePayloadStatus::Registered) };
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::Update(
	const FrameNativeSurfacePayloadTarget& target) noexcept
{
	if (!target.IsValidIdentity()
		|| target.width > kMaximumSurfaceDimension
		|| target.height > kMaximumSurfaceDimension) {
		return { EFrameNativeSurfacePayloadStatus::Invalid };
	}
	if (!m_hasTarget) return { EFrameNativeSurfacePayloadStatus::NotRegistered };
	if (target.surfaceId != m_target.surfaceId
		|| target.surfaceLifetimeEpoch != m_target.surfaceLifetimeEpoch) {
		return { EFrameNativeSurfacePayloadStatus::Stale };
	}
	if (target.deviceEpoch < m_target.deviceEpoch
		|| target.displayEpoch < m_target.displayEpoch
		|| target.layoutEpoch < m_target.layoutEpoch) {
		return { EFrameNativeSurfacePayloadStatus::Stale };
	}

	if (target.width == 0 || target.height == 0) {
		CloseRegistered();
		m_target = target;
		return { EFrameNativeSurfacePayloadStatus::ZeroSized };
	}
	if (!m_sink.IsUsable()) return { EFrameNativeSurfacePayloadStatus::SinkRejected };

	const auto registration = MakeRegistration(target);
	if (!EnsureRegistration(registration)) {
		return { EFrameNativeSurfacePayloadStatus::SinkRejected };
	}
	m_target = target;
	return { VisibilityStatus(target, EFrameNativeSurfacePayloadStatus::Updated) };
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::Submit(
	const HDC sourceDc, const RECT& requestedDirtyRect) noexcept
{
	const auto capture = CapturePending(sourceDc, requestedDirtyRect);
	if (capture.status != EFrameNativeSurfacePayloadStatus::Captured) {
		return capture;
	}
	return PublishPending();
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::CapturePending(
	const HDC sourceDc, const RECT& requestedDirtyRect) noexcept
{
	// Each paint boundary supersedes the previous unpublished paint, including
	// a failed/hidden capture. Never let a stale payload cross a later boundary.
	DiscardPending();
	if (!m_hasTarget || !m_registered) {
		return { EFrameNativeSurfacePayloadStatus::NotRegistered };
	}
	if (m_target.minimized) return { EFrameNativeSurfacePayloadStatus::Minimized };
	if (!m_target.visible) return { EFrameNativeSurfacePayloadStatus::Hidden };
	if (m_target.width == 0 || m_target.height == 0) {
		return { EFrameNativeSurfacePayloadStatus::ZeroSized };
	}
	if (sourceDc == nullptr) return { EFrameNativeSurfacePayloadStatus::NoSource };

	RECT dirty{};
	if (!ResolveDirtyRect(requestedDirtyRect, m_target.width, m_target.height, dirty)) {
		return { EFrameNativeSurfacePayloadStatus::NoDirtyPixels };
	}
	const auto dirtyWidth = static_cast<std::uint32_t>(dirty.right - dirty.left);
	const auto dirtyHeight = static_cast<std::uint32_t>(dirty.bottom - dirty.top);
	if (dirtyWidth > (std::numeric_limits<std::uint32_t>::max)() / 4u
		|| dirtyHeight > kMaximumPayloadBytes / (static_cast<std::size_t>(dirtyWidth) * 4u)) {
		return { EFrameNativeSurfacePayloadStatus::PayloadTooLarge };
	}
	if (m_requestId == (std::numeric_limits<std::uint64_t>::max)()) {
		return { EFrameNativeSurfacePayloadStatus::RequestExhausted };
	}
	if (!EnsureStaging(m_target.width, m_target.height)) {
		return { EFrameNativeSurfacePayloadStatus::CaptureFailed };
	}
	if (::BitBlt(m_stagingDc, dirty.left, dirty.top,
		dirtyWidth, dirtyHeight, sourceDc, dirty.left, dirty.top, SRCCOPY) == FALSE) {
		return { EFrameNativeSurfacePayloadStatus::CaptureFailed };
	}

	const auto payloadPitch = dirtyWidth * 4u;
	const auto payloadBytes = static_cast<std::size_t>(payloadPitch) * dirtyHeight;
	std::shared_ptr<std::vector<std::uint8_t>> payload;
	try {
		payload = std::make_shared<std::vector<std::uint8_t>>(payloadBytes);
	}
	catch (...) {
		return { EFrameNativeSurfacePayloadStatus::PayloadTooLarge };
	}

	const auto* sourcePixels = static_cast<const std::uint8_t*>(m_stagingBits);
	for (std::uint32_t row = 0; row < dirtyHeight; ++row) {
		const auto* source = sourcePixels
			+ static_cast<std::size_t>(dirty.top + static_cast<LONG>(row)) * m_stagingPitch
			+ static_cast<std::size_t>(dirty.left) * 4u;
		auto* destination = payload->data() + static_cast<std::size_t>(row) * payloadPitch;
		std::copy_n(source, payloadPitch, destination);
		for (std::uint32_t pixel = 0; pixel < dirtyWidth; ++pixel) {
			destination[pixel * 4u + 3u] = 0xff;
		}
	}

	FrameNativeSurfaceFrame frame{};
	frame.surfaceId = m_target.surfaceId;
	frame.surfaceLifetimeEpoch = m_target.surfaceLifetimeEpoch;
	frame.deviceEpoch = m_target.deviceEpoch;
	frame.displayEpoch = m_target.displayEpoch;
	frame.layoutEpoch = m_target.layoutEpoch;
	frame.requestId = m_requestId + 1;
	frame.width = m_target.width;
	frame.height = m_target.height;
	frame.pitch = payloadPitch;
	frame.dirtyRect = dirty;
	frame.compactDirtyPayload = true;
	frame.pixels = std::move(payload);
	if (!frame.IsValid()) return { EFrameNativeSurfacePayloadStatus::Invalid };

	try {
		m_pendingFrame = std::make_shared<const FrameNativeSurfaceFrame>(std::move(frame));
	}
	catch (...) {
		return { EFrameNativeSurfacePayloadStatus::PayloadTooLarge };
	}
	++m_requestId;
	return { EFrameNativeSurfacePayloadStatus::Captured };
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::PublishPending() noexcept
{
	if (!m_hasTarget || !m_registered) {
		return { EFrameNativeSurfacePayloadStatus::NotRegistered };
	}
	if (m_pendingFrame == nullptr) {
		return { EFrameNativeSurfacePayloadStatus::NoPending };
	}
	if (m_target.minimized) {
		DiscardPending();
		return { EFrameNativeSurfacePayloadStatus::Minimized };
	}
	if (!m_target.visible) {
		DiscardPending();
		return { EFrameNativeSurfacePayloadStatus::Hidden };
	}
	const auto& frame = *m_pendingFrame;
	if (frame.surfaceId != m_target.surfaceId
		|| frame.surfaceLifetimeEpoch != m_target.surfaceLifetimeEpoch
		|| frame.deviceEpoch != m_target.deviceEpoch
		|| frame.displayEpoch != m_target.displayEpoch
		|| frame.layoutEpoch != m_target.layoutEpoch) {
		DiscardPending();
		return { EFrameNativeSurfacePayloadStatus::Stale };
	}
	if (!m_sink.IsUsable()) return { EFrameNativeSurfacePayloadStatus::SinkRejected };
	try {
		if (!m_sink.submitFrame(m_pendingFrame)) {
			return { EFrameNativeSurfacePayloadStatus::SinkRejected };
		}
	}
	catch (...) {
		return { EFrameNativeSurfacePayloadStatus::SinkRejected };
	}
	DiscardPending();
	return { EFrameNativeSurfacePayloadStatus::Submitted };
}

FrameNativeSurfacePayloadResult FrameNativeSurfacePayloadAdapter::Close() noexcept
{
	if (!m_hasTarget && !m_registered) {
		return { EFrameNativeSurfacePayloadStatus::AlreadyClosed };
	}
	CloseRegistered();
	m_hasTarget = false;
	m_target = {};
	m_registration = {};
	return { EFrameNativeSurfacePayloadStatus::Closed };
}

void FrameNativeSurfacePayloadAdapter::CloseRegistered() noexcept
{
	DiscardPending();
	if (!m_registered) return;
	try {
		if (m_sink.closeSurface) {
			m_sink.closeSurface(m_registration.presentation.surfaceId,
				m_registration.presentation.surfaceLifetimeEpoch);
		}
	}
	catch (...) {
	}
	m_registered = false;
	m_registration = {};
}

void FrameNativeSurfacePayloadAdapter::ResetStaging() noexcept
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

bool FrameNativeSurfacePayloadAdapter::EnsureStaging(
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

	const auto nextWidth = std::max(width, m_stagingWidth);
	const auto nextHeight = std::max(height, m_stagingHeight);
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
	const auto bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS,
		&bits, nullptr, 0);
	if (bitmap == nullptr || bits == nullptr) {
		if (m_stagingDc == nullptr) ::DeleteDC(dc);
		return false;
	}
	const auto previous = ::SelectObject(dc, bitmap);
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

bool FrameNativeSurfacePayloadAdapter::ResolveDirtyRect(
	const RECT& requested, const std::uint32_t width,
	const std::uint32_t height, RECT& dirty) const noexcept
{
	if (requested.left == 0 && requested.top == 0
		&& requested.right == 0 && requested.bottom == 0) {
		return false;
	}
	const RECT full{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	dirty.left = std::clamp(requested.left, full.left, full.right);
	dirty.top = std::clamp(requested.top, full.top, full.bottom);
	dirty.right = std::clamp(requested.right, full.left, full.right);
	dirty.bottom = std::clamp(requested.bottom, full.top, full.bottom);
	return dirty.left < dirty.right && dirty.top < dirty.bottom;
}

FrameNativeSurfaceRegistration FrameNativeSurfacePayloadAdapter::MakeRegistration(
	const FrameNativeSurfacePayloadTarget& target) const noexcept
{
	FrameNativeSurfaceRegistration registration{};
	registration.presentation.surfaceId = target.surfaceId;
	registration.presentation.surfaceLifetimeEpoch = target.surfaceLifetimeEpoch;
	registration.presentation.deviceEpoch = target.deviceEpoch;
	registration.presentation.layoutEpoch = target.layoutEpoch;
	registration.presentation.width = target.width;
	registration.presentation.height = target.height;
	registration.presentation.visible = target.visible && !target.minimized;
	registration.targetWindow = registration.presentation.visible ? target.targetWindow : nullptr;
	registration.x = target.x;
	registration.y = target.y;
	return registration;
}

bool FrameNativeSurfacePayloadAdapter::EnsureRegistration(
	const FrameNativeSurfaceRegistration& registration) noexcept
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

bool FrameNativeSurfacePayloadAdapter::SameRegistration(
	const FrameNativeSurfaceRegistration& left,
	const FrameNativeSurfaceRegistration& right) noexcept
{
	return left.presentation.surfaceId == right.presentation.surfaceId
		&& left.presentation.surfaceLifetimeEpoch == right.presentation.surfaceLifetimeEpoch
		&& left.presentation.deviceEpoch == right.presentation.deviceEpoch
		&& left.presentation.layoutEpoch == right.presentation.layoutEpoch
		&& left.presentation.width == right.presentation.width
		&& left.presentation.height == right.presentation.height
		&& left.presentation.visible == right.presentation.visible
		&& left.targetWindow == right.targetWindow
		&& left.x == right.x && left.y == right.y;
}

EFrameNativeSurfacePayloadStatus FrameNativeSurfacePayloadAdapter::VisibilityStatus(
	const FrameNativeSurfacePayloadTarget& target,
	const EFrameNativeSurfacePayloadStatus visibleStatus) noexcept
{
	if (target.minimized) return EFrameNativeSurfacePayloadStatus::Minimized;
	if (!target.visible) return EFrameNativeSurfacePayloadStatus::Hidden;
	return visibleStatus;
}

} // namespace workbench::rendering
