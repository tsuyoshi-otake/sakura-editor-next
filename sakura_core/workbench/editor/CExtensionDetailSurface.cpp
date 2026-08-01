/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/CExtensionDetailSurface.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"SakuraWorkbenchExtensionDetailSurface";
constexpr std::size_t kMaxReadmeCharacters = 64 * 1024;

enum class MarkdownBlockKind { Paragraph, Heading, ListItem, Code, Rule };

struct MarkdownBlock {
	MarkdownBlockKind kind = MarkdownBlockKind::Paragraph;
	std::wstring text;
	int level = 0;
	bool ordered = false;
	int number = 0;
};

std::wstring_view Trim(std::wstring_view value) noexcept
{
	while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
	while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r')) value.remove_suffix(1);
	return value;
}

bool IsRule(std::wstring_view line) noexcept
{
	line = Trim(line);
	if (line.size() < 3) return false;
	wchar_t marker = 0;
	std::size_t count = 0;
	for (const wchar_t character : line) {
		if (character == L' ' || character == L'\t') continue;
		if (marker == 0) marker = character;
		if (character != marker || (marker != L'-' && marker != L'*' && marker != L'_')) return false;
		++count;
	}
	return count >= 3;
}

std::wstring InlineText(std::wstring_view source)
{
	std::wstring result(source);
	std::size_t open = 0;
	while ((open = result.find(L'[', open)) != std::wstring::npos) {
		const std::size_t close = result.find(L"](", open + 1);
		if (close == std::wstring::npos) break;
		const std::size_t end = result.find(L')', close + 2);
		if (end == std::wstring::npos) break;
		const std::wstring label = result.substr(open + 1, close - open - 1);
		const std::wstring url = result.substr(close + 2, end - close - 2);
		result.replace(open, end - open + 1, label + L" (" + url + L")");
		open += label.size() + url.size() + 3;
	}
	std::wstring plain;
	plain.reserve(result.size());
	for (std::size_t index = 0; index < result.size(); ++index) {
		if (result[index] == L'\\' && index + 1 < result.size()) {
			plain += result[++index];
			continue;
		}
		if (result[index] == L'*' || result[index] == L'_' || result[index] == L'~' || result[index] == L'`') continue;
		plain += result[index];
	}
	return plain;
}

