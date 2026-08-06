/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "workbench/scm/GitDiffModel.h"
#include "workbench/scm/GitLineStaging.h"

using namespace workbench::scm;

namespace {

//! The object name `git hash-object` prints for an empty blob: 40 hex digits.
constexpr std::wstring_view kEmptyBlobName = L"e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

[[nodiscard]] GitLineChange Modification(int originalStart, int originalEnd, int modifiedStart, int modifiedEnd)
{
	return GitLineChange{ originalStart, originalEnd, modifiedStart, modifiedEnd };
}

} // namespace

// ----------------------------------------------------------------------------
// The two encodings a `LineChange` carries.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, ToGitLineChangesEncodesInsertionWithZeroOriginalEnd)
{
	// A half-open original range of length zero is an insertion, and upstream
	// records the line it follows rather than an empty range.
	GitLineDiff diff;
	diff.changes.push_back(GitLineRangeMapping{ GitLineRange{ 2, 2 }, GitLineRange{ 2, 3 } });

	const auto changes = ToGitLineChanges(diff);

	ASSERT_EQ(1U, changes.size());
	EXPECT_EQ(Modification(1, 0, 2, 2), changes[0]);
	EXPECT_TRUE(changes[0].IsInsertion());
	EXPECT_FALSE(changes[0].IsDeletion());
}

TEST(GitLineStaging, ToGitLineChangesEncodesDeletionWithZeroModifiedEnd)
{
	GitLineDiff diff;
	diff.changes.push_back(GitLineRangeMapping{ GitLineRange{ 2, 3 }, GitLineRange{ 2, 2 } });

	const auto changes = ToGitLineChanges(diff);

	ASSERT_EQ(1U, changes.size());
	EXPECT_EQ(Modification(2, 2, 1, 0), changes[0]);
	EXPECT_TRUE(changes[0].IsDeletion());
	EXPECT_FALSE(changes[0].IsInsertion());
}

TEST(GitLineStaging, ToGitLineChangesKeepsBothSidesOfAModification)
{
	GitLineDiff diff;
	diff.changes.push_back(GitLineRangeMapping{ GitLineRange{ 2, 4 }, GitLineRange{ 2, 3 } });

	const auto changes = ToGitLineChanges(diff);

	ASSERT_EQ(1U, changes.size());
	EXPECT_EQ(Modification(2, 3, 2, 2), changes[0]);
}

TEST(GitLineStaging, InvertGitLineChangeSwapsTheSides)
{
	const GitLineChange insertion = Modification(1, 0, 2, 3);
	const GitLineChange inverted = InvertGitLineChange(insertion);

	// An insertion seen from the other side is a deletion. This is the whole of
	// how unstage reuses the stage algorithm.
	EXPECT_EQ(Modification(2, 3, 1, 0), inverted);
	EXPECT_TRUE(inverted.IsDeletion());
	EXPECT_EQ(insertion, InvertGitLineChange(inverted));
}

TEST(GitLineStaging, IsGitLineChangeBeforeOrdersByModifiedThenOriginal)
{
	EXPECT_TRUE(IsGitLineChangeBefore(Modification(1, 1, 1, 1), Modification(1, 1, 2, 2)));
	EXPECT_FALSE(IsGitLineChangeBefore(Modification(1, 1, 2, 2), Modification(1, 1, 1, 1)));
	EXPECT_TRUE(IsGitLineChangeBefore(Modification(1, 1, 2, 2), Modification(3, 3, 2, 2)));
	EXPECT_FALSE(IsGitLineChangeBefore(Modification(1, 1, 1, 1), Modification(1, 1, 1, 1)));
}

// ----------------------------------------------------------------------------
// The one end-of-line sequence a text model would hold.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, DetectGitTextEolPrefersTheMajorityTerminator)
{
	EXPECT_EQ(L"\n", DetectGitTextEol(L"a\nb\nc"));
	EXPECT_EQ(L"\r\n", DetectGitTextEol(L"a\r\nb\r\nc\n"));
	EXPECT_EQ(L"\r\n", DetectGitTextEol(L"a\rb"));
}

