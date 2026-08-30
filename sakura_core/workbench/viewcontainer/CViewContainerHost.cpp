/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerHost.h"

#include "CSelectLang.h"
#include "sakura_rc.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <string_view>
#include <windowsx.h>

namespace workbench::viewcontainer {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraViewContainerHost";
constexpr int kDefaultDpi = 96;
constexpr int kOutlineHeaderHeightDip = 24;
constexpr int kOutlinePreferredHeightDip = 180;
constexpr int kOutlineMinimumHeightDip = 96;
constexpr int kEmptyTextInsetDip = 20;

int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi);
}

[[nodiscard]] std::wstring LocalizedString(UINT resourceId, const wchar_t* fallback)
{
	const auto localized = CSelectLang::LoadStringW(resourceId);
	if (!localized.empty()) return std::wstring(localized);
	return fallback != nullptr ? std::wstring(fallback) : std::wstring();
}

bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CViewContainerHost::WindowProc;
	windowClass.lpszClassName = kWindowClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

[[nodiscard]] bool PaintFontGlyph(
	HDC dc,
	const workbench::icons::IconRect& box,
	HFONT font,
	wchar_t glyph,
	COLORREF color
) noexcept
{
	if (dc == nullptr || font == nullptr || glyph == L'\0' || box.Width() <= 0 || box.Height() <= 0) {
		return false;
	}
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	const HGDIOBJ oldFont = ::SelectObject(dc, font);
	if (oldFont == nullptr || oldFont == HGDI_ERROR) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, color);
	RECT glyphRect{ box.left, box.top, box.right, box.bottom };
	const wchar_t text[] = { glyph, L'\0' };
	const int drawn = ::DrawTextW(dc, text, 1, &glyphRect,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::RestoreDC(dc, saved);
	return drawn != 0;
}

} // namespace

CViewContainerHost::CViewContainerHost(std::shared_ptr<CViewContainerPages> pages,
	std::string logicalHostId, OutlineExpandedCallback outlineExpanded,
	OutlineRevealCallback outlineRevealed)
	: m_pages(std::move(pages))
	, m_logicalHostId(std::move(logicalHostId))
	, m_outlineExpandedCallback(std::move(outlineExpanded))
	, m_outlineRevealCallback(std::move(outlineRevealed))
{
}

CViewContainerHost::~CViewContainerHost()
{
	Close();
}

bool CViewContainerHost::Create(HWND parent)
{
	if (m_closed || m_window != nullptr || parent == nullptr) return false;
	m_instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (m_instance == nullptr) m_instance = ::GetModuleHandleW(nullptr);
	if (!EnsureWindowClass(m_instance)) return false;
	m_window = ::CreateWindowExW(0, kWindowClass, L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, nullptr,
		m_instance, this);
	return m_window != nullptr;
}

void CViewContainerHost::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (m_closed || m_window == nullptr) return;
	m_bounds = contentRect;
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_font.Dpi() != m_dpi) (void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
	::SetWindowPos(m_window, nullptr, contentRect.left, contentRect.top,
		std::max(0L, contentRect.right - contentRect.left),
		std::max(0L, contentRect.bottom - contentRect.top),
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
	LayoutChildren();
}

HFONT CViewContainerHost::AcquireCodiconFont(int height) noexcept
{
	if (height <= 0) return nullptr;
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) {
		ReleaseCodiconFont();
		return nullptr;
	}
	if (m_codiconFont != nullptr && m_codiconFontHeight == height) return m_codiconFont;

	ReleaseCodiconFont();
	LOGFONTW logFont{};
	logFont.lfHeight = -height;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
	logFont.lfFaceName[faceName.size()] = L'\0';
	m_codiconFont = ::CreateFontIndirectW(&logFont);
	if (m_codiconFont != nullptr) m_codiconFontHeight = height;
	return m_codiconFont;
}

void CViewContainerHost::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

void CViewContainerHost::Activate()
{
	if (m_page.empty() || !m_pages || !m_pages->IsUsable()) return;
	if (!OwnsPage(m_page)) return;
	if (m_page == pageIds::SourceControl) {
		if (auto* scm = m_pages->SourceControl()) scm->Activate();
		return;
	}
	if (m_page == pageIds::Search) {
		if (auto* view = m_pages->Search()) view->Activate();
		return;
	}
	if (m_page == pageIds::Extensions) {
		if (auto* view = m_pages->Extensions()) view->Activate();
		return;
	}
	if (m_page == pageIds::Explorer) {
		if (auto* explorer = m_pages->Explorer()) explorer->Activate();
		return;
	}
}

