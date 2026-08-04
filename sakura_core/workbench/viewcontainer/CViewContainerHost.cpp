/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/viewcontainer/CViewContainerHost.h"

#include "extension/CExtensionPane.h"
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
constexpr int kContributedViewsPreferredHeightDip = 180;
constexpr int kContributedViewsMinimumHeightDip = 96;

//! VS Code's Secondary Side Bar empty state. Matching the upstream wording keeps the
//! drop affordance discoverable instead of leaving a blank Part.
constexpr wchar_t kEmptyMessage[] = L"Drag a view here to display it.";

int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi);
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
	OutlineExpandedCallback outlineExpanded, OutlineRevealCallback outlineRevealed)
	: m_pages(std::move(pages))
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
		std::max(0L, contentRect.bottom - contentRect.top), SWP_NOACTIVATE | SWP_NOZORDER);
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
	if (!m_page || !m_pages || !m_pages->IsUsable()) return;
	if (!OwnsPage(*m_page)) return;
	switch (*m_page) {
	case ViewContainerPage::SourceControl:
		if (auto* scm = m_pages->SourceControl()) scm->Activate();
		return;
	case ViewContainerPage::Extensions:
		// VS Code's Extensions view puts the caret in its Marketplace search box.
		if (auto* marketplace = m_pages->Marketplace();
			marketplace != nullptr && marketplace->GetHwnd() != nullptr) {
			marketplace->SetFocusToSearchBox();
			return;
		}
		if (auto* extensions = m_pages->Extensions()) extensions->Activate();
		return;
	case ViewContainerPage::Explorer:
		if (auto* explorer = m_pages->Explorer()) explorer->Activate();
		return;
	case ViewContainerPage::Count:
		return;
	}
}

void CViewContainerHost::Deactivate()
{
	if (!m_pages || !m_pages->IsUsable() || !m_page || !OwnsPage(*m_page)) return;
	switch (*m_page) {
	case ViewContainerPage::SourceControl:
		if (auto* scm = m_pages->SourceControl()) scm->Deactivate();
		return;
	case ViewContainerPage::Extensions:
		if (auto* extensions = m_pages->Extensions()) extensions->Deactivate();
		return;
	case ViewContainerPage::Explorer:
		if (auto* explorer = m_pages->Explorer()) explorer->Deactivate();
		if (auto* outline = m_pages->Outline()) outline->Deactivate();
		return;
	case ViewContainerPage::Count:
		return;
	}
}

bool CViewContainerHost::PreTranslateMessage(MSG& message)
{
	if (!m_pages || !m_pages->IsUsable() || !m_page || !OwnsPage(*m_page)) return false;
	switch (*m_page) {
	case ViewContainerPage::SourceControl: {
		auto* scm = m_pages->SourceControl();
		return scm != nullptr && scm->PreTranslateMessage(message);
	}
	case ViewContainerPage::Extensions: {
		// The Marketplace is a control container, so Tab/arrow navigation between its
		// search box, list, and buttons needs dialog-message handling. Only messages that
		// really belong to it are offered, so no other surface loses its own keys.
		if (auto* marketplace = m_pages->Marketplace()) {
			const HWND pane = marketplace->GetHwnd();
			if (pane != nullptr && ::IsWindow(pane) && message.hwnd != nullptr
				&& (message.hwnd == pane || ::IsChild(pane, message.hwnd) != FALSE)
				&& ::IsDialogMessageW(pane, &message) != FALSE) {
				return true;
			}
		}
		auto* extensions = m_pages->Extensions();
		return extensions != nullptr && extensions->PreTranslateMessage(message);
	}
	case ViewContainerPage::Explorer:
		break;
	case ViewContainerPage::Count:
		return false;
	}
	auto* outline = m_pages->Outline();
	auto* explorer = m_pages->Explorer();
	return (m_pages->IsOutlineExpanded() && outline != nullptr && outline->PreTranslateMessage(message))
		|| (explorer != nullptr && explorer->PreTranslateMessage(message));
}

void CViewContainerHost::Close()
{
	if (m_closed) return;
	m_closed = true;
	// The pages are shared with the other side bar and are owned elsewhere. Detaching
	// them before this window dies keeps their HWNDs valid for the surviving host.
	if (m_pages && m_pages->IsUsable()) {
		for (std::size_t index = 0; index < kViewContainerPageCount; ++index) {
			const auto page = static_cast<ViewContainerPage>(index);
			if (m_pages->AttachedHost(page) == m_window) m_pages->Attach(page, nullptr);
		}
	}
	if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
	m_window = nullptr;
	ReleaseCodiconFont();
}

void CViewContainerHost::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_pages) m_pages->SetPalette(palette);
	if (m_window) ::InvalidateRect(m_window, nullptr, TRUE);
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
			RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE | RDW_NOINTERNALPAINT);
	}
	if (expanded) NotifyOutlineRevealed();
}

