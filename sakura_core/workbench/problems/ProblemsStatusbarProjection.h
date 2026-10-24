/*! @file
 * @brief Problems status-bar projection for VS Code `status.problems` and
 * `statusbar.currentProblem`.
 *
 * Pure value projection over `MarkerService` snapshots: counts, packed text,
 * tooltip text, and the current-marker-under-caret lookup. Localized label
 * templates are injected by the caller so the model stays language-free.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/problems/MarkerNavigationModel.h"
#include "workbench/problems/MarkerService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::problems {

//! upstream `markerService.getStatistics()` の表示対象分。Hint は unknowns に
//! 畳まれて表示されないため、ここには出ない。
struct ProblemsStatusCounts final {
	std::uint32_t errors{};
	std::uint32_t warnings{};
	std::uint32_t infos{};

	[[nodiscard]] bool operator==(const ProblemsStatusCounts&) const noexcept = default;
};

//! スナップショット内の Error/Warning/Info マーカーをそのまま数える
//! (upstream getStatistics は重複排除をしない)。
[[nodiscard]] ProblemsStatusCounts CountProblemsStatusMarkers(
	const ProblemsSnapshot& snapshot) noexcept;

//! upstream `packNumber`: n > 9999 → manyProblemsText ("10K+")、
//! n > 999 → 先頭桁 + "K"、それ以外は 10 進表記。
[[nodiscard]] std::wstring PackMarkerCount(std::uint32_t count,
	std::wstring_view manyProblemsText);

//! upstream `getMarkersText`: `"$(error) N $(warning) N"` に
//! infos があれば `" $(info) N"` を足す。
[[nodiscard]] std::wstring BuildProblemsStatusText(
	const ProblemsStatusCounts& counts, std::wstring_view manyProblemsText);

//! `"{0}"` を含む localize テンプレートへ件数を埋め込む。
//! プレースホルダーが無いときは件数をそのまま後置しない (upstream localize2 相当)。
[[nodiscard]] std::wstring FormatMarkerCountTitle(
	std::wstring_view format, std::uint32_t count);

//! upstream `getMarkersTooltip`: 非ゼロの区分だけを `", "` 連結。
//! 全部ゼロなら `noProblemsText` ("No Problems")。
[[nodiscard]] std::wstring BuildProblemsStatusTooltip(
	const ProblemsStatusCounts& counts,
	std::wstring_view errorsTitleFormat,
	std::wstring_view warningsTitleFormat,
	std::wstring_view infosTitleFormat,
	std::wstring_view noProblemsText);

//! `statusbar.currentProblem` に出す 1 項目。`text` は
//! `"$(error|warning|info) <message の先頭行>"`。
struct CurrentProblemStatus final {
	EMarkerSeverity severity{ EMarkerSeverity::Error };
	std::wstring text;

	[[nodiscard]] bool operator==(const CurrentProblemStatus&) const noexcept = default;
};

//! upstream `ShowCurrentMarkerInStatusbarContribution.getMarker()`:
//! `problems.showCurrentInStatus` が有効で、アクティブ リソース内の
//! Error|Warning|Info マーカー(resource→severity→start 順)のうち
//! キャレット位置を含む最初のもの。
[[nodiscard]] std::optional<CurrentProblemStatus> FindCurrentProblemStatus(
	const ProblemsSnapshot& snapshot,
	std::wstring_view resourceUri,
	bool showCurrentInStatus,
	MarkerNavigationPosition position);

} // namespace workbench::problems