TEST(GitLineStaging, DetectGitTextEolNeedsMoreThanHalfToChooseCarriageReturn)
{
	// Upstream's rule is a strict majority: an even split stays with `\n`.
	EXPECT_EQ(L"\n", DetectGitTextEol(L"a\r\nb\nc"));
}

TEST(GitLineStaging, DetectGitTextEolFallsBackToThePlatformTerminator)
{
	// No terminator is no evidence, so `files.eol`'s `auto` default decides. This
	// product only runs on Windows.
	EXPECT_EQ(L"\r\n", DetectGitTextEol(L"abc"));
	EXPECT_EQ(L"\r\n", DetectGitTextEol(L""));
}

TEST(GitLineStaging, MakeGitStagingTextKeepsTheFinalEmptyLine)
{
	const GitStagingText text = MakeGitStagingText(L"a\nb\n");

	EXPECT_EQ(3, text.LineCount());
	EXPECT_EQ(L"", text.lines.back());
	EXPECT_EQ(L"\n", text.eol);

	// An empty text is one empty line, exactly as an empty `TextDocument` is.
	EXPECT_EQ(1, MakeGitStagingText(L"").LineCount());
}

// ----------------------------------------------------------------------------
// `applyLineChanges`.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, ApplyGitLineChangesWithNoChangesReturnsTheOriginal)
{
	const GitStagingText original = MakeGitStagingText(L"a\nb\n");
	const GitStagingText modified = MakeGitStagingText(L"A\nB\n");

	EXPECT_EQ(L"a\nb\n", ApplyGitLineChanges(original, modified, {}));
}

TEST(GitLineStaging, ApplyGitLineChangesStagesOneModifiedLine)
{
	const GitStagingText original = MakeGitStagingText(L"a\nb\nc\n");
	const GitStagingText modified = MakeGitStagingText(L"a\nB\nc\n");

	EXPECT_EQ(L"a\nB\nc\n", ApplyGitLineChanges(original, modified, { Modification(2, 2, 2, 2) }));
}

TEST(GitLineStaging, ApplyGitLineChangesStagesAnInsertionInTheMiddle)
{
	const GitStagingText original = MakeGitStagingText(L"a\nc\n");
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc\n");

	EXPECT_EQ(L"a\nb\nc\n", ApplyGitLineChanges(original, modified, { Modification(1, 0, 2, 2) }));
}

TEST(GitLineStaging, ApplyGitLineChangesStagesAnInsertionPastTheLastLine)
{
	// Neither text ends in a terminator. The inserted line must contribute the
	// separator between the old last line and itself, and not a second one.
	const GitStagingText original = MakeGitStagingText(L"a\nb");
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc");

	EXPECT_EQ(L"a\nb\nc", ApplyGitLineChanges(original, modified, { Modification(2, 0, 3, 3) }));
}

TEST(GitLineStaging, ApplyGitLineChangesStagesADeletionInTheMiddle)
{
	const GitStagingText original = MakeGitStagingText(L"a\nb\nc\n");
	const GitStagingText modified = MakeGitStagingText(L"a\nc\n");

	EXPECT_EQ(L"a\nc\n", ApplyGitLineChanges(original, modified, { Modification(2, 2, 1, 0) }));
}

TEST(GitLineStaging, ApplyGitLineChangesDropsTheTerminatorOfADeletedLastLine)
{
	// microsoft/vscode#59670. Without the end-of-document adjustment the result
	// would keep a trailing `\n` that the modified text does not have, and the
	// staged blob would differ from the file the user is looking at.
	const GitStagingText original = MakeGitStagingText(L"a\nb\nc");
	const GitStagingText modified = MakeGitStagingText(L"a\nb");

	EXPECT_EQ(L"a\nb", ApplyGitLineChanges(original, modified, { Modification(3, 3, 2, 0) }));
}

