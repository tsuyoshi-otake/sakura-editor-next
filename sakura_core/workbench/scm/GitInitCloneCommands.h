/*! @file
 * @brief `git.init` and `git.clone`, and the Source Control empty-state welcome model.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitPrompt.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! Stable resource keys for SCM presentation text.  The command models remain
//! HWND-free; the native workbench supplies a resolver backed by the Sakura
//! language resources when presenting these values.
enum class EScmTextKey : std::uint16_t {
	SourceControlTitle,
	RepositoriesTitle,
	ChangesTitle,
	GraphTitle,
	GraphUnavailable,
	GitFolderNoRepository,
	GitEmptyWorkbench,
	GitWorkspaceNoRepository,
	GitEmptyWorkspace,
	GitInitializeRepository,
	GitCloneRepository,
	GitAddFolderToWorkspace,
	GitChooseFolder,
	GitOpenFolder,
	GitOpen,
	GitInitHomeWarning,
	GitInitOpenPrompt,
	GitInitFolderPicker,
	GitInitSuccess,
	GitInitCancelledHome,
	GitCloneOverwriteWarning,
	GitOverwrite,
	GitRepositoryUrl,
	GitInvalidUrlFolder,
	GitRepositoryLocation,
	GitCloneNonEmpty,
	GitCloneCancelled,
	GitOpenChanges,
	GitOpenFile,
	GitStageChanges,
	GitUnstageChanges,
	GitDiscardChanges,
	GitStageAllChanges,
	GitUnstageAllChanges,
	GitDiscardAllChanges,
	GitPublishBranch,
	GitSynchronizeChanges,
	GitCheckoutBranchTag,
	GitCommitMessage,
	//! The Source Control action button and its dropdown. These are the bare
	//! titles the button renders, not the `Git: ...` Command Palette titles.
	GitCommitAction,
	GitCommitAmendAction,
	GitCommitAndPushAction,
	GitCommitAndSyncAction,
};

using ScmTextResolver = std::function<std::wstring(EScmTextKey, std::wstring_view)>;

// ---------------------------------------------------------------------------
// git.init
// ---------------------------------------------------------------------------

//! One already-open workspace folder, as `git.init` needs to see it: just the
//! display name and the filesystem path upstream reads from
//! `WorkspaceFolder.name` / `WorkspaceFolder.uri.fsPath`.
struct GitInitWorkspaceFolder final {
	std::wstring name;
	std::wstring path;

	[[nodiscard]] bool operator==(const GitInitWorkspaceFolder&) const = default;
};

//! One selectable row in the "which folder" picker `git.init` shows when more
//! than one workspace folder is open, or when a single folder is open but
//! `skipFolderPrompt` was not asked for. Mirrors upstream's
//! `{ label, description, folder }` / trailing `{ label: 'Choose Folder...' }`
//! item shapes from `extensions/git/src/commands.ts`'s `init` command.
struct GitInitFolderPickItem final {
	std::wstring label;
	std::wstring description;
	//! Empty for the trailing "Choose Folder..." row, which names no folder.
	std::wstring path;

	[[nodiscard]] bool operator==(const GitInitFolderPickItem&) const = default;
};

//! Presents the folder-pick list built by `BuildGitInitFolderPickItems` and
//! returns the chosen index, or nothing on dismissal — upstream's
//! `showQuickPick` returning `undefined`.
using GitInitFolderPickPresenter = std::function<std::optional<std::size_t>(
	const std::vector<GitInitFolderPickItem>& items, std::wstring_view placeholder)>;

//! Upstream's native folder-browse dialog (`window.showOpenDialog` with
//! `canSelectFolders: true, canSelectMany: false`). Returns the chosen
//! absolute path, or nothing when the user cancels.
using GitFolderBrowser = std::function<std::optional<std::wstring>(
	std::wstring_view openLabel, std::wstring_view startingDirectory)>;

//!
//! @brief Runs one git command in a working directory resolved at call time.
//!
//! Unlike every other command family in this directory, `git.init`'s working
//! directory is exactly the thing the command itself resolves (from the
//! folder pick or the folder browser), so it cannot be closed over when the
//! context is built. `workingDirectory` becomes `RunGit`'s `-C` argument.
//!
using GitInitCommandInvoker = std::function<GitExecutionResult(
	std::wstring_view workingDirectory, const std::vector<std::wstring>& arguments)>;

//! Shows one human-readable message: upstream's warnings and failure reasons,
//! the same shape `GitBranchCommandContext::message` / `GitSyncCommandContext::
//! message` already use elsewhere in this directory.
using GitMessagePresenter = std::function<void(std::wstring_view message)>;

enum class EGitInitCommandStatus : std::uint8_t {
	Succeeded,
	//! The user dismissed the folder pick, the folder browser, or the home-
	//! directory guard. Not a failure.
	Cancelled,
	//! A git invocation did not succeed, or a required presenter was missing.
	Failed,
};

//!
//! @brief What should happen to the freshly initialized folder.
//!
//! This model only *decides*; actually opening a folder in this or another
//! window is a separate, runtime-owned capability this command does not
//! reach — see `sakura_core/workbench/scm/CLAUDE.md`'s "git.init and
//! git.clone" section for the recorded gap.
//!
enum class EGitInitPostAction : std::uint8_t {
	//! `skipFolderPrompt` resolved a single already-open folder: upstream's
	//! `askToOpen = false` path. There is nothing further to offer.
	AlreadyOpen,
	//! The user should be asked whether to open the new repository, exactly as
	//! upstream's `Would you like to open the initialized repository?` prompt
	//! does after a folder-browser pick.
	OfferToOpen,
};

struct GitInitCommandResult final {
	EGitInitCommandStatus status{ EGitInitCommandStatus::Failed };
	//! The initialized repository's absolute path. Empty unless `status ==
	//! Succeeded`.
	std::wstring repositoryPath;
	EGitInitPostAction postAction{ EGitInitPostAction::OfferToOpen };
	//! Populated for every non-`Succeeded` status: the failure text on
	//! `Failed`, and the reason the command stopped on `Cancelled`.
	std::wstring message;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitInitCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitInitCommandResult&) const = default;
};

/*!
	@brief Everything `git.init` needs, injected. Deliberately HWND-free.
*/
struct GitInitCommandContext final {
	//! Every currently open workspace folder, in upstream's
	//! `workspace.workspaceFolders` order. Empty for the Empty workbench state.
	std::vector<GitInitWorkspaceFolder> openFolders;
	//! The user's home directory, for the guard in
	//! `IsGitInitHomeDirectoryGuardTriggered`. Upstream reads `os.homedir()`.
	std::wstring homeDirectory;

