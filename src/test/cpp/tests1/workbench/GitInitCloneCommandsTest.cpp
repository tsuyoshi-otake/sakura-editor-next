/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workbench/scm/GitFailureText.h"
#include "workbench/scm/GitInitCloneCommands.h"

using namespace workbench::scm;

namespace {

using Arguments = std::vector<std::wstring>;

[[nodiscard]] GitExecutionResult Ok(std::string_view standardOutput = {})
{
	GitExecutionResult result;
	result.status = EGitExecutionStatus::Succeeded;
	result.exitCode = 0;
	result.standardOutput.assign(standardOutput.begin(), standardOutput.end());
	return result;
}

[[nodiscard]] GitExecutionResult Failure(std::string_view standardError, std::string_view standardOutput = {})
{
	GitExecutionResult result;
	// A non-zero exit is still `Failed` only when the process never ran; a git
	// that ran and refused reports `Succeeded` with a non-zero exit code — see
	// `GitCommandRunner.h`'s own doc comment.
	result.status = EGitExecutionStatus::Succeeded;
	result.exitCode = 1;
	result.standardOutput.assign(standardOutput.begin(), standardOutput.end());
	result.standardError.assign(standardError);
	return result;
}

[[nodiscard]] GitExecutionResult CancelledExecution()
{
	GitExecutionResult result;
	result.status = EGitExecutionStatus::Cancelled;
	result.exitCode = -1;
	return result;
}

//! Records every `git.init` interaction and answers from scripted queues, the
//! same fixture shape `GitSyncCommandsTest.cpp` uses for `git.sync`.
class FakeGitInit final
{
public:
	std::vector<std::pair<std::wstring, Arguments>> invocations;
	std::vector<GitExecutionResult> responses;
	std::vector<std::wstring> messages;

	std::vector<std::vector<GitInitFolderPickItem>> folderPickItems;
	std::vector<std::wstring> folderPickPlaceholders;
	std::vector<std::optional<std::size_t>> folderPicks;

	std::vector<std::pair<std::wstring, std::wstring>> browseCalls;
	std::vector<std::optional<std::wstring>> browseResults;

	std::vector<GitPrompt> prompts;
	std::vector<std::optional<std::size_t>> confirmations;

	//! When false, the corresponding presenter is left unset so a missing-
	//! collaborator path can be exercised.
	bool hasFolderPick{ true };
	bool hasBrowseForFolder{ true };
	bool hasConfirm{ true };
	bool hasRun{ true };

	[[nodiscard]] GitInitCommandContext Context()
	{
		GitInitCommandContext context;
		if (hasRun) {
			context.run = [this](std::wstring_view workingDirectory, const Arguments& arguments) {
				invocations.emplace_back(std::wstring(workingDirectory), arguments);
				if (invocations.size() <= responses.size()) {
					return responses[invocations.size() - 1];
				}
				return Ok();
			};
		}
		context.message = [this](std::wstring_view message) { messages.emplace_back(message); };
		if (hasFolderPick) {
			context.folderPick = [this](const std::vector<GitInitFolderPickItem>& items, std::wstring_view placeholder)
				-> std::optional<std::size_t> {
				folderPickItems.push_back(items);
				folderPickPlaceholders.emplace_back(placeholder);
				if (folderPickPlaceholders.size() <= folderPicks.size()) {
					return folderPicks[folderPickPlaceholders.size() - 1];
				}
				return std::nullopt;
			};
		}
		if (hasBrowseForFolder) {
			context.browseForFolder = [this](std::wstring_view openLabel, std::wstring_view startingDirectory)
				-> std::optional<std::wstring> {
				browseCalls.emplace_back(std::wstring(openLabel), std::wstring(startingDirectory));
				if (browseCalls.size() <= browseResults.size()) {
					return browseResults[browseCalls.size() - 1];
				}
				return std::nullopt;
			};
		}
		if (hasConfirm) {
			context.confirm = [this](const GitPrompt& prompt) -> std::optional<std::size_t> {
				prompts.push_back(prompt);
				if (prompts.size() <= confirmations.size()) {
					return confirmations[prompts.size() - 1];
				}
				return std::size_t{ 0 };
			};
		}
		return context;
	}
};

//! Records every `git.clone` interaction, split across the three phases the
//! non-blocking design separates: prepare (prompts, validates the
//! destination), execute (the potentially long `git clone` itself, forwarded
//! a real `HANDLE`), and complete (turns the raw result into the typed one).
class FakeGitClone final
{
public:
	std::vector<std::wstring> urlPrompts;
	std::vector<std::optional<std::wstring>> urlAnswers;

