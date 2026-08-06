/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitInitCloneCommands.h"

#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/scm/GitFailureText.h"

#include <utility>

namespace workbench::scm {

using commands::json::AtEnd;
using commands::json::Expect;
using commands::json::ReadBoolean;
using commands::json::SkipWhitespace;

namespace {

//! Upstream's own strings, read verbatim from `extensions/git/package.nls.json`
//! at tag 1.95.3 (`view.workbench.scm.folder`, `view.workbench.scm.empty`).
//! The markdown link syntax and the "read our docs" trailer are not carried
//! here: this model exposes the same command each link names as a distinct,
//! typed `GitScmWelcomeAction` instead of embedded markdown, matching how the
//! repository band already separates its own commands from its label text.
constexpr std::wstring_view kFolderNoRepositoryMessage =
	L"The folder currently open doesn't have a Git repository. You can "
	L"initialize a repository which will enable source control features "
	L"powered by Git.";
constexpr std::wstring_view kEmptyWorkbenchMessage =
	L"In order to use Git features, you can open a folder containing a Git "
	L"repository or clone from a URL.";

constexpr std::wstring_view kInitializeRepositoryLabel = L"Initialize Repository";
constexpr std::wstring_view kCloneRepositoryLabel = L"Clone Repository";
constexpr std::wstring_view kChooseFolderLabel = L"Choose Folder...";
constexpr std::wstring_view kOpenLabel = L"Open";

//! `!result.Succeeded() || result.exitCode != 0`: the compound condition every
//! call site in this directory uses, because a process that ran and refused
//! reports `status == Succeeded` with a non-zero exit code — see
//! `GitCommandRunner.h`'s own doc comment and `GitSyncCommandsTest.cpp`'s
//! `Failure()` builder.
[[nodiscard]] bool GitCommandFailed(const GitExecutionResult& result) noexcept
{
	return !result.Succeeded() || result.exitCode != 0;
}

[[nodiscard]] GitInitCommandResult MakeInitFailed(std::wstring message)
{
	GitInitCommandResult result;
	result.status = EGitInitCommandStatus::Failed;
	result.message = std::move(message);
	return result;
}

[[nodiscard]] GitInitCommandResult MakeInitCancelled(std::wstring message = {})
{
	GitInitCommandResult result;
	result.status = EGitInitCommandStatus::Cancelled;
	result.message = std::move(message);
	return result;
}

[[nodiscard]] GitInitCommandResult MakeInitSucceeded(std::wstring repositoryPath, EGitInitPostAction postAction)
{
	GitInitCommandResult result;
	result.status = EGitInitCommandStatus::Succeeded;
	result.repositoryPath = std::move(repositoryPath);
	result.postAction = postAction;
	return result;
}

[[nodiscard]] GitCloneCommandResult MakeCloneFailed(std::wstring message)
{
	GitCloneCommandResult result;
	result.status = EGitCloneCommandStatus::Failed;
	result.message = std::move(message);
	return result;
}

[[nodiscard]] GitCloneCommandResult MakeCloneCancelled(std::wstring message = {})
{
	GitCloneCommandResult result;
	result.status = EGitCloneCommandStatus::Cancelled;
	result.message = std::move(message);
	return result;
}

[[nodiscard]] GitCloneCommandResult MakeCloneSucceeded(std::wstring repositoryPath)
{
	GitCloneCommandResult result;
	result.status = EGitCloneCommandStatus::Succeeded;
	result.repositoryPath = std::move(repositoryPath);
	return result;
}

} // namespace

// ---------------------------------------------------------------------------
// git.init
// ---------------------------------------------------------------------------

std::vector<GitInitFolderPickItem> BuildGitInitFolderPickItems(const std::vector<GitInitWorkspaceFolder>& openFolders)
{
	std::vector<GitInitFolderPickItem> items;
	items.reserve(openFolders.size() + 1);
	for (const GitInitWorkspaceFolder& folder : openFolders) {
		items.push_back(GitInitFolderPickItem{ folder.name, folder.path, folder.path });
	}
	// Upstream always appends this row after the real folders; an empty
	// `path` is what the caller uses to recognize it and fall through to the
	// folder browser.
	items.push_back(GitInitFolderPickItem{ std::wstring(kChooseFolderLabel), std::wstring{}, std::wstring{} });
	return items;
}

bool IsGitInitHomeDirectoryGuardTriggered(std::wstring_view homeDirectory, std::wstring_view chosenPath)
{
	if (homeDirectory.empty() || chosenPath.size() < homeDirectory.size()) {
		return false;
	}
	return chosenPath.substr(0, homeDirectory.size()) == homeDirectory;
}

GitPrompt BuildGitInitHomeDirectoryPrompt(std::wstring_view chosenPath)
{
	GitPrompt prompt;
	prompt.message = L"This will create a Git repository in \"" + std::wstring(chosenPath) +
		L"\". Are you sure you want to initialize a Git repository in your home directory?";
	prompt.choices = { std::wstring(kInitializeRepositoryLabel) };
	prompt.warning = true;
	prompt.modal = true;
	return prompt;
}

GitPrompt BuildGitInitOpenPrompt()
{
	GitPrompt prompt;
	prompt.message = L"Would you like to open the initialized repository?";
	prompt.choices = { std::wstring(kOpenLabel) };
	prompt.warning = false;
	prompt.modal = false;
	return prompt;
}

std::vector<std::wstring> BuildGitInitArguments()
{
	return { std::wstring(L"init") };
}

bool ParseGitInitSkipFolderPromptArgument(std::string_view argumentsJson)
{
	std::size_t index = 0;
	SkipWhitespace(argumentsJson, index);
	if (index >= argumentsJson.size()) {
		// The argument-less invocation (a Command Palette run), not a malformed
		// payload: upstream's own default is to ask.
		return false;
	}
	if (!Expect(argumentsJson, index, '[')) {
		return false;
	}
	bool skipFolderPrompt = false;
	if (!ReadBoolean(argumentsJson, index, skipFolderPrompt)) {
		return false;
	}
	if (!Expect(argumentsJson, index, ']')) {
		return false;
	}
	if (!AtEnd(argumentsJson, index)) {
		return false;
	}
	return skipFolderPrompt;
}

GitInitCommandResult RunGitInit(const GitInitCommandContext& context, bool skipFolderPrompt)
{
	std::wstring chosenPath;
	EGitInitPostAction postAction = EGitInitPostAction::OfferToOpen;
	// The home-directory guard only applies to a path the user picked through
	// the folder browser. A folder that is already open was necessarily
	// created/opened some other way already, so re-guarding it here would ask
	// the user to bless a state that already exists.
	bool applyHomeGuard = false;

	if (skipFolderPrompt && context.openFolders.size() == 1) {
		// Upstream's fast path for `git.init?[true]`: a single open folder
		// resolves with no prompt at all.
		chosenPath = context.openFolders.front().path;
		postAction = EGitInitPostAction::AlreadyOpen;
	} else {
		if (!context.folderPick) {
			return MakeInitFailed(L"No folder picker is available to choose where to initialize the repository.");
		}

		const std::vector<GitInitFolderPickItem> items = BuildGitInitFolderPickItems(context.openFolders);
		const std::optional<std::size_t> picked =
			context.folderPick(items, L"Pick workspace folder to initialize git repo on");
		if (!picked.has_value() || *picked >= items.size()) {
			return MakeInitCancelled();
		}

		const GitInitFolderPickItem& item = items[*picked];
		if (!item.path.empty()) {
			chosenPath = item.path;
			postAction = EGitInitPostAction::AlreadyOpen;
		} else {
			if (!context.browseForFolder) {
				return MakeInitFailed(L"No folder browser is available to choose where to initialize the repository.");
			}
			const std::optional<std::wstring> browsed =
				context.browseForFolder(kInitializeRepositoryLabel, context.homeDirectory);
			if (!browsed.has_value() || browsed->empty()) {
				return MakeInitCancelled();
			}
			chosenPath = *browsed;
			postAction = EGitInitPostAction::OfferToOpen;
			applyHomeGuard = true;
		}
	}

	if (applyHomeGuard && IsGitInitHomeDirectoryGuardTriggered(context.homeDirectory, chosenPath)) {
		if (!context.confirm) {
			return MakeInitFailed(
				L"No confirmation presenter is available to guard initializing a repository in the home directory.");
		}
		const std::optional<std::size_t> choice = context.confirm(BuildGitInitHomeDirectoryPrompt(chosenPath));
		if (!choice.has_value() || *choice != 0) {
			return MakeInitCancelled(L"Initializing a Git repository in the home directory was declined.");
		}
	}

	if (!context.run) {
		return MakeInitFailed(L"No git invoker is available to run git init.");
	}

	const GitExecutionResult result = context.run(chosenPath, BuildGitInitArguments());
	if (GitCommandFailed(result)) {
		std::wstring failure = DescribeGitFailure(result);
		if (context.message) {
			context.message(failure);
		}
		return MakeInitFailed(std::move(failure));
	}

	if (context.message) {
		context.message(L"Initialized repository in \"" + chosenPath + L"\".");
	}

	return MakeInitSucceeded(chosenPath, postAction);
}

// ---------------------------------------------------------------------------
// git.clone
// ---------------------------------------------------------------------------

std::wstring DeriveGitCloneFolderName(std::wstring_view url)
{
	std::wstring trimmed(url);
	while (!trimmed.empty() && (trimmed.back() == L'/' || trimmed.back() == L'\\')) {
		trimmed.pop_back();
	}

	const std::size_t separator = trimmed.find_last_of(L"/\\");
	std::wstring name = (separator == std::wstring::npos) ? trimmed : trimmed.substr(separator + 1);

	// `>=` rather than `>`: a last segment that is exactly ".git" (no repository
	// name at all) must strip down to empty, not survive as a literal ".git"
	// folder name, so `RunGitClonePrepare` can actually detect and refuse it.
	constexpr std::wstring_view kGitSuffix = L".git";
	if (name.size() >= kGitSuffix.size() &&
		name.compare(name.size() - kGitSuffix.size(), kGitSuffix.size(), kGitSuffix) == 0) {
		name.resize(name.size() - kGitSuffix.size());
	}

	return name;
}

GitPrompt BuildGitCloneOverwritePrompt(std::wstring_view folderName)
{
	GitPrompt prompt;
	prompt.message = L"A folder for the repository \"" + std::wstring(folderName) +
		L"\" already exists and is not empty. Do you want to overwrite it?";
	prompt.choices = { std::wstring(L"Overwrite") };
	prompt.warning = true;
	prompt.modal = true;
	return prompt;
}

std::vector<std::wstring> BuildGitCloneArguments(
	std::wstring_view url, std::wstring_view destinationPath, const GitCloneOptions& options)
{
	std::vector<std::wstring> arguments{ std::wstring(L"clone"), std::wstring(url), std::wstring(destinationPath) };
	if (options.recurseSubmodules) {
		arguments.push_back(std::wstring(L"--recurse-submodules"));
	}
	return arguments;
}

std::optional<GitCloneRequest> RunGitClonePrepare(const GitCloneCommandContext& context)
{
	if (!context.promptForUrl) {
		return std::nullopt;
	}
	const std::optional<std::wstring> url = context.promptForUrl(L"Repository URL", L"Repository URL", L"");
	if (!url.has_value() || url->empty()) {
		return std::nullopt;
	}

	const std::wstring folderName = DeriveGitCloneFolderName(*url);
	if (folderName.empty()) {
		if (context.message) {
			context.message(L"The repository URL did not resolve to a folder name.");
		}
		return std::nullopt;
	}

	if (!context.browseForParentDirectory) {
		return std::nullopt;
	}
	const std::optional<std::wstring> parentDirectory =
		context.browseForParentDirectory(L"Select Repository Location", context.homeDirectory);
	if (!parentDirectory.has_value() || parentDirectory->empty()) {
		return std::nullopt;
	}

	std::wstring destination = *parentDirectory;
	if (!destination.empty() && destination.back() != L'\\' && destination.back() != L'/') {
		destination += L'\\';
	}
	destination += folderName;

	if (context.pathState) {
		const EGitPathState state = context.pathState(destination);
		if (state == EGitPathState::NonEmpty) {
			// No recursive-delete primitive exists on this path yet, so a
			// non-empty destination is refused outright with an explanation
			// rather than presenting `BuildGitCloneOverwritePrompt` and being
			// unable to honor a "yes" — see this directory's CLAUDE.md.
			if (context.message) {
				context.message(
					L"A folder for the repository \"" + folderName + L"\" already exists and is not empty.");
			}
			return std::nullopt;
		}
	}

	return GitCloneRequest{ *url, destination };
}

GitExecutionResult RunGitCloneExecute(
	const GitCloneRequest& request, const GitCloneOptions& options, const GitCloneInvoker& invoker, HANDLE stop)
{
	if (!invoker) {
		GitExecutionResult result;
		result.status = EGitExecutionStatus::InvalidRequest;
		return result;
	}
	return invoker(BuildGitCloneArguments(request.url, request.destinationPath, options), stop);
}

GitCloneCommandResult RunGitCloneComplete(const GitCloneRequest& request, const GitExecutionResult& result)
{
	if (result.status == EGitExecutionStatus::Cancelled) {
		return MakeCloneCancelled(L"Cloning was cancelled.");
	}
	if (GitCommandFailed(result)) {
		return MakeCloneFailed(DescribeGitFailure(result));
	}
	return MakeCloneSucceeded(request.destinationPath);
}

// ---------------------------------------------------------------------------
// Source Control empty-state welcome content
// ---------------------------------------------------------------------------

GitScmWelcomeModel BuildGitScmWelcomeModel(bool hasFolder, bool hasRepository)
{
	GitScmWelcomeModel model;

	if (hasRepository) {
		model.content = EGitScmWelcomeContent::None;
		return model;
	}

	if (hasFolder) {
		model.content = EGitScmWelcomeContent::FolderNoRepository;
		model.message = std::wstring(kFolderNoRepositoryMessage);
		// `git.init?[true]`: upstream's `skipFolderPrompt` argument, decoded
		// from its `%5Btrue%5D`-encoded link target.
		model.actions.push_back(GitScmWelcomeAction{ std::wstring(kInitializeRepositoryLabel), "git.init", "[true]" });
		return model;
	}

	model.content = EGitScmWelcomeContent::EmptyWorkbench;
	model.message = std::wstring(kEmptyWorkbenchMessage);
	// Upstream also links `Open Folder` (`command:vscode.openFolder`) here;
	// that command has no route in this product yet and is out of this
	// pass's scope, so only `Clone Repository` is modeled.
	model.actions.push_back(GitScmWelcomeAction{ std::wstring(kCloneRepositoryLabel), "git.clone", "" });
	return model;
}

} // namespace workbench::scm
