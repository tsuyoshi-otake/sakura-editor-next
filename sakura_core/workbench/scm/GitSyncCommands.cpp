/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitSyncCommands.h"

#include "workbench/scm/GitFailureText.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace workbench::scm {

namespace {

[[nodiscard]] std::wstring ResolveText(const GitRefTextResolver& text, std::string_view key,
	std::wstring_view fallback, std::wstring_view argument0 = {}, std::wstring_view argument1 = {})
{
	std::wstring result = text ? text(key, argument0) : std::wstring{};
	if (result.empty()) result = fallback;
	const auto replace = [&result](std::wstring_view marker, std::wstring_view value) {
		std::size_t position = 0;
		while (!value.empty() && (position = result.find(marker, position)) != std::wstring::npos) {
			result.replace(position, marker.size(), value);
			position += value.size();
		}
	};
	replace(L"{0}", argument0);
	replace(L"{1}", argument1);
	return result;
}

[[nodiscard]] std::wstring ResolveText(const GitSyncCommandContext& context, std::string_view key,
	std::wstring_view fallback, std::wstring_view argument0 = {}, std::wstring_view argument1 = {})
{
	return ResolveText(context.text, key, fallback, argument0, argument1);
}

//! Upstream's own warnings, from `CommandCenter`. Each command family has its
//! own wording — "fetch from", "pull from", "push to", "publish to" — so they
//! are not collapsed into one shared sentence.
constexpr std::wstring_view kNoFetchRemotes = L"This repository has no remotes configured to fetch from.";
constexpr std::wstring_view kNoPullRemotes = L"Your repository has no remotes configured to pull from.";
constexpr std::wstring_view kNoPushRemotes = L"Your repository has no remotes configured to push to.";
constexpr std::wstring_view kNoPublishRemotes = L"Your repository has no remotes configured to publish to.";
constexpr std::wstring_view kNoBranchToPush = L"Please check out a branch to push to a remote.";
constexpr std::wstring_view kFetchPlaceholder = L"Select a remote to fetch";
//! `Repository.HEAD` is undefined, so upstream's `_sync` returns without a word.
constexpr std::wstring_view kNoHead = L"no branch is checked out";
constexpr std::wstring_view kReadOnlyRemote = L"the remote is read-only, so nothing was pushed";
constexpr std::wstring_view kNothingToPush = L"the branch is not ahead of its upstream";
constexpr std::wstring_view kNoPresenter = L"The sync command has no presenter.";
constexpr std::wstring_view kDismissed = L"the user dismissed the prompt";

//! Upstream's button titles.
constexpr std::wstring_view kOkChoice = L"OK";
constexpr std::wstring_view kPullChoice = L"Pull";
constexpr std::wstring_view kDontPullChoice = L"Don't Pull";

//! Needles a lambda reads. Declared here rather than inside their functions
//! because a function-local `constexpr` referenced from a lambda would have to
//! be captured to be odr-used, and capturing a constant is noise.
constexpr std::wstring_view kConflictPrefix = L"CONFLICT (";
constexpr std::wstring_view kAuthenticationNeedle = L"authentication failed for '";

[[nodiscard]] GitSyncCommandResult Succeeded()
{
	return { EGitSyncCommandStatus::Succeeded, {}, {} };
}

[[nodiscard]] GitSyncCommandResult NotApplicable(std::wstring_view message)
{
	return { EGitSyncCommandStatus::NotApplicable, {}, std::wstring(message) };
}

[[nodiscard]] GitSyncCommandResult Cancelled(std::wstring_view message)
{
	return { EGitSyncCommandStatus::Cancelled, {}, std::wstring(message) };
}

[[nodiscard]] GitSyncCommandResult Failed(GitSyncFailure failure)
{
	auto message = failure.message;
	return { EGitSyncCommandStatus::Failed, std::move(failure), std::move(message) };
}

//! A structural failure that never reached git, so it has no git reason.
[[nodiscard]] GitSyncCommandResult FailedLocally(std::wstring_view message)
{
	return Failed({ EGitSyncFailureReason::Other, EGitSyncRecovery::None, std::wstring(message) });
}

void Notify(const GitSyncCommandContext& context, std::wstring_view message)
{
	if (context.message) {
		context.message(message);
	}
}

[[nodiscard]] std::wstring DecodeStandardOutput(const GitExecutionResult& result)
{
	const std::string_view bytes(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size());
	return DecodeGitOutput(bytes);
}

[[nodiscard]] std::wstring DecodeStandardError(const GitExecutionResult& result)
{
	return DecodeGitOutput(result.standardError);
}

[[nodiscard]] std::wstring ToLower(std::wstring_view value)
{
	std::wstring lowered(value);
	for (auto& character : lowered) {
		character = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(character)));
	}
	return lowered;
}

[[nodiscard]] bool Contains(std::wstring_view text, std::wstring_view needle)
{
	return text.find(needle) != std::wstring_view::npos;
}

//! The `i` flag on a pattern that has no other metacharacters.
[[nodiscard]] bool ContainsInsensitive(std::wstring_view text, std::wstring_view needle)
{
	return Contains(ToLower(text), ToLower(needle));
}