	std::vector<std::pair<std::wstring, std::wstring>> browseCalls;
	std::vector<std::optional<std::wstring>> browseResults;

	std::vector<std::wstring> pathStateQueries;
	std::vector<EGitPathState> pathStateAnswers;

	std::vector<GitPrompt> prompts;
	std::vector<std::wstring> messages;

	bool hasPromptForUrl{ true };
	bool hasBrowseForParentDirectory{ true };
	bool hasPathState{ true };
	bool hasConfirm{ true };

	[[nodiscard]] GitCloneCommandContext Context()
	{
		GitCloneCommandContext context;
		if (hasPromptForUrl) {
			context.promptForUrl = [this](std::wstring_view prompt, std::wstring_view, std::wstring_view)
				-> std::optional<std::wstring> {
				urlPrompts.emplace_back(prompt);
				if (urlPrompts.size() <= urlAnswers.size()) {
					return urlAnswers[urlPrompts.size() - 1];
				}
				return std::nullopt;
			};
		}
		if (hasBrowseForParentDirectory) {
			context.browseForParentDirectory = [this](std::wstring_view openLabel, std::wstring_view startingDirectory)
				-> std::optional<std::wstring> {
				browseCalls.emplace_back(std::wstring(openLabel), std::wstring(startingDirectory));
				if (browseCalls.size() <= browseResults.size()) {
					return browseResults[browseCalls.size() - 1];
				}
				return std::nullopt;
			};
		}
		if (hasPathState) {
			context.pathState = [this](std::wstring_view path) {
				pathStateQueries.emplace_back(path);
				if (pathStateQueries.size() <= pathStateAnswers.size()) {
					return pathStateAnswers[pathStateQueries.size() - 1];
				}
				return EGitPathState::Absent;
			};
		}
		if (hasConfirm) {
			context.confirm = [this](const GitPrompt& prompt) -> std::optional<std::size_t> {
				prompts.push_back(prompt);
				return std::size_t{ 0 };
			};
		}
		context.message = [this](std::wstring_view message) { messages.emplace_back(message); };
		return context;
	}
};

} // namespace

// ----------------------------------------------------------------------------
// git.init: folder resolution.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, BuildGitInitFolderPickItemsAppendsChooseFolderAfterEveryOpenFolder)
{
	const std::vector<GitInitWorkspaceFolder> folders{
		GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" },
		GitInitWorkspaceFolder{ L"docs", L"C:\\repo\\docs" },
	};

	const auto items = BuildGitInitFolderPickItems(folders);

	ASSERT_EQ(3U, items.size());
	EXPECT_EQ(L"app", items[0].label);
	EXPECT_EQ(L"C:\\repo\\app", items[0].path);
	EXPECT_EQ(L"docs", items[1].label);
	EXPECT_EQ(L"C:\\repo\\docs", items[1].path);
	EXPECT_EQ(L"Choose Folder...", items[2].label);
	EXPECT_TRUE(items[2].path.empty());
}

TEST(GitInitCloneCommands, BuildGitInitFolderPickItemsIsJustTheTrailingRowWhenNoFolderIsOpen)
{
	const auto items = BuildGitInitFolderPickItems({});

	ASSERT_EQ(1U, items.size());
	EXPECT_TRUE(items[0].path.empty());
}

