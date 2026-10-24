/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

platform::uri::Uri Uri(std::wstring text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

workbench::problems::ProblemMarker Marker(
	std::uint32_t startLine, std::uint32_t startColumn, std::uint32_t endLine, std::uint32_t endColumn,
	workbench::problems::EMarkerSeverity severity, std::string message, std::string source)
{
	workbench::problems::ProblemMarker marker;
	marker.range = { startLine, startColumn, endLine, endColumn };
	marker.severity = severity;
	marker.message = std::move(message);
	marker.source = std::move(source);
	return marker;
}

} // namespace

TEST(ProblemsOutputPanelProjection, FlattensMarkerResourcesDeterministicallyAndPreservesFullHalfOpenRange)
{
	workbench::problems::ProblemsSnapshot snapshot;
	snapshot.revision = 41;
	snapshot.resources = {
		{ Uri(L"file:///C:/zeta.txt"), {}, { Marker(7, 3, 9, 11, workbench::problems::EMarkerSeverity::Warning,
			"later", "lint") } },
		{ Uri(L"file:///C:/alpha.txt"), {}, { Marker(0, 0, 0, 5, workbench::problems::EMarkerSeverity::Error,
			"first", "compiler") } },
	};

	const auto first = workbench::win32::ProjectProblemsPanel(snapshot);
	std::ranges::reverse(snapshot.resources);
	const auto second = workbench::win32::ProjectProblemsPanel(snapshot);

	EXPECT_EQ(first, second);
	ASSERT_EQ(2U, first.entries.size());
	EXPECT_EQ(L"file:///C:/alpha.txt", first.entries[0].resourceUri);
	EXPECT_EQ(L"file:///C:/zeta.txt", first.entries[1].resourceUri);
	EXPECT_EQ(7U, first.entries[1].range.startLine);
	EXPECT_EQ(3U, first.entries[1].range.startColumn);
	EXPECT_EQ(9U, first.entries[1].range.endLine);
	EXPECT_EQ(11U, first.entries[1].range.endColumn);
	EXPECT_EQ(workbench::win32::EProblemsPanelSeverity::Warning, first.entries[1].severity);
}

TEST(ProblemsOutputPanelProjection, PreservesOutputAuthorityMetadataAndProjectedTextDeterministically)
{
	workbench::output::OutputServiceSnapshot snapshot;
	snapshot.revision = 12;
	snapshot.activeChannelId = "channel-b";

	workbench::output::OutputChannelSnapshot second;
	second.channelId = "channel-b";
	second.label = "Build Log";
	second.projectedText = "[info] finished";
	second.visible = true;
	second.lastShowPreservedFocus = true;

	workbench::output::OutputChannelSnapshot first;
	first.channelId = "channel-a";
	first.label = "Build";
	first.projectedText = "compiler output";
	first.visible = false;
	first.lastShowPreservedFocus = false;

	snapshot.channels = { second, first };
	const auto projection = workbench::win32::ProjectOutputPanel(snapshot);

	EXPECT_EQ(12U, projection.revision);
	EXPECT_FALSE(projection.stopped);
	ASSERT_TRUE(projection.activeChannelId.has_value());
	EXPECT_EQ("channel-b", *projection.activeChannelId);
	ASSERT_EQ(2U, projection.channels.size());
	EXPECT_EQ("channel-a", projection.channels[0].channelId);
	EXPECT_EQ("channel-b", projection.channels[1].channelId);
	EXPECT_EQ(L"[info] finished", projection.channels[1].projectedText);
	EXPECT_TRUE(projection.channels[1].visible);
	EXPECT_TRUE(projection.channels[1].lastShowPreservedFocus);
}

TEST(ProblemsOutputPanelProjection, ParsesProblemsViewFilterTextLikeUpstreamFilterOptions)
{
	const auto plain = workbench::win32::ParseProblemsViewFilterText(L"eslint");
	EXPECT_EQ(L"eslint", plain.text);
	EXPECT_FALSE(plain.negate);
	EXPECT_TRUE(plain.includeSources.empty());
	EXPECT_TRUE(plain.excludeSources.empty());
	// Every non-negated comma piece is also a resource glob upstream.
	EXPECT_EQ(std::vector<std::wstring>{ L"eslint" }, plain.includePatterns);
	EXPECT_TRUE(plain.excludePatterns.empty());

	const auto source = workbench::win32::ParseProblemsViewFilterText(L"@source:ESLint foo");
	EXPECT_EQ(L"foo", source.text);
	EXPECT_EQ(std::vector<std::wstring>{ L"eslint" }, source.includeSources);

	const auto negatedSource = workbench::win32::ParseProblemsViewFilterText(L"!@source:eslint");
	EXPECT_EQ(std::vector<std::wstring>{ L"eslint" }, negatedSource.excludeSources);
	EXPECT_TRUE(negatedSource.text.empty());

	const auto quoted = workbench::win32::ParseProblemsViewFilterText(L"@source:\"my source\"");
	EXPECT_EQ(std::vector<std::wstring>{ L"my source" }, quoted.includeSources);

	const auto negated = workbench::win32::ParseProblemsViewFilterText(L"!word");
	EXPECT_TRUE(negated.negate);
	EXPECT_EQ(L"word", negated.text);

	const auto globs = workbench::win32::ParseProblemsViewFilterText(L"*.ts, !*.d.ts");
	EXPECT_EQ(std::vector<std::wstring>{ L"*.ts" }, globs.includePatterns);
	EXPECT_EQ(std::vector<std::wstring>{ L"*.d.ts" }, globs.excludePatterns);
}