TEST(GitLineStaging, ApplyGitLineChangesWithEveryChangeReproducesTheModifiedText)
{
	const std::wstring originalText = L"one\ntwo\nthree\nfour\n";
	const std::wstring modifiedText = L"one\nTWO\nthree\nfour\nfive\n";
	const GitStagingText original = MakeGitStagingText(originalText);
	const GitStagingText modified = MakeGitStagingText(modifiedText);

	const GitLineDiff diff = ComputeGitLineDiff(original.lines, modified.lines);
	ASSERT_FALSE(diff.hitTimeout);

	EXPECT_EQ(modifiedText, ApplyGitLineChanges(original, modified, ToGitLineChanges(diff)));
}

TEST(GitLineStaging, ApplyGitLineChangesUsesTheOriginalTerminatorForKeptLines)
{
	// The two sides can hold different terminators, and each contributes its own.
	// This is why `eol` travels with the lines instead of being guessed at the join.
	const GitStagingText original = MakeGitStagingText(L"a\r\nb\r\nc\r\n");
	const GitStagingText modified = MakeGitStagingText(L"a\nB\nc\n");

	EXPECT_EQ(L"a\r\nB\nc\r\n", ApplyGitLineChanges(original, modified, { Modification(2, 2, 2, 2) }));
}

// ----------------------------------------------------------------------------
// Selections.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, NormalizeGitSelectedLinesSortsAndMergesAdjacentSpans)
{
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc\nd\n");

	const auto merged = NormalizeGitSelectedLines(
		{ GitSelectedLines{ 2, 2 }, GitSelectedLines{ 0, 0 }, GitSelectedLines{ 1, 1 } }, modified);

	ASSERT_EQ(1U, merged.size());
	EXPECT_EQ((GitSelectedLines{ 0, 2 }), merged[0]);
}

TEST(GitLineStaging, NormalizeGitSelectedLinesKeepsSeparatedSpansApart)
{
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc\nd\n");

	const auto merged = NormalizeGitSelectedLines(
		{ GitSelectedLines{ 3, 3 }, GitSelectedLines{ 0, 0 } }, modified);

	ASSERT_EQ(2U, merged.size());
	EXPECT_EQ((GitSelectedLines{ 0, 0 }), merged[0]);
	EXPECT_EQ((GitSelectedLines{ 3, 3 }), merged[1]);
}

TEST(GitLineStaging, NormalizeGitSelectedLinesUnionsOverlappingSpans)
{
	// The documented divergence: upstream's `toLineRanges` reduces two
	// overlapping spans to their *intersection* and silently drops the rest of
	// the selection. Staging fewer lines than the user selected is exactly the
	// operational-safety failure this work exists to remove.
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc\nd\n");

	const auto merged = NormalizeGitSelectedLines(
		{ GitSelectedLines{ 1, 2 }, GitSelectedLines{ 0, 1 } }, modified);

	ASSERT_EQ(1U, merged.size());
	EXPECT_EQ((GitSelectedLines{ 0, 2 }), merged[0]);
}

TEST(GitLineStaging, NormalizeGitSelectedLinesOrientsAndClampsEverySpan)
{
	const GitStagingText modified = MakeGitStagingText(L"a\nb\nc");

	const auto merged = NormalizeGitSelectedLines({ GitSelectedLines{ 9, -4 } }, modified);

	ASSERT_EQ(1U, merged.size());
	EXPECT_EQ((GitSelectedLines{ 0, 2 }), merged[0]);
}