TEST(GitInitCloneCommands, IsGitInitHomeDirectoryGuardTriggeredMatchesTheHomeDirectoryItself)
{
	EXPECT_TRUE(IsGitInitHomeDirectoryGuardTriggered(L"C:\\Users\\dev", L"C:\\Users\\dev"));
}

TEST(GitInitCloneCommands, IsGitInitHomeDirectoryGuardTriggeredIsFalseForAnUnrelatedFolder)
{
	EXPECT_FALSE(IsGitInitHomeDirectoryGuardTriggered(L"C:\\Users\\dev", L"C:\\repo\\app"));
}

TEST(GitInitCloneCommands, IsGitInitHomeDirectoryGuardTriggeredIsFalseWithNoHomeDirectory)
{
	EXPECT_FALSE(IsGitInitHomeDirectoryGuardTriggered(L"", L"C:\\Users\\dev"));
}

TEST(GitInitCloneCommands, BuildGitInitArgumentsIsExactlyInit)
{
	EXPECT_EQ(Arguments({ L"init" }), BuildGitInitArguments());
}

TEST(GitInitCloneCommands, BuildGitInitHomeDirectoryPromptNamesThePathAndOffersOneChoice)
{
	const GitPrompt prompt = BuildGitInitHomeDirectoryPrompt(L"C:\\Users\\dev");

	EXPECT_NE(std::wstring::npos, prompt.message.find(L"C:\\Users\\dev"));
	ASSERT_EQ(1U, prompt.choices.size());
	EXPECT_EQ(L"\u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u521d\u671f\u5316", prompt.choices[0]);
	EXPECT_TRUE(prompt.warning);
	EXPECT_TRUE(prompt.modal);
}

// ----------------------------------------------------------------------------
// git.init: `ParseGitInitSkipFolderPromptArgument`.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentIsFalseWithNoArguments)
{
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument(""));
}

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentReadsTrueFromTheWelcomeContentPayload)
{
	EXPECT_TRUE(ParseGitInitSkipFolderPromptArgument("[true]"));
}

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentReadsFalseExplicitly)
{
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("[false]"));
}

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentIsFalseForMalformedJson)
{
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("[tru"));
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("true"));
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("{}"));
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("[null]"));
}

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentIgnoresTrailingWhitespace)
{
	EXPECT_TRUE(ParseGitInitSkipFolderPromptArgument("[true] "));
}

TEST(GitInitCloneCommands, ParseGitInitSkipFolderPromptArgumentIsFalseForTrailingContent)
{
	EXPECT_FALSE(ParseGitInitSkipFolderPromptArgument("[true, false]"));
}

// ----------------------------------------------------------------------------
// git.init: `RunGitInit`, the `skipFolderPrompt` fast path.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, RunGitInitSkipFolderPromptWithOneOpenFolderRunsWithNoPrompting)
{
	FakeGitInit fake;
	fake.responses.push_back(Ok());
	GitInitCommandContext context = fake.Context();
	context.openFolders = { GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" } };

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/true);

	EXPECT_TRUE(result.Succeeded());
	EXPECT_EQ(L"C:\\repo\\app", result.repositoryPath);
	EXPECT_EQ(EGitInitPostAction::AlreadyOpen, result.postAction);
	EXPECT_TRUE(fake.folderPickPlaceholders.empty());
	EXPECT_TRUE(fake.browseCalls.empty());
	EXPECT_TRUE(fake.prompts.empty()); // No home-directory guard on the fast path.
	ASSERT_EQ(1U, fake.invocations.size());
	EXPECT_EQ(L"C:\\repo\\app", fake.invocations[0].first);
	EXPECT_EQ(Arguments({ L"init" }), fake.invocations[0].second);
}