//! Visits each line, `\r` stripped, which is what a `m`-flagged `^`/`$` sees.
template <typename Predicate>
[[nodiscard]] bool AnyLine(std::wstring_view text, Predicate predicate)
{
	std::size_t begin = 0;
	while (true) {
		std::size_t end = text.find(L'\n', begin);
		const bool last = end == std::wstring_view::npos;
		if (last) {
			end = text.size();
		}
		std::wstring_view line = text.substr(begin, end - begin);
		if (!line.empty() && line.back() == L'\r') {
			line.remove_suffix(1);
		}
		if (predicate(line)) {
			return true;
		}
		if (last) {
			return false;
		}
		begin = end + 1;
	}
}

//! The first line, `\r` stripped. A pattern anchored with `^` and **no** `m`
//! flag can only match here, because `.` never crosses a newline.
[[nodiscard]] std::wstring_view FirstLine(std::wstring_view text)
{
	auto line = text.substr(0, std::min(text.find(L'\n'), text.size()));
	if (!line.empty() && line.back() == L'\r') {
		line.remove_suffix(1);
	}
	return line;
}

//! `! \[rejected\].*\(<suffix>\)` on one line, which is how git reports a
//! per-ref refusal inside an otherwise multi-line report.
[[nodiscard]] bool HasRejectedLine(std::wstring_view text, std::wstring_view parenthesized)
{
	return AnyLine(text, [parenthesized](std::wstring_view line) {
		const auto rejected = line.find(L"! [rejected]");
		return rejected != std::wstring_view::npos
			&& line.find(parenthesized, rejected) != std::wstring_view::npos;
	});
}

//! `^CONFLICT \([^)]+\): \b`, which git writes to standard **output**.
[[nodiscard]] bool HasConflictLine(std::wstring_view text)
{
	return AnyLine(text, [](std::wstring_view line) {
		if (!line.starts_with(kConflictPrefix)) {
			return false;
		}
		const auto close = line.find(L')', kConflictPrefix.size());
		// `[^)]+` needs at least one character before the closing parenthesis.
		if (close == std::wstring_view::npos || close == kConflictPrefix.size()) {
			return false;
		}
		if (line.size() <= close + 3 || line[close + 1] != L':' || line[close + 2] != L' ') {
			return false;
		}
		// The trailing `\b` requires a word character after the space.
		const auto next = line[close + 3];
		return std::iswalnum(static_cast<std::wint_t>(next)) != 0 || next == L'_';
	});
}

//!
//! @brief `getGitErrorCode`, restricted to the codes this path can produce.
//!
//! Upstream's ladder also classifies branch, worktree, config-file, and pipe
//! failures. A fetch, pull, or push cannot reach those, and inventing
//! enumerators for them would make the type claim coverage it does not have, so
//! they fall through to `Other` — which is a recognized failure with git's own
//! message, never a silent success.
//!
[[nodiscard]] EGitSyncFailureReason ClassifyGenericGitFailure(std::wstring_view standardError)
{
	if (Contains(standardError, L"Another git process seems to be running in this repository")
		|| Contains(standardError, L"If no other git process is currently running")) {
		return EGitSyncFailureReason::RepositoryIsLocked;
	}
	if (ContainsInsensitive(standardError, L"Authentication failed")) {
		return EGitSyncFailureReason::AuthenticationFailed;
	}
	if (ContainsInsensitive(standardError, L"Not a git repository")) {
		return EGitSyncFailureReason::NotAGitRepository;
	}
	if (Contains(standardError, L"Repository not found")) {
		return EGitSyncFailureReason::RepositoryNotFound;
	}
	if (Contains(standardError, L"unable to access")) {
		return EGitSyncFailureReason::CantAccessRemote;
	}
	if (Contains(standardError, L"Couldn't find remote ref")) {
		return EGitSyncFailureReason::NoRemoteReference;
	}
	// `/Please,? commit your changes or stash them/` — the comma is optional.
	if (Contains(standardError, L"Please commit your changes or stash them")
		|| Contains(standardError, L"Please, commit your changes or stash them")) {
		return EGitSyncFailureReason::DirtyWorkTree;
	}
	if (Contains(standardError, L"detected dubious ownership in repository at")) {
		return EGitSyncFailureReason::NotASafeGitRepository;
	}
	return EGitSyncFailureReason::Other;
}

//! `Authentication failed for '(.*)'`, greedy to the line's last quote.
[[nodiscard]] std::wstring ExtractAuthenticationTarget(std::wstring_view standardError)
{
	std::wstring target;
	(void)AnyLine(standardError, [&target](std::wstring_view line) {
		const auto lowered = ToLower(line);
		const auto found = lowered.find(kAuthenticationNeedle);
		if (found == std::wstring::npos) {
			return false;
		}
		const auto begin = found + kAuthenticationNeedle.size();
		const auto end = line.rfind(L'\'');
		if (end == std::wstring_view::npos || end < begin) {
			return false;
		}
		target.assign(line.substr(begin, end - begin));
		return true;
	});
	return target;
}