TEST(GitLineStaging, IntersectGitLineChangeNarrowsBothSidesOfAnEqualLengthChange)
{
	const GitStagingText modified = MakeGitStagingText(L"a\nB\nC\nd\n");

	// Two original lines replaced by two modified lines, of which only the first
	// is selected.
	const auto narrowed = IntersectGitLineChange(modified, Modification(2, 3, 2, 3), GitSelectedLines{ 1, 1 });

	ASSERT_TRUE(narrowed.has_value());
	EXPECT_EQ(Modification(2, 2, 2, 2), *narrowed);
}

TEST(GitLineStaging, IntersectGitLineChangeKeepsTheWholeOriginalSideOfALopsidedChange)
{
	// One original line became three. There is no way to say which original lines
	// the selected modified line replaced, so upstream keeps the original side
	// whole rather than guessing.
	const GitStagingText modified = MakeGitStagingText(L"a\np\nq\nr\nb\n");

	const auto narrowed = IntersectGitLineChange(modified, Modification(2, 2, 2, 4), GitSelectedLines{ 2, 2 });

	ASSERT_TRUE(narrowed.has_value());
	EXPECT_EQ(Modification(2, 2, 3, 3), *narrowed);
}

TEST(GitLineStaging, IntersectGitLineChangeReturnsNothingForAnUnrelatedSelection)
{
	const GitStagingText modified = MakeGitStagingText(L"a\nB\nC\nd\n");

	EXPECT_FALSE(IntersectGitLineChange(modified, Modification(2, 3, 2, 3), GitSelectedLines{ 4, 4 }).has_value());
}

TEST(GitLineStaging, IntersectGitLineChangeStagesADeletionWholeFromEitherNeighbour)
{
	// A deletion has no modified text to take a part of, so its range is the seam
	// between the two surviving lines and selecting either one reaches it.
	const GitStagingText modified = MakeGitStagingText(L"a\nd\n");
	const GitLineChange deletion = Modification(2, 3, 1, 0);

	const auto fromAbove = IntersectGitLineChange(modified, deletion, GitSelectedLines{ 0, 0 });
	const auto fromBelow = IntersectGitLineChange(modified, deletion, GitSelectedLines{ 1, 1 });

	ASSERT_TRUE(fromAbove.has_value());
	ASSERT_TRUE(fromBelow.has_value());
	EXPECT_EQ(deletion, *fromAbove);
	EXPECT_EQ(deletion, *fromBelow);
}

TEST(GitLineStaging, SelectGitLineChangesTakesEachChangeAtMostOnce)
{
	// Upstream's `reduce` short-circuits on the first selection that reaches the
	// change, so two selections covering one change do not stage it twice.
	const GitStagingText modified = MakeGitStagingText(L"a\nB\nC\nd\n");
	const std::vector<GitLineChange> changes{ Modification(2, 3, 2, 3) };

	const auto selected = SelectGitLineChanges(
		modified, changes, { GitSelectedLines{ 1, 1 }, GitSelectedLines{ 2, 2 } });

	ASSERT_EQ(1U, selected.size());
	EXPECT_EQ(Modification(2, 2, 2, 2), selected[0]);
}

TEST(GitLineStaging, SelectGitLineChangesDropsEveryUnselectedChange)
{
	const GitStagingText modified = MakeGitStagingText(L"A\nb\nC\n");
	const std::vector<GitLineChange> changes{ Modification(1, 1, 1, 1), Modification(3, 3, 3, 3) };

	const auto selected = SelectGitLineChanges(modified, changes, { GitSelectedLines{ 2, 2 } });

	ASSERT_EQ(1U, selected.size());
	EXPECT_EQ(Modification(3, 3, 3, 3), selected[0]);
}

TEST(GitLineStaging, SelectingOneOfTwoChangesStagesOnlyThatOne)
{
	const std::wstring originalText = L"one\ntwo\nthree\n";
	const GitStagingText original = MakeGitStagingText(originalText);
	const GitStagingText modified = MakeGitStagingText(L"ONE\ntwo\nTHREE\n");
	const auto changes = ToGitLineChanges(ComputeGitLineDiff(original.lines, modified.lines));
	ASSERT_EQ(2U, changes.size());

	// Only the last line is selected, so only it reaches the index.
	const auto selected = SelectGitLineChanges(modified, changes, { GitSelectedLines{ 2, 2 } });

	EXPECT_EQ(L"one\ntwo\nTHREE\n", ApplyGitLineChanges(original, modified, selected));
}

