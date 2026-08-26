/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/controls/CInputBoxGeometry.h"

#include <cstdlib>

namespace {

using workbench::controls::CenterSingleLineEditor;

//! The 26-DIP input box and its 13px text line at 96 DPI, which is what the
//! Search widget and Quick Input both paint.
constexpr RECT kFrame{ 20, 6, 468, 32 };
constexpr int kLineHeight = 13;
constexpr int kHorizontalInset = 1;
constexpr int kFallbackLineHeight = 20;

TEST(InputBoxGeometry, CentersTheMeasuredTextLineInsideTheFrame)
{
	const RECT editor = CenterSingleLineEditor(
		kFrame, kLineHeight, kHorizontalInset, kFallbackLineHeight);

	EXPECT_EQ(kLineHeight, editor.bottom - editor.top);
	const LONG above = editor.top - kFrame.top;
	const LONG below = kFrame.bottom - editor.bottom;
	EXPECT_EQ((kFrame.bottom - kFrame.top - kLineHeight) / 2, above);
	EXPECT_LE(std::abs(above - below), 1);
}

TEST(InputBoxGeometry, KeepsTheEditorInsideTheVerticalBorders)
{
	const RECT editor = CenterSingleLineEditor(
		kFrame, kLineHeight, kHorizontalInset, kFallbackLineHeight);

	EXPECT_EQ(kFrame.left + kHorizontalInset, editor.left);
	EXPECT_EQ(kFrame.right - kHorizontalInset, editor.right);
}

TEST(InputBoxGeometry, UnmeasuredFontUsesTheFallbackLineHeight)
{
	const RECT editor = CenterSingleLineEditor(kFrame, 0, kHorizontalInset, kFallbackLineHeight);

	EXPECT_EQ(kFallbackLineHeight, editor.bottom - editor.top);
	EXPECT_EQ(3, editor.top - kFrame.top);
	EXPECT_EQ(3, kFrame.bottom - editor.bottom);
}

TEST(InputBoxGeometry, ALineTallerThanTheFrameIsClampedToTheFrame)
{
	const RECT editor = CenterSingleLineEditor(kFrame, 999, kHorizontalInset, kFallbackLineHeight);

	EXPECT_EQ(kFrame.top, editor.top);
	EXPECT_EQ(kFrame.bottom, editor.bottom);
}

TEST(InputBoxGeometry, DegenerateFrameNeverProducesInvertedBounds)
{
	for (const RECT& frame : { RECT{ 0, 0, 0, 0 }, RECT{ 10, 10, 11, 10 }, RECT{ 5, 9, 4, 3 } }) {
		const RECT editor = CenterSingleLineEditor(frame, kLineHeight, 6, kFallbackLineHeight);
		EXPECT_LE(editor.left, editor.right);
		EXPECT_LE(editor.top, editor.bottom);
	}
}

} // namespace