//!
//! @brief The `default:` branch of upstream's error switch.
//!
//! Its `err.message` alternative has no counterpart: `RunGit` reports a missing
//! git, a timeout, or a cancellation as its own terminal status, and
//! `DescribeGitFailure` already renders those, so this path is reached only for
//! a process that really did run and really did fail.
//!
[[nodiscard]] std::wstring BuildHintMessage(const GitExecutionResult& result)
{
	const auto standardError = DecodeStandardError(result);
	const auto standardOutput = DecodeStandardOutput(result);
	const auto& source = !standardError.empty() ? standardError : standardOutput;

	std::vector<std::wstring> lines;
	bool droppedErrorPrefix = false;
	bool droppedHuskyLine = false;
	(void)AnyLine(source, [&](std::wstring_view line) {
		// `.replace(/^error: /mi, '')` — the first such line only, prefix removed.
		if (!droppedErrorPrefix && ToLower(line).starts_with(L"error: ")) {
			droppedErrorPrefix = true;
			line.remove_prefix(7);
		}
		// `.replace(/^> husky.*$/mi, '')` — the first such line only, emptied and
		// then dropped by the same filter that drops every other empty line.
		else if (!droppedHuskyLine && ToLower(line).starts_with(L"> husky")) {
			droppedHuskyLine = true;
			line = {};
		}
		if (!line.empty()) {
			lines.emplace_back(line);
		}
		return false;
	});

	if (lines.empty()) {
		return L"Git error";
	}
	// Upstream reads the **last** hint when there was standard output, because a
	// hook's own diagnosis is printed after whatever it echoed first.
	return L"Git: " + (!standardOutput.empty() ? lines.back() : lines.front());
}

//! One git invocation, classified by the operation that issued it.
using GitFailureClassifier = EGitSyncFailureReason (*)(const GitExecutionResult&);

//!
//! @brief `ClassifyGitPushFailure` with no force-push mode.
//!
//! A plain function rather than a lambda because `GitFailureClassifier` is a
//! function pointer, and nothing on this path force-pushes: binding a mode would
//! be state that only ever holds one value.
//!
[[nodiscard]] EGitSyncFailureReason ClassifyPlainPushFailure(const GitExecutionResult& result)
{
	return ClassifyGitPushFailure(result, EGitForcePushMode::None);
}

[[nodiscard]] bool RunStep(
	const GitSyncCommandContext& context,
	const std::vector<std::wstring>& arguments,
	GitFailureClassifier classify,
	GitSyncCommandResult& failure)
{
	const auto result = context.run(arguments);
	if (result.Succeeded() && result.exitCode == 0) {
		return true;
	}
	auto described = DescribeGitSyncFailure(classify(result), result, context.text);
	Notify(context, described.message);
	failure = Failed(std::move(described));
	return false;
}

//!
//! @brief `checkIfMaybeRebased`, including its fail-open behavior.
//!
//! A probe that cannot run is not evidence of a rebase, so it answers "go
//! ahead" — the same choice upstream makes, and the safe one: refusing to pull
//! because a diagnostic command failed would block the recovery it exists to
//! protect.
//!
[[nodiscard]] bool ConfirmNotRebased(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, bool& cancelled)
{
	cancelled = false;
	if (context.configuration.ignoreRebaseWarning) {
		return true;
	}
	const auto result = context.run(BuildMaybeRebasedArguments(state.headName));
	if (!ParseMaybeRebased(result)) {
		return true;
	}
	if (!context.confirm) {
		return true;
	}
	const auto prompt = BuildMaybeRebasedPrompt(state.headName, context.text);
	const auto pick = context.confirm(prompt);
	if (pick.has_value() && *pick == 0) {
		return true;
	}
	cancelled = true;
	return false;
}

//! The push half of a sync, and the whole of `git.push` / `git.publish`.
[[nodiscard]] GitSyncCommandResult PushWith(
	const GitSyncCommandContext& context, const GitPushOptions& options)
{
	GitSyncCommandResult failure;
	if (!RunStep(context, BuildGitPushArguments(options), &ClassifyPlainPushFailure, failure)) {
		return failure;
	}
	return Succeeded();
}

} // namespace

std::vector<std::wstring> BuildGitRemoteArguments()
{
	return { L"remote", L"--verbose" };
}

std::vector<GitRemote> ParseGitRemotes(std::string_view bytes)
{
	std::vector<GitRemote> remotes;
	const auto text = DecodeGitOutput(bytes);
	(void)AnyLine(text, [&remotes](std::wstring_view line) {
		while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) {
			line.remove_prefix(1);
		}
		while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) {
			line.remove_suffix(1);
		}
		if (line.empty()) {
			return false;
		}
		// `line.split(/\s/)` gives [name, url, type]; git separates them with a
		// tab and a space, so any run of whitespace is one separator here.
		std::vector<std::wstring_view> fields;
		std::size_t begin = 0;
		while (begin < line.size() && fields.size() < 3) {
			while (begin < line.size() && (line[begin] == L' ' || line[begin] == L'\t')) {
				++begin;
			}
			if (begin >= line.size()) {
				break;
			}
			auto end = begin;
			while (end < line.size() && line[end] != L' ' && line[end] != L'\t') {
				++end;
			}
			fields.push_back(line.substr(begin, end - begin));
			begin = end;
		}
		if (fields.size() < 2) {
			return false;
		}
		const auto name = fields[0];
		const auto url = fields[1];
		const auto type = fields.size() > 2 ? fields[2] : std::wstring_view{};

		auto found = std::find_if(
			remotes.begin(), remotes.end(), [name](const GitRemote& remote) { return remote.name == name; });
		if (found == remotes.end()) {
			remotes.push_back(GitRemote{ std::wstring(name), {}, {}, false });
			found = std::prev(remotes.end());
		}
		if (ContainsInsensitive(type, L"fetch")) {
			found->fetchUrl.assign(url);
		} else if (ContainsInsensitive(type, L"push")) {
			found->pushUrl.assign(url);
		} else {
			// An unrecognized type is upstream's `else`: the URL serves both.
			found->fetchUrl.assign(url);
			found->pushUrl.assign(url);
		}
		return false;
	});

	// Upstream applies this after parsing, to every remote, regardless of which
	// of its two parsers produced them.
	for (auto& remote : remotes) {
		remote.isReadOnly = remote.pushUrl.empty() || remote.pushUrl == L"no_push";
	}
	return remotes;
}

