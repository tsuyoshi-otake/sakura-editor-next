/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace workbench::viewcontainer {

//! VS Code paneview.ts / paneview.css at e6aeab60511647b9b00f0bf2f0f02b15f278abb5.
//! A pane header is not the physical host's ViewContainer title bar.
inline constexpr int kViewPaneHeaderDip = 22;
inline constexpr int kViewPaneHeaderIconInsetDip = 2;
inline constexpr int kViewPaneIconDip = 16;
inline constexpr int kViewPaneHeaderTextGapDip = 2;
inline constexpr int kViewPaneRowDip = 22;
inline constexpr int kViewPaneRowInsetDip = 10;
inline constexpr int kViewPaneSashDip = 4;
inline constexpr int kViewPaneMinimumBodyDip = 120;
inline constexpr int kViewPaneActionPaddingDip = 2;
inline constexpr int kViewPaneActionGapDip = 4;
inline constexpr int kViewPaneActionTrailingDip = 8;
inline constexpr std::size_t kMaximumViewPanes = 64;

struct ViewPaneSize final {
	bool collapsed{};
	int preferredBody{};
	int minimumBody{};
};
struct ViewPaneBounds final {
	int top{};
	int headerBottom{};
	int bottom{};
	[[nodiscard]] constexpr bool operator==(const ViewPaneBounds&) const = default;
};
struct ViewPaneStackLayout final {
	std::array<ViewPaneBounds, kMaximumViewPanes> panes{};
	std::size_t count{};
	int contentHeight{};
	bool valid{};
};

//! O(N), allocation-free vertical SplitView projection in physical pixels.
//! Minimum extents cause outer scrolling instead of clipping later headers.
//! Expanded bodies share surplus proportionally; collapsed bodies use no space.
[[nodiscard]] constexpr ViewPaneStackLayout BuildViewPaneStackLayout(
	const std::span<const ViewPaneSize> sizes, const int viewportHeight, const int headerHeight) noexcept
{
	ViewPaneStackLayout result;
	if (sizes.size() > kMaximumViewPanes || viewportHeight < 0 || viewportHeight > 1000000
		|| headerHeight < 1 || headerHeight > 1024) return result;
	std::int64_t minimum = static_cast<std::int64_t>(sizes.size()) * headerHeight;
	std::int64_t weight{};
	for (const auto& size : sizes) {
		if (size.preferredBody < 0 || size.preferredBody > 1000000
			|| size.minimumBody < 0 || size.minimumBody > 1000000) return result;
		if (!size.collapsed) {
			minimum += size.minimumBody;
			weight += std::max(1, size.preferredBody - size.minimumBody);
		}
	}
	const auto extra = std::max<std::int64_t>(0, viewportHeight - minimum);
	std::int64_t accumulatedWeight{}, assignedExtra{};
	int cursor{};
	for (const auto& size : sizes) {
		auto& pane = result.panes[result.count++];
		pane.top = cursor;
		cursor += headerHeight;
		pane.headerBottom = cursor;
		if (!size.collapsed) {
			accumulatedWeight += std::max(1, size.preferredBody - size.minimumBody);
			const auto cumulative = extra * accumulatedWeight / weight;
			cursor += size.minimumBody + static_cast<int>(cumulative - assignedExtra);
			assignedExtra = cumulative;
		}
		pane.bottom = cursor;
	}
	result.contentHeight = cursor;
	result.valid = true;
	return result;
}

} // namespace workbench::viewcontainer