TEST(ProblemsOutputPanelProjection, MatchesFuzzyLikeUpstreamMatchesFuzzy2)
{
	EXPECT_TRUE(workbench::win32::ProblemsViewMatchesFuzzy(L"ts", L"TypeScript"));
	EXPECT_TRUE(workbench::win32::ProblemsViewMatchesFuzzy(L"TS", L"typescript"));
	EXPECT_TRUE(workbench::win32::ProblemsViewMatchesFuzzy(L"mkr", L"marker"));
	EXPECT_FALSE(workbench::win32::ProblemsViewMatchesFuzzy(L"cba", L"abc"));
	EXPECT_FALSE(workbench::win32::ProblemsViewMatchesFuzzy(L"", L"marker"));
	EXPECT_FALSE(workbench::win32::ProblemsViewMatchesFuzzy(L"x", L""));
}

TEST(ProblemsOutputPanelProjection, GlobMatchesResourcePathsLikeUpstreamSetPattern)
{
	EXPECT_TRUE(workbench::win32::ProblemsViewGlobMatches(L"*.ts", L"C:\\src\\a.ts"));
	EXPECT_TRUE(workbench::win32::ProblemsViewGlobMatches(L".ts", L"C:\\src\\a.ts"));
	EXPECT_TRUE(workbench::win32::ProblemsViewGlobMatches(
		L"**/node_modules/**", L"C:\\w\\node_modules\\pkg\\index.js"));
	EXPECT_TRUE(workbench::win32::ProblemsViewGlobMatches(L"src/*.ts", L"C:\\w\\src\\a.ts"));
	EXPECT_TRUE(workbench::win32::ProblemsViewGlobMatches(L"a.??", L"C:\\src\\a.ts"));
	EXPECT_FALSE(workbench::win32::ProblemsViewGlobMatches(L"*.js", L"C:\\src\\a.ts"));
	EXPECT_FALSE(workbench::win32::ProblemsViewGlobMatches(L"", L"C:\\src\\a.ts"));
	EXPECT_FALSE(workbench::win32::ProblemsViewGlobMatches(L"*.ts", L""));
}

TEST(ProblemsOutputPanelProjection, GroupsSortsAndCountsLikeMarkersModel)
{
	workbench::problems::ProblemsSnapshot snapshot;
	snapshot.revision = 3;
	snapshot.resources = {
		{ Uri(L"file:///C:/dir/zeta.txt"), {}, {
			Marker(0, 0, 0, 1, workbench::problems::EMarkerSeverity::Information, "info", "lint"),
			Marker(1, 0, 1, 1, workbench::problems::EMarkerSeverity::Error, "err", "lint"),
			Marker(2, 0, 2, 1, workbench::problems::EMarkerSeverity::Hint, "hint", "lint"),
		} },
		{ Uri(L"file:///C:/dir/alpha.txt"), {}, {
			Marker(4, 2, 4, 6, workbench::problems::EMarkerSeverity::Warning, "warn", "lint"),
		} },
	};
	// The task-matcher/language-server double-report shape upstream removes (#244424).
	snapshot.resources.push_back(snapshot.resources[1]);

	const auto panel = workbench::win32::ProjectProblemsPanel(snapshot);
	const auto tree = workbench::win32::ProjectProblemsViewTree(panel, {}, std::nullopt);

	EXPECT_EQ(3U, tree.revision);
	EXPECT_EQ(3U, tree.totalMarkers); // Hint is not a model severity; the dup is merged.
	EXPECT_EQ(3U, tree.visibleMarkers);
	EXPECT_EQ(1U, tree.errors);
	EXPECT_EQ(1U, tree.warnings);
	EXPECT_EQ(1U, tree.infos);
	EXPECT_EQ(workbench::win32::EProblemsViewEmptyState::None, tree.emptyState);

	ASSERT_EQ(2U, tree.resources.size());
	// The resource containing an Error sorts before the Warning-only resource.
	EXPECT_EQ(L"file:///C:/dir/zeta.txt", tree.resources[0].resourceUri);
	EXPECT_EQ(L"zeta.txt", tree.resources[0].name);
	EXPECT_EQ(L"C:\\dir", tree.resources[0].description);
	EXPECT_EQ(L"alpha.txt", tree.resources[1].name);

	const auto& markers = tree.resources[0].markers;
	ASSERT_EQ(2U, markers.size());
	EXPECT_EQ(workbench::win32::EProblemsPanelSeverity::Error, markers[0].entry.severity);
	EXPECT_EQ(workbench::win32::EProblemsPanelSeverity::Information, markers[1].entry.severity);
}