const GitRemote* FindGitRemote(const std::vector<GitRemote>& remotes, std::wstring_view name)
{
	if (name.empty()) {
		return nullptr;
	}
	const auto found = std::find_if(
		remotes.begin(), remotes.end(), [name](const GitRemote& remote) { return remote.name == name; });
	return found == remotes.end() ? nullptr : &*found;
}

std::vector<std::wstring> BuildGitFetchArguments(const GitFetchOptions& options)
{
	std::vector<std::wstring> arguments{ L"fetch" };
	if (!options.remote.empty()) {
		arguments.push_back(options.remote);
		if (!options.ref.empty()) {
			arguments.push_back(options.ref);
		}
	} else if (options.all) {
		arguments.emplace_back(L"--all");
	}
	if (options.prune) {
		arguments.emplace_back(L"--prune");
	}
	return arguments;
}

std::vector<std::wstring> BuildGitPullArguments(
	bool rebase, std::wstring_view remote, std::wstring_view branch, const GitPullOptions& options)
{
	std::vector<std::wstring> arguments{ L"pull" };
	if (options.tags) {
		arguments.emplace_back(L"--tags");
	}
	if (options.unshallow) {
		arguments.emplace_back(L"--unshallow");
	}
	if (options.autoStash) {
		arguments.emplace_back(L"--autostash");
	}
	if (rebase) {
		arguments.emplace_back(L"-r");
	}
	// Upstream requires **both**; a remote with no branch would make git guess a
	// refspec, which is exactly the ambiguity the pair exists to remove.
	if (!remote.empty() && !branch.empty()) {
		arguments.emplace_back(remote);
		arguments.emplace_back(branch);
	}
	return arguments;
}

std::vector<std::wstring> BuildGitPushArguments(const GitPushOptions& options)
{
	std::vector<std::wstring> arguments{ L"push" };
	if (options.forcePushMode == EGitForcePushMode::ForceWithLease
		|| options.forcePushMode == EGitForcePushMode::ForceWithLeaseIfIncludes) {
		arguments.emplace_back(L"--force-with-lease");
		if (options.forcePushMode == EGitForcePushMode::ForceWithLeaseIfIncludes) {
			arguments.emplace_back(L"--force-if-includes");
		}
	} else if (options.forcePushMode == EGitForcePushMode::Force) {
		arguments.emplace_back(L"--force");
	}
	if (options.setUpstream) {
		arguments.emplace_back(L"-u");
	}
	if (options.followTags) {
		arguments.emplace_back(L"--follow-tags");
	}
	if (options.tags) {
		arguments.emplace_back(L"--tags");
	}
	if (!options.remote.empty()) {
		arguments.push_back(options.remote);
	}
	if (!options.name.empty()) {
		arguments.push_back(options.name);
	}
	return arguments;
}

std::vector<std::wstring> BuildMaybeRebasedArguments(std::wstring_view branch)
{
	std::wstring range(branch);
	range += L"...";
	range += branch;
	range += L"@{upstream}";
	return { L"log", L"--oneline", L"--cherry", std::move(range), L"--" };
}

bool ParseMaybeRebased(const GitExecutionResult& result)
{
	if (!result.Succeeded() || result.exitCode != 0) {
		return false;
	}
	const auto text = DecodeStandardOutput(result);
	return !text.empty() && text.front() == L'=';
}

EGitSyncFailureReason ClassifyGitFetchFailure(const GitExecutionResult& result)
{
	const auto standardError = DecodeStandardError(result);
	if (Contains(standardError, L"No remote repository specified.")) {
		return EGitSyncFailureReason::NoRemoteRepositorySpecified;
	}
	if (Contains(standardError, L"Could not read from remote repository")) {
		return EGitSyncFailureReason::RemoteConnectionError;
	}
	if (HasRejectedLine(standardError, L"(non-fast-forward)")) {
		return EGitSyncFailureReason::BranchFastForwardRejected;
	}
	return ClassifyGenericGitFailure(standardError);
}