void CViewContainerHost::Deactivate()
{
	if (!m_pages || !m_pages->IsUsable() || m_page.empty() || !OwnsPage(m_page)) return;
	if (m_page == pageIds::SourceControl) {
		if (auto* scm = m_pages->SourceControl()) scm->Deactivate();
		return;
	}
	if (m_page == pageIds::Search) {
		if (auto* view = m_pages->Search()) view->Deactivate();
		return;
	}
	if (m_page == pageIds::Extensions) {
		if (auto* view = m_pages->Extensions()) view->Deactivate();
		return;
	}
	if (m_page == pageIds::Explorer) {
		if (auto* explorer = m_pages->Explorer()) explorer->Deactivate();
		if (auto* outline = m_pages->Outline()) outline->Deactivate();
		return;
	}
}

bool CViewContainerHost::PreTranslateMessage(MSG& message)
{
	if (!m_pages || !m_pages->IsUsable() || m_page.empty() || !OwnsPage(m_page)
		|| !m_pages->IsMessageTarget(m_page, message.hwnd)) return false;
	if (m_page == pageIds::SourceControl) {
		auto* scm = m_pages->SourceControl();
		return scm != nullptr && scm->PreTranslateMessage(message);
	}
	if (m_page == pageIds::Search) {
		auto* view = m_pages->Search();
		return view != nullptr && view->PreTranslateMessage(message);
	}
	if (m_page == pageIds::Extensions) {
		auto* view = m_pages->Extensions();
		return view != nullptr && view->PreTranslateMessage(message);
	}
	if (m_page != pageIds::Explorer) return false;
	auto* outline = m_pages->Outline();
	auto* explorer = m_pages->Explorer();
	const auto belongsTo = [&message](HWND root) noexcept {
		return root != nullptr && (message.hwnd == root || ::IsChild(root, message.hwnd));
	};
	if (m_pages->IsOutlineExpanded() && outline != nullptr
		&& belongsTo(outline->GetHwnd())) {
		return outline->PreTranslateMessage(message);
	}
	return explorer != nullptr && belongsTo(explorer->GetHwnd())
		&& explorer->PreTranslateMessage(message);
}

void CViewContainerHost::Close()
{
	if (m_closed) return;
	// The pages are shared with the other side bar and are owned elsewhere. Detaching
	// them before this window dies keeps their HWNDs valid for the surviving host.
	bool attachedPagePreserved = false;
	if (m_pages && m_pages->IsUsable()) {
		for (const auto& id : m_pages->PageIds()) {
			if (m_pages->AttachedHost(id) == m_window) {
				const auto detached = m_pages->Detach(id);
				const bool stillAttached = detached.finalState
					? FinalStateOwnsHost(detached.finalState)
					: m_pages->AttachedHost(id) == m_window;
				if (!detached.Succeeded() && stillAttached) {
					attachedPagePreserved = true;
				}
			}
		}
	}
	if (attachedPagePreserved) {
		// Destroying this HWND would recursively destroy the pool-owned wrapper. Leave
		// an inert child for the page owner to detach or close. Park it outside the
		// enclosing Panel host so that parent's teardown cannot destroy the wrapper.
		m_closed = true;
		bool preservedOutsideOwner = false;
		if (m_window != nullptr && ::IsWindow(m_window)) {
			const HWND parkingParent = m_pages ? m_pages->ParkingParent() : nullptr;
			if (parkingParent != nullptr && ::IsWindow(parkingParent)) {
				if (::GetParent(m_window) == parkingParent) {
					preservedOutsideOwner = true;
				} else {
					::SetLastError(ERROR_SUCCESS);
					const HWND previousParent = ::SetParent(m_window, parkingParent);
					preservedOutsideOwner = (previousParent != nullptr
						|| ::GetLastError() == ERROR_SUCCESS)
						&& ::GetParent(m_window) == parkingParent;
				}
			}
			::ShowWindow(m_window, SW_HIDE);
			::SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
		}
		m_closeStatus = preservedOutsideOwner
			? EViewContainerHostCloseStatus::DetachedPagePreserved
			: EViewContainerHostCloseStatus::DetachedPagePreservationFailed;
		m_window = nullptr;
		m_backBuffer.Reset();
		ReleaseCodiconFont();
		return;
	}
	if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
	m_window = nullptr;
	m_backBuffer.Reset();
	ReleaseCodiconFont();
	m_closeStatus = EViewContainerHostCloseStatus::Closed;
	m_closed = true;
}

