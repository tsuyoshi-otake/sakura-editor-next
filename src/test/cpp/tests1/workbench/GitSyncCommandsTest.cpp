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
#include <vector>

#include "workbench/scm/GitFailureText.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/GitSyncCommands.h"

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
	// that ran and refused reports `Succeeded` with a non-zero exit code, which
	// is exactly the case every classifier below must handle.
	result.status = EGitExecutionStatus::Succeeded;
	result.exitCode = 1;
	result.standardOutput.assign(standardOutput.begin(), standardOutput.end());
	result.standardError.assign(standardError);
	return result;
}

//! Records every invocation and answers from a scripted queue.
class FakeGit final
{
public:
	std::vector<Arguments> invocations;
	std::vector<GitExecutionResult> responses;
	std::vector<std::wstring> messages;
	std::vector<GitPrompt> prompts;
	std::vector<std::wstring> placeholders;
	//! What `confirm` returns, consumed in order. Nothing means dismissal.
	std::vector<std::optional<std::size_t>> confirmations;
	std::vector<std::optional<std::size_t>> picks;

	[[nodiscard]] GitSyncCommandContext Context()
	{
		GitSyncCommandContext context;
		context.run = [this](const Arguments& arguments) {
			invocations.push_back(arguments);
			if (invocations.size() <= responses.size()) {
				return responses[invocations.size() - 1];
			}
			return Ok();
		};
		context.message = [this](std::wstring_view message) { messages.emplace_back(message); };
		context.confirm = [this](const GitPrompt& prompt) -> std::optional<std::size_t> {
			prompts.push_back(prompt);
			if (prompts.size() <= confirmations.size()) {
				return confirmations[prompts.size() - 1];
			}
			return std::size_t{ 0 };
		};
		context.pickRemote = [this](const std::vector<GitRemotePickItem>& items, std::wstring_view placeholder)
			-> std::optional<std::size_t> {
			placeholders.emplace_back(placeholder);
			(void)items;
			if (placeholders.size() <= picks.size()) {
				return picks[placeholders.size() - 1];
			}
			return std::size_t{ 0 };
		};
		return context;
	}
};

[[nodiscard]] GitRemote Remote(std::wstring name, std::wstring fetchUrl, std::wstring pushUrl)
{
	GitRemote remote;
	remote.name = std::move(name);
	remote.fetchUrl = std::move(fetchUrl);
	remote.pushUrl = std::move(pushUrl);
	remote.isReadOnly = remote.pushUrl.empty() || remote.pushUrl == L"no_push";
	return remote;
}

[[nodiscard]] GitSyncRepositoryState TrackedState()
{
	GitSyncRepositoryState state;
	state.headName = L"main";
	state.upstreamRemote = L"origin";
	state.upstreamName = L"main";
	state.ahead = 1;
	state.remotes.push_back(Remote(L"origin", L"https://example.invalid/r.git", L"https://example.invalid/r.git"));
	return state;
}

} // namespace

// ----------------------------------------------------------------------------
// `getRemotesGit`.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, BuildGitRemoteArgumentsAsksForTheVerboseListing)
{
	EXPECT_EQ(Arguments({ L"remote", L"--verbose" }), BuildGitRemoteArguments());
}

TEST(GitSyncCommands, ParseGitRemotesCollapsesTheFetchAndPushRowsIntoOneRemote)
{
	const auto remotes = ParseGitRemotes(
		"origin\thttps://example.invalid/fetch.git (fetch)\n"
		"origin\thttps://example.invalid/push.git (push)\n");

	ASSERT_EQ(1U, remotes.size());
	EXPECT_EQ(L"origin", remotes[0].name);
	EXPECT_EQ(L"https://example.invalid/fetch.git", remotes[0].fetchUrl);
	EXPECT_EQ(L"https://example.invalid/push.git", remotes[0].pushUrl);
	EXPECT_FALSE(remotes[0].isReadOnly);
}

TEST(GitSyncCommands, ParseGitRemotesKeepsRemoteOrderAndSeparatesNames)
{
	const auto remotes = ParseGitRemotes(
		"origin\thttps://example.invalid/o.git (fetch)\n"
		"origin\thttps://example.invalid/o.git (push)\n"
		"upstream\thttps://example.invalid/u.git (fetch)\n"
		"upstream\thttps://example.invalid/u.git (push)\n");

	ASSERT_EQ(2U, remotes.size());
	EXPECT_EQ(L"origin", remotes[0].name);
	EXPECT_EQ(L"upstream", remotes[1].name);
}

TEST(GitSyncCommands, ParseGitRemotesTreatsAMissingPushUrlAsReadOnly)
{
	// Only the fetch row exists, so nothing may be pushed to this remote.
	const auto remotes = ParseGitRemotes("mirror\thttps://example.invalid/m.git (fetch)\n");

	ASSERT_EQ(1U, remotes.size());
	EXPECT_TRUE(remotes[0].isReadOnly);
	EXPECT_TRUE(remotes[0].pushUrl.empty());
}

