/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "workbench/commands/ApiCommandArguments.h"
#include "workbench/scm/GitBranchCommands.h"
#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitCommitCommands.h"
#include "workbench/scm/GitDiffModel.h"
#include "workbench/scm/GitHistoryModel.h"
#include "workbench/scm/GitRefModel.h"
#include "workbench/scm/GitScmMenus.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/GitScmPublisher.h"
#include "workbench/scm/GitStageCommands.h"
#include "workbench/scm/ScmViewStackLayout.h"
#include "workbench/scm/SourceControlService.h"

using namespace std::string_literals;

namespace workbench::scm {
namespace {

ScmOwner Owner()
{
	return { std::string(kGitProviderId), 1 };
}

GitChange Change(const wchar_t* path, const wchar_t index, const wchar_t worktree)
{
	GitChange change;
	change.path = path;
	change.indexStatus = index;
	change.worktreeStatus = worktree;
	return change;
}

GitChange Untracked(const wchar_t* path)
{
	GitChange change;
	change.path = path;
	change.untracked = true;
	change.status = L'?';
	return change;
}

GitChange Conflicted(const wchar_t* path, const wchar_t index, const wchar_t worktree)
{
	auto change = Change(path, index, worktree);
	change.conflicted = true;
	return change;
}

const ScmResourceGroupState* FindGroup(const ScmProviderState& provider, std::string_view id)
{
	const auto found = std::find_if(provider.groups.begin(), provider.groups.end(),
		[id](const ScmResourceGroupState& group) { return group.id == id; });
	return found == provider.groups.end() ? nullptr : &*found;
}

GitScmState RepositoryState()
{
	GitScmState state;
	state.repository = true;
	state.branch = L"master";
	state.upstream = L"origin/master";
	return state;
}

//!
//! @brief The comparison a published row's command actually names.
//!
//! The two sides are read back out of the URIs the command carries instead of
//! being compared as strings, so the assertion is about which two texts the
//! click compares and not about how `toGitUri` happens to spell them.
//!
void ExpectPublishedDiff(const ScmResourceState& resource, std::wstring_view root,
	const GitDiffEndpoint& original, const GitDiffEndpoint& modified, std::wstring_view title)
{
	ASSERT_TRUE(resource.command.has_value());
	// Upstream's `resolveChangeCommand` titles both branches `localize('open',
	// "Open")`, so a diff and a plain open are named the same thing.
	EXPECT_EQ("Open", resource.command->title);
	EXPECT_EQ("vscode.diff", resource.command->command);
	const auto arguments = commands::ParseApiDiffArguments(resource.command->argumentsJson);
	ASSERT_TRUE(arguments.has_value());
	EXPECT_EQ(title, arguments->title);
	EXPECT_EQ(original, ResolveGitDiffEndpointUri(arguments->originalUri, root));
	EXPECT_EQ(modified, ResolveGitDiffEndpointUri(arguments->modifiedUri, root));
}

void ExpectPublishedOpen(const ScmResourceState& resource, std::wstring_view root,
	const GitDiffEndpoint& modified, std::optional<bool> overrideEditor, std::wstring_view label)
{
	ASSERT_TRUE(resource.command.has_value());
	EXPECT_EQ("Open", resource.command->title);
	EXPECT_EQ("vscode.open", resource.command->command);
	const auto arguments = commands::ParseApiOpenArguments(resource.command->argumentsJson);
	ASSERT_TRUE(arguments.has_value());
	EXPECT_EQ(label, arguments->label);
	EXPECT_EQ(overrideEditor, arguments->overrideEditor);
	EXPECT_EQ(modified, ResolveGitDiffEndpointUri(arguments->resourceUri, root));
}

GitDiffEndpoint InRepository(std::wstring_view ref, std::wstring_view path)
{
	return { EGitDiffSource::Repository, std::wstring(ref), std::wstring(path) };
}

GitDiffEndpoint OnDisk(std::wstring_view path)
{
	return { EGitDiffSource::WorkingTree, {}, std::wstring(path) };
}

} // namespace

TEST(GitCommandRunner, QuotesOnlyArgumentsThatNeedIt)
{
	// A quoted argument that did not need quoting would still parse, but the
	// unquoted form is what a developer sees in a diagnostic command line.
	EXPECT_EQ(L"status", QuoteGitArgument(L"status"));
	EXPECT_EQ(L"--porcelain=v2", QuoteGitArgument(L"--porcelain=v2"));
	EXPECT_EQ(L"\"\"", QuoteGitArgument(L""));
	EXPECT_EQ(L"\"my branch\"", QuoteGitArgument(L"my branch"));
	EXPECT_EQ(L"\"a\\\"b\"", QuoteGitArgument(L"a\"b"));
	// CommandLineToArgvW only treats a backslash run as escapes when a quote
	// follows it, so a trailing run must be doubled or it would escape the
	// closing quote and swallow the next argument.
	EXPECT_EQ(L"\"C:\\dir with space\\\\\"", QuoteGitArgument(L"C:\\dir with space\\"));
}

TEST(ScmViewStackLayout, ReservesANonInteractiveGraphFrameBelowChanges)
{
	const ScmGraphPresentation graph;
	EXPECT_EQ(EScmGraphPresentationStatus::Unavailable, graph.status);
	EXPECT_FALSE(graph.IsInteractive());
	EXPECT_FALSE(graph.ShouldRenderFrameForProvider(false));
	EXPECT_TRUE(graph.ShouldRenderFrameForProvider(true));

	const auto layout = BuildScmViewStackLayout({
		.clientTop = 0,
		.clientBottom = 500,
		.viewHeaderHeight = 30,
		.repositoryRowHeight = 22,
		.inputOuterMargin = 5,
		.inputHeight = 26,
		.graphBodyHeight = 48,
		.repositoriesVisible = true,
		.changesHeaderVisible = true,
		.inputVisible = true,
		.graphVisible = true,
	});
	EXPECT_EQ((ScmVerticalBounds{ 0, 30 }), layout.repositoriesHeader);
	EXPECT_EQ((ScmVerticalBounds{ 30, 52 }), layout.repositoryRow);
	EXPECT_EQ((ScmVerticalBounds{ 52, 82 }), layout.changesHeader);
	EXPECT_EQ((ScmVerticalBounds{ 87, 113 }), layout.input);
	EXPECT_EQ((ScmVerticalBounds{ 118, 422 }), layout.changesBody);
	EXPECT_EQ((ScmVerticalBounds{ 422, 452 }), layout.graphHeader);
	EXPECT_EQ((ScmVerticalBounds{ 452, 500 }), layout.graphBody);

	const auto empty = BuildScmViewStackLayout({
		.clientTop = 0,
		.clientBottom = 250,
		.viewHeaderHeight = 30,
		.repositoryRowHeight = 22,
		.inputOuterMargin = 5,
		.inputHeight = 26,
		.graphBodyHeight = 48,
		.repositoriesVisible = false,
		.changesHeaderVisible = false,
		.inputVisible = false,
		.graphVisible = false,
	});
	EXPECT_TRUE(empty.repositoriesHeader.Empty());
	EXPECT_TRUE(empty.repositoryRow.Empty());
	EXPECT_TRUE(empty.changesHeader.Empty());
	EXPECT_EQ((ScmVerticalBounds{ 0, 250 }), empty.changesBody);
	EXPECT_TRUE(empty.graphHeader.Empty());
	EXPECT_TRUE(empty.graphBody.Empty());
}

TEST(ScmViewStackLayout, ReservesTheCommitActionButtonUnderTheInput)
{
	// The input's own trailing margin is the button's top margin, and the button
	// carries one more below it, so the list starts a full margin lower than it
	// does with no button.
	const auto layout = BuildScmViewStackLayout({
		.clientTop = 0,
		.clientBottom = 500,
		.viewHeaderHeight = 30,
		.repositoryRowHeight = 22,
		.inputOuterMargin = 5,
		.inputHeight = 26,
		.actionButtonHeight = 26,
		.graphBodyHeight = 48,
		.repositoriesVisible = true,
		.changesHeaderVisible = true,
		.inputVisible = true,
		.actionButtonVisible = true,
		.graphVisible = true,
	});
	EXPECT_EQ((ScmVerticalBounds{ 87, 113 }), layout.input);
	EXPECT_EQ((ScmVerticalBounds{ 118, 144 }), layout.actionButton);
	EXPECT_EQ((ScmVerticalBounds{ 149, 422 }), layout.changesBody);

	// No button is no band: the list keeps the position it had before the button
	// existed, so a repository with nothing to commit loses no room.
	const auto without = BuildScmViewStackLayout({
		.clientTop = 0,
		.clientBottom = 500,
		.viewHeaderHeight = 30,
		.repositoryRowHeight = 22,
		.inputOuterMargin = 5,
		.inputHeight = 26,
		.actionButtonHeight = 26,
		.graphBodyHeight = 48,
		.repositoriesVisible = true,
		.changesHeaderVisible = true,
		.inputVisible = true,
		.actionButtonVisible = false,
		.graphVisible = true,
	});
	EXPECT_TRUE(without.actionButton.Empty());
	EXPECT_EQ((ScmVerticalBounds{ 118, 422 }), without.changesBody);
}

TEST(GitScmMenus, ContributesTheCommitActionButtonOnlyWhileThereAreChanges)
{
	// Upstream contributes no action button for a repository with nothing to
	// commit; it does not contribute a disabled one.
	EXPECT_FALSE(BuildGitCommitActionButton(false, true).has_value());

	const auto button = BuildGitCommitActionButton(true, true);
	ASSERT_TRUE(button.has_value());
	EXPECT_EQ(L"$(check) Commit", button->title);
	EXPECT_EQ("git.commit", button->commandId);
	EXPECT_TRUE(button->enabled);
	ASSERT_EQ(5u, button->secondaryCommands.size());
	EXPECT_EQ("git.commit", button->secondaryCommands[0].commandId);
	EXPECT_EQ(L"Commit", button->secondaryCommands[0].title);
	EXPECT_EQ("git.commitAmend", button->secondaryCommands[1].commandId);
	EXPECT_EQ(L"Commit (Amend)", button->secondaryCommands[1].title);
	EXPECT_TRUE(button->secondaryCommands[2].separator);
	EXPECT_EQ("git.commit", button->secondaryCommands[3].commandId);
	EXPECT_EQ(L"Commit & Push", button->secondaryCommands[3].title);
	EXPECT_EQ(R"(["git.push"])", button->secondaryCommands[3].argumentsJson);
	EXPECT_EQ("git.commit", button->secondaryCommands[4].commandId);
	EXPECT_EQ(L"Commit & Sync", button->secondaryCommands[4].title);
	EXPECT_EQ(R"(["git.sync"])", button->secondaryCommands[4].argumentsJson);

	// A disabled input box disables the button, exactly as a running repository
	// operation disables both upstream.
	const auto disabled = BuildGitCommitActionButton(true, false);
	ASSERT_TRUE(disabled.has_value());
	EXPECT_FALSE(disabled->enabled);
}

TEST(GitCommitCommands, ParsesTheActionButtonsPostCommitPayload)
{
	const auto empty = ParseGitCommitPostCommandArguments("");
	ASSERT_TRUE(empty.has_value());
	EXPECT_EQ(EGitPostCommitCommand::None, *empty);

	const auto noAction = ParseGitCommitPostCommandArguments("[ ]\t");
	ASSERT_TRUE(noAction.has_value());
	EXPECT_EQ(EGitPostCommitCommand::None, *noAction);

	const auto push = ParseGitCommitPostCommandArguments(R"(["git.push"])");
	ASSERT_TRUE(push.has_value());
	EXPECT_EQ(EGitPostCommitCommand::Push, *push);

	const auto sync = ParseGitCommitPostCommandArguments(R"([ "git.sync" ])");
	ASSERT_TRUE(sync.has_value());
	EXPECT_EQ(EGitPostCommitCommand::Sync, *sync);
}

TEST(GitCommitCommands, RejectsUnsupportedPostCommitPayloads)
{
	for (const auto payload : {
		std::string("null"), std::string("[\"git.fetch\"]"),
		std::string("[\"git.push\", \"git.sync\"]"), std::string("[true]"),
		std::string("[\"git.push\"] trailing") }) {
		EXPECT_FALSE(ParseGitCommitPostCommandArguments(payload).has_value()) << payload;
	}
}

TEST(GitCommandRunner, BuildsCommandLineAndPrependsRepositoryDirectory)
{
	EXPECT_EQ(L"\"C:\\Program Files\\Git\\git.exe\" status --branch",
		BuildGitCommandLine(L"C:\\Program Files\\Git\\git.exe", { L"status", L"--branch" }));

	GitExecutionRequest request;
	request.workingDirectory = L"C:\\repo";
	request.arguments = { L"status" };
	const auto effective = BuildEffectiveGitArguments(request);
	ASSERT_EQ(3U, effective.size());
	EXPECT_EQ(L"-C", effective[0]);
	EXPECT_EQ(L"C:\\repo", effective[1]);
	EXPECT_EQ(L"status", effective[2]);
}

TEST(GitCommandRunner, RejectsRequestsThatCannotBeExecuted)
{
	GitExecutionRequest valid;
	valid.workingDirectory = L"C:\\repo";
	valid.arguments = { L"status" };
	EXPECT_TRUE(IsExecutableGitRequest(valid));

	auto noDirectory = valid;
	noDirectory.workingDirectory.clear();
	EXPECT_FALSE(IsExecutableGitRequest(noDirectory));

	auto noArguments = valid;
	noArguments.arguments.clear();
	EXPECT_FALSE(IsExecutableGitRequest(noArguments));

	auto tooManyArguments = valid;
	tooManyArguments.arguments.assign(kMaximumGitArguments + 1, L"x");
	EXPECT_FALSE(IsExecutableGitRequest(tooManyArguments));

	auto longArgument = valid;
	longArgument.arguments = { std::wstring(kMaximumGitArgumentLength + 1, L'x') };
	EXPECT_FALSE(IsExecutableGitRequest(longArgument));

	auto noTimeout = valid;
	noTimeout.timeoutMilliseconds = 0;
	EXPECT_FALSE(IsExecutableGitRequest(noTimeout));

	auto noOutputBudget = valid;
	noOutputBudget.maximumOutputBytes = 0;
	EXPECT_FALSE(IsExecutableGitRequest(noOutputBudget));

	// A rejected request must be rejected by RunGit too, and with the terminal
	// state that names the cause instead of an empty successful result.
	const auto result = RunGit(noDirectory, nullptr);
	EXPECT_EQ(EGitExecutionStatus::InvalidRequest, result.status);
	EXPECT_FALSE(result.Succeeded());
}

TEST(GitScmModel, ParsesBranchAheadBehindAndChanges)
{
	const std::string status = "# branch.head feature/test\0# branch.upstream origin/feature/test\0"
		"# branch.ab +2 -3\0? new.txt\0"
		"1 M. N... 100644 100644 100644 abcdef abcdef src/file.cpp\0"s;
	const auto state = ParsePorcelainV2(status);
	EXPECT_TRUE(state.repository);
	EXPECT_EQ(L"feature/test", state.branch);
	EXPECT_EQ(2, state.ahead);
	EXPECT_EQ(3, state.behind);
	ASSERT_EQ(2U, state.changes.size());
	EXPECT_EQ(L"new.txt", state.changes[0].path);
	EXPECT_EQ(L"src/file.cpp", state.changes[1].path);
	EXPECT_EQ(L"origin/feature/test", state.upstream);
}

TEST(GitScmPublisher, ClassifiesTheTwoGitAreasSeparately)
{
	// Staged and unstaged edits of one path are two different comparisons, so
	// the same file legitimately appears in both groups.
	const auto both = ClassifyChange(Change(L"a.txt", L'M', L'M'), EUntrackedChangesPolicy::Mixed);
	EXPECT_TRUE(both.index);
	EXPECT_TRUE(both.workingTree);
	EXPECT_FALSE(both.merge);

	const auto stagedOnly = ClassifyChange(Change(L"a.txt", L'A', L'.'), EUntrackedChangesPolicy::Mixed);
	EXPECT_TRUE(stagedOnly.index);
	EXPECT_FALSE(stagedOnly.workingTree);

	const auto worktreeOnly = ClassifyChange(Change(L"a.txt", L'.', L'M'), EUntrackedChangesPolicy::Mixed);
	EXPECT_FALSE(worktreeOnly.index);
	EXPECT_TRUE(worktreeOnly.workingTree);

	// A conflict belongs to Merge Changes alone; listing it under Changes too
	// would offer a stage action for a file that must be resolved first.
	const auto conflict = ClassifyChange(Conflicted(L"a.txt", L'U', L'U'), EUntrackedChangesPolicy::Mixed);
	EXPECT_TRUE(conflict.merge);
	EXPECT_FALSE(conflict.index);
	EXPECT_FALSE(conflict.workingTree);
}

TEST(GitScmPublisher, HonorsTheUntrackedChangesPolicy)
{
	const auto change = Untracked(L"new.txt");
	EXPECT_TRUE(ClassifyChange(change, EUntrackedChangesPolicy::Mixed).workingTree);
	EXPECT_FALSE(ClassifyChange(change, EUntrackedChangesPolicy::Mixed).untracked);
	EXPECT_TRUE(ClassifyChange(change, EUntrackedChangesPolicy::Separate).untracked);
	EXPECT_FALSE(ClassifyChange(change, EUntrackedChangesPolicy::Separate).workingTree);
	EXPECT_FALSE(ClassifyChange(change, EUntrackedChangesPolicy::Hidden).Any());
}

TEST(GitScmPublisher, EachGroupsRowCarriesItsOwnAreasStatus)
{
	const auto deletedInIndex = Change(L"gone.txt", L'D', L'.');
	const auto indexStatus = GitGroupStatus(deletedInIndex, EGitResourceGroup::Index);
	ASSERT_TRUE(indexStatus.has_value());
	EXPECT_EQ("Index Deleted", GitFileStatusText(*indexStatus));
	EXPECT_EQ(L'D', GitFileStatusLetter(*indexStatus));
	EXPECT_TRUE(IsGitFileStatusStruckThrough(*indexStatus));

	const auto modified = Change(L"a.txt", L'.', L'M');
	const auto worktreeStatus = GitGroupStatus(modified, EGitResourceGroup::WorkingTree);
	ASSERT_TRUE(worktreeStatus.has_value());
	EXPECT_EQ("Modified", GitFileStatusText(*worktreeStatus));
	EXPECT_FALSE(IsGitFileStatusStruckThrough(*worktreeStatus));

	const auto bothAdded = Conflicted(L"a.txt", L'A', L'A');
	const auto mergeStatus = GitGroupStatus(bothAdded, EGitResourceGroup::Merge);
	ASSERT_TRUE(mergeStatus.has_value());
	EXPECT_EQ("Conflict: Both Added", GitFileStatusText(*mergeStatus));
	EXPECT_EQ(L'!', GitFileStatusLetter(*mergeStatus));

	const auto untracked = Untracked(L"new.txt");
	const auto untrackedStatus = GitGroupStatus(untracked, EGitResourceGroup::Untracked);
	ASSERT_TRUE(untrackedStatus.has_value());
	EXPECT_EQ("Untracked", GitFileStatusText(*untrackedStatus));
	EXPECT_EQ(L'U', GitFileStatusLetter(*untrackedStatus));

	// The same change read through the two areas is two different statuses, which
	// is the whole reason a row may not be shared between the two groups.
	const auto addedThenEdited = Change(L"a.txt", L'A', L'M');
	EXPECT_EQ(EGitFileStatus::IndexAdded, GitGroupStatus(addedThenEdited, EGitResourceGroup::Index));
	EXPECT_EQ(EGitFileStatus::Modified, GitGroupStatus(addedThenEdited, EGitResourceGroup::WorkingTree));

	// A group whose area publishes no row is refused rather than given the other
	// area's status: an index `T` is a real porcelain code upstream lists nowhere.
	EXPECT_FALSE(GitGroupStatus(Change(L"a.txt", L'T', L'M'), EGitResourceGroup::Index).has_value());
	EXPECT_FALSE(GitGroupStatus(Change(L"a.txt", L'M', L'C'), EGitResourceGroup::WorkingTree).has_value());
}

TEST(GitScmPublisher, OneFileKeepsOneBadgeWithUpstreamsGroupPrecedence)
{
	// Upstream's decoration map is keyed by URI and collected index-then-
	// workingTree, so the later group wins: a staged add that was edited again
	// afterwards shows `M`, not `A`.
	const auto addedThenEdited = Change(L"a.txt", L'A', L'M');
	const auto bothAreas = ClassifyChange(addedThenEdited, EUntrackedChangesPolicy::Mixed);
	ASSERT_TRUE(bothAreas.index);
	ASSERT_TRUE(bothAreas.workingTree);
	EXPECT_EQ(EGitFileStatus::Modified, GitDecorationStatus(addedThenEdited, bothAreas));

	// Staged only keeps the index letter, which is what makes the rule above a
	// precedence rule and not a blanket preference for the worktree column.
	const auto stagedAdd = Change(L"b.txt", L'A', L'.');
	EXPECT_EQ(EGitFileStatus::IndexAdded,
		GitDecorationStatus(stagedAdd, ClassifyChange(stagedAdd, EUntrackedChangesPolicy::Mixed)));

	// The worktree column wins only when it actually published a row. A `C`
	// there publishes none, so the badge falls back to the index row, which is
	// the only row this file has.
	const auto copiedInWorktree = Change(L"c.txt", L'M', L'C');
	EXPECT_EQ(EGitFileStatus::IndexModified,
		GitDecorationStatus(copiedInWorktree, ClassifyChange(copiedInWorktree, EUntrackedChangesPolicy::Mixed)));

	// Neither area publishes a row, so the file has no badge at all rather than
	// one for a row that is not on screen.
	const auto nothingPublishable = Change(L"d.txt", L'T', L'C');
	EXPECT_FALSE(GitDecorationStatus(nothingPublishable,
		ClassifyChange(nothingPublishable, EUntrackedChangesPolicy::Mixed)).has_value());
}

TEST(GitScmPublisher, StagedAndUnstagedRowsOfOnePathDescribeDifferentComparisons)
{
	auto state = RepositoryState();
	state.changes = { Change(L"a.txt", L'A', L'M') };

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	const auto* index = FindGroup(publication.provider, "index");
	const auto* workingTree = FindGroup(publication.provider, "workingTree");
	ASSERT_NE(nullptr, index);
	ASSERT_NE(nullptr, workingTree);
	ASSERT_EQ(1U, index->resources.size());
	ASSERT_EQ(1U, workingTree->resources.size());

	// Same file, same URI, two rows — and the two rows must not claim the same
	// thing. Staged Changes compares HEAD with the index, Changes compares the
	// index with the worktree, so a shared row would describe one of them wrongly.
	EXPECT_EQ(index->resources[0].resourceUri.ToString(), workingTree->resources[0].resourceUri.ToString());
	EXPECT_EQ("Index Added", index->resources[0].tooltip);
	EXPECT_EQ("Modified", workingTree->resources[0].tooltip);
}

TEST(GitScmPublisher, SuppliedTextResolverLocalizesPublishedGroupsAndDiffTitles)
{
	auto state = RepositoryState();
	state.changes = { Change(L"src/a.txt", L'M', L'.') };
	const GitDiffTextResolver text = [](std::string_view key, std::wstring_view argument) -> std::wstring {
		if (key == "GitScmStagedChanges") return L"Localized staged";
		if (key == "GitDiffIndex") return std::wstring(argument) + L" [localized index]";
		return std::wstring{};
	};

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state,
		EUntrackedChangesPolicy::Mixed, text);
	const auto* index = FindGroup(publication.provider, "index");
	ASSERT_NE(nullptr, index);
	EXPECT_EQ("Localized staged", index->label);
	ASSERT_EQ(1U, index->resources.size());
	const auto diff = ResolveGitDiffInput(MakeGitDiffRow(state.changes[0], EGitFileStatus::IndexModified, true), text);
	EXPECT_EQ(L"a.txt [localized index]", diff.title);
}

