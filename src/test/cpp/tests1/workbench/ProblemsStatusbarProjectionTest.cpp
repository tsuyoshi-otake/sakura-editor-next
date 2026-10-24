/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/problems/ProblemsStatusbarProjection.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::problems {
namespace {

platform::uri::Uri Uri(std::wstring_view text)
{
	const auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

MarkerRange Range(std::uint32_t startLine, std::uint32_t startColumn,
	std::uint32_t endLine, std::uint32_t endColumn)
{
	return { startLine, startColumn, endLine, endColumn };
}

ProblemMarker Marker(MarkerRange range, EMarkerSeverity severity, std::string message)
{
	ProblemMarker marker;
	marker.range = range;
	marker.severity = severity;
	marker.message = std::move(message);
	return marker;
}

ProblemsSnapshot Snapshot(std::vector<ProblemResourceSnapshot> resources)
{
	ProblemsSnapshot snapshot;
	snapshot.resources = std::move(resources);
	return snapshot;
}

ProblemResourceSnapshot Resource(std::wstring_view uri, std::vector<ProblemMarker> markers)
{
	return { Uri(uri), {}, std::move(markers) };
}

TEST(ProblemsStatusbarProjection, CountsIncludeErrorWarningInfoButNotHint)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Error, "e1"),
			Marker(Range(1, 0, 1, 1), EMarkerSeverity::Error, "e2"),
			Marker(Range(2, 0, 2, 1), EMarkerSeverity::Warning, "w"),
			Marker(Range(3, 0, 3, 1), EMarkerSeverity::Information, "i"),
			Marker(Range(4, 0, 4, 1), EMarkerSeverity::Hint, "hint"),
		}),
		Resource(L"file:///c%3A/b.cpp", {
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Warning, "w2"),
		}),
	});

	const auto counts = CountProblemsStatusMarkers(snapshot);
	EXPECT_EQ(2u, counts.errors);
	EXPECT_EQ(2u, counts.warnings);
	EXPECT_EQ(1u, counts.infos);
}

TEST(ProblemsStatusbarProjection, PackNumberMatchesUpstreamThresholds)
{
	EXPECT_EQ(L"0", PackMarkerCount(0, L"10K+"));
	EXPECT_EQ(L"42", PackMarkerCount(42, L"10K+"));
	EXPECT_EQ(L"999", PackMarkerCount(999, L"10K+"));
	EXPECT_EQ(L"1K", PackMarkerCount(1000, L"10K+"));
	EXPECT_EQ(L"9K", PackMarkerCount(9999, L"10K+"));
	EXPECT_EQ(L"10K+", PackMarkerCount(10000, L"10K+"));
	EXPECT_EQ(L"10K+", PackMarkerCount(12345, L"10K+"));
	// upstream packNumber は先頭 1 桁 + "K"。10000-19999 のみ manyProblemsText。
	EXPECT_EQ(L"2K", PackMarkerCount(2345, L"10K+"));
}

TEST(ProblemsStatusbarProjection, StatusTextAlwaysShowsErrorAndWarning)
{
	const auto text = BuildProblemsStatusText({ 3, 1, 0 }, L"10K+");
	EXPECT_EQ(L"$(error) 3 $(warning) 1", text);
}

TEST(ProblemsStatusbarProjection, StatusTextAddsInfosOnlyWhenNonzero)
{
	const auto text = BuildProblemsStatusText({ 3, 1, 2 }, L"10K+");
	EXPECT_EQ(L"$(error) 3 $(warning) 1 $(info) 2", text);
}

TEST(ProblemsStatusbarProjection, TooltipJoinsOnlyNonzeroCategories)
{
	const auto tooltip = BuildProblemsStatusTooltip({ 2, 0, 4 },
		L"Errors: {0}", L"Warnings: {0}", L"Infos: {0}", L"No Problems");
	EXPECT_EQ(L"Errors: 2, Infos: 4", tooltip);
}

TEST(ProblemsStatusbarProjection, TooltipFallsBackToNoProblemsText)
{
	const auto tooltip = BuildProblemsStatusTooltip({ 0, 0, 0 },
		L"Errors: {0}", L"Warnings: {0}", L"Infos: {0}", L"No Problems");
	EXPECT_EQ(L"No Problems", tooltip);
}

TEST(ProblemsStatusbarProjection, CountTitleWithoutPlaceholderStaysLiteral)
{
	EXPECT_EQ(L"Errors", FormatMarkerCountTitle(L"Errors", 7));
	EXPECT_EQ(L"エラー: 7", FormatMarkerCountTitle(L"エラー: {0}", 7));
}

TEST(ProblemsStatusbarProjection, CurrentProblemRequiresTheSetting)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 5), EMarkerSeverity::Error, "boom"),
		}),
	});
	EXPECT_FALSE(FindCurrentProblemStatus(snapshot, L"file:///c%3A/a.cpp", false, { 0, 2 }).has_value());
	EXPECT_TRUE(FindCurrentProblemStatus(snapshot, L"file:///c%3A/a.cpp", true, { 0, 2 }).has_value());
	EXPECT_FALSE(FindCurrentProblemStatus(snapshot, L"", true, { 0, 2 }).has_value());
}

TEST(ProblemsStatusbarProjection, CurrentProblemPicksTheFirstContainingMarkerInSeverityOrder)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			// severity 順に並び替えられるので Error が Info より先に見つかる。
			Marker(Range(0, 0, 0, 9), EMarkerSeverity::Information, "info first line"),
			Marker(Range(0, 0, 0, 9), EMarkerSeverity::Error, "error here"),
		}),
	});
	const auto current = FindCurrentProblemStatus(snapshot, L"file:///c%3A/a.cpp", true, { 0, 2 });
	ASSERT_TRUE(current.has_value());
	EXPECT_EQ(EMarkerSeverity::Error, current->severity);
	EXPECT_EQ(L"$(error) error here", current->text);
}

TEST(ProblemsStatusbarProjection, CurrentProblemTextUsesTheMessageFirstLine)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 9), EMarkerSeverity::Warning, "first\r\nsecond\nthird"),
		}),
	});
	const auto current = FindCurrentProblemStatus(snapshot, L"file:///c%3A/a.cpp", true, { 0, 2 });
	ASSERT_TRUE(current.has_value());
	EXPECT_EQ(L"$(warning) first", current->text);
}

TEST(ProblemsStatusbarProjection, CurrentProblemIgnoresOtherResourcesAndHints)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 9), EMarkerSeverity::Hint, "hint here"),
		}),
		Resource(L"file:///c%3A/b.cpp", {
			Marker(Range(0, 0, 0, 9), EMarkerSeverity::Error, "other file"),
		}),
	});
	EXPECT_FALSE(FindCurrentProblemStatus(snapshot, L"file:///c%3A/a.cpp", true, { 0, 2 }).has_value());
}

} // namespace
} // namespace workbench::problems