TEST(GitInitCloneCommands, RunGitInitSkipFolderPromptWithNoOpenFolderFallsThroughToThePicker)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::nullopt); // Dismissed.
	GitInitCommandContext context = fake.Context();

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/true);

	EXPECT_EQ(EGitInitCommandStatus::Cancelled, result.status);
	EXPECT_EQ(1U, fake.folderPickPlaceholders.size());
}

TEST(GitInitCloneCommands, RunGitInitSkipFolderPromptWithTwoOpenFoldersFallsThroughToThePicker)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::nullopt);
	GitInitCommandContext context = fake.Context();
	context.openFolders = {
		GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" },
		GitInitWorkspaceFolder{ L"docs", L"C:\\repo\\docs" },
	};

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/true);

	EXPECT_EQ(EGitInitCommandStatus::Cancelled, result.status);
	EXPECT_EQ(1U, fake.folderPickPlaceholders.size());
}

// ----------------------------------------------------------------------------
// git.init: the folder picker path.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, RunGitInitPickingAnOpenFolderSkipsTheHomeDirectoryGuard)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::size_t{ 0 }); // The first open folder.
	fake.responses.push_back(Ok());
	GitInitCommandContext context = fake.Context();
	context.homeDirectory = L"C:\\repo"; // Would trip the guard if it applied.
	context.openFolders = { GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" } };

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_TRUE(result.Succeeded());
	EXPECT_EQ(EGitInitPostAction::AlreadyOpen, result.postAction);
	EXPECT_TRUE(fake.prompts.empty());
}

TEST(GitInitCloneCommands, RunGitInitChooseFolderCancelledAtTheBrowserIsCancelled)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::size_t{ 0 }); // The trailing "Choose Folder..." row.
	fake.browseResults.push_back(std::nullopt); // The user dismissed the browser.
	GitInitCommandContext context = fake.Context();

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_EQ(EGitInitCommandStatus::Cancelled, result.status);
	ASSERT_EQ(1U, fake.browseCalls.size());
	EXPECT_TRUE(fake.invocations.empty());
}

TEST(GitInitCloneCommands, RunGitInitChooseFolderInTheHomeDirectoryAsksForConfirmation)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::size_t{ 0 });
	fake.browseResults.push_back(std::wstring(L"C:\\Users\\dev"));
	fake.confirmations.push_back(std::size_t{ 0 }); // "Initialize Repository".
	fake.responses.push_back(Ok());
	GitInitCommandContext context = fake.Context();
	context.homeDirectory = L"C:\\Users\\dev";

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_TRUE(result.Succeeded());
	EXPECT_EQ(EGitInitPostAction::OfferToOpen, result.postAction);
	ASSERT_EQ(1U, fake.prompts.size());
	EXPECT_TRUE(fake.prompts[0].warning);
	ASSERT_EQ(1U, fake.invocations.size());
	EXPECT_EQ(L"C:\\Users\\dev", fake.invocations[0].first);
}

TEST(GitInitCloneCommands, RunGitInitDecliningTheHomeDirectoryGuardIsCancelledAndRunsNoGit)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::size_t{ 0 });
	fake.browseResults.push_back(std::wstring(L"C:\\Users\\dev"));
	fake.confirmations.push_back(std::nullopt); // Dismissed.
	GitInitCommandContext context = fake.Context();
	context.homeDirectory = L"C:\\Users\\dev";

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_EQ(EGitInitCommandStatus::Cancelled, result.status);
	EXPECT_TRUE(fake.invocations.empty());
}

TEST(GitInitCloneCommands, RunGitInitChooseFolderOutsideTheHomeDirectorySkipsTheGuard)
{
	FakeGitInit fake;
	fake.folderPicks.push_back(std::size_t{ 0 });
	fake.browseResults.push_back(std::wstring(L"C:\\repo\\new"));
	fake.responses.push_back(Ok());
	GitInitCommandContext context = fake.Context();
	context.homeDirectory = L"C:\\Users\\dev";

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_TRUE(result.Succeeded());
	EXPECT_TRUE(fake.prompts.empty());
}

