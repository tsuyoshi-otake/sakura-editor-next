/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/CWorkspaceTrustEditorSurface.h"

#include "util/string_ex.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <utility>

namespace {
constexpr wchar_t kWindowClass[] = L"SakuraWorkbenchWorkspaceTrustEditorSurface";

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

int ScaleValue(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? 96 : dpi) + 48) / 96);
}
}

CWorkspaceTrustEditorSurface::CWorkspaceTrustEditorSurface()
	: CWnd(L"CWorkspaceTrustEditorSurface")
{
}

CWorkspaceTrustEditorSurface::~CWorkspaceTrustEditorSurface()
{
	Destroy();
	ReleaseFont();
}

HWND CWorkspaceTrustEditorSurface::Open(HINSTANCE hInstance, HWND hwndParent)
{
	if (hInstance == nullptr || hwndParent == nullptr || GetHwnd() != nullptr) return nullptr;
	if (RegisterWC(hInstance, nullptr, nullptr, ::LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	const HWND window = Create(hwndParent, 0, kWindowClass, L"Workspace Trust",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 0, 0, nullptr);
	if (window == nullptr) return nullptr;
	EnsureFont();
	m_hwndClose = ::CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)), hInstance, nullptr);
	if (m_hwndClose != nullptr) ::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	if (m_hwndClose == nullptr) {
		Destroy();
		return nullptr;
	}
	if (m_hasPrompt) RebuildGrantButtons();
	LayoutChildren();
	return window;
}

void CWorkspaceTrustEditorSurface::Destroy() noexcept
{
	DestroyGrantButtons();
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) {
		CWnd::DestroyWindow();
	} else {
		_SetHwnd(nullptr);
		m_hwndClose = nullptr;
	}
	ReleaseCodiconFont();
}

void CWorkspaceTrustEditorSurface::Layout(const RECT& bounds, unsigned int dpi)
{
	if (GetHwnd() == nullptr || !::IsWindow(GetHwnd())) return;
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	(void)dpi; // Child-window DPI is authoritative; WM_DPICHANGED refreshes it.
	::SetWindowPos(GetHwnd(), nullptr, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	LayoutChildren();
	::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CWorkspaceTrustEditorSurface::Show() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_SHOWNA);
}

void CWorkspaceTrustEditorSurface::Hide() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_HIDE);
}

void CWorkspaceTrustEditorSurface::Focus() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd())) ::SetFocus(GetHwnd());
}

bool CWorkspaceTrustEditorSurface::IsVisible() const noexcept
{
	return GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd()) != FALSE;
}