void CViewContainerHost::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_pages) m_pages->SetPalette(palette);
	if (m_window) ::InvalidateRect(m_window, nullptr, FALSE);
}

bool CViewContainerHost::IsOutlineExpanded() const noexcept
{
	return m_pages && m_pages->IsOutlineExpanded();
}

void CViewContainerHost::SetOutlineExpanded(bool expanded)
{
	if (!m_pages || m_pages->IsOutlineExpanded() == expanded) return;
	m_pages->SetOutlineExpanded(expanded);

	// Retain the child/model across collapse.  On reopen, keep both the host and the
	// cached child non-painting until the child has its final bounds and visibility; this
	// prevents the old/zero rectangle from producing an intermediate empty frame.
	auto* outline = m_pages->Outline();
	const HWND outlineWindow = outline != nullptr ? outline->GetHwnd() : nullptr;
	if (m_window != nullptr) ::SendMessageW(m_window, WM_SETREDRAW, FALSE, 0);
	if (outlineWindow != nullptr) ::SendMessageW(outlineWindow, WM_SETREDRAW, FALSE, 0);
	LayoutChildren();
	if (outlineWindow != nullptr) ::SendMessageW(outlineWindow, WM_SETREDRAW, TRUE, 0);
	if (m_window != nullptr) {
		::SendMessageW(m_window, WM_SETREDRAW, TRUE, 0);
		::RedrawWindow(m_window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_NOINTERNALPAINT);
	}
	if (expanded) NotifyOutlineRevealed();
}

void CViewContainerHost::NotifyOutlineRevealed() noexcept
{
	if (m_closed || !m_outlineRevealCallback || !m_pages || !m_pages->IsUsable()
		|| !m_pages->IsOutlineExpanded() || m_page != pageIds::Explorer
		|| !OwnsPage(pageIds::Explorer)) {
		return;
	}
	try {
		m_outlineRevealCallback();
	}
	catch (...) {
		// A projection callback cannot unwind through the native window procedure. The
		// retained cached model remains the explicit visible terminal state.
	}
}

bool CViewContainerHost::RequestOutlineExpanded(bool expanded) noexcept
{
	if (m_closed || !m_pages) return false;
	if (m_pages->IsOutlineExpanded() == expanded) return true;
	if (m_outlineExpandedCallback) {
		try {
			if (!m_outlineExpandedCallback(expanded)) return false;
		}
		catch (...) {
			return false;
		}
	}
	SetOutlineExpanded(expanded);
	return true;
}

void CViewContainerHost::FocusOutline()
{
	// Outline is a View nested in the Explorer ViewContainer, so focusing it also
	// selects that container. This mirrors VS Code's `outline.focus`.
	const auto shown = ShowPage(pageIds::Explorer);
	if (shown != EViewContainerHostPageStatus::Applied
		&& shown != EViewContainerHostPageStatus::AlreadyApplied) {
		return;
	}
	// Focus is a projection/activation operation, not a user expansion request.
	SetOutlineExpanded(true);
	if (m_pages) {
		if (auto* outline = m_pages->Outline()) outline->Activate();
	}
}