TEST(GitSyncCommands, ParseGitRemotesTreatsTheNoPushSentinelAsReadOnly)
{
	const auto remotes = ParseGitRemotes(
		"blocked\thttps://example.invalid/b.git (fetch)\n"
		"blocked\tno_push (push)\n");

	ASSERT_EQ(1U, remotes.size());
	EXPECT_TRUE(remotes[0].isReadOnly);
}

TEST(GitSyncCommands, ParseGitRemotesGivesAnUntypedRowBothUrls)
{
	const auto remotes = ParseGitRemotes("plain\thttps://example.invalid/p.git\n");

	ASSERT_EQ(1U, remotes.size());
	EXPECT_EQ(L"https://example.invalid/p.git", remotes[0].fetchUrl);
	EXPECT_EQ(L"https://example.invalid/p.git", remotes[0].pushUrl);
	EXPECT_FALSE(remotes[0].isReadOnly);
}

TEST(GitSyncCommands, FindGitRemoteAnswersNullForAnAbsentOrEmptyName)
{
	const std::vector<GitRemote> remotes{ Remote(L"origin", L"f", L"p") };

	ASSERT_NE(nullptr, FindGitRemote(remotes, L"origin"));
	EXPECT_EQ(L"origin", FindGitRemote(remotes, L"origin")->name);
	EXPECT_EQ(nullptr, FindGitRemote(remotes, L"upstream"));
	EXPECT_EQ(nullptr, FindGitRemote(remotes, L""));
}

// ----------------------------------------------------------------------------
// Argument construction, in upstream's exact order.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, BuildGitFetchArgumentsPrefersANamedRemoteOverAll)
{
	GitFetchOptions options;
	options.remote = L"origin";
	options.ref = L"main";
	options.all = true;

	// `--all` is in an `else if`, so naming a remote suppresses it.
	EXPECT_EQ(Arguments({ L"fetch", L"origin", L"main" }), BuildGitFetchArguments(options));
}

TEST(GitSyncCommands, BuildGitFetchArgumentsEmitsAllAndPrune)
{
	GitFetchOptions options;
	options.all = true;
	options.prune = true;

	EXPECT_EQ(Arguments({ L"fetch", L"--all", L"--prune" }), BuildGitFetchArguments(options));
}

TEST(GitSyncCommands, BuildGitFetchArgumentsIgnoresARefWithNoRemote)
{
	GitFetchOptions options;
	options.ref = L"main";

	EXPECT_EQ(Arguments({ L"fetch" }), BuildGitFetchArguments(options));
}

TEST(GitSyncCommands, BuildGitPullArgumentsKeepsUpstreamsFlagOrder)
{
	GitPullOptions options;
	options.tags = true;
	options.unshallow = true;
	options.autoStash = true;

	EXPECT_EQ(
		Arguments({ L"pull", L"--tags", L"--unshallow", L"--autostash", L"-r", L"origin", L"main" }),
		BuildGitPullArguments(true, L"origin", L"main", options));
}

TEST(GitSyncCommands, BuildGitPullArgumentsOmitsAHalfNamedRefspec)
{
	// A remote with no branch would make git guess, which is the ambiguity the
	// pair exists to remove.
	EXPECT_EQ(Arguments({ L"pull" }), BuildGitPullArguments(false, L"origin", L"", {}));
	EXPECT_EQ(Arguments({ L"pull" }), BuildGitPullArguments(false, L"", L"main", {}));
}

TEST(GitSyncCommands, BuildGitPushArgumentsKeepsUpstreamsFlagOrder)
{
	GitPushOptions options;
	options.remote = L"origin";
	options.name = L"main:main";
	options.setUpstream = true;
	options.followTags = true;

	EXPECT_EQ(
		Arguments({ L"push", L"-u", L"--follow-tags", L"origin", L"main:main" }),
		BuildGitPushArguments(options));
}

TEST(GitSyncCommands, BuildGitPushArgumentsPairsForceIfIncludesWithForceWithLease)
{
	GitPushOptions options;
	options.forcePushMode = EGitForcePushMode::ForceWithLeaseIfIncludes;

	EXPECT_EQ(Arguments({ L"push", L"--force-with-lease", L"--force-if-includes" }), BuildGitPushArguments(options));

	options.forcePushMode = EGitForcePushMode::ForceWithLease;
	EXPECT_EQ(Arguments({ L"push", L"--force-with-lease" }), BuildGitPushArguments(options));

	options.forcePushMode = EGitForcePushMode::Force;
	EXPECT_EQ(Arguments({ L"push", L"--force" }), BuildGitPushArguments(options));
}