TEST(ProblemsOutputPanelProjection, AppliesSeverityActiveFileAndTextFiltersLikeUpstreamFilter)
{
	workbench::problems::ProblemsSnapshot snapshot;
	snapshot.resources = {
		{ Uri(L"file:///C:/dir/one.txt"), {}, {
			Marker(0, 0, 0, 1, workbench::problems::EMarkerSeverity::Error, "first", "eslint"),
			Marker(1, 0, 1, 1, workbench::problems::EMarkerSeverity::Warning, "second", "tsc"),
		} },
		{ Uri(L"file:///C:/dir/two.txt"), {}, {
			Marker(0, 0, 0, 1, workbench::problems::EMarkerSeverity::Error, "third", "eslint"),
		} },
	};
	const auto panel = workbench::win32::ProjectProblemsPanel(snapshot);

	workbench::win32::ProblemsViewFilterState noErrors;
	noErrors.showErrors = false;
	const auto warningsOnly = workbench::win32::ProjectProblemsViewTree(panel, noErrors, std::nullopt);
	EXPECT_EQ(3U, warningsOnly.totalMarkers);
	EXPECT_EQ(1U, warningsOnly.visibleMarkers);
	ASSERT_EQ(1U, warningsOnly.resources.size());
	EXPECT_EQ(L"second", warningsOnly.resources[0].markers[0].entry.message);

	// file URIs compare path-insensitively on Windows (extUri identity).
	workbench::win32::ProblemsViewFilterState activeOnly;
	activeOnly.activeFileOnly = true;
	const auto active = workbench::win32::ProjectProblemsViewTree(
		panel, activeOnly, std::wstring{ L"file:///c:/dir/ONE.TXT" });
	EXPECT_EQ(2U, active.visibleMarkers);
	ASSERT_EQ(1U, active.resources.size());
	EXPECT_EQ(L"file:///C:/dir/one.txt", active.resources[0].resourceUri);

	workbench::win32::ProblemsViewFilterState bySource;
	bySource.text = L"@source:eslint";
	const auto eslintOnly = workbench::win32::ProjectProblemsViewTree(panel, bySource, std::nullopt);
	EXPECT_EQ(2U, eslintOnly.visibleMarkers);
	EXPECT_EQ(2U, eslintOnly.resources.size());

	workbench::win32::ProblemsViewFilterState byPattern;
	byPattern.text = L"*.txt, !one.txt";
	const auto patternFiltered = workbench::win32::ProjectProblemsViewTree(panel, byPattern, std::nullopt);
	EXPECT_EQ(1U, patternFiltered.visibleMarkers);
	ASSERT_EQ(1U, patternFiltered.resources.size());
	EXPECT_EQ(L"two.txt", patternFiltered.resources[0].name);
}

TEST(ProblemsOutputPanelProjection, SelectsTheUpstreamEmptyState)
{
	workbench::problems::ProblemsSnapshot empty;
	const auto emptyPanel = workbench::win32::ProjectProblemsPanel(empty);
	const auto workspace = workbench::win32::ProjectProblemsViewTree(emptyPanel, {}, std::nullopt);
	EXPECT_EQ(workbench::win32::EProblemsViewEmptyState::Workspace, workspace.emptyState);

	workbench::problems::ProblemsSnapshot snapshot;
	snapshot.resources = {
		{ Uri(L"file:///C:/dir/one.txt"), {}, {
			Marker(0, 0, 0, 1, workbench::problems::EMarkerSeverity::Error, "first", "eslint"),
		} },
	};
	const auto panel = workbench::win32::ProjectProblemsPanel(snapshot);

	workbench::win32::ProblemsViewFilterState noMatch;
	noMatch.text = L"zzz";
	const auto filtered = workbench::win32::ProjectProblemsViewTree(panel, noMatch, std::nullopt);
	EXPECT_EQ(workbench::win32::EProblemsViewEmptyState::Filtered, filtered.emptyState);

	workbench::win32::ProblemsViewFilterState activeOnly;
	activeOnly.activeFileOnly = true;
	const auto activeEmpty = workbench::win32::ProjectProblemsViewTree(
		panel, activeOnly, std::wstring{ L"file:///C:/dir/other.txt" });
	EXPECT_EQ(workbench::win32::EProblemsViewEmptyState::ActiveFile, activeEmpty.emptyState);
}

