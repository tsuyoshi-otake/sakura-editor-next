/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "WorkbenchLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace workbench {
namespace {

constexpr int kDefaultDpi = 96;
constexpr int kTitleHeightDip = 34;
constexpr int kDocumentTabsHeightDip = 32;
constexpr int kActivityBarWidthDip = 42;
constexpr int kStatusHeightDip = 22;
constexpr int kSplitterDip = 4;
constexpr int kEditorMinimumWidthDip = 320;
constexpr int kEditorMinimumHeightDip = 180;

[[nodiscard]] int NonNegative(int value) noexcept
{
	return std::max(0, value);
}

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const auto scaled = (static_cast<std::int64_t>(NonNegative(dip)) * effectiveDpi + kDefaultDpi / 2) / kDefaultDpi;
	return static_cast<int>(std::min<std::int64_t>(scaled, std::numeric_limits<int>::max()));
}

[[nodiscard]] bool IsShown(WorkbenchPanelState state) noexcept
{
	return state != WorkbenchPanelState::Hidden;
}

[[nodiscard]] int ResolveChromeHeight(int physicalOverride, int standardDip,
	unsigned int dpi, int remainingHeight) noexcept
{
	const int desired = physicalOverride >= 0 ? physicalOverride : ScaleDip(standardDip, dpi);
	return std::min(NonNegative(remainingHeight), NonNegative(desired));
}

[[nodiscard]] WorkbenchRect MakeRect(int left, int top, int right, int bottom) noexcept
{
	left = NonNegative(left);
	top = NonNegative(top);
	right = std::max(left, NonNegative(right));
	bottom = std::max(top, NonNegative(bottom));
	return { left, top, right, bottom };
}

[[nodiscard]] int BoundedEnd(int start, int length, int limit) noexcept
{
	const auto end = static_cast<std::int64_t>(start) + NonNegative(length);
	return static_cast<int>(std::min<std::int64_t>(end, limit));
}

// Scale desired auxiliary widths into budget while retaining their relative proportions.
void FitInto(int budget, std::array<int*, 5> values, const std::array<int, 5>& desired) noexcept
{
	budget = NonNegative(budget);
	std::int64_t total = 0;
	for (const int value : desired) {
		total += value;
	}
	if (total <= budget) {
		for (std::size_t i = 0; i < values.size(); ++i) {
			*values[i] = desired[i];
		}
		return;
	}

	int used = 0;
	for (std::size_t i = 0; i < values.size(); ++i) {
		const int fitted = static_cast<int>((static_cast<std::int64_t>(desired[i]) * budget) / total);
		*values[i] = fitted;
		used += fitted;
	}
	// Give the rounding remainder to the final non-empty region. This is stable across calls.
	for (std::size_t i = values.size(); used < budget && i-- > 0;) {
		if (desired[i] != 0) {
			++*values[i];
			++used;
		}
	}
}

} // namespace