TEST(GitScmPublisher, EachRowsCommandOpensTheComparisonThatRowIsAbout)
{
	auto state = RepositoryState();
	// One path staged and edited again, and one path edited only. They are the
	// two halves of upstream's `sanitizeRef('~')` rule, which reads whether the
	// *path* has a Staged Changes row rather than which row was clicked.
	state.changes = { Change(L"src/a.txt", L'M', L'M'), Change(L"src/b.txt", L'.', L'M') };

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	const auto* index = FindGroup(publication.provider, "index");
	const auto* workingTree = FindGroup(publication.provider, "workingTree");
	ASSERT_NE(nullptr, index);
	ASSERT_NE(nullptr, workingTree);
	ASSERT_EQ(1U, index->resources.size());
	ASSERT_EQ(2U, workingTree->resources.size());

	// Staged Changes compares HEAD with the index, under the row's own name.
	ExpectPublishedDiff(index->resources[0], LR"(C:\repo)",
		InRepository(L"HEAD", L"src/a.txt"), InRepository({}, L"src/a.txt"), L"a.txt (Index)");

	// The unstaged row of that same path compares the **index** with the file on
	// disk. Comparing against HEAD here would silently fold the staged edit into
	// the diff and show the user changes they have already staged.
	ExpectPublishedDiff(workingTree->resources[0], LR"(C:\repo)",
		InRepository({}, L"src/a.txt"), OnDisk(L"src/a.txt"), L"a.txt (Working Tree)");

	// Nothing is staged for the second path, so the same row shape compares
	// against HEAD instead. One published fact, two different comparisons.
	ExpectPublishedDiff(workingTree->resources[1], LR"(C:\repo)",
		InRepository(L"HEAD", L"src/b.txt"), OnDisk(L"src/b.txt"), L"b.txt (Working Tree)");
}

TEST(GitScmPublisher, ARowWithOneSideOpensItAndCarriesUpstreamsOverride)
{
	auto state = RepositoryState();
	state.changes = { Untracked(L"src/new.txt"), Conflicted(L"src/c.txt", L'U', L'U') };

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	const auto* workingTree = FindGroup(publication.provider, "workingTree");
	const auto* merge = FindGroup(publication.provider, "merge");
	ASSERT_NE(nullptr, workingTree);
	ASSERT_NE(nullptr, merge);
	ASSERT_EQ(1U, workingTree->resources.size());
	ASSERT_EQ(1U, merge->resources.size());

	// An untracked file has nothing to compare against, so upstream opens it and
	// leaves `override` undefined.
	ExpectPublishedOpen(workingTree->resources[0], LR"(C:\repo)",
		OnDisk(L"src/new.txt"), std::nullopt, L"new.txt (Untracked)");

	// A both-modified conflict opens the working-tree file too, but upstream
	// passes `override: false` there. Absent and `false` are different requests,
	// so the distinction survives the publication rather than being flattened.
	ExpectPublishedOpen(merge->resources[0], LR"(C:\repo)",
		OnDisk(L"src/c.txt"), false, L"c.txt (Working Tree)");
}

TEST(GitScmPublisher, ARowWhoseComparisonHasNeitherSidePublishesNoCommand)
{
	auto state = RepositoryState();
	// Both sides deleted the file, so neither of upstream's resolvers names a
	// text to show. It still publishes a row — the conflict has to be visible
	// and resolvable — but the row's click has nothing to open.
	state.changes = { Conflicted(L"src/gone.txt", L'D', L'D') };

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	const auto* merge = FindGroup(publication.provider, "merge");
	ASSERT_NE(nullptr, merge);
	ASSERT_EQ(1U, merge->resources.size());
	EXPECT_EQ("Conflict: Both Deleted", merge->resources[0].tooltip);
	// No command at all, rather than one that would resolve to an empty editor.
	EXPECT_FALSE(merge->resources[0].command.has_value());
}

TEST(GitScmPublisher, ACodeUpstreamListsNowherePublishesNoRow)
{
	auto state = RepositoryState();
	// An index `T` and a working-tree `C` are both real porcelain codes that
	// upstream's switches list nowhere, so each publishes no row at all.
	state.changes = { Change(L"typechange.txt", L'T', L'T'), Change(L"copied.txt", L'M', L'C') };

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	const auto* index = FindGroup(publication.provider, "index");
	const auto* workingTree = FindGroup(publication.provider, "workingTree");
	ASSERT_NE(nullptr, index);
	ASSERT_NE(nullptr, workingTree);
	ASSERT_EQ(1U, index->resources.size());
	ASSERT_EQ(1U, workingTree->resources.size());
	EXPECT_EQ("Index Modified", index->resources[0].tooltip);
	EXPECT_EQ("Type Changed", workingTree->resources[0].tooltip);

	// Two files, two rows, two badges, and two operands: a row that was never
	// published must not leave an operand a menu could act on.
	ASSERT_TRUE(publication.provider.count.has_value());
	EXPECT_EQ(2, *publication.provider.count);
	EXPECT_EQ(2U, publication.decorations.size());
	EXPECT_EQ(2U, publication.operands.size());
	for (const auto& operand : publication.operands) {
		EXPECT_NE(EGitResourceGroup::Untracked, operand.resource.group);
	}
}

