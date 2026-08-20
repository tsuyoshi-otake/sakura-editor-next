/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>

#include "workbench/IconMetrics.h"

namespace workbench::views {

//! Geometry of upstream VS Code's `viewsWelcome` content, shared by every View
//! that contributes it. Explorer and Source Control are two Views rendering the
//! same contribution, so they must produce identical boxes; owning the numbers
//! here is what keeps them from drifting apart again.
//!
//! Measured from the installed VS Code 1.133.0 stylesheet
//! (`workbench.desktop.main.css`):
//!
//! - `.welcome-view-content { padding: 0 20px 1em; align-items: center; }`
//! - `.welcome-view-content > * { margin-block-start: 1em; }`
//! - `.welcome-view-content > .button-container { width: 100%; max-width: 300px; }`
//! - `.monaco-text-button { box-sizing: border-box; padding: 4px 8px;
//!    border-radius: 4px; border: 1px solid ...; line-height: 16px; font-size: 12px; }`
//!
//! The button's `font-size` and `line-height` are fixed by that rule, so its box
//! is 26 CSS pixels tall whatever font the label happens to use. A height
//! measured from the running font is therefore not a closer approximation of VS
//! Code; it is a different control that changes size with the theme.
constexpr int kWelcomeHorizontalInsetDip = 20;
constexpr int kWelcomeColumnMaxDip = 300;
constexpr int kWelcomeButtonLineHeightDip = 16;
constexpr int kWelcomeButtonPaddingYDip = 4;
constexpr int kWelcomeButtonBorderDip = 1;
constexpr int kWelcomeButtonCornerRadiusDip = 4;
//! `box-sizing: border-box`, so the border is inside the 26-DIP box.
constexpr int kWelcomeButtonBoxDip =
	kWelcomeButtonLineHeightDip + 2 * kWelcomeButtonPaddingYDip + 2 * kWelcomeButtonBorderDip;
static_assert(kWelcomeButtonBoxDip == 26);

//! Height of one `viewsWelcome` action button.
[[nodiscard]] constexpr int WelcomeButtonHeight(unsigned int dpi) noexcept
{
	return std::max(1, icons::ScaleDip(kWelcomeButtonBoxDip, dpi));
}

//! `border-radius` of one `viewsWelcome` action button.
[[nodiscard]] constexpr int WelcomeButtonCornerRadius(unsigned int dpi) noexcept
{
	return std::max(1, icons::ScaleDip(kWelcomeButtonCornerRadiusDip, dpi));
}

//! The content's left/right padding.
[[nodiscard]] constexpr int WelcomeHorizontalInset(unsigned int dpi) noexcept
{
	return icons::ScaleDip(kWelcomeHorizontalInsetDip, dpi);
}

//! Width of the centered button column: the full content width, capped at the
//! `.button-container` maximum. Paragraphs are not capped and stay full width.
[[nodiscard]] constexpr int WelcomeButtonColumnWidth(int contentWidth, unsigned int dpi) noexcept
{
	return std::min(std::max(0, contentWidth), icons::ScaleDip(kWelcomeColumnMaxDip, dpi));
}

} // namespace workbench::views