// ----------------------------------------------------------------------------
// git.init: failure surfacing and missing collaborators.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, RunGitInitAGitFailureIsReportedAndSentToMessage)
{
	FakeGitInit fake;
	fake.responses.push_back(Failure("fatal: unable to create directory\n"));
	GitInitCommandContext context = fake.Context();
	context.openFolders = { GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" } };

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/true);

	EXPECT_EQ(EGitInitCommandStatus::Failed, result.status);
	EXPECT_EQ(L"fatal: unable to create directory", result.message);
	ASSERT_EQ(1U, fake.messages.size());
	EXPECT_EQ(L"fatal: unable to create directory", fake.messages[0]);
}

TEST(GitInitCloneCommands, RunGitInitWithNoFolderPickerFailsInsteadOfCrashingWhenPromptingIsNeeded)
{
	FakeGitInit fake;
	fake.hasFolderPick = false;
	GitInitCommandContext context = fake.Context();

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_EQ(EGitInitCommandStatus::Failed, result.status);
}

TEST(GitInitCloneCommands, RunGitInitWithNoBrowserFailsWhenTheTrailingRowIsChosen)
{
	FakeGitInit fake;
	fake.hasBrowseForFolder = false;
	fake.folderPicks.push_back(std::size_t{ 0 });
	GitInitCommandContext context = fake.Context();

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_EQ(EGitInitCommandStatus::Failed, result.status);
}

TEST(GitInitCloneCommands, RunGitInitWithNoConfirmPresenterFailsWhenTheGuardWouldFire)
{
	FakeGitInit fake;
	fake.hasConfirm = false;
	fake.folderPicks.push_back(std::size_t{ 0 });
	fake.browseResults.push_back(std::wstring(L"C:\\Users\\dev"));
	GitInitCommandContext context = fake.Context();
	context.homeDirectory = L"C:\\Users\\dev";

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/false);

	EXPECT_EQ(EGitInitCommandStatus::Failed, result.status);
	EXPECT_TRUE(fake.invocations.empty());
}

TEST(GitInitCloneCommands, RunGitInitWithNoInvokerFails)
{
	FakeGitInit fake;
	fake.hasRun = false;
	GitInitCommandContext context = fake.Context();
	context.openFolders = { GitInitWorkspaceFolder{ L"app", L"C:\\repo\\app" } };

	const GitInitCommandResult result = RunGitInit(context, /*skipFolderPrompt=*/true);

	EXPECT_EQ(EGitInitCommandStatus::Failed, result.status);
}

// ----------------------------------------------------------------------------
// git.clone: folder-name derivation and argument construction.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, DeriveGitCloneFolderNameStripsTheGitSuffix)
{
	EXPECT_EQ(L"sakura-editor-next", DeriveGitCloneFolderName(L"https://example.invalid/tsuyoshi-otake/sakura-editor-next.git"));
}

TEST(GitInitCloneCommands, DeriveGitCloneFolderNameToleratesATrailingSlash)
{
	EXPECT_EQ(L"repo", DeriveGitCloneFolderName(L"https://example.invalid/group/repo/"));
}

TEST(GitInitCloneCommands, DeriveGitCloneFolderNameHandlesAnSshStyleUrlWithBackslashesNormalizedByCaller)
{
	EXPECT_EQ(L"repo", DeriveGitCloneFolderName(L"git@example.invalid:group/repo.git"));
}

TEST(GitInitCloneCommands, DeriveGitCloneFolderNameIsEmptyForAUrlThatNamesNoRepository)
{
	EXPECT_TRUE(DeriveGitCloneFolderName(L"https://example.invalid/.git").empty());
}

TEST(GitInitCloneCommands, BuildGitCloneArgumentsIsCloneUrlThenDestination)
{
	EXPECT_EQ(
		Arguments({ L"clone", L"https://example.invalid/r.git", L"C:\\dest\\r" }),
		BuildGitCloneArguments(L"https://example.invalid/r.git", L"C:\\dest\\r", {}));
}