std::vector<MarkdownBlock> ParseMarkdown(std::wstring_view source)
{
	if (source.size() > kMaxReadmeCharacters) source = source.substr(0, kMaxReadmeCharacters);
	std::vector<MarkdownBlock> blocks;
	std::wstring paragraph;
	bool inFence = false;
	std::wstring code;

	const auto flushParagraph = [&]() {
		if (!paragraph.empty()) {
			blocks.push_back({ MarkdownBlockKind::Paragraph, InlineText(paragraph), 0, false, 0 });
			paragraph.clear();
		}
	};
	const auto flushCode = [&]() {
		if (!code.empty()) {
			blocks.push_back({ MarkdownBlockKind::Code, code, 0, false, 0 });
			code.clear();
		}
	};

	std::size_t start = 0;
	while (start <= source.size()) {
		const std::size_t end = source.find(L'\n', start);
		const std::wstring_view raw = source.substr(start, end == std::wstring::npos ? source.size() - start : end - start);
		const std::wstring_view line = Trim(raw);
		if (line.size() >= 3 && line.substr(0, 3) == L"```") {
			if (inFence) flushCode();
			else flushParagraph();
			inFence = !inFence;
		} else if (inFence) {
			if (!code.empty()) code += L'\n';
			code += raw;
		} else if (line.empty()) {
			flushParagraph();
		} else if (IsRule(line)) {
			flushParagraph();
			blocks.push_back({ MarkdownBlockKind::Rule, {}, 0, false, 0 });
		} else {
			std::size_t hashCount = 0;
			while (hashCount < line.size() && line[hashCount] == L'#') ++hashCount;
			if (hashCount > 0 && hashCount <= 6 && hashCount < line.size() && line[hashCount] == L' ') {
				flushParagraph();
				blocks.push_back({ MarkdownBlockKind::Heading, InlineText(Trim(line.substr(hashCount))), static_cast<int>(hashCount), false, 0 });
			} else {
				std::wstring_view item = line;
				bool ordered = false;
				int number = 0;
				if (item.size() >= 2 && (item[0] == L'-' || item[0] == L'*' || item[0] == L'+') && (item[1] == L' ' || item[1] == L'\t')) {
					item = Trim(item.substr(2));
				} else {
					std::size_t digits = 0;
					while (digits < item.size() && item[digits] >= L'0' && item[digits] <= L'9') ++digits;
					if (digits > 0 && digits + 1 < item.size() && item[digits] == L'.' && (item[digits + 1] == L' ' || item[digits + 1] == L'\t')) {
						ordered = true;
						for (std::size_t digit = 0; digit < digits; ++digit) number = number * 10 + (item[digit] - L'0');
						item = Trim(item.substr(digits + 1));
					}
				}
				if (item.data() != line.data() || ordered) {
					flushParagraph();
					blocks.push_back({ MarkdownBlockKind::ListItem, InlineText(item), 0, ordered, number });
				} else {
					if (!paragraph.empty()) paragraph += L' ';
					paragraph += line;
				}
			}
		}
		if (end == std::wstring::npos) break;
		start = end + 1;
	}
	if (inFence) flushCode();
	else flushParagraph();
	return blocks;
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

CExtensionDetailSurface::~CExtensionDetailSurface()
{
	Destroy();
	ReleaseFont();
}

HWND CExtensionDetailSurface::Open(HINSTANCE hInstance, HWND hwndParent)
{
	if (hInstance == nullptr || hwndParent == nullptr || GetHwnd() != nullptr) return nullptr;
	if (RegisterWC(hInstance, nullptr, nullptr, ::LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	const HWND window = Create(hwndParent, 0, kWindowClass, L"Extension Details",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, nullptr);
	if (window == nullptr) return nullptr;
	EnsureFont();
	m_hwndClose = ::CreateWindowExW(0, WC_BUTTONW, L"×", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)), hInstance, nullptr);
	m_hwndInstall = ::CreateWindowExW(0, WC_BUTTONW, L"Install / Update", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInstallButtonId)), hInstance, nullptr);
	if (m_hwndClose != nullptr) ::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	if (m_hwndInstall != nullptr) {
		::SendMessageW(m_hwndInstall, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		::EnableWindow(m_hwndInstall, FALSE);
	}
	if (m_hwndClose == nullptr || m_hwndInstall == nullptr) {
		Destroy();
		return nullptr;
	}
	LayoutChildren();
	return window;
}

