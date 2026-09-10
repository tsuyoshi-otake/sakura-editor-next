/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib AND MIT
*/
#pragma once

// The Sakura GDI wrapper is Zlib; the imported GitHub Actions Activity Bar icon
// geometry is MIT.
// Imported from github/vscode-github-actions at 45e962b4439e6476d67937206566c0470743c660.
// See GITHUB-ACTIONS-ATTRIBUTION.md beside this file for the source SVG, the
// license, and the local adaptations.

#include "workbench/icons/GitHubActionsStatusIcons.h"

namespace workbench::icons::github_actions {

//! The path upstream's package.json names as the `github-actions`
//! ViewContainer icon. The light and dark copies of the file are identical.
constexpr std::wstring_view kContainerIconPath = L"resources/icons/light/explorer.svg";

namespace detail {

//! Upstream explorer.svg (the Octicons `workflow` glyph), with each rounded
//! corner's arc rewritten as a cubic Bezier segment.
constexpr std::string_view kExplorerPath = R"svg(M1 3C1 1.8954 1.8954 1 3 1H9.5C10.6046 1 11.5 1.8954 11.5 3V9.5C11.5 10.6046 10.6046 11.5 9.5 11.5H7V15.563C7 16.355 7.644 17 8.438 17H12.5V14.5C12.5 13.3954 13.3954 12.5 14.5 12.5H21C22.1046 12.5 23 13.3954 23 14.5V21C23 22.1046 22.1046 23 21 23H14.5C13.3954 23 12.5 22.1046 12.5 21V18.5H8.437C6.815 18.4989 5.5006 17.184 5.5 15.562V11.5H3C1.8954 11.5 1 10.6046 1 9.5ZM3 2.5C2.7239 2.5 2.5 2.7239 2.5 3V9.5C2.5 9.7761 2.7239 10 3 10H9.5C9.7761 10 10 9.7761 10 9.5V3C10 2.7239 9.7761 2.5 9.5 2.5ZM14.5 14C14.2239 14 14 14.2239 14 14.5V21C14 21.2761 14.2239 21.5 14.5 21.5H21C21.2761 21.5 21.5 21.2761 21.5 21V14.5C21.5 14.2239 21.2761 14 21 14Z)svg";

//! The SVG has no fill of its own: VS Code uses an extension's container icon
//! as a mask painted in the Activity Bar foreground, so the caller supplies it.
constexpr StatusIconLayer kExplorerLayer{ 24, false, kExplorerPath };

} // namespace detail

//! True for a container icon path this build can draw.
[[nodiscard]] constexpr bool IsContainerIcon(std::wstring_view name) noexcept
{
	return name == kContainerIconPath;
}

//! Draws a known Activity Bar container icon as a single-colour mask in
//! `color`. Returns false and draws nothing for any other name.
inline bool DrawContainerIcon(HDC dc, const IconRect& box, std::wstring_view name, COLORREF color) noexcept
{
	if (dc == nullptr || !IsContainerIcon(name) || box.Width() <= 0 || box.Height() <= 0) return false;
	const IconRect viewport = codicons::detail::SvgIconLetterboxBounds(box);
	if (viewport.Width() <= 0) return false;
	return detail::FillLayer(dc, viewport, detail::kExplorerLayer, color);
}

} // namespace workbench::icons::github_actions