TEST(GitInitCloneCommands, BuildGitCloneArgumentsAppendsRecurseSubmodulesWhenRequested)
{
	GitCloneOptions options;
	options.recurseSubmodules = true;

	EXPECT_EQ(
		Arguments({ L"clone", L"u", L"d", L"--recurse-submodules" }),
		BuildGitCloneArguments(L"u", L"d", options));
}

TEST(GitInitCloneCommands, BuildGitCloneOverwritePromptNamesTheFolder)
{
	const GitPrompt prompt = BuildGitCloneOverwritePrompt(L"sakura-editor-next");

	EXPECT_NE(std::wstring::npos, prompt.message.find(L"sakura-editor-next"));
	EXPECT_TRUE(prompt.warning);
}

// ----------------------------------------------------------------------------
// git.clone: phase 1, `RunGitClonePrepare`.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, RunGitClonePrepareResolvesTheUrlAndDestination)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/r.git"));
	fake.browseResults.push_back(std::wstring(L"C:\\dest"));
	fake.pathStateAnswers.push_back(EGitPathState::Absent);
	GitCloneCommandContext context = fake.Context();

	const std::optional<GitCloneRequest> request = RunGitClonePrepare(context);

	ASSERT_TRUE(request.has_value());
	EXPECT_EQ(L"https://example.invalid/r.git", request->url);
	EXPECT_EQ(L"C:\\dest\\r", request->destinationPath);
}

TEST(GitInitCloneCommands, RunGitClonePrepareAcceptsAnEmptyExistingDirectory)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/r.git"));
	fake.browseResults.push_back(std::wstring(L"C:\\dest"));
	fake.pathStateAnswers.push_back(EGitPathState::EmptyDirectory);
	GitCloneCommandContext context = fake.Context();

	const std::optional<GitCloneRequest> request = RunGitClonePrepare(context);

	EXPECT_TRUE(request.has_value());
}

TEST(GitInitCloneCommands, RunGitClonePrepareCancelledAtTheUrlPromptReturnsNothing)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::nullopt);
	GitCloneCommandContext context = fake.Context();

	EXPECT_FALSE(RunGitClonePrepare(context).has_value());
	EXPECT_TRUE(fake.browseCalls.empty()); // Never asked where, having no URL yet.
}

TEST(GitInitCloneCommands, RunGitClonePrepareCancelledAtTheDestinationBrowserReturnsNothing)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/r.git"));
	fake.browseResults.push_back(std::nullopt);
	GitCloneCommandContext context = fake.Context();

	EXPECT_FALSE(RunGitClonePrepare(context).has_value());
}

TEST(GitInitCloneCommands, RunGitClonePrepareRefusesANonEmptyDestinationAndReportsWhy)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/r.git"));
	fake.browseResults.push_back(std::wstring(L"C:\\dest"));
	fake.pathStateAnswers.push_back(EGitPathState::NonEmpty);
	GitCloneCommandContext context = fake.Context();

	const std::optional<GitCloneRequest> request = RunGitClonePrepare(context);

	EXPECT_FALSE(request.has_value());
	ASSERT_EQ(1U, fake.messages.size());
	EXPECT_NE(std::wstring::npos, fake.messages[0].find(L"r"));
	// No destructive delete primitive exists on this path, so the overwrite
	// prompt is never presented for an answer this code could not honor.
	EXPECT_TRUE(fake.prompts.empty());
}

TEST(GitInitCloneCommands, RunGitClonePrepareRefusesAUrlWithNoDerivableFolderName)
{
	FakeGitClone fake;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/.git"));
	GitCloneCommandContext context = fake.Context();

	EXPECT_FALSE(RunGitClonePrepare(context).has_value());
	EXPECT_TRUE(fake.browseCalls.empty());
}

