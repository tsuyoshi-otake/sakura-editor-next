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
//! at tag 1.95.3 (`view.workbench.scm.empty`, `.folder`, `.workspace`, and
//! `.emptyWorkspace`).
//! The markdown link syntax and the "read our docs" trailer are not carried
//! here: this model exposes the same command each link names as a distinct,
//! typed `GitScmWelcomeAction` instead of embedded markdown, matching how the
//! repository band already separates its own commands from its label text.
constexpr std::wstring_view kFolderNoRepositoryMessage =
	L"The currently open folder does not contain a Git repository. Initialize a repository "
	L"to enable source control features powered by Git.";
constexpr std::wstring_view kEmptyWorkbenchMessage =
	L"To use Git features, open a folder containing a Git repository or clone from a URL.";
constexpr std::wstring_view kWorkspaceNoRepositoryMessage =
	L"The currently open workspace does not contain a folder with a Git repository. "
	L"Initialize a repository in a folder to enable source control features powered by Git.";
constexpr std::wstring_view kEmptyWorkspaceMessage =
	L"The currently open workspace does not contain a folder with a Git repository.";

constexpr std::wstring_view kInitializeRepositoryLabel = L"Initialize Repository";
constexpr std::wstring_view kCloneRepositoryLabel = L"Clone Repository";
constexpr std::wstring_view kAddFolderToWorkspaceLabel = L"Add Folder to Workspace";
constexpr std::wstring_view kChooseFolderLabel = L"Choose Folder...";
constexpr std::wstring_view kOpenFolderLabel = L"Open Folder";
constexpr std::wstring_view kOpenLabel = L"Open";

std::wstring Text(const ScmTextResolver& resolver, EScmTextKey key,
	std::wstring_view fallback, std::wstring_view argument = {})
{
	if (resolver) {
		if (std::wstring value = resolver(key, argument); !value.empty()) return value;
	}
	return std::wstring(fallback);
}

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

std::vector<GitInitFolderPickItem> BuildGitInitFolderPickItems(
	const std::vector<GitInitWorkspaceFolder>& openFolders, const ScmTextResolver& text)
{
	std::vector<GitInitFolderPickItem> items;
	items.reserve(openFolders.size() + 1);
	for (const GitInitWorkspaceFolder& folder : openFolders) {
		items.push_back(GitInitFolderPickItem{ folder.name, folder.path, folder.path });
	}
	// Upstream always appends this row after the real folders; an empty
	// `path` is what the caller uses to recognize it and fall through to the
	// folder browser.
	items.push_back(GitInitFolderPickItem{ Text(text, EScmTextKey::GitChooseFolder, kChooseFolderLabel), {}, {} });
	return items;
}

bool IsGitInitHomeDirectoryGuardTriggered(std::wstring_view homeDirectory, std::wstring_view chosenPath)
{
	if (homeDirectory.empty() || chosenPath.size() < homeDirectory.size()) {
		return false;
	}
	return chosenPath.substr(0, homeDirectory.size()) == homeDirectory;
}

GitPrompt BuildGitInitHomeDirectoryPrompt(std::wstring_view chosenPath, const ScmTextResolver& text)
{
	GitPrompt prompt;
	if (text) {
		prompt.message = Text(text, EScmTextKey::GitInitHomeWarning,
			L"This will create a Git repository in \"{0}\". Are you sure you want to initialize a Git repository in your home directory?", chosenPath);
		const std::wstring marker = L"{0}";
		if (const std::size_t at = prompt.message.find(marker); at != std::wstring::npos) {
			prompt.message.replace(at, marker.size(), chosenPath);
		}
		prompt.choices = { Text(text, EScmTextKey::GitInitializeRepository, kInitializeRepositoryLabel) };
		prompt.warning = true;
		prompt.modal = true;
		return prompt;
	}
	prompt.message = L"\u300c" + std::wstring(chosenPath) +
		L"\u300d\u306b Git \u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u4f5c\u6210\u3057\u307e\u3059\u3002\u30db\u30fc\u30e0\u30c7\u30a3\u30ec\u30af\u30c8\u30ea\u306b Git \u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u521d\u671f\u5316\u3057\u3066\u3082\u3088\u308d\u3057\u3044\u3067\u3059\u304b\uff1f";
	prompt.choices = { std::wstring(kInitializeRepositoryLabel) };
	prompt.warning = true;
	prompt.modal = true;
	return prompt;
}

