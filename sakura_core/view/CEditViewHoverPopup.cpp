/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "view/CEditViewHoverPopup.h"

#include "markdown/CMarkdownPreviewWnd.h"

#include <algorithm>

namespace {
constexpr wchar_t kWindowClass[] = L"SakuraEditViewHoverPopup";

int ScaleValue(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? 96 : dpi) + 48) / 96);
}
}

CEditViewHoverPopup::CEditViewHoverPopup()
	: CWnd(L"CEditViewHoverPopup")
{
}

//! Defined out of line so the header can forward-declare CMarkdownPreviewWnd.
//! Destroy() runs first, which joins the preview's worker and destroys its
//! window before this object's other members are torn down.
CEditViewHoverPopup::~CEditViewHoverPopup()
{
	Destroy();
}

bool CEditViewHoverPopup::Create(HINSTANCE hInstance, HWND hwndOwner)
{
	if (hInstance == nullptr || hwndOwner == nullptr || GetHwnd() != nullptr) return false;
	if (RegisterWC(hInstance, nullptr, nullptr, ::LoadCursorW(nullptr, IDC_ARROW),
			reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1), nullptr, kWindowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}
	// WS_EX_TOOLWINDOW keeps the popup out of the taskbar/Alt+Tab, matching the
	// existing dictionary-tip precedent (CTipWnd). WS_EX_NOACTIVATE is the
	// extra bit CTipWnd does not need (it is never shown while typing), but
	// this popup can appear while the caret still owns focus, so it must never
	// steal activation merely by being shown.
	// CWnd:: qualification is required here: this class declares its own
	// two-argument Create(HINSTANCE, HWND) above, which hides the base class's
	// whole Create overload set (C++ name hiding), not just the matching
	// signature.
	const HWND window = CWnd::Create(hwndOwner, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"",
		WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, 0, 0, 0, 0, nullptr);
	if (window == nullptr) return false;
	auto preview = std::make_unique<markdown::CMarkdownPreviewWnd>();
	if (!preview->Create(window)) {
		Destroy();
		return false;
	}
	preview->SetPalette(m_palette);
	m_preview = std::move(preview);
	return true;
}

void CEditViewHoverPopup::Destroy() noexcept
{
	// Close the preview before the popup window dies so its worker thread is
	// joined while this object is still fully alive, mirroring
	// CExtensionDetailSurface::Destroy().
	if (m_preview) m_preview->Close();
	m_preview.reset();
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) {
		CWnd::DestroyWindow();
	}
}

void CEditViewHoverPopup::ShowMarkdown(const std::wstring& markdown, POINT screenAnchor, unsigned int dpi)
{
	if (GetHwnd() == nullptr || !::IsWindow(GetHwnd()) || !m_preview) return;
	// Captured for LayoutPreview (called both from here and from OnSize), so
	// the preview child lays out at the DPI of the monitor this popup is
	// actually shown on rather than always assuming 96.
	m_dpi = dpi == 0 ? 96 : dpi;
	if (IsBlank(markdown)) {
		// A provider that answered with nothing (or content that trims to
		// nothing) is a real "no hover" outcome, not a rendering failure --
		// the same rule CExtensionDetailSurface::PublishReadme applies to a
		// blank README. Drop the previous content so a later, different
		// request cannot briefly show stale text before its own parse lands.
		Hide();
		m_preview->SetDocument({});
		return;
	}
	std::wstring source = markdown;
	const bool truncated = source.size() > kMaxMarkdownCharacters;
	if (truncated) source.resize(kMaxMarkdownCharacters);
	// No documentPath and no workspaceRoot: hover Markdown has no local root of
	// its own, so every image/link resolves as ResourceDisposition::ExternalBlocked
	// instead of being fetched. This popup performs no network access.
	markdown::ParseOptions options;
	const markdown::PreviewRenderKey key{ ++m_renderGeneration, 0 };
	if (!m_preview->QueueDocument(std::move(source), std::move(options), truncated, key)) {
		Hide();
		return;
	}

	const int width = ScaleValue(kMaxWidthDip, dpi);
	const int height = ScaleValue(kMaxHeightDip, dpi);
	int x = screenAnchor.x + ScaleValue(kAnchorOffsetXDip, dpi);
	int y = screenAnchor.y + ScaleValue(kAnchorOffsetYDip, dpi);

	RECT workArea{ 0, 0, 0, 0 };
	const HMONITOR monitor = ::MonitorFromPoint(screenAnchor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info)) {
		workArea = info.rcWork;
	} else {
		workArea.right = ::GetSystemMetrics(SM_CXSCREEN);
		workArea.bottom = ::GetSystemMetrics(SM_CYSCREEN);
	}
	// Clamp so the whole popup stays inside the anchor monitor's working area;
	// a hover near a screen edge must never be partly off-screen.
	x = std::clamp(x, static_cast<int>(workArea.left), std::max(static_cast<int>(workArea.left), static_cast<int>(workArea.right) - width));
	y = std::clamp(y, static_cast<int>(workArea.top), std::max(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom) - height));

	::SetWindowPos(GetHwnd(), nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	LayoutPreview();
	::ShowWindow(GetHwnd(), SW_SHOWNA);
	m_preview->Show(true);
}

void CEditViewHoverPopup::Hide() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_HIDE);
	if (m_preview) m_preview->Show(false);
}

bool CEditViewHoverPopup::IsVisible() const noexcept
{
	return GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd()) != FALSE;
}

void CEditViewHoverPopup::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (m_preview) m_preview->SetPalette(m_palette);
}

LRESULT CEditViewHoverPopup::OnSize(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LayoutPreview();
	return CallDefWndProc(hwnd, msg, wp, lp);
}

void CEditViewHoverPopup::LayoutPreview() noexcept
{
	if (!m_preview || GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	m_preview->Layout(client, m_dpi);
}

bool CEditViewHoverPopup::IsBlank(const std::wstring& markdown) noexcept
{
	return markdown.find_first_not_of(L" \t\r\n\f\v") == std::wstring::npos;
}