void CWorkspaceTrustEditorSurface::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CWorkspaceTrustEditorSurface::ShowPrompt(workbench::WorkspaceTrustPromptModel model)
{
	m_promptModel = std::move(model);
	m_hasPrompt = true;
	// A fresh model describes the current state; a result banner from a prior
	// prompt (possibly for a different window/workspace) must not linger and
	// misdescribe it.
	m_grantResult.reset();
	RebuildGrantButtons();
	LayoutChildren();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CWorkspaceTrustEditorSurface::ClearPrompt()
{
	m_promptModel = {};
	m_hasPrompt = false;
	m_grantResult.reset();
	DestroyGrantButtons();
	LayoutChildren();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CWorkspaceTrustEditorSurface::SetGrantResult(workbench::WorkspaceTrustGrantResult result)
{
	m_grantResult = std::move(result);
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CWorkspaceTrustEditorSurface::SetOnGrantRequested(GrantRequestedCallback callback)
{
	m_onGrantRequested = std::move(callback);
}

void CWorkspaceTrustEditorSurface::SetOnCloseRequested(CloseRequestedCallback callback)
{
	m_onCloseRequested = std::move(callback);
}

unsigned int CWorkspaceTrustEditorSurface::Dpi() const noexcept
{
	const UINT dpi = GetHwnd() != nullptr ? ::GetDpiForWindow(GetHwnd()) : 96;
	return dpi == 0 ? 96 : dpi;
}

int CWorkspaceTrustEditorSurface::ScaleDip(int dip) const noexcept
{
	return ScaleValue(dip, Dpi());
}

void CWorkspaceTrustEditorSurface::EnsureFont()
{
	if (m_font != nullptr && m_boldFont != nullptr) return;
	NONCLIENTMETRICSW metrics{ sizeof(metrics) };
	if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
	m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
	LOGFONTW bold = metrics.lfMessageFont;
	bold.lfWeight = FW_SEMIBOLD;
	m_boldFont = ::CreateFontIndirectW(&bold);
}

void CWorkspaceTrustEditorSurface::ReleaseFont() noexcept
{
	if (m_font != nullptr) ::DeleteObject(m_font);
	if (m_boldFont != nullptr) ::DeleteObject(m_boldFont);
	m_font = nullptr;
	m_boldFont = nullptr;
}

HFONT CWorkspaceTrustEditorSurface::AcquireCodiconFont(int height) noexcept
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

void CWorkspaceTrustEditorSurface::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

int CWorkspaceTrustEditorSurface::GrantButtonId(std::size_t index) const noexcept
{
	// Grant IDs occupy the small block starting at kGrantButtonBaseId. That
	// block also contains kCloseButtonId, so any index whose natural ID would
	// land on it is pushed one further, keeping every grant button ID distinct
	// from the fixed close-button ID no matter how many options are offered.
	int id = kGrantButtonBaseId + static_cast<int>(index);
	if (id >= kCloseButtonId) id += 1;
	return id;
}

bool CWorkspaceTrustEditorSurface::OffersGrant() const noexcept
{
	// Mirrors CEditWnd::ExecuteManageWorkspaceTrust: only an untrusted window
	// with a writable trusted-folders list and at least one nameable resource
	// gets a grant affordance. Every other state is informational only.
	return m_hasPrompt
		&& m_promptModel.state != config::EWorkspaceTrustState::Trusted
		&& m_promptModel.persistenceReady
		&& !m_promptModel.options.empty();
}

void CWorkspaceTrustEditorSurface::DestroyGrantButtons() noexcept
{
	for (auto& button : m_grantButtons) {
		if (button.hwnd != nullptr && ::IsWindow(button.hwnd)) ::DestroyWindow(button.hwnd);
	}
	m_grantButtons.clear();
}

void CWorkspaceTrustEditorSurface::RebuildGrantButtons()
{
	DestroyGrantButtons();
	if (GetHwnd() == nullptr || !OffersGrant()) return;
	const HINSTANCE hInstance = GetAppInstance();
	m_grantButtons.reserve(m_promptModel.options.size());
	for (std::size_t index = 0; index < m_promptModel.options.size(); ++index) {
		const auto& option = m_promptModel.options[index];
		SGrantButton button;
		button.id = GrantButtonId(index);
		button.option = option;
		// The heading names the category of grant; option.displayUri (drawn by
		// DrawGrantButton beneath it) names the exact resource that grant would
		// write, exactly as CEditWnd::ExecuteManageWorkspaceTrust's command-link
		// labels do. Consent must name a resource, not only a category.
		button.heading = option.scope == workbench::EWorkspaceTrustGrantScope::ParentFolder
			? L"Trust the authors of all files in the parent folder"
			: (option.resourceCount > 1
					  ? L"Yes, I trust the authors of every folder in this workspace"
					  : L"Yes, I trust the authors");
		button.hwnd = ::CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
			0, 0, 0, 0, GetHwnd(), reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), hInstance, nullptr);
		if (button.hwnd == nullptr) continue;
		if (m_font != nullptr) ::SendMessageW(button.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		m_grantButtons.push_back(std::move(button));
	}
}

void CWorkspaceTrustEditorSurface::DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept
{
	const bool selected = (draw.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
	const COLORREF backgroundColor = selected ? m_palette.panel.ToColorRef() : m_palette.raised.ToColorRef();
	if (const HBRUSH background = ::CreateSolidBrush(backgroundColor); background != nullptr) {
		::FillRect(draw.hDC, &draw.rcItem, background);
		::DeleteObject(background);
	}
	if ((draw.itemState & ODS_FOCUS) != 0) {
		if (const HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef()); border != nullptr) {
			::FrameRect(draw.hDC, &draw.rcItem, border);
			::DeleteObject(border);
		}
	}
	const int width = static_cast<int>(draw.rcItem.right - draw.rcItem.left);
	const int height = static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
	const int side = (std::min)(ScaleDip(18), (std::min)(width, height));
	const workbench::icons::IconRect iconBox{
		draw.rcItem.left + (width - side) / 2,
		draw.rcItem.top + (height - side) / 2,
		draw.rcItem.left + (width - side) / 2 + side,
		draw.rcItem.top + (height - side) / 2 + side };
	const auto glyph = workbench::icons::FindCodiconGlyph(L"close");
	const COLORREF color = m_palette.secondaryText.ToColorRef();
	if (!PaintFontGlyph(draw.hDC, iconBox, AcquireCodiconFont(side), glyph.value_or(L'\0'), color)) {
		workbench::icons::codicons::Draw(draw.hDC, iconBox,
			workbench::icons::codicons::Icon::Close, color);
	}
}

bool CWorkspaceTrustEditorSurface::DrawGrantButton(const DRAWITEMSTRUCT& draw) noexcept
{
	const SGrantButton* found = nullptr;
	for (const auto& button : m_grantButtons) {
		if (button.hwnd == draw.hwndItem) {
			found = &button;
			break;
		}
	}
	if (found == nullptr) return false;

	// Prominent action: owner-drawn with the button role colors rather than
	// the extension surface's system-drawn Install button, per this surface's
	// requirement that the trust grant reads as the prominent action it is.
	const bool hot = (draw.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
	const COLORREF background = hot
		? m_palette.buttonHoverBackground.ToColorRef()
		: m_palette.buttonBackground.ToColorRef();
	if (const HBRUSH brush = ::CreateSolidBrush(background); brush != nullptr) {
		::FillRect(draw.hDC, &draw.rcItem, brush);
		::DeleteObject(brush);
	}
	if ((draw.itemState & ODS_FOCUS) != 0) {
		RECT focusRect = draw.rcItem;
		::InflateRect(&focusRect, -2, -2);
		::DrawFocusRect(draw.hDC, &focusRect);
	}
	const int padding = ScaleDip(10);
	const RECT headingRect{
		draw.rcItem.left + padding, draw.rcItem.top + ScaleDip(6),
		draw.rcItem.right - padding, draw.rcItem.top + ScaleDip(24) };
	const RECT uriRect{
		draw.rcItem.left + padding, draw.rcItem.top + ScaleDip(26),
		draw.rcItem.right - padding, draw.rcItem.bottom - ScaleDip(4) };
	PaintText(draw.hDC, found->heading.c_str(), headingRect, m_palette.buttonForeground.ToColorRef(),
		DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, true);
	PaintText(draw.hDC, found->option.displayUri.c_str(), uriRect, m_palette.buttonForeground.ToColorRef(),
		DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, false);
	return true;
}

void CWorkspaceTrustEditorSurface::LayoutChildren()
{
	if (GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const int padding = ScaleDip(18);
	const int closeSide = ScaleDip(28);
	if (m_hwndClose != nullptr) {
		::SetWindowPos(m_hwndClose, nullptr, std::max(0L, client.right - padding - closeSide), padding / 2,
			closeSide, closeSide, SWP_NOZORDER | SWP_NOACTIVATE);
	}

	const int bodyTop = PaintHeader(nullptr, client);
	std::vector<RECT> buttonRects;
	PaintBody(nullptr, client, bodyTop, buttonRects);
	for (std::size_t index = 0; index < m_grantButtons.size(); ++index) {
		if (m_grantButtons[index].hwnd == nullptr) continue;
		if (index < buttonRects.size()) {
			const RECT& rect = buttonRects[index];
			::SetWindowPos(m_grantButtons[index].hwnd, nullptr, rect.left, rect.top,
				rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
		} else {
			// The model changed under us between RebuildGrantButtons and this
			// layout pass in a way that produced fewer rectangles than
			// buttons. Hide rather than leave a stale button at an old
			// coordinate: an invisible extra control is safer than a visible
			// one whose label no longer matches what a click would grant.
			::SetWindowPos(m_grantButtons[index].hwnd, nullptr, 0, 0, 0, 0,
				SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
		}
	}
}

int CWorkspaceTrustEditorSurface::PaintHeader(HDC dc, const RECT& client)
{
	const int padding = ScaleDip(18);
	if (dc != nullptr) {
		RECT header{ 0, 0, client.right, ScaleDip(44) };
		const HBRUSH headerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
		::FillRect(dc, &header, headerBrush);
		::DeleteObject(headerBrush);
	}
	PaintText(dc, L"Workspace Trust",
		RECT{ padding, 0, (std::max)(padding, static_cast<int>(client.right) - padding * 3), ScaleDip(44) },
		m_palette.primaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, true);
	return ScaleDip(44) + ScaleDip(18);
}

void CWorkspaceTrustEditorSurface::PaintBody(HDC dc, const RECT& client, int top, std::vector<RECT>& buttonRects)
{
	buttonRects.clear();
	const int padding = ScaleDip(18);
	const int contentRight = (std::max)(padding, static_cast<int>(client.right) - padding);

	if (!m_hasPrompt) {
		// No model has ever been projected. This is an explicit fail-closed
		// state -- the page has nothing to show because the host has not
		// supplied one -- rather than a silently blank body.
		PaintText(dc, L"Workspace trust information is not available for this window.",
			RECT{ padding, top, contentRight, top + ScaleDip(40) },
			m_palette.disabledText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK);
		return;
	}

	// Every branch reports a real state. None of them fabricates a grantable
	// choice to keep the page from looking empty; this mirrors
	// CEditWnd::ExecuteManageWorkspaceTrust's instruction/content selection.
	const bool alreadyTrusted = m_promptModel.state == config::EWorkspaceTrustState::Trusted;
	std::wstring instruction;
	std::wstring content;
	if (alreadyTrusted) {
		instruction = L"You trust the authors of the files in this window.";
	} else if (!m_promptModel.persistenceReady) {
		instruction = L"Workspace trust cannot be granted right now.";
		content = L"The Trusted Folders and Workspaces list could not be read, so a grant "
				  L"would not survive this session. No trust decision was recorded.";
	} else if (m_promptModel.options.empty()) {
		instruction = L"There is nothing in this window to trust.";
		content = L"Open a folder or a workspace first. Trust applies to files on disk, so an "
				  L"empty window has no resource a decision could name.";
	} else {
		instruction = L"Do you trust the authors of the files in this window?";
		content = L"Until you do, this window stays in Restricted Mode: extension code that "
				  L"declares it needs a trusted workspace does not run.";
	}

	PaintText(dc, instruction.c_str(), RECT{ padding, top, contentRight, top + ScaleDip(30) },
		m_palette.primaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK, true);
	top += ScaleDip(36);
	if (!content.empty()) {
		PaintText(dc, content.c_str(), RECT{ padding, top, contentRight, top + ScaleDip(56) },
			m_palette.descriptionText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK);
		top += ScaleDip(64);
	} else {
		top += ScaleDip(8);
	}

	if (OffersGrant()) {
		const int buttonWidth = (std::min)(ScaleDip(460), (std::max)(0, contentRight - padding));
		const int buttonHeight = ScaleDip(48);
		const int gap = ScaleDip(10);
		for (std::size_t index = 0; index < m_promptModel.options.size(); ++index) {
			const RECT buttonRect{ padding, top, padding + buttonWidth, top + buttonHeight };
			buttonRects.push_back(buttonRect);
			top += buttonHeight + gap;
		}
	}

	if (m_grantResult.has_value()) {
		top += ScaleDip(8);
		std::wstring resultText;
		COLORREF resultColor = m_palette.primaryText.ToColorRef();
		// Every EWorkspaceTrustGrantStatus maps to a distinct, honest message.
		// Only Granted/AlreadyTrusted are successes; every other status states
		// the failure rather than ever saying "granted".
		switch (m_grantResult->status) {
		case workbench::EWorkspaceTrustGrantStatus::Granted:
			resultText = L"Trust was granted.";
			break;
		case workbench::EWorkspaceTrustGrantStatus::AlreadyTrusted:
			resultText = L"This resource was already trusted; nothing new was written.";
			break;
		case workbench::EWorkspaceTrustGrantStatus::NotApplicable:
			resultText = L"Nothing about this window could be trusted through that choice.";
			resultColor = m_palette.danger.ToColorRef();
			break;
		case workbench::EWorkspaceTrustGrantStatus::PersistenceUnavailable:
			resultText = L"The decision could not be saved: the trusted-folders list is unavailable.";
			resultColor = m_palette.danger.ToColorRef();
			break;
		case workbench::EWorkspaceTrustGrantStatus::Conflict:
			resultText = L"The trusted-folders list changed elsewhere; the grant was not applied.";
			resultColor = m_palette.danger.ToColorRef();
			break;
		case workbench::EWorkspaceTrustGrantStatus::Stopped:
			resultText = L"The workbench stopped before the grant could complete.";
			resultColor = m_palette.danger.ToColorRef();
			break;
		case workbench::EWorkspaceTrustGrantStatus::Failed:
		default:
			resultText = L"The trust grant failed.";
			resultColor = m_palette.danger.ToColorRef();
			break;
		}
		if (!m_grantResult->diagnostic.empty()) {
			resultText += L" (";
			resultText += u8stowcs(m_grantResult->diagnostic);
			resultText += L")";
		}
		PaintText(dc, resultText.c_str(), RECT{ padding, top, contentRight, top + ScaleDip(40) },
			resultColor, DT_LEFT | DT_TOP | DT_WORDBREAK);
		top += ScaleDip(44);
	}
}

void CWorkspaceTrustEditorSurface::PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold)
{
	// A null dc means the caller is measuring layout, not drawing it -- see
	// LayoutChildren, which calls PaintHeader/PaintBody with dc == nullptr so
	// the button rectangles it computes can never disagree with what Paint()
	// actually draws.
	if (dc == nullptr) return;
	::SetTextColor(dc, color);
	::SetBkMode(dc, TRANSPARENT);
	const HGDIOBJ old = ::SelectObject(dc, bold ? m_boldFont : m_font);
	::DrawTextW(dc, text, -1, &bounds, format);
	if (old != nullptr) ::SelectObject(dc, old);
}

void CWorkspaceTrustEditorSurface::Paint()
{
	PAINTSTRUCT ps{};
	const HDC dc = ::BeginPaint(GetHwnd(), &ps);
	if (dc == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const HBRUSH background = ::CreateSolidBrush(m_palette.canvas.ToColorRef());
	::FillRect(dc, &client, background);
	::DeleteObject(background);
	const int bodyTop = PaintHeader(dc, client);
	std::vector<RECT> buttonRects;
	// Grant buttons paint themselves through WM_DRAWITEM; buttonRects is only
	// needed here to keep the same call shape LayoutChildren uses, so this
	// pass and the layout pass measure text identically.
	PaintBody(dc, client, bodyTop, buttonRects);
	::EndPaint(GetHwnd(), &ps);
}

void CWorkspaceTrustEditorSurface::InvokeGrant(workbench::EWorkspaceTrustGrantScope scope)
{
	if (!m_onGrantRequested) return;
	try { m_onGrantRequested(scope); } catch (...) { /* callbacks are composition-owned; keep the surface alive */ }
}

void CWorkspaceTrustEditorSurface::InvokeClose()
{
	if (!m_onCloseRequested) return;
	try { m_onCloseRequested(); } catch (...) { /* callbacks are composition-owned; keep the surface alive */ }
}

LRESULT CWorkspaceTrustEditorSurface::DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: Paint(); return 0;
	case WM_DRAWITEM:
		if (wp == static_cast<WPARAM>(kCloseButtonId) && lp != 0) {
			DrawCloseButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
			return TRUE;
		}
		if (lp != 0) {
			const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
			if (DrawGrantButton(draw)) return TRUE;
		}
		break;
	case WM_SIZE: LayoutChildren(); return 0;
	case WM_DPICHANGED:
		ReleaseFont();
		ReleaseCodiconFont();
		EnsureFont();
		if (m_hwndClose != nullptr) ::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		for (const auto& button : m_grantButtons) {
			if (button.hwnd != nullptr) ::SendMessageW(button.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		}
		LayoutChildren();
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wp) == kCloseButtonId && HIWORD(wp) == BN_CLICKED) {
			InvokeClose();
			return 0;
		}
		if (HIWORD(wp) == BN_CLICKED) {
			for (const auto& button : m_grantButtons) {
				if (button.id == static_cast<int>(LOWORD(wp))) {
					InvokeGrant(button.option.scope);
					break;
				}
			}
		}
		return 0;
	case WM_SETFOCUS: m_focused = true; ::InvalidateRect(hwnd, nullptr, FALSE); return 0;
	case WM_KILLFOCUS: m_focused = false; ::InvalidateRect(hwnd, nullptr, FALSE); return 0;
	case WM_NCDESTROY:
		m_hwndClose = nullptr;
		for (auto& button : m_grantButtons) button.hwnd = nullptr;
		m_grantButtons.clear();
		_SetHwnd(nullptr);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	default: break;
	}
	return CWnd::DispatchEvent(hwnd, msg, wp, lp);
}
