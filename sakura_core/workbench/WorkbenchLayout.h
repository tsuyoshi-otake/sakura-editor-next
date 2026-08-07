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

// Compatibility name retained for the pure-layout callers introduced first.
using WorkbenchPaneState = WorkbenchPanelState;

//! All configurable dimensions are in DIPs; the client size is in physical pixels.
struct WorkbenchLayoutRequest {
	int clientWidth = 0;
	int clientHeight = 0;
	unsigned int dpi = 96;
	//! -1 selects the standard DIP token; non-negative values are physical pixels.
	int titleBarHeightPixels = -1;
	int topAccessoryHeightPixels = 0;
	//! The Restricted Mode banner has no standard DIP token in this model: its
	//! height is decided by the native control that measures its own text, not
	//! by a fixed design value.  It therefore belongs with the accessory bands:
	//! 0 means absent and any non-negative value is already physical pixels.
	//! Do not "fix" this into a -1 DIP-token sentinel; there is no standard size
	//! to select.
	int bannerHeightPixels = 0;
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
	//! Directly below the title bar and above everything else, spanning the
	//! full client width, matching VS Code's banner stacking order.
	WorkbenchRect banner;
	WorkbenchRect topAccessory;
	WorkbenchRect activityBar;
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