TEST(GitLineStaging, UnstagingSelectedLinesIsTheSameAlgorithmInverted)
{
	// Unstage compares the index against HEAD and applies the inverted change to
	// the index content, which is upstream's `unstageSelectedRanges`.
	const GitStagingText head = MakeGitStagingText(L"one\ntwo\n");
	const GitStagingText index = MakeGitStagingText(L"ONE\nTWO\n");
	const auto changes = ToGitLineChanges(ComputeGitLineDiff(index.lines, head.lines));

	std::vector<GitLineChange> inverted;
	for (const auto& change : changes) inverted.push_back(InvertGitLineChange(change));
	const auto selected = SelectGitLineChanges(head, inverted, { GitSelectedLines{ 0, 0 } });
	std::vector<GitLineChange> toApply;
	for (const auto& change : selected) toApply.push_back(InvertGitLineChange(change));

	EXPECT_EQ(L"one\nTWO\n", ApplyGitLineChanges(index, head, toApply));
}

// ----------------------------------------------------------------------------
// The bytes that reach the index.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, ClassifyGitTextEncodingReportsWhichBranchTheDecoderTook)
{
	EXPECT_EQ(EGitTextEncoding::Utf8, ClassifyGitTextEncoding("plain ascii"));
	EXPECT_EQ(EGitTextEncoding::Utf8, ClassifyGitTextEncoding("\xE3\x81\x82"));
	EXPECT_EQ(EGitTextEncoding::Utf8, ClassifyGitTextEncoding(""));

	// Not valid UTF-8, so `DecodeGitOutput` widened each byte instead.
	EXPECT_EQ(EGitTextEncoding::Latin1Fallback, ClassifyGitTextEncoding("\xFF\xFE"));
}

TEST(GitLineStaging, EncodeGitTextInvertsTheDecoderItWasGiven)
{
	const auto utf8 = EncodeGitText(L"aあb", EGitTextEncoding::Utf8);
	ASSERT_TRUE(utf8.has_value());
	EXPECT_EQ(std::string("a\xE3\x81\x82" "b"), *utf8);

	const auto latin1 = EncodeGitText(L"aÿb", EGitTextEncoding::Latin1Fallback);
	ASSERT_TRUE(latin1.has_value());
	EXPECT_EQ(std::string("a\xFF" "b"), *latin1);

	EXPECT_EQ(std::string(), EncodeGitText(L"", EGitTextEncoding::Utf8).value_or("x"));
	EXPECT_EQ(std::string(), EncodeGitText(L"", EGitTextEncoding::Latin1Fallback).value_or("x"));
}

TEST(GitLineStaging, EncodeGitTextFailsClosedRatherThanSubstituting)
{
	// A staged blob is durable content. A replacement character here would write
	// corruption into the index under the name of a successful stage.
	EXPECT_FALSE(EncodeGitText(L"あ", EGitTextEncoding::Latin1Fallback).has_value());

	const std::wstring unpairedSurrogate(1, static_cast<wchar_t>(0xD800));
	EXPECT_FALSE(EncodeGitText(unpairedSurrogate, EGitTextEncoding::Utf8).has_value());
}

// ----------------------------------------------------------------------------
// The two git invocations `Repository.stage` makes.
// ----------------------------------------------------------------------------

TEST(GitLineStaging, BuildGitHashObjectArgumentsPassesThePathSoFiltersApply)
{
	// `--path` is what makes git apply the `.gitattributes` filters belonging to
	// that path, so this blob matches the one a plain `git add` would write.
	const std::vector<std::wstring> expected{ L"hash-object", L"--stdin", L"-w", L"--path", L"src/a.txt" };

	EXPECT_EQ(expected, BuildGitHashObjectArguments(L"src/a.txt"));
}