TEST(ProblemsOutputPanelProjection, SerializesMarkersLikeUpstreamToStringAndCopy)
{
	workbench::win32::ProblemsPanelEntry entry;
	entry.resourceUri = L"file:///C:/dir/one.txt";
	entry.range = { 0, 2, 3, 9 };
	entry.severity = workbench::win32::EProblemsPanelSeverity::Warning;
	entry.message = L"say \"hi\"";
	entry.source = L"eslint";
	entry.code = L"no-undef";
	entry.owner = L"ext";
	entry.relatedInformation.push_back({
		.resourceUri = L"file:///C:/dir/two.txt",
		.range = { 4, 0, 4, 5 },
		.message = L"see also",
	});

	const auto json = workbench::win32::SerializeProblemsViewMarker(entry);
	EXPECT_NE(std::wstring::npos, json.find(L"\"resource\": \"/C:/dir/one.txt\""));
	EXPECT_NE(std::wstring::npos, json.find(L"\"severity\": 4"));
	EXPECT_NE(std::wstring::npos, json.find(L"\"message\": \"say \\\"hi\\\"\""));
	EXPECT_NE(std::wstring::npos, json.find(L"\"source\": \"eslint\""));
	EXPECT_NE(std::wstring::npos, json.find(L"\"code\": \"no-undef\""));
	EXPECT_NE(std::wstring::npos, json.find(L"\"startLineNumber\": 1"));
	EXPECT_NE(std::wstring::npos, json.find(L"\"startColumn\": 3"));
	EXPECT_NE(std::wstring::npos, json.find(L"\"endLineNumber\": 4"));
	EXPECT_NE(std::wstring::npos, json.find(L"\"endColumn\": 10"));
	EXPECT_NE(std::wstring::npos, json.find(L"\"relatedInformation\": ["));

	workbench::win32::ProblemsPanelEntry second;
	second.resourceUri = L"file:///C:/dir/two.txt";
	second.range = { 0, 0, 0, 1 };
	second.severity = workbench::win32::EProblemsPanelSeverity::Error;
	second.message = L"other";

	const std::vector<const workbench::win32::ProblemsPanelEntry*> entries{ &entry, &second };
	const auto copy = workbench::win32::BuildProblemsViewCopyText(entries);
	EXPECT_EQ(L'[', copy.front());
	EXPECT_EQ(L']', copy.back());
	EXPECT_NE(std::wstring::npos, copy.find(L"\"severity\": 8"));
	EXPECT_NE(std::wstring::npos, copy.find(L"\"severity\": 4"));
}

TEST(ProblemsOutputPanelProjection, ProjectsRelatedInformationAsChildRows)
{
	workbench::problems::ProblemMarker marker;
	marker.range = { 0, 0, 0, 1 };
	marker.severity = workbench::problems::EMarkerSeverity::Error;
	marker.message = "main";
	marker.relatedInformation.push_back({
		.resource = Uri(L"file:///C:/dir/ref.txt"),
		.range = { 2, 1, 2, 4 },
		.message = "reference",
	});

	workbench::problems::ProblemsSnapshot snapshot;
	snapshot.resources = { { Uri(L"file:///C:/dir/one.txt"), {}, { marker } } };
	const auto panel = workbench::win32::ProjectProblemsPanel(snapshot);

	ASSERT_EQ(1U, panel.entries.size());
	ASSERT_EQ(1U, panel.entries[0].relatedInformation.size());
	EXPECT_EQ(L"file:///C:/dir/ref.txt", panel.entries[0].relatedInformation[0].resourceUri);
	EXPECT_EQ(2U, panel.entries[0].relatedInformation[0].range.startLine);
	EXPECT_EQ(L"reference", panel.entries[0].relatedInformation[0].message);
}

TEST(ProblemsOutputPanelProjection, PreservesStoppedSnapshotsWithoutInventingPresentationState)
{
	workbench::problems::ProblemsSnapshot problems;
	problems.revision = 8;
	problems.stopped = true;
	const auto projectedProblems = workbench::win32::ProjectProblemsPanel(problems);
	EXPECT_EQ(8U, projectedProblems.revision);
	EXPECT_TRUE(projectedProblems.stopped);
	EXPECT_TRUE(projectedProblems.entries.empty());

	workbench::output::OutputServiceSnapshot output;
	output.revision = 9;
	output.stopped = true;
	const auto projectedOutput = workbench::win32::ProjectOutputPanel(output);
	EXPECT_EQ(9U, projectedOutput.revision);
	EXPECT_TRUE(projectedOutput.stopped);
	EXPECT_FALSE(projectedOutput.activeChannelId.has_value());
	EXPECT_TRUE(projectedOutput.channels.empty());
}
