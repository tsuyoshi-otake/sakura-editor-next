/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/problems/MarkerNavigationModel.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::problems {
namespace {

//! file:/// URI 文字列。テストでは Windows path 経由にせず URI 直書きで固定する。
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

//! 1 リソース 1 コレクションのスナップショットを積むヘルパー。
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

const CMarkerNavigationModel::WordAtPosition kNoWords =
	[](MarkerNavigationPosition) -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
		return std::nullopt;
	};

TEST(MarkerNavigationModel, SortOrderSettingFallsBackToSeverity)
{
	EXPECT_EQ(EMarkerSortOrder::Severity, MarkerSortOrderFromSetting(L"severity"));
	EXPECT_EQ(EMarkerSortOrder::Position, MarkerSortOrderFromSetting(L"position"));
	EXPECT_EQ(EMarkerSortOrder::Severity, MarkerSortOrderFromSetting(L"bogus"));
	EXPECT_EQ(EMarkerSortOrder::Severity, MarkerSortOrderFromSetting(L""));
}

TEST(MarkerNavigationModel, ContainsPositionIsInclusiveOnBothEnds)
{
	const auto range = Range(2, 3, 2, 8);
	EXPECT_TRUE(MarkerNavigationRangeContainsPosition(range, { 2, 3 }));
	EXPECT_TRUE(MarkerNavigationRangeContainsPosition(range, { 2, 8 }));
	EXPECT_TRUE(MarkerNavigationRangeContainsPosition(range, { 2, 5 }));
	EXPECT_FALSE(MarkerNavigationRangeContainsPosition(range, { 2, 2 }));
	EXPECT_FALSE(MarkerNavigationRangeContainsPosition(range, { 2, 9 }));
	EXPECT_FALSE(MarkerNavigationRangeContainsPosition(range, { 1, 9 }));
	EXPECT_FALSE(MarkerNavigationRangeContainsPosition(range, { 3, 0 }));
}

TEST(MarkerNavigationModel, BuildFiltersHintMarkersAndNonMatchingResources)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/src/a.cpp", {
			Marker(Range(0, 0, 0, 5), EMarkerSeverity::Error, "e"),
			Marker(Range(1, 0, 1, 5), EMarkerSeverity::Hint, "hint"),
		}),
		Resource(L"file:///c%3A/src/b.cpp", {
			Marker(Range(0, 0, 0, 5), EMarkerSeverity::Warning, "w"),
		}),
	});

	const auto all = BuildMarkerNavigationMarkers(snapshot, std::nullopt,
		EMarkerSortOrder::Severity);
	ASSERT_EQ(2u, all.size());
	// resourceUri は `resource.toString()` の canonical 形 (%3A は復号される)。
	EXPECT_EQ(L"file:///c:/src/a.cpp", all[0].resourceUri);
	EXPECT_EQ(EMarkerSeverity::Error, all[0].severity);
	EXPECT_EQ(EMarkerSeverity::Warning, all[1].severity);

	const auto onlyB = BuildMarkerNavigationMarkers(snapshot,
		std::optional<std::wstring>(L"file:///c%3A/src/b.cpp"), EMarkerSortOrder::Severity);
	ASSERT_EQ(1u, onlyB.size());
	EXPECT_EQ(L"file:///c:/src/b.cpp", onlyB[0].resourceUri);
}

TEST(MarkerNavigationModel, SeverityOrderIsResourceThenSeverityThenStart)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/b.cpp", {
			Marker(Range(5, 0, 5, 1), EMarkerSeverity::Information, "info-late"),
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Error, "err"),
			Marker(Range(2, 0, 2, 1), EMarkerSeverity::Warning, "warn"),
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Information, "info-early"),
		}),
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(9, 0, 9, 1), EMarkerSeverity::Error, "a-err"),
		}),
	});

	const auto markers = BuildMarkerNavigationMarkers(snapshot, std::nullopt,
		EMarkerSortOrder::Severity);
	// resource 先、a.cpp の Error が先頭。b.cpp 内は severity → start。
	ASSERT_EQ(5u, markers.size());
	EXPECT_EQ(L"a-err", markers[0].message);
	EXPECT_EQ(L"err", markers[1].message);
	EXPECT_EQ(L"warn", markers[2].message);
	EXPECT_EQ(L"info-early", markers[3].message);
	EXPECT_EQ(L"info-late", markers[4].message);
}

TEST(MarkerNavigationModel, PositionOrderIsResourceThenStartThenSeverity)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/b.cpp", {
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Error, "err"),
			Marker(Range(2, 0, 2, 1), EMarkerSeverity::Warning, "warn"),
			Marker(Range(0, 0, 0, 1), EMarkerSeverity::Information, "info-early"),
		}),
	});

	const auto markers = BuildMarkerNavigationMarkers(snapshot, std::nullopt,
		EMarkerSortOrder::Position);
	// position 順では start が先、同点なら severity。
	ASSERT_EQ(3u, markers.size());
	EXPECT_EQ(L"err", markers[0].message);
	EXPECT_EQ(L"info-early", markers[1].message);
	EXPECT_EQ(L"warn", markers[2].message);
}

