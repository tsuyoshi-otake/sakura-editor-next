/*! @file
	@brief HWND-free Problems and Output panel presentation snapshots.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "workbench/output/OutputServiceTypes.h"

namespace workbench::problems {
struct ProblemsSnapshot;
}

namespace workbench::output {
struct OutputServiceSnapshot;
}

namespace workbench::win32 {

//! The native panel preserves the complete zero-based half-open marker range.
struct ProblemsPanelRange final {
	std::uint32_t startLine{};
	std::uint32_t startColumn{};
	std::uint32_t endLine{};
	std::uint32_t endColumn{};

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelRange&) const noexcept = default;
};

enum class EProblemsPanelSeverity : std::uint8_t {
	Error,
	Warning,
	Information,
	Hint,
};

//! VS Code's IRelatedInformation projected for the native Problems view.
struct ProblemsPanelRelatedInformation final {
	std::wstring resourceUri;
	ProblemsPanelRange range;
	std::wstring message;

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelRelatedInformation&) const noexcept = default;
};

//! A value-only native panel entry. resourceUri is a full canonical URI, never a truncated file path.
struct ProblemsPanelEntry final {
	std::wstring resourceUri;
	ProblemsPanelRange range;
	EProblemsPanelSeverity severity{ EProblemsPanelSeverity::Error };
	std::wstring message;
	std::wstring source;
	std::wstring code;
	//! The marker-collection owner identifier; empty when the service has none.
	std::wstring owner;
	std::vector<ProblemsPanelRelatedInformation> relatedInformation;

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelEntry&) const noexcept = default;
};

struct ProblemsPanelSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::vector<ProblemsPanelEntry> entries;

	[[nodiscard]] constexpr bool operator==(const ProblemsPanelSnapshot&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// VS Code Problems view (`workbench.panel.markers.view`) tree projection.
// Pure: no HWND, theme, font, or mutation; consumed by the native panel tool.
// ---------------------------------------------------------------------------

//! The Problems view's filter state. The text box is parsed separately through
//! ParseProblemsViewFilterText; the toggles mirror upstream's More Filters menu.
struct ProblemsViewFilterState final {
	std::wstring text;
	bool showErrors{ true };
	bool showWarnings{ true };
	bool showInfos{ true };
	bool activeFileOnly{};

	[[nodiscard]] constexpr bool operator==(const ProblemsViewFilterState&) const noexcept = default;
};

//! Parsed `markers.action.filter` text: `@source:` filters, `!` negation, and
//! comma-separated glob patterns, exactly as upstream's FilterOptions builds it.
struct ProblemsViewFilterOptions final {
	std::wstring text;
	bool negate{};

	std::vector<std::wstring> includePatterns;
	std::vector<std::wstring> excludePatterns;
	std::vector<std::wstring> includeSources;
	std::vector<std::wstring> excludeSources;

	[[nodiscard]] constexpr bool operator==(const ProblemsViewFilterOptions&) const noexcept = default;
};

[[nodiscard]] ProblemsViewFilterOptions ParseProblemsViewFilterText(std::wstring_view filterText);

//! VS Code's fuzzy match (matchesFuzzy2): every query character must appear in
//! order in the candidate; the match itself is case-insensitive.
[[nodiscard]] bool ProblemsViewMatchesFuzzy(std::wstring_view query, std::wstring_view candidate) noexcept;

//! Matches `pattern` (a VS Code glob like `*.ts` or `** /node_modules/**`) against
//! the resource path. Patterns are expanded the way FilterOptions.setPattern does
//! (`**/{pattern}` and `**/{pattern}/**`), so a pattern may hit any contiguous
//! run of path segments. `*` and `?` work inside a segment; a `**` segment spans
//! any number of segments. Matching is case-sensitive, like upstream's glob.
[[nodiscard]] bool ProblemsViewGlobMatches(std::wstring_view pattern, std::wstring_view resourcePath);

struct ProblemsViewMarker final {
	ProblemsPanelEntry entry;
	//! VS Code Marker.lines: the message split on \r\n, \r, or \n; at least one.
	std::vector<std::wstring> lines;

	[[nodiscard]] bool operator==(const ProblemsViewMarker&) const noexcept = default;
};

//! VS Code ResourceMarkers: one file and its sorted, filter-visible markers.
struct ProblemsViewResource final {
	std::wstring resourceUri;
	//! The label path (Windows path for file URIs); also the sort key upstream calls `path`.
	std::wstring path;
	std::wstring name;
	std::wstring description;
	std::vector<ProblemsViewMarker> markers;

	[[nodiscard]] bool operator==(const ProblemsViewResource&) const noexcept = default;
};

enum class EProblemsViewEmptyState : std::uint8_t {
	None,
	//! "No problems have been detected in the workspace."
	Workspace,
	//! "No problems have been detected in the current file."
	ActiveFile,
	//! "No results found with provided filter criteria." + Clear Filters.
	Filtered,
};

//! The fully projected view: grouped, sorted, filtered rows plus the counts the
//! tab badge and the `Showing X of Y` filter badge need.
struct ProblemsViewTree final {
	std::uint64_t revision{};
	bool stopped{};
	std::vector<ProblemsViewResource> resources;
	//! Total Error+Warning+Info markers in the model (the badge's Y and the tab count).
	std::uint32_t totalMarkers{};
	//! Markers visible after every filter (the badge's X).
	std::uint32_t visibleMarkers{};
	std::uint32_t errors{};
	std::uint32_t warnings{};
	std::uint32_t infos{};
	EProblemsViewEmptyState emptyState{ EProblemsViewEmptyState::None };

	[[nodiscard]] bool operator==(const ProblemsViewTree&) const noexcept = default;
};

//! Groups, sorts, and filters the flat panel projection exactly as VS Code's
//! MarkersModel + Filter do. `activeResourceUri` is the canonical URI of the
//! active editor's document when `state.activeFileOnly` is on.
[[nodiscard]] ProblemsViewTree ProjectProblemsViewTree(
	const ProblemsPanelSnapshot& snapshot,
	const ProblemsViewFilterState& state,
	const std::optional<std::wstring>& activeResourceUri);

//! VS Code Marker.toString(): the marker serialized as tab-indented JSON.
[[nodiscard]] std::wstring SerializeProblemsViewMarker(const ProblemsPanelEntry& entry);

//! problems.action.copy payload: `[{marker},{marker}]` joined like upstream's
//! `[${markers}]` (each element serialized via SerializeProblemsViewMarker).
[[nodiscard]] std::wstring BuildProblemsViewCopyText(const std::vector<const ProblemsPanelEntry*>& entries);

//! A value-only channel presentation. projectedText covers both Output and structured Log channels.
struct OutputPanelChannel final {
	std::string channelId;
	std::wstring label;
	std::wstring projectedText;
	//! Output/Log channel classification from the service model.
	workbench::output::EOutputChannelKind kind{ workbench::output::EOutputChannelKind::Output };
	//! The channel owner's stable id (for example "core.tasks"); extension-versus-core
	//! grouping in `workbench.action.showOutputChannels` reads this, never the label.
	std::string ownerId;
	bool visible{};
	bool lastShowPreservedFocus{};

	[[nodiscard]] constexpr bool operator==(const OutputPanelChannel&) const noexcept = default;
};

struct OutputPanelSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::optional<std::string> activeChannelId;
	std::vector<OutputPanelChannel> channels;

	[[nodiscard]] constexpr bool operator==(const OutputPanelSnapshot&) const noexcept = default;
};

//! Projects a copied MarkerService snapshot into a deterministic native presentation value.
[[nodiscard]] ProblemsPanelSnapshot ProjectProblemsPanel(const problems::ProblemsSnapshot& snapshot);

//! Projects a copied OutputService snapshot into a deterministic native presentation value.
[[nodiscard]] OutputPanelSnapshot ProjectOutputPanel(const output::OutputServiceSnapshot& snapshot);

// ---------------------------------------------------------------------------
// VS Code Output view (`workbench.panel.output`) projections.
// Pure: no HWND, theme, font, or mutation; consumed by the native panel tool.
// ---------------------------------------------------------------------------

//! The Output view filter's parsed text. Upstream's classic filter splits the
//! input on commas with `splitGlobAware` (commas inside double quotes are
//! literal), trims each piece, and treats `!`-prefixed pieces as exclusion
//! patterns. Surrounding double quotes are stripped from each pattern.
struct OutputViewFilterOptions final {
	std::vector<std::wstring> includePatterns;
	std::vector<std::wstring> excludePatterns;

	[[nodiscard]] bool operator==(const OutputViewFilterOptions&) const noexcept = default;
};

[[nodiscard]] OutputViewFilterOptions ParseOutputViewFilterText(std::wstring_view filterText);

//! One half-open character range inside `OutputViewFilteredText::text`, where
//! an include pattern matched. The native view paints these as upstream's
//! find-match highlight decorations.
struct OutputViewTextRange final {
	std::uint32_t offset{};
	std::uint32_t length{};

	[[nodiscard]] constexpr bool operator==(const OutputViewTextRange&) const noexcept = default;
};

//! The filtered display text for one channel. `text` joins the visible lines
//! with \r\n, the native control's line ending; `hiddenLineCount` feeds no
//! badge upstream but keeps the projection's loss explicit.
struct OutputViewFilteredText final {
	std::wstring text;
	std::vector<OutputViewTextRange> matchRanges;
	std::uint32_t hiddenLineCount{};

	[[nodiscard]] bool operator==(const OutputViewFilteredText&) const noexcept = default;
};

//! Upstream's per-line filter: a line is visible when no exclude pattern is a
//! substring of it and (with no include patterns, or when at least one include
//! pattern is a substring). Matching is case-sensitive like upstream's Matcher.
[[nodiscard]] OutputViewFilteredText ApplyOutputViewFilter(
	std::wstring_view projectedText, const OutputViewFilterOptions& options);

//! Upstream's channel grouping for both `workbench.action.showOutputChannels`
//! and the view-title `workbench.output.menu.switchOutput` selector: channels
//! whose owner is not a `core.*` workbench producer count as extension
//! channels and come first, then the core channels. Upstream also has a third
//! `2_user_outputchannels` group behind `channel.user`; this service has no
//! user-channel kind, so the projection carries the two groups it can prove.
//! Each group is ordered by label.
struct OutputViewChannelGroups final {
	std::vector<std::size_t> extensionChannels;
	std::vector<std::size_t> coreChannels;

	//! Upstream inserts one separator only when both groups are non-empty.
	[[nodiscard]] bool HasSeparator() const noexcept
	{
		return !extensionChannels.empty() && !coreChannels.empty();
	}

	[[nodiscard]] bool operator==(const OutputViewChannelGroups&) const noexcept = default;
};

[[nodiscard]] OutputViewChannelGroups GroupOutputViewChannels(
	const std::vector<OutputPanelChannel>& channels);

} // namespace workbench::win32