EViewContainerHostPageStatus CViewContainerHost::ShowPage(
	const std::string_view containerId)
{
	if (m_closed || !m_pages || !m_pages->IsUsable()) {
		return EViewContainerHostPageStatus::InvalidHost;
	}
	std::string desired;
	try {
		desired.assign(containerId);
	} catch (...) {
		return EViewContainerHostPageStatus::AttachFailed;
	}
	if (!desired.empty() && !m_pages->Contains(desired)) {
		return EViewContainerHostPageStatus::UnknownContainer;
	}
	const auto host = PageHost();
	if (!desired.empty() && !host) return EViewContainerHostPageStatus::InvalidHost;

	if (desired.empty()) {
		std::string previous;
		try {
			previous = m_page;
		} catch (...) {
			return EViewContainerHostPageStatus::DetachFailed;
		}
		if (!previous.empty() && OwnsPage(previous)) {
			const auto detached = m_pages->Detach(previous);
			if (!detached.Succeeded()) {
				const bool previousAttached = detached.finalState
					? FinalStateOwnsHost(detached.finalState) : OwnsPage(previous);
				ProjectActualPage(desired, previous, false, previousAttached);
				return EViewContainerHostPageStatus::DetachFailed;
			}
		}
		m_page.clear();
		LayoutChildren();
		if (m_window) ::InvalidateRect(m_window, nullptr, FALSE);
		return EViewContainerHostPageStatus::Cleared;
	}
	if (m_page == desired && OwnsPage(desired)) {
		LayoutChildren();
		return EViewContainerHostPageStatus::AlreadyApplied;
	}

	std::string previous;
	try {
		previous = m_page;
	} catch (...) {
		return EViewContainerHostPageStatus::AttachFailed;
	}
	const bool detachedPrevious = !previous.empty() && previous != desired && OwnsPage(previous);
	if (detachedPrevious) {
		const auto detached = m_pages->Detach(previous);
		if (!detached.Succeeded()) {
			const bool previousAttached = detached.finalState
				? FinalStateOwnsHost(detached.finalState) : OwnsPage(previous);
			ProjectActualPage(desired, previous, false, previousAttached);
			return EViewContainerHostPageStatus::DetachFailed;
		}
	}
	const auto attached = m_pages->Attach(desired, *host);
	if (!attached.Succeeded()) {
		// A failed pool transition can still terminate with the destination attached
		// when rollback detachment fails. Never attach the previous page beside it.
		bool desiredAttached = attached.finalState
			? FinalStateOwnsHost(attached.finalState) : OwnsPage(desired);
		if (desiredAttached) {
			const auto detachedDesired = m_pages->Detach(desired);
			desiredAttached = detachedDesired.finalState
				? FinalStateOwnsHost(detachedDesired.finalState) : OwnsPage(desired);
			if (desiredAttached) {
				ProjectActualPage(desired, previous, true, false);
				return EViewContainerHostPageStatus::CompensationFailed;
			}
		}
		if (detachedPrevious) {
			const auto compensation = m_pages->Attach(previous, *host);
			if (!compensation.Succeeded()) {
				const bool previousAttached = compensation.finalState
					? FinalStateOwnsHost(compensation.finalState) : OwnsPage(previous);
				ProjectActualPage(desired, previous, false, previousAttached);
				return EViewContainerHostPageStatus::CompensationFailed;
			}
			ProjectActualPage(desired, previous, false, true);
			return EViewContainerHostPageStatus::AttachFailed;
		}
		ProjectActualPage(desired, previous, false, false);
		return EViewContainerHostPageStatus::AttachFailed;
	}
	m_page = std::move(desired);
	LayoutChildren();
	if (m_window) ::InvalidateRect(m_window, nullptr, FALSE);
	return EViewContainerHostPageStatus::Applied;
}

bool CViewContainerHost::FinalStateOwnsHost(
	const std::optional<ViewContainerPageState>& state) const noexcept
{
	return state && state->state == EViewContainerPageStableState::Attached
		&& state->host && state->host->nativeParent
		== reinterpret_cast<ViewContainerNativeHandle>(m_window);
}

void CViewContainerHost::ProjectActualPage(
	std::string& desired, std::string& previous,
	const bool desiredAttached, const bool previousAttached) noexcept
{
	if (desiredAttached && !desired.empty()) {
		m_page.swap(desired);
	} else if (previousAttached && !previous.empty()) {
		m_page.swap(previous);
	} else {
		m_page.clear();
	}
	LayoutChildren();
	if (m_window) ::InvalidateRect(m_window, nullptr, FALSE);
}

bool CViewContainerHost::OwnsPage(const std::string_view containerId) const noexcept
{
	return m_pages != nullptr && m_window != nullptr && !containerId.empty()
		&& m_pages->AttachedHost(containerId) == m_window;
}

std::optional<ViewContainerPageHost> CViewContainerHost::PageHost() const noexcept
{
	if (m_window == nullptr || !::IsWindow(m_window)) return std::nullopt;
	layout::EViewContainerLocation location;
	if (m_logicalHostId == layout::ids::part::Sidebar) {
		location = layout::EViewContainerLocation::Sidebar;
	} else if (m_logicalHostId == layout::ids::part::Auxiliarybar) {
		location = layout::EViewContainerLocation::AuxiliaryBar;
	} else {
		return std::nullopt;
	}
	try {
		return ViewContainerPageHost{ m_logicalHostId, location,
			reinterpret_cast<ViewContainerNativeHandle>(m_window) };
	} catch (...) {
		return std::nullopt;
	}
}

int CViewContainerHost::OutlineHeaderHeightPixels(unsigned int dpi) noexcept
{
	return ScaleDip(kOutlineHeaderHeightDip, dpi);
}