TEST(GitSyncCommands, BuildGitPushArgumentsOmitsEmptyOperands)
{
	// This is what makes an untracked branch reach git as a bare `git push`, so
	// git itself — not a guess here — reports that there is no upstream.
	EXPECT_EQ(Arguments({ L"push" }), BuildGitPushArguments({}));
}

TEST(GitSyncCommands, BuildMaybeRebasedArgumentsAsksForTheCherryRange)
{
	EXPECT_EQ(
		Arguments({ L"log", L"--oneline", L"--cherry", L"main...main@{upstream}", L"--" }),
		BuildMaybeRebasedArguments(L"main"));
}

TEST(GitSyncCommands, ParseMaybeRebasedReadsOnlyTheLeadingEqualsSign)
{
	EXPECT_TRUE(ParseMaybeRebased(Ok("= 1234567 a commit\n")));
	EXPECT_FALSE(ParseMaybeRebased(Ok("+ 1234567 a commit\n")));
	EXPECT_FALSE(ParseMaybeRebased(Ok()));
	// A probe that could not run is not evidence of a rebase.
	EXPECT_FALSE(ParseMaybeRebased(Failure("fatal: no upstream configured")));
}

// ----------------------------------------------------------------------------
// Classification: each operation's own `catch`, then the generic ladder.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, ClassifyGitFetchFailureRecognizesItsOwnThreeCauses)
{
	EXPECT_EQ(
		EGitSyncFailureReason::NoRemoteRepositorySpecified,
		ClassifyGitFetchFailure(Failure("fatal: No remote repository specified.")));
	EXPECT_EQ(
		EGitSyncFailureReason::RemoteConnectionError,
		ClassifyGitFetchFailure(Failure("fatal: Could not read from remote repository.")));
	EXPECT_EQ(
		EGitSyncFailureReason::BranchFastForwardRejected,
		ClassifyGitFetchFailure(Failure(" ! [rejected]        main -> main  (non-fast-forward)\n")));
}

TEST(GitSyncCommands, ClassifyGitPullFailureReadsTheConflictReportFromStandardOutput)
{
	// git writes `CONFLICT (...)` to stdout, so a classifier that only read
	// stderr would report a merge conflict as an unrecognized failure.
	EXPECT_EQ(
		EGitSyncFailureReason::Conflict,
		ClassifyGitPullFailure(Failure("", "Auto-merging a.txt\nCONFLICT (content): Merge conflict in a.txt\n")));
}

TEST(GitSyncCommands, ClassifyGitPullFailureRecognizesItsOwnCauses)
{
	EXPECT_EQ(
		EGitSyncFailureReason::NoUserNameConfigured,
		ClassifyGitPullFailure(Failure("*** Please tell me who you are.")));
	EXPECT_EQ(
		EGitSyncFailureReason::DirtyWorkTree,
		ClassifyGitPullFailure(Failure("error: Cannot pull with rebase: You have unstaged changes.")));
	EXPECT_EQ(
		EGitSyncFailureReason::CantLockRef, ClassifyGitPullFailure(Failure("error: cannot lock ref 'refs/heads/main'")));
	EXPECT_EQ(
		EGitSyncFailureReason::CantRebaseMultipleBranches,
		ClassifyGitPullFailure(Failure("fatal: Cannot rebase onto multiple branches.")));
	EXPECT_EQ(
		EGitSyncFailureReason::TagConflict,
		ClassifyGitPullFailure(Failure(" ! [rejected]  v1 -> v1  (would clobber existing tag)\n")));
}

TEST(GitSyncCommands, ClassifyGitPushFailureSeparatesRejectionFromLeaseStaleness)
{
	const auto rejected = Failure(
		"To https://example.invalid/r.git\n"
		" ! [rejected]        main -> main (stale info)\n"
		"error: failed to push some refs to 'https://example.invalid/r.git'\n");

	// The same stderr means different things depending on how it was pushed.
	EXPECT_EQ(EGitSyncFailureReason::PushRejected, ClassifyGitPushFailure(rejected, EGitForcePushMode::None));
	EXPECT_EQ(
		EGitSyncFailureReason::ForcePushWithLeaseRejected,
		ClassifyGitPushFailure(rejected, EGitForcePushMode::ForceWithLease));
}

TEST(GitSyncCommands, ClassifyGitPushFailureRecognizesPermissionAndNoUpstream)
{
	EXPECT_EQ(
		EGitSyncFailureReason::PermissionDenied,
		ClassifyGitPushFailure(Failure("Permission to r.git denied to nobody."), EGitForcePushMode::None));
	EXPECT_EQ(
		EGitSyncFailureReason::NoUpstreamBranch,
		ClassifyGitPushFailure(
			Failure("fatal: The current branch topic has no upstream branch.\n"), EGitForcePushMode::None));
}