EGitSyncFailureReason ClassifyGitPullFailure(const GitExecutionResult& result)
{
	// The conflict report goes to standard output; every other test reads stderr.
	if (HasConflictLine(DecodeStandardOutput(result))) {
		return EGitSyncFailureReason::Conflict;
	}
	const auto standardError = DecodeStandardError(result);
	if (Contains(standardError, L"Please tell me who you are.")) {
		return EGitSyncFailureReason::NoUserNameConfigured;
	}
	if (Contains(standardError, L"Could not read from remote repository")) {
		return EGitSyncFailureReason::RemoteConnectionError;
	}
	if (ContainsInsensitive(standardError, L"Pulling is not possible because you have unmerged files")
		|| ContainsInsensitive(standardError, L"Pull is not possible because you have unmerged files")
		|| ContainsInsensitive(standardError, L"Cannot pull with rebase: You have unstaged changes")
		|| ContainsInsensitive(standardError, L"Your local changes to the following files would be overwritten")
		|| ContainsInsensitive(standardError, L"Please, commit your changes before you can merge")) {
		return EGitSyncFailureReason::DirtyWorkTree;
	}
	if (ContainsInsensitive(standardError, L"cannot lock ref")
		|| ContainsInsensitive(standardError, L"unable to update local ref")) {
		return EGitSyncFailureReason::CantLockRef;
	}
	if (ContainsInsensitive(standardError, L"cannot rebase onto multiple branches")) {
		return EGitSyncFailureReason::CantRebaseMultipleBranches;
	}
	if (HasRejectedLine(standardError, L"(would clobber existing tag)")) {
		return EGitSyncFailureReason::TagConflict;
	}
	return ClassifyGenericGitFailure(standardError);
}

EGitSyncFailureReason ClassifyGitPushFailure(
	const GitExecutionResult& result, EGitForcePushMode forcePushMode)
{
	const auto standardError = DecodeStandardError(result);
	const bool rejected = AnyLine(standardError, [](std::wstring_view line) {
		return line.starts_with(L"error: failed to push some refs to");
	});
	if (rejected) {
		if (forcePushMode == EGitForcePushMode::ForceWithLease
			&& HasRejectedLine(standardError, L"(stale info)")) {
			return EGitSyncFailureReason::ForcePushWithLeaseRejected;
		}
		if (forcePushMode == EGitForcePushMode::ForceWithLeaseIfIncludes
			&& HasRejectedLine(standardError, L"(remote ref updated since checkout)")) {
			return EGitSyncFailureReason::ForcePushWithLeaseIfIncludesRejected;
		}
		return EGitSyncFailureReason::PushRejected;
	}
	if (AnyLine(standardError, [](std::wstring_view line) {
			const auto permission = line.find(L"Permission");
			return permission != std::wstring_view::npos
				&& line.find(L"denied", permission) != std::wstring_view::npos;
		})) {
		return EGitSyncFailureReason::PermissionDenied;
	}
	if (Contains(standardError, L"Could not read from remote repository")) {
		return EGitSyncFailureReason::RemoteConnectionError;
	}
	// Upstream's pattern has no `m` flag, so it can only match the first line.
	const auto first = FirstLine(standardError);
	if (first.starts_with(L"fatal: The current branch ")
		&& Contains(first, L" has no upstream branch")) {
		return EGitSyncFailureReason::NoUpstreamBranch;
	}
	return ClassifyGenericGitFailure(standardError);
}

EGitSyncRecovery RecoveryForGitSyncFailure(EGitSyncFailureReason reason) noexcept
{
	switch (reason) {
	case EGitSyncFailureReason::AuthenticationFailed:
	case EGitSyncFailureReason::PermissionDenied:
		return EGitSyncRecovery::Authenticate;
	case EGitSyncFailureReason::PushRejected:
	case EGitSyncFailureReason::ForcePushWithLeaseRejected:
	case EGitSyncFailureReason::ForcePushWithLeaseIfIncludesRejected:
	case EGitSyncFailureReason::BranchFastForwardRejected:
		return EGitSyncRecovery::PullThenRetry;
	case EGitSyncFailureReason::Conflict:
		return EGitSyncRecovery::ResolveConflicts;
	case EGitSyncFailureReason::DirtyWorkTree:
		return EGitSyncRecovery::CleanWorkingTree;
	case EGitSyncFailureReason::NoUserNameConfigured:
		return EGitSyncRecovery::ConfigureIdentity;
	case EGitSyncFailureReason::NoUpstreamBranch:
		return EGitSyncRecovery::PublishBranch;
	case EGitSyncFailureReason::RemoteConnectionError:
	case EGitSyncFailureReason::CantAccessRemote:
	case EGitSyncFailureReason::RepositoryIsLocked:
	case EGitSyncFailureReason::CantLockRef:
		// The identical command can succeed once the network path or the other
		// git process clears, so these are the retryable ones.
		return EGitSyncRecovery::RetryLater;
	case EGitSyncFailureReason::None:
	case EGitSyncFailureReason::RepositoryNotFound:
	case EGitSyncFailureReason::NoRemoteReference:
	case EGitSyncFailureReason::NoRemoteRepositorySpecified:
	case EGitSyncFailureReason::TagConflict:
	case EGitSyncFailureReason::CantRebaseMultipleBranches:
	case EGitSyncFailureReason::NotAGitRepository:
	case EGitSyncFailureReason::NotASafeGitRepository:
	case EGitSyncFailureReason::Other:
	default:
		return EGitSyncRecovery::None;
	}
}