	GitInitCommandInvoker run;
	GitInitFolderPickPresenter folderPick;
	GitFolderBrowser browseForFolder;
	GitPromptPresenter confirm;
	GitMessagePresenter message;
	//! Optional presentation resolver. Empty keeps the model's English fallback.
	ScmTextResolver text;
};

//! Upstream's `Choose Folder...` row is always appended after the real
//! folders; this builds exactly that list, in exactly that order.
[[nodiscard]] std::vector<GitInitFolderPickItem> BuildGitInitFolderPickItems(
	const std::vector<GitInitWorkspaceFolder>& openFolders, const ScmTextResolver& text = {});

//!
//! @brief Upstream's home-directory guard from `git.init`.
//!
//! `homeUri` must be a **prefix** of the chosen path, compared the way
//! upstream compares it: on the string form, case-sensitively, with no
//! path-boundary normalization beyond what the two strings already carry. A
//! sibling directory that merely shares the home directory's prefix
//! character-for-character (e.g. `C:\Users2` against a home of `C:\Users`) is
//! upstream's own edge case, not one introduced here.
//!
[[nodiscard]] bool IsGitInitHomeDirectoryGuardTriggered(std::wstring_view homeDirectory, std::wstring_view chosenPath);

//! `'This will create a Git repository in "{0}". Are you sure you want to
//! initialize a Git repository in your home directory?'`, with the single
//! `Initialize Repository` button upstream shows there.
[[nodiscard]] GitPrompt BuildGitInitHomeDirectoryPrompt(
	std::wstring_view chosenPath, const ScmTextResolver& text = {});