TEST(GitScmPublisher, EncodesDirtinessInTheCheckoutIconNotTheBranchName)
{
	auto state = RepositoryState();
	EXPECT_EQ("$(git-branch)", GitCheckoutStatusIcon(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Change(L"a.txt", L'.', L'M') };
	EXPECT_EQ("$(git-branch-changes)", GitCheckoutStatusIcon(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Change(L"a.txt", L'M', L'.') };
	EXPECT_EQ("$(git-branch-staged-changes)", GitCheckoutStatusIcon(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Conflicted(L"a.txt", L'U', L'U') };
	EXPECT_EQ("$(git-branch-conflicts)", GitCheckoutStatusIcon(state, EUntrackedChangesPolicy::Mixed));

	auto detached = RepositoryState();
	detached.branch.clear();
	EXPECT_EQ("$(git-commit)", GitCheckoutStatusIcon(detached, EUntrackedChangesPolicy::Mixed));

	const auto checkout = BuildCheckoutStatusBarCommand(RepositoryState(), EUntrackedChangesPolicy::Mixed);
	EXPECT_EQ("git.checkout", checkout.command);
	EXPECT_EQ("$(git-branch) master", checkout.title);
}

TEST(GitScmPublisher, PublishesTheSyncCommandTheWayUpstreamDoes)
{
	auto noUpstream = RepositoryState();
	noUpstream.upstream.clear();
	const auto publish = BuildSyncStatusBarCommand(noUpstream);
	EXPECT_EQ("git.publish", publish.command);
	EXPECT_EQ("$(cloud-upload) Publish Branch", publish.title);

	const auto synced = BuildSyncStatusBarCommand(RepositoryState());
	EXPECT_EQ("git.sync", synced.command);
	EXPECT_EQ("$(sync)", synced.title);

	auto diverged = RepositoryState();
	diverged.ahead = 2;
	diverged.behind = 3;
	const auto counts = BuildSyncStatusBarCommand(diverged);
	EXPECT_EQ("git.sync", counts.command);
	EXPECT_NE(std::string::npos, counts.title.find("3"));
	EXPECT_NE(std::string::npos, counts.title.find("2"));
}

TEST(GitScmModel, ParsesTheObjectNameAndTheDetachedHead)
{
	const std::string detached = "# branch.oid 1234567890abcdef1234567890abcdef12345678\0"
		"# branch.head (detached)\0"s;
	const auto state = ParsePorcelainV2(detached);
	EXPECT_TRUE(state.repository);
	// `(detached)` is not a branch name. Storing it would invent a branch, and
	// the short object name is what names HEAD in that state.
	EXPECT_TRUE(state.branch.empty());
	EXPECT_EQ(L"1234567890abcdef1234567890abcdef12345678", state.commit);

	// `(initial)` is upstream's undefined `HEAD.commit`, not an object name.
	const auto unborn = ParsePorcelainV2("# branch.oid (initial)\0# branch.head master\0"s);
	EXPECT_TRUE(unborn.repository);
	EXPECT_EQ(L"master", unborn.branch);
	EXPECT_TRUE(unborn.commit.empty());
}

TEST(GitScmPublisher, NamesHeadTheWayUpstreamsHeadShortNameDoes)
{
	auto state = RepositoryState();
	EXPECT_EQ("master", GitHeadShortName(state));

	// Detached: upstream's `headShortName` falls back to `substr(0, 8)`, a literal
	// 8 rather than `git.commitShortHashLength`. That setting's default is 7 and
	// governs only the Quick Pick's descriptions, so the two lengths differ on
	// purpose and unifying them would move the status bar off VS Code by one
	// character.
	auto detached = RepositoryState();
	detached.branch.clear();
	detached.commit = L"1234567890abcdef";
	EXPECT_EQ("12345678", GitHeadShortName(detached));
	EXPECT_EQ(7, kGitCommitShortHashLength);

	// A short object name is never padded; a repository whose HEAD names
	// nothing at all has no short name rather than a placeholder one.
	auto shortCommit = detached;
	shortCommit.commit = L"1234";
	EXPECT_EQ("1234", GitHeadShortName(shortCommit));
	auto unborn = detached;
	unborn.commit.clear();
	EXPECT_TRUE(GitHeadShortName(unborn).empty());
	EXPECT_TRUE(GitHeadLabel(unborn, EUntrackedChangesPolicy::Mixed).empty());

	// An unborn detached HEAD has no label to click, so the status item names
	// HEAD rather than rendering an empty, unclickable gap.
	const auto command = BuildCheckoutStatusBarCommand(unborn, EUntrackedChangesPolicy::Mixed);
	EXPECT_EQ("$(git-commit) HEAD", command.title);
	EXPECT_EQ("HEAD, Checkout Branch/Tag...", command.tooltip);
}

TEST(GitScmPublisher, AppendsUpstreamsHeadLabelMarkers)
{
	// The icon and the markers carry the same information, and upstream renders
	// both: `headLabel` is not a second encoding that replaces the icon.
	auto state = RepositoryState();
	EXPECT_EQ("master", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Change(L"a.txt", L'.', L'M') };
	EXPECT_EQ("master*", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Change(L"a.txt", L'M', L'.') };
	EXPECT_EQ("master+", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	// Upstream's order is `*`, then `+`, then `!`, and one path with both staged
	// and unstaged edits genuinely earns the first two at once.
	state.changes = { Change(L"a.txt", L'M', L'M') };
	EXPECT_EQ("master*+", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Conflicted(L"a.txt", L'U', L'U') };
	EXPECT_EQ("master!", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	state.changes = { Change(L"a.txt", L'M', L'M'), Conflicted(L"b.txt", L'U', L'U') };
	EXPECT_EQ("master*+!", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));

	// The markers read the classified groups, not the raw status columns, so an
	// untracked file follows `git.untrackedChanges` exactly as the groups do.
	state.changes = { Untracked(L"new.txt") };
	EXPECT_EQ("master*", GitHeadLabel(state, EUntrackedChangesPolicy::Mixed));
	EXPECT_EQ("master", GitHeadLabel(state, EUntrackedChangesPolicy::Hidden));

	// The whole label is what the status item's tooltip names.
	const auto command = BuildCheckoutStatusBarCommand(state, EUntrackedChangesPolicy::Mixed);
	EXPECT_EQ("$(git-branch-changes) master*", command.title);
	EXPECT_EQ("master*, Checkout Branch/Tag...", command.tooltip);
}

TEST(GitScmPublisher, GivesTheSyncItemUpstreamsPerStateTooltip)
{
	auto noUpstream = RepositoryState();
	noUpstream.upstream.clear();
	EXPECT_EQ("Publish Branch", BuildSyncStatusBarCommand(noUpstream).tooltip);

	EXPECT_EQ("Synchronize Changes", BuildSyncStatusBarCommand(RepositoryState()).tooltip);

	auto behind = RepositoryState();
	behind.behind = 3;
	EXPECT_EQ("Pull 3 commits from origin/master", BuildSyncStatusBarCommand(behind).tooltip);

	auto ahead = RepositoryState();
	ahead.ahead = 2;
	EXPECT_EQ("Push 2 commits to origin/master", BuildSyncStatusBarCommand(ahead).tooltip);

	auto diverged = RepositoryState();
	diverged.ahead = 2;
	diverged.behind = 3;
	EXPECT_EQ("Pull 3 and push 2 commits between origin/master",
		BuildSyncStatusBarCommand(diverged).tooltip);
	// Behind is rendered first in the title too, so the arrows and the sentence
	// cannot disagree about which number is which.
	EXPECT_EQ("$(sync) 3\xe2\x86\x93 2\xe2\x86\x91", BuildSyncStatusBarCommand(diverged).title);
}

TEST(GitScmPublisher, BuildsUpstreamIdentitiesGroupsAndCount)
{
	auto state = RepositoryState();
	state.changes = {
		Conflicted(L"conflict.txt", L'U', L'U'),
		Change(L"both.txt", L'M', L'M'),
		Untracked(L"new.txt"),
	};

	const auto publication = BuildGitPublication(Owner(), L"C:\\repo\\project", state);
	const auto& provider = publication.provider;
	EXPECT_EQ("git", provider.id);
	// `label` is the source-control system, `name` is this repository. The
	// repository row renders `name` and puts `label` in its title, so conflating
	// the two would make the row read `Git` for every repository at once.
	EXPECT_EQ("Git", provider.label);
	EXPECT_EQ("project", provider.name);
	EXPECT_EQ("project", provider.Name());
	ASSERT_TRUE(provider.rootUri.has_value());
	ASSERT_TRUE(provider.acceptInputCommand.has_value());
	EXPECT_EQ("git.commit", provider.acceptInputCommand->command);
	ASSERT_EQ(2U, provider.statusBarCommands.size());
	EXPECT_EQ("git.checkout", provider.statusBarCommands[0].command);
	// `updateInputBoxPlaceholder` names the branch in double quotes; the quotes
	// are what separate the branch from the surrounding sentence when a branch
	// name contains a space.
	EXPECT_EQ("Message (Ctrl+Enter to commit on \"master\")", provider.inputBox.placeholder);

	const GitDiffTextResolver localized = [](std::string_view key, std::wstring_view argument) {
		if (key != "GitCommitMessageOnBranch") return std::wstring{};
		return std::wstring(L"Localized commit on ") + std::wstring(argument);
	};
	const auto localizedPublication = BuildGitPublication(Owner(), L"C:\\repo\\project", state,
		EUntrackedChangesPolicy::Mixed, localized);
	EXPECT_EQ("Localized commit on master", localizedPublication.provider.inputBox.placeholder);

	// Upstream declaration order, so the SCM view renders the groups in the
	// order a VS Code user already knows.
	ASSERT_EQ(4U, provider.groups.size());
	EXPECT_EQ("merge", provider.groups[0].id);
	EXPECT_EQ("index", provider.groups[1].id);
	EXPECT_EQ("workingTree", provider.groups[2].id);
	EXPECT_EQ("untracked", provider.groups[3].id);
	EXPECT_EQ("Staged Changes", provider.groups[1].label);
	EXPECT_TRUE(provider.groups[0].hideWhenEmpty);
	EXPECT_TRUE(provider.groups[3].hideWhenEmpty);
	EXPECT_FALSE(provider.groups[2].hideWhenEmpty);

	ASSERT_EQ(1U, FindGroup(provider, "merge")->resources.size());
	ASSERT_EQ(1U, FindGroup(provider, "index")->resources.size());
	// `both.txt` staged and unstaged, plus the untracked file under the default
	// mixed policy.
	ASSERT_EQ(2U, FindGroup(provider, "workingTree")->resources.size());
	EXPECT_TRUE(FindGroup(provider, "untracked")->resources.empty());

	// The badge count is resources, not files: `both.txt` really is two rows.
	ASSERT_TRUE(provider.count.has_value());
	EXPECT_EQ(4, *provider.count);
	// Decorations are the opposite: the side table is keyed by resource URI, so
	// `both.txt` contributes one badge even though it occupies two rows. Three
	// files, four resources — asserting both is what keeps the two counts from
	// being quietly conflated.
	EXPECT_EQ(3U, publication.decorations.size());
	EXPECT_TRUE(publication.rejectedPaths.empty());
}

TEST(GitScmPublisher, PublishesReplacesAndRetractsOneProvider)
{
	SourceControlService service;
	GitScmPublisher publisher(&service, Owner());

	auto state = RepositoryState();
	state.changes = { Change(L"a.txt", L'.', L'M') };
	const auto first = publisher.Publish(L"C:\\repo\\project", state);
	EXPECT_TRUE(first == EScmOperationStatus::Succeeded || first == EScmOperationStatus::Replayed);
	EXPECT_TRUE(publisher.HasProvider());

	auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.providers.size());
	EXPECT_EQ("git", snapshot.providers[0].id);
	ASSERT_EQ(1U, FindGroup(snapshot.providers[0], "workingTree")->resources.size());

	// A second publication must replace the provider rather than register a
	// second one, so the view never sees the same repository twice.
	state.changes.push_back(Change(L"b.txt", L'.', L'M'));
	EXPECT_TRUE(publisher.Publish(L"C:\\repo\\project", state) != EScmOperationStatus::InvalidProvider);
	snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.providers.size());
	EXPECT_EQ(2U, FindGroup(snapshot.providers[0], "workingTree")->resources.size());

	// An unchanged refresh must not spend a service revision.
	const auto revision = snapshot.revision;
	EXPECT_EQ(EScmOperationStatus::Replayed, publisher.Publish(L"C:\\repo\\project", state));
	EXPECT_EQ(revision, service.Snapshot().revision);

	EXPECT_EQ(EScmOperationStatus::Succeeded, publisher.Retract());
	EXPECT_FALSE(publisher.HasProvider());
	EXPECT_TRUE(service.Snapshot().providers.empty());

	// Retract is idempotent: a torn-down repository must not fail on a second
	// teardown path reaching the same publisher.
	EXPECT_EQ(EScmOperationStatus::NotApplicable, publisher.Retract());
}

TEST(GitScmPublisher, WithoutAServiceEveryOperationIsNotApplicable)
{
	GitScmPublisher publisher(nullptr, Owner());
	EXPECT_EQ(EScmOperationStatus::NotApplicable, publisher.Publish(L"C:\\repo", RepositoryState()));
	EXPECT_FALSE(publisher.HasProvider());
	EXPECT_EQ(EScmOperationStatus::NotApplicable, publisher.Retract());
}

// --- GitRefModel -----------------------------------------------------------

//! Builds one `for-each-ref` output line in upstream's NUL-separated format.
std::string RefLine(const char* refName, const char* commit, const char* tagCommit = "")
{
	std::string line = refName;
	line.push_back('\0');
	line += commit;
	line.push_back('\0');
	line += tagCommit;
	line.push_back('\n');
	return line;
}

constexpr const char* kCommitA = "1111111111111111111111111111111111111111";
constexpr const char* kCommitB = "2222222222222222222222222222222222222222";
constexpr const char* kCommitC = "3333333333333333333333333333333333333333";
constexpr const char* kCommitD = "4444444444444444444444444444444444444444";

const GitCheckoutItem* FindByRefName(const std::vector<GitCheckoutItem>& items, const wchar_t* refName)
{
	const auto found = std::find_if(items.begin(), items.end(), [refName](const GitCheckoutItem& item) {
		return item.kind == EGitCheckoutItemKind::Ref && item.refName == refName;
	});
	return found == items.end() ? nullptr : &*found;
}

TEST(GitRefModel, BuildsUpstreamForEachRefArguments)
{
	// The format string is what makes a ref name with a space survive parsing,
	// so it is asserted verbatim rather than merely "contains --format".
	const auto arguments = BuildForEachRefArguments();
	ASSERT_EQ(3U, arguments.size());
	EXPECT_EQ(L"for-each-ref", arguments[0]);
	EXPECT_EQ(L"--format", arguments[1]);
	EXPECT_EQ(L"%(refname)%00%(objectname)%00%(*objectname)", arguments[2]);

	// `--count` is only emitted when a bound was asked for; upstream omits it
	// entirely otherwise rather than passing a sentinel.
	const auto bounded = BuildForEachRefArguments(50);
	ASSERT_EQ(4U, bounded.size());
	EXPECT_EQ(L"--count=50", bounded[1]);
}

TEST(GitRefModel, ParsesHeadsRemoteHeadsAndTags)
{
	std::string output;
	output += RefLine("refs/heads/main", kCommitA);
	output += RefLine("refs/remotes/origin/main", kCommitB);
	// An annotated tag names the tag object in `%(objectname)` and the commit it
	// points at in `%(*objectname)`. Upstream reports the commit.
	output += RefLine("refs/tags/v1.0.0", kCommitC, kCommitD);

	const auto refs = ParseForEachRef(output);
	ASSERT_EQ(3U, refs.size());

	EXPECT_EQ(EGitRefKind::Head, refs[0].kind);
	EXPECT_EQ(L"main", refs[0].name);
	EXPECT_EQ(L"1111111111111111111111111111111111111111", refs[0].commit);
	EXPECT_TRUE(refs[0].remote.empty());

	// The remote is captured separately even though the name still carries it,
	// because a checkout needs the remote to find the tracking branch.
	EXPECT_EQ(EGitRefKind::RemoteHead, refs[1].kind);
	EXPECT_EQ(L"origin/main", refs[1].name);
	EXPECT_EQ(L"origin", refs[1].remote);

	EXPECT_EQ(EGitRefKind::Tag, refs[2].kind);
	EXPECT_EQ(L"v1.0.0", refs[2].name);
	EXPECT_EQ(L"4444444444444444444444444444444444444444", refs[2].commit);

	// A lightweight tag has no dereferenced object and keeps its own name.
	const auto lightweight = ParseForEachRef(RefLine("refs/tags/v0.9", kCommitC));
	ASSERT_EQ(1U, lightweight.size());
	EXPECT_EQ(L"3333333333333333333333333333333333333333", lightweight[0].commit);
}

TEST(GitRefModel, SkipsLinesUpstreamWouldNotMatch)
{
	std::string output;
	output += RefLine("refs/heads/good", kCommitA);
	// Not under `refs/`.
	output += RefLine("HEAD", kCommitB);
	// A name containing a space: upstream's `([^ ]+)` rejects it, and inventing
	// a ref here would make this list disagree with VS Code's.
	output += RefLine("refs/heads/bad name", kCommitB);
	// Not a 40-hex object name.
	output += RefLine("refs/heads/short", "abc");
	// A namespace upstream has no regex for.
	output += RefLine("refs/stash", kCommitB);
	// A bare remote directory with no branch component.
	output += RefLine("refs/remotes/origin", kCommitB);
	output += "\n";

	const auto refs = ParseForEachRef(output);
	ASSERT_EQ(1U, refs.size());
	EXPECT_EQ(L"good", refs[0].name);

	EXPECT_TRUE(ParseForEachRef("").empty());
}

TEST(GitRefModel, BuildsCheckoutQuickPickInUpstreamOrder)
{
	std::vector<GitRef> refs;
	refs.push_back({ EGitRefKind::Head, L"main", L"1111111111111111111111111111111111111111", {} });
	refs.push_back({ EGitRefKind::RemoteHead, L"origin/main", L"2222222222222222222222222222222222222222", L"origin" });
	refs.push_back({ EGitRefKind::Tag, L"v1.0.0", L"3333333333333333333333333333333333333333", {} });
	// `origin/HEAD` is a symbolic alias, not a branch to check out.
	refs.push_back({ EGitRefKind::RemoteHead, L"origin/HEAD", L"2222222222222222222222222222222222222222", L"origin" });

	const auto items = BuildCheckoutItems(refs, /*detached*/ false, /*filterIsEmpty*/ true);

	// With nothing typed the three command rows lead, because they are the
	// actions the user cannot reach by typing a branch name.
	ASSERT_EQ(9U, items.size());
	EXPECT_EQ(EGitCheckoutItemKind::CreateBranch, items[0].kind);
	EXPECT_EQ(L"$(plus) Create new branch...", items[0].label);
	EXPECT_EQ(EGitCheckoutItemKind::CreateBranchFrom, items[1].kind);
	EXPECT_EQ(L"$(plus) Create new branch from...", items[1].label);
	EXPECT_EQ(EGitCheckoutItemKind::CheckoutDetached, items[2].kind);
	EXPECT_EQ(L"$(debug-disconnect) Checkout detached...", items[2].label);

	// Each non-empty group is introduced by its own separator, in the order
	// local, remote, tags.
	EXPECT_EQ(EGitCheckoutItemKind::Separator, items[3].kind);
	EXPECT_EQ(L"branches", items[3].label);
	EXPECT_EQ(L"$(git-branch) main", items[4].label);
	EXPECT_EQ(L"1111111", items[4].description);

	EXPECT_EQ(EGitCheckoutItemKind::Separator, items[5].kind);
	EXPECT_EQ(L"remote branches", items[5].label);
	EXPECT_EQ(L"$(cloud) origin/main", items[6].label);
	EXPECT_EQ(L"Remote branch at 2222222", items[6].description);
	EXPECT_EQ(L"origin", items[6].remote);

	EXPECT_EQ(EGitCheckoutItemKind::Separator, items[7].kind);
	EXPECT_EQ(L"tags", items[7].label);
	EXPECT_EQ(L"$(tag) v1.0.0", items[8].label);
	EXPECT_EQ(L"Tag at 3333333", items[8].description);

	EXPECT_EQ(nullptr, FindByRefName(items, L"origin/HEAD"));
	EXPECT_EQ(L"Select a branch or tag to checkout", CheckoutPlaceholder(false));
}

TEST(GitRefModel, MovesCommandsBelowTheRefsOnceAFilterIsTyped)
{
	std::vector<GitRef> refs;
	refs.push_back({ EGitRefKind::Head, L"main", L"1111111111111111111111111111111111111111", {} });

	const auto filtered = BuildCheckoutItems(refs, /*detached*/ false, /*filterIsEmpty*/ false);
	ASSERT_EQ(6U, filtered.size());
	EXPECT_EQ(EGitCheckoutItemKind::Separator, filtered[0].kind);
	EXPECT_EQ(EGitCheckoutItemKind::Ref, filtered[1].kind);
	// A blank separator, not a labelled group header, is what upstream inserts
	// between the matched refs and the commands.
	EXPECT_EQ(EGitCheckoutItemKind::Separator, filtered[2].kind);
	EXPECT_TRUE(filtered[2].label.empty());
	EXPECT_EQ(EGitCheckoutItemKind::CreateBranch, filtered[3].kind);
	EXPECT_EQ(EGitCheckoutItemKind::CheckoutDetached, filtered[5].kind);

	// An empty repository still offers the commands, and offers them alone:
	// there is no group to put before or after them.
	const auto empty = BuildCheckoutItems({}, false, false);
	ASSERT_EQ(3U, empty.size());
	EXPECT_EQ(EGitCheckoutItemKind::CreateBranch, empty[0].kind);
}

TEST(GitRefModel, DetachedCheckoutDropsTagsCommandsAndKeepsOriginHead)
{
	std::vector<GitRef> refs;
	refs.push_back({ EGitRefKind::Head, L"main", L"1111111111111111111111111111111111111111", {} });
	refs.push_back({ EGitRefKind::RemoteHead, L"origin/HEAD", L"2222222222222222222222222222222222222222", L"origin" });
	refs.push_back({ EGitRefKind::Tag, L"v1.0.0", L"3333333333333333333333333333333333333333", {} });

	const auto items = BuildCheckoutItems(refs, /*detached*/ true, /*filterIsEmpty*/ true);

	// Detaching onto a tag is what `git.checkout` already does, so the detached
	// picker drops the tag group instead of duplicating it.
	EXPECT_EQ(nullptr, FindByRefName(items, L"v1.0.0"));
	EXPECT_TRUE(std::none_of(items.begin(), items.end(), [](const GitCheckoutItem& item) {
		return item.kind == EGitCheckoutItemKind::CreateBranch
			|| item.kind == EGitCheckoutItemKind::CreateBranchFrom
			|| item.kind == EGitCheckoutItemKind::CheckoutDetached;
	}));
	// Detached mode is the one place `origin/HEAD` is a legitimate target.
	EXPECT_NE(nullptr, FindByRefName(items, L"origin/HEAD"));
	EXPECT_EQ(L"Select a branch to checkout in detached mode", CheckoutPlaceholder(true));
}

TEST(GitRefModel, BranchFromLeadsWithHeadAndAlwaysSkipsOriginHead)
{
	std::vector<GitRef> refs;
	refs.push_back({ EGitRefKind::Head, L"main", L"1111111111111111111111111111111111111111", {} });
	refs.push_back({ EGitRefKind::Tag, L"v1.0.0", L"3333333333333333333333333333333333333333", {} });
	refs.push_back({ EGitRefKind::RemoteHead, L"origin/HEAD", L"2222222222222222222222222222222222222222", L"origin" });

	const auto items = BuildBranchFromItems(refs, L"4444444444444444444444444444444444444444");

	// HEAD is the default target, so it is the first row and carries no icon.
	ASSERT_FALSE(items.empty());
	EXPECT_EQ(L"HEAD", items[0].label);
	EXPECT_EQ(L"HEAD", items[0].refName);
	EXPECT_EQ(L"4444444", items[0].description);

	// Unlike the checkout picker, this one has no detached exception at all.
	EXPECT_EQ(nullptr, FindByRefName(items, L"origin/HEAD"));
	// Tags are legitimate branch points here.
	EXPECT_NE(nullptr, FindByRefName(items, L"v1.0.0"));
}

TEST(GitRefModel, SanitizesBranchNamesTheWayUpstreamDoes)
{
	// Each replacement below is a character git itself rejects in a ref name.
	EXPECT_EQ(L"feature-new", SanitizeBranchName(L"feature new"));
	EXPECT_EQ(L"feature", SanitizeBranchName(L"  feature  "));
	EXPECT_EQ(L"feature", SanitizeBranchName(L"---feature"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a..b"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a~b"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a^b"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a:b"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a\\b"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a*b"));
	EXPECT_EQ(L"-hidden", SanitizeBranchName(L".hidden"));
	EXPECT_EQ(L"a-", SanitizeBranchName(L"a."));
	EXPECT_EQ(L"a-", SanitizeBranchName(L"a/"));
	EXPECT_EQ(L"a-b", SanitizeBranchName(L"a[b"));
	EXPECT_EQ(L"feature-x", SanitizeBranchName(L"feature.lock/x"));

	// A slash between segments is legal and must survive; sanitizing it away
	// would silently flatten every `feature/...` name users actually type.
	EXPECT_EQ(L"feature/new-thing", SanitizeBranchName(L"feature/new thing"));

	// The whitespace replacement character is configurable upstream.
	EXPECT_EQ(L"feature_new", SanitizeBranchName(L"feature new", L'_'));

	// Empty stays empty rather than becoming the replacement character.
	EXPECT_TRUE(SanitizeBranchName(L"").empty());
}

TEST(GitRefModel, ValidatesBranchNamesInUpstreamOrder)
{
	std::vector<GitRef> refs;
	refs.push_back({ EGitRefKind::Head, L"main", L"1111111111111111111111111111111111111111", {} });

	const auto ok = ValidateBranchName(L"feature", refs);
	EXPECT_EQ(EGitBranchNameValidation::Valid, ok.state);
	EXPECT_EQ(L"feature", ok.sanitizedName);
	EXPECT_TRUE(ok.message.empty());

	// The collision is checked against the sanitized name, so typing `main `
	// must be reported as an existing branch rather than accepted as new.
	const auto existing = ValidateBranchName(L"main ", refs);
	EXPECT_EQ(EGitBranchNameValidation::AlreadyExists, existing.state);
	EXPECT_EQ(L"Branch \"main\" already exists", existing.message);

	// A name that only needed sanitizing is still accepted; upstream shows the
	// resulting name as information, not as an error.
	const auto sanitized = ValidateBranchName(L"my feature", refs);
	EXPECT_EQ(EGitBranchNameValidation::Sanitized, sanitized.state);
	EXPECT_EQ(L"my-feature", sanitized.sanitizedName);
	EXPECT_EQ(L"The new branch will be \"my-feature\"", sanitized.message);

	// An empty box is upstream's cancel path: `promptForBranchName` returns the
	// empty name and `_branch` returns without running anything.
	const auto empty = ValidateBranchName(L"", refs);
	EXPECT_EQ(EGitBranchNameValidation::Empty, empty.state);
	EXPECT_TRUE(empty.sanitizedName.empty());

	// A whitespace-only name is NOT that path. Upstream trims to the empty
	// string and then its `^\s*$` alternative replaces it with the whitespace
	// character, so the proposed name is `-` and the flow continues. Reproduce
	// that rather than "improving" it: a divergence here would make the same
	// keystrokes create a differently-named branch than in VS Code.
	const auto whitespace = ValidateBranchName(L"   ", refs);
	EXPECT_EQ(EGitBranchNameValidation::Sanitized, whitespace.state);
	EXPECT_EQ(L"-", whitespace.sanitizedName);
}

TEST(GitRefModel, BuildsUpstreamCheckoutAndBranchArguments)
{
	const auto checkout = BuildCheckoutArguments(L"main", /*detached*/ false);
	ASSERT_EQ(3U, checkout.size());
	EXPECT_EQ(L"checkout", checkout[0]);
	EXPECT_EQ(L"-q", checkout[1]);
	EXPECT_EQ(L"main", checkout[2]);

	const auto detached = BuildCheckoutArguments(L"1111111", /*detached*/ true);
	ASSERT_EQ(4U, detached.size());
	EXPECT_EQ(L"--detach", detached[2]);
	EXPECT_EQ(L"1111111", detached[3]);

	// Checking out a remote head with no local counterpart tracks it rather
	// than detaching, which is what makes the new branch push to the right place.
	const auto tracking = BuildCheckoutTrackingArguments(L"origin/feature");
	ASSERT_EQ(4U, tracking.size());
	EXPECT_EQ(L"--track", tracking[2]);
	EXPECT_EQ(L"origin/feature", tracking[3]);

	// Creating a branch checks it out in the same command, and `--no-track`
	// keeps it from silently adopting the target's upstream.
	const auto created = BuildCreateBranchArguments(L"feature", L"HEAD");
	ASSERT_EQ(6U, created.size());
	EXPECT_EQ(L"-b", created[2]);
	EXPECT_EQ(L"feature", created[3]);
	EXPECT_EQ(L"--no-track", created[4]);
	EXPECT_EQ(L"HEAD", created[5]);
}

// --- GitBranchCommands -----------------------------------------------------

//! One `for-each-ref refs/heads` line in `%(refname:short)%00%(upstream:short)`.
std::string TrackingLine(const char* branch, const char* upstream)
{
	std::string line = branch;
	line.push_back('\0');
	line += upstream;
	line.push_back('\n');
	return line;
}

//! Replays canned git output and records every argument vector, so the assertion
//! is the exact command the flow chose rather than an effect observed in a real
//! repository. The listing commands answer regardless of the failure switch,
//! because a test about a failing checkout must still be able to reach it.
class FakeGit final {
public:
	std::vector<std::vector<std::wstring>> invocations;
	//! `for-each-ref` over every namespace.
	std::string refs;
	//! `for-each-ref refs/heads`, upstream's `findTrackingBranches` listing.
	std::string tracking;
	std::string head = kCommitA;
	//! Applied to the one command that is neither a listing nor `rev-parse`.
	bool actionFails = false;
	std::string actionStandardError;

	GitExecutionResult operator()(const std::vector<std::wstring>& arguments)
	{
		invocations.push_back(arguments);
		GitExecutionResult result;
		result.status = EGitExecutionStatus::Succeeded;
		result.exitCode = 0;
		const auto emit = [&result](const std::string& bytes) {
			result.standardOutput.assign(bytes.begin(), bytes.end());
		};
		if (!arguments.empty() && arguments[0] == L"for-each-ref") {
			const bool heads = std::find(arguments.begin(), arguments.end(), L"refs/heads") != arguments.end();
			emit(heads ? tracking : refs);
			return result;
		}
		if (!arguments.empty() && arguments[0] == L"rev-parse") {
			emit(head + "\n");
			return result;
		}
		if (actionFails) {
			result.status = EGitExecutionStatus::Failed;
			result.exitCode = 1;
			result.standardError = actionStandardError;
		}
		return result;
	}

	[[nodiscard]] const std::vector<std::wstring>& Last() const { return invocations.back(); }
};

//! Picks the row carrying this ref name. Selecting by name rather than by index
//! is deliberate: the index of a ref depends on how many group separators
//! precede it, and a test that encoded that would pass for the wrong reason.
GitQuickPickPresenter PickRef(std::wstring refName, std::vector<std::wstring>* placeholders = nullptr)
{
	return [refName = std::move(refName), placeholders](
		const std::vector<GitCheckoutItem>& items, std::wstring_view placeholder) -> std::optional<std::size_t> {
		if (placeholders != nullptr) {
			placeholders->emplace_back(placeholder);
		}
		for (std::size_t index = 0; index < items.size(); ++index) {
			if (items[index].refName == refName) {
				return index;
			}
		}
		return std::nullopt;
	};
}

GitQuickPickPresenter PickKind(EGitCheckoutItemKind kind)
{
	return [kind](const std::vector<GitCheckoutItem>& items, std::wstring_view) -> std::optional<std::size_t> {
		for (std::size_t index = 0; index < items.size(); ++index) {
			if (items[index].kind == kind) {
				return index;
			}
		}
		return std::nullopt;
	};
}

struct RecordedPrompt final {
	std::wstring prompt;
	std::wstring placeholder;
	std::wstring value;
};

//! Types the given names in order; running out of names dismisses the box.
GitInputBoxPresenter TypeNames(std::vector<std::wstring> names, std::vector<RecordedPrompt>* recorded = nullptr)
{
	auto queue = std::make_shared<std::vector<std::wstring>>(std::move(names));
	auto next = std::make_shared<std::size_t>(0);
	return [queue, next, recorded](std::wstring_view prompt, std::wstring_view placeholder, std::wstring_view value)
		-> std::optional<std::wstring> {
		if (recorded != nullptr) {
			recorded->push_back({ std::wstring(prompt), std::wstring(placeholder), std::wstring(value) });
		}
		if (*next >= queue->size()) {
			return std::nullopt;
		}
		return (*queue)[(*next)++];
	};
}

GitBranchCommandContext MakeBranchContext(FakeGit& git, std::vector<std::wstring>* messages = nullptr)
{
	GitBranchCommandContext context;
	context.run = [&git](const std::vector<std::wstring>& arguments) { return git(arguments); };
	context.quickPick = [](const std::vector<GitCheckoutItem>&, std::wstring_view) -> std::optional<std::size_t> {
		return std::nullopt;
	};
	context.inputBox = [](std::wstring_view, std::wstring_view, std::wstring_view) -> std::optional<std::wstring> {
		return std::nullopt;
	};
	if (messages != nullptr) {
		context.message = [messages](std::wstring_view message) { messages->emplace_back(message); };
	}
	return context;
}

TEST(GitBranchCommands, ListsTrackingBranchesTheWayUpstreamDoes)
{
	const auto arguments = BuildTrackingBranchArguments();
	ASSERT_EQ(4U, arguments.size());
	EXPECT_EQ(L"for-each-ref", arguments[0]);
	EXPECT_EQ(L"--format", arguments[1]);
	EXPECT_EQ(L"%(refname:short)%00%(upstream:short)", arguments[2]);
	// Restricting the listing to `refs/heads` is what keeps a remote ref from
	// matching itself and being "checked out" as its own tracking branch.
	EXPECT_EQ(L"refs/heads", arguments[3]);

	std::string output;
	output += TrackingLine("main", "origin/main");
	output += TrackingLine("feature", "origin/feature");
	// A branch with no upstream at all.
	output += TrackingLine("scratch", "");
	// A near miss: upstream compares the whole value, so this must not match.
	output += TrackingLine("feature-2", "origin/feature-2");

	const auto matched = ParseTrackingBranches(output, L"origin/feature");
	ASSERT_EQ(1U, matched.size());
	EXPECT_EQ(L"feature", matched[0]);

	EXPECT_TRUE(ParseTrackingBranches(output, L"origin/absent").empty());
	// An empty remote name would otherwise match every unpushed local branch.
	EXPECT_TRUE(ParseTrackingBranches(output, L"").empty());
}

TEST(GitBranchCommands, ChecksOutARemoteHeadThroughItsExistingLocalBranch)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA) + RefLine("refs/heads/feature", kCommitB)
		+ RefLine("refs/remotes/origin/feature", kCommitB);
	git.tracking = TrackingLine("main", "origin/main") + TrackingLine("feature", "origin/feature");

	std::vector<std::wstring> messages;
	auto context = MakeBranchContext(git, &messages);
	context.quickPick = PickRef(L"origin/feature");

	const auto result = RunGitCheckout(context, /*detached*/ false);
	EXPECT_TRUE(result.Succeeded());
	EXPECT_TRUE(messages.empty());

	// Checking the remote ref out directly would detach HEAD, so upstream
	// switches to the local branch that already tracks it.
	ASSERT_EQ(3U, git.Last().size());
	EXPECT_EQ(L"checkout", git.Last()[0]);
	EXPECT_EQ(L"-q", git.Last()[1]);
	EXPECT_EQ(L"feature", git.Last()[2]);
}

TEST(GitBranchCommands, TracksARemoteHeadThatHasNoLocalBranch)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA) + RefLine("refs/remotes/origin/feature", kCommitB);
	git.tracking = TrackingLine("main", "origin/main");

	auto context = MakeBranchContext(git);
	context.quickPick = PickRef(L"origin/feature");

	EXPECT_TRUE(RunGitCheckout(context, /*detached*/ false).Succeeded());
	ASSERT_EQ(4U, git.Last().size());
	EXPECT_EQ(L"--track", git.Last()[2]);
	EXPECT_EQ(L"origin/feature", git.Last()[3]);
}

TEST(GitBranchCommands, DetachedCheckoutTakesTheRemoteRefItself)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA) + RefLine("refs/remotes/origin/feature", kCommitB);
	git.tracking = TrackingLine("main", "origin/main");

	auto context = MakeBranchContext(git);
	context.quickPick = PickRef(L"origin/feature");

	EXPECT_TRUE(RunGitCheckout(context, /*detached*/ true).Succeeded());
	ASSERT_EQ(4U, git.Last().size());
	EXPECT_EQ(L"--detach", git.Last()[2]);
	EXPECT_EQ(L"origin/feature", git.Last()[3]);
	// The tracking lookup belongs to the attached path alone; asking for it here
	// would contradict the explicit request to detach.
	EXPECT_TRUE(std::none_of(git.invocations.begin(), git.invocations.end(), [](const std::vector<std::wstring>& arguments) {
		return std::find(arguments.begin(), arguments.end(), L"refs/heads") != arguments.end();
	}));
}

TEST(GitBranchCommands, TheDetachedRowReopensThePickerInDetachedMode)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	std::vector<std::wstring> placeholders;
	auto context = MakeBranchContext(git);
	context.quickPick = [&placeholders](const std::vector<GitCheckoutItem>& items, std::wstring_view placeholder)
		-> std::optional<std::size_t> {
		placeholders.emplace_back(placeholder);
		const auto kind = placeholders.size() == 1U ? EGitCheckoutItemKind::CheckoutDetached : EGitCheckoutItemKind::Ref;
		return PickKind(kind)(items, placeholder);
	};

	EXPECT_TRUE(RunGitCheckout(context, /*detached*/ false).Succeeded());
	ASSERT_EQ(2U, placeholders.size());
	EXPECT_EQ(L"Select a branch or tag to checkout", placeholders[0]);
	EXPECT_EQ(L"Select a branch to checkout in detached mode", placeholders[1]);
	ASSERT_EQ(4U, git.Last().size());
	EXPECT_EQ(L"--detach", git.Last()[2]);
	EXPECT_EQ(L"main", git.Last()[3]);
}