void CViewContainerHost::NotifyOutlineRevealed() noexcept
{
	if (m_closed || !m_outlineRevealCallback || !m_pages || !m_pages->IsUsable()
		|| !m_pages->IsOutlineExpanded() || m_page != ViewContainerPage::Explorer
		|| !OwnsPage(ViewContainerPage::Explorer)) {
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
	ShowPage(ViewContainerPage::Explorer);
	// Focus is a projection/activation operation, not a user expansion request.
	SetOutlineExpanded(true);
	if (m_pages) {
		if (auto* outline = m_pages->Outline()) outline->Activate();
	}
}

void CViewContainerHost::ShowPage(std::optional<ViewContainerPage> page)
{
	if (m_closed || !m_pages) return;
	if (page && *page == ViewContainerPage::Count) page.reset();
	m_page = page;
	if (m_page && m_pages->IsUsable()) m_pages->Attach(*m_page, m_window);
	LayoutChildren();
	if (m_window) ::InvalidateRect(m_window, nullptr, TRUE);
}

bool CViewContainerHost::OwnsPage(ViewContainerPage page) const noexcept
{
	return m_pages != nullptr && m_window != nullptr && m_pages->AttachedHost(page) == m_window;
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
		if (m_page != ViewContainerPage::Explorer) break;
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
		if (!::IsRectEmpty(&previousHeader)) ::InvalidateRect(m_window, &previousHeader, TRUE);
		if (!::IsRectEmpty(&m_outlineHeader)) ::InvalidateRect(m_window, &m_outlineHeader, TRUE);
	};

	// Never touch a page the other side bar now renders; only pages still parented here
	// are this host's to hide.
	for (std::size_t index = 0; index < kViewContainerPageCount; ++index) {
		const auto page = static_cast<ViewContainerPage>(index);
		if (!OwnsPage(page)) continue;
		if (m_page && *m_page == page) continue;
		m_pages->SetPageVisible(page, false);
	}

	if (!m_page || !OwnsPage(*m_page)) {
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}

	if (*m_page == ViewContainerPage::SourceControl) {
		if (auto* scm = m_pages->SourceControl()) scm->Layout(client, m_dpi);
		m_pages->SetPageVisible(ViewContainerPage::SourceControl, true);
		m_outlineHeader = {};
		invalidateHeaderChange();
		return;
	}
	if (*m_page == ViewContainerPage::Extensions) {
		LayoutExtensionsPage(client);
		m_pages->SetPageVisible(ViewContainerPage::Extensions, true);
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
	explorer->Layout(explorerBounds, m_dpi);
	outline->Layout(outlineBounds, m_dpi);
	m_pages->SetPageVisible(ViewContainerPage::Explorer, true);
	invalidateHeaderChange();
}

void CViewContainerHost::LayoutExtensionsPage(const RECT& client)
{
	// VS Code's `workbench.view.extensions` renders the Marketplace itself. A tree View
	// contributed by an extension is an additional section under it, so with nothing
	// contributed the Marketplace owns the whole container and reserves no empty strip.
	auto* marketplace = m_pages->Marketplace();
	auto* contributed = m_pages->Extensions();
	const bool hasContributed = m_pages->HasContributedExtensionViews();
	const HWND marketplaceWindow = marketplace != nullptr ? marketplace->GetHwnd() : nullptr;

	RECT marketplaceBounds = client;
	RECT contributedBounds{ client.left, client.bottom, client.right, client.bottom };
	if (marketplaceWindow == nullptr) {
		// No profile-scoped Marketplace exists in this composition, so the container has
		// only whatever extensions contributed. This is an explicit absence, not a stand-in.
		marketplaceBounds = { client.left, client.top, client.right, client.top };
		contributedBounds = client;
	} else if (hasContributed) {
		const int available = std::max(0L, client.bottom - client.top);
		const int minimum = ScaleDip(kContributedViewsMinimumHeightDip, m_dpi);
		int height = std::clamp(ScaleDip(kContributedViewsPreferredHeightDip, m_dpi), minimum,
			std::max(minimum, available / 2));
		height = std::min(height, available);
		marketplaceBounds.bottom = client.bottom - height;
		contributedBounds = { client.left, marketplaceBounds.bottom, client.right, client.bottom };
	}

	if (marketplaceWindow != nullptr && ::IsWindow(marketplaceWindow)) {
		::MoveWindow(marketplaceWindow, marketplaceBounds.left, marketplaceBounds.top,
			std::max(0L, marketplaceBounds.right - marketplaceBounds.left),
			std::max(0L, marketplaceBounds.bottom - marketplaceBounds.top), TRUE);
	}
	if (contributed != nullptr) contributed->Layout(contributedBounds, m_dpi);
}

void CViewContainerHost::Paint()
{
	PAINTSTRUCT paint{};
	const HDC dc = ::BeginPaint(m_window, &paint);
	const HBRUSH background = ::CreateSolidBrush(m_palette.sideBar.ToColorRef());
	::FillRect(dc, &paint.rcPaint, background);
	::DeleteObject(background);
	if (m_font.Get()) ::SelectObject(dc, m_font.Get());
	::SetBkMode(dc, TRANSPARENT);
	// A container that the other side bar has taken over is no longer rendered here, so
	// this Part is empty even though its owner has not applied the new selection yet.
	if (!m_page || !OwnsPage(*m_page)) {
		RECT client{};
		::GetClientRect(m_window, &client);
		const int inset = ScaleDip(kEmptyTextInsetDip, m_dpi);
		RECT text{ client.left + inset, client.top + inset, client.right - inset, client.bottom - inset };
		::SetTextColor(dc, m_palette.secondaryText.ToColorRef());
		RECT measured = text;
		::DrawTextW(dc, kEmptyMessage, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
		const int height = measured.bottom - measured.top;
		const int room = std::max(0L, text.bottom - text.top);
		if (height < room) text.top += (room - height) / 2;
		::DrawTextW(dc, kEmptyMessage, -1, &text, DT_WORDBREAK | DT_LEFT);
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
		::DrawTextW(dc, L"OUTLINE", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
	::EndPaint(m_window, &paint);
}

bool CViewContainerHost::IsOutlineHeaderPoint(POINT point) const noexcept
{
	return ::PtInRect(&m_outlineHeader, point) != FALSE;
}

} // namespace workbench::viewcontainer
