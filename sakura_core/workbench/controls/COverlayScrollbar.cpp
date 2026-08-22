/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/controls/COverlayScrollbar.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace workbench::controls {
namespace {

constexpr wchar_t kOverlayScrollbarClass[] = L"SakuraWorkbenchOverlayScrollbar";
//! The reserved hit area, and the visible bar inside it. VS Code's overlay is
//! wider than the thumb it paints so the pointer finds it without pixel precision.
constexpr int kOverlayWidthDip = 10;
constexpr int kThumbWidthDip = 6;
constexpr int kMinimumThumbDip = 20;

} // namespace

COverlayScrollbar::~COverlayScrollbar()
{
	Destroy();
}

int COverlayScrollbar::ScaleDip(int value) const noexcept
{
	return std::max(1, ::MulDiv(value, static_cast<int>(m_dpi == 0 ? 96u : m_dpi), 96));
}

bool COverlayScrollbar::EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = &COverlayScrollbar::WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpszClassName = kOverlayScrollbarClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool COverlayScrollbar::Create(HWND parent, HWND target, SetTopRowCallback setTopRow,
	OverlayScrollbarSource source, OverlayScrollbarOrientation orientation)
{
	if (m_window != nullptr || parent == nullptr || target == nullptr || !setTopRow) return false;
	auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (instance == nullptr) instance = ::GetModuleHandleW(nullptr);
	if (!EnsureClass(instance)) return false;
	m_parent = parent;
	m_target = target;
	m_source = source;
	m_orientation = orientation;
	m_setTopRow = std::move(setTopRow);
	m_window = ::CreateWindowExW(0, kOverlayScrollbarClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_window == nullptr) {
		Detach();
		return false;
	}
	return true;
}

void COverlayScrollbar::Destroy() noexcept
{
	EndDrag(true);
	if (m_window != nullptr) ::DestroyWindow(m_window);
	Detach();
}

void COverlayScrollbar::Detach() noexcept
{
	m_window = nullptr;
	m_parent = nullptr;
	m_target = nullptr;
	m_source = OverlayScrollbarSource::TargetWindowBar;
	m_orientation = OverlayScrollbarOrientation::Vertical;
	m_bounds = RECT{};
	m_hasBounds = false;
	m_setTopRow = nullptr;
	m_scrollModel.reset();
	m_hover = false;
	m_trackingMouseLeave = false;
	m_dragging = false;
	m_thumbGrabOffset = 0;
}

void COverlayScrollbar::SetScrollModel(const OverlayScrollbarModel& model) noexcept
{
	m_scrollModel = NormalizeOverlayScrollbarModel(model);
}

COverlayScrollbar::Layout COverlayScrollbar::GetLayout() const noexcept
{
	Layout layout;
	if (m_target == nullptr || m_window == nullptr) return layout;
	if (m_source == OverlayScrollbarSource::ExplicitModel) {
		if (!m_scrollModel) return layout;
		const auto model = NormalizeOverlayScrollbarModel(*m_scrollModel);
		layout.contentExtent = model.contentExtent;
		layout.viewportExtent = model.viewportExtent;
		layout.offset = model.offset;
	} else {
		SCROLLINFO info{ sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
		const int bar = m_source == OverlayScrollbarSource::ScrollbarControl
			? SB_CTL : (IsHorizontal() ? SB_HORZ : SB_VERT);
		if (!::GetScrollInfo(m_target, bar, &info)) return layout;
		layout.contentExtent = std::max(0, info.nMax - info.nMin + 1);
		layout.viewportExtent = std::max(1, static_cast<int>(info.nPage));
		layout.offset = info.nPos - info.nMin;
	}
	layout.maximumOffset = std::max(0, layout.contentExtent - layout.viewportExtent);
	layout.offset = std::clamp(layout.offset, 0, layout.maximumOffset);
	layout.pageStep = std::max(1, layout.viewportExtent);
	if (layout.maximumOffset == 0) return layout;

	RECT client{};
	if (!::GetClientRect(m_window, &client)) return layout;
	const int extent = IsHorizontal() ? (client.right - client.left) : (client.bottom - client.top);
	if (extent <= 0) return layout;
	layout.track = client;
	const int minimumThumb = std::min(extent, ScaleDip(kMinimumThumbDip));
	const int proportionalThumb = static_cast<int>(
		(static_cast<long long>(extent) * layout.viewportExtent)
			/ std::max(1, layout.contentExtent));
	const int thumbExtent = std::clamp(std::max(minimumThumb, proportionalThumb), 1, extent);
	const int travel = extent - thumbExtent;
	const int offset = static_cast<int>(
		(static_cast<long long>(travel) * layout.offset) / layout.maximumOffset);
	layout.thumb = IsHorizontal()
		? RECT{ client.left + offset, client.top, client.left + offset + thumbExtent, client.bottom }
		: RECT{ client.left, client.top + offset, client.right, client.top + offset + thumbExtent };
	layout.scrollable = true;
	return layout;
}

void COverlayScrollbar::Paint(HDC dc) const
{
	const auto layout = GetLayout();
	if (dc == nullptr || !layout.scrollable) return;
	const bool active = m_hover || m_dragging;
	if (const HBRUSH background = ::CreateSolidBrush(active ? m_colors.trackHover : m_colors.background);
		background != nullptr) {
		::FillRect(dc, &layout.track, background);
		::DeleteObject(background);
	}
	RECT thumb = layout.thumb;
	if (IsHorizontal()) {
		thumb.top = std::max(thumb.top, thumb.bottom - ScaleDip(kThumbWidthDip));
	} else {
		thumb.left = std::max(thumb.left, thumb.right - ScaleDip(kThumbWidthDip));
	}
	if (const HBRUSH thumbBrush = ::CreateSolidBrush(active ? m_colors.thumbHover : m_colors.thumb);
		thumbBrush != nullptr) {
		::FillRect(dc, &thumb, thumbBrush);
		::DeleteObject(thumbBrush);
	}
}

void COverlayScrollbar::UpdateHover(POINT point)
{
	const auto layout = GetLayout();
	const bool hover = layout.scrollable && point.x >= layout.track.left && point.x < layout.track.right
		&& point.y >= layout.track.top && point.y < layout.track.bottom;
	if (m_hover != hover) {
		m_hover = hover;
		Invalidate();
	}
	if (hover && !m_trackingMouseLeave && m_window != nullptr) {
		TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
		m_trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
	}
}

void COverlayScrollbar::EndDrag(bool releaseCapture) noexcept
{
	const bool wasInteractive = m_dragging;
	m_dragging = false;
	m_thumbGrabOffset = 0;
	if (releaseCapture && m_window != nullptr && ::GetCapture() == m_window) ::ReleaseCapture();
	if (wasInteractive) Invalidate();
}

void COverlayScrollbar::ScrollToPosition(int position)
{
	if (!m_setTopRow) return;
	const auto layout = GetLayout();
	m_setTopRow(std::clamp(position, 0, layout.maximumOffset));
	Update();
}

void COverlayScrollbar::DragTo(int pointerPosition)
{
	if (!m_dragging) return;
	const auto layout = GetLayout();
	if (!layout.scrollable) {
		EndDrag(true);
		return;
	}
	const int thumbExtent = IsHorizontal()
		? (layout.thumb.right - layout.thumb.left) : (layout.thumb.bottom - layout.thumb.top);
	const int trackExtent = IsHorizontal()
		? (layout.track.right - layout.track.left) : (layout.track.bottom - layout.track.top);
	const int travel = trackExtent - thumbExtent;
	if (travel <= 0) return;
	const int trackOrigin = static_cast<int>(IsHorizontal() ? layout.track.left : layout.track.top);
	const int position = std::clamp(pointerPosition - m_thumbGrabOffset - trackOrigin, 0, travel);
	ScrollToPosition(static_cast<int>((static_cast<long long>(layout.maximumOffset) * position) / travel));
}

HWND COverlayScrollbar::ScrolledWindow() const noexcept
{
	// A hidden ScrollbarControl target only stores metadata, so focus and wheel
	// input belong to its owner. Other sources use the target as the surface.
	return m_source == OverlayScrollbarSource::ScrollbarControl ? m_parent : m_target;
}

void COverlayScrollbar::Invalidate() const noexcept
{
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
}

void COverlayScrollbar::Update()
{
	if (m_window == nullptr || m_target == nullptr || m_parent == nullptr) return;
	if (m_source == OverlayScrollbarSource::ScrollbarControl) {
		// The control stays alive as the scroll model and stops being drawn.
		if ((::GetWindowLongPtrW(m_target, GWL_STYLE) & WS_VISIBLE) != 0) {
			::ShowWindow(m_target, SW_HIDE);
		}
	} else {
		const LONG_PTR style = ::GetWindowLongPtrW(m_target, GWL_STYLE);
		if (m_source == OverlayScrollbarSource::TargetWindowBar
			&& (style & (WS_HSCROLL | WS_VSCROLL)) != 0) {
			(void)::ShowScrollBar(m_target, SB_BOTH, FALSE);
		}
		// The workbench may lay out a page while its parent ViewContainer is hidden.
		// Keep the target control's own visibility state as the source of truth:
		// IsWindowVisible would include the hidden parent and suppress the overlay
		// until an unrelated layout pass.
		if ((::GetWindowLongPtrW(m_target, GWL_STYLE) & WS_VISIBLE) == 0) {
			::ShowWindow(m_window, SW_HIDE);
			return;
		}
	}
	RECT client{};
	if (m_hasBounds) {
		client = m_bounds;
	} else {
		if (!::GetClientRect(m_target, &client)) return;
		(void)::MapWindowPoints(m_target, m_parent, reinterpret_cast<LPPOINT>(&client), 2);
	}
	const int clientWidth = std::max(0, static_cast<int>(client.right - client.left));
	const int clientHeight = std::max(0, static_cast<int>(client.bottom - client.top));
	if (clientWidth <= 0 || clientHeight <= 0) {
		::ShowWindow(m_window, SW_HIDE);
		return;
	}
	if (IsHorizontal()) {
		const int overlayHeight = std::min(ScaleDip(kOverlayWidthDip), clientHeight);
		::SetWindowPos(m_window, HWND_TOP, static_cast<int>(client.left),
			static_cast<int>(client.bottom) - overlayHeight, clientWidth, overlayHeight,
			SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
	} else {
		const int overlayWidth = std::min(ScaleDip(kOverlayWidthDip), clientWidth);
		::SetWindowPos(m_window, HWND_TOP, static_cast<int>(client.right) - overlayWidth,
			static_cast<int>(client.top), overlayWidth, clientHeight,
			SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW);
	}
	const bool show = GetLayout().scrollable;
	::ShowWindow(m_window, show ? SW_SHOWNOACTIVATE : SW_HIDE);
	if (!show) {
		m_hover = false;
		m_trackingMouseLeave = false;
		EndDrag(true);
		return;
	}
	Invalidate();
}

LRESULT CALLBACK COverlayScrollbar::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	auto* self = reinterpret_cast<COverlayScrollbar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (self == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		self->Paint(dc);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		self->UpdateHover(point);
		self->DragTo(self->IsHorizontal() ? point.x : point.y);
		return 0;
	}
	case WM_MOUSELEAVE:
		self->m_trackingMouseLeave = false;
		if (!self->m_dragging && self->m_hover) {
			self->m_hover = false;
			self->Invalidate();
		}
		return 0;
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const auto layout = self->GetLayout();
		if (!layout.scrollable) return 0;
		::SetFocus(self->ScrolledWindow());
		const int pointer = self->IsHorizontal() ? point.x : point.y;
		const int thumbStart = static_cast<int>(self->IsHorizontal() ? layout.thumb.left : layout.thumb.top);
		const int thumbEnd = static_cast<int>(self->IsHorizontal() ? layout.thumb.right : layout.thumb.bottom);
		if (pointer >= thumbStart && pointer < thumbEnd) {
			self->m_dragging = true;
			self->m_thumbGrabOffset = pointer - thumbStart;
			::SetCapture(window);
		} else {
			self->ScrollToPosition(layout.offset
				+ (pointer < thumbStart ? -layout.pageStep : layout.pageStep));
		}
		self->UpdateHover(point);
		self->Invalidate();
		return 0;
	}
	case WM_LBUTTONUP:
		self->EndDrag(true);
		return 0;
	case WM_MOUSEWHEEL:
		if (const HWND scrolled = self->ScrolledWindow(); scrolled != nullptr) {
			(void)::SendMessageW(scrolled, message, wParam, lParam);
		}
		self->Update();
		return 0;
	case WM_NCDESTROY:
		self->EndDrag(false);
		self->m_window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	default:
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::controls
