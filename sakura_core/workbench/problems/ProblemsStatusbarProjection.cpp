/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/problems/ProblemsStatusbarProjection.h"

#include <string_view>

namespace workbench::problems {
namespace {

//! upstream `getType`: 深刻度から codicon 名つき `$(name)` プレフィックス。
[[nodiscard]] std::wstring_view SeverityIconText(EMarkerSeverity severity) noexcept
{
	switch (severity) {
	case EMarkerSeverity::Error: return L"$(error)";
	case EMarkerSeverity::Warning: return L"$(warning)";
	case EMarkerSeverity::Information: return L"$(info)";
	default: return L"";
	}
}

//! upstream `strings.splitLines(message)[0]`。\r\n / \r / \n のどれでも切る。
[[nodiscard]] std::wstring_view FirstMessageLine(std::wstring_view message) noexcept
{
	const auto end = message.find_first_of(L"\r\n");
	return end == std::wstring_view::npos ? message : message.substr(0, end);
}

} // namespace

ProblemsStatusCounts CountProblemsStatusMarkers(const ProblemsSnapshot& snapshot) noexcept
{
	ProblemsStatusCounts counts;
	for (const auto& resource : snapshot.resources) {
		for (const auto& marker : resource.markers) {
			switch (marker.severity) {
			case EMarkerSeverity::Error: ++counts.errors; break;
			case EMarkerSeverity::Warning: ++counts.warnings; break;
			case EMarkerSeverity::Information: ++counts.infos; break;
			default: break; // Hint は upstream の unknowns で表示対象外。
			}
		}
	}
	return counts;
}

std::wstring PackMarkerCount(std::uint32_t count, std::wstring_view manyProblemsText)
{
	if (count > 9999) return std::wstring(manyProblemsText);
	if (count > 999) {
		// upstream は `n.toString().charAt(0) + 'K'`。
		const auto decimal = std::to_wstring(count);
		return decimal.substr(0, 1) + L'K';
	}
	return std::to_wstring(count);
}

std::wstring BuildProblemsStatusText(
	const ProblemsStatusCounts& counts, std::wstring_view manyProblemsText)
{
	std::wstring text;
	text += L"$(error) ";
	text += PackMarkerCount(counts.errors, manyProblemsText);
	text += L" $(warning) ";
	text += PackMarkerCount(counts.warnings, manyProblemsText);
	if (counts.infos > 0) {
		text += L" $(info) ";
		text += PackMarkerCount(counts.infos, manyProblemsText);
	}
	return text;
}

std::wstring FormatMarkerCountTitle(std::wstring_view format, std::uint32_t count)
{
	const auto placeholder = format.find(L"{0}");
	const auto decimal = std::to_wstring(count);
	if (placeholder == std::wstring_view::npos) {
		std::wstring result(format);
		return result;
	}
	std::wstring result;
	result.reserve(format.size() + decimal.size());
	result.append(format.substr(0, placeholder));
	result.append(decimal);
	result.append(format.substr(placeholder + 3));
	return result;
}

std::wstring BuildProblemsStatusTooltip(
	const ProblemsStatusCounts& counts,
	std::wstring_view errorsTitleFormat,
	std::wstring_view warningsTitleFormat,
	std::wstring_view infosTitleFormat,
	std::wstring_view noProblemsText)
{
	std::wstring tooltip;
	const auto append = [&tooltip](std::wstring_view format, std::uint32_t count) {
		if (!tooltip.empty()) tooltip += L", ";
		tooltip += FormatMarkerCountTitle(format, count);
	};
	if (counts.errors > 0) append(errorsTitleFormat, counts.errors);
	if (counts.warnings > 0) append(warningsTitleFormat, counts.warnings);
	if (counts.infos > 0) append(infosTitleFormat, counts.infos);
	if (tooltip.empty()) return std::wstring(noProblemsText);
	return tooltip;
}

std::optional<CurrentProblemStatus> FindCurrentProblemStatus(
	const ProblemsSnapshot& snapshot,
	std::wstring_view resourceUri,
	bool showCurrentInStatus,
	MarkerNavigationPosition position)
{
	if (!showCurrentInStatus || resourceUri.empty()) return std::nullopt;

	// upstream updateMarkers: `read({resource, severities: E|W|I})` を
	// compareMarker(resource→severity→start) で並べる。`find` は
	// 配列先頭から containsPosition の最初のマーカーを返す。
	const auto markers = BuildMarkerNavigationMarkers(
		snapshot, std::optional<std::wstring>(resourceUri), EMarkerSortOrder::Severity);
	for (const auto& marker : markers) {
		if (!MarkerNavigationRangeContainsPosition(marker.range, position)) continue;
		CurrentProblemStatus status{ .severity = marker.severity };
		status.text = SeverityIconText(marker.severity);
		status.text += L' ';
		status.text += FirstMessageLine(marker.message);
		return status;
	}
	return std::nullopt;
}

} // namespace workbench::problems