//! `'Would you like to open the initialized repository?'` Upstream also
//! offers `Open in New Window` and `Add to Workspace`; neither has a route
//! here yet (no multi-window open, no workspace-folder mutation), so only
//! `Open` is offered — see the recorded divergence in this directory's
//! `CLAUDE.md`.
[[nodiscard]] GitPrompt BuildGitInitOpenPrompt(const ScmTextResolver& text = {});

//! `git init` takes no arguments beyond the working directory `RunGit`
//! already threads through `-C`, so this returns the fixed `["init"]`
//! upstream sends. Kept as a named builder, matching every other command in
//! this directory, so a future upstream flag lands in one place.
[[nodiscard]] std::vector<std::wstring> BuildGitInitArguments();

//!
//! @brief Decodes the wire payload a `git.init` invocation carries into the
//! `skipFolderPrompt` bool `RunGitInit` takes.
//!
//! The Source Control welcome content's `Initialize Repository` action
//! (`BuildGitScmWelcomeModel`) dispatches `git.init` with `argumentsJson ==
//! "[true]"`, mirroring upstream's `command:git.init?%5Btrue%5D` welcome-
//! content link, which VS Code decodes as a one-element JSON array holding
//! `true`. A Command Palette invocation carries no arguments at all. Both are
//! legitimate: an empty payload resolves to `false` (ask, as if no argument
//! were passed — upstream's own default), and any payload that is present but
//! is not exactly `[true]` or `[false]` also resolves to `false`, the safe
//! choice that shows the folder prompt rather than silently skipping it.
//!
[[nodiscard]] bool ParseGitInitSkipFolderPromptArgument(std::string_view argumentsJson);

//!
//! @brief Upstream's `git.init(skipFolderPrompt)`.
//!
//! `skipFolderPrompt == true` and exactly one open folder resolves that
//! folder with no prompt at all — upstream's own fast path, and the one the
//! Source Control empty state's `command:git.init?[true]` link relies on. Any
//! other combination of open folders shows the folder pick, whose trailing
//! "Choose Folder..." row and a genuinely empty workbench both fall through
//! to the folder-browser dialog, guarded by the home-directory confirmation.
//!
[[nodiscard]] GitInitCommandResult RunGitInit(const GitInitCommandContext& context, bool skipFolderPrompt);

// ---------------------------------------------------------------------------
// git.clone
// ---------------------------------------------------------------------------

//! Does a path exist, and if so, is it usable as a clone destination? Injected
//! so the model never touches the real filesystem: this is the seam that lets
//! `git.clone`'s tests run without creating or reading a single real directory.
enum class EGitPathState : std::uint8_t {
	//! Nothing at this path yet: git may create it.
	Absent,
	//! An existing, empty directory: `git clone` may still target it.
	EmptyDirectory,
	//! An existing, non-empty directory, or an existing file: refuse.
	NonEmpty,
};
using GitPathExistsPredicate = std::function<EGitPathState(std::wstring_view path)>;

//! A plain input box, standing in for upstream's live-typed remote-source
//! Quick Pick (`pickRemoteSource` in `extensions/git-base/src/remoteSource.ts`).
//! `CQuickInputDialog` cannot render a Quick Pick item that updates as
//! the user types, so `git.clone` degrades to the same plain "provide a value"
//! box the checkout/branch commands already use for text entry — see
//! `GitBranchCommands.h`'s identically-shaped `GitInputBoxPresenter`.
using GitCloneUrlPresenter = std::function<std::optional<std::wstring>(
	std::wstring_view prompt, std::wstring_view placeholder, std::wstring_view value)>;

//!
//! @brief Runs a git command that may run for a long time and can be
//! cancelled mid-flight.
//!
//! `stop` is threaded straight through to `RunGit(request, stop)`, so a caller
//! that wires a real cancellation event gets real cancellation and a caller
//! that passes `nullptr` gets the ordinary blocking behavior every other
//! command family in this directory already has.
//!
using GitCloneInvoker = std::function<GitExecutionResult(const std::vector<std::wstring>& arguments, HANDLE stop)>;