TEST(GitSyncCommands, ClassifyGitPushFailureIgnoresANoUpstreamSentenceBelowTheFirstLine)
{
	// Upstream's pattern carries no `m` flag, so only the first line can match.
	EXPECT_NE(
		EGitSyncFailureReason::NoUpstreamBranch,
		ClassifyGitPushFailure(
			Failure("warning: something else\nfatal: The current branch topic has no upstream branch.\n"),
			EGitForcePushMode::None));
}

TEST(GitSyncCommands, ClassifyGitFetchFailureFallsBackToTheGenericLadder)
{
	EXPECT_EQ(
		EGitSyncFailureReason::AuthenticationFailed,
		ClassifyGitFetchFailure(Failure("fatal: Authentication failed for 'https://example.invalid/r.git/'")));
	EXPECT_EQ(
		EGitSyncFailureReason::RepositoryIsLocked,
		ClassifyGitFetchFailure(Failure("fatal: Another git process seems to be running in this repository")));
	EXPECT_EQ(
		EGitSyncFailureReason::RepositoryNotFound, ClassifyGitFetchFailure(Failure("remote: Repository not found.")));
	EXPECT_EQ(
		EGitSyncFailureReason::NotASafeGitRepository,
		ClassifyGitFetchFailure(Failure("fatal: detected dubious ownership in repository at 'C:/r'")));
	EXPECT_EQ(EGitSyncFailureReason::Other, ClassifyGitFetchFailure(Failure("fatal: something new")));
}

// ----------------------------------------------------------------------------
// Recovery and message rendering.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, RecoveryForGitSyncFailureRoutesEachCauseToItsAction)
{
	EXPECT_EQ(EGitSyncRecovery::Authenticate, RecoveryForGitSyncFailure(EGitSyncFailureReason::AuthenticationFailed));
	EXPECT_EQ(EGitSyncRecovery::Authenticate, RecoveryForGitSyncFailure(EGitSyncFailureReason::PermissionDenied));
	EXPECT_EQ(EGitSyncRecovery::PullThenRetry, RecoveryForGitSyncFailure(EGitSyncFailureReason::PushRejected));
	EXPECT_EQ(EGitSyncRecovery::ResolveConflicts, RecoveryForGitSyncFailure(EGitSyncFailureReason::Conflict));
	EXPECT_EQ(EGitSyncRecovery::CleanWorkingTree, RecoveryForGitSyncFailure(EGitSyncFailureReason::DirtyWorkTree));
	EXPECT_EQ(
		EGitSyncRecovery::ConfigureIdentity, RecoveryForGitSyncFailure(EGitSyncFailureReason::NoUserNameConfigured));
	EXPECT_EQ(EGitSyncRecovery::PublishBranch, RecoveryForGitSyncFailure(EGitSyncFailureReason::NoUpstreamBranch));
	EXPECT_EQ(EGitSyncRecovery::RetryLater, RecoveryForGitSyncFailure(EGitSyncFailureReason::RepositoryIsLocked));
	EXPECT_EQ(EGitSyncRecovery::None, RecoveryForGitSyncFailure(EGitSyncFailureReason::None));
	EXPECT_EQ(EGitSyncRecovery::None, RecoveryForGitSyncFailure(EGitSyncFailureReason::Other));
}

TEST(GitSyncCommands, DescribeGitSyncFailureQuotesTheRemoteAuthenticationFailedFor)
{
	const auto failure = DescribeGitSyncFailure(
		EGitSyncFailureReason::AuthenticationFailed,
		Failure("fatal: Authentication failed for 'https://example.invalid/r.git/'"));

	EXPECT_EQ(EGitSyncRecovery::Authenticate, failure.recovery);
	EXPECT_EQ(L"Failed to authenticate to git remote:\n\nhttps://example.invalid/r.git/", failure.message);
}

TEST(GitSyncCommands, DescribeGitSyncFailureUsesUpstreamsOwnSentences)
{
	EXPECT_EQ(
		L"Can't push refs to remote. Try running \"Pull\" first to integrate your changes.",
		DescribeGitSyncFailure(EGitSyncFailureReason::PushRejected, Failure("error: failed to push some refs to x"))
			.message);
	EXPECT_EQ(
		L"There are merge conflicts. Please resolve them before committing your changes.",
		DescribeGitSyncFailure(EGitSyncFailureReason::Conflict, Failure("", "CONFLICT (content): x")).message);
	EXPECT_EQ(
		L"Make sure you configure your \"user.name\" and \"user.email\" in git.",
		DescribeGitSyncFailure(EGitSyncFailureReason::NoUserNameConfigured, Failure("Please tell me who you are."))
			.message);
}

TEST(GitSyncCommands, DescribeGitSyncFailureFallsBackToTheFirstStderrHintLine)
{
	const auto failure = DescribeGitSyncFailure(
		EGitSyncFailureReason::Other, Failure("error: the real reason\nhint: an unrelated suggestion\n"));

	EXPECT_EQ(L"Git: the real reason", failure.message);
}