TEST(MarkerNavigationModel, MatchesFollowsUpstreamFilterRules)
{
	CMarkerNavigationModel model(std::nullopt, {}, EMarkerSortOrder::Severity);
	// upstream matches: `!filter && !uri` のみ true。フィルタ無しモデルは
	// undefined クエリ (inFiles) だけに一致する。
	EXPECT_TRUE(model.Matches(std::nullopt));
	EXPECT_FALSE(model.Matches(std::optional<std::wstring>(L"file:///c:/a.cpp")));

	CMarkerNavigationModel filtered(
		std::optional<std::wstring>(L"file:///c:/a.cpp"), {}, EMarkerSortOrder::Severity);
	// URI フィルタは `toString() ===` の文字列等値 (isEqual ではない)。
	EXPECT_TRUE(filtered.Matches(std::optional<std::wstring>(L"file:///c:/a.cpp")));
	EXPECT_FALSE(filtered.Matches(std::optional<std::wstring>(L"file:///C:/a.cpp")));
	EXPECT_FALSE(filtered.Matches(std::optional<std::wstring>(L"file:///c:/b.cpp")));
	EXPECT_FALSE(filtered.Matches(std::nullopt));
}

TEST(MarkerNavigationModel, FirstMoveSelectsTheContainingMarker)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "first"),
			Marker(Range(2, 1, 2, 6), EMarkerSeverity::Error, "inside"),
			Marker(Range(4, 0, 4, 3), EMarkerSeverity::Error, "last"),
		}),
	});
	CMarkerNavigationModel model(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);

	// upstream _initIdx: position が含まれるマーカーがそのまま選択される。
	EXPECT_TRUE(model.Move(true, L"file:///c%3A/a.cpp", { 2, 3 }, kNoWords));
	const auto selected = model.Selected();
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(1u, selected->markerIndex);
	EXPECT_EQ(2u, selected->ordinal);
	EXPECT_EQ(3u, selected->total);
	EXPECT_EQ(L"inside", model.MarkerAt(selected->markerIndex).message);
}

TEST(MarkerNavigationModel, FirstMoveBeforeMarkerSelectsItOrThePreviousOnBackward)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "first"),
			Marker(Range(2, 1, 2, 6), EMarkerSeverity::Error, "middle"),
			Marker(Range(4, 0, 4, 3), EMarkerSeverity::Error, "last"),
		}),
	});
	CMarkerNavigationModel forward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	// position (2,0) は middle の手前: fwd はそのまま middle を選択する。
	forward.Move(true, L"file:///c%3A/a.cpp", { 2, 0 }, kNoWords);
	EXPECT_EQ(L"middle", forward.MarkerAt(forward.Selected()->markerIndex).message);

	CMarkerNavigationModel backward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	// upstream `wentPast && !fwd` -> `_nextIdx -= 1`: 逆行は 1 つ前の first。
	backward.Move(false, L"file:///c%3A/a.cpp", { 2, 0 }, kNoWords);
	EXPECT_EQ(L"first", backward.MarkerAt(backward.Selected()->markerIndex).message);
}

TEST(MarkerNavigationModel, FirstMovePastTheLastMarkerWrapsForwardAndSticksBackward)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "first"),
			Marker(Range(4, 0, 4, 3), EMarkerSeverity::Error, "last"),
		}),
	});
	CMarkerNavigationModel forward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	// ファイル末尾を過ぎた: fwd は先頭へ巻き戻る。
	forward.Move(true, L"file:///c%3A/a.cpp", { 9, 0 }, kNoWords);
	EXPECT_EQ(L"first", forward.MarkerAt(forward.Selected()->markerIndex).message);

	CMarkerNavigationModel backward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	backward.Move(false, L"file:///c%3A/a.cpp", { 9, 0 }, kNoWords);
	EXPECT_EQ(L"last", backward.MarkerAt(backward.Selected()->markerIndex).message);
}

TEST(MarkerNavigationModel, FirstMoveInAResourceWithoutMarkersUsesInsertionIndex)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "a"),
		}),
		Resource(L"file:///c%3A/z.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "z"),
		}),
	});
	CMarkerNavigationModel forward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	// m.cpp はマーカー無し: fwd は挿入位置 (z.cpp の index) をそのまま選ぶ。
	forward.Move(true, L"file:///c%3A/m.cpp", { 0, 0 }, kNoWords);
	EXPECT_EQ(L"z", forward.MarkerAt(forward.Selected()->markerIndex).message);

	CMarkerNavigationModel backward(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	// 逆行は挿入位置の 1 つ手前 (a.cpp)。
	backward.Move(false, L"file:///c%3A/m.cpp", { 0, 0 }, kNoWords);
	EXPECT_EQ(L"a", backward.MarkerAt(backward.Selected()->markerIndex).message);
}

