/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace workbench::win32 {
namespace {

[[nodiscard]] std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return L"\xfffd";

	const auto sourceLength = static_cast<int>(value.size());
	const int characterCount = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
	if (characterCount <= 0) return L"\xfffd";

	std::wstring result(static_cast<std::size_t>(characterCount), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength,
		result.data(), characterCount) != characterCount) {
		return L"\xfffd";
	}
	return result;
}

[[nodiscard]] EProblemsPanelSeverity ProjectSeverity(const problems::EMarkerSeverity severity) noexcept
{
	switch (severity) {
	case problems::EMarkerSeverity::Error: return EProblemsPanelSeverity::Error;
	case problems::EMarkerSeverity::Warning: return EProblemsPanelSeverity::Warning;
	case problems::EMarkerSeverity::Information: return EProblemsPanelSeverity::Information;
	case problems::EMarkerSeverity::Hint: return EProblemsPanelSeverity::Hint;
	}
	return EProblemsPanelSeverity::Error;
}

[[nodiscard]] std::wstring ProblemLocation(const std::wstring& resourceUri, const ProblemsPanelRange& range)
{
	return resourceUri + L":" + std::to_wstring(static_cast<std::uint64_t>(range.startLine) + 1U) +
		L":" + std::to_wstring(static_cast<std::uint64_t>(range.startColumn) + 1U);
}

[[nodiscard]] bool ProblemsPanelEntryLess(const ProblemsPanelEntry& left, const ProblemsPanelEntry& right) noexcept
{
	return std::tie(left.resourceUri, left.range.startLine, left.range.startColumn, left.range.endLine,
		left.range.endColumn, left.severity, left.message, left.source) <
		std::tie(right.resourceUri, right.range.startLine, right.range.startColumn, right.range.endLine,
			right.range.endColumn, right.severity, right.message, right.source);
}

} // namespace

ProblemsPanelSnapshot ProjectProblemsPanel(const problems::ProblemsSnapshot& snapshot)
{
	ProblemsPanelSnapshot result{ .revision = snapshot.revision, .stopped = snapshot.stopped };
	for (const auto& resource : snapshot.resources) {
		const auto resourceUri = resource.resource.ToString();
		for (const auto& marker : resource.markers) {
			ProblemsPanelEntry entry{
				.resourceUri = resourceUri,
				.range = { marker.range.startLine, marker.range.startColumn, marker.range.endLine, marker.range.endColumn },
				.severity = ProjectSeverity(marker.severity),
				.message = Utf8ToWide(marker.message),
				.source = marker.source ? Utf8ToWide(*marker.source) : std::wstring{},
			};
			entry.location = ProblemLocation(entry.resourceUri, entry.range);
			result.entries.push_back(std::move(entry));
		}
	}

	std::sort(result.entries.begin(), result.entries.end(), ProblemsPanelEntryLess);
	return result;
}

OutputPanelSnapshot ProjectOutputPanel(const output::OutputServiceSnapshot& snapshot)
{
	OutputPanelSnapshot result{
		.revision = snapshot.revision,
		.stopped = snapshot.stopped,
		.activeChannelId = snapshot.activeChannelId,
	};
	result.channels.reserve(snapshot.channels.size());
	for (const auto& channel : snapshot.channels) {
		result.channels.push_back({
			.channelId = channel.channelId,
			.label = Utf8ToWide(channel.label),
			.projectedText = Utf8ToWide(channel.projectedText),
			.visible = channel.visible,
			.lastShowPreservedFocus = channel.lastShowPreservedFocus,
		});
	}
	std::sort(result.channels.begin(), result.channels.end(), [](const OutputPanelChannel& left, const OutputPanelChannel& right) {
		return left.channelId < right.channelId;
	});
	return result;
}

} // namespace workbench::win32
