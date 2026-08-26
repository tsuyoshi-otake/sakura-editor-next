/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/controls/CInputBoxGeometry.h"

namespace workbench::controls {

int MeasureTextLineHeight(HWND owner, HFONT font) noexcept
{
	if (owner == nullptr) return 0;
	const HDC dc = ::GetDC(owner);
	if (dc == nullptr) return 0;
	const HGDIOBJ previousFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	TEXTMETRICW metrics{};
	const int height = ::GetTextMetricsW(dc, &metrics) != FALSE
		? static_cast<int>(metrics.tmHeight) : 0;
	if (previousFont != nullptr) (void)::SelectObject(dc, previousFont);
	(void)::ReleaseDC(owner, dc);
	return height;
}

} // namespace workbench::controls