GitSyncFailure DescribeGitSyncFailure(EGitSyncFailureReason reason, const GitExecutionResult& result)
{
	return DescribeGitSyncFailure(reason, result, {});
}

GitSyncFailure DescribeGitSyncFailure(
	EGitSyncFailureReason reason, const GitExecutionResult& result, const GitRefTextResolver& text)
{
	GitSyncFailure failure{ reason, RecoveryForGitSyncFailure(reason), {} };
	if (reason == EGitSyncFailureReason::None) {
		return failure;
	}

	// A command that never produced a process has no git sentence to quote, so
	// the shared renderer answers before any of upstream's texts apply.
	switch (result.status) {
	case EGitExecutionStatus::Succeeded:
	case EGitExecutionStatus::Failed:
		break;
	default:
		failure.message = DescribeGitFailure(result);
		return failure;
	}

	switch (reason) {
	case EGitSyncFailureReason::DirtyWorkTree:
		failure.message = ResolveText(text, "GitDirtyWorkTree",
			L"Please clean your repository working tree before checkout.");
		break;
	case EGitSyncFailureReason::PushRejected:
		failure.message = ResolveText(text, "GitPushRejected",
			L"Can't push refs to remote. Try running \"Pull\" first to integrate your changes.");
		break;
	case EGitSyncFailureReason::ForcePushWithLeaseRejected:
	case EGitSyncFailureReason::ForcePushWithLeaseIfIncludesRejected:
		failure.message = ResolveText(text, "GitForcePushRejected",
			L"Can't force push refs to remote. The tip of the remote-tracking branch has been updated "
			L"since the last checkout. Try running \"Pull\" first to pull the latest changes from the "
			L"remote branch first.");
		break;
	case EGitSyncFailureReason::Conflict:
		failure.message = ResolveText(text, "GitConflict",
			L"There are merge conflicts. Please resolve them before committing your changes.");
		break;
	case EGitSyncFailureReason::AuthenticationFailed: {
		const auto target = ExtractAuthenticationTarget(DecodeStandardError(result));
		failure.message = target.empty()
			? ResolveText(text, "GitAuthenticationFailed", L"Failed to authenticate to git remote.")
			: ResolveText(text, "GitAuthenticationFailedTarget",
				L"Failed to authenticate to git remote:\n\n{0}", target);
		break;
	}
	case EGitSyncFailureReason::NoUserNameConfigured:
		// Upstream also offers a `Learn More` action opening
		// https://aka.ms/vscode-setup-git. There is no notification-action
		// surface on this path, so the sentence stands alone.
		failure.message = ResolveText(text, "GitNoUserNameConfigured",
			L"Make sure you configure your \"user.name\" and \"user.email\" in git.");
		break;
	default:
		failure.message = BuildHintMessage(result);
		break;
	}
	return failure;
}

bool SplitGitUpstream(std::wstring_view upstream, std::wstring& remote, std::wstring& name)
{
	const auto slash = upstream.find(L'/');
	if (slash == std::wstring_view::npos) {
		return false;
	}
	remote.assign(upstream.substr(0, slash));
	name.assign(upstream.substr(slash + 1));
	return true;
}

GitSyncRepositoryState BuildGitSyncRepositoryState(const GitScmState& state, std::vector<GitRemote> remotes)
{
	GitSyncRepositoryState result;
	result.remotes = std::move(remotes);
	if (!state.repository) {
		return result;
	}
	result.headName = state.branch;
	result.ahead = state.ahead;
	result.behind = state.behind;
	// An upstream with no slash is not a tracking ref, so neither half is
	// guessed; the branch is then treated as untracked, which is the state
	// `git.publish` exists for.
	(void)SplitGitUpstream(state.upstream, result.upstreamRemote, result.upstreamName);
	return result;
}

std::vector<GitRemotePickItem> BuildFetchRemotePickItems(const GitSyncRepositoryState& state)
{
	return BuildFetchRemotePickItems(state, {});
}

std::vector<GitRemotePickItem> BuildFetchRemotePickItems(
	const GitSyncRepositoryState& state, const GitRefTextResolver& text)
{
	std::vector<GitRemotePickItem> items;
	items.reserve(state.remotes.size() + 1);
	for (std::size_t index = 0; index < state.remotes.size(); ++index) {
		const auto& remote = state.remotes[index];
		items.push_back({ EGitRemotePickKind::Remote, L"$(cloud) " + remote.name, remote.fetchUrl, index });
	}
	// Upstream moves HEAD's own remote to the top so the default is first.
	if (!state.upstreamRemote.empty()) {
		const auto found = std::find_if(items.begin(), items.end(), [&state](const GitRemotePickItem& item) {
			return state.remotes[item.remoteIndex].name == state.upstreamRemote;
		});
		if (found != items.end() && found != items.begin()) {
			std::rotate(items.begin(), found, std::next(found));
		}
	}
	items.push_back({ EGitRemotePickKind::AllRemotes,
		L"$(cloud-download) " + ResolveText(text, "GitFetchAllRemotes", L"Fetch all remotes"), {}, 0 });
	return items;
}