TEST(GitBranchCommands, ReportsGitsOwnFailureReason)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);
	git.actionFails = true;
	git.actionStandardError = "error: Your local changes would be overwritten by checkout.\n";

	std::vector<std::wstring> messages;
	auto context = MakeBranchContext(git, &messages);
	context.quickPick = PickRef(L"main");

	const auto result = RunGitCheckout(context, /*detached*/ false);
	EXPECT_EQ(EGitBranchCommandStatus::Failed, result.status);
	// git names the actual cause; a sentence written here would name a guess.
	EXPECT_EQ(L"error: Your local changes would be overwritten by checkout.", result.message);
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ(result.message, messages[0]);
}

TEST(GitBranchCommands, ADismissedPickerIsCancelledNotFailed)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	std::vector<std::wstring> messages;
	auto context = MakeBranchContext(git, &messages);

	const auto result = RunGitCheckout(context, /*detached*/ false);
	EXPECT_EQ(EGitBranchCommandStatus::Cancelled, result.status);
	EXPECT_TRUE(result.message.empty());
	// Dismissing a picker is not a failure, so nothing is reported to the user.
	EXPECT_TRUE(messages.empty());
	// Only the listing ran; no repository state was touched.
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(L"for-each-ref", git.invocations[0][0]);
}

TEST(GitBranchCommands, AnEmptyBranchNameCancelsWithoutCreatingAnything)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	auto context = MakeBranchContext(git);
	context.inputBox = TypeNames({ L"" });

	// Upstream's `promptForBranchName` returns the empty name and `_branch`
	// returns without running anything, so this is a cancel rather than an error.
	EXPECT_EQ(EGitBranchCommandStatus::Cancelled, RunGitCreateBranch(context, /*from*/ false).status);
	ASSERT_EQ(1U, git.invocations.size());
}

TEST(GitBranchCommands, ReAsksForABranchNameThatAlreadyExists)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	std::vector<RecordedPrompt> prompts;
	auto context = MakeBranchContext(git);
	context.inputBox = TypeNames({ L"main", L"feature" }, &prompts);

	EXPECT_TRUE(RunGitCreateBranch(context, /*from*/ false).Succeeded());

	ASSERT_EQ(2U, prompts.size());
	EXPECT_EQ(L"Please provide a new branch name", prompts[0].prompt);
	EXPECT_EQ(L"Branch name", prompts[0].placeholder);
	EXPECT_TRUE(prompts[0].value.empty());
	// The collision is what the second ask must say, and the rejected text must
	// still be there to edit; retyping it from scratch is not what VS Code does.
	EXPECT_EQ(L"Branch \"main\" already exists", prompts[1].prompt);
	EXPECT_EQ(L"main", prompts[1].value);

	ASSERT_EQ(6U, git.Last().size());
	EXPECT_EQ(L"-b", git.Last()[2]);
	EXPECT_EQ(L"feature", git.Last()[3]);
	EXPECT_EQ(L"--no-track", git.Last()[4]);
	EXPECT_EQ(L"HEAD", git.Last()[5]);
}

TEST(GitBranchCommands, ReportsASanitizedNameWithoutAskingAgain)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	std::vector<RecordedPrompt> prompts;
	std::vector<std::wstring> messages;
	auto context = MakeBranchContext(git, &messages);
	context.inputBox = TypeNames({ L"my feature" }, &prompts);

	EXPECT_TRUE(RunGitCreateBranch(context, /*from*/ false).Succeeded());
	// Upstream shows this as information and still accepts the name, so exactly
	// one ask happens and the sanitized name is what git receives.
	ASSERT_EQ(1U, prompts.size());
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ(L"The new branch will be \"my-feature\"", messages[0]);
	EXPECT_EQ(L"my-feature", git.Last()[3]);
}

TEST(GitBranchCommands, BranchFromPicksTheTargetBeforeAskingForTheName)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA) + RefLine("refs/tags/v1.0.0", kCommitC);
	git.head = kCommitD;

	std::vector<std::wstring> placeholders;
	auto context = MakeBranchContext(git);
	context.quickPick = PickRef(L"v1.0.0", &placeholders);
	context.inputBox = TypeNames({ L"feature" });

	EXPECT_TRUE(RunGitCreateBranch(context, /*from*/ true).Succeeded());
	ASSERT_EQ(1U, placeholders.size());
	EXPECT_EQ(L"Select a ref to create the branch from", placeholders[0]);
	// The picked ref, not HEAD, is the branch point.
	ASSERT_EQ(6U, git.Last().size());
	EXPECT_EQ(L"feature", git.Last()[3]);
	EXPECT_EQ(L"v1.0.0", git.Last()[5]);

	// The HEAD row's description needs the object name, so the flow reads it.
	EXPECT_TRUE(std::any_of(git.invocations.begin(), git.invocations.end(), [](const std::vector<std::wstring>& arguments) {
		return arguments.size() == 2U && arguments[0] == L"rev-parse" && arguments[1] == L"HEAD";
	}));
}

TEST(GitBranchCommands, TheCreateBranchRowRunsTheSameFlowAsTheBranchCommand)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	auto context = MakeBranchContext(git);
	context.quickPick = PickKind(EGitCheckoutItemKind::CreateBranch);
	context.inputBox = TypeNames({ L"feature" });

	EXPECT_TRUE(RunGitCheckout(context, /*detached*/ false).Succeeded());
	ASSERT_EQ(6U, git.Last().size());
	EXPECT_EQ(L"feature", git.Last()[3]);
	// A row inside the checkout picker still branches from HEAD, because the
	// row that branches from something else is the separate `from` row.
	EXPECT_EQ(L"HEAD", git.Last()[5]);
}

TEST(GitBranchCommands, AFailedListingNeverOpensAPicker)
{
	FakeGit git;
	git.refs = RefLine("refs/heads/main", kCommitA);

	std::vector<std::wstring> messages;
	auto context = MakeBranchContext(git, &messages);
	bool opened = false;
	context.quickPick = [&opened](const std::vector<GitCheckoutItem>&, std::wstring_view) -> std::optional<std::size_t> {
		opened = true;
		return std::nullopt;
	};
	context.run = [](const std::vector<std::wstring>&) {
		GitExecutionResult result;
		result.status = EGitExecutionStatus::GitUnavailable;
		return result;
	};

	const auto result = RunGitCheckout(context, /*detached*/ false);
	EXPECT_EQ(EGitBranchCommandStatus::Failed, result.status);
	// An empty ref list and an unreadable one are different facts; offering an
	// empty picker would render the second as the first.
	EXPECT_FALSE(opened);
	EXPECT_EQ(L"Git was not found on PATH.", result.message);
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ(result.message, messages[0]);
}

TEST(GitBranchCommands, WithoutAPresenterTheCommandFailsInsteadOfRunningGit)
{
	FakeGit git;
	GitBranchCommandContext context;
	context.run = [&git](const std::vector<std::wstring>& arguments) { return git(arguments); };

	EXPECT_EQ(EGitBranchCommandStatus::Failed, RunGitCheckout(context, false).status);
	EXPECT_EQ(EGitBranchCommandStatus::Failed, RunGitCreateBranch(context, false).status);
	EXPECT_TRUE(git.invocations.empty());
}

//! Records every invocation and answers `git branch` the way a repository with
//! at least one commit does, which is the case almost every test wants.
struct StageGit final {
	std::vector<std::vector<std::wstring>> invocations;
	//! Empty output from `git branch` is how upstream detects an unborn branch.
	std::string branches = "* main\n";
	bool actionFails = false;
	std::string actionStandardError;

	GitExecutionResult operator()(const std::vector<std::wstring>& arguments)
	{
		invocations.push_back(arguments);
		GitExecutionResult result;
		result.status = EGitExecutionStatus::Succeeded;
		result.exitCode = 0;
		if (!arguments.empty() && arguments[0] == L"branch") {
			result.standardOutput.assign(branches.begin(), branches.end());
			return result;
		}
		if (actionFails) {
			result.status = EGitExecutionStatus::Failed;
			result.exitCode = 1;
			result.standardError = actionStandardError;
		}
		return result;
	}

	[[nodiscard]] const std::vector<std::wstring>& Last() const { return invocations.back(); }

	//! Every invocation except the `git branch` probe, which is bookkeeping
	//! rather than an operation the user asked for.
	[[nodiscard]] std::vector<std::vector<std::wstring>> Operations() const
	{
		std::vector<std::vector<std::wstring>> operations;
		for (const auto& arguments : invocations) {
			if (arguments.size() != 1U || arguments[0] != L"branch") {
				operations.push_back(arguments);
			}
		}
		return operations;
	}
};

GitStageResource StagedRow(std::wstring path)
{
	return { std::move(path), EGitResourceGroup::Index, false, false };
}

GitStageResource ChangedRow(std::wstring path)
{
	return { std::move(path), EGitResourceGroup::WorkingTree, false, false };
}

GitStageResource DeletedRow(std::wstring path)
{
	GitStageResource resource = ChangedRow(std::move(path));
	resource.deleted = true;
	return resource;
}

//! An untracked row under the default `git.untrackedChanges: mixed`, where it
//! is listed in Changes but is still untracked.
GitStageResource UntrackedRow(std::wstring path)
{
	GitStageResource resource = ChangedRow(std::move(path));
	resource.untracked = true;
	return resource;
}

//! Answers a confirmation with a fixed choice index and records the prompt.
GitDiscardConfirmationPresenter Choose(std::size_t index, std::vector<GitDiscardPrompt>* prompts = nullptr)
{
	return [index, prompts](const GitDiscardPrompt& prompt) -> std::optional<std::size_t> {
		if (prompts != nullptr) {
			prompts->push_back(prompt);
		}
		return index;
	};
}

GitStageCommandContext MakeStageContext(StageGit& git, std::vector<std::wstring>* messages = nullptr)
{
	GitStageCommandContext context;
	context.repositoryRoot = LR"(C:\repo)";
	context.run = [&git](const std::vector<std::wstring>& arguments) { return git(arguments); };
	context.confirm = Choose(0);
	if (messages != nullptr) {
		context.message = [messages](std::wstring_view message) { messages->emplace_back(message); };
	}
	return context;
}

TEST(GitStageCommands, StagesWorkingTreeAndUntrackedRowsWithAddAll)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitStage(context, { ChangedRow(L"a.txt"), UntrackedRow(L"b.txt") }).Succeeded());
	ASSERT_EQ(1U, git.invocations.size());
	// `-A` is what makes staging a deleted row record the deletion; `-u` would
	// silently do nothing for an untracked row.
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-A", L"--", L"a.txt", L"b.txt" }), git.Last());
}

TEST(GitStageCommands, StageAllSkipsUntrackedRowsWhenTheyAreNotMixedIn)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitStage(context, { ChangedRow(L"a.txt") }, /*updateOnly*/ true).Succeeded());
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-u", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, StagingIgnoresRowsThatAreAlreadyStaged)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitStage(context, { StagedRow(L"staged.txt"), ChangedRow(L"a.txt") }).Succeeded());
	// The same file can appear in both groups; only the Changes row is an
	// operand here, which is exactly what makes the two rows different things.
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-A", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, AnEmptySelectionForItsGroupsRunsNothing)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_EQ(EGitStageCommandStatus::NotApplicable, RunGitStage(context, { StagedRow(L"staged.txt") }).status);
	EXPECT_EQ(EGitStageCommandStatus::NotApplicable, RunGitUnstage(context, { ChangedRow(L"a.txt") }).status);
	EXPECT_EQ(EGitStageCommandStatus::NotApplicable, RunGitDiscard(context, { StagedRow(L"staged.txt") }).status);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitStageCommands, StagingAMergeConflictFailsClosedInsteadOfStagingMarkers)
{
	StageGit git;
	std::vector<std::wstring> messages;
	auto context = MakeStageContext(git, &messages);

	GitStageResource conflicted{ L"c.txt", EGitResourceGroup::Merge, false, false };
	const auto result = RunGitStage(context, { conflicted, ChangedRow(L"a.txt") });

	// Upstream scans for conflict markers and confirms before staging one. Until
	// that exists, staging a file that may still contain `<<<<<<<` is refused
	// outright rather than approximated.
	EXPECT_EQ(EGitStageCommandStatus::UnsupportedMergeConflict, result.status);
	EXPECT_FALSE(result.message.empty());
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ(result.message, messages[0]);
	// Nothing was staged, including the unrelated row in the same selection.
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitStageCommands, UnstagesIndexRowsWithResetAgainstHead)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitUnstage(context, { StagedRow(L"a.txt"), ChangedRow(L"b.txt") }).Succeeded());
	const auto operations = git.Operations();
	ASSERT_EQ(1U, operations.size());
	EXPECT_EQ((std::vector<std::wstring>{ L"reset", L"-q", L"HEAD", L"--", L"a.txt" }), operations[0]);
}

TEST(GitStageCommands, UnstagesWithRmCachedOnAnUnbornBranch)
{
	StageGit git;
	git.branches.clear();
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitUnstage(context, { StagedRow(L"a.txt") }).Succeeded());
	const auto operations = git.Operations();
	ASSERT_EQ(1U, operations.size());
	// There is no HEAD to reset to before the first commit, so the only way to
	// unstage is to remove the path from the index.
	EXPECT_EQ((std::vector<std::wstring>{ L"rm", L"--cached", L"-r", L"--", L"a.txt" }), operations[0]);
}

TEST(GitStageCommands, UnstageAllResetsTheWholeIndexRatherThanListingPaths)
{
	StageGit git;
	auto context = MakeStageContext(git);

	EXPECT_TRUE(RunGitUnstageAll(context).Succeeded());
	const auto operations = git.Operations();
	ASSERT_EQ(1U, operations.size());
	// Upstream's `revert([])`. Listing every staged path instead would break on
	// an index holding more paths than one command line can carry.
	EXPECT_EQ((std::vector<std::wstring>{ L"reset", L"-q", L"HEAD", L"--", L"." }), operations[0]);
}

TEST(GitStageCommands, DiscardingTrackedChangesConfirmsThenRestoresFromTheIndex)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);

	EXPECT_TRUE(RunGitDiscard(context, { ChangedRow(L"a.txt") }).Succeeded());

	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(L"Are you sure you want to discard changes in 'a.txt'?", prompts[0].message);
	ASSERT_EQ(1U, prompts[0].choices.size());
	EXPECT_EQ(L"Discard File", prompts[0].choices[0].label);
	EXPECT_EQ((std::vector<std::wstring>{ L"checkout", L"-q", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, DiscardingOnlyDeletedFilesSaysRestoreInsteadOfDiscard)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);

	EXPECT_TRUE(RunGitDiscard(context, { DeletedRow(L"a.txt"), DeletedRow(L"b.txt") }).Succeeded());

	ASSERT_EQ(1U, prompts.size());
	// Bringing a file back and throwing an edit away are opposite outcomes, so a
	// dialog that says "discard" while it restores would be actively misleading.
	EXPECT_EQ(L"Are you sure you want to restore ALL 2 files?", prompts[0].message);
	ASSERT_EQ(1U, prompts[0].choices.size());
	EXPECT_EQ(L"Restore All 2 Files", prompts[0].choices[0].label);
}

TEST(GitStageCommands, DiscardingUntrackedFilesUsesTheRecycleBinAndNotGitClean)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	std::vector<std::wstring> trashed;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);
	context.trash = [&trashed](const std::vector<std::wstring>& paths) {
		trashed = paths;
		return std::vector<std::wstring>{};
	};

	EXPECT_TRUE(RunGitDiscard(context, { UntrackedRow(L"new.txt") }).Succeeded());

	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(L"Are you sure you want to DELETE the following untracked file: 'new.txt'?", prompts[0].message);
	// The detail is the promise the button makes, and it is only true because
	// the file really goes to the bin instead of through `git clean -f`.
	EXPECT_EQ(L"You can restore this file from the Recycle Bin.", prompts[0].detail);
	ASSERT_EQ(1U, prompts[0].choices.size());
	EXPECT_EQ(L"Move to Recycle Bin", prompts[0].choices[0].label);
	EXPECT_EQ((std::vector<std::wstring>{ LR"(C:\repo\new.txt)" }), trashed);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitStageCommands, WithoutARecycleBinTheUntrackedWordingBecomesTheIrreversibleWarning)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);

	EXPECT_TRUE(RunGitDiscard(context, { UntrackedRow(L"new.txt") }).Succeeded());

	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(L"Are you sure you want to DELETE the following untracked file: 'new.txt'?"
		L"\n\nThis is IRREVERSIBLE!\nThis file will be FOREVER LOST if you proceed.",
		prompts[0].message);
	EXPECT_TRUE(prompts[0].detail.empty());
	EXPECT_EQ(L"Delete File", prompts[0].choices[0].label);
	EXPECT_EQ((std::vector<std::wstring>{ L"clean", L"-f", L"-q", L"--", L"new.txt" }), git.Last());
}

TEST(GitStageCommands, ARefusedRecycleBinAsksAgainForExactlyThePathsItRefused)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);
	context.trash = [](const std::vector<std::wstring>& paths) {
		return std::vector<std::wstring>{ paths.back() };
	};

	EXPECT_TRUE(RunGitDiscard(context, { UntrackedRow(L"a.txt"), UntrackedRow(L"b.txt") }).Succeeded());

	ASSERT_EQ(2U, prompts.size());
	EXPECT_EQ(L"Failed to delete using the Recycle Bin. Do you want to permanently delete instead?",
		prompts[1].message);
	// The file that did reach the bin must not be deleted a second time, so the
	// fallback carries only the refusal.
	ASSERT_EQ(1U, prompts[1].choices.size());
	EXPECT_EQ(L"Delete File", prompts[1].choices[0].label);
	EXPECT_EQ((std::vector<std::wstring>{ L"clean", L"-f", L"-q", L"--", L"b.txt" }), git.Last());
}