GitPrompt BuildGitInitOpenPrompt(const ScmTextResolver& text)
{
	GitPrompt prompt;
	if (text) {
		prompt.message = Text(text, EScmTextKey::GitInitOpenPrompt,
			L"Would you like to open the initialized repository?");
		prompt.choices = { Text(text, EScmTextKey::GitOpen, kOpenLabel) };
		prompt.warning = false;
		prompt.modal = false;
		return prompt;
	}
	prompt.message = L"\u521d\u671f\u5316\u3057\u305f\u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u958b\u304d\u307e\u3059\u304b\uff1f";
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
			return MakeInitFailed(L"\u30ea\u30dd\u30b8\u30c8\u30ea\u306e\u521d\u671f\u5316\u5148\u3092\u9078\u629e\u3059\u308b\u30d5\u30a9\u30eb\u30c0\u30fc\u30d4\u30c3\u30ab\u30fc\u3092\u5229\u7528\u3067\u304d\u307e\u305b\u3093\u3002");
		}

		const std::vector<GitInitFolderPickItem> items = BuildGitInitFolderPickItems(context.openFolders, context.text);
		const std::optional<std::size_t> picked =
			context.folderPick(items, L"Git \u30ea\u30dd\u30b8\u30c8\u30ea\u3092\u521d\u671f\u5316\u3059\u308b\u30ef\u30fc\u30af\u30b9\u30da\u30fc\u30b9\u30d5\u30a9\u30eb\u30c0\u30fc\u3092\u9078\u629e");
		if (!picked.has_value() || *picked >= items.size()) {
			return MakeInitCancelled();
		}

		const GitInitFolderPickItem& item = items[*picked];
		if (!item.path.empty()) {
			chosenPath = item.path;
			postAction = EGitInitPostAction::AlreadyOpen;
		} else {
			if (!context.browseForFolder) {
				return MakeInitFailed(L"\u30ea\u30dd\u30b8\u30c8\u30ea\u306e\u521d\u671f\u5316\u5148\u3092\u9078\u629e\u3059\u308b\u30d5\u30a9\u30eb\u30c0\u30fc\u30d6\u30e9\u30a6\u30b6\u30fc\u3092\u5229\u7528\u3067\u304d\u307e\u305b\u3093\u3002");
			}
			const std::optional<std::wstring> browsed =
				context.browseForFolder(Text(context.text, EScmTextKey::GitInitializeRepository, kInitializeRepositoryLabel), context.homeDirectory);
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
				L"\u30db\u30fc\u30e0\u30c7\u30a3\u30ec\u30af\u30c8\u30ea\u3078\u306e\u30ea\u30dd\u30b8\u30c8\u30ea\u521d\u671f\u5316\u3092\u78ba\u8a8d\u3059\u308b\u30c0\u30a4\u30a2\u30ed\u30b0\u3092\u5229\u7528\u3067\u304d\u307e\u305b\u3093\u3002");
		}
		const std::optional<std::size_t> choice = context.confirm(BuildGitInitHomeDirectoryPrompt(chosenPath, context.text));
		if (!choice.has_value() || *choice != 0) {
			return MakeInitCancelled(Text(context.text, EScmTextKey::GitInitCancelledHome,
				L"Initializing a Git repository in the home directory was cancelled"));
		}
	}

	if (!context.run) {
		return MakeInitFailed(L"git init \u3092\u5b9f\u884c\u3059\u308b Git \u547c\u3073\u51fa\u3057\u6a5f\u80fd\u3092\u5229\u7528\u3067\u304d\u307e\u305b\u3093\u3002");
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
		std::wstring message = Text(context.text, EScmTextKey::GitInitSuccess,
			L"Initialized a repository in \"{0}\"", chosenPath);
		const std::wstring marker = L"{0}";
		if (const std::size_t at = message.find(marker); at != std::wstring::npos) {
			message.replace(at, marker.size(), chosenPath);
		}
		context.message(message);
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

GitPrompt BuildGitCloneOverwritePrompt(std::wstring_view folderName, const ScmTextResolver& text)
{
	GitPrompt prompt;
	if (text) {
		prompt.message = Text(text, EScmTextKey::GitCloneOverwriteWarning,
			L"A folder for the repository \"{0}\" already exists and is not empty. Do you want to overwrite it?", folderName);
		const std::wstring marker = L"{0}";
		if (const std::size_t at = prompt.message.find(marker); at != std::wstring::npos) {
			prompt.message.replace(at, marker.size(), folderName);
		}
		prompt.choices = { Text(text, EScmTextKey::GitOverwrite, L"Overwrite") };
		prompt.warning = true;
		prompt.modal = true;
		return prompt;
	}
	prompt.message = L"\u30ea\u30dd\u30b8\u30c8\u30ea\u7528\u306e\u30d5\u30a9\u30eb\u30c0\u30fc\u300c" + std::wstring(folderName) +
		L"\u300d\u306f\u3059\u3067\u306b\u5b58\u5728\u3057\u3001\u7a7a\u3067\u306f\u3042\u308a\u307e\u305b\u3093\u3002\u4e0a\u66f8\u304d\u3057\u307e\u3059\u304b\uff1f";
	prompt.choices = { std::wstring(L"\u4e0a\u66f8\u304d") };
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
	const std::wstring repositoryUrl = Text(context.text, EScmTextKey::GitRepositoryUrl, L"Repository URL");
	const std::optional<std::wstring> url = context.promptForUrl(repositoryUrl, repositoryUrl, L"");
	if (!url.has_value() || url->empty()) {
		return std::nullopt;
	}

	const std::wstring folderName = DeriveGitCloneFolderName(*url);
	if (folderName.empty()) {
		if (context.message) {
			context.message(Text(context.text, EScmTextKey::GitInvalidUrlFolder,
				L"Could not derive a repository folder name from the URL"));
		}
		return std::nullopt;
	}

	if (!context.browseForParentDirectory) {
		return std::nullopt;
	}
		const std::optional<std::wstring> parentDirectory =
			context.browseForParentDirectory(Text(context.text, EScmTextKey::GitRepositoryLocation,
				L"Repository location"), context.homeDirectory);
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
				context.message(Text(context.text, EScmTextKey::GitCloneNonEmpty,
					L"The repository folder \"{0}\" already exists and is not empty", folderName));
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
		return MakeCloneCancelled(L"\u30af\u30ed\u30fc\u30f3\u306f\u30ad\u30e3\u30f3\u30bb\u30eb\u3055\u308c\u307e\u3057\u305f\u3002");
	}
	if (GitCommandFailed(result)) {
		return MakeCloneFailed(DescribeGitFailure(result));
	}
	return MakeCloneSucceeded(request.destinationPath);
}

