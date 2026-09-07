/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/viewcontainer/ViewPaneChrome.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/ThemeIconResolver.h"
#include <array>

namespace workbench::viewcontainer {

void PaintViewPaneIcon(HDC dc, const RECT& bounds, const std::wstring_view name, const COLORREF color) noexcept
{
	if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	try {
		const auto icon = icons::ResolveThemeIcon(name, icons::CCodiconFont::Instance().FaceName());
		if (icon.font && !icon.fontIcon.glyph.empty()) {
			const HFONT font = icons::CreateLabelRunGlyphFont(icon.fontIcon.faceName, bounds.bottom - bounds.top);
			if (!font) return;
			const auto previousFont = ::SelectObject(dc, font);
			const auto previousMode = ::SetBkMode(dc, TRANSPARENT);
			const auto previousColor = ::SetTextColor(dc, color);
			RECT rect = bounds;
			::DrawTextW(dc, icon.fontIcon.glyph.data(), static_cast<int>(icon.fontIcon.glyph.size()), &rect,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			::SetTextColor(dc, previousColor);
			::SetBkMode(dc, previousMode);
			::SelectObject(dc, previousFont);
			::DeleteObject(font);
		} else {
			icons::codicons::Draw(dc, { bounds.left, bounds.top, bounds.right, bounds.bottom }, icon.builtin, color);
		}
	} catch (...) {
		// Paint has no pending operation. The next invalidation can redraw the
		// glyph; native control text remains its accessible/action identity.
	}
}

void PaintViewPaneHeader(HDC dc, RECT bounds, const std::wstring_view title, const bool collapsed,
	const bool separator, const unsigned int dpi, const theme::ThemePalette& palette) noexcept
{
	if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	if (separator) {
		const auto previous = ::SetDCBrushColor(dc, palette.border.ToColorRef());
		RECT line{ bounds.left, bounds.top, bounds.right, bounds.top + 1 };
		::FillRect(dc, &line, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
		::SetDCBrushColor(dc, previous);
	}
	const int side = icons::ScaleDip(kViewPaneIconDip, dpi);
	const LONG left = bounds.left + icons::ScaleDip(kViewPaneHeaderIconInsetDip, dpi);
	const LONG top = bounds.top + (bounds.bottom - bounds.top - side) / 2;
	PaintViewPaneIcon(dc, { left, top, left + side, top + side },
		collapsed ? L"chevron-right" : L"chevron-down", palette.secondaryText.ToColorRef());
	bounds.left = left + side + icons::ScaleDip(kViewPaneHeaderTextGapDip, dpi);
	if (bounds.right <= bounds.left) return;
	// CSS text-transform leaves the stable control/accessibility name intact.
	std::array<wchar_t, 3073> uppercase{};
	const int mapped = title.size() <= 1024 ? ::LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE,
		title.data(), static_cast<int>(title.size()), uppercase.data(), static_cast<int>(uppercase.size()), nullptr, nullptr, 0) : 0;
	const auto text = mapped > 0 ? std::wstring_view(uppercase.data(), mapped) : title;
	LOGFONTW description{};
	const auto oldFont = static_cast<HFONT>(::GetCurrentObject(dc, OBJ_FONT));
	HFONT headerFont{};
	if (::GetObjectW(oldFont, sizeof(description), &description) == sizeof(description)) {
		description.lfHeight = -icons::ScaleDip(11, dpi);
		const auto language = PRIMARYLANGID(::GetThreadUILanguage());
		description.lfWeight = language == LANG_JAPANESE || language == LANG_CHINESE || language == LANG_KOREAN ? FW_NORMAL : FW_BOLD;
		headerFont = ::CreateFontIndirectW(&description);
		if (headerFont) ::SelectObject(dc, headerFont);
	}
	const auto previousColor = ::SetTextColor(dc, palette.primaryText.ToColorRef());
	const auto previousMode = ::SetBkMode(dc, TRANSPARENT);
	::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &bounds,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	::SetBkMode(dc, previousMode);
	::SetTextColor(dc, previousColor);
	if (headerFont) { ::SelectObject(dc, oldFont); ::DeleteObject(headerFont); }
}

} // namespace workbench::viewcontainer
