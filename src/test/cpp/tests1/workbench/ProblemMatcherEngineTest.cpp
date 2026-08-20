/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/tasks/ProblemMatcherEngine.h"

#include <string>
#include <vector>

namespace workbench::tasks {
namespace {

platform::uri::Uri WindowsPath(const wchar_t* value)
{
	auto parsed = platform::uri::Uri::FromWindowsPath(value);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

ProblemMatcherRunContext AbsoluteContext(const char* ownerId = "task.compiler", const std::uint64_t generation = 1,
	const char* collectionId = "build")
{
	ProblemMatcherRunContext context;
	context.owner = { ownerId, generation };
	context.collectionId = collectionId;
	return context;
}

ProblemMatcherDefinition SingleLineDefinition(std::wstring regexp, const int fileGroup, const int lineGroup,
	const int columnGroup, const int severityGroup, const int codeGroup, const int messageGroup)
{
	ProblemMatcherDefinition definition;
	definition.owner = L"compiler";
	definition.source = L"compiler";
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;
	ProblemMatcherPattern pattern;
	pattern.regexp = std::move(regexp);
	if (fileGroup) pattern.file = fileGroup;
	if (lineGroup) pattern.line = lineGroup;
	if (columnGroup) pattern.column = columnGroup;
	if (severityGroup) pattern.severity = severityGroup;
	if (codeGroup) pattern.code = codeGroup;
	if (messageGroup) pattern.message = messageGroup;
	definition.patterns = { pattern };
	return definition;
}

TEST(ProblemMatcherEngine, MatchesOneLineAndProducesAReplaceRequestForItsResource)
{
	const auto definition = SingleLineDefinition(
		LR"(^(.*?)\((\d+),(\d+)\):\s*(error|warning)\s+([A-Z]\d+):\s*(.*)$)", 1, 2, 3, 4, 5, 6);
	const std::vector<std::string> lines = { "C:\\src\\main.cpp(10,5): error C1234: unexpected token" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	const auto& request = outcome.replacements.front();
	EXPECT_EQ("task.compiler", request.collection.owner.id);
	EXPECT_EQ("build", request.collection.id);
	ASSERT_EQ(1U, request.markers.size());
	const auto& marker = request.markers.front();
	EXPECT_EQ(9U, marker.range.startLine);
	EXPECT_EQ(4U, marker.range.startColumn);
	EXPECT_EQ(problems::EMarkerSeverity::Error, marker.severity);
	EXPECT_EQ("unexpected token", marker.message);
	ASSERT_TRUE(marker.code.has_value());
	EXPECT_EQ("C1234", *marker.code);
}

TEST(ProblemMatcherEngine, GroupsMultipleMarkersByResourceAndOrdersResourcesDeterministically)
{
	const auto definition = SingleLineDefinition(
		LR"(^(.*?)\((\d+),(\d+)\):\s*(error|warning)\s+([A-Z]\d+):\s*(.*)$)", 1, 2, 3, 4, 5, 6);
	const std::vector<std::string> lines = {
		"C:\\src\\zeta.cpp(1,1): error E001: first",
		"C:\\src\\alpha.cpp(2,2): warning W002: second",
		"C:\\src\\zeta.cpp(3,3): error E003: third",
	};

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(2U, outcome.replacements.size());
	EXPECT_EQ(WindowsPath(L"C:\\src\\alpha.cpp").ToString(), outcome.replacements[0].resource.ToString());
	ASSERT_EQ(1U, outcome.replacements[0].markers.size());
	EXPECT_EQ(WindowsPath(L"C:\\src\\zeta.cpp").ToString(), outcome.replacements[1].resource.ToString());
	ASSERT_EQ(2U, outcome.replacements[1].markers.size());
}

TEST(ProblemMatcherEngine, ResolvesBuiltinMsCompileMatcherByName)
{
	const auto lookup = BuiltinProblemMatchers::Resolve(L"$msCompile");
	ASSERT_TRUE(lookup.Succeeded());
	const std::vector<std::string> lines = { "C:\\src\\main.cpp(4,2): error C2065: undeclared identifier" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(*lookup.definition, lines, AbsoluteContext());
	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	ASSERT_EQ(1U, outcome.replacements.front().markers.size());
	EXPECT_EQ("undeclared identifier", outcome.replacements.front().markers.front().message);
}

TEST(ProblemMatcherEngine, UnknownBuiltinNameIsRejected)
{
	const auto lookup = BuiltinProblemMatchers::Resolve(L"$notARealMatcher");
	EXPECT_EQ(EProblemMatcherLookupStatus::UnknownName, lookup.status);
	EXPECT_FALSE(lookup.definition.has_value());
}

TEST(ProblemMatcherEngine, ResolvesARelativeFileLocationAgainstTheSuppliedWorkspaceRoot)
{
	auto definition = SingleLineDefinition(LR"(^(.*?):(\d+):\s*(.*)$)", 1, 2, 0, 0, 0, 3);
	definition.fileLocation = EProblemMatcherFileLocation::Relative;
	ProblemMatcherRunContext context = AbsoluteContext();
	context.workspaceRoot = WindowsPath(L"C:\\Workspace\\project");
	const std::vector<std::string> lines = { "src/main.cpp:7: broken thing" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, context);

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	EXPECT_EQ(WindowsPath(L"C:\\Workspace\\project\\src\\main.cpp").ToString(),
		outcome.replacements.front().resource.ToString());
}

TEST(ProblemMatcherEngine, RelativeFileLocationWithoutAWorkspaceRootIsATypedFailure)
{
	auto definition = SingleLineDefinition(LR"(^(.*?):(\d+):\s*(.*)$)", 1, 2, 0, 0, 0, 3);
	definition.fileLocation = EProblemMatcherFileLocation::Relative;
	const std::vector<std::string> lines = { "src/main.cpp:7: broken thing" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::MissingWorkspaceRoot, outcome.status);
	EXPECT_TRUE(outcome.replacements.empty());
}

TEST(ProblemMatcherEngine, InvalidRegexpIsATypedFailureRatherThanASilentNoMatch)
{
	auto definition = SingleLineDefinition(L"(unterminated[", 1, 2, 0, 0, 0, 3);
	const std::vector<std::string> lines = { "irrelevant" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidRegexp, outcome.status);
}

TEST(ProblemMatcherEngine, OutOfRangeCaptureGroupIndexIsATypedFailure)
{
	// The pattern has only one capture group, but "line" references group 5.
	auto definition = SingleLineDefinition(LR"(^(.*?):(\d+):(.*)$)", 1, 5, 0, 0, 0, 3);

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, {}, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidGroupIndex, outcome.status);
}

TEST(ProblemMatcherEngine, ZeroLineNumberIsRejectedRatherThanTreatedAsOneBased)
{
	const auto definition = SingleLineDefinition(LR"(^(.*?)\((\d+),(\d+)\):\s*(.*)$)", 1, 2, 3, 0, 0, 4);
	const std::vector<std::string> lines = { "C:\\a.cpp(0,1): boom" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidLineOrColumnValue, outcome.status);
	ASSERT_TRUE(outcome.failingLineIndex.has_value());
	EXPECT_EQ(0U, *outcome.failingLineIndex);
}

TEST(ProblemMatcherEngine, NegativeLikeLineTextIsRejectedRatherThanParsed)
{
	// "-5" cannot even be captured by \d+, so this exercises the from_chars rejection path
	// through a group that captures non-digit-only text.
	auto definition = SingleLineDefinition(LR"(^(.*?)\((.+),(\d+)\):\s*(.*)$)", 1, 2, 3, 0, 0, 4);
	const std::vector<std::string> lines = { "C:\\a.cpp(-5,1): boom" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidLineOrColumnValue, outcome.status);
}

TEST(ProblemMatcherEngine, UnrecognizedSeverityWordIsATypedFailureRatherThanADefault)
{
	const auto definition = SingleLineDefinition(
		LR"(^(.*?)\((\d+),(\d+)\):\s*(\w+)\s*:\s*(.*)$)", 1, 2, 3, 4, 0, 5);
	const std::vector<std::string> lines = { "C:\\a.cpp(1,1): catastrophe: boom" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidSeverityValue, outcome.status);
}

TEST(ProblemMatcherEngine, MalformedUtf8LineIsATypedFailure)
{
	const auto definition = SingleLineDefinition(LR"(^(.*?)\((\d+),(\d+)\):\s*(.*)$)", 1, 2, 3, 0, 0, 4);
	const std::vector<std::string> lines = { std::string("C:\\a.cpp(1,1): broken \xC0\x80 sequence") };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::InvalidUtf8Line, outcome.status);
	ASSERT_TRUE(outcome.failingLineIndex.has_value());
	EXPECT_EQ(0U, *outcome.failingLineIndex);
}

TEST(ProblemMatcherEngine, EmptyPatternListIsATypedFailure)
{
	ProblemMatcherDefinition definition;
	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, {}, AbsoluteContext());
	EXPECT_EQ(EProblemMatchStatus::EmptyPatternList, outcome.status);
}

TEST(ProblemMatcherEngine, MatcherMissingAMessageCaptureAcrossEveryPatternIsATypedFailure)
{
	ProblemMatcherDefinition definition;
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;
	ProblemMatcherPattern pattern;
	pattern.regexp = LR"(^(.*?)\((\d+)\)$)";
	pattern.file = 1;
	pattern.line = 2;
	definition.patterns = { pattern };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, {}, AbsoluteContext());