TEST(GitInitCloneCommands, RunGitClonePrepareWithNoUrlPresenterReturnsNothing)
{
	FakeGitClone fake;
	fake.hasPromptForUrl = false;
	GitCloneCommandContext context = fake.Context();

	EXPECT_FALSE(RunGitClonePrepare(context).has_value());
}

TEST(GitInitCloneCommands, RunGitClonePrepareWithNoBrowserReturnsNothing)
{
	FakeGitClone fake;
	fake.hasBrowseForParentDirectory = false;
	fake.urlAnswers.push_back(std::wstring(L"https://example.invalid/r.git"));
	GitCloneCommandContext context = fake.Context();

	EXPECT_FALSE(RunGitClonePrepare(context).has_value());
}

// ----------------------------------------------------------------------------
// git.clone: phase 2/3, execute and complete.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, RunGitCloneExecuteForwardsTheArgumentsAndTheStopHandle)
{
	const GitCloneRequest request{ L"https://example.invalid/r.git", L"C:\\dest\\r" };
	std::vector<Arguments> seenArguments;
	std::vector<HANDLE> seenStopHandles;
	// A real-looking, never-dereferenced HANDLE value stands in for a real
	// cancellation event; the model only forwards it.
	HANDLE stop = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234));

	const GitExecutionResult result = RunGitCloneExecute(
		request, {},
		[&](const Arguments& arguments, HANDLE stopHandle) {
			seenArguments.push_back(arguments);
			seenStopHandles.push_back(stopHandle);
			return Ok();
		},
		stop);

	EXPECT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, seenArguments.size());
	EXPECT_EQ(Arguments({ L"clone", L"https://example.invalid/r.git", L"C:\\dest\\r" }), seenArguments[0]);
	ASSERT_EQ(1U, seenStopHandles.size());
	EXPECT_EQ(stop, seenStopHandles[0]);
}

TEST(GitInitCloneCommands, RunGitCloneExecuteWithNoInvokerReportsInvalidRequest)
{
	const GitCloneRequest request{ L"u", L"d" };

	const GitExecutionResult result = RunGitCloneExecute(request, {}, GitCloneInvoker{}, nullptr);

	EXPECT_EQ(EGitExecutionStatus::InvalidRequest, result.status);
}

TEST(GitInitCloneCommands, RunGitCloneCompleteOnSuccessNamesTheDestinationAsTheRepository)
{
	const GitCloneRequest request{ L"u", L"C:\\dest\\r" };

	const GitCloneCommandResult result = RunGitCloneComplete(request, Ok());

	EXPECT_TRUE(result.Succeeded());
	EXPECT_EQ(L"C:\\dest\\r", result.repositoryPath);
}

TEST(GitInitCloneCommands, RunGitCloneCompleteOnFailureCarriesGitsOwnMessage)
{
	const GitCloneRequest request{ L"u", L"d" };

	const GitCloneCommandResult result = RunGitCloneComplete(request, Failure("fatal: could not read Username\n"));

	EXPECT_EQ(EGitCloneCommandStatus::Failed, result.status);
	EXPECT_EQ(L"fatal: could not read Username", result.message);
}

TEST(GitInitCloneCommands, RunGitCloneCompleteOnCancellationIsCancelledNotFailed)
{
	const GitCloneRequest request{ L"u", L"d" };

	const GitCloneCommandResult result = RunGitCloneComplete(request, CancelledExecution());

	EXPECT_EQ(EGitCloneCommandStatus::Cancelled, result.status);
	EXPECT_FALSE(result.message.empty());
}