enum class EGitCloneCommandStatus : std::uint8_t {
	Succeeded,
	//! The user dismissed the URL prompt, the destination browser, or the
	//! overwrite guard. Not a failure.
	Cancelled,
	//! A git invocation did not succeed, or a required presenter was missing.
	Failed,
};

//! `git.defaultCloneDirectory`. Hard-coded absent (no configured default) at
//! this pass, following this subsystem's established pattern of carrying an
//! upstream setting at its documented default rather than reading
//! configuration that cannot yet be read — see `CLAUDE.md`'s "Divergences".
struct GitCloneOptions final {
	bool recurseSubmodules{ false };

	[[nodiscard]] bool operator==(const GitCloneOptions&) const = default;
};

//! The resolved inputs to a clone, produced by `RunGitClonePrepare` and
//! consumed by `RunGitCloneExecute`. A value here means every prompt the
//! upstream flow shows has already been answered.
struct GitCloneRequest final {
	std::wstring url;
	//! The full destination directory the repository will be cloned into —
	//! upstream's `parentPath` joined with `DeriveGitCloneFolderName(url)`.
	std::wstring destinationPath;

	[[nodiscard]] bool operator==(const GitCloneRequest&) const = default;
};

struct GitCloneCommandResult final {
	EGitCloneCommandStatus status{ EGitCloneCommandStatus::Failed };
	std::wstring repositoryPath;
	//! Populated for every non-`Succeeded` status.
	std::wstring message;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitCloneCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitCloneCommandResult&) const = default;
};

/*!
	@brief Everything `git.clone`'s prepare phase needs, injected. HWND-free.
*/
struct GitCloneCommandContext final {
	//! The user's home directory, passed as `browseForParentDirectory`'s
	//! starting directory — the same role `GitInitCommandContext::
	//! homeDirectory` plays for `git.init`'s folder browser.
	std::wstring homeDirectory;

	GitCloneUrlPresenter promptForUrl;
	GitFolderBrowser browseForParentDirectory;
	GitPathExistsPredicate pathState;
	GitPromptPresenter confirm;
	GitMessagePresenter message;
	ScmTextResolver text;
};

//! Upstream's `getRepositoryName` (`extensions/git-base/src/remoteSource.ts` /
//! `git.ts`'s clone command): the last path segment, with a trailing `.git`
//! and a trailing slash both stripped.
[[nodiscard]] std::wstring DeriveGitCloneFolderName(std::wstring_view url);

//! `'A folder for the repository "{0}" already exists and is not empty. Do
//! you want to overwrite it?'`, upstream's own guard before cloning into an
//! existing directory. Built and exposed for reuse, but `RunGitClonePrepare`
//! does not currently present it — see that function's own comment for why.
[[nodiscard]] GitPrompt BuildGitCloneOverwritePrompt(
	std::wstring_view folderName, const ScmTextResolver& text = {});

//! `["clone", url, destinationPath]`, plus `--recurse-submodules` when
//! `options.recurseSubmodules` is set — upstream's own conditional argument
//! from `Git.clone`.
[[nodiscard]] std::vector<std::wstring> BuildGitCloneArguments(
	std::wstring_view url, std::wstring_view destinationPath, const GitCloneOptions& options);

//!
//! @brief Phase 1: resolve the URL and destination, and validate the
//! destination is usable.
//!
//! Returns nothing when the user cancels (an empty URL or a dismissed folder
//! browser) and also when the resolved destination cannot be used. Upstream
//! offers to delete and overwrite an existing non-empty destination; that
//! requires an actual recursive-delete primitive this pass does not add, so a
//! `NonEmpty` `pathState` is refused outright with an explanatory message
//! instead of presenting a confirmation whose "yes" this code could not
//! honor — see this directory's `CLAUDE.md` for the recorded divergence.
//! `BuildGitCloneOverwritePrompt` stays exposed for the pass that adds the
//! delete primitive and can wire it in without changing this function's
//! contract.
//!
[[nodiscard]] std::optional<GitCloneRequest> RunGitClonePrepare(const GitCloneCommandContext& context);

