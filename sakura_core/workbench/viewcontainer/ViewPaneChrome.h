/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "theme/CThemeService.h"
#include "workbench/viewcontainer/ViewPaneLayout.h"
#include <string_view>

namespace workbench::viewcontainer {

//! Shared SCM/contributed-View chrome. The caller has selected its theme font,
//! painted the background, and excluded the title-action strip from bounds.
void PaintViewPaneHeader(HDC dc, RECT bounds, std::wstring_view title, bool collapsed,
	bool separator, unsigned int dpi, const theme::ThemePalette& palette) noexcept;
void PaintViewPaneIcon(HDC dc, const RECT& bounds, std::wstring_view icon, COLORREF color) noexcept;

} // namespace workbench::viewcontainer