WorkbenchLayout CalculateWorkbenchLayout(const WorkbenchLayoutRequest& request) noexcept
{
	const int width = NonNegative(request.clientWidth);
	const int height = NonNegative(request.clientHeight);
	const unsigned int dpi = request.dpi == 0 ? kDefaultDpi : request.dpi;

	const int titleHeight = ResolveChromeHeight(
		request.titleBarHeightPixels, kTitleHeightDip, dpi, height);
	const int topAccessoryHeight = ResolveChromeHeight(
		request.topAccessoryHeightPixels, 0, dpi, height - titleHeight);
	const int tabsHeight = ResolveChromeHeight(
		request.documentTabsHeightPixels, kDocumentTabsHeightDip, dpi,
		height - titleHeight - topAccessoryHeight);
	const int statusHeight = ResolveChromeHeight(
		request.statusBarHeightPixels, kStatusHeightDip, dpi,
		height - titleHeight - topAccessoryHeight - tabsHeight);
	const int bottomAccessoryHeight = ResolveChromeHeight(
		request.bottomAccessoryHeightPixels, 0, dpi,
		height - titleHeight - topAccessoryHeight - tabsHeight - statusHeight);
	const int bodyTop = titleHeight + topAccessoryHeight + tabsHeight;
	const int bodyBottom = height - statusHeight - bottomAccessoryHeight;
	const int activityWidth = std::min(width, ScaleDip(kActivityBarWidthDip, dpi));

	WorkbenchLayout layout;
	layout.titleBar = MakeRect(0, 0, width, titleHeight);
	layout.topAccessory = MakeRect(0, titleHeight, width, titleHeight + topAccessoryHeight);
	layout.activityBar = MakeRect(0, bodyTop, activityWidth, bodyBottom);
	layout.documentTabs = MakeRect(0, titleHeight + topAccessoryHeight, width, bodyTop);
	layout.bottomAccessory = MakeRect(0, bodyBottom, width, bodyBottom + bottomAccessoryHeight);
	layout.statusBar = MakeRect(0, bodyBottom, width, height);
	layout.statusBar.top = layout.bottomAccessory.bottom;

	int leftPane = 0;
	int leftSplitter = 0;
	int minimap = 0;
	int rightSplitter = 0;
	int rightPane = 0;
	const int desiredLeftPane = IsShown(request.leftPane) ? ScaleDip(request.leftPaneWidthDip, dpi) : 0;
	const int desiredLeftSplitter = desiredLeftPane == 0 ? 0 : ScaleDip(kSplitterDip, dpi);
	const int desiredMinimap = request.showMinimap ? ScaleDip(request.minimapWidthDip, dpi) : 0;
	const int desiredRightPane = IsShown(request.rightPane) ? ScaleDip(request.rightPaneWidthDip, dpi) : 0;
	const int desiredRightSplitter = desiredRightPane == 0 ? 0 : ScaleDip(kSplitterDip, dpi);
	const int mainWidth = width - activityWidth;
	const int editorMinimumWidth = std::min(mainWidth, ScaleDip(kEditorMinimumWidthDip, dpi));
	FitInto(mainWidth - editorMinimumWidth,
		{ &leftPane, &leftSplitter, &minimap, &rightSplitter, &rightPane },
		{ desiredLeftPane, desiredLeftSplitter, desiredMinimap, desiredRightSplitter, desiredRightPane });

	const int centralLeft = activityWidth + leftPane + leftSplitter;
	const int centralRight = width - rightPane - rightSplitter;
	const int desiredBottomPane = IsShown(request.bottomPane) ? ScaleDip(request.bottomPaneHeightDip, dpi) : 0;
	const int desiredBottomSplitter = desiredBottomPane == 0 ? 0 : ScaleDip(kSplitterDip, dpi);
	const auto desiredBottomHeight = static_cast<std::int64_t>(desiredBottomPane) + desiredBottomSplitter;
	const int editorMinimumHeight = std::min(bodyBottom - bodyTop, ScaleDip(kEditorMinimumHeightDip, dpi));
	int bottomPane = 0;
	int bottomSplitter = 0;
	const int bottomBudget = std::max(0, bodyBottom - bodyTop - editorMinimumHeight);
	if (desiredBottomHeight <= bottomBudget) {
		bottomPane = desiredBottomPane;
		bottomSplitter = desiredBottomSplitter;
	} else if (desiredBottomHeight > 0) {
		bottomPane = static_cast<int>((static_cast<std::int64_t>(desiredBottomPane) * bottomBudget) / desiredBottomHeight);
		bottomSplitter = bottomBudget - bottomPane;
	}

	const int editorBottom = bodyBottom - bottomPane - bottomSplitter;
	const int minimapLeft = std::max(centralLeft, centralRight - minimap);
	layout.leftPane = MakeRect(activityWidth, bodyTop, activityWidth + leftPane, bodyBottom);
	layout.leftSplitter = MakeRect(activityWidth + leftPane, bodyTop, centralLeft, bodyBottom);
	layout.editor = MakeRect(centralLeft, bodyTop, minimapLeft, editorBottom);
	layout.minimap = MakeRect(minimapLeft, bodyTop, centralRight, editorBottom);
	layout.rightSplitter = MakeRect(centralRight, bodyTop, centralRight + rightSplitter, bodyBottom);
	layout.rightPane = MakeRect(centralRight + rightSplitter, bodyTop, width, bodyBottom);
	layout.bottomSplitter = MakeRect(centralLeft, editorBottom, centralRight, editorBottom + bottomSplitter);
	layout.bottomPane = MakeRect(centralLeft, editorBottom + bottomSplitter, centralRight, bodyBottom);
	const int headerHeight = ScaleDip(request.paneHeaderHeightDip, dpi);
	layout.leftPaneHeader = MakeRect(layout.leftPane.left, layout.leftPane.top, layout.leftPane.right,
		BoundedEnd(layout.leftPane.top, headerHeight, layout.leftPane.bottom));
	layout.rightPaneHeader = MakeRect(layout.rightPane.left, layout.rightPane.top, layout.rightPane.right,
		BoundedEnd(layout.rightPane.top, headerHeight, layout.rightPane.bottom));
	layout.bottomPaneHeader = MakeRect(layout.bottomPane.left, layout.bottomPane.top, layout.bottomPane.right,
		BoundedEnd(layout.bottomPane.top, headerHeight, layout.bottomPane.bottom));
	return layout;
}

} // namespace workbench
