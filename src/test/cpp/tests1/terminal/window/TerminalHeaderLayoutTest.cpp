/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/window/TerminalHeaderLayout.h"

#include <array>
#include <utility>

namespace terminal {
namespace {

constexpr std::array kVisualOrder{
	TerminalHeaderTarget::Profile,
	TerminalHeaderTarget::New,
	TerminalHeaderTarget::Dropdown,
	TerminalHeaderTarget::Split,
	TerminalHeaderTarget::Kill,
	TerminalHeaderTarget::More,
	TerminalHeaderTarget::Maximize,
	TerminalHeaderTarget::Close,
};

[[nodiscard]] POINT CenterOf(const RECT& rect)
{
	return POINT{ rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
}

void ExpectValidRect(const RECT& rect, const RECT& container)
{
	EXPECT_GE(rect.right, rect.left);
	EXPECT_GE(rect.bottom, rect.top);
	EXPECT_GE(rect.left, container.left);
	EXPECT_GE(rect.top, container.top);
	EXPECT_LE(rect.right, container.right);
	EXPECT_LE(rect.bottom, container.bottom);
}

TEST(TerminalHeaderLayout, ScalesOneRowHeaderAndActionsAtSupportedDpis)
{
	for (const auto [dpi, expectedHeight] : std::array{
		std::pair{ 96U, 30 }, std::pair{ 144U, 45 }, std::pair{ 192U, 60 } }) {
		const RECT bounds{ 7, 11, 1207, 211 };
		const auto layout = CalculateTerminalHeaderLayout(bounds, dpi);
		EXPECT_EQ(expectedHeight, layout.header.bottom - layout.header.top);
		EXPECT_EQ(bounds.left, layout.header.left);
		EXPECT_EQ(bounds.right, layout.header.right);
		ExpectValidRect(layout.title, layout.header);
		ExpectValidRect(layout.underline, layout.header);
		for (const auto target : kVisualOrder) {
			const auto& rect = layout.RectFor(target);
			ExpectValidRect(rect, layout.header);
			EXPECT_GT(rect.right, rect.left);
			EXPECT_EQ(target, layout.HitTest(CenterOf(rect)));
		}
	}
}

TEST(TerminalHeaderLayout, PreservesLeftToRightVisualOrderWithoutOverlap)
{
	const auto layout = CalculateTerminalHeaderLayout(RECT{ 0, 0, 800, 100 }, 96);
	for (std::size_t index = 1; index < kVisualOrder.size(); ++index) {
		const auto& previous = layout.RectFor(kVisualOrder[index - 1]);
		const auto& current = layout.RectFor(kVisualOrder[index]);
		EXPECT_LE(previous.right, current.left);
	}
	EXPECT_LE(layout.title.right, layout.RectFor(TerminalHeaderTarget::Profile).left);
}

TEST(TerminalHeaderLayout, ReturnsNoneForActionGapsAndOutsideHeader)
{
	const auto layout = CalculateTerminalHeaderLayout(RECT{ 0, 0, 800, 100 }, 96);
	const auto& split = layout.RectFor(TerminalHeaderTarget::Split);
	const auto& kill = layout.RectFor(TerminalHeaderTarget::Kill);
	ASSERT_LT(split.right, kill.left);
	EXPECT_EQ(TerminalHeaderTarget::None,
		layout.HitTest(POINT{ split.right, (split.top + split.bottom) / 2 }));
	EXPECT_EQ(TerminalHeaderTarget::None, layout.HitTest(POINT{ -1, 10 }));
	EXPECT_EQ(TerminalHeaderTarget::None, layout.HitTest(POINT{ 799, layout.header.bottom }));
}

TEST(TerminalHeaderLayout, OmitsPanelActionsWhenTheContainingPartOwnsCommonChrome)
{
	const auto layout = CalculateTerminalHeaderLayout(RECT{ 0, 0, 800, 100 }, 96, false);
	EXPECT_EQ(layout.RectFor(TerminalHeaderTarget::Maximize).left,
		layout.RectFor(TerminalHeaderTarget::Maximize).right);
	EXPECT_EQ(layout.RectFor(TerminalHeaderTarget::Close).left,
		layout.RectFor(TerminalHeaderTarget::Close).right);
	EXPECT_EQ(TerminalHeaderTarget::None, layout.HitTest(POINT{ 799, 15 }));
	EXPECT_GT(layout.RectFor(TerminalHeaderTarget::More).right,
		layout.RectFor(TerminalHeaderTarget::More).left);
}

TEST(TerminalHeaderLayout, HiddenActionsTakeNoSpaceAndAreNotHitTestable)
{
	const RECT bounds{ 0, 0, 600, 40 };
	const auto all = CalculateTerminalHeaderLayout(bounds, 96, false, 34);
	const auto hidden = CalculateTerminalHeaderLayout(bounds, 96, false, 34,
		TerminalHeaderActionVisibility{ false, false });

	for (const auto target : { TerminalHeaderTarget::Profile, TerminalHeaderTarget::Split, TerminalHeaderTarget::Kill }) {
		const auto& rect = hidden.RectFor(target);
		EXPECT_EQ(rect.left, rect.right);
		ExpectValidRect(rect, hidden.header);
		EXPECT_NE(target, hidden.HitTest(CenterOf(all.RectFor(target))));
	}
	// More keeps its slot; Dropdown moves up to where Kill was, with no gap.
	EXPECT_EQ(all.RectFor(TerminalHeaderTarget::More).left, hidden.RectFor(TerminalHeaderTarget::More).left);
	EXPECT_EQ(all.RectFor(TerminalHeaderTarget::Kill).right, hidden.RectFor(TerminalHeaderTarget::Dropdown).right);
	EXPECT_GT(hidden.title.right, all.title.right);

	const auto withProfile = CalculateTerminalHeaderLayout(bounds, 96, false, 34,
		TerminalHeaderActionVisibility{ true, false });
	EXPECT_LT(withProfile.RectFor(TerminalHeaderTarget::Profile).left,
		withProfile.RectFor(TerminalHeaderTarget::Profile).right);
	EXPECT_EQ(hidden.RectFor(TerminalHeaderTarget::New).left - 2,
		withProfile.RectFor(TerminalHeaderTarget::Profile).right);
}

TEST(TerminalHeaderLayout, HiddenActionsLeaveTheirSpaceToTheRemainingActions)
{
	// 100 DIP fits More, Kill and Split but not Dropdown. Without Split and Kill,
	// New and Dropdown fit instead of collapsing with them.
	const RECT bounds{ 0, 0, 100, 34 };
	const auto all = CalculateTerminalHeaderLayout(bounds, 96, false, 34);
	EXPECT_EQ(all.RectFor(TerminalHeaderTarget::Dropdown).left, all.RectFor(TerminalHeaderTarget::Dropdown).right);
	EXPECT_EQ(all.RectFor(TerminalHeaderTarget::New).left, all.RectFor(TerminalHeaderTarget::New).right);

	const auto hidden = CalculateTerminalHeaderLayout(bounds, 96, false, 34,
		TerminalHeaderActionVisibility{ true, false });
	for (const auto target : { TerminalHeaderTarget::New, TerminalHeaderTarget::Dropdown, TerminalHeaderTarget::More }) {
		const auto& rect = hidden.RectFor(target);
		EXPECT_LT(rect.left, rect.right);
		EXPECT_EQ(target, hidden.HitTest(CenterOf(rect)));
	}
	// Profile still collapses when its whole hit target does not fit.
	EXPECT_EQ(hidden.RectFor(TerminalHeaderTarget::Profile).left, hidden.RectFor(TerminalHeaderTarget::Profile).right);
}

TEST(TerminalHeaderLayout, CollapsesWholeActionsForNarrowAndDegenerateBounds)
{
	const auto narrow = CalculateTerminalHeaderLayout(RECT{ 0, 0, 60, 30 }, 96);
	EXPECT_GT(narrow.RectFor(TerminalHeaderTarget::Close).right,
		narrow.RectFor(TerminalHeaderTarget::Close).left);
	EXPECT_EQ(narrow.RectFor(TerminalHeaderTarget::Maximize).left,
		narrow.RectFor(TerminalHeaderTarget::Maximize).right);
	EXPECT_EQ(narrow.RectFor(TerminalHeaderTarget::Profile).left,
		narrow.RectFor(TerminalHeaderTarget::Profile).right);

	for (const RECT bounds : std::array{
		RECT{ 0, 0, 48, 30 }, RECT{ 10, 20, 10, 20 }, RECT{ 20, 30, 5, 10 } }) {
		const auto layout = CalculateTerminalHeaderLayout(bounds, 192);
		ExpectValidRect(layout.title, layout.header);
		ExpectValidRect(layout.underline, layout.header);
		for (const auto target : kVisualOrder) {
			ExpectValidRect(layout.RectFor(target), layout.header);
		}
		for (std::size_t first = 0; first < kVisualOrder.size(); ++first) {
			const auto& a = layout.RectFor(kVisualOrder[first]);
			if (a.right <= a.left) continue;
			for (std::size_t second = first + 1; second < kVisualOrder.size(); ++second) {
				const auto& b = layout.RectFor(kVisualOrder[second]);
				if (b.right <= b.left) continue;
				EXPECT_TRUE(a.right <= b.left || b.right <= a.left);
			}
		}
	}
}

} // namespace
} // namespace terminal