TEST(GitStageCommands, ACancelledRecycleBinFallbackStillRestoresTheTrackedRows)
{
	StageGit git;
	std::size_t asked = 0;
	auto context = MakeStageContext(git);
	context.confirm = [&asked](const GitDiscardPrompt&) -> std::optional<std::size_t> {
		// Confirm the discard, then dismiss the permanent-delete fallback.
		return ++asked == 1U ? std::optional<std::size_t>(1U) : std::nullopt;
	};
	context.trash = [](const std::vector<std::wstring>& paths) { return paths; };

	const auto result = RunGitDiscard(context, { ChangedRow(L"a.txt"), UntrackedRow(L"new.txt") });

	// The tracked half was confirmed and happened; the untracked half was
	// declined. Reporting success would claim the whole request went through.
	EXPECT_EQ(EGitStageCommandStatus::Cancelled, result.status);
	EXPECT_EQ((std::vector<std::wstring>{ L"checkout", L"-q", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, AMixedSelectionOffersTwoButtonsThatDiscardDifferentSets)
{
	StageGit git;
	std::vector<GitDiscardPrompt> prompts;
	std::vector<std::wstring> trashed;
	auto context = MakeStageContext(git);
	context.confirm = Choose(0, &prompts);
	context.trash = [&trashed](const std::vector<std::wstring>& paths) {
		trashed = paths;
		return std::vector<std::wstring>{};
	};

	EXPECT_TRUE(RunGitDiscard(context, { ChangedRow(L"a.txt"), UntrackedRow(L"new.txt") }).Succeeded());

	ASSERT_EQ(1U, prompts.size());
	ASSERT_EQ(2U, prompts[0].choices.size());
	EXPECT_EQ(L"Discard 1 Tracked File", prompts[0].choices[0].label);
	EXPECT_EQ(L"Discard All 2 Files", prompts[0].choices[1].label);
	// Choosing the first button leaves the irreversible half alone. Collapsing
	// the two into one "yes" is precisely what would make that impossible.
	EXPECT_TRUE(trashed.empty());
	EXPECT_EQ((std::vector<std::wstring>{ L"checkout", L"-q", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, TheSecondMixedButtonDiscardsBothHalves)
{
	StageGit git;
	std::vector<std::wstring> trashed;
	auto context = MakeStageContext(git);
	context.confirm = Choose(1);
	context.trash = [&trashed](const std::vector<std::wstring>& paths) {
		trashed = paths;
		return std::vector<std::wstring>{};
	};

	EXPECT_TRUE(RunGitDiscard(context, { ChangedRow(L"a.txt"), UntrackedRow(L"new.txt") }).Succeeded());
	EXPECT_EQ((std::vector<std::wstring>{ LR"(C:\repo\new.txt)" }), trashed);
	EXPECT_EQ((std::vector<std::wstring>{ L"checkout", L"-q", L"--", L"a.txt" }), git.Last());
}

TEST(GitStageCommands, ADismissedDiscardConfirmationChangesNothing)
{
	StageGit git;
	auto context = MakeStageContext(git);
	context.confirm = [](const GitDiscardPrompt&) -> std::optional<std::size_t> { return std::nullopt; };

	const auto result = RunGitDiscard(context, { ChangedRow(L"a.txt") });
	EXPECT_EQ(EGitStageCommandStatus::Cancelled, result.status);
	EXPECT_TRUE(result.message.empty());
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitStageCommands, DiscardWithoutAConfirmationPresenterRefusesToRunGit)
{
	StageGit git;
	auto context = MakeStageContext(git);
	context.confirm = nullptr;

	// A missing presenter is a composition defect, and the only safe reading of
	// it is "do nothing" — never a silent discard.
	EXPECT_EQ(EGitStageCommandStatus::Failed, RunGitDiscard(context, { ChangedRow(L"a.txt") }).status);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitStageCommands, ReportsGitsOwnFailureReason)
{
	StageGit git;
	git.actionFails = true;
	git.actionStandardError = "error: pathspec 'a.txt' did not match any file(s) known to git\n";

	std::vector<std::wstring> messages;
	auto context = MakeStageContext(git, &messages);

	const auto result = RunGitStage(context, { ChangedRow(L"a.txt") });
	EXPECT_EQ(EGitStageCommandStatus::Failed, result.status);
	EXPECT_EQ(L"error: pathspec 'a.txt' did not match any file(s) known to git", result.message);
	ASSERT_EQ(1U, messages.size());
	EXPECT_EQ(result.message, messages[0]);
}

TEST(GitStageCommands, SplitsALongPathListAcrossInvocations)
{
	StageGit git;
	auto context = MakeStageContext(git);

	std::vector<GitStageResource> resources;
	for (int index = 0; index < 200; ++index) {
		resources.push_back(ChangedRow(L"folder/file" + std::to_wstring(index) + L".txt"));
	}

	EXPECT_TRUE(RunGitStage(context, resources).Succeeded());
	// The runner caps the argument count, so 200 paths cannot be one command.
	EXPECT_GT(git.invocations.size(), 1U);

	std::vector<std::wstring> seen;
	for (const auto& arguments : git.invocations) {
		ASSERT_GT(arguments.size(), 3U);
		EXPECT_EQ(L"add", arguments[0]);
		// Every chunk repeats the `--` terminator, so no path in any chunk can
		// be read as an option.
		EXPECT_EQ(L"--", arguments[2]);
		seen.insert(seen.end(), arguments.begin() + 3, arguments.end());
	}
	EXPECT_EQ(resources.size(), seen.size());
	EXPECT_EQ(L"folder/file0.txt", seen.front());
	EXPECT_EQ(L"folder/file199.txt", seen.back());
}

TEST(GitStageCommands, StopsAtTheFirstFailingChunk)
{
	StageGit git;
	auto context = MakeStageContext(git);
	context.run = [&git](const std::vector<std::wstring>& arguments) {
		auto result = git(arguments);
		if (git.invocations.size() == 1U) {
			result.status = EGitExecutionStatus::Failed;
			result.exitCode = 1;
			result.standardError = "fatal: bad pathspec\n";
		}
		return result;
	};

	std::vector<GitStageResource> resources;
	for (int index = 0; index < 200; ++index) {
		resources.push_back(ChangedRow(L"folder/file" + std::to_wstring(index) + L".txt"));
	}

	EXPECT_EQ(EGitStageCommandStatus::Failed, RunGitStage(context, resources).status);
	EXPECT_EQ(1U, git.invocations.size());
}

TEST(GitStageCommands, ChunkLimitsReserveWhatTheRunnerPrepends)
{
	const auto limits = GitPathChunkLimits::ForRepository(LR"(C:\repo)");
	// `BuildEffectiveGitArguments` adds `-C <root>` after the chunk is built, so
	// a chunk that exactly filled the raw limits would be rejected by the runner.
	EXPECT_LT(limits.maximumArguments, kMaximumGitArguments);
	EXPECT_LT(limits.maximumCommandLineLength, kMaximumGitCommandLineLength);
}

TEST(GitStageCommands, AnOversizedSinglePathStillGetsItsOwnInvocation)
{
	GitPathChunkLimits limits;
	limits.maximumArguments = 8;
	limits.maximumCommandLineLength = 16;

	const auto chunks = BuildGitPathChunks({ L"add", L"-A", L"--" }, { std::wstring(4096, L'x') }, limits);
	// Truncating it would silently change which file is operated on, so the
	// oversized request is left intact to fail visibly at the runner.
	ASSERT_EQ(1U, chunks.size());
	EXPECT_EQ(4U, chunks[0].size());
}

TEST(GitStageCommands, JoinsRepositoryPathsInTheNativeForm)
{
	// Porcelain reports `/`; the Recycle Bin call wants the native separator.
	EXPECT_EQ(LR"(C:\repo\src\a.txt)", JoinRepositoryPath(LR"(C:\repo)", L"src/a.txt"));
	EXPECT_EQ(LR"(C:\repo\a.txt)", JoinRepositoryPath(LR"(C:\repo\)", L"/a.txt"));
}

TEST(GitStageCommands, RoundTripsEveryFieldOfAnArgumentsPayload)
{
	// The group is half of the operand's identity and the two status flags decide
	// which git command a row reaches, so all four must survive the trip.
	const std::vector<GitStageResource> resources{
		{ L"src/a.cpp", EGitResourceGroup::WorkingTree, false, false },
		{ L"src/a.cpp", EGitResourceGroup::Index, false, true },
		{ L"new.txt", EGitResourceGroup::Untracked, true, false },
		{ L"conflict.txt", EGitResourceGroup::Merge, false, false },
	};

	const auto payload = BuildGitStageArguments(resources);
	const auto parsed = ParseGitStageArguments(payload);
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(resources, *parsed);
}

TEST(GitStageCommands, EscapesPathsThatJsonCannotCarryLiterally)
{
	const std::vector<GitStageResource> resources{
		{ LR"(dir "quoted"\back\slash.txt)", EGitResourceGroup::WorkingTree, false, false },
		// Two-, three-, and four-byte UTF-8 (the last a surrogate pair in UTF-16),
		// so the codec is proven over the whole range a real path can hold rather
		// than over ASCII alone. Both projects compile with `/source-charset:utf-8`.
		{ L"é/日本語/\U0001F600.txt", EGitResourceGroup::Index, false, false },
	};

	const auto parsed = ParseGitStageArguments(BuildGitStageArguments(resources));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(resources, *parsed);
}

TEST(GitStageCommands, TreatsAnAbsentPayloadAsAnEmptySelection)
{
	// This is the two-argument `Execute` overload's invocation, not a malformed
	// payload: the commands then report `NotApplicable` rather than failing.
	for (const std::string_view empty : { std::string_view{}, std::string_view{ "   \t\r\n " } }) {
		const auto parsed = ParseGitStageArguments(empty);
		ASSERT_TRUE(parsed.has_value());
		EXPECT_TRUE(parsed->empty());
	}
	const auto emptyArray = ParseGitStageArguments("[]");
	ASSERT_TRUE(emptyArray.has_value());
	EXPECT_TRUE(emptyArray->empty());
}

TEST(GitStageCommands, RejectsEveryMalformedArgumentsPayload)
{
	// A partially parsed selection would stage or discard a set the user never
	// chose, so each of these is nothing rather than a shorter list.
	constexpr std::string_view payloads[]{
		R"({"group":"index","path":"a.txt"})",                       // not an array
		R"([{"group":"staged","path":"a.txt"}])",                    // group token we do not publish
		R"([{"group":"index","path":"a.txt","extra":1}])",           // unknown key
		R"([{"group":"index"}])",                                    // no path
		R"([{"path":"a.txt"}])",                                     // no group
		R"([{}])",                                                   // neither
		R"([{"group":"index","path":""}])",                          // empty path
		R"([{"group":"index","path":"a.txt","untracked":"yes"}])",   // flag is not a boolean
		R"([{"group":"index","path":"a.txt"}] trailing)",            // text after the array
		R"([{"group":"index","path":"a.txt"})",                      // unterminated array
		R"([{"group":"index","path":"a.txt"},])",                    // trailing comma
	};
	for (const auto& payload : payloads) {
		EXPECT_FALSE(ParseGitStageArguments(payload).has_value()) << payload;
	}
}

TEST(GitStageCommands, RejectsMoreRowsThanTheBoundAllows)
{
	std::vector<GitStageResource> resources;
	for (std::size_t index = 0; index <= kMaximumGitStageArgumentResources; ++index) {
		resources.push_back(ChangedRow(L"file" + std::to_wstring(index) + L".txt"));
	}
	// One over the bound fails closed; exactly the bound still parses, so the
	// limit guards allocation without rejecting a selection it accepts elsewhere.
	EXPECT_FALSE(ParseGitStageArguments(BuildGitStageArguments(resources)).has_value());
	resources.pop_back();
	const auto parsed = ParseGitStageArguments(BuildGitStageArguments(resources));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(kMaximumGitStageArgumentResources, parsed->size());
}

TEST(GitStageCommands, GroupTokensAgreeWithThePublishedGroupIds)
{
	// `GitStageCommands.h` restates these rather than including the publisher,
	// whose own include would point it at the layer it must stay free of. This is
	// the assertion that keeps the two restatements from drifting apart.
	EXPECT_EQ(kGitMergeGroupId, kGitStageGroupTokenMerge);
	EXPECT_EQ(kGitIndexGroupId, kGitStageGroupTokenIndex);
	EXPECT_EQ(kGitWorkingTreeGroupId, kGitStageGroupTokenWorkingTree);
	EXPECT_EQ(kGitUntrackedGroupId, kGitStageGroupTokenUntracked);
}

//! One expected operand row, spelled out in full.
GitStageResource Row(std::wstring path, EGitResourceGroup group, bool untracked, bool deleted)
{
	return { std::move(path), group, untracked, deleted };
}

TEST(GitScmPublisher, CollectsOneStageResourcePerRenderedRow)
{
	auto state = RepositoryState();
	state.changes = {
		Change(L"both.txt", L'M', L'M'),
		Untracked(L"new.txt"),
		Conflicted(L"conflict.txt", L'U', L'U'),
	};

	const auto resources = CollectGitStageResources(state);
	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	// A row the operand list names is exactly a row the view renders, so the two
	// counts are the same number derived from the same classification.
	std::size_t rows = 0;
	for (const auto& group : publication.provider.groups) rows += group.resources.size();
	ASSERT_EQ(rows, resources.size());

	// `both.txt` occupies Staged Changes *and* Changes, which is what makes
	// "stage all" and "unstage all" name different sets for the same file.
	ASSERT_EQ(4U, resources.size());
	EXPECT_EQ(Row(L"both.txt", EGitResourceGroup::Index, false, false), resources[0]);
	EXPECT_EQ(Row(L"both.txt", EGitResourceGroup::WorkingTree, false, false), resources[1]);
	// Under the default `mixed` policy an untracked file is listed in Changes,
	// so its group is WorkingTree while its status is still untracked.
	EXPECT_EQ(Row(L"new.txt", EGitResourceGroup::WorkingTree, true, false), resources[2]);
	EXPECT_EQ(Row(L"conflict.txt", EGitResourceGroup::Merge, false, false), resources[3]);
}

TEST(GitScmPublisher, MarksDeletionPerRowRatherThanPerFile)
{
	auto state = RepositoryState();
	// Staged as an addition, then deleted from the worktree: the same file, two
	// rows, and only the second one is a deletion.
	state.changes = { Change(L"gone.txt", L'A', L'D'), Change(L"removed.txt", L'D', L'.') };

	const auto resources = CollectGitStageResources(state);
	ASSERT_EQ(3U, resources.size());
	EXPECT_EQ(EGitResourceGroup::Index, resources[0].group);
	EXPECT_FALSE(resources[0].deleted);
	EXPECT_EQ(EGitResourceGroup::WorkingTree, resources[1].group);
	EXPECT_TRUE(resources[1].deleted);
	EXPECT_EQ(EGitResourceGroup::Index, resources[2].group);
	EXPECT_TRUE(resources[2].deleted);
}

TEST(GitScmPublisher, CollectsSeparateUntrackedRowsUnderTheSeparatePolicy)
{
	auto state = RepositoryState();
	state.changes = { Untracked(L"new.txt") };

	const auto separate = CollectGitStageResources(state, EUntrackedChangesPolicy::Separate);
	ASSERT_EQ(1U, separate.size());
	EXPECT_EQ(EGitResourceGroup::Untracked, separate[0].group);
	EXPECT_TRUE(separate[0].untracked);
	// Hidden renders no row at all, so there is no operand either.
	EXPECT_TRUE(CollectGitStageResources(state, EUntrackedChangesPolicy::Hidden).empty());
}

TEST(GitScmPublisher, CollectsNothingWithoutARepository)
{
	GitScmState state;
	state.changes = { Change(L"a.txt", L'.', L'M') };
	// "No repository here" retracts the provider, so there is no rendered row for
	// a stale change list to become an operand of.
	EXPECT_TRUE(CollectGitStageResources(state).empty());
}

TEST(GitScmPublisher, GivesEveryPublishedRowItsOwnOperand)
{
	auto state = RepositoryState();
	state.changes = {
		Change(L"both.txt", L'M', L'M'),
		Untracked(L"new.txt"),
		Conflicted(L"conflict.txt", L'U', L'U'),
	};

	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	// The side table is one entry per *row*, not per file: the decoration table
	// above is keyed by URI and legitimately holds fewer entries.
	std::size_t rows = 0;
	for (const auto& group : publication.provider.groups) rows += group.resources.size();
	ASSERT_EQ(rows, publication.operands.size());
	EXPECT_EQ(3U, publication.decorations.size());

	// Every rendered row must find its operand through the (uri, group) key the
	// view joins on, and both halves are required: the same URI appears in two
	// groups, and those two rows stage and unstage different things. Position is
	// not the contract — the operand list follows the change walk, while the
	// groups are four separate lists.
	for (const auto& group : publication.provider.groups) {
		const auto kind = ParseGitResourceGroupId(group.id);
		ASSERT_TRUE(kind.has_value());
		for (const auto& resource : group.resources) {
			const auto uri = resource.resourceUri.ToString();
			const auto found = std::find_if(publication.operands.begin(), publication.operands.end(),
				[&uri, &kind](const GitResourceOperand& candidate) {
					return candidate.resource.group == *kind && candidate.resourceUri == uri;
				});
			EXPECT_NE(publication.operands.end(), found);
		}
	}

	// One derivation, two consumers. A second walk that disagreed by one row
	// would let a menu name a file the view is not showing.
	const auto collected = CollectGitStageResources(state);
	ASSERT_EQ(collected.size(), publication.operands.size());
	for (std::size_t i = 0; i < collected.size(); ++i) {
		EXPECT_EQ(collected[i], publication.operands[i].resource);
	}
}

//! The diff row published for one `(path, group)` pair, or nothing.
const GitDiffRow* FindDiffRow(const std::vector<GitDiffRowEntry>& rows,
	std::wstring_view path, EGitResourceGroup group)
{
	const auto found = std::find_if(rows.begin(), rows.end(),
		[path, group](const GitDiffRowEntry& entry) {
			return entry.group == group && entry.row.path == path;
		});
	return found == rows.end() ? nullptr : &found->row;
}

TEST(GitScmPublisher, CollectsOneDiffRowPerRenderedRow)
{
	auto state = RepositoryState();
	state.changes = {
		Change(L"both.txt", L'M', L'M'),
		Untracked(L"new.txt"),
		Conflicted(L"conflict.txt", L'U', L'U'),
	};

	const auto rows = CollectGitDiffRows(state);
	const auto publication = BuildGitPublication(Owner(), LR"(C:\repo)", state);
	// One derivation, three consumers. A row the diff resolver sees is exactly a
	// row the view renders and a row a menu can name.
	std::size_t rendered = 0;
	for (const auto& group : publication.provider.groups) rendered += group.resources.size();
	ASSERT_EQ(rendered, rows.size());
	ASSERT_EQ(CollectGitStageResources(state).size(), rows.size());

	// Each row carries its own area's status, because the Staged Changes row
	// compares HEAD with the index while the Changes row compares the index with
	// the worktree.
	ASSERT_EQ(4U, rows.size());
	EXPECT_EQ(EGitResourceGroup::Index, rows[0].group);
	EXPECT_EQ(EGitFileStatus::IndexModified, rows[0].row.status);
	EXPECT_EQ(EGitResourceGroup::WorkingTree, rows[1].group);
	EXPECT_EQ(EGitFileStatus::Modified, rows[1].row.status);
	EXPECT_EQ(EGitFileStatus::Untracked, rows[2].row.status);
	EXPECT_EQ(EGitResourceGroup::Merge, rows[3].group);
	EXPECT_EQ(EGitFileStatus::BothModified, rows[3].row.status);
}

TEST(GitScmPublisher, AnUnstagedEditIsComparedAgainstWhicheverSideHoldsTheStagedWork)
{
	auto state = RepositoryState();
	// `both.txt` is staged *and* edited again; `only.txt` is edited with nothing
	// staged. Upstream's `sanitizeRef('~')` searches the index group for the
	// path, so this is one fact per change rather than one per row.
	state.changes = { Change(L"both.txt", L'M', L'M'), Change(L"only.txt", L'.', L'M') };
	const auto rows = CollectGitDiffRows(state);

	const auto* staged = FindDiffRow(rows, L"both.txt", EGitResourceGroup::WorkingTree);
	ASSERT_NE(nullptr, staged);
	EXPECT_TRUE(staged->stagedInIndex);
	const auto stagedInput = ResolveGitDiffInput(*staged);
	ASSERT_TRUE(stagedInput.original.has_value());
	// The empty ref is the index: without it the diff would silently include the
	// work the user already staged.
	EXPECT_EQ(L""s, stagedInput.original->ref);

	const auto* plain = FindDiffRow(rows, L"only.txt", EGitResourceGroup::WorkingTree);
	ASSERT_NE(nullptr, plain);
	EXPECT_FALSE(plain->stagedInIndex);
	const auto plainInput = ResolveGitDiffInput(*plain);
	ASSERT_TRUE(plainInput.original.has_value());
	EXPECT_EQ(L"HEAD"s, plainInput.original->ref);

	// The Staged Changes row of the same file is a different comparison, and it
	// does not read the flag at all.
	const auto* index = FindDiffRow(rows, L"both.txt", EGitResourceGroup::Index);
	ASSERT_NE(nullptr, index);
	const auto indexInput = ResolveGitDiffInput(*index);
	ASSERT_TRUE(indexInput.original.has_value());
	EXPECT_EQ(L"HEAD"s, indexInput.original->ref);
}

TEST(GitScmPublisher, ADiffRowKeepsThePreRenameNameHeadKnowsTheContentUnder)
{
	auto state = RepositoryState();
	auto renamed = Change(L"new.txt", L'R', L'.');
	renamed.originalPath = L"old.txt";
	state.changes = { renamed };

	const auto rows = CollectGitDiffRows(state);
	ASSERT_EQ(1U, rows.size());
	EXPECT_EQ(L"new.txt"s, rows[0].row.path);
	EXPECT_EQ(L"old.txt"s, rows[0].row.originalPath);
	const auto input = ResolveGitDiffInput(rows[0].row);
	ASSERT_TRUE(input.original.has_value());
	EXPECT_EQ(L"old.txt"s, input.original->path);
}

TEST(GitScmPublisher, CollectsNoDiffRowForAThingTheViewDoesNotRender)
{
	auto state = RepositoryState();
	state.changes = { Untracked(L"new.txt") };

	const auto separate = CollectGitDiffRows(state, EUntrackedChangesPolicy::Separate);
	ASSERT_EQ(1U, separate.size());
	EXPECT_EQ(EGitResourceGroup::Untracked, separate[0].group);
	// Hidden renders no row, so there is nothing to open a diff on.
	EXPECT_TRUE(CollectGitDiffRows(state, EUntrackedChangesPolicy::Hidden).empty());

	// "No repository here" retracts the provider, so a stale change list must not
	// become a diff the user can open.
	GitScmState none;
	none.changes = { Change(L"a.txt", L'.', L'M') };
	EXPECT_TRUE(CollectGitDiffRows(none).empty());
}

TEST(GitScmMenus, ReproducesUpstreamsResourceStateContributionPerGroup)
{
	// `navigation` first, then `1_modification`; within one group upstream sorts
	// equal orders by title, so Discard precedes Stage and Open Changes precedes
	// Open File.
	//
	// Merge Changes is the one group whose `navigation` upstream gives no
	// `git.openChange` at all: a conflicted file is opened, not compared.
	const std::vector<GitMenuItem> merge = {
		{ "git.openFile", L"Open File", false },
		{ {}, {}, true },
		{ "git.stage", L"Stage Changes", false },
	};
	EXPECT_EQ(merge, BuildGitResourceContextMenu(EGitResourceGroup::Merge));

	const std::vector<GitMenuItem> index = {
		{ "git.openChange", L"Open Changes", false },
		{ "git.openFile", L"Open File", false },
		{ {}, {}, true },
		{ "git.unstage", L"Unstage Changes", false },
	};
	EXPECT_EQ(index, BuildGitResourceContextMenu(EGitResourceGroup::Index));

	const std::vector<GitMenuItem> workingTree = {
		{ "git.openChange", L"Open Changes", false },
		{ "git.openFile", L"Open File", false },
		{ {}, {}, true },
		{ "git.clean", L"Discard Changes", false },
		{ "git.stage", L"Stage Changes", false },
	};
	EXPECT_EQ(workingTree, BuildGitResourceContextMenu(EGitResourceGroup::WorkingTree));
	EXPECT_EQ(workingTree, BuildGitResourceContextMenu(EGitResourceGroup::Untracked));
}

TEST(GitScmMenus, OmitsEveryUpstreamEntryThatHasNoRouteHere)
{
	// An unroutable entry is absent, never rendered disabled and never
	// approximated by a different command. Each of these is contributed by
	// upstream for one of the groups below.
	const std::vector<std::string> unroutable = {
		"git.openHEADFile", "git.openFile2", "git.ignore",
		"git.revealInExplorer", "git.revealFileInOS.windows", "git.compareWithWorkspace",
	};
	for (const auto group : { EGitResourceGroup::Merge, EGitResourceGroup::Index,
			EGitResourceGroup::WorkingTree, EGitResourceGroup::Untracked }) {
		for (const auto& item : BuildGitResourceContextMenu(group)) {
			EXPECT_EQ(unroutable.end(), std::find(unroutable.begin(), unroutable.end(), item.commandId));
			// A separator carries neither id nor title; everything else carries both.
			EXPECT_EQ(item.separator, item.commandId.empty());
			EXPECT_EQ(item.separator, item.title.empty());
		}
	}
}

TEST(GitScmMenus, TitlesAreUpstreamsBareStringsNotTheCommandPalettePrefixedOnes)
{
	// `WorkbenchCommandDescriptor::title` is `Git: Stage Changes`, which is the
	// Command Palette's form. A menu that reused it would read wrong in place.
	for (const auto& item : BuildGitResourceContextMenu(EGitResourceGroup::WorkingTree)) {
		EXPECT_EQ(std::wstring::npos, item.title.find(L"Git: "));
	}
}

TEST(GitScmMenus, ReproducesUpstreamsResourceGroupContributionUnderTheDefaultPolicy)
{
	// Only `git.unstageAll` is contributed for `index`, and it is registered here.
	const std::vector<GitMenuItem> index = { { "git.unstageAll", L"Unstage All Changes", false } };
	EXPECT_EQ(index, BuildGitResourceGroupContextMenu(EGitResourceGroup::Index, EUntrackedChangesPolicy::Mixed));

	// Upstream's `mixed` branch, sorted by title at equal order.
	const std::vector<GitMenuItem> workingTree = {
		{ "git.cleanAll", L"Discard All Changes", false },
		{ "git.stageAll", L"Stage All Changes", false },
	};
	EXPECT_EQ(workingTree,
		BuildGitResourceGroupContextMenu(EGitResourceGroup::WorkingTree, EUntrackedChangesPolicy::Mixed));

	// `git.stageAllMerge`, and the `*Tracked` / `*Untracked` variants, are not
	// registered. An empty model is how the caller learns to show no menu at all
	// rather than a popup whose entries would silently do nothing.
	EXPECT_TRUE(BuildGitResourceGroupContextMenu(EGitResourceGroup::Merge, EUntrackedChangesPolicy::Mixed).empty());
	EXPECT_TRUE(BuildGitResourceGroupContextMenu(EGitResourceGroup::Untracked, EUntrackedChangesPolicy::Separate).empty());
	EXPECT_TRUE(BuildGitResourceGroupContextMenu(EGitResourceGroup::WorkingTree, EUntrackedChangesPolicy::Separate).empty());
}

TEST(GitScmMenus, RecognizesOnlyTheBuiltInGitGroupIds)
{
	EXPECT_EQ(EGitResourceGroup::Merge, ParseGitResourceGroupId(kGitMergeGroupId));
	EXPECT_EQ(EGitResourceGroup::Index, ParseGitResourceGroupId(kGitIndexGroupId));
	EXPECT_EQ(EGitResourceGroup::WorkingTree, ParseGitResourceGroupId(kGitWorkingTreeGroupId));
	EXPECT_EQ(EGitResourceGroup::Untracked, ParseGitResourceGroupId(kGitUntrackedGroupId));
	// An extension-contributed provider's group is not a Git group, and giving it
	// Git's menu would offer to stage something Git does not own.
	EXPECT_FALSE(ParseGitResourceGroupId("changes").has_value());
	EXPECT_FALSE(ParseGitResourceGroupId("").has_value());
	EXPECT_FALSE(ParseGitResourceGroupId("WorkingTree").has_value());
}

//! A throwaway git metadata directory. `ReadInProgressState` probes `MERGE_HEAD`
//! and the rebase state directories with `GetFileAttributesW`, which no injected
//! callable can intercept, so the in-progress cases need real files.
class TemporaryGitDirectory final {
public:
	explicit TemporaryGitDirectory(const wchar_t* name)
	{
		m_path = std::filesystem::temp_directory_path() / L"sakura-git-commit-test" / name;
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
		std::filesystem::create_directories(m_path);
	}
	TemporaryGitDirectory(const TemporaryGitDirectory&) = delete;
	TemporaryGitDirectory& operator=(const TemporaryGitDirectory&) = delete;
	~TemporaryGitDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	void Touch(const wchar_t* name) const { std::ofstream(m_path / name) << "x"; }
	void MakeDirectory(const wchar_t* name) const { std::filesystem::create_directories(m_path / name); }
	[[nodiscard]] std::wstring Path() const { return m_path.wstring(); }

private:
	std::filesystem::path m_path;
};

std::string NarrowGitOutput(std::wstring_view text)
{
	if (text.empty()) return {};
	const int required = ::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
	return result;
}

//!
//! @brief Records every commit-path invocation and answers the probes.
//!
//! The probes (`rev-parse`, `config`, `rev-list`, `log`) answer regardless of the
//! failure switch, because a test about a failing commit must still be able to
//! reach the commit. Responses are selected by a token the command contains
//! rather than by `arguments[0]`: `requireUserConfig` splices
//! `-c user.useConfigOnly=true` in front, so the verb is not always first.
//!
class CommitGit final {
public:
	struct Invocation final {
		std::vector<std::wstring> arguments;
		std::string standardInput;
	};

	std::vector<Invocation> invocations;
	//! `rev-parse --absolute-git-dir`. Empty fails the probe, which is how a
	//! repository with neither a merge nor a rebase in progress reads here.
	std::wstring gitDirectory;
	//! `config --get-all user.name` / `user.email`, which upstream consults only
	//! after a commit has already failed.
	bool identityConfigured = true;
	//! `rev-list --parents -n 1 HEAD`, verbatim. Two tokens is one parent.
	std::string revList = "0000000 1111111\n";
	bool revListFails = false;
	//! `log -1 --format=%B`.
	std::string headMessage = "previous message\n";
	//! The token identifying the one command that fails, e.g. `commit` or `add`.
	std::wstring failingCommand;
	std::string failingStandardError;

	GitExecutionResult operator()(const std::vector<std::wstring>& arguments, std::string_view standardInput)
	{
		invocations.push_back({ arguments, std::string(standardInput) });
		GitExecutionResult result;
		result.status = EGitExecutionStatus::Succeeded;
		result.exitCode = 0;
		const auto has = [&arguments](std::wstring_view token) {
			return std::find(arguments.begin(), arguments.end(), token) != arguments.end();
		};
		const auto emit = [&result](const std::string& bytes) {
			result.standardOutput.assign(bytes.begin(), bytes.end());
		};
		if (has(L"rev-parse")) {
			if (gitDirectory.empty()) {
				result.exitCode = 1;
			} else {
				emit(NarrowGitOutput(gitDirectory) + "\n");
			}
			return result;
		}
		if (has(L"config")) {
			emit(identityConfigured ? "Someone\n" : "");
			return result;
		}
		if (has(L"rev-list")) {
			if (revListFails) {
				result.exitCode = 1;
			} else {
				emit(revList);
			}
			return result;
		}
		if (has(L"log")) {
			emit(headMessage);
			return result;
		}
		if (!failingCommand.empty() && has(failingCommand)) {
			result.status = EGitExecutionStatus::Failed;
			result.exitCode = 1;
			result.standardError = failingStandardError;
		}
		return result;
	}

	//! Every invocation except the probes above, which are bookkeeping rather
	//! than an operation the user asked for.
	[[nodiscard]] std::vector<std::vector<std::wstring>> Operations() const
	{
		std::vector<std::vector<std::wstring>> operations;
		for (const auto& invocation : invocations) {
			const auto& arguments = invocation.arguments;
			const auto has = [&arguments](std::wstring_view token) {
				return std::find(arguments.begin(), arguments.end(), token) != arguments.end();
			};
			if (has(L"rev-parse") || has(L"config") || has(L"rev-list") || has(L"log")) continue;
			operations.push_back(arguments);
		}
		return operations;
	}

	//! The stdin of the one `git commit`, which is where the message travels.
	[[nodiscard]] std::string CommitStandardInput() const
	{
		for (const auto& invocation : invocations) {
			const auto& arguments = invocation.arguments;
			if (std::find(arguments.begin(), arguments.end(), L"commit") != arguments.end()) {
				return invocation.standardInput;
			}
		}
		return {};
	}
};

GitCommitCommandContext MakeCommitContext(CommitGit& git)
{
	GitCommitCommandContext context;
	context.repositoryRoot = LR"(C:\repo)";
	context.run = [&git](const std::vector<std::wstring>& arguments, std::string_view standardInput) {
		return git(arguments, standardInput);
	};
	return context;
}

//! Answers a commit confirmation with a fixed choice and records the prompt.
GitCommitConfirmationPresenter ChooseCommit(
	std::optional<std::size_t> index, std::vector<GitCommitPrompt>* prompts = nullptr)
{
	return [index, prompts](const GitCommitPrompt& prompt) -> std::optional<std::size_t> {
		if (prompts != nullptr) {
			prompts->push_back(prompt);
		}
		return index;
	};
}

//! A repository whose HEAD is `main` and already has a commit. The staged paths
//! are real absolute paths, because the unsaved-document filter joins on them.
GitCommitRepositoryState CommitState(std::size_t stagedCount, std::size_t workingTreeCount)
{
	GitCommitRepositoryState state;
	state.stagedCount = stagedCount;
	state.workingTreeCount = workingTreeCount;
	state.headHasCommit = true;
	state.headShortName = L"main";
	for (std::size_t index = 0; index < stagedCount; ++index) {
		state.stagedPaths.push_back(LR"(C:\repo\staged)" + std::to_wstring(index) + L".txt");
	}
	return state;
}

//! The exact `git commit` upstream builds for a plain message under the default
//! configuration, spelled once so every flow test asserts the same command.
std::vector<std::wstring> PlainCommit()
{
	return { L"-c", L"user.useConfigOnly=true", L"commit", L"--quiet",
		L"--allow-empty-message", L"--file", L"-", L"--allow-empty-message" };
}

TEST(GitCommitCommands, AMessageTravelsOnStdinAndAllowEmptyMessageIsEmittedTwice)
{
	const auto invocation = BuildGitCommitInvocation(L"hello", {});
	// Upstream appends `--allow-empty-message` once for the message and a second
	// time on its `!useEditor` branch. Emitting it once would be tidier and would
	// also be a different command line.
	EXPECT_EQ(PlainCommit(), invocation.arguments);
	EXPECT_EQ("hello", invocation.standardInput);
	EXPECT_TRUE(invocation.writesStandardInput);
}

TEST(GitCommitCommands, NoMessageOpensStdinAsAnEmptyFileRatherThanAnEditor)
{
	const auto invocation = BuildGitCommitInvocation(L"", {});
	EXPECT_EQ((std::vector<std::wstring>{ L"-c", L"user.useConfigOnly=true", L"commit", L"--quiet",
		L"--file", L"-", L"--allow-empty-message" }), invocation.arguments);
	// stdin is still written and closed, which is what keeps git from opening the
	// editor this build cannot host.
	EXPECT_TRUE(invocation.writesStandardInput);
	EXPECT_TRUE(invocation.standardInput.empty());
}

TEST(GitCommitCommands, AmendWithNoMessageKeepsThePreviousOneWithNoEdit)
{
	GitCommitOptions options;
	options.amend = true;
	const auto invocation = BuildGitCommitInvocation(L"", options);
	EXPECT_EQ((std::vector<std::wstring>{ L"-c", L"user.useConfigOnly=true", L"commit", L"--quiet",
		L"--amend", L"--no-edit", L"--allow-empty-message" }), invocation.arguments);
	EXPECT_FALSE(invocation.writesStandardInput);
}

TEST(GitCommitCommands, EveryOptionKeepsUpstreamsOwnArgumentOrder)
{
	GitCommitOptions options;
	options.requireUserConfig = false;
	options.signoff = true;
	options.signCommit = false;
	options.empty = true;
	options.noVerify = true;
	const auto invocation = BuildGitCommitInvocation(L"m", options);
	// `--no-gpg-sign` is what `signCommit = false` means; saying nothing about
	// signing is a third state, and it is `std::nullopt`.
	EXPECT_EQ((std::vector<std::wstring>{ L"commit", L"--quiet", L"--allow-empty-message", L"--file", L"-",
		L"--allow-empty-message", L"--signoff", L"--no-gpg-sign", L"--allow-empty", L"--no-verify" }),
		invocation.arguments);

	options.signCommit = true;
	EXPECT_EQ((std::vector<std::wstring>{ L"commit", L"--quiet", L"--allow-empty-message", L"--file", L"-",
		L"--allow-empty-message", L"--signoff", L"-S", L"--allow-empty", L"--no-verify" }),
		BuildGitCommitInvocation(L"m", options).arguments);

	// Saying nothing about signing is a third state, distinct from both of the
	// above, and it must emit neither flag rather than defaulting to one.
	options.signCommit.reset();
	EXPECT_EQ((std::vector<std::wstring>{ L"commit", L"--quiet", L"--allow-empty-message", L"--file", L"-",
		L"--allow-empty-message", L"--signoff", L"--allow-empty", L"--no-verify" }),
		BuildGitCommitInvocation(L"m", options).arguments);
}

TEST(GitCommitCommands, TheEditorPathNeitherWritesStdinNorRepeatsAllowEmptyMessage)
{
	GitCommitOptions options;
	options.useEditor = true;
	options.verbose = true;
	const auto invocation = BuildGitCommitInvocation(L"", options);
	EXPECT_EQ((std::vector<std::wstring>{ L"-c", L"user.useConfigOnly=true", L"commit", L"--quiet",
		L"--verbose" }), invocation.arguments);
	EXPECT_FALSE(invocation.writesStandardInput);
}

TEST(GitCommitCommands, RepositoryStateCountsGroupsSeparatelyAndKeepsStagedPathsAbsolute)
{
	const std::vector<GitStageResource> resources = {
		StagedRow(L"a.txt"), ChangedRow(L"b.txt"), UntrackedRow(L"c.txt"),
		GitStageResource{ L"d.txt", EGitResourceGroup::Merge, false, false },
	};

	const auto state = BuildGitCommitRepositoryState(LR"(C:\repo)", resources, L"main", true);
	EXPECT_EQ(1U, state.stagedCount);
	// The unsaved-document filter compares absolute paths, so the staged set's
	// identity - not merely its size - has to survive this derivation.
	EXPECT_EQ((std::vector<std::wstring>{ LR"(C:\repo\a.txt)" }), state.stagedPaths);
	// A Merge row is neither staged nor a working-tree change; it is a conflict.
	EXPECT_EQ(2U, state.workingTreeCount);
	EXPECT_FALSE(state.workingTreeAllUntracked);
	EXPECT_TRUE(state.headHasCommit);
	EXPECT_EQ(L"main", state.headShortName);
}

TEST(GitCommitCommands, EveryWorkingTreeRowUntrackedIsADistinctFactFromHavingNone)
{
	EXPECT_TRUE(BuildGitCommitRepositoryState(
		LR"(C:\repo)", { UntrackedRow(L"c.txt") }, L"main", true).workingTreeAllUntracked);
	// No rows at all is not "all of them are untracked": upstream's
	// `smartCommitChanges === 'tracked'` no-op condition requires rows to skip.
	EXPECT_FALSE(BuildGitCommitRepositoryState(LR"(C:\repo)", {}, L"main", true).workingTreeAllUntracked);
}

TEST(GitCommitCommands, PromptsCarryUpstreamsOwnStringsAndSeverity)
{
	const auto one = BuildUnsavedDocumentsPrompt({ LR"(C:\repo\a.txt)" });
	EXPECT_NE(std::wstring::npos, one.message.find(L"The following file has unsaved changes"));
	// A confirmation names the file, so the user knows what would be left out.
	EXPECT_NE(std::wstring::npos, one.message.find(L"a.txt"));
	EXPECT_EQ((std::vector<std::wstring>{ L"Save All & Commit Changes", L"Commit Changes" }), one.choices);

	const auto many = BuildUnsavedDocumentsPrompt({ LR"(C:\repo\a.txt)", LR"(C:\repo\b.txt)" });
	EXPECT_NE(std::wstring::npos, many.message.find(L"There are 2 unsaved files."));

	// The only informational, non-modal one of the set. Escalating it into a
	// warning would report "there is nothing to commit" as a hazard.
	const auto noChanges = BuildNoChangesPrompt();
	EXPECT_EQ(L"There are no changes to commit.", noChanges.message);
	EXPECT_EQ((std::vector<std::wstring>{ L"Create Empty Commit" }), noChanges.choices);
	EXPECT_FALSE(noChanges.warning);
	EXPECT_FALSE(noChanges.modal);

	// `Always` and `Never` write Settings upstream; there is no writer here, so
	// they are absent rather than present and inert.
	EXPECT_EQ((std::vector<std::wstring>{ L"Yes" }), BuildNoStagedChangesPrompt().choices);
	// Likewise `OK, Don't Ask Again`.
	EXPECT_EQ((std::vector<std::wstring>{ L"OK" }), BuildNoVerifyCommitPrompt().choices);
	EXPECT_TRUE(BuildNoVerifyCommitPrompt().modal);
	EXPECT_EQ((std::vector<std::wstring>{ L"Undo merge commit" }), BuildUndoMergeCommitPrompt().choices);
}

TEST(GitCommitCommands, CommitsTheInputBoxMessageAndThenClearsTheBox)
{
	CommitGit git;
	auto context = MakeCommitContext(git);

	const auto result = RunGitCommit(context, CommitState(1, 0), L"a message", {});
	EXPECT_EQ(EGitCommitCommandStatus::Succeeded, result.status);
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ(PlainCommit(), git.Operations()[0]);
	EXPECT_EQ("a message", git.CommitStandardInput());
	// `commitOperationCleanup` resets the box; an absent value would mean "leave
	// it alone", which would leave a committed message sitting in the field.
	ASSERT_TRUE(result.inputBoxValue.has_value());
	EXPECT_TRUE(result.inputBoxValue->empty());
}

TEST(GitCommitCommands, AnEmptyBoxPromptsForAMessageAndNamesTheBranch)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	std::wstring placeholder;
	std::wstring prompt;
	context.promptForMessage = [&](std::wstring_view p, std::wstring_view q) -> std::optional<std::wstring> {
		placeholder = p;
		prompt = q;
		return std::wstring(L"typed");
	};

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"", {}).Succeeded());
	// Which branch the commit lands on is the operational-safety half of this
	// prompt, so it is in the placeholder rather than only in the status bar.
	EXPECT_EQ(LR"(Message (commit on "main"))", placeholder);
	EXPECT_EQ(L"Please provide a commit message", prompt);
	EXPECT_EQ("typed", git.CommitStandardInput());
}

TEST(GitCommitCommands, ADetachedHeadFallsBackToTheBranchlessPlaceholder)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	std::wstring placeholder;
	context.promptForMessage = [&](std::wstring_view p, std::wstring_view) -> std::optional<std::wstring> {
		placeholder = p;
		return std::wstring(L"typed");
	};
	auto state = CommitState(1, 0);
	state.headShortName.clear();

	EXPECT_TRUE(RunGitCommit(context, state, L"", {}).Succeeded());
	EXPECT_EQ(L"Commit message", placeholder);
}

TEST(GitCommitCommands, DismissingTheMessagePromptCancelsAndLeavesTheBoxAlone)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	context.promptForMessage = [](std::wstring_view, std::wstring_view) { return std::nullopt; };

	const auto result = RunGitCommit(context, CommitState(1, 0), L"", {});
	EXPECT_EQ(EGitCommitCommandStatus::Cancelled, result.status);
	EXPECT_FALSE(result.inputBoxValue.has_value());
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, AnEmptyMessageWithNothingToAmendCommitsNothing)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	// An empty box is a dismissal; an empty *answer* is a real answer, and
	// upstream still refuses to commit on it unless amending.
	context.promptForMessage = [](std::wstring_view, std::wstring_view) { return std::wstring{}; };

	EXPECT_EQ(EGitCommitCommandStatus::NotApplicable, RunGitCommit(context, CommitState(1, 0), L"", {}).status);
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, AmendingOverAnExistingCommitNeedsNoMessageAtAll)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	bool prompted = false;
	context.promptForMessage = [&](std::wstring_view, std::wstring_view) {
		prompted = true;
		return std::wstring{};
	};
	GitCommitOptions options;
	options.amend = true;

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"", options).Succeeded());
	// Upstream deliberately yields no message here so `--amend --no-edit` keeps
	// the previous one; asking would invite the user to retype what git has.
	EXPECT_FALSE(prompted);
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ((std::vector<std::wstring>{ L"-c", L"user.useConfigOnly=true", L"commit", L"--quiet",
		L"--amend", L"--no-edit", L"--allow-empty-message" }), git.Operations()[0]);
}

