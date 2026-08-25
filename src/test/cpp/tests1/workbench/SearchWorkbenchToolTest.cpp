/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/search/CSearchWorkbenchTool.h"

namespace {

int ScaleDip(int dip, unsigned int dpi)
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? 96u : dpi), 96);
}

void ExpectRect(const RECT& expected, const RECT& actual)
{
	EXPECT_EQ(expected.left, actual.left);
	EXPECT_EQ(expected.top, actual.top);
	EXPECT_EQ(expected.right, actual.right);
	EXPECT_EQ(expected.bottom, actual.bottom);
}

TEST(SearchWorkbenchToolGeometry, MatchesVsCodeSearchWidgetAtDefaultDpi)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, false);

	ExpectRect(RECT{ 2, 0, 468, 38 }, geometry.container);
	ExpectRect(RECT{ 20, 6, 468, 32 }, geometry.queryBox);
	ExpectRect(RECT{ 2, 6, 18, 32 }, geometry.toggleReplace);
	ExpectRect(RECT{ 21, 9, 384, 29 }, geometry.queryEdit);
	EXPECT_EQ(26, geometry.queryBox.bottom - geometry.queryBox.top);
	EXPECT_EQ(18, geometry.queryBox.left - geometry.container.left);
	EXPECT_EQ(16, geometry.toggleReplace.right - geometry.toggleReplace.left);
	EXPECT_EQ(6, geometry.queryBox.top - geometry.container.top);
	EXPECT_EQ(6, geometry.container.bottom - geometry.queryBox.bottom);
}

TEST(SearchWorkbenchToolGeometry, ReplaceRowUsesCssMarginsAndCentersNativeEdit)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, true);

	ExpectRect(RECT{ 20, 6, 468, 32 }, geometry.queryBox);
	ExpectRect(RECT{ 20, 38, 440, 64 }, geometry.replaceBox);
	ExpectRect(RECT{ 444, 39, 468, 63 }, geometry.replaceAll);
	ExpectRect(RECT{ 2, 6, 18, 64 }, geometry.toggleReplace);
	ExpectRect(RECT{ 21, 9, 384, 29 }, geometry.queryEdit);
	ExpectRect(RECT{ 21, 41, 396, 61 }, geometry.replaceEdit);
	EXPECT_EQ(6, geometry.replaceBox.top - geometry.queryBox.bottom);
	EXPECT_EQ(6, geometry.container.bottom - geometry.replaceBox.bottom);
	EXPECT_EQ(20, geometry.replaceEdit.bottom - geometry.replaceEdit.top);
	EXPECT_EQ(geometry.queryEdit.top - geometry.queryBox.top,
		geometry.queryBox.bottom - geometry.queryEdit.bottom);
	EXPECT_EQ(geometry.replaceEdit.top - geometry.replaceBox.top,
		geometry.replaceBox.bottom - geometry.replaceEdit.bottom);
}

TEST(SearchWorkbenchToolGeometry, PreservesCssRelationshipsAcrossSupportedDpi)
{
	constexpr RECT client{ 10, 20, 810, 620 };
	for (const unsigned int dpi : { 96u, 120u, 144u, 192u }) {
		const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
			client, dpi, true);
		const int inputHeight = ScaleDip(26, dpi);

		EXPECT_EQ(client.left + ScaleDip(2, dpi), geometry.container.left);
		EXPECT_EQ(client.right - ScaleDip(12, dpi), geometry.container.right);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.queryBox.top - geometry.container.top);
		EXPECT_EQ(ScaleDip(18, dpi), geometry.queryBox.left - geometry.container.left);
		EXPECT_EQ(ScaleDip(16, dpi),
			geometry.toggleReplace.right - geometry.toggleReplace.left);
		EXPECT_EQ(inputHeight, geometry.queryBox.bottom - geometry.queryBox.top);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.replaceBox.top - geometry.queryBox.bottom);
		EXPECT_EQ(inputHeight - 2 * ScaleDip(3, dpi),
			geometry.replaceEdit.bottom - geometry.replaceEdit.top);
		EXPECT_EQ(geometry.queryEdit.top - geometry.queryBox.top,
			geometry.queryBox.bottom - geometry.queryEdit.bottom);
		EXPECT_EQ(geometry.replaceEdit.top - geometry.replaceBox.top,
			geometry.replaceBox.bottom - geometry.replaceEdit.bottom);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.container.bottom - geometry.replaceBox.bottom);
		EXPECT_LE(geometry.queryEdit.left, geometry.queryEdit.right);
		EXPECT_LE(geometry.replaceEdit.left, geometry.replaceEdit.right);
	}
}

TEST(SearchWorkbenchToolGeometry, ClampsNarrowClientWithoutNegativeEditBounds)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 32, 100 }, 192, true);

	EXPECT_LE(geometry.container.left, geometry.container.right);
	EXPECT_LE(geometry.queryEdit.left, geometry.queryEdit.right);
	EXPECT_LE(geometry.replaceEdit.left, geometry.replaceEdit.right);
	EXPECT_EQ(ScaleDip(26, 192) - 2 * ScaleDip(3, 192),
		geometry.queryEdit.bottom - geometry.queryEdit.top);
	EXPECT_EQ(ScaleDip(26, 192) - 2 * ScaleDip(3, 192),
		geometry.replaceEdit.bottom - geometry.replaceEdit.top);
}

} // namespace