TEST(GitLineStaging, BuildGitObjectDetailsArgumentsReadTheModeFromHeadThenTheIndex)
{
	const std::vector<std::wstring> head{ L"ls-tree", L"-l", L"HEAD", L"--", L"src/a.txt" };
	const std::vector<std::wstring> staged{ L"ls-files", L"--stage", L"--", L"src/a.txt" };

	EXPECT_EQ(head, BuildGitHeadObjectDetailsArguments(L"src/a.txt"));
	EXPECT_EQ(staged, BuildGitStagedObjectDetailsArguments(L"src/a.txt"));
}

TEST(GitLineStaging, ParseGitObjectModeReadsBothListingFormats)
{
	EXPECT_EQ(L"100644",
		ParseGitObjectMode(L"100644 blob e69de29bb2d1d6434b8b29ae775ad8c2e48c5391      12\tsrc/a.txt\n"));
	EXPECT_EQ(L"100755",
		ParseGitObjectMode(L"100755 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 0\tsrc/a.txt\n"));
}

TEST(GitLineStaging, ParseGitObjectModeRefusesAnythingThatIsNotAMode)
{
	// No output is upstream's `UnknownPath`: the file is in neither HEAD nor the
	// index, and the entry has to be added at `100644` instead.
	EXPECT_FALSE(ParseGitObjectMode(L"").has_value());
	EXPECT_FALSE(ParseGitObjectMode(L"\n").has_value());
	EXPECT_FALSE(ParseGitObjectMode(L"fatal: not a git repository\n").has_value());
	EXPECT_FALSE(ParseGitObjectMode(L"1006440 blob abc 1\tsrc/a.txt\n").has_value());
	EXPECT_FALSE(ParseGitObjectMode(L"100844 blob abc 1\tsrc/a.txt\n").has_value());
}

TEST(GitLineStaging, BuildGitUpdateIndexArgumentsOmitTheAddFlagInsteadOfEmptyingIt)
{
	// Upstream leaves an empty string in that slot. An empty argument survives
	// this product's quoting as `""`, which git would read as a pathspec.
	const std::vector<std::wstring> updated{
		L"update-index", L"--cacheinfo", L"100644", std::wstring(kEmptyBlobName), L"src/a.txt" };
	const std::vector<std::wstring> added{
		L"update-index", L"--add", L"--cacheinfo", L"100644", std::wstring(kEmptyBlobName), L"src/a.txt" };

	EXPECT_EQ(updated, BuildGitUpdateIndexArguments(L"100644", kEmptyBlobName, L"src/a.txt", false));
	EXPECT_EQ(added, BuildGitUpdateIndexArguments(L"100644", kEmptyBlobName, L"src/a.txt", true));
}

TEST(GitLineStaging, ParseGitHashObjectNameTrimsAndChecksTheShape)
{
	EXPECT_EQ(std::wstring(kEmptyBlobName),
		ParseGitHashObjectName(std::wstring(kEmptyBlobName) + L"\n"));

	// A repository may use SHA-256, which is the repository's choice.
	const std::wstring sha256(64, L'a');
	EXPECT_EQ(sha256, ParseGitHashObjectName(sha256 + L"\r\n"));

	EXPECT_FALSE(ParseGitHashObjectName(L"").has_value());
	EXPECT_FALSE(ParseGitHashObjectName(L"\n").has_value());
	EXPECT_FALSE(ParseGitHashObjectName(L"fatal: unable to hash object\n").has_value());
	EXPECT_FALSE(ParseGitHashObjectName(std::wstring(kEmptyBlobName).substr(1) + L"\n").has_value());
	EXPECT_FALSE(ParseGitHashObjectName(std::wstring(40, L'A')).has_value());
}