TEST(GitCommitCommands, NothingStagedOffersToStageEverythingAndThenAddsTheWholeWorktree)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0, &prompts);

	EXPECT_TRUE(RunGitCommit(context, CommitState(0, 1), L"m", {}).Succeeded());
	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(BuildNoStagedChangesPrompt(), prompts[0]);
	ASSERT_EQ(2U, git.Operations().size());
	// `add([], …)` is `add -A -- .` upstream: the whole worktree, not a listing
	// of the rows the last refresh happened to know about.
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-A", L"--", L"." }), git.Operations()[0]);
	EXPECT_EQ(PlainCommit(), git.Operations()[1]);
}

TEST(GitCommitCommands, DecliningTheSmartCommitSuggestionCommitsNothing)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(std::nullopt);

	EXPECT_EQ(EGitCommitCommandStatus::Cancelled, RunGitCommit(context, CommitState(0, 1), L"m", {}).status);
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, SuggestSmartCommitOffReturnsEarlyWithoutAsking)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0, &prompts);
	context.configuration.suggestSmartCommit = false;

	EXPECT_EQ(EGitCommitCommandStatus::NotApplicable, RunGitCommit(context, CommitState(0, 1), L"m", {}).status);
	EXPECT_TRUE(prompts.empty());
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, NoChangesAtAllOffersAnEmptyCommitAndDeclineCommitsNothing)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(std::nullopt, &prompts);

	EXPECT_EQ(EGitCommitCommandStatus::NotApplicable, RunGitCommit(context, CommitState(0, 0), L"m", {}).status);
	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(BuildNoChangesPrompt(), prompts[0]);
	EXPECT_TRUE(git.Operations().empty());

	CommitGit accepted;
	auto acceptedContext = MakeCommitContext(accepted);
	acceptedContext.confirm = ChooseCommit(0);
	EXPECT_TRUE(RunGitCommit(acceptedContext, CommitState(0, 0), L"m", {}).Succeeded());
	ASSERT_EQ(1U, accepted.Operations().size());
	auto expected = PlainCommit();
	expected.push_back(L"--allow-empty");
	EXPECT_EQ(expected, accepted.Operations()[0]);
}

TEST(GitCommitCommands, UnsavedStagedDocumentsAreSavedAndRestagedBeforeCommitting)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0, &prompts);
	context.dirtyDocuments = []() { return std::vector<std::wstring>{ LR"(C:\repo\staged0.txt)" }; };
	bool saved = false;
	context.saveDocuments = [&saved]() { saved = true; return true; };

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"m", {}).Succeeded());
	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(BuildUnsavedDocumentsPrompt({ LR"(C:\repo\staged0.txt)" }), prompts[0]);
	EXPECT_TRUE(saved);
	ASSERT_EQ(2U, git.Operations().size());
	// Saving made the worktree differ from what was staged, so the newly written
	// bytes have to be re-added or the commit records the pre-save content.
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-A", L"--", LR"(C:\repo\staged0.txt)" }), git.Operations()[0]);
	EXPECT_EQ(PlainCommit(), git.Operations()[1]);
}

TEST(GitCommitCommands, CommittingWithoutSavingSkipsTheSaveAndTheRestage)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(1);
	context.dirtyDocuments = []() { return std::vector<std::wstring>{ LR"(C:\repo\staged0.txt)" }; };
	bool saved = false;
	context.saveDocuments = [&saved]() { saved = true; return true; };

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"m", {}).Succeeded());
	EXPECT_FALSE(saved);
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ(PlainCommit(), git.Operations()[0]);
}