void CExtensionDetailSurface::Destroy() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) {
		CWnd::DestroyWindow();
	} else {
		_SetHwnd(nullptr);
		m_hwndClose = nullptr;
		m_hwndInstall = nullptr;
	}
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
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::ShowExtension(const SOpenVsxExtension& extension)
{
	m_extension = extension;
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	m_hasExtension = true;
	m_scrollOffset = 0;
	m_contentHeight = 0;
	m_maxScrollOffset = 0;
	if (m_hwndInstall != nullptr) ::EnableWindow(m_hwndInstall, m_onInstallRequested ? TRUE : FALSE);
	LayoutChildren();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::ClearExtension()
{
	m_extension = {};
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	m_hasExtension = false;
	m_scrollOffset = 0;
	m_contentHeight = 0;
	m_maxScrollOffset = 0;
	if (m_hwndInstall != nullptr) ::EnableWindow(m_hwndInstall, FALSE);
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeMarkdown(std::wstring markdown)
{
	m_readmeMarkdown = std::move(markdown);
	m_readmeError.clear();
	m_readmeState = ReadmeState::Ready;
	m_scrollOffset = 0;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeLoading()
{
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Loading;
	m_scrollOffset = 0;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeError(std::wstring message)
{
	m_readmeMarkdown.clear();
	m_readmeError = std::move(message);
	m_readmeState = ReadmeState::Error;
	m_scrollOffset = 0;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetReadmeUnsupported()
{
	m_readmeMarkdown.clear();
	m_readmeError.clear();
	m_readmeState = ReadmeState::Unsupported;
	m_scrollOffset = 0;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CExtensionDetailSurface::SetOnInstallRequested(InstallRequestedCallback callback)
{
	m_onInstallRequested = std::move(callback);
	if (m_hwndInstall != nullptr) ::EnableWindow(m_hwndInstall, m_hasExtension && m_onInstallRequested ? TRUE : FALSE);
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
}

void CExtensionDetailSurface::PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold)
{
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

void CExtensionDetailSurface::PaintReadme(HDC dc, int left, int* top, int right, int bottom)
{
	const int width = std::max(1, right - left);
	const int lineGap = ScaleDip(7);
	const auto drawMessage = [&](const std::wstring& message, COLORREF color) {
		if (*top >= bottom) return;
		RECT bounds{ left, *top, right, bottom };
		PaintText(dc, message.c_str(), bounds, color, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
		RECT measured = bounds;
		const HGDIOBJ old = ::SelectObject(dc, m_font);
		::DrawTextW(dc, message.c_str(), -1, &measured, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
		if (old != nullptr) ::SelectObject(dc, old);
		*top = std::min(bottom, static_cast<int>(measured.bottom) + lineGap);
	};

	switch (m_readmeState) {
	case ReadmeState::Loading:
		drawMessage(L"Loading README…", m_palette.secondaryText.ToColorRef());
		return;
	case ReadmeState::Error:
		drawMessage(m_readmeError.empty() ? L"README could not be loaded." : m_readmeError, m_palette.danger.ToColorRef());
		return;
	case ReadmeState::Unsupported:
		drawMessage(L"README/Markdown content was not supplied by the extension model.", m_palette.disabledText.ToColorRef());
		return;
	case ReadmeState::Ready:
		break;
	}

	const std::vector<MarkdownBlock> blocks = ParseMarkdown(m_readmeMarkdown);
	if (blocks.empty()) {
		drawMessage(L"This extension did not provide README content.", m_palette.disabledText.ToColorRef());
		return;
	}

	for (const MarkdownBlock& block : blocks) {
		if (*top >= bottom) break;
		if (block.kind == MarkdownBlockKind::Rule) {
			const int y = *top + ScaleDip(6);
			const HPEN pen = ::CreatePen(PS_SOLID, 1, m_palette.border.ToColorRef());
			const HGDIOBJ old = ::SelectObject(dc, pen);
			::MoveToEx(dc, left, y, nullptr);
			::LineTo(dc, right, y);
			if (old != nullptr) ::SelectObject(dc, old);
			::DeleteObject(pen);
			*top = std::min(bottom, y + ScaleDip(14));
			continue;
		}

		std::wstring text = block.text;
		COLORREF color = m_palette.primaryText.ToColorRef();
		UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS;
		bool bold = false;
		int extraTop = ScaleDip(8);
		if (block.kind == MarkdownBlockKind::Heading) {
			bold = true;
			extraTop = ScaleDip(block.level <= 2 ? 12 : 8);
			color = m_palette.primaryText.ToColorRef();
		} else if (block.kind == MarkdownBlockKind::ListItem) {
			text = (block.ordered ? std::to_wstring(block.number) + L". " : L"• ") + text;
		} else if (block.kind == MarkdownBlockKind::Code) {
			const int codePadding = ScaleDip(8);
			RECT measured{ 0, 0, width - codePadding * 2, 0 };
			const HGDIOBJ oldFont = ::SelectObject(dc, m_font);
			::DrawTextW(dc, text.c_str(), -1, &measured, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
			if (oldFont != nullptr) ::SelectObject(dc, oldFont);
			const int codeHeight = std::max(ScaleDip(24), static_cast<int>(measured.bottom) + codePadding * 2);
			RECT codeBounds{ left, *top + extraTop, right, std::min(bottom, *top + extraTop + codeHeight) };
			const HBRUSH codeBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
			::FillRect(dc, &codeBounds, codeBrush);
			::DeleteObject(codeBrush);
			RECT codeText{ left + codePadding, codeBounds.top + codePadding, right - codePadding, codeBounds.bottom - codePadding };
			PaintText(dc, text.c_str(), codeText, m_palette.primaryText.ToColorRef(), format, false);
			*top = codeBounds.bottom + lineGap;
			continue;
		}

		RECT measured{ left, *top + extraTop, right, *top + extraTop };
		const HGDIOBJ oldFont = ::SelectObject(dc, bold ? m_boldFont : m_font);
		::DrawTextW(dc, text.c_str(), -1, &measured, format | DT_CALCRECT);
		if (oldFont != nullptr) ::SelectObject(dc, oldFont);
		measured.bottom = static_cast<LONG>(std::min(
			bottom,
			std::max(static_cast<int>(measured.bottom), static_cast<int>(measured.top) + ScaleDip(20))));
		PaintText(dc, text.c_str(), measured, color, format, bold);
		*top = std::min(bottom, static_cast<int>(measured.bottom) + lineGap);
	}
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
	const int contentRight = std::max<int>(padding, static_cast<int>(client.right) - padding);
	int top = ScaleDip(58);

	RECT header{ 0, 0, client.right, ScaleDip(44) };
	const HBRUSH headerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
	::FillRect(dc, &header, headerBrush);
	::DeleteObject(headerBrush);
	const std::wstring headerText = m_hasExtension ? (L"Extension: " + (m_extension.sDisplayName.empty() ? m_extension.sName : m_extension.sDisplayName)) : L"Extension";
	PaintText(dc, headerText.c_str(), RECT{ padding, 0, std::max<int>(padding, static_cast<int>(client.right) - padding * 3), header.bottom }, m_palette.primaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, true);
	PaintText(dc, L"Details", RECT{ padding, top, contentRight, top + ScaleDip(30) }, m_palette.disabledText.ToColorRef(), DT_LEFT | DT_SINGLELINE, false);
	top += ScaleDip(42);

	if (m_hasExtension) {
		const std::wstring displayName = m_extension.sDisplayName.empty() ? m_extension.sName : m_extension.sDisplayName;
		const int heroRight = std::max(padding, contentRight - ScaleDip(140));
		const int tileSide = ScaleDip(64);
		RECT tile{ padding, top, padding + tileSide, top + tileSide };
		const HBRUSH tileBrush = ::CreateSolidBrush(m_palette.accent.ToColorRef());
		::FillRect(dc, &tile, tileBrush);
		::DeleteObject(tileBrush);
		std::wstring initials;
		for (const wchar_t character : displayName) {
			if (character == L' ' || character == L'-' || character == L'_') continue;
			initials += character;
			if (initials.size() == 2) break;
		}
		if (initials.empty()) initials = L"?";
		PaintText(dc, initials.c_str(), tile, m_palette.highlightText.ToColorRef(), DT_CENTER | DT_VCENTER | DT_SINGLELINE, true);
		const int textLeft = tile.right + ScaleDip(14);
		PaintText(dc, displayName.c_str(), RECT{ textLeft, top, heroRight, top + ScaleDip(38) }, m_palette.primaryText.ToColorRef(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, true);
		top += ScaleDip(34);
		const std::wstring publisher = (m_extension.bVerified ? L"✓ Verified " : L"") + m_extension.sNamespace;
		PaintText(dc, publisher.c_str(), RECT{ textLeft, top, heroRight, top + ScaleDip(24) }, m_extension.bVerified ? RGB(88, 166, 255) : m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		const std::wstring version = L"Version " + (m_extension.sVersion.empty() ? L"—" : m_extension.sVersion) + L"   •   " + CountText(m_extension.nDownloadCount) +
			(m_extension.HasRating() ? L"   •   ★ " + std::to_wstring(m_extension.dAverageRating).substr(0, 3) : L"");
		PaintText(dc, version.c_str(), RECT{ textLeft, top + ScaleDip(24), heroRight, top + ScaleDip(48) }, m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		top += ScaleDip(70);
		PaintSectionHeading(dc, L"DETAILS", padding, &top, contentRight);
		if (!m_extension.sDescription.empty()) {
			RECT description{ padding, top, contentRight, top + ScaleDip(72) };
			PaintText(dc, m_extension.sDescription.c_str(), description, m_palette.primaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
			top += ScaleDip(86);
		}
		const int contentStart = top;
		const int viewportBottom = (std::max)(contentStart, static_cast<int>(client.bottom) - padding);
		const int savedDc = ::SaveDC(dc);
		::IntersectClipRect(dc, 0, contentStart, client.right, client.bottom);
		top -= m_scrollOffset;
		PaintReadme(dc, padding, &top, contentRight, viewportBottom);
		PaintSectionHeading(dc, L"FEATURES", padding, &top, contentRight);
		PaintText(dc, L"No feature list was supplied by this extension model.", RECT{ padding, top, contentRight, top + ScaleDip(44) }, m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK);
		top += ScaleDip(58);
		PaintSectionHeading(dc, L"CHANGELOG", padding, &top, contentRight);
		PaintText(dc, L"No changelog was supplied by this extension model.", RECT{ padding, top, contentRight, top + ScaleDip(44) }, m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK);
		top += ScaleDip(58);
		PaintSectionHeading(dc, L"EXTENSION PACK", padding, &top, contentRight);
		PaintText(dc, L"No extension pack information was supplied by this extension model.", RECT{ padding, top, contentRight, top + ScaleDip(36) }, m_palette.secondaryText.ToColorRef(), DT_LEFT | DT_TOP | DT_WORDBREAK);
		m_contentHeight = (std::max)(0, top + m_scrollOffset + padding - contentStart);
		if (savedDc != 0) ::RestoreDC(dc, savedDc);
		const int viewportHeight = (std::max)(1, static_cast<int>(client.bottom) - contentStart);
		const int maxScroll = (std::max)(0, m_contentHeight - viewportHeight);
		m_maxScrollOffset = maxScroll;
		if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
		SCROLLINFO scrollInfo{ sizeof(scrollInfo), SIF_RANGE | SIF_PAGE | SIF_POS, 0, m_contentHeight, static_cast<UINT>(viewportHeight), m_scrollOffset, 0 };
		::SetScrollInfo(GetHwnd(), SB_VERT, &scrollInfo, TRUE);
	}
	::EndPaint(GetHwnd(), &ps);
}

void CExtensionDetailSurface::ScrollTo(int offset) noexcept
{
	const int clamped = (std::max)(0, (std::min)(offset, m_maxScrollOffset));
	if (clamped == m_scrollOffset) return;
	m_scrollOffset = clamped;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
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
	case WM_SIZE: LayoutChildren(); return 0;
	case WM_VSCROLL: {
		SCROLLINFO info{ sizeof(info), SIF_ALL };
		::GetScrollInfo(hwnd, SB_VERT, &info);
		int next = m_scrollOffset;
		switch (LOWORD(wp)) {
		case SB_LINEUP: next -= ScaleDip(32); break;
		case SB_LINEDOWN: next += ScaleDip(32); break;
		case SB_PAGEUP: next -= static_cast<int>(info.nPage); break;
		case SB_PAGEDOWN: next += static_cast<int>(info.nPage); break;
		case SB_THUMBTRACK: next = info.nTrackPos; break;
		case SB_TOP: next = 0; break;
		case SB_BOTTOM: next = m_maxScrollOffset; break;
		default: return 0;
		}
		ScrollTo(next);
		return 0;
	}
	case WM_MOUSEWHEEL: {
		const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		ScrollTo(m_scrollOffset - notches * ScaleDip(48));
		return 0;
	}
	case WM_DPICHANGED:
		ReleaseFont();
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