//!
//! @brief Phase 2: run the (potentially long) `git clone` itself.
//!
//! Kept separate from prepare/complete so a caller can run it off the UI
//! thread and poll or wait on `stop` for cancellation without also having to
//! re-run prompt presentation.
//!
[[nodiscard]] GitExecutionResult RunGitCloneExecute(
	const GitCloneRequest& request, const GitCloneOptions& options, const GitCloneInvoker& invoker, HANDLE stop);

//! Phase 3: turn the raw execution result into the typed command result.
[[nodiscard]] GitCloneCommandResult RunGitCloneComplete(const GitCloneRequest& request, const GitExecutionResult& result);

// ---------------------------------------------------------------------------
// Source Control empty-state welcome content
// ---------------------------------------------------------------------------

//!
//! @brief Which welcome content the Source Control view's empty state shows.
//!
//! The caller projects the real `workbenchState` plus its exact folder shape
//! into this enum. Keeping the two `.code-workspace` variants distinct prevents
//! a workspace with zero folders from accidentally offering `git.init`, and
//! prevents a workspace with folders from looking like a completely empty
//! window.
enum class EGitScmWelcomeWorkspaceState : std::uint8_t {
	//! `workbenchState == empty`.
	Empty,
	//! `workbenchState == folder`, which necessarily has exactly one folder.
	Folder,
	//! `workbenchState == workspace` and the workspace has one or more folders.
	WorkspaceWithFolders,
	//! `workbenchState == workspace` and `workspaceFolderCount == 0`.
	WorkspaceWithoutFolders,
};
//!
	enum class EGitScmWelcomeContent : std::uint8_t {
	//! A repository is open: no welcome content at all.
	None,
	//! Folder workbench state, no repository open: upstream's
	//! `view.workbench.scm.folder`, `Initialize Repository` only.
	FolderNoRepository,
	//! Multi-folder workspace, no repository open: upstream's
	//! `view.workbench.scm.workspace`, `Initialize Repository` with no argument.
	WorkspaceNoRepository,
	//! Saved/untitled workspace containing no folders: upstream's
	//! `view.workbench.scm.emptyWorkspace`, `Add Folder to Workspace` only.
	EmptyWorkspace,
	//! Empty workbench state (no folder open): upstream's
	//! `view.workbench.scm.empty`, with `Open Folder` followed by
	//! `Clone Repository`.
	EmptyWorkbench,
};

//! One clickable link inside the welcome content, carrying the exact command
//! id and JSON arguments a `CommandCallback` already accepts — the same shape
//! `ScmCommand::command` / `argumentsJson` use elsewhere in this directory.
struct GitScmWelcomeAction final {
	std::wstring label;
	std::string command;
	std::string argumentsJson;

	[[nodiscard]] bool operator==(const GitScmWelcomeAction&) const = default;
};

struct GitScmWelcomeModel final {
	EGitScmWelcomeContent content{ EGitScmWelcomeContent::None };
	std::wstring message;
	std::vector<GitScmWelcomeAction> actions;

	[[nodiscard]] bool operator==(const GitScmWelcomeModel&) const = default;
};

//!
//! @brief Decides which welcome content applies, and builds its exact text
//! and command.
//!
//! `workspaceState` is the composition root's projection of the runtime's
//! semantic workspace snapshot; `hasRepository` is `SourceControlService`
//! currently publishing a provider. Upstream's Git availability and special
//! repository gates (`git.missing`, `git.parentRepositoryCount`,
//! `git.unsafeRepositoryCount`, `git.closedRepositoryCount`) still have no
//! backing native state and remain omitted rather than fabricated.
//!
[[nodiscard]] GitScmWelcomeModel BuildGitScmWelcomeModel(
	EGitScmWelcomeWorkspaceState workspaceState, bool hasRepository, const ScmTextResolver& text = {});

} // namespace workbench::scm