LRESULT CALLBACK CViewContainerHost::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CViewContainerHost*>(
			reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		if (self) self->m_window = window;
	}
	auto* self = reinterpret_cast<CViewContainerHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCDESTROY) {
		self->m_window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	return self->HandleMessage(message, wParam, lParam);
}

LRESULT CViewContainerHost::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_PAINT:
		Paint();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		// The class has no CS_HREDRAW/CS_VREDRAW, so a resize invalidates only the
		// newly exposed strip. The OUTLINE header is anchored to the bottom edge and
		// would otherwise leave its previous label painted at the old height.
		::InvalidateRect(m_window, nullptr, FALSE);
		return 0;
	case WM_LBUTTONUP: {
		if (m_page != pageIds::Explorer) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (IsOutlineHeaderPoint(point)) {
			(void)RequestOutlineExpanded(!IsOutlineExpanded());
			return 0;
		}
		break;
	}
	case WM_SETCURSOR: {
		POINT point{};
		::GetCursorPos(&point);
		::ScreenToClient(m_window, &point);
		if (IsOutlineHeaderPoint(point)) {
			::SetCursor(::LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CViewContainerHost::LayoutChildren()
{
	if (m_window == nullptr || !m_pages || !m_pages->IsUsable()) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	// The header strip is repositioned by every relayout, so the region it used to
	// occupy must be repainted as well. Invalidating only the new rectangle left the
	// previous "OUTLINE" label on screen once per layout change.
	const RECT previousHeader = m_outlineHeader;
	const auto invalidateHeaderChange = [this, &previousHeader]() noexcept {
		if (!::IsRectEmpty(&previousHeader)) ::InvalidateRect(m_window, &previousHeader, FALSE);
		if (!::IsRectEmpty(&m_outlineHeader)) ::InvalidateRect(m_window, &m_outlineHeader, FALSE);
	};

	// Never touch a page the other side bar now renders; only pages still parented here
	// are this host's to hide.
	for (const auto& id : m_pages->PageIds()) {
		if (!OwnsPage(id)) continue;
		if (m_page == id) continue;
		m_pages->SetPageVisible(id, false);
	}

	if (m_page.empty() || !OwnsPage(m_page)) {
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}
	if (m_page == pageIds::SourceControl) {
		m_pages->LayoutPage(m_page, client);
		if (auto* scm = m_pages->SourceControl()) scm->Layout(client, m_dpi);
		m_pages->SetPageVisible(pageIds::SourceControl, true);
		m_pages->NotifyPageLayout(pageIds::SourceControl);
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}
	if (m_page == pageIds::Search) {
		m_pages->LayoutPage(m_page, client);
		if (auto* view = m_pages->Search()) view->Layout(client, m_dpi);
		m_pages->SetPageVisible(pageIds::Search, true);
		m_pages->NotifyPageLayout(pageIds::Search);
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}
	if (m_page == pageIds::Extensions) {
		m_pages->LayoutPage(m_page, client);
		if (auto* view = m_pages->Extensions()) view->Layout(client, m_dpi);
		m_pages->SetPageVisible(pageIds::Extensions, true);
		m_pages->NotifyPageLayout(pageIds::Extensions);
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}
	if (m_page != pageIds::Explorer) {
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}

	auto* explorer = m_pages->Explorer();
	auto* outline = m_pages->Outline();
	if (explorer == nullptr || outline == nullptr) return;
	const bool outlineExpanded = m_pages->IsOutlineExpanded();
	const int headerHeight = OutlineHeaderHeightPixels(m_dpi);
	const int available = std::max(0L, client.bottom - client.top);
	int outlineHeight = 0;
	if (outlineExpanded) {
		const int minimum = ScaleDip(kOutlineMinimumHeightDip, m_dpi);
		outlineHeight = std::clamp(ScaleDip(kOutlinePreferredHeightDip, m_dpi), minimum,
			std::max(minimum, available / 2));
		outlineHeight = std::min(outlineHeight, std::max(0, available - headerHeight));
	}
	m_outlineHeader = { client.left, client.bottom - outlineHeight - headerHeight, client.right,
		client.bottom - outlineHeight };
	const RECT explorerBounds{ client.left, client.top, client.right, m_outlineHeader.top };
	const RECT outlineBounds{ client.left, m_outlineHeader.bottom, client.right, client.bottom };
	m_pages->LayoutPage(m_page, client, &m_outlineHeader);
	explorer->Layout(explorerBounds, m_dpi);
	outline->Layout(outlineBounds, m_dpi);
	m_pages->SetPageVisible(pageIds::Explorer, true);
	m_pages->NotifyPageLayout(pageIds::Explorer);
	invalidateHeaderChange();
}

void CViewContainerHost::Paint()
{
	PAINTSTRUCT paint{};
	const HDC target = ::BeginPaint(m_window, &paint);
	RECT client{};
	::GetClientRect(m_window, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	const bool buffered = width > 0 && height > 0 && m_backBuffer.Ensure(target, width, height);
	const HDC dc = buffered ? m_backBuffer.Dc() : target;
	const HBRUSH background = ::CreateSolidBrush(m_palette.sideBar.ToColorRef());
	::FillRect(dc, &client, background);
	::DeleteObject(background);
	if (m_font.Get()) ::SelectObject(dc, m_font.Get());
	::SetBkMode(dc, TRANSPARENT);
	// A container that the other side bar has taken over is no longer rendered here, so
	// this Part is empty even though its owner has not applied the new selection yet.
	if (m_page.empty() || !OwnsPage(m_page)) {
		const auto message = LocalizedString(STR_WORKBENCH_VIEWCONTAINER_EMPTY,
			L"Drag a view here to display it.");
		DrawCenteredMessage(dc, message);
		if (buffered) (void)m_backBuffer.Present(target, client);
		::EndPaint(m_window, &paint);
		return;
	}
	if (!::IsRectEmpty(&m_outlineHeader)) {
		const HPEN border = ::CreatePen(PS_SOLID, 1, m_palette.border.ToColorRef());
		const HGDIOBJ oldPen = ::SelectObject(dc, border);
		::MoveToEx(dc, m_outlineHeader.left, m_outlineHeader.top, nullptr);
		::LineTo(dc, m_outlineHeader.right, m_outlineHeader.top);
		::SelectObject(dc, oldPen);
		::DeleteObject(border);
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		RECT chevron = m_outlineHeader;
		chevron.left += ScaleDip(6, m_dpi);
		chevron.right = chevron.left + ScaleDip(12, m_dpi);
		const bool expanded = IsOutlineExpanded();
		const auto icon = expanded ? workbench::icons::codicons::Icon::ChevronDown
			: workbench::icons::codicons::Icon::ChevronRight;
		const auto glyphName = expanded ? L"chevron-down" : L"chevron-right";
		const auto glyph = workbench::icons::FindCodiconGlyph(glyphName);
		const workbench::icons::IconRect iconBox{
			static_cast<int>(chevron.left), static_cast<int>(chevron.top),
			static_cast<int>(chevron.right), static_cast<int>(chevron.bottom) };
		if (!PaintFontGlyph(dc, iconBox, AcquireCodiconFont(ScaleDip(16, m_dpi)),
			glyph.value_or(L'\0'), m_palette.primaryText.ToColorRef())) {
			workbench::icons::codicons::Draw(dc, iconBox, icon, m_palette.primaryText.ToColorRef());
		}
		RECT text = m_outlineHeader;
		text.left += ScaleDip(22, m_dpi);
		const auto outlineLabel = LocalizedString(STR_WORKBENCH_VIEWCONTAINER_OUTLINE, L"OUTLINE");
		::DrawTextW(dc, outlineLabel.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
	if (buffered) (void)m_backBuffer.Present(target, client);
	::EndPaint(m_window, &paint);
}

bool CViewContainerHost::IsOutlineHeaderPoint(POINT point) const noexcept
{
	return ::PtInRect(&m_outlineHeader, point) != FALSE;
}

void CViewContainerHost::DrawCenteredMessage(HDC dc, std::wstring_view message) const
{
	RECT client{};
	::GetClientRect(m_window, &client);
	const int inset = ScaleDip(kEmptyTextInsetDip, m_dpi);
	RECT text{ client.left + inset, client.top + inset, client.right - inset, client.bottom - inset };
	::SetTextColor(dc, m_palette.secondaryText.ToColorRef());
	RECT measured = text;
	::DrawTextW(dc, message.data(), static_cast<int>(message.size()), &measured,
		DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
	const int height = measured.bottom - measured.top;
	const int room = std::max(0L, text.bottom - text.top);
	if (height < room) text.top += (room - height) / 2;
	::DrawTextW(dc, message.data(), static_cast<int>(message.size()), &text, DT_WORDBREAK | DT_LEFT);
}

} // namespace workbench::viewcontainer