TEST(GitSyncCommands, DescribeGitSyncFailurePrefersTheLastLineWhenGitAlsoWroteStandardOutput)
{
	// A hook echoes first and states its verdict last, so with stdout present
	// upstream reads the end of the report rather than its beginning.
	const auto failure
		= DescribeGitSyncFailure(EGitSyncFailureReason::Other, Failure("first line\nthe verdict\n", "hook output\n"));

	EXPECT_EQ(L"Git: the verdict", failure.message);
}

TEST(GitSyncCommands, DescribeGitSyncFailureDefersToTheSharedRendererWhenGitNeverRan)
{
	GitExecutionResult result;
	result.status = EGitExecutionStatus::GitUnavailable;

	const auto failure = DescribeGitSyncFailure(EGitSyncFailureReason::Other, result);

	// The shared renderer owns "git is not installed"; this path must not
	// invent a sentence for a process that produced no output at all.
	EXPECT_EQ(DescribeGitFailure(result), failure.message);
	EXPECT_EQ(L"Git was not found on PATH.", failure.message);
}

// ----------------------------------------------------------------------------
// Repository state.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, SplitGitUpstreamSplitsAtTheFirstSlash)
{
	std::wstring remote;
	std::wstring name;

	ASSERT_TRUE(SplitGitUpstream(L"origin/feature/a", remote, name));
	EXPECT_EQ(L"origin", remote);
	EXPECT_EQ(L"feature/a", name);

	std::wstring untouchedRemote = L"kept";
	std::wstring untouchedName = L"kept";
	EXPECT_FALSE(SplitGitUpstream(L"origin", untouchedRemote, untouchedName));
	EXPECT_EQ(L"kept", untouchedRemote);
	EXPECT_EQ(L"kept", untouchedName);
}

TEST(GitSyncCommands, BuildGitSyncRepositoryStateCarriesTheDivergenceAndUpstream)
{
	GitScmState scm;
	scm.repository = true;
	scm.branch = L"main";
	scm.upstream = L"origin/main";
	scm.ahead = 2;
	scm.behind = 3;

	const auto state = BuildGitSyncRepositoryState(scm, { Remote(L"origin", L"f", L"p") });

	EXPECT_EQ(L"main", state.headName);
	EXPECT_EQ(L"origin", state.upstreamRemote);
	EXPECT_EQ(L"main", state.upstreamName);
	EXPECT_EQ(2, state.ahead);
	EXPECT_EQ(3, state.behind);
	EXPECT_TRUE(state.HasUpstream());
}

TEST(GitSyncCommands, BuildGitSyncRepositoryStateLeavesANonRepositoryEmpty)
{
	GitScmState scm;
	scm.branch = L"main";
	scm.upstream = L"origin/main";

	const auto state = BuildGitSyncRepositoryState(scm, { Remote(L"origin", L"f", L"p") });

	EXPECT_TRUE(state.headName.empty());
	EXPECT_FALSE(state.HasUpstream());
}

TEST(GitSyncCommands, BuildGitSyncRepositoryStateTreatsADetachedHeadAsUntracked)
{
	GitScmState scm;
	scm.repository = true;
	scm.commit = L"1234567";

	const auto state = BuildGitSyncRepositoryState(scm, {});

	EXPECT_TRUE(state.headName.empty());
	EXPECT_FALSE(state.HasUpstream());
}

// ----------------------------------------------------------------------------
// Picks and prompts.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, BuildFetchRemotePickItemsMovesTheUpstreamRemoteToTheFront)
{
	GitSyncRepositoryState state;
	state.headName = L"main";
	state.upstreamRemote = L"upstream";
	state.upstreamName = L"main";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));
	state.remotes.push_back(Remote(L"upstream", L"u-fetch", L"u-push"));

	const auto items = BuildFetchRemotePickItems(state);

	ASSERT_EQ(3U, items.size());
	EXPECT_EQ(L"$(cloud) upstream", items[0].label);
	EXPECT_EQ(L"u-fetch", items[0].description);
	EXPECT_EQ(L"$(cloud) origin", items[1].label);
	EXPECT_EQ(EGitRemotePickKind::AllRemotes, items[2].kind);
	EXPECT_EQ(L"$(cloud-download) Fetch all remotes", items[2].label);
}

TEST(GitSyncCommands, BuildPublishRemotePickItemsOffersEveryRemoteIncludingAReadOnlyOne)
{
	// Upstream's publish pick does not filter by push URL, so the row stays and
	// git reports the refusal instead of the remote silently disappearing.
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));
	state.remotes.push_back(Remote(L"mirror", L"m-fetch", L""));

	const auto items = BuildPublishRemotePickItems(state);

	ASSERT_EQ(2U, items.size());
	EXPECT_EQ(L"origin", items[0].label);
	EXPECT_EQ(L"o-push", items[0].description);
	EXPECT_EQ(L"mirror", items[1].label);
}

