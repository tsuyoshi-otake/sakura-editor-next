/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "workbench/decorations/FileDecorationModel.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/GitScmPublisher.h"

namespace workbench::decorations {
namespace {

FileDecorationEntry Entry(
	std::wstring path, std::wstring badge, EFileDecorationColor color, bool propagate = true)
{
	FileDecorationEntry entry;
	entry.path = std::move(path);
	entry.decoration.badge = std::move(badge);
	entry.decoration.tooltip = L"Modified";
	entry.decoration.color = color;
	entry.decoration.propagate = propagate;
	return entry;
}

FileDecorationTable TableOf(std::vector<FileDecorationEntry> entries)
{
	FileDecorationTable table;
	table.Replace(std::move(entries));
	return table;
}

TEST(FileDecorationTable, ResolvesTheResourceOwnDecoration)
{
	const auto table = TableOf({ Entry(LR"(C:\repo\a.txt)", L"M", EFileDecorationColor::GitModified) });

	const auto resolved = table.Resolve(LR"(C:\repo\a.txt)", false);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(EFileDecorationColor::GitModified, resolved->color);
	EXPECT_EQ(L"M", resolved->badge);
	EXPECT_EQ(L"Modified", resolved->tooltip);
	EXPECT_FALSE(resolved->containsChildren);
}

TEST(FileDecorationTable, MatchesRegardlessOfCaseOrSeparator)
{
	const auto table = TableOf({ Entry(LR"(C:/Repo/Sub/A.txt)", L"M", EFileDecorationColor::GitModified) });

	EXPECT_TRUE(table.Resolve(LR"(c:\repo\sub\a.txt)", false).has_value());
	EXPECT_TRUE(table.Resolve(LR"(C:\repo\sub\a.txt\)", false).has_value());
}

TEST(FileDecorationTable, UndecoratedResourceResolvesToNothing)
{
	const auto table = TableOf({ Entry(LR"(C:\repo\a.txt)", L"M", EFileDecorationColor::GitModified) });

	EXPECT_FALSE(table.Resolve(LR"(C:\repo\b.txt)", false).has_value());
	EXPECT_FALSE(table.Resolve(L"", true).has_value());
}

TEST(FileDecorationTable, AncestorTakesTheBubbleFromAPropagatingDescendant)
{
	const auto table = TableOf({ Entry(LR"(C:\repo\sub\a.txt)", L"U", EFileDecorationColor::GitUntracked) });

	// includeChildren is what a folder row asks for; a file row never does.
	EXPECT_FALSE(table.Resolve(LR"(C:\repo\sub)", false).has_value());

	const auto resolved = table.Resolve(LR"(C:\repo\sub)", true);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(EFileDecorationColor::GitUntracked, resolved->color);
	EXPECT_TRUE(resolved->badge.empty());
	EXPECT_TRUE(resolved->containsChildren);
	EXPECT_EQ(kContainsChildrenTooltip, resolved->tooltip);
}

TEST(FileDecorationTable, SiblingPrefixIsNotADescendant)
{
	const auto table = TableOf({ Entry(LR"(C:\repobar\a.txt)", L"M", EFileDecorationColor::GitModified) });

	EXPECT_FALSE(table.Resolve(LR"(C:\repo)", true).has_value());
}

TEST(FileDecorationTable, ANonPropagatingDescendantLeavesTheAncestorUndecorated)
{
	const auto table = TableOf(
		{ Entry(LR"(C:\repo\sub\a.txt)", L"D", EFileDecorationColor::GitDeleted, false) });

	EXPECT_FALSE(table.Resolve(LR"(C:\repo\sub)", true).has_value());
	EXPECT_TRUE(table.Resolve(LR"(C:\repo\sub\a.txt)", false).has_value());
}

TEST(FileDecorationTable, OwnDecorationWinsTheColorOverADescendant)
{
	const auto table = TableOf({
		Entry(LR"(C:\repo\sub)", L"M", EFileDecorationColor::GitModified),
		Entry(LR"(C:\repo\sub\a.txt)", L"U", EFileDecorationColor::GitUntracked),
	});

	const auto resolved = table.Resolve(LR"(C:\repo\sub)", true);
	ASSERT_TRUE(resolved.has_value());
	// Upstream reduces right over a list headed by the resource's own data, so
	// the resource keeps its own color while the bubble replaces its letter.
	EXPECT_EQ(EFileDecorationColor::GitModified, resolved->color);
	EXPECT_TRUE(resolved->containsChildren);
	EXPECT_TRUE(resolved->badge.empty());
}

TEST(FileDecorationTable, ReplaceDropsEmptyPathsAndDuplicates)
{
	const auto table = TableOf({
		Entry(L"", L"M", EFileDecorationColor::GitModified),
		Entry(LR"(C:\repo\a.txt)", L"M", EFileDecorationColor::GitModified),
		Entry(LR"(C:\REPO\A.TXT)", L"U", EFileDecorationColor::GitUntracked),
	});

	EXPECT_EQ(1u, table.Size());
	const auto resolved = table.Resolve(LR"(C:\repo\a.txt)", false);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(EFileDecorationColor::GitModified, resolved->color);
}

TEST(FileDecorationTable, ClearRemovesEverything)
{
	auto table = TableOf({ Entry(LR"(C:\repo\a.txt)", L"M", EFileDecorationColor::GitModified) });
	table.Clear();

	EXPECT_TRUE(table.Empty());
	EXPECT_FALSE(table.Resolve(LR"(C:\repo\a.txt)", false).has_value());
}

} // namespace
} // namespace workbench::decorations