TEST(GitCommitCommands, AFailedSaveStopsTheCommitRatherThanCommittingStaleBytes)
{
	CommitGit git;
	std::vector<std::wstring> messages;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0);
	context.message = [&messages](std::wstring_view message) { messages.emplace_back(message); };
	context.dirtyDocuments = []() { return std::vector<std::wstring>{ LR"(C:\repo\staged0.txt)" }; };
	context.saveDocuments = []() { return false; };

	const auto result = RunGitCommit(context, CommitState(1, 0), L"m", {});
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_FALSE(result.message.empty());
	EXPECT_EQ(1U, messages.size());
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, DismissingTheUnsavedPromptCancels)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(std::nullopt);
	context.dirtyDocuments = []() { return std::vector<std::wstring>{ LR"(C:\repo\staged0.txt)" }; };
	context.saveDocuments = []() { return true; };

	EXPECT_EQ(EGitCommitCommandStatus::Cancelled, RunGitCommit(context, CommitState(1, 0), L"m", {}).status);
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, WithSomethingStagedTheUnsavedPromptOnlyNamesStagedDocuments)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0, &prompts);
	// A dirty document that is not staged cannot be left out of this commit,
	// because it was never going into it.
	context.dirtyDocuments = []() { return std::vector<std::wstring>{ LR"(C:\repo\other.txt)" }; };
	context.saveDocuments = []() { return true; };

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"m", {}).Succeeded());
	EXPECT_TRUE(prompts.empty());
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ(PlainCommit(), git.Operations()[0]);
}

TEST(GitCommitCommands, NoVerifyIsRefusedUntilItsSettingAllowsIt)
{
	CommitGit git;
	std::vector<std::wstring> messages;
	auto context = MakeCommitContext(git);
	context.message = [&messages](std::wstring_view message) { messages.emplace_back(message); };
	GitCommitOptions options;
	options.noVerify = true;

	const auto result = RunGitCommit(context, CommitState(1, 0), L"m", options);
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	// The refusal names the setting that would allow it, so skipping the hooks
	// stays a deliberate act rather than a mystery.
	EXPECT_NE(std::wstring::npos, result.message.find(L"git.allowNoVerifyCommit"));
	EXPECT_EQ(1U, messages.size());
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, AnAllowedNoVerifyCommitStillConfirmsFirst)
{
	CommitGit git;
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(std::nullopt, &prompts);
	context.configuration.allowNoVerifyCommit = true;
	GitCommitOptions options;
	options.noVerify = true;

	EXPECT_EQ(EGitCommitCommandStatus::Cancelled, RunGitCommit(context, CommitState(1, 0), L"m", options).status);
	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(BuildNoVerifyCommitPrompt(), prompts[0]);
	EXPECT_TRUE(git.Operations().empty());

	CommitGit accepted;
	auto acceptedContext = MakeCommitContext(accepted);
	acceptedContext.confirm = ChooseCommit(0);
	acceptedContext.configuration.allowNoVerifyCommit = true;
	EXPECT_TRUE(RunGitCommit(acceptedContext, CommitState(1, 0), L"m", options).Succeeded());
	auto expected = PlainCommit();
	expected.push_back(L"--no-verify");
	ASSERT_EQ(1U, accepted.Operations().size());
	EXPECT_EQ(expected, accepted.Operations()[0]);
}

TEST(GitCommitCommands, AFailedCommitReportsGitsOwnReasonAndKeepsTheMessage)
{
	CommitGit git;
	git.failingCommand = L"commit";
	git.failingStandardError = "error: pathspec did not match\n";
	std::vector<std::wstring> messages;
	auto context = MakeCommitContext(git);
	context.message = [&messages](std::wstring_view message) { messages.emplace_back(message); };

	const auto result = RunGitCommit(context, CommitState(1, 0), L"m", {});
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_EQ(L"error: pathspec did not match", result.message);
	EXPECT_EQ(1U, messages.size());
	// An absent value is what keeps a failed commit from discarding the message
	// that failed, so the user can fix the cause and press commit again.
	EXPECT_FALSE(result.inputBoxValue.has_value());
}

TEST(GitCommitCommands, AFailedCommitWithNoConfiguredIdentitySaysSo)
{
	CommitGit git;
	git.failingCommand = L"commit";
	git.failingStandardError = "error: something\n";
	git.identityConfigured = false;
	auto context = MakeCommitContext(git);

	const auto result = RunGitCommit(context, CommitState(1, 0), L"m", {});
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_NE(std::wstring::npos, result.message.find(L"user.name"));
	EXPECT_NE(std::wstring::npos, result.message.find(L"user.email"));
}

TEST(GitCommitCommands, AnEmptyMessageAbortIsReportedAsTheCancellationItIs)
{
	CommitGit git;
	git.failingCommand = L"commit";
	git.failingStandardError = "Aborting commit due to empty commit message\n";
	// The identity probes must not be able to overwrite a reason git already
	// gave, which is why upstream matches stderr before asking about config.
	git.identityConfigured = false;
	auto context = MakeCommitContext(git);

	EXPECT_EQ(L"Commit operation was cancelled due to empty commit message.",
		RunGitCommit(context, CommitState(1, 0), L"m", {}).message);
}

TEST(GitCommitCommands, AFailedRestageStopsBeforeTheCommit)
{
	CommitGit git;
	git.failingCommand = L"add";
	git.failingStandardError = "fatal: bad pathspec\n";
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0);

	const auto result = RunGitCommit(context, CommitState(0, 1), L"m", {});
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_EQ(L"fatal: bad pathspec", result.message);
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ((std::vector<std::wstring>{ L"add", L"-A", L"--", L"." }), git.Operations()[0]);
}

TEST(GitCommitCommands, ARebaseInProgressFailsClosedInsteadOfCommittingOntoADetachedHead)
{
	const TemporaryGitDirectory directory(L"rebase");
	directory.MakeDirectory(L"rebase-merge");
	directory.Touch(L"REBASE_HEAD");
	CommitGit git;
	git.gitDirectory = directory.Path();
	std::vector<std::wstring> messages;
	auto context = MakeCommitContext(git);
	context.message = [&messages](std::wstring_view message) { messages.emplace_back(message); };

	const auto result = RunGitCommit(context, CommitState(1, 0), L"m", {});
	// Upstream continues the rebase here; that model does not exist, so this
	// refuses rather than writing a commit the user did not ask for.
	EXPECT_EQ(EGitCommitCommandStatus::UnsupportedRebaseInProgress, result.status);
	EXPECT_FALSE(result.message.empty());
	EXPECT_EQ(1U, messages.size());
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, AStaleRebaseHeadWithNoRebaseDirectoryDoesNotBlockACommit)
{
	const TemporaryGitDirectory directory(L"stale-rebase");
	// `REBASE_HEAD` outlives the rebase that wrote it, so it alone must not
	// refuse an otherwise ordinary commit.
	directory.Touch(L"REBASE_HEAD");
	CommitGit git;
	git.gitDirectory = directory.Path();
	auto context = MakeCommitContext(git);

	EXPECT_TRUE(RunGitCommit(context, CommitState(1, 0), L"m", {}).Succeeded());
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ(PlainCommit(), git.Operations()[0]);
}

TEST(GitCommitCommands, AMergeInProgressCommitsWithNothingStagedAndNoEmptyCommitPrompt)
{
	const TemporaryGitDirectory directory(L"merge");
	directory.Touch(L"MERGE_HEAD");
	CommitGit git;
	git.gitDirectory = directory.Path();
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(0, &prompts);

	// A merge with everything already resolved and staged looks like "nothing to
	// commit" to the counters, and upstream commits it rather than asking.
	EXPECT_TRUE(RunGitCommit(context, CommitState(0, 0), L"m", {}).Succeeded());
	EXPECT_TRUE(prompts.empty());
	ASSERT_EQ(1U, git.Operations().size());
	EXPECT_EQ(PlainCommit(), git.Operations()[0]);
}

TEST(GitCommitCommands, MissingCallablesFailInsteadOfSilentlySkippingTheirGate)
{
	GitCommitCommandContext context;
	context.repositoryRoot = LR"(C:\repo)";
	EXPECT_EQ(EGitCommitCommandStatus::Failed, RunGitCommit(context, CommitState(1, 0), L"m", {}).status);
	EXPECT_EQ(EGitCommitCommandStatus::Failed, RunGitUndoCommit(context, CommitState(1, 0)).status);

	CommitGit git;
	auto missingConfirm = MakeCommitContext(git);
	// The suggestion gate has no presenter, so it cannot be answered; staging
	// everything anyway would be exactly the accident the prompt prevents.
	EXPECT_EQ(EGitCommitCommandStatus::Failed, RunGitCommit(missingConfirm, CommitState(0, 1), L"m", {}).status);
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, UndoResetsSoftOntoTheParentAndRestoresTheUndoneMessage)
{
	CommitGit git;
	auto context = MakeCommitContext(git);

	const auto result = RunGitUndoCommit(context, CommitState(0, 0));
	EXPECT_EQ(EGitCommitCommandStatus::Succeeded, result.status);
	ASSERT_EQ(1U, git.Operations().size());
	// `--soft` keeps the undone commit's content staged. A mixed or hard reset
	// would unstage or destroy it, which is a different operation entirely.
	EXPECT_EQ((std::vector<std::wstring>{ L"reset", L"--soft", L"HEAD~" }), git.Operations()[0]);
	ASSERT_TRUE(result.inputBoxValue.has_value());
	// `%B` emits a trailing newline that is not part of the message.
	EXPECT_EQ(L"previous message", *result.inputBoxValue);
}

TEST(GitCommitCommands, UndoingTheFirstCommitDeletesHeadAndUnstagesEverything)
{
	CommitGit git;
	// One token is the commit itself, so this commit has no parent to reset onto.
	git.revList = "0000000\n";
	auto context = MakeCommitContext(git);

	EXPECT_TRUE(RunGitUndoCommit(context, CommitState(0, 0)).Succeeded());
	ASSERT_EQ(2U, git.Operations().size());
	EXPECT_EQ((std::vector<std::wstring>{ L"update-ref", L"-d", L"HEAD" }), git.Operations()[0]);
	// With HEAD gone the branch is unborn, so unstaging is `rm --cached`, not a
	// reset against a HEAD that no longer exists.
	EXPECT_EQ((std::vector<std::wstring>{ L"rm", L"--cached", L"-r", L"--", L"." }), git.Operations()[1]);
}

TEST(GitCommitCommands, UndoingAMergeCommitIsConfirmedFirst)
{
	CommitGit git;
	git.revList = "0000000 1111111 2222222\n";
	std::vector<GitCommitPrompt> prompts;
	auto context = MakeCommitContext(git);
	context.confirm = ChooseCommit(std::nullopt, &prompts);

	EXPECT_EQ(EGitCommitCommandStatus::Cancelled, RunGitUndoCommit(context, CommitState(0, 0)).status);
	ASSERT_EQ(1U, prompts.size());
	EXPECT_EQ(BuildUndoMergeCommitPrompt(), prompts[0]);
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, UndoingAnUnbornHeadIsNotApplicableRatherThanAFailure)
{
	CommitGit git;
	auto context = MakeCommitContext(git);
	std::vector<std::wstring> messages;
	context.message = [&messages](std::wstring_view message) { messages.emplace_back(message); };
	auto state = CommitState(0, 0);
	state.headHasCommit = false;

	EXPECT_EQ(EGitCommitCommandStatus::NotApplicable, RunGitUndoCommit(context, state).status);
	EXPECT_EQ(1U, messages.size());
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitCommitCommands, AnUnreadableCommitIsNotUndone)
{
	CommitGit git;
	git.revListFails = true;
	auto context = MakeCommitContext(git);

	const auto result = RunGitUndoCommit(context, CommitState(0, 0));
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_FALSE(result.message.empty());
	// Not knowing how many parents HEAD has means not knowing whether `HEAD~`
	// even exists, so nothing is reset.
	EXPECT_TRUE(git.Operations().empty());
}

TEST(GitCommitCommands, AFailedUndoResetLeavesTheBoxAlone)
{
	CommitGit git;
	git.failingCommand = L"reset";
	git.failingStandardError = "fatal: could not reset\n";
	auto context = MakeCommitContext(git);

	const auto result = RunGitUndoCommit(context, CommitState(0, 0));
	EXPECT_EQ(EGitCommitCommandStatus::Failed, result.status);
	EXPECT_EQ(L"fatal: could not reset", result.message);
	EXPECT_FALSE(result.inputBoxValue.has_value());
}

// ---------------------------------------------------------------------------
// Upstream's `Status` derivation, shared by the rows and by the diff resolver.
// ---------------------------------------------------------------------------

TEST(GitScmModel, TheTwoAreasOfOneChangeCarryTwoStatuses)
{
	const auto change = Change(L"a.txt", L'M', L'M');
	EXPECT_EQ(EGitFileStatus::IndexModified, ClassifyGitFileStatus(change, EGitChangeArea::Index));
	EXPECT_EQ(EGitFileStatus::Modified, ClassifyGitFileStatus(change, EGitChangeArea::WorkingTree));
}

TEST(GitScmModel, AWorkingTreeRIsAnIntentToRenameNotARename)
{
	// Upstream's second switch maps `R` to INDEX_RENAMED and its third maps `R` to
	// INTENT_TO_RENAME. Reading the index name for a working-tree row would tell
	// the user a rename is staged when it is not.
	const auto change = Change(L"a.txt", L'.', L'R');
	EXPECT_EQ(EGitFileStatus::IntentToRename, ClassifyGitFileStatus(change, EGitChangeArea::WorkingTree));
	EXPECT_EQ(L'R', GitFileStatusLetter(EGitFileStatus::IntentToRename));
	EXPECT_EQ("Intent to Rename", GitFileStatusText(EGitFileStatus::IntentToRename));
}

TEST(GitScmModel, TheCodesUpstreamListsNowhereProduceNoStatus)
{
	// `C` exists in upstream's index switch only, and `T` in its working-tree
	// switch only. The other half falls out of the switch with no case, which is
	// no row rather than an invented one.
	EXPECT_FALSE(ClassifyGitFileStatus(Change(L"a.txt", L'T', L'.'), EGitChangeArea::Index).has_value());
	EXPECT_FALSE(ClassifyGitFileStatus(Change(L"a.txt", L'.', L'C'), EGitChangeArea::WorkingTree).has_value());
}

TEST(GitScmModel, AWorkingTreeTypeChangeIsTypeChanged)
{
	const auto change = Change(L"a.txt", L'.', L'T');
	EXPECT_EQ(EGitFileStatus::TypeChanged, ClassifyGitFileStatus(change, EGitChangeArea::WorkingTree));
	EXPECT_EQ(L'T', GitFileStatusLetter(EGitFileStatus::TypeChanged));
	EXPECT_EQ("Type Changed", GitFileStatusText(EGitFileStatus::TypeChanged));
}

TEST(GitScmModel, AConflictHasOneStatusInEitherArea)
{
	const auto change = Conflicted(L"a.txt", L'U', L'U');
	EXPECT_EQ(EGitFileStatus::BothModified, ClassifyGitFileStatus(change, EGitChangeArea::Index));
	EXPECT_EQ(EGitFileStatus::BothModified, ClassifyGitFileStatus(change, EGitChangeArea::WorkingTree));
	EXPECT_EQ(L'!', GitFileStatusLetter(EGitFileStatus::BothModified));
}

TEST(GitScmModel, AnUnmergedPairUpstreamDoesNotListProducesNoStatus)
{
	const auto change = Conflicted(L"a.txt", L'U', L'M');
	EXPECT_FALSE(ClassifyGitFileStatus(change, EGitChangeArea::WorkingTree).has_value());
}

TEST(GitScmModel, EveryDeletedFlavourIsStruckThrough)
{
	EXPECT_TRUE(IsGitFileStatusStruckThrough(EGitFileStatus::Deleted));
	EXPECT_TRUE(IsGitFileStatusStruckThrough(EGitFileStatus::IndexDeleted));
	EXPECT_TRUE(IsGitFileStatusStruckThrough(EGitFileStatus::BothDeleted));
	EXPECT_TRUE(IsGitFileStatusStruckThrough(EGitFileStatus::DeletedByUs));
	EXPECT_TRUE(IsGitFileStatusStruckThrough(EGitFileStatus::DeletedByThem));
	EXPECT_FALSE(IsGitFileStatusStruckThrough(EGitFileStatus::Modified));
	EXPECT_FALSE(IsGitFileStatusStruckThrough(EGitFileStatus::Untracked));
}

// ---------------------------------------------------------------------------
// What a row compares.
// ---------------------------------------------------------------------------

TEST(GitDiffModel, AStagedEditComparesHeadWithTheIndex)
{
	const auto row = MakeGitDiffRow(Change(L"a.txt", L'M', L'.'), EGitFileStatus::IndexModified, true);
	const auto input = ResolveGitDiffInput(row);

	EXPECT_EQ(EGitDiffCommandKind::Diff, input.kind);
	ASSERT_TRUE(input.original.has_value());
	EXPECT_EQ(EGitDiffSource::Repository, input.original->source);
	EXPECT_EQ(L"HEAD", input.original->ref);
	ASSERT_TRUE(input.modified.has_value());
	EXPECT_EQ(EGitDiffSource::Repository, input.modified->source);
	// The index is the **empty** ref, not an absent one: `git show :a.txt`.
	EXPECT_EQ(L"", input.modified->ref);
	EXPECT_EQ(L"a.txt (Index)", input.title);
}

TEST(GitDiffModel, AStagedRenameReadsHeadUnderTheOldName)
{
	auto change = Change(L"new.txt", L'R', L'.');
	change.originalPath = L"old.txt";
	const auto row = MakeGitDiffRow(change, EGitFileStatus::IndexRenamed, true);
	const auto input = ResolveGitDiffInput(row);

	ASSERT_TRUE(input.original.has_value());
	// HEAD knows the content only under the name it had there.
	EXPECT_EQ(L"old.txt", input.original->path);
	ASSERT_TRUE(input.modified.has_value());
	EXPECT_EQ(L"new.txt", input.modified->path);
	EXPECT_EQ(L"new.txt (Index)", input.title);
}

TEST(GitDiffModel, AnUnstagedEditComparesAgainstWhicheverSideHoldsTheBaseline)
{
	const auto change = Change(L"a.txt", L'M', L'M');

	// Something is staged for this path, so upstream's `sanitizeRef('~')` resolves
	// to the index. Comparing against HEAD here would silently fold the staged
	// work into the unstaged row.
	const auto staged = ResolveGitDiffInput(MakeGitDiffRow(change, EGitFileStatus::Modified, true));
	ASSERT_TRUE(staged.original.has_value());
	EXPECT_EQ(L"", staged.original->ref);

	const auto unstaged = ResolveGitDiffInput(MakeGitDiffRow(change, EGitFileStatus::Modified, false));
	ASSERT_TRUE(unstaged.original.has_value());
	EXPECT_EQ(L"HEAD", unstaged.original->ref);

	ASSERT_TRUE(unstaged.modified.has_value());
	EXPECT_EQ(EGitDiffSource::WorkingTree, unstaged.modified->source);
	EXPECT_EQ(L"a.txt (Working Tree)", unstaged.title);
}

TEST(GitDiffModel, AnUntrackedFileIsOpenedRatherThanCompared)
{
	const auto row = MakeGitDiffRow(Untracked(L"a.txt"), EGitFileStatus::Untracked, false);
	const auto input = ResolveGitDiffInput(row);

	EXPECT_EQ(EGitDiffCommandKind::Open, input.kind);
	EXPECT_FALSE(input.original.has_value());
	ASSERT_TRUE(input.modified.has_value());
	EXPECT_EQ(EGitDiffSource::WorkingTree, input.modified->source);
	EXPECT_EQ(L"a.txt (Untracked)", input.title);
}

TEST(GitDiffModel, ADeletedFileOpensWhatWasRemoved)
{
	const auto row = MakeGitDiffRow(Change(L"a.txt", L'.', L'D'), EGitFileStatus::Deleted, false);
	const auto input = ResolveGitDiffInput(row);

	EXPECT_EQ(EGitDiffCommandKind::Open, input.kind);
	ASSERT_TRUE(input.modified.has_value());
	EXPECT_EQ(L"HEAD", input.modified->ref);
	EXPECT_EQ(L"a.txt (Deleted)", input.title);
}

TEST(GitDiffModel, AConflictDeletedByUsComparesTheMergeBaseWithTheirs)
{
	const auto row = MakeGitDiffRow(Conflicted(L"a.txt", L'D', L'U'), EGitFileStatus::DeletedByUs, false);
	const auto input = ResolveGitDiffInput(row);

	EXPECT_EQ(EGitDiffCommandKind::Diff, input.kind);
	ASSERT_TRUE(input.original.has_value());
	EXPECT_EQ(L":1", input.original->ref);
	ASSERT_TRUE(input.modified.has_value());
	EXPECT_EQ(L":3", input.modified->ref);
	EXPECT_EQ(L"a.txt (Theirs)", input.title);
}

TEST(GitDiffModel, AStatusWithNeitherSideRefusesToOpenAnything)
{
	const auto row = MakeGitDiffRow(Conflicted(L"a.txt", L'D', L'D'), EGitFileStatus::BothDeleted, false);
	const auto input = ResolveGitDiffInput(row);

	// Both sides are gone. Upstream's arms list neither, and its title switch
	// falls through to `''`.
	EXPECT_EQ(EGitDiffCommandKind::None, input.kind);
	EXPECT_FALSE(input.original.has_value());
	EXPECT_FALSE(input.modified.has_value());
	EXPECT_TRUE(input.title.empty());
}

TEST(GitDiffModel, AnEndpointBecomesGitShowOnlyWhenItReadsTheRepository)
{
	const GitDiffEndpoint index{ EGitDiffSource::Repository, L"", L"dir/a.txt" };
	const std::vector<std::wstring> expected{ L"show", L"--textconv", L":dir/a.txt" };
	EXPECT_EQ(expected, BuildGitShowArguments(index));

	const GitDiffEndpoint head{ EGitDiffSource::Repository, L"HEAD", L"dir/a.txt" };
	const std::vector<std::wstring> expectedHead{ L"show", L"--textconv", L"HEAD:dir/a.txt" };
	EXPECT_EQ(expectedHead, BuildGitShowArguments(head));

	// The working tree is read from disk. Asking git for it would be a different
	// file whenever the index and the disk disagree, which is exactly the case a
	// diff exists to show.
	const GitDiffEndpoint worktree{ EGitDiffSource::WorkingTree, {}, L"dir/a.txt" };
	EXPECT_TRUE(BuildGitShowArguments(worktree).empty());
}