TEST(GitSyncCommands, PromptsCarryUpstreamsWordingAndOmitTheSettingsWritingButtons)
{
	const auto sync = BuildSyncConfirmationPrompt(L"origin", L"main");
	EXPECT_EQ(L"This action will pull and push commits from and to \"origin/main\".", sync.message);
	ASSERT_EQ(1U, sync.choices.size());
	EXPECT_EQ(L"OK", sync.choices[0]);
	EXPECT_TRUE(sync.modal);

	const auto publish = BuildPublishBranchPrompt(L"topic");
	EXPECT_EQ(L"The branch \"topic\" has no remote branch. Would you like to publish this branch?", publish.message);
	ASSERT_EQ(1U, publish.choices.size());

	const auto rebased = BuildMaybeRebasedPrompt(L"main");
	EXPECT_EQ(
		L"It looks like the current branch \"main\" might have been rebased. Are you sure you still want to pull "
		L"into it?",
		rebased.message);
	ASSERT_EQ(2U, rebased.choices.size());
	EXPECT_EQ(L"Pull", rebased.choices[0]);
	EXPECT_EQ(L"Don't Pull", rebased.choices[1]);
	// `Always Pull` writes `git.ignoreRebaseWarning`; there is no writer here.
	EXPECT_FALSE(rebased.modal);
}

// ----------------------------------------------------------------------------
// Orchestration.
// ----------------------------------------------------------------------------

TEST(GitSyncCommands, RunGitFetchWarnsAndStopsWithNoRemotes)
{
	FakeGit git;
	const auto result = RunGitFetch(git.Context(), {}, EGitFetchScope::Default);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	EXPECT_TRUE(git.invocations.empty());
	ASSERT_EQ(1U, git.messages.size());
	EXPECT_EQ(L"This repository has no remotes configured to fetch from.", git.messages[0]);
}

TEST(GitSyncCommands, RunGitFetchDoesNotAskWhenThereIsOnlyOneRemote)
{
	FakeGit git;
	const auto result = RunGitFetch(git.Context(), TrackedState(), EGitFetchScope::Default);

	EXPECT_TRUE(result.Succeeded());
	EXPECT_TRUE(git.placeholders.empty());
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"fetch" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitFetchAsksWhichRemoteWhenThereIsMoreThanOne)
{
	auto state = TrackedState();
	state.remotes.push_back(Remote(L"upstream", L"u-fetch", L"u-push"));

	FakeGit git;
	git.picks.push_back(std::size_t{ 1 });

	const auto result = RunGitFetch(git.Context(), state, EGitFetchScope::Default);

	EXPECT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, git.placeholders.size());
	EXPECT_EQ(L"Select a remote to fetch", git.placeholders[0]);
	// The upstream remote was moved to the front, so index 1 is the other one.
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"fetch", L"upstream" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitFetchReachesTheAllScopeThroughTheFetchAllRow)
{
	auto state = TrackedState();
	state.remotes.push_back(Remote(L"upstream", L"u-fetch", L"u-push"));

	FakeGit git;
	git.picks.push_back(std::size_t{ 2 });

	EXPECT_TRUE(RunGitFetch(git.Context(), state, EGitFetchScope::Default).Succeeded());

	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"fetch", L"--all" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitFetchCancelsOnADismissedPick)
{
	auto state = TrackedState();
	state.remotes.push_back(Remote(L"upstream", L"u-fetch", L"u-push"));

	FakeGit git;
	git.picks.push_back(std::nullopt);

	EXPECT_EQ(EGitSyncCommandStatus::Cancelled, RunGitFetch(git.Context(), state, EGitFetchScope::Default).status);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitSyncCommands, RunGitFetchAppliesPruneOnFetchWithoutDoublingAnExplicitPrune)
{
	FakeGit git;
	auto context = git.Context();
	context.configuration.pruneOnFetch = true;

	EXPECT_TRUE(RunGitFetch(context, TrackedState(), EGitFetchScope::Prune).Succeeded());

	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"fetch", L"--prune" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitFetchReportsATypedFailure)
{
	FakeGit git;
	git.responses.push_back(Failure("fatal: Authentication failed for 'https://example.invalid/r.git/'"));

	const auto result = RunGitFetch(git.Context(), TrackedState(), EGitFetchScope::Default);

	EXPECT_EQ(EGitSyncCommandStatus::Failed, result.status);
	EXPECT_EQ(EGitSyncFailureReason::AuthenticationFailed, result.failure.reason);
	EXPECT_EQ(EGitSyncRecovery::Authenticate, result.failure.recovery);
	ASSERT_EQ(1U, git.messages.size());
	EXPECT_EQ(result.failure.message, git.messages[0]);
}