std::vector<GitRemotePickItem> BuildPublishRemotePickItems(const GitSyncRepositoryState& state)
{
	std::vector<GitRemotePickItem> items;
	items.reserve(state.remotes.size());
	for (std::size_t index = 0; index < state.remotes.size(); ++index) {
		const auto& remote = state.remotes[index];
		items.push_back({ EGitRemotePickKind::Remote, remote.name, remote.pushUrl, index });
	}
	return items;
}

GitPrompt BuildSyncConfirmationPrompt(std::wstring_view remote, std::wstring_view branch,
	const GitRefTextResolver& text)
{
	GitPrompt prompt;
	prompt.message = ResolveText(text, "GitSyncConfirmation",
		L"This action will pull and push commits from and to \"{0}/{1}\".", remote, branch);
	prompt.choices.emplace_back(ResolveText(text, "GitOk", kOkChoice));
	prompt.warning = true;
	prompt.modal = true;
	return prompt;
}

GitPrompt BuildPublishBranchPrompt(std::wstring_view branch, const GitRefTextResolver& text)
{
	GitPrompt prompt;
	prompt.message = ResolveText(text, "GitPublishBranchPrompt",
		L"The branch \"{0}\" has no remote branch. Would you like to publish this branch?", branch);
	prompt.choices.emplace_back(ResolveText(text, "GitOk", kOkChoice));
	prompt.warning = true;
	prompt.modal = true;
	return prompt;
}

GitPrompt BuildMaybeRebasedPrompt(std::wstring_view branch, const GitRefTextResolver& text)
{
	GitPrompt prompt;
	prompt.message = branch.empty()
		? ResolveText(text, "GitMaybeRebasedNoName", L"It looks like the current branch might have been rebased. Are you sure you still want to pull into it?")
		: ResolveText(text, "GitMaybeRebased", L"It looks like the current branch \"{0}\" might have been rebased. Are you sure you still want to pull into it?", branch);
	prompt.choices.emplace_back(ResolveText(text, "GitPull", kPullChoice));
	prompt.choices.emplace_back(ResolveText(text, "GitDontPull", kDontPullChoice));
	prompt.warning = true;
	prompt.modal = false;
	return prompt;
}

GitSyncCommandResult RunGitFetch(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, EGitFetchScope scope)
{
	if (!context.run) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	if (state.remotes.empty()) {
		const auto message = ResolveText(context, "GitNoFetchRemotes", kNoFetchRemotes);
		Notify(context, message);
		return NotApplicable(message);
	}

	GitFetchOptions options;
	options.all = scope == EGitFetchScope::All;
	options.prune = scope == EGitFetchScope::Prune;

	// `git.fetch` with more than one remote asks which; `git.fetchPrune` and
	// `git.fetchAll` never do, because their scope is already unambiguous.
	if (scope == EGitFetchScope::Default && state.remotes.size() > 1) {
		if (!context.pickRemote) {
			return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
		}
		const auto items = BuildFetchRemotePickItems(state, context.text);
		const auto chosen = context.pickRemote(items, ResolveText(context, "GitFetchPlaceholder", kFetchPlaceholder));
		if (!chosen.has_value() || *chosen >= items.size()) {
			return Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed));
		}
		const auto& item = items[*chosen];
		if (item.kind == EGitRemotePickKind::AllRemotes) {
			options.all = true;
		} else {
			options.remote = state.remotes[item.remoteIndex].name;
		}
	}

	// `_fetch` applies the setting only when the caller did not already ask.
	if (!options.prune && context.configuration.pruneOnFetch) {
		options.prune = true;
	}

	GitSyncCommandResult failure;
	if (!RunStep(context, BuildGitFetchArguments(options), &ClassifyGitFetchFailure, failure)) {
		return failure;
	}
	return Succeeded();
}

GitSyncCommandResult RunGitPull(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, bool rebase)
{
	if (!context.run) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	if (state.remotes.empty()) {
		const auto message = ResolveText(context, "GitNoPullRemotes", kNoPullRemotes);
		Notify(context, message);
		return NotApplicable(message);
	}

	if (context.configuration.fetchOnPull) {
		// Upstream awaits this fetch, so a failure aborts the pull rather than
		// silently pulling against stale remote-tracking refs.
		const auto fetched = RunGitFetch(context, state, EGitFetchScope::All);
		if (fetched.status == EGitSyncCommandStatus::Failed) {
			return fetched;
		}
	}

	bool cancelled = false;
	if (!ConfirmNotRebased(context, state, cancelled)) {
		return cancelled ? Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed)) : Succeeded();
	}

	GitPullOptions options;
	options.tags = context.configuration.pullTags;
	options.autoStash = context.configuration.autoStash;

	std::wstring remote;
	std::wstring branch;
	if (state.HasUpstream()) {
		remote = state.upstreamRemote;
		branch = state.upstreamName;
	}

	GitSyncCommandResult failure;
	if (!RunStep(
			context, BuildGitPullArguments(rebase, remote, branch, options), &ClassifyGitPullFailure, failure)) {
		return failure;
	}
	return Succeeded();
}