TEST(GitDiffModel, ARepositoryPathUsesForwardSlashes)
{
	auto change = Change(L"dir\\sub\\a.txt", L'M', L'.');
	const auto input = ResolveGitDiffInput(MakeGitDiffRow(change, EGitFileStatus::IndexModified, true));

	ASSERT_TRUE(input.original.has_value());
	EXPECT_EQ(L"dir/sub/a.txt", input.original->path);
	// The title is the basename either way.
	EXPECT_EQ(L"a.txt (Index)", input.title);
}

// ---------------------------------------------------------------------------
// Lines and the line diff.
// ---------------------------------------------------------------------------

TEST(GitDiffModel, ATrailingTerminatorProducesAFinalEmptyLine)
{
	const std::vector<std::wstring> expected{ L"a", L"" };
	EXPECT_EQ(expected, SplitGitDiffLines(L"a\n"));

	const std::vector<std::wstring> two{ L"a", L"b" };
	EXPECT_EQ(two, SplitGitDiffLines(L"a\nb"));
	EXPECT_EQ(two, SplitGitDiffLines(L"a\r\nb"));
	EXPECT_EQ(two, SplitGitDiffLines(L"a\rb"));

	const std::vector<std::wstring> empty{ L"" };
	EXPECT_EQ(empty, SplitGitDiffLines(L""));
}

TEST(GitDiffModel, IdenticalTextHasNoChanges)
{
	const std::vector<std::wstring> lines{ L"a", L"b", L"c" };
	const auto diff = ComputeGitLineDiff(lines, lines);
	EXPECT_TRUE(diff.changes.empty());
	EXPECT_FALSE(diff.hitTimeout);
}

TEST(GitDiffModel, AnInsertionHasAnEmptyOriginalRangeWhereTheLinesWent)
{
	const auto diff = ComputeGitLineDiff({ L"a", L"c" }, { L"a", L"b", L"c" });
	ASSERT_EQ(1U, diff.changes.size());
	EXPECT_TRUE(diff.changes[0].original.IsEmpty());
	EXPECT_EQ(2, diff.changes[0].original.startLineNumber);
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].modified);
}

TEST(GitDiffModel, ADeletionHasAnEmptyModifiedRange)
{
	const auto diff = ComputeGitLineDiff({ L"a", L"b", L"c" }, { L"a", L"c" });
	ASSERT_EQ(1U, diff.changes.size());
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].original);
	EXPECT_TRUE(diff.changes[0].modified.IsEmpty());
	EXPECT_EQ(2, diff.changes[0].modified.startLineNumber);
}

TEST(GitDiffModel, AReplacedLineIsOneRegionOnBothSides)
{
	const auto diff = ComputeGitLineDiff({ L"a", L"b", L"c" }, { L"a", L"x", L"c" });
	ASSERT_EQ(1U, diff.changes.size());
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].original);
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].modified);
}

TEST(GitDiffModel, AnUnchangedLineBetweenTwoEditsSplitsThemIntoTwoRegions)
{
	const auto diff = ComputeGitLineDiff(
		{ L"a", L"b", L"c", L"d", L"e" }, { L"a", L"x", L"c", L"y", L"e" });
	ASSERT_EQ(2U, diff.changes.size());
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].original);
	EXPECT_EQ((GitLineRange{ 2, 3 }), diff.changes[0].modified);
	EXPECT_EQ((GitLineRange{ 4, 5 }), diff.changes[1].original);
	EXPECT_EQ((GitLineRange{ 4, 5 }), diff.changes[1].modified);
}

TEST(GitDiffModel, AWholeFileRewriteIsOneRegion)
{
	const auto diff = ComputeGitLineDiff({ L"a", L"b" }, { L"x", L"y", L"z" });
	ASSERT_EQ(1U, diff.changes.size());
	EXPECT_EQ((GitLineRange{ 1, 3 }), diff.changes[0].original);
	EXPECT_EQ((GitLineRange{ 1, 4 }), diff.changes[0].modified);
	EXPECT_FALSE(diff.hitTimeout);
}

TEST(GitDiffModel, ADiffBeyondTheBoundReportsItselfAsBounded)
{
	// Past the edit-distance bound the subdivision is missing, so the result must
	// say so rather than presenting one coarse region as if it were the answer.
	std::vector<std::wstring> original;
	std::vector<std::wstring> modified;
	for (int index = 0; index < 1100; ++index) {
		original.push_back(L"o" + std::to_wstring(index));
		modified.push_back(L"m" + std::to_wstring(index));
	}

	const auto diff = ComputeGitLineDiff(original, modified);
	EXPECT_TRUE(diff.hitTimeout);
	ASSERT_EQ(1U, diff.changes.size());
	EXPECT_EQ((GitLineRange{ 1, 1101 }), diff.changes[0].original);
	EXPECT_EQ((GitLineRange{ 1, 1101 }), diff.changes[0].modified);
}

TEST(GitDiffModel, UnchangedTextAlignsBothSidesOneToOne)
{
	const std::vector<GitDiffViewRow> expected{
		{ false, 1, 1 },
		{ false, 2, 2 },
		{ false, 3, 3 },
	};
	EXPECT_EQ(expected, BuildGitDiffViewRows(3, 3, {}));
}

TEST(GitDiffModel, AnInsertedLineLeavesTheOriginalSideBlankAtThatRow)
{
	// The blank is the whole point: without it the unchanged `c` would sit one
	// row higher on the left than on the right.
	const std::vector<std::wstring> original{ L"a", L"c" };
	const std::vector<std::wstring> modified{ L"a", L"b", L"c" };
	const auto rows = BuildGitDiffViewRows(static_cast<int>(original.size()),
		static_cast<int>(modified.size()), ComputeGitLineDiff(original, modified));

	const std::vector<GitDiffViewRow> expected{
		{ false, 1, 1 },
		{ true, 0, 2 },
		{ false, 2, 3 },
	};
	EXPECT_EQ(expected, rows);
}

TEST(GitDiffModel, ADeletedLineLeavesTheModifiedSideBlankAtThatRow)
{
	const std::vector<std::wstring> original{ L"a", L"b", L"c" };
	const std::vector<std::wstring> modified{ L"a", L"c" };
	const auto rows = BuildGitDiffViewRows(static_cast<int>(original.size()),
		static_cast<int>(modified.size()), ComputeGitLineDiff(original, modified));

	const std::vector<GitDiffViewRow> expected{
		{ false, 1, 1 },
		{ true, 2, 0 },
		{ false, 3, 2 },
	};
	EXPECT_EQ(expected, rows);
}

TEST(GitDiffModel, AnUnevenRegionStartsLevelAndPadsAtItsEnd)
{
	// One line became three. The region's first row carries both sides, and the
	// two rows the original does not reach are padding rather than a second
	// region, so the unchanged tail stays level.
	const std::vector<std::wstring> original{ L"a", L"b", L"z" };
	const std::vector<std::wstring> modified{ L"a", L"x", L"y", L"w", L"z" };
	const auto rows = BuildGitDiffViewRows(static_cast<int>(original.size()),
		static_cast<int>(modified.size()), ComputeGitLineDiff(original, modified));

	const std::vector<GitDiffViewRow> expected{
		{ false, 1, 1 },
		{ true, 2, 2 },
		{ true, 0, 3 },
		{ true, 0, 4 },
		{ false, 3, 5 },
	};
	EXPECT_EQ(expected, rows);
}

TEST(GitDiffModel, TwoRegionsKeepTheUnchangedLineBetweenThemAligned)
{
	const std::vector<std::wstring> original{ L"a", L"b", L"c", L"d", L"e" };
	const std::vector<std::wstring> modified{ L"a", L"x", L"c", L"y", L"e" };
	const auto rows = BuildGitDiffViewRows(static_cast<int>(original.size()),
		static_cast<int>(modified.size()), ComputeGitLineDiff(original, modified));

	const std::vector<GitDiffViewRow> expected{
		{ false, 1, 1 },
		{ true, 2, 2 },
		{ false, 3, 3 },
		{ true, 4, 4 },
		{ false, 5, 5 },
	};
	EXPECT_EQ(expected, rows);
}

TEST(GitDiffModel, ANewFileIsAllInsertionAndAnEmptiedFileIsAllDeletion)
{
	const std::vector<std::wstring> empty{ L"" };
	const std::vector<std::wstring> content{ L"a", L"b" };

	const auto added = BuildGitDiffViewRows(static_cast<int>(empty.size()),
		static_cast<int>(content.size()), ComputeGitLineDiff(empty, content));
	ASSERT_EQ(2U, added.size());
	EXPECT_TRUE(std::ranges::all_of(added, [](const GitDiffViewRow& row) { return row.changed; }));
	EXPECT_EQ(0, added[1].originalLineNumber);

	const auto removed = BuildGitDiffViewRows(static_cast<int>(content.size()),
		static_cast<int>(empty.size()), ComputeGitLineDiff(content, empty));
	ASSERT_EQ(2U, removed.size());
	EXPECT_TRUE(std::ranges::all_of(removed, [](const GitDiffViewRow& row) { return row.changed; }));
	EXPECT_EQ(0, removed[1].modifiedLineNumber);
}

TEST(GitDiffModel, AGitUriQueryRoundTripsAndKeepsTheIndexRefEmpty)
{
	const GitUriParams params{ LR"(C:\repo\src\a.cpp)", L"" };
	const auto query = BuildGitUriQuery(params);
	// A Windows path's separators are JSON escapes, exactly as upstream's
	// `JSON.stringify` writes them.
	EXPECT_EQ(LR"({"path":"C:\\repo\\src\\a.cpp","ref":""})"s, query);

	const auto parsed = ParseGitUriQuery(query);
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(params, *parsed);
}

TEST(GitDiffModel, AGitUriQueryFailsClosedOnAnythingItCannotActOn)
{
	// A member this build cannot honour, upstream's own `submoduleOf` included:
	// reading the rest would compare the wrong repository's file.
	EXPECT_FALSE(ParseGitUriQuery(LR"({"path":"C:\\a","ref":"","submoduleOf":"C:\\b"})").has_value());
	// No path names no file, and a truncated or trailing-garbage object is not a
	// shorter request.
	EXPECT_FALSE(ParseGitUriQuery(LR"({"ref":"HEAD"})").has_value());
	EXPECT_FALSE(ParseGitUriQuery(LR"({"path":""})").has_value());
	EXPECT_FALSE(ParseGitUriQuery(LR"({"path":"C:\\a")").has_value());
	EXPECT_FALSE(ParseGitUriQuery(LR"({"path":"C:\\a","ref":""}x)").has_value());
	// An unescaped backslash is not a path with separators; it is malformed JSON.
	EXPECT_FALSE(ParseGitUriQuery(LR"({"path":"C:\a"})").has_value());
	EXPECT_FALSE(ParseGitUriQuery(L"").has_value());
}

TEST(GitDiffModel, AWorkingTreeSideIsTheFileUriAndARepositorySideCarriesTheRef)
{
	const std::wstring root = LR"(C:\repo)";

	const GitDiffEndpoint worktree{ EGitDiffSource::WorkingTree, L"", L"src/a.cpp" };
	EXPECT_EQ(L"file:///C:/repo/src/a.cpp"s, BuildGitDiffEndpointUri(worktree, root));

	const GitDiffEndpoint head{ EGitDiffSource::Repository, L"HEAD", L"src/a.cpp" };
	const auto uri = BuildGitDiffEndpointUri(head, root);
	// Upstream's `toGitUri`: the scheme changes and the path does not, so both
	// sides of the comparison still name the same file.
	EXPECT_TRUE(uri.starts_with(L"git:///C:/repo/src/a.cpp?"));

	const auto resolved = ResolveGitDiffEndpointUri(uri, root);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(head, *resolved);
}

TEST(GitDiffModel, AnIndexSideKeepsItsEmptyRefThroughTheUri)
{
	const std::wstring root = LR"(C:\repo)";
	const GitDiffEndpoint index{ EGitDiffSource::Repository, L"", L"src/a.cpp" };

	const auto resolved = ResolveGitDiffEndpointUri(BuildGitDiffEndpointUri(index, root), root);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(index, *resolved);
	// The whole point of preserving it: `git show :path` reads the index.
	EXPECT_EQ(L":src/a.cpp"s, BuildGitShowArguments(*resolved).back());
}

TEST(GitDiffModel, AUriIsResolvedAgainstTheRepositoryItWasIssuedAgainst)
{
	const std::wstring root = LR"(C:\repo)";
	const GitDiffEndpoint worktree{ EGitDiffSource::WorkingTree, L"", L"src/a.cpp" };
	const auto uri = BuildGitDiffEndpointUri(worktree, root);

	// Case and separator form name one file on Windows, so a differently spelled
	// root is the same root.
	const auto sameRoot = ResolveGitDiffEndpointUri(uri, LR"(c:/REPO\)");
	ASSERT_TRUE(sameRoot.has_value());
	EXPECT_EQ(worktree, *sameRoot);

	// A sibling that merely shares the spelling as a prefix is a different
	// repository, and the file is outside it.
	EXPECT_FALSE(ResolveGitDiffEndpointUri(uri, LR"(C:\rep)").has_value());
	EXPECT_FALSE(ResolveGitDiffEndpointUri(uri, LR"(C:\other)").has_value());
	// The root itself is not a file inside the root.
	EXPECT_FALSE(ResolveGitDiffEndpointUri(uri, LR"(C:\repo\src\a.cpp)").has_value());
}

TEST(GitDiffModel, AGitUriWithNoQueryNamesNothing)
{
	// An absent query is not an index reference with an empty ref.
	EXPECT_FALSE(ResolveGitDiffEndpointUri(L"git:///C:/repo/src/a.cpp", LR"(C:\repo)").has_value());
	EXPECT_FALSE(ResolveGitDiffEndpointUri(L"not a uri", LR"(C:\repo)").has_value());
}

TEST(GitHistoryModel, AsksGitForOneRecordPerCommitWithSeparatorsThatCannotAppearInAField)
{
	const auto arguments = MakeGitHistoryArguments(50);
	ASSERT_EQ(4u, arguments.size());
	EXPECT_EQ(L"log", arguments[0]);
	// Topological order is what makes a parent appear below its child, which is
	// the only order the swimlane walk can be built from.
	EXPECT_EQ(L"--topo-order", arguments[1]);
	EXPECT_EQ(L"--max-count=50", arguments[2]);
	EXPECT_EQ(MakeGitHistoryFormat(), arguments[3]);
	// A zero page would return the whole history, so it is clamped rather than
	// passed through as "no limit".
	EXPECT_EQ(L"--max-count=1", MakeGitHistoryArguments(0)[2]);
}

TEST(GitHistoryModel, ParsesEveryFieldAndKeepsASubjectThatContainsSpaces)
{
	// Each separator ends its own string literal: `\x1f` followed by a hex digit
	// would otherwise be read as one longer hexadecimal escape.
	const std::string output =
		"a1\x1f" "b2 c3\x1f" "HEAD -> main, origin/main, tag: v1.0\x1f" "Ada\x1f"
		"ada@example.com\x1f" "1700000000\x1f" "Fix: the thing, twice\x1f"
		"Fix: the thing, twice\n\nAnd say why.\n\x1e"
		"b2\x1f\x1f\x1f" "Ada\x1f" "ada@example.com\x1f" "1699000000\x1f" "Root\x1f" "Root\n\x1e";
	const auto items = ParseGitHistory(output);
	ASSERT_EQ(2u, items.size());
	EXPECT_EQ(L"a1", items[0].id);
	ASSERT_EQ(2u, items[0].parentIds.size());
	EXPECT_EQ(L"b2", items[0].parentIds[0]);
	EXPECT_EQ(L"c3", items[0].parentIds[1]);
	EXPECT_EQ(L"Ada", items[0].authorName);
	EXPECT_EQ(1700000000, items[0].authorTimestamp);
	EXPECT_EQ(L"Fix: the thing, twice", items[0].subject);
	// The full message keeps the body and its blank line, and loses only git's
	// trailing newline: it is the clipboard payload, not the row's label.
	EXPECT_EQ(L"Fix: the thing, twice\n\nAnd say why.", items[0].message);
	EXPECT_EQ(L"Root", items[1].message);
	// The root commit has no parents and no decorations; both are empty fields,
	// not missing ones.
	EXPECT_TRUE(items[1].parentIds.empty());
	EXPECT_TRUE(items[1].refs.empty());
}

TEST(GitHistoryModel, ClassifiesDecorationsTheWayGitSpellsThem)
{
	const auto refs = ParseGitHistoryRefs(L"HEAD -> main, origin/main, tag: v1.0, feature");
	ASSERT_EQ(4u, refs.size());
	// `HEAD -> main` names the checked-out branch, so the arrow carries the
	// distinction and the branch name survives the prefix.
	EXPECT_EQ(EGitHistoryRefKind::Head, refs[0].kind);
	EXPECT_EQ(L"main", refs[0].name);
	EXPECT_EQ(EGitHistoryRefKind::RemoteBranch, refs[1].kind);
	EXPECT_EQ(EGitHistoryRefKind::Tag, refs[2].kind);
	EXPECT_EQ(L"v1.0", refs[2].name);
	EXPECT_EQ(EGitHistoryRefKind::LocalBranch, refs[3].kind);

	const auto detached = ParseGitHistoryRefs(L"HEAD");
	ASSERT_EQ(1u, detached.size());
	EXPECT_EQ(EGitHistoryRefKind::Head, detached[0].kind);
	EXPECT_EQ(L"HEAD", detached[0].name);
}

TEST(GitHistoryModel, KeepsTheSharedParentLaneSoTheBranchConnectorCanRejoin)
{
	//   m  -- merge of a and b
	//   |\
	//   a |
	//   | b
	//   r  -- both sides descend from the root
	const std::vector<GitHistoryItem> items{
		{ .id = L"m", .parentIds = { L"a", L"b" } },
		{ .id = L"a", .parentIds = { L"r" } },
		{ .id = L"b", .parentIds = { L"r" } },
		{ .id = L"r" },
	};
	const auto rows = BuildScmHistoryGraph(items);
	ASSERT_EQ(4u, rows.size());
	// The merge starts the drawing, so nothing enters its row and two lanes leave
	// it: one per parent.
	EXPECT_TRUE(rows[0].inputSwimlanes.empty());
	EXPECT_EQ(0u, rows[0].circleLane);
	ASSERT_EQ(2u, rows[0].outputSwimlanes.size());
	EXPECT_EQ(L"a", rows[0].outputSwimlanes[0].id);
	EXPECT_EQ(L"b", rows[0].outputSwimlanes[1].id);
	// `a` sits on the lane that was waiting for it, and the second lane keeps its
	// own position so `b`'s line does not jump sideways.
	EXPECT_EQ(0u, rows[1].circleLane);
	ASSERT_EQ(2u, rows[1].outputSwimlanes.size());
	EXPECT_EQ(L"r", rows[1].outputSwimlanes[0].id);
	EXPECT_EQ(L"b", rows[1].outputSwimlanes[1].id);
	// `b`'s parent is already awaited on lane 0, but the second copy is kept on
	// lane 1. The renderer needs it on the next row to draw the branch connector
	// back into the shared commit; dropping it would cut the line off at `b`.
	EXPECT_EQ(1u, rows[2].circleLane);
	ASSERT_EQ(2u, rows[2].outputSwimlanes.size());
	EXPECT_EQ(L"r", rows[2].outputSwimlanes[0].id);
	EXPECT_EQ(L"r", rows[2].outputSwimlanes[1].id);
	// Both lanes arrive at the root. The second lane is consumed by the merge
	// path, so the root still has one visible circle and no output lane.
	EXPECT_EQ(0u, rows[3].circleLane);
	EXPECT_TRUE(rows[3].outputSwimlanes.empty());
}

TEST(ScmViewStackLayout, ACollapsedSectionKeepsItsHeaderAndGivesUpItsBody)
{
	const auto layout = BuildScmViewStackLayout({
		.clientTop = 0,
		.clientBottom = 500,
		.viewHeaderHeight = 30,
		.repositoryRowHeight = 22,
		.inputOuterMargin = 5,
		.inputHeight = 26,
		.graphBodyHeight = 100,
		.sashHeight = 4,
		.minimumBodyHeight = 66,
		.repositoriesVisible = true,
		.repositoriesCollapsed = true,
		.changesCollapsed = true,
		.graphCollapsed = true,
		.changesHeaderVisible = true,
		.inputVisible = true,
		.graphVisible = true,
	});
	// Every header survives, because a section with no header could not be
	// reopened.
	EXPECT_EQ((ScmVerticalBounds{ 0, 30 }), layout.repositoriesHeader);
	EXPECT_TRUE(layout.repositoryRow.Empty());
	EXPECT_EQ((ScmVerticalBounds{ 30, 60 }), layout.changesHeader);
	// The commit box belongs to the Changes section, so it collapses with it.
	EXPECT_TRUE(layout.input.Empty());
	EXPECT_EQ((ScmVerticalBounds{ 470, 500 }), layout.graphHeader);
	EXPECT_TRUE(layout.graphBody.Empty());
	// A collapsed Graph has no height to drag, so it offers no sash.
	EXPECT_TRUE(layout.sash.Empty());
}

TEST(ScmViewStackLayout, TheSashStraddlesTheBoundaryAndTheGraphCannotStarveTheChangeList)
{
	const auto measurements = ScmViewStackMeasurements{
		.clientTop = 0,
		.clientBottom = 300,
		.viewHeaderHeight = 30,
		.graphBodyHeight = 100,
		.sashHeight = 4,
		.minimumBodyHeight = 66,
		.changesHeaderVisible = true,
		.graphVisible = true,
	};
	const auto layout = BuildScmViewStackLayout(measurements);
	EXPECT_EQ((ScmVerticalBounds{ 30, 170 }), layout.changesBody);
	EXPECT_EQ((ScmVerticalBounds{ 170, 200 }), layout.graphHeader);
	EXPECT_EQ((ScmVerticalBounds{ 200, 300 }), layout.graphBody);
	// The sash is an overlay across the boundary, so it consumes no layout space
	// and the two bands still meet exactly at 170.
	EXPECT_EQ((ScmVerticalBounds{ 168, 172 }), layout.sash);

	// A drag past the change list's floor is clamped: the list keeps its minimum
	// rather than the Graph taking the whole view.
	auto greedy = measurements;
	greedy.graphBodyHeight = 1000;
	const auto clamped = BuildScmViewStackLayout(greedy);
	EXPECT_EQ((ScmVerticalBounds{ 30, 96 }), clamped.changesBody);
	EXPECT_EQ(66, clamped.changesBody.bottom - clamped.changesBody.top);
}

} // namespace workbench::scm