TEST(GitSyncCommands, RunGitPullNamesTheUpstreamAndCarriesPullTags)
{
	FakeGit git;
	const auto result = RunGitPull(git.Context(), TrackedState(), false);

	EXPECT_TRUE(result.Succeeded());
	ASSERT_EQ(2U, git.invocations.size());
	// The rebase probe runs first, and answering "not rebased" asks nothing.
	EXPECT_EQ(BuildMaybeRebasedArguments(L"main"), git.invocations[0]);
	EXPECT_EQ(Arguments({ L"pull", L"--tags", L"origin", L"main" }), git.invocations[1]);
	EXPECT_TRUE(git.prompts.empty());
}

TEST(GitSyncCommands, RunGitPullAsksBeforePullingIntoAPossiblyRebasedBranch)
{
	FakeGit git;
	git.responses.push_back(Ok("= 1234567 a commit\n"));
	git.confirmations.push_back(std::size_t{ 1 }); // `Don't Pull`

	const auto result = RunGitPull(git.Context(), TrackedState(), false);

	EXPECT_EQ(EGitSyncCommandStatus::Cancelled, result.status);
	ASSERT_EQ(1U, git.prompts.size());
	// Only the probe ran; the pull itself was never issued.
	EXPECT_EQ(1U, git.invocations.size());
}

TEST(GitSyncCommands, RunGitPullFetchesEveryRemoteFirstWhenFetchOnPullIsSet)
{
	FakeGit git;
	auto context = git.Context();
	context.configuration.fetchOnPull = true;

	EXPECT_TRUE(RunGitPull(context, TrackedState(), true).Succeeded());

	ASSERT_EQ(3U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"fetch", L"--all" }), git.invocations[0]);
	EXPECT_EQ(Arguments({ L"pull", L"--tags", L"-r", L"origin", L"main" }), git.invocations[2]);
}

TEST(GitSyncCommands, RunGitPushSendsTheExplicitRefspecForATrackedBranch)
{
	FakeGit git;
	EXPECT_TRUE(RunGitPush(git.Context(), TrackedState()).Succeeded());

	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push", L"origin", L"main:main" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitPushOffersToPublishWhenGitReportsNoUpstream)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));

	FakeGit git;
	git.responses.push_back(Failure("fatal: The current branch topic has no upstream branch.\n"));
	git.confirmations.push_back(std::size_t{ 0 }); // `OK`

	const auto result = RunGitPush(git.Context(), state);

	EXPECT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, git.prompts.size());
	EXPECT_EQ(BuildPublishBranchPrompt(L"topic"), git.prompts[0]);
	ASSERT_EQ(2U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push" }), git.invocations[0]);
	EXPECT_EQ(Arguments({ L"push", L"-u", L"origin", L"topic" }), git.invocations[1]);
}

TEST(GitSyncCommands, RunGitPushDoesNotPublishWhenTheConfirmationIsDismissed)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));

	FakeGit git;
	git.responses.push_back(Failure("fatal: The current branch topic has no upstream branch.\n"));
	git.confirmations.push_back(std::nullopt);

	EXPECT_EQ(EGitSyncCommandStatus::Cancelled, RunGitPush(git.Context(), state).status);
	EXPECT_EQ(1U, git.invocations.size());
}

TEST(GitSyncCommands, RunGitPushDeclinesADetachedHead)
{
	GitSyncRepositoryState state;
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));

	FakeGit git;
	const auto result = RunGitPush(git.Context(), state);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	EXPECT_TRUE(git.invocations.empty());
	ASSERT_EQ(1U, git.messages.size());
	EXPECT_EQ(L"Please check out a branch to push to a remote.", git.messages[0]);
}

TEST(GitSyncCommands, RunGitSyncConfirmsThenPullsAndPushes)
{
	FakeGit git;
	const auto result = RunGitSync(git.Context(), TrackedState(), false);

	EXPECT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, git.prompts.size());
	EXPECT_EQ(BuildSyncConfirmationPrompt(L"origin", L"main"), git.prompts[0]);
	ASSERT_EQ(3U, git.invocations.size());
	EXPECT_EQ(BuildMaybeRebasedArguments(L"main"), git.invocations[0]);
	EXPECT_EQ(Arguments({ L"pull", L"--tags", L"origin", L"main" }), git.invocations[1]);
	EXPECT_EQ(Arguments({ L"push", L"origin", L"main:main" }), git.invocations[2]);
}

TEST(GitSyncCommands, RunGitSyncCancelsWithoutTouchingGitWhenTheConfirmationIsDeclined)
{
	FakeGit git;
	git.confirmations.push_back(std::nullopt);

	EXPECT_EQ(EGitSyncCommandStatus::Cancelled, RunGitSync(git.Context(), TrackedState(), false).status);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitSyncCommands, RunGitSyncSkipsThePushHalfForAReadOnlyRemote)
{
	auto state = TrackedState();
	state.remotes[0] = Remote(L"origin", L"o-fetch", L"");

	FakeGit git;
	const auto result = RunGitSync(git.Context(), state, false);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	// A read-only remote also skips the confirmation, because there is nothing
	// to warn about: the push that the warning is for cannot happen.
	EXPECT_TRUE(git.prompts.empty());
	ASSERT_EQ(2U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"pull", L"--tags", L"origin", L"main" }), git.invocations[1]);
}