GitSyncCommandResult RunGitPush(const GitSyncCommandContext& context, const GitSyncRepositoryState& state)
{
	if (!context.run) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	if (state.remotes.empty()) {
		// Upstream offers `Add Remote` here; `git.addRemote` is not registered,
		// so the warning stands alone rather than offering a route that is not
		// implemented.
		const auto message = ResolveText(context, "GitNoPushRemotes", kNoPushRemotes);
		Notify(context, message);
		return NotApplicable(message);
	}
	if (state.headName.empty()) {
		const auto message = ResolveText(context, "GitNoBranchToPush", kNoBranchToPush);
		Notify(context, message);
		return NotApplicable(message);
	}

	GitPushOptions options;
	if (state.HasUpstream()) {
		options.remote = state.upstreamRemote;
		options.name = state.headName + L":" + state.upstreamName;
	}

	const auto result = context.run(BuildGitPushArguments(options));
	if (result.Succeeded() && result.exitCode == 0) {
		return Succeeded();
	}

	auto failure = DescribeGitSyncFailure(
		ClassifyGitPushFailure(result, options.forcePushMode), result, context.text);
	if (failure.reason != EGitSyncFailureReason::NoUpstreamBranch) {
		Notify(context, failure.message);
		return Failed(std::move(failure));
	}

	// The one place a failed command legitimately becomes a different one: git
	// refused because the branch is not tracked, and publishing is the operation
	// the user actually wanted.
	if (!context.confirm) {
		Notify(context, failure.message);
		return Failed(std::move(failure));
	}
	const auto pick = context.confirm(BuildPublishBranchPrompt(state.headName, context.text));
	if (!pick.has_value() || *pick != 0) {
		return Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed));
	}
	return RunGitPublish(context, state);
}

GitSyncCommandResult RunGitSync(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, bool rebase)
{
	if (!context.run) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	if (state.headName.empty()) {
		return NotApplicable(ResolveText(context, "GitSyncNoHead", kNoHead));
	}
	if (!state.HasUpstream()) {
		// Upstream's `_sync` degrades to a push, which then offers to publish.
		return RunGitPush(context, state);
	}

	const auto* remote = FindGitRemote(state.remotes, state.upstreamRemote);
	const bool readOnly = remote != nullptr && remote->isReadOnly;

	if (!readOnly && context.configuration.confirmSync) {
		if (!context.confirm) {
			return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
		}
		const auto prompt = BuildSyncConfirmationPrompt(state.upstreamRemote, state.upstreamName, context.text);
		const auto pick = context.confirm(prompt);
		if (!pick.has_value() || *pick != 0) {
			return Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed));
		}
	}

	if (context.configuration.fetchOnPull) {
		const auto fetched = RunGitFetch(context, state, EGitFetchScope::All);
		if (fetched.status == EGitSyncCommandStatus::Failed) {
			return fetched;
		}
	}

	bool cancelled = false;
	if (!ConfirmNotRebased(context, state, cancelled)) {
		// Declining the rebase warning skips the pull, and upstream then falls
		// straight through to the push half rather than abandoning the sync.
		if (cancelled) {
			return Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed));
		}
	} else {
		GitPullOptions pullOptions;
		pullOptions.tags = context.configuration.pullTags;
		pullOptions.autoStash = context.configuration.autoStash;
		GitSyncCommandResult failure;
		if (!RunStep(
				context,
				BuildGitPullArguments(rebase, state.upstreamRemote, state.upstreamName, pullOptions),
				&ClassifyGitPullFailure,
				failure)) {
			return failure;
		}
	}

	if (readOnly) {
		return NotApplicable(ResolveText(context, "GitSyncReadOnlyRemote", kReadOnlyRemote));
	}
	if (state.ahead <= 0) {
		return NotApplicable(ResolveText(context, "GitSyncNothingToPush", kNothingToPush));
	}

	GitPushOptions pushOptions;
	pushOptions.remote = state.upstreamRemote;
	pushOptions.name = state.headName + L":" + state.upstreamName;
	pushOptions.setUpstream = false;
	pushOptions.followTags = context.configuration.followTagsWhenSync;
	return PushWith(context, pushOptions);
}

GitSyncCommandResult RunGitPublish(const GitSyncCommandContext& context, const GitSyncRepositoryState& state)
{
	if (!context.run) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	if (state.remotes.empty()) {
		// With no remote and no `RemoteSourcePublisher`, upstream shows exactly
		// this warning and stops. There is no publisher registry here, so this
		// is the only branch that can be taken — not an approximation of one.
		const auto message = ResolveText(context, "GitNoPublishRemotes", kNoPublishRemotes);
		Notify(context, message);
		return NotApplicable(message);
	}

	GitPushOptions options;
	options.name = state.headName;
	options.setUpstream = true;

	if (state.remotes.size() == 1) {
		options.remote = state.remotes.front().name;
		return PushWith(context, options);
	}

	if (!context.pickRemote) {
		return FailedLocally(ResolveText(context, "GitSyncNoPresenter", kNoPresenter));
	}
	const auto items = BuildPublishRemotePickItems(state);
	const std::wstring placeholder = ResolveText(context, "GitPublishRemotePicker",
		L"Pick a remote to publish the branch \"{0}\" to:", state.headName);
	const auto chosen = context.pickRemote(items, placeholder);
	if (!chosen.has_value() || *chosen >= items.size()) {
		return Cancelled(ResolveText(context, "GitSyncDismissed", kDismissed));
	}
	options.remote = state.remotes[items[*chosen].remoteIndex].name;
	return PushWith(context, options);
}

} // namespace workbench::scm
