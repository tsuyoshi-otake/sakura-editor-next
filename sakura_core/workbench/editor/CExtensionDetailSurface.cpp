/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/CExtensionDetailSurface.h"

#include "markdown/CMarkdownPreviewWnd.h"
#include "workbench/extension/ExtensionIconDecoder.h"
#include "workbench/extension/ExtensionInstallButtonState.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

// AlphaBlend is the established GDI compositing call for a premultiplied-alpha
// DIB section in this codebase (see CMarkdownPreviewWnd.cpp); linking it here
// keeps the icon-paint dependency self-contained instead of touching the
// project's AdditionalDependencies.
#pragma comment(lib, "Msimg32.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"SakuraWorkbenchExtensionDetailSurface";
constexpr std::size_t kMaxReadmeCharacters = 64 * 1024;

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

//! A registry entry that simply has no README is a real answer, not a failure,
//! so it must not reach the preview: an empty parse would render an unexplained
//! blank body instead of saying so.
[[nodiscard]] bool IsBlankReadme(std::wstring_view markdown) noexcept
{
	return markdown.find_first_not_of(L" \t\r\n\f\v") == std::wstring_view::npos;
}

int ScaleValue(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? 96 : dpi) + 48) / 96);
}

std::wstring CountText(long long count)
{
	if (count >= 1000000) return std::to_wstring(count / 1000000) + L"M downloads";
	if (count >= 1000) return std::to_wstring(count / 1000) + L"K downloads";
	return std::to_wstring(std::max<long long>(0, count)) + L" downloads";
}
}

CExtensionDetailSurface::CExtensionDetailSurface()
	: CWnd(L"CExtensionDetailSurface")
{
}

//! Defined out of line so the header can forward-declare CMarkdownPreviewWnd.
//! Destroy() runs first, which joins the preview's worker and destroys its
//! window before the unique_ptr releases it.
CExtensionDetailSurface::~CExtensionDetailSurface()
{
	Destroy();
	ReleaseFont();
	ReleaseIconBitmap();
}

HWND CExtensionDetailSurface::Open(HINSTANCE hInstance, HWND hwndParent)
{
	if (hInstance == nullptr || hwndParent == nullptr || GetHwnd() != nullptr) return nullptr;
	if (RegisterWC(hInstance, nullptr, nullptr, ::LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	// No WS_VSCROLL: the metadata header has a fixed height and the README body
	// is a child window that owns its own scrollbar, exactly as VS Code's
	// extension editor keeps its header pinned while the README scrolls.
	const HWND window = Create(hwndParent, 0, kWindowClass, L"Extension Details",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 0, 0, nullptr);
	if (window == nullptr) return nullptr;
	EnsureFont();
	m_hwndClose = ::CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)), hInstance, nullptr);
	m_hwndInstall = ::CreateWindowExW(0, WC_BUTTONW, L"Install / Update", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallButtonId)), hInstance, nullptr);
	if (m_hwndClose != nullptr) ::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	if (m_hwndInstall != nullptr) ::SendMessageW(m_hwndInstall, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	if (m_hwndClose == nullptr || m_hwndInstall == nullptr) {
		Destroy();
		return nullptr;
	}
	// The README body reuses the editor's Markdown preview window. A failure to
	// create it is not fatal to the metadata surface: PublishReadme keeps the
	// typed state and PaintReadmeStatus reports it, so the extension still opens.
	auto preview = std::make_unique<markdown::CMarkdownPreviewWnd>();
	if (preview->Create(window)) {
		preview->SetPalette(m_palette);
		m_readmePreview = std::move(preview);
	}
	RefreshInstallButtonState();
	LayoutChildren();
	return window;
}

void CExtensionDetailSurface::Destroy() noexcept
{
	// Close the preview before the parent window dies so its worker thread is
	// joined while this object is still fully alive.
	if (m_readmePreview) m_readmePreview->Close();
	m_readmePreview.reset();
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) {
		CWnd::DestroyWindow();
	} else {
		_SetHwnd(nullptr);
		m_hwndClose = nullptr;
		m_hwndInstall = nullptr;
	}
	ReleaseCodiconFont();
}

void CExtensionDetailSurface::Layout(const RECT& bounds, unsigned int dpi)
{
	if (GetHwnd() == nullptr || !::IsWindow(GetHwnd())) return;
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	(void)dpi; // Child-window DPI is authoritative; WM_DPICHANGED refreshes it.
	::SetWindowPos(GetHwnd(), nullptr, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	LayoutChildren();
	::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::Show() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_SHOWNA);
}

