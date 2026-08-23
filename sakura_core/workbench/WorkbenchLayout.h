/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>

namespace workbench {

//! A rectangle expressed in physical client pixels.  It deliberately has no Win32 dependency.
struct WorkbenchRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int Width() const noexcept { return right - left; }
	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
	[[nodiscard]] constexpr bool operator==(const WorkbenchRect&) const noexcept = default;
};

enum class WorkbenchPanelState : std::uint8_t {
	Hidden,
	Visible,
	DragResizing,
};

//! The supported values of VS Code's `workbench.activityBar.location` setting.
//!
//! Hidden is intentionally not represented here.  The native workbench does not
//! have an Activity Bar-hidden layout yet, so accepting that value would make a
//! request look supported while silently projecting one of the other layouts.
enum class ActivityBarLocation : std::uint8_t {
	Default,
	Top,
	Bottom,
};

//! Returns whether a value crossing an untyped settings boundary is supported.
[[nodiscard]] constexpr bool IsSupportedActivityBarLocation(ActivityBarLocation location) noexcept
{
	return location == ActivityBarLocation::Default
		|| location == ActivityBarLocation::Top
		|| location == ActivityBarLocation::Bottom;
}

// Compatibility name retained for the pure-layout callers introduced first.
using WorkbenchPaneState = WorkbenchPanelState;

//! All configurable dimensions are in DIPs; the client size is in physical pixels.
struct WorkbenchLayoutRequest {
	int clientWidth = 0;
	int clientHeight = 0;
	unsigned int dpi = 96;
	ActivityBarLocation activityBarLocation = ActivityBarLocation::Default;
	//! -1 selects the standard DIP token; non-negative values are physical pixels.
	int titleBarHeightPixels = -1;
	int topAccessoryHeightPixels = 0;
	int documentTabsHeightPixels = -1;
	int bottomAccessoryHeightPixels = 0;
	int statusBarHeightPixels = -1;

	WorkbenchPanelState leftPane = WorkbenchPanelState::Visible;
	WorkbenchPanelState rightPane = WorkbenchPanelState::Visible;
	WorkbenchPanelState bottomPane = WorkbenchPanelState::Visible;
	bool showMinimap = false;
	//! Expands the visible bottom pane over the editor while preserving document tabs.
	//! This is window-local runtime state and is never persisted with pane extents.
	bool bottomPaneMaximized = false;

	int leftPaneWidthDip = 280;
	int rightPaneWidthDip = 260;
	int bottomPaneHeightDip = 220;
	int minimapWidthDip = 100;
	int paneHeaderHeightDip = 30;
};

//! Result rectangles never contain a negative coordinate or inverted edge.
struct WorkbenchLayout {
	WorkbenchRect titleBar;
	WorkbenchRect topAccessory;
	//! The legacy/default Activity Bar rectangle.  It aliases primaryActivityBar
	//! for the default location and remains empty for horizontal locations.
	WorkbenchRect activityBar;
	//! Explicit Activity Bar rectangles for the Primary and Secondary Side Bars.
	//! The Secondary rectangle is empty when that side bar is hidden.
	WorkbenchRect primaryActivityBar;
	WorkbenchRect secondaryActivityBar;
	WorkbenchRect documentTabs;
	WorkbenchRect leftPane;
	WorkbenchRect leftPaneHeader;
	WorkbenchRect leftSplitter;
	WorkbenchRect editor;
	WorkbenchRect minimap;
	WorkbenchRect rightSplitter;
	WorkbenchRect rightPane;
	WorkbenchRect rightPaneHeader;
	WorkbenchRect bottomSplitter;
	WorkbenchRect bottomPane;
	WorkbenchRect bottomPaneHeader;
	WorkbenchRect bottomAccessory;
	WorkbenchRect statusBar;

	[[nodiscard]] constexpr bool operator==(const WorkbenchLayout&) const noexcept = default;
};

//! Computes an entirely deterministic workbench geometry from explicit client pixels and DPI.
[[nodiscard]] WorkbenchLayout CalculateWorkbenchLayout(const WorkbenchLayoutRequest& request) noexcept;

} // namespace workbench