// ----------------------------------------------------------------------------
// The Source Control empty-state welcome model.
// ----------------------------------------------------------------------------

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelIsNoneWithARepositoryOpen)
{
	const GitScmWelcomeModel model = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::Folder, /*hasRepository=*/true);

	EXPECT_EQ(EGitScmWelcomeContent::None, model.content);
	EXPECT_TRUE(model.actions.empty());
}

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelOffersInitWithAnOpenFolderAndNoRepository)
{
	const GitScmWelcomeModel model = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::Folder, /*hasRepository=*/false);

	EXPECT_EQ(EGitScmWelcomeContent::FolderNoRepository, model.content);
	ASSERT_EQ(1U, model.actions.size());
	EXPECT_EQ("git.init", model.actions[0].command);
	EXPECT_EQ("[true]", model.actions[0].argumentsJson);
	EXPECT_EQ(L"\u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u521d\u671f\u5316", model.actions[0].label);
}

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelOffersOpenFolderThenCloneWithNoFolderOpen)
{
	const GitScmWelcomeModel model = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::Empty, /*hasRepository=*/false);

	EXPECT_EQ(EGitScmWelcomeContent::EmptyWorkbench, model.content);
	ASSERT_EQ(2U, model.actions.size());
	EXPECT_EQ("vscode.openFolder", model.actions[0].command);
	EXPECT_TRUE(model.actions[0].argumentsJson.empty());
	EXPECT_EQ(L"Open Folder", model.actions[0].label);
	EXPECT_EQ("git.cloneRecursive", model.actions[1].command);
	EXPECT_TRUE(model.actions[1].argumentsJson.empty());
	EXPECT_EQ(L"Clone Repository", model.actions[1].label);
}

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelOffersInitForWorkspaceWithFolders)
{
	const GitScmWelcomeModel model = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::WorkspaceWithFolders, /*hasRepository=*/false);

	EXPECT_EQ(EGitScmWelcomeContent::WorkspaceNoRepository, model.content);
	ASSERT_EQ(1U, model.actions.size());
	EXPECT_EQ("git.init", model.actions[0].command);
	EXPECT_TRUE(model.actions[0].argumentsJson.empty());
	EXPECT_EQ(L"\u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u521d\u671f\u5316", model.actions[0].label);
}

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelOffersAddFolderForEmptyWorkspace)
{
	const GitScmWelcomeModel model = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::WorkspaceWithoutFolders, /*hasRepository=*/false);

	EXPECT_EQ(EGitScmWelcomeContent::EmptyWorkspace, model.content);
	ASSERT_EQ(1U, model.actions.size());
	EXPECT_EQ("workbench.action.addRootFolder", model.actions[0].command);
	EXPECT_TRUE(model.actions[0].argumentsJson.empty());
	EXPECT_EQ(L"Add Folder to Workspace", model.actions[0].label);
}

TEST(GitInitCloneCommands, BuildGitScmWelcomeModelInitAndCloneAreMutuallyExclusive)
{
	// Byte-verified against `extensions/git/package.json` at tag 1.95.3:
	// The four upstream viewsWelcome contributions key off the explicit
	// workbench state, so each state has its own content and action contract.
	const GitScmWelcomeModel folderModel = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::Folder, false);
	const GitScmWelcomeModel workspaceModel = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::WorkspaceWithFolders, false);
	const GitScmWelcomeModel emptyWorkspaceModel = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::WorkspaceWithoutFolders, false);
	const GitScmWelcomeModel emptyModel = BuildGitScmWelcomeModel(
		EGitScmWelcomeWorkspaceState::Empty, false);

	EXPECT_NE(folderModel.content, emptyModel.content);
	for (const GitScmWelcomeAction& action : folderModel.actions) {
		EXPECT_NE("git.clone", action.command);
	}
	for (const GitScmWelcomeAction& action : workspaceModel.actions) EXPECT_EQ("git.init", action.command);
	for (const GitScmWelcomeAction& action : emptyWorkspaceModel.actions) {
		EXPECT_NE("git.init", action.command);
		EXPECT_NE("git.cloneRecursive", action.command);
	}
	for (const GitScmWelcomeAction& action : emptyModel.actions) {
		EXPECT_NE("git.init", action.command);
		EXPECT_NE("workbench.action.addRootFolder", action.command);
	}
}