	EXPECT_EQ(EProblemMatchStatus::MissingRequiredCaptureField, outcome.status);
}

TEST(ProblemMatcherEngine, TooManyOutputLinesIsATypedFailure)
{
	const auto definition = SingleLineDefinition(LR"(^(.*?)\((\d+),(\d+)\):\s*(.*)$)", 1, 2, 3, 0, 0, 4);
	ProblemMatcherEngineLimits limits;
	limits.maximumLines = 1;
	const std::vector<std::string> lines = { "C:\\a.cpp(1,1): first", "C:\\a.cpp(2,1): second" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext(), limits);

	EXPECT_EQ(EProblemMatchStatus::MaximumLinesExceeded, outcome.status);
}

TEST(ProblemMatcherEngine, OverlongLineIsATypedFailure)
{
	const auto definition = SingleLineDefinition(LR"(^(.*?)\((\d+),(\d+)\):\s*(.*)$)", 1, 2, 3, 0, 0, 4);
	ProblemMatcherEngineLimits limits;
	limits.maximumLineLength = 8;
	const std::vector<std::string> lines = { "C:\\a.cpp(1,1): far too long a message for the limit" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext(), limits);

	EXPECT_EQ(EProblemMatchStatus::MaximumLineLengthExceeded, outcome.status);
}

TEST(ProblemMatcherEngine, MatchesAMultilineChainAndAssociatesTheSecondLineWithTheFirstLinesFile)
{
	ProblemMatcherDefinition definition;
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;
	ProblemMatcherPattern header;
	header.regexp = LR"(^File:\s*(.*)$)";
	header.file = 1;
	ProblemMatcherPattern detail;
	detail.regexp = LR"(^\s*Line\s*(\d+):\s*(error|warning)\s*:\s*(.*)$)";
	detail.line = 1;
	detail.severity = 2;
	detail.message = 3;
	definition.patterns = { header, detail };
	const std::vector<std::string> lines = { "File: C:\\a.cpp", "  Line 9: error: oops" };

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	ASSERT_EQ(1U, outcome.replacements.front().markers.size());
	EXPECT_EQ(8U, outcome.replacements.front().markers.front().range.startLine);
	EXPECT_EQ("oops", outcome.replacements.front().markers.front().message);
}

TEST(ProblemMatcherEngine, ALoopingLastPatternProducesMultipleMarkersUnderOneUnchangedHeader)
{
	ProblemMatcherDefinition definition;
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;
	ProblemMatcherPattern header;
	header.regexp = LR"(^File:\s*(.*)$)";
	header.file = 1;
	ProblemMatcherPattern detail;
	detail.regexp = LR"(^\s*Line\s*(\d+):\s*(error|warning)\s*:\s*(.*)$)";
	detail.line = 1;
	detail.severity = 2;
	detail.message = 3;
	detail.loop = true;
	definition.patterns = { header, detail };
	const std::vector<std::string> lines = {
		"File: C:\\a.cpp",
		"  Line 1: error: first",
		"  Line 2: warning: second",
	};

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	ASSERT_EQ(2U, outcome.replacements.front().markers.size());
}

TEST(ProblemMatcherEngine, AChainBrokenBeforeItsLastPatternResetsAndRetriesTheSameLineFromTheStart)
{
	ProblemMatcherDefinition definition;
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;
	ProblemMatcherPattern header;
	header.regexp = LR"(^File:\s*(.*)$)";
	header.file = 1;
	ProblemMatcherPattern detail;
	detail.regexp = LR"(^\s*Line\s*(\d+):\s*(error|warning)\s*:\s*(.*)$)";
	detail.line = 1;
	detail.severity = 2;
	detail.message = 3;
	definition.patterns = { header, detail };
	const std::vector<std::string> lines = {
		"File: C:\\a.cpp",
		"an unrelated line that matches neither pattern",
		"File: C:\\b.cpp",
		"  Line 4: error: recovered",
	};

	const auto outcome = ProblemMatcherEngine::ProcessOutputLines(definition, lines, AbsoluteContext());

	ASSERT_TRUE(outcome.Succeeded());
	ASSERT_EQ(1U, outcome.replacements.size());
	EXPECT_EQ(WindowsPath(L"C:\\b.cpp").ToString(), outcome.replacements.front().resource.ToString());
	ASSERT_EQ(1U, outcome.replacements.front().markers.size());
	EXPECT_EQ("recovered", outcome.replacements.front().markers.front().message);
}

} // namespace
} // namespace workbench::tasks