TEST(MarkerNavigationModel, SubsequentMovesAdvanceAndWrapAround)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "first"),
			Marker(Range(4, 0, 4, 3), EMarkerSeverity::Error, "second"),
		}),
	});
	CMarkerNavigationModel model(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);

	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(L"first", model.MarkerAt(model.Selected()->markerIndex).message);
	// _nextIdx が居る限り +1/-1 の周回だけをする。
	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(L"second", model.MarkerAt(model.Selected()->markerIndex).message);
	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(L"first", model.MarkerAt(model.Selected()->markerIndex).message);
	model.Move(false, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(L"second", model.MarkerAt(model.Selected()->markerIndex).message);
}

TEST(MarkerNavigationModel, EmptyRangeMarkerExpandsThroughTheWordProvider)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(1, 4, 1, 4), EMarkerSeverity::Error, "empty"),
			Marker(Range(3, 0, 3, 3), EMarkerSeverity::Error, "other"),
		}),
	});
	CMarkerNavigationModel model(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);

	// upstream `model.getWordAtPosition`: 空レンジを単語へ拡げてから包含判定。
	const CMarkerNavigationModel::WordAtPosition words =
		[](MarkerNavigationPosition position) -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
			if (position == MarkerNavigationPosition{ 1, 4 }) {
				return std::pair<std::uint32_t, std::uint32_t>{ 2, 7 };
			}
			return std::nullopt;
		};
	model.Move(true, L"file:///c%3A/a.cpp", { 1, 5 }, words);
	const auto selected = model.Selected();
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(L"empty", model.MarkerAt(selected->markerIndex).message);
}

TEST(MarkerNavigationModel, SetMarkersKeepsIndexWhenContentIsUnchanged)
{
	const auto build = [] {
		return std::vector<MarkerNavigationMarker>{
			MarkerNavigationMarker{ L"file:///c%3A/a.cpp", Range(0, 0, 0, 3),
				EMarkerSeverity::Error, L"first" },
			MarkerNavigationMarker{ L"file:///c%3A/a.cpp", Range(4, 0, 4, 3),
				EMarkerSeverity::Error, L"second" },
		};
	};
	CMarkerNavigationModel model(std::nullopt, build(), EMarkerSortOrder::Severity);
	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(1u, model.Selected()->markerIndex);

	// 同一内容の updateMarker では _nextIdx を保持する。
	EXPECT_FALSE(model.SetMarkers(build()));
	EXPECT_EQ(1u, model.Selected()->markerIndex);

	// 内容が変われば upstream と同じく -1 に戻る。
	auto changed = build();
	changed.push_back(MarkerNavigationMarker{ L"file:///c%3A/a.cpp", Range(9, 0, 9, 3),
		EMarkerSeverity::Error, L"third" });
	EXPECT_TRUE(model.SetMarkers(std::move(changed)));
	EXPECT_FALSE(model.Selected().has_value());
}

TEST(MarkerNavigationModel, ResetIndexReinitializesFromTheCaretOnNextMove)
{
	const auto snapshot = Snapshot({
		Resource(L"file:///c%3A/a.cpp", {
			Marker(Range(0, 0, 0, 3), EMarkerSeverity::Error, "first"),
			Marker(Range(4, 0, 4, 3), EMarkerSeverity::Error, "second"),
		}),
	});
	CMarkerNavigationModel model(std::nullopt,
		BuildMarkerNavigationMarkers(snapshot, std::nullopt, EMarkerSortOrder::Severity),
		EMarkerSortOrder::Severity);
	model.Move(true, L"file:///c%3A/a.cpp", { 0, 1 }, kNoWords);
	EXPECT_EQ(0u, model.Selected()->markerIndex);

	model.ResetIndex();
	EXPECT_FALSE(model.Selected().has_value());
	// リセット後の move はキャレット基準で初期化し直す。
	model.Move(true, L"file:///c%3A/a.cpp", { 4, 1 }, kNoWords);
	EXPECT_EQ(L"second", model.MarkerAt(model.Selected()->markerIndex).message);
}

TEST(MarkerNavigationModel, EmptyModelMovesNowhereAndSelectsNothing)
{
	CMarkerNavigationModel model(std::nullopt, {}, EMarkerSortOrder::Severity);
	EXPECT_FALSE(model.Move(true, L"file:///c%3A/a.cpp", { 0, 0 }, kNoWords));
	EXPECT_FALSE(model.Selected().has_value());
}

} // namespace
} // namespace workbench::problems