TEST(GitSyncCommands, RunGitSyncSkipsThePushHalfWhenTheBranchIsNotAhead)
{
	auto state = TrackedState();
	state.ahead = 0;

	FakeGit git;
	const auto result = RunGitSync(git.Context(), state, false);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	ASSERT_EQ(2U, git.invocations.size());
}

TEST(GitSyncCommands, RunGitSyncDegradesToAPushWhenTheBranchHasNoUpstream)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));

	FakeGit git;
	EXPECT_TRUE(RunGitSync(git.Context(), state, false).Succeeded());

	// No sync confirmation: there is no `remote/branch` pair to name yet.
	EXPECT_TRUE(git.prompts.empty());
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitSyncDeclinesADetachedHead)
{
	FakeGit git;
	const auto result = RunGitSync(git.Context(), {}, false);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	EXPECT_TRUE(git.invocations.empty());
}

TEST(GitSyncCommands, RunGitSyncCarriesFollowTagsWhenConfigured)
{
	FakeGit git;
	auto context = git.Context();
	context.configuration.followTagsWhenSync = true;

	EXPECT_TRUE(RunGitSync(context, TrackedState(), true).Succeeded());

	ASSERT_EQ(3U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push", L"--follow-tags", L"origin", L"main:main" }), git.invocations[2]);
}

TEST(GitSyncCommands, RunGitSyncStopsAtAFailedPullWithoutPushing)
{
	FakeGit git;
	git.responses.push_back(Ok());
	git.responses.push_back(Failure("", "CONFLICT (content): Merge conflict in a.txt\n"));

	const auto result = RunGitSync(git.Context(), TrackedState(), false);

	EXPECT_EQ(EGitSyncCommandStatus::Failed, result.status);
	EXPECT_EQ(EGitSyncFailureReason::Conflict, result.failure.reason);
	EXPECT_EQ(EGitSyncRecovery::ResolveConflicts, result.failure.recovery);
	EXPECT_EQ(2U, git.invocations.size());
}

TEST(GitSyncCommands, RunGitPublishPushesWithUpstreamToTheOnlyRemote)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));

	FakeGit git;
	EXPECT_TRUE(RunGitPublish(git.Context(), state).Succeeded());

	EXPECT_TRUE(git.placeholders.empty());
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push", L"-u", L"origin", L"topic" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitPublishAsksWhichRemoteWhenThereIsMoreThanOne)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";
	state.remotes.push_back(Remote(L"origin", L"o-fetch", L"o-push"));
	state.remotes.push_back(Remote(L"fork", L"f-fetch", L"f-push"));

	FakeGit git;
	git.picks.push_back(std::size_t{ 1 });

	EXPECT_TRUE(RunGitPublish(git.Context(), state).Succeeded());

	ASSERT_EQ(1U, git.placeholders.size());
	EXPECT_EQ(L"Pick a remote to publish the branch \"topic\" to:", git.placeholders[0]);
	ASSERT_EQ(1U, git.invocations.size());
	EXPECT_EQ(Arguments({ L"push", L"-u", L"fork", L"topic" }), git.invocations[0]);
}

TEST(GitSyncCommands, RunGitPublishWarnsWhenThereIsNoRemoteAtAll)
{
	GitSyncRepositoryState state;
	state.headName = L"topic";

	FakeGit git;
	const auto result = RunGitPublish(git.Context(), state);

	EXPECT_EQ(EGitSyncCommandStatus::NotApplicable, result.status);
	EXPECT_TRUE(git.invocations.empty());
	ASSERT_EQ(1U, git.messages.size());
	EXPECT_EQ(L"Your repository has no remotes configured to publish to.", git.messages[0]);
}

TEST(GitSyncCommands, EveryCommandFailsClosedWithoutAnInvoker)
{
	GitSyncCommandContext context;

	EXPECT_EQ(EGitSyncCommandStatus::Failed, RunGitFetch(context, TrackedState(), EGitFetchScope::Default).status);
	EXPECT_EQ(EGitSyncCommandStatus::Failed, RunGitPull(context, TrackedState(), false).status);
	EXPECT_EQ(EGitSyncCommandStatus::Failed, RunGitPush(context, TrackedState()).status);
	EXPECT_EQ(EGitSyncCommandStatus::Failed, RunGitSync(context, TrackedState(), false).status);
	EXPECT_EQ(EGitSyncCommandStatus::Failed, RunGitPublish(context, TrackedState()).status);
}