void CExtensionDetailSurface::Hide() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_HIDE);
}

void CExtensionDetailSurface::Focus() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd())) ::SetFocus(GetHwnd());
}

bool CExtensionDetailSurface::IsVisible() const noexcept
{
	return GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd()) != FALSE;
}

void CExtensionDetailSurface::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (m_readmePreview) m_readmePreview->SetPalette(m_palette);
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::ShowExtension(const SOpenVsxExtension& extension)
{
	m_extension = extension;
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	ReleaseIconBitmap();
	m_iconState = IconState::Unsupported;
	m_installedVersion.reset();
	m_hasExtension = true;
	RefreshInstallButtonState();
	PublishReadme();
	LayoutChildren();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::ClearExtension()
{
	m_extension = {};
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	ReleaseIconBitmap();
	m_iconState = IconState::Unsupported;
	m_installedVersion.reset();
	m_hasExtension = false;
	RefreshInstallButtonState();
	PublishReadme();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeMarkdown(std::wstring markdown)
{
	m_readmeMarkdown = std::move(markdown);
	m_readmeError.clear();
	m_readmeState = ReadmeState::Ready;
	PublishReadme();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeLoading()
{
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Loading;
	PublishReadme();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeError(std::wstring message)
{
	m_readmeMarkdown.clear();
	m_readmeError = std::move(message);
	m_readmeState = ReadmeState::Error;
	PublishReadme();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeUnsupported()
{
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	PublishReadme();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetIconImage(std::vector<std::byte> encodedBytes)
{
	ReleaseIconBitmap();
	const workbench::extension::DecodedExtensionIcon decoded =
		workbench::extension::DecodeExtensionIconBitmap(encodedBytes);
	if (decoded.IsValid()) {
		m_iconBitmap = decoded.bitmap;
		m_iconWidth = decoded.width;
		m_iconHeight = decoded.height;
		m_iconState = IconState::Ready;
	} else {
		m_iconState = IconState::Error;
	}
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetIconLoading()
{
	ReleaseIconBitmap();
	m_iconState = IconState::Loading;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetIconUnsupported()
{
	ReleaseIconBitmap();
	m_iconState = IconState::Unsupported;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetInstalledVersion(std::optional<std::wstring> installedVersion)
{
	m_installedVersion = std::move(installedVersion);
	RefreshInstallButtonState();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetOnInstallRequested(InstallRequestedCallback callback)
{
	m_onInstallRequested = std::move(callback);
	RefreshInstallButtonState();
}

void CExtensionDetailSurface::SetOnCloseRequested(CloseRequestedCallback callback)
{
	m_onCloseRequested = std::move(callback);
}

unsigned int CExtensionDetailSurface::Dpi() const noexcept
{
	const UINT dpi = GetHwnd() != nullptr ? ::GetDpiForWindow(GetHwnd()) : 96;
	return dpi == 0 ? 96 : dpi;
}

int CExtensionDetailSurface::ScaleDip(int dip) const noexcept
{
	return ScaleValue(dip, Dpi());
}

void CExtensionDetailSurface::EnsureFont()
{
	if (m_font != nullptr && m_boldFont != nullptr) return;
	NONCLIENTMETRICSW metrics{ sizeof(metrics) };
	if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
	m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
	LOGFONTW bold = metrics.lfMessageFont;
	bold.lfWeight = FW_SEMIBOLD;
	m_boldFont = ::CreateFontIndirectW(&bold);
}

void CExtensionDetailSurface::ReleaseFont() noexcept
{
	if (m_font != nullptr) ::DeleteObject(m_font);
	if (m_boldFont != nullptr) ::DeleteObject(m_boldFont);
	m_font = nullptr;
	m_boldFont = nullptr;
}

HFONT CExtensionDetailSurface::AcquireCodiconFont(int height) noexcept
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

void CExtensionDetailSurface::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

void CExtensionDetailSurface::DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept
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

void CExtensionDetailSurface::ReleaseIconBitmap() noexcept
{
	if (m_iconBitmap != nullptr) ::DeleteObject(m_iconBitmap);
	m_iconBitmap = nullptr;
	m_iconWidth = 0;
	m_iconHeight = 0;
}

bool CExtensionDetailSurface::DrawIconBitmap(HDC dc, const RECT& tile) noexcept
{
	if (dc == nullptr || m_iconState != IconState::Ready || m_iconBitmap == nullptr
		|| m_iconWidth <= 0 || m_iconHeight <= 0) return false;
	const int tileWidth = static_cast<int>(tile.right - tile.left);
	const int tileHeight = static_cast<int>(tile.bottom - tile.top);
	if (tileWidth <= 0 || tileHeight <= 0) return false;

	const HDC memDc = ::CreateCompatibleDC(dc);
	if (memDc == nullptr) return false;
	const HGDIOBJ oldBitmap = ::SelectObject(memDc, m_iconBitmap);
	if (oldBitmap == nullptr || oldBitmap == HGDI_ERROR) {
		::DeleteDC(memDc);
		return false;
	}
	BLENDFUNCTION blend{};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = 255;
	blend.AlphaFormat = AC_SRC_ALPHA; // m_iconBitmap is premultiplied-alpha 32bpp BGRA
	const BOOL blended = ::AlphaBlend(dc, tile.left, tile.top, tileWidth, tileHeight,
		memDc, 0, 0, m_iconWidth, m_iconHeight, blend);
	::SelectObject(memDc, oldBitmap);
	::DeleteDC(memDc);
	return blended != FALSE;
}

void CExtensionDetailSurface::RefreshInstallButtonState() noexcept
{
	if (m_hwndInstall == nullptr) return;
	const workbench::extension::InstallButtonState state = workbench::extension::ComputeInstallButtonState(
		m_hasExtension, m_installedVersion, m_extension.sVersion, m_onInstallRequested != nullptr);
	::SetWindowTextW(m_hwndInstall, state.label.c_str());
	::EnableWindow(m_hwndInstall, state.enabled ? TRUE : FALSE);
}

void CExtensionDetailSurface::LayoutChildren()
{
	if (GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const int padding = ScaleDip(18);
	const int closeSide = ScaleDip(28);
	const int buttonWidth = ScaleDip(128);
	const int buttonHeight = ScaleDip(30);
	if (m_hwndClose != nullptr) ::SetWindowPos(m_hwndClose, nullptr, std::max(0L, client.right - padding - closeSide), padding / 2,
		closeSide, closeSide, SWP_NOZORDER | SWP_NOACTIVATE);
	if (m_hwndInstall != nullptr) ::SetWindowPos(m_hwndInstall, nullptr, std::max(0L, client.right - padding - buttonWidth), ScaleDip(64),
		buttonWidth, buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
	LayoutReadmePreview();
}

void CExtensionDetailSurface::LayoutReadmePreview()
{
	if (!m_readmePreview || GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const int padding = ScaleDip(18);
	// Measure the header and the pinned boundary footer through the same code
	// that paints them, so the body can never start or end at a different y than
	// the surrounding chrome actually occupies.
	const int bodyTop = PaintHeader(nullptr, client);
	const int bodyBottom = PaintBoundaryFooter(nullptr, client);
	const RECT body{
		padding,
		bodyTop,
		(std::max)(padding, static_cast<int>(client.right) - padding),
		(std::max)(bodyTop, bodyBottom) };
	m_readmePreview->Layout(body, Dpi());
	// A body with no room left is a normal state for a very short window; hide
	// the preview rather than leaving a zero-height child asserting a scrollbar.
	const bool renderable = m_hasExtension
		&& m_readmeState == ReadmeState::Ready
		&& !IsBlankReadme(m_readmeMarkdown)
		&& body.bottom > body.top && body.right > body.left;
	m_readmePreview->Show(renderable);
}

void CExtensionDetailSurface::PublishReadme()
{
	if (!m_readmePreview) return;
	if (m_readmeState != ReadmeState::Ready || IsBlankReadme(m_readmeMarkdown)) {
		m_readmePreview->Show(false);
		// Drop the previous extension's README so a later Ready state cannot
		// briefly show the wrong content before its own parse completes.
		m_readmePreview->SetDocument({});
		return;
	}
	std::wstring source = m_readmeMarkdown;
	const bool truncated = source.size() > kMaxReadmeCharacters;
	if (truncated) source.resize(kMaxReadmeCharacters);
	// No documentPath and no workspaceRoot: a Marketplace README has no local
	// root, so the parser classifies every image and link as external and the
	// preview reports them instead of fetching them. This surface performs no
	// network access, and that boundary is what keeps it true.
	markdown::ParseOptions options;
	options.frontMatterMode = markdown::FrontMatterMode::Hide;
	// The generation alone orders renders here; there is no editable buffer to
	// carry a revision, so it stays 0 and only the generation moves.
	const markdown::PreviewRenderKey key{ ++m_readmeGeneration, 0 };
	if (!m_readmePreview->QueueDocument(std::move(source), std::move(options), truncated, key)) {
		m_readmePreview->Show(false);
		return;
	}
	LayoutReadmePreview();
}

void CExtensionDetailSurface::PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold)
{
	// A null dc means the caller is measuring the layout, not drawing it. Every
	// advance in PaintHeader/PaintBoundaryFooter is a plain arithmetic step, so
	// swallowing the draw here is what lets one code path serve both passes.
	if (dc == nullptr) return;
	::SetTextColor(dc, color);
	::SetBkMode(dc, TRANSPARENT);
	const HGDIOBJ old = ::SelectObject(dc, bold ? m_boldFont : m_font);
	::DrawTextW(dc, text, -1, &bounds, format);
	if (old != nullptr) ::SelectObject(dc, old);
}

void CExtensionDetailSurface::PaintSectionHeading(HDC dc, const wchar_t* text, int left, int* top, int right)
{
	RECT heading{ left, *top, right, *top + ScaleDip(26) };
	PaintText(dc, text, heading, m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE, true);
	*top += ScaleDip(32);
}

int CExtensionDetailSurface::PaintHeader(HDC dc, const RECT& client)
{
	const int padding = ScaleDip(18);
	const int contentRight = (std::max)(padding, static_cast<int>(client.right) - padding);
	int top = ScaleDip(58);

	if (dc != nullptr) {
		RECT header{ 0, 0, client.right, ScaleDip(44) };
		const HBRUSH headerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
		::FillRect(dc, &header, headerBrush);
		::DeleteObject(headerBrush);
	}
	const std::wstring headerText = m_hasExtension
		? (L"Extension: " + (m_extension.sDisplayName.empty() ? m_extension.sName : m_extension.sDisplayName))
		: L"Extension";
	PaintText(dc, headerText.c_str(),
		RECT{ padding, 0, (std::max)(padding, static_cast<int>(client.right) - padding * 3), ScaleDip(44) },
		m_palette.primaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, true);
	PaintText(dc, L"Details", RECT{ padding, top, contentRight, top + ScaleDip(30) },
		m_palette.disabledText.ToColorRef(), DT_LEFT | DT_SINGLELINE, false);
	top += ScaleDip(42);
	if (!m_hasExtension) return top;

	const std::wstring displayName = m_extension.sDisplayName.empty() ? m_extension.sName : m_extension.sDisplayName;
	const int heroRight = (std::max)(padding, contentRight - ScaleDip(140));
	const int tileSide = ScaleDip(64);
	const RECT tile{ padding, top, padding + tileSide, top + tileSide };
	if (dc != nullptr && !DrawIconBitmap(dc, tile)) {
		// No decoded icon (Unsupported/Loading/Error) falls back to an initials
		// tile rather than leaving a blank hole or a fake image.
		RECT tileFill = tile;
		const HBRUSH tileBrush = ::CreateSolidBrush(m_palette.accent.ToColorRef());
		::FillRect(dc, &tileFill, tileBrush);
		::DeleteObject(tileBrush);
		std::wstring initials;
		for (const wchar_t character : displayName) {
			if (character == L' ' || character == L'-' || character == L'_') continue;
			initials += character;
			if (initials.size() == 2) break;
		}
		if (initials.empty()) initials = L"?";
		PaintText(dc, initials.c_str(), tile, m_palette.highlightText.ToColorRef(), DT_CENTER | DT_VCENTER | DT_SINGLELINE, true);
	}
	const int textLeft = tile.right + ScaleDip(14);
	PaintText(dc, displayName.c_str(), RECT{ textLeft, top, heroRight, top + ScaleDip(38) },
		m_palette.primaryText.ToColorRef(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, true);
	top += ScaleDip(34);
	const std::wstring publisher = (m_extension.bVerified ? L"\u2713 Verified " : L"") + m_extension.sNamespace;
	PaintText(dc, publisher.c_str(), RECT{ textLeft, top, heroRight, top + ScaleDip(24) },
		m_extension.bVerified ? RGB(88, 166, 255) : m_palette.secondaryText.ToColorRef(),
		DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
	const std::wstring version = L"Version " + (m_extension.sVersion.empty() ? L"\u2014" : m_extension.sVersion) + L"   \u2022   " + CountText(m_extension.nDownloadCount) +
		(m_extension.HasRating() ? L"   \u2022   \u2605 " + std::to_wstring(m_extension.dAverageRating).substr(0, 3) : L"");
	PaintText(dc, version.c_str(), RECT{ textLeft, top + ScaleDip(24), heroRight, top + ScaleDip(48) },
		m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
	top += ScaleDip(70);
	if (m_extension.bDeprecated) {
		// Wording matches VS Code's DeprecatedExtensionsView banner base string
		// (extensionsWidgets.ts): "This extension is deprecated as it is no
		// longer being maintained." Real VS Code appends an extension/setting
		// migration suggestion when the registry supplies one; this DTO does
		// not carry that structured migration data, so only the base sentence
		// is shown rather than fabricating a suggestion.
		const int bannerHeight = ScaleDip(36);
		if (dc != nullptr) {
			RECT banner{ padding, top, contentRight, top + bannerHeight };
			const HBRUSH bannerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
			::FillRect(dc, &banner, bannerBrush);
			::DeleteObject(bannerBrush);
		}
		PaintText(dc, L"\u26a0 This extension is deprecated as it is no longer being maintained.",
			RECT{ padding + ScaleDip(10), top, contentRight - ScaleDip(10), top + bannerHeight },
			m_palette.danger.ToColorRef(), DT_LEFT | DT_VCENTER | DT_WORDBREAK);
		top += bannerHeight + ScaleDip(14);
	}
	PaintSectionHeading(dc, L"DETAILS", padding, &top, contentRight);
	if (!m_extension.sDescription.empty()) {
		PaintText(dc, m_extension.sDescription.c_str(), RECT{ padding, top, contentRight, top + ScaleDip(72) },
			m_palette.primaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
		top += ScaleDip(86);
	}
	return top;
}

int CExtensionDetailSurface::PaintBoundaryFooter(HDC dc, const RECT& client)
{
	if (!m_hasExtension) return (std::max)(0, static_cast<int>(client.bottom));
	const int padding = ScaleDip(18);
	const int contentRight = (std::max)(padding, static_cast<int>(client.right) - padding);

	// FEATURES: SOpenVsxExtension carries no contributed-command/configuration
	// data at all (see OpenVsxProtocol.h), so this is an invariant capability
	// boundary rather than a per-extension fact. CHANGELOG differs: the registry
	// DTO does carry sChangelogUrl, so branch on it instead of unconditionally
	// claiming nothing exists -- this class still never fetches remote content,
	// so a present URL is reported as an explicit "not fetched" boundary.
	// Both stay pinned to the bottom rather than scrolling away inside the
	// README, per this directory's CLAUDE.md ("must remain explicit").
	const int rowHeight = ScaleDip(20);
	const int footerHeight = ScaleDip(12) + rowHeight * 2 + ScaleDip(8);
	const int footerTop = (std::max)(0, static_cast<int>(client.bottom) - footerHeight);
	if (dc != nullptr) {
		const HPEN pen = ::CreatePen(PS_SOLID, 1, m_palette.border.ToColorRef());
		const HGDIOBJ oldPen = ::SelectObject(dc, pen);
		::MoveToEx(dc, padding, footerTop, nullptr);
		::LineTo(dc, contentRight, footerTop);
		if (oldPen != nullptr) ::SelectObject(dc, oldPen);
		::DeleteObject(pen);
	}
	const int labelWidth = ScaleDip(84);
	int rowTop = footerTop + ScaleDip(12);
	const auto paintRow = [&](const wchar_t* label, const wchar_t* text) {
		PaintText(dc, label, RECT{ padding, rowTop, padding + labelWidth, rowTop + rowHeight },
			m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE, true);
		PaintText(dc, text, RECT{ padding + labelWidth, rowTop, contentRight, rowTop + rowHeight },
			m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		rowTop += rowHeight;
	};
	paintRow(L"FEATURES", L"This view does not read extension-contributed commands or configuration.");
	paintRow(L"CHANGELOG", m_extension.sChangelogUrl.empty()
		? L"No changelog was supplied by this extension's registry entry."
		: L"A changelog is available upstream; this view does not fetch remote content.");
	// EXTENSION PACK: the DTO has no extensionPack field in any state, so unlike
	// FEATURES/CHANGELOG there is no fact to report at all. Real VS Code only
	// renders that tab when the manifest declares a pack; omit the row entirely
	// rather than printing an eternally-static placeholder under a label that
	// implies a checked-and-empty result.
	return footerTop;
}

void CExtensionDetailSurface::PaintReadmeStatus(HDC dc, const RECT& body)
{
	if (body.bottom <= body.top || body.right <= body.left) return;
	std::wstring message;
	COLORREF color = m_palette.secondaryText.ToColorRef();
	switch (m_readmeState) {
	case ReadmeState::Loading:
		message = L"Loading README\u2026";
		break;
	case ReadmeState::Error:
		message = m_readmeError.empty() ? L"README could not be loaded." : m_readmeError;
		color = m_palette.danger.ToColorRef();
		break;
	case ReadmeState::Unsupported:
		message = L"README/Markdown content was not supplied by the extension model.";
		color = m_palette.disabledText.ToColorRef();
		break;
	case ReadmeState::Ready:
		// A Ready-but-blank README is a real registry answer rather than a
		// failure, and the preview child is hidden for it, so report the fact
		// instead of leaving an unexplained empty body.
		if (!IsBlankReadme(m_readmeMarkdown)) return;
		message = L"This extension did not provide README content.";
		color = m_palette.disabledText.ToColorRef();
		break;
	}
	PaintText(dc, message.c_str(), body, color, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

void CExtensionDetailSurface::Paint()
{
	PAINTSTRUCT ps{};
	const HDC dc = ::BeginPaint(GetHwnd(), &ps);
	if (dc == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const HBRUSH background = ::CreateSolidBrush(m_palette.canvas.ToColorRef());
	::FillRect(dc, &client, background);
	::DeleteObject(background);
	const int padding = ScaleDip(18);
	const int bodyTop = PaintHeader(dc, client);
	const int bodyBottom = PaintBoundaryFooter(dc, client);
	if (m_hasExtension) {
		const RECT body{
			padding,
			bodyTop,
			(std::max)(padding, static_cast<int>(client.right) - padding),
			(std::max)(bodyTop, bodyBottom) };
		// Ready content belongs to the preview child, which WS_CLIPCHILDREN keeps
		// this pass out of; only the states it cannot render are painted here.
		PaintReadmeStatus(dc, body);
	}
	::EndPaint(GetHwnd(), &ps);
}

void CExtensionDetailSurface::InvokeInstall()
{
	if (!m_hasExtension || !m_onInstallRequested) return;
	try { m_onInstallRequested(); } catch (...) { /* callbacks are composition-owned; keep the surface alive */ }
}

void CExtensionDetailSurface::InvokeClose()
{
	if (!m_onCloseRequested) return;
	try { m_onCloseRequested(); } catch (...) { /* callbacks are composition-owned; keep the surface alive */ }
}

LRESULT CExtensionDetailSurface::DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: Paint(); return 0;
	case WM_DRAWITEM:
		if (wp == static_cast<WPARAM>(kCloseButtonId) && lp != 0) {
			DrawCloseButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
			return TRUE;
		}
		break;
	case WM_SIZE: LayoutChildren(); return 0;
	// No WM_VSCROLL/WM_MOUSEWHEEL here: the README body is the shared Markdown
	// preview child, which owns its own scrollbar and wheel handling. Adding a
	// second scroll authority on the parent would fight it.
	case WM_DPICHANGED:
		ReleaseFont();
		ReleaseCodiconFont();
		EnsureFont();
		if (m_hwndClose != nullptr) ::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		if (m_hwndInstall != nullptr) ::SendMessageW(m_hwndInstall, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		LayoutChildren();
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wp) == kInstallButtonId && HIWORD(wp) == BN_CLICKED) InvokeInstall();
		if (LOWORD(wp) == kCloseButtonId && HIWORD(wp) == BN_CLICKED) InvokeClose();
		return 0;
	case WM_SETFOCUS: m_focused = true; ::InvalidateRect(hwnd, nullptr, FALSE); return 0;
	case WM_KILLFOCUS: m_focused = false; ::InvalidateRect(hwnd, nullptr, FALSE); return 0;
	case WM_NCDESTROY:
		m_hwndClose = nullptr;
		m_hwndInstall = nullptr;
		_SetHwnd(nullptr);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	default: break;
	}
	return CWnd::DispatchEvent(hwnd, msg, wp, lp);
}
