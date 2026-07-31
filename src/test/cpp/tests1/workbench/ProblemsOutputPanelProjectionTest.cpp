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
	EXPECT_EQ(7U, first.entries[1].range.startLine);
	EXPECT_EQ(3U, first.entries[1].range.startColumn);
	EXPECT_EQ(9U, first.entries[1].range.endLine);
	EXPECT_EQ(11U, first.entries[1].range.endColumn);
	EXPECT_EQ(L"file:///C:/zeta.txt:8:4", first.entries[1].location);
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