// ---------------------------------------------------------------------------
// Source Control empty-state welcome content
// ---------------------------------------------------------------------------

GitScmWelcomeModel BuildGitScmWelcomeModel(
	EGitScmWelcomeWorkspaceState workspaceState, bool hasRepository, const ScmTextResolver& text)
{
	GitScmWelcomeModel model;

	if (hasRepository) {
		model.content = EGitScmWelcomeContent::None;
		return model;
	}

	switch (workspaceState) {
	case EGitScmWelcomeWorkspaceState::Folder:
		model.content = EGitScmWelcomeContent::FolderNoRepository;
		model.message = Text(text, EScmTextKey::GitFolderNoRepository, kFolderNoRepositoryMessage);
		// `git.init?[true]`: upstream's `skipFolderPrompt` argument, decoded
		// from its `%5Btrue%5D`-encoded link target.
		model.actions.push_back(GitScmWelcomeAction{ Text(text, EScmTextKey::GitInitializeRepository, kInitializeRepositoryLabel), "git.init", "[true]" });
		return model;
	case EGitScmWelcomeWorkspaceState::WorkspaceWithFolders:
		model.content = EGitScmWelcomeContent::WorkspaceNoRepository;
		model.message = Text(text, EScmTextKey::GitWorkspaceNoRepository, kWorkspaceNoRepositoryMessage);
		// Unlike the single-folder welcome link, the user must choose one of the
		// workspace folders, so upstream passes no `skipFolderPrompt` argument.
		model.actions.push_back(GitScmWelcomeAction{ Text(text, EScmTextKey::GitInitializeRepository, kInitializeRepositoryLabel), "git.init", "" });
		return model;
	case EGitScmWelcomeWorkspaceState::WorkspaceWithoutFolders:
		model.content = EGitScmWelcomeContent::EmptyWorkspace;
		model.message = Text(text, EScmTextKey::GitEmptyWorkspace, kEmptyWorkspaceMessage);
		model.actions.push_back(GitScmWelcomeAction{
			Text(text, EScmTextKey::GitAddFolderToWorkspace, kAddFolderToWorkspaceLabel), "workbench.action.addRootFolder", "" });
		return model;
	case EGitScmWelcomeWorkspaceState::Empty:
		break;
	}

	model.content = EGitScmWelcomeContent::EmptyWorkbench;
	model.message = Text(text, EScmTextKey::GitEmptyWorkbench, kEmptyWorkbenchMessage);
	// Keep the source order from the Git extension's viewsWelcome contribution.
	// These are API command ids, not local aliases: the command registry owns
	// their runtime routing.
	model.actions.push_back(GitScmWelcomeAction{
		Text(text, EScmTextKey::GitOpenFolder, kOpenFolderLabel), "vscode.openFolder", "" });
	model.actions.push_back(GitScmWelcomeAction{ Text(text, EScmTextKey::GitCloneRepository, kCloneRepositoryLabel), "git.cloneRecursive", "" });
	return model;
}

} // namespace workbench::scm