namespace workbench::scm {
namespace {

TEST(GitFileStatusDecoration, ColorsMirrorUpstreamGetStatusColor)
{
	using decorations::EFileDecorationColor;

	EXPECT_EQ(EFileDecorationColor::GitStageModified,
		GitFileStatusDecorationColor(EGitFileStatus::IndexModified));
	EXPECT_EQ(EFileDecorationColor::GitModified, GitFileStatusDecorationColor(EGitFileStatus::Modified));
	EXPECT_EQ(EFileDecorationColor::GitModified, GitFileStatusDecorationColor(EGitFileStatus::TypeChanged));
	EXPECT_EQ(EFileDecorationColor::GitStageDeleted,
		GitFileStatusDecorationColor(EGitFileStatus::IndexDeleted));
	EXPECT_EQ(EFileDecorationColor::GitDeleted, GitFileStatusDecorationColor(EGitFileStatus::Deleted));
	EXPECT_EQ(EFileDecorationColor::GitAdded, GitFileStatusDecorationColor(EGitFileStatus::IndexAdded));
	EXPECT_EQ(EFileDecorationColor::GitAdded, GitFileStatusDecorationColor(EGitFileStatus::IntentToAdd));
	EXPECT_EQ(EFileDecorationColor::GitRenamed, GitFileStatusDecorationColor(EGitFileStatus::IndexRenamed));
	EXPECT_EQ(EFileDecorationColor::GitRenamed, GitFileStatusDecorationColor(EGitFileStatus::IndexCopied));
	EXPECT_EQ(EFileDecorationColor::GitRenamed, GitFileStatusDecorationColor(EGitFileStatus::IntentToRename));
	EXPECT_EQ(EFileDecorationColor::GitUntracked, GitFileStatusDecorationColor(EGitFileStatus::Untracked));
	// Every conflict state falls to upstream's default branch.
	EXPECT_EQ(EFileDecorationColor::GitConflicting, GitFileStatusDecorationColor(EGitFileStatus::BothModified));
}

TEST(GitFileStatusDecoration, OnlyDeletionsStopPropagating)
{
	EXPECT_FALSE(DoesGitFileStatusPropagate(EGitFileStatus::Deleted));
	EXPECT_FALSE(DoesGitFileStatusPropagate(EGitFileStatus::IndexDeleted));
	EXPECT_TRUE(DoesGitFileStatusPropagate(EGitFileStatus::Modified));
	EXPECT_TRUE(DoesGitFileStatusPropagate(EGitFileStatus::Untracked));
	EXPECT_TRUE(DoesGitFileStatusPropagate(EGitFileStatus::BothModified));
}

TEST(BuildGitFileDecorationEntries, ProjectsFileUrisOntoNativePaths)
{
	std::vector<GitResourceDecoration> decorations;
	decorations.push_back({ L"file:///C:/repo/a.txt", L'M', EGitFileStatus::Modified });
	decorations.push_back({ L"file:///C:/repo/gone.txt", L'D', EGitFileStatus::Deleted });
	decorations.push_back({ L"not a uri", L'M', EGitFileStatus::Modified });

	const auto entries = BuildGitFileDecorationEntries(decorations);
	ASSERT_EQ(2u, entries.size());

	EXPECT_EQ(LR"(C:\repo\a.txt)", entries[0].path);
	EXPECT_EQ(L"M", entries[0].decoration.badge);
	EXPECT_EQ(decorations::EFileDecorationColor::GitModified, entries[0].decoration.color);
	EXPECT_TRUE(entries[0].decoration.propagate);
	EXPECT_EQ(L"Modified", entries[0].decoration.tooltip);

	EXPECT_EQ(LR"(C:\repo\gone.txt)", entries[1].path);
	EXPECT_EQ(decorations::EFileDecorationColor::GitDeleted, entries[1].decoration.color);
	EXPECT_FALSE(entries[1].decoration.propagate);
}

} // namespace
} // namespace workbench::scm
