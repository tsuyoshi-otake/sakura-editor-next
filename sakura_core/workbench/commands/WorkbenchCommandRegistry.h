/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/commands/WorkbenchContextKeyService.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::commands {

inline constexpr std::size_t kMaxWorkbenchCommandIdLength = 160;
inline constexpr std::size_t kMaxWorkbenchCommandExpressionLength = 1'024;

enum class EWorkbenchCommandSurface : std::uint8_t {
	CommandPalette,
	Menu,
	ActivityBar,
	Keybinding,
};

//! `legacyFunctionCode` is an integer compatibility alias, never an EFunctionCode dependency.
struct WorkbenchCommandSurfaceBinding {
	EWorkbenchCommandSurface surface = EWorkbenchCommandSurface::CommandPalette;
	std::string slotId;
	std::optional<std::int32_t> legacyFunctionCode;
	[[nodiscard]] bool operator==(const WorkbenchCommandSurfaceBinding&) const noexcept = default;
};

enum class EWorkbenchCommandExecutorTarget : std::uint8_t {
	None,
	Layout,
	Editor,
	Terminal,
	Debug,
	LegacyNative,
};

struct WorkbenchCommandDescriptor {
	std::string id;
	std::string title;
	WorkbenchCommandOwner owner;
	std::string whenClause;
	std::string enablementClause;
	EWorkbenchCommandExecutorTarget executorTarget = EWorkbenchCommandExecutorTarget::None;
	std::vector<WorkbenchCommandSurfaceBinding> surfaceBindings;
};

enum class EWorkbenchCommandExecutionStatus : std::uint8_t {
	Succeeded,
	NotApplicable,
	Disabled,
	UnknownCommand,
	Unsupported,
	Failed,
};

struct WorkbenchCommandExecutionResult {
	EWorkbenchCommandExecutionStatus status = EWorkbenchCommandExecutionStatus::Failed;
	std::string detail;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EWorkbenchCommandExecutionStatus::Succeeded; }
};

using WorkbenchCommandExecutor = std::function<WorkbenchCommandExecutionResult()>;

/*!
	@brief An executor that receives the invocation's arguments payload.

	VS Code commands take arguments (`Command.arguments`), and a resource-scoped
	one is meaningless without them: `git.stage` must know *which* rows of *which*
	group it was asked to stage. An argument-less executor cannot express that, so
	this is an additive second executor shape rather than a change to the one
	above - the existing argument-less commands neither need nor gain a payload.

	The payload is opaque to the registry. Its shape is a contract between the
	surface that publishes the command's arguments and the executor the
	composition root binds, exactly as in VS Code.
*/
using WorkbenchCommandArgumentExecutor =
	std::function<WorkbenchCommandExecutionResult(std::string_view argumentsJson)>;

//! Optional bindings supplied by the native composition root. Empty executors remain explicitly Unsupported.
struct WorkbenchBuiltinCommandExecutors {
	WorkbenchCommandExecutor showCommands;
	WorkbenchCommandExecutor openSettings;
	WorkbenchCommandExecutor openFolder;
	WorkbenchCommandExecutor newUntitledFile;
	WorkbenchCommandExecutor newWindow;
	WorkbenchCommandExecutor openFile;
	WorkbenchCommandExecutor openWorkspace;
	WorkbenchCommandExecutor openRecent;
	WorkbenchCommandExecutor addRootFolder;
	WorkbenchCommandExecutor saveWorkspaceAs;
	WorkbenchCommandExecutor duplicateWorkspaceInNewWindow;
	WorkbenchCommandExecutor save;
	WorkbenchCommandExecutor saveAs;
	WorkbenchCommandExecutor saveAll;
	WorkbenchCommandExecutor closeActiveEditor;
	WorkbenchCommandExecutor closeFolder;
	WorkbenchCommandExecutor closeWindow;
	WorkbenchCommandExecutor quit;
	WorkbenchCommandExecutor openGlobalKeybindings;
	WorkbenchCommandExecutor toggleSidebarVisibility;
	WorkbenchCommandExecutor showExplorer;
	WorkbenchCommandExecutor showProblems;
	WorkbenchCommandExecutor toggleOutput;
	WorkbenchCommandExecutor selectTheme;
	WorkbenchCommandExecutor showNotifications;
	WorkbenchCommandExecutor hideNotifications;
	WorkbenchCommandExecutor toggleStatusbarVisibility;
	WorkbenchCommandExecutor markdownShowPreview;
	WorkbenchCommandExecutor markdownShowPreviewToSide;
	WorkbenchCommandExecutor markdownShowLockedPreviewToSide;
	WorkbenchCommandExecutor markdownShowSource;
	WorkbenchCommandExecutor markdownShowPreviewSecuritySelector;
	WorkbenchCommandExecutor markdownPreviewRefresh;
	WorkbenchCommandExecutor markdownPreviewToggleLock;
	WorkbenchCommandExecutor markdownReopenAsPreview;
	WorkbenchCommandExecutor markdownReopenAsSource;
	WorkbenchCommandExecutor markdownTogglePreview;
	//! VS Code's own **API commands** (`workbench/api/common/apiCommands.ts`).
	//! They exist so that any contributor - the built-in Git provider included -
	//! can open a comparison or a resource without knowing which editor will
	//! serve it. Upstream registers them through `CommandsRegistry` with no
	//! `MenuRegistry` contribution, so they carry no surface binding here either:
	//! they are callable, never listed. Both take arguments, because a comparison
	//! with no operands is not a comparison.
	WorkbenchCommandArgumentExecutor vscodeDiff;
	WorkbenchCommandArgumentExecutor vscodeOpen;
};

/*!
	@brief Executors for the built-in Git provider's branch commands.

	Kept separate from `WorkbenchBuiltinCommandExecutors` because they belong to
	one provider rather than to the workbench shell, and because upstream ships
	them from the `vscode.git` extension rather than from the workbench itself.
	A command left empty here still registers and still resolves, but executes as
	`Unsupported` - the sanctioned typed boundary.
*/
struct WorkbenchGitCommandExecutors {
	WorkbenchCommandExecutor checkout;
	WorkbenchCommandExecutor checkoutDetached;
	WorkbenchCommandExecutor branch;
	WorkbenchCommandExecutor branchFrom;
	//! Repository-creation-scoped: unlike every other member of this struct,
	//! `git.init` and `git.clone` are the two commands upstream still offers
	//! when **no** repository is open at all - their entire purpose is to
	//! create the state every other Git command requires. `git.init` carries
	//! the `skipFolderPrompt` boolean the built-in provider's `viewsWelcome`
	//! button passes (`git.init true`) as its argument payload; `git.clone`
	//! takes no argument, exactly like upstream's own zero-parameter handler.
	WorkbenchCommandArgumentExecutor init;
	WorkbenchCommandExecutor clone;
	//! Resource-scoped: upstream declares these on `scm/resourceState/context`
	//! and passes the selected `SourceControlResourceState` values as arguments.
	//! Which rows, and which group each row came from, is the whole operand.
	WorkbenchCommandArgumentExecutor stage;
	WorkbenchCommandArgumentExecutor unstage;
	WorkbenchCommandArgumentExecutor clean;
	//! `git.openChange`. Resource-scoped like the three above, and for the same
	//! reason: the operand is one row, and which group that row came from decides
	//! whether the comparison is HEAD-against-index or index-against-working-tree.
	WorkbenchCommandArgumentExecutor openChange;
	//! Group-scoped: upstream declares these on `scm/resourceGroup/context`, and
	//! their handlers take only the repository. The group is implied by the
	//! command, so they need no payload.
	WorkbenchCommandExecutor stageAll;
	WorkbenchCommandExecutor unstageAll;
	WorkbenchCommandExecutor cleanAll;
	//! Repository-scoped: upstream's `commitWithAnyInput` takes only the
	//! repository and reads the commit message off its own SCM input box, so
	//! these carry no payload either.
	WorkbenchCommandExecutor commit;
	WorkbenchCommandExecutor commitAmend;
	WorkbenchCommandExecutor undoCommit;
	//! Diff-editor-scoped: upstream's handlers take no operand at all. They read
	//! the active diff editor and its current selections, so the operand is the
	//! editor state at the moment of invocation rather than anything a caller
	//! could pass.
	WorkbenchCommandExecutor stageSelectedRanges;
	WorkbenchCommandExecutor unstageSelectedRanges;
	//! Remote-scoped: upstream's `fetch`, `pull`, `push`, `sync`, and `publish`
	//! handlers take only the repository. Which remote, which branch, and whether
	//! to rebase come from the repository's own HEAD/upstream state or from a
	//! Quick Pick, never from a caller's payload. The rebase and prune variants
	//! are separate commands upstream, so they are separate executors here rather
	//! than one executor reading a flag a caller invented.
	WorkbenchCommandExecutor fetch;
	WorkbenchCommandExecutor fetchPrune;
	WorkbenchCommandExecutor fetchAll;
	WorkbenchCommandExecutor pull;
	WorkbenchCommandExecutor pullRebase;
	WorkbenchCommandExecutor push;
	WorkbenchCommandExecutor sync;
	WorkbenchCommandExecutor syncRebase;
	WorkbenchCommandExecutor publish;
};

/*!
	@brief Executors for the Explorer's file-operation commands.

	Kept separate from `WorkbenchBuiltinCommandExecutors` because these belong
	to the Files Explorer feature (`workbench/contrib/files`) rather than to the
	workbench shell. A command left empty here still registers and still
	resolves, but executes as `Unsupported` - the sanctioned typed boundary.

	Every member is resource-scoped and therefore an argument executor: upstream
	passes the selected Explorer resource (`explorer.newFile`'s target folder,
	`renameFile`'s file, `copyFilePath`'s file, ...) as the command argument,
	and without it the command has no operand. The payload is the single-URI
	list `ExplorerCommandArguments.h` defines. Upstream additionally passes a
	multi-select list as a second argument to `moveFileToTrash`/`deleteFile`/
	`copyFilePath`; this native Explorer has no multi-select yet, so that second
	argument is deliberately absent from the payload contract rather than
	accepted and ignored.
*/
struct WorkbenchExplorerCommandExecutors {
	//! `explorer.newFile` / `explorer.newFolder`. The resource is the directory
	//! the new entry is created in.
	WorkbenchCommandArgumentExecutor newFile;
	WorkbenchCommandArgumentExecutor newFolder;
	//! `renameFile` (F2). The resource is the entry being renamed; the new name
	//! comes from the Explorer's inline input, exactly as upstream's handler
	//! reads it from the tree's rename box rather than from the argument list.
	WorkbenchCommandArgumentExecutor renameFile;
	//! `moveFileToTrash` (Delete) and `deleteFile` (Shift+Delete). Two distinct
	//! upstream commands, never one executor reading a flag: "Delete" moves to
	//! the recycle bin and "Delete Permanently" does not.
	WorkbenchCommandArgumentExecutor moveFileToTrash;
	WorkbenchCommandArgumentExecutor deleteFile;
	WorkbenchCommandArgumentExecutor copyFilePath;
	WorkbenchCommandArgumentExecutor copyRelativeFilePath;
	//! `revealFileInOS`, upstream's native-window command whose label is
	//! platform-selected; on Windows it is "Reveal in File Explorer".
	WorkbenchCommandArgumentExecutor revealFileInOS;
};

/*!
	@brief Executors for the update surfaces.

	Upstream splits the same four operations across two command families: the
	Command Palette actions in `update.contribution.ts`
	(`update.checkForUpdate`, `update.downloadUpdate`, `update.installUpdate`,
	`update.restartToUpdate`, `update.showUpdateInfo`) and the gear menu's
	`7_update` group in `update.ts` (`update.check`, `update.downloadNow`,
	`update.install`, `update.restart`). Both families call the same
	`IUpdateService` methods, so one executor is bound to both members of each
	pair rather than two callbacks that could drift apart.

	The four progress entries (`update.checking`, `update.downloading`,
	`update.updating`, `update.cancelling`) take no executor at all: upstream
	registers them with `precondition: false`, so they are labels showing what
	the update is doing, not actions. They register with an enablement clause of
	`false` and terminate as `Disabled`, never as `Unsupported`, because being
	unavailable *right now* is different from not being implemented.
*/
struct WorkbenchUpdateCommandExecutors {
	WorkbenchCommandExecutor checkForUpdates;
	WorkbenchCommandExecutor downloadUpdate;
	WorkbenchCommandExecutor applyUpdate;
	WorkbenchCommandExecutor quitAndInstall;
	WorkbenchCommandExecutor showUpdateInfo;
};

//! Upstream's title-bar entry, registered into `MenuId.TitleBarUpdate` at
//! order 0 with the title `"Update"`.
inline constexpr std::string_view kUpdateIndicatorCommandId = "workbench.actions.updateIndicator";

//! The states in which upstream's title-bar entry is visible
//! (`updateTitleBarEntry.ts`'s `ACTIONABLE_STATES`), as one `when` clause.
inline constexpr std::string_view kUpdateIndicatorWhenClause =
	"updateState == 'available for download' || updateState == 'downloaded' || updateState == 'ready'";

/*!
	@brief The command `workbench.actions.updateIndicator` delegates to.

	Upstream's title-bar entry is one button whose click runs a different command
	depending on the state it is showing. Resolving that here, from the same
	immutable context snapshot the `when` clause is evaluated against, keeps the
	choice in the pure layer and keeps the title bar from holding a second copy
	of the update state.

	Returns `std::nullopt` for any state outside `ACTIONABLE_STATES`, including a
	missing or non-string `updateState`. A non-actionable state has no command,
	which is exactly why the button is not visible in it.
*/
[[nodiscard]] std::optional<std::string> ResolveUpdateIndicatorCommand(
	const WorkbenchContextKeySnapshot& context);

enum class EWorkbenchCommandRegistrationStatus : std::uint8_t {
	Succeeded,
	NotApplicable,
	Invalid,
	Conflict,
};

struct WorkbenchCommandRegistrationResult {
	EWorkbenchCommandRegistrationStatus status = EWorkbenchCommandRegistrationStatus::Invalid;
	std::uint64_t revision{};
	[[nodiscard]] bool Succeeded() const noexcept { return status == EWorkbenchCommandRegistrationStatus::Succeeded; }
};

struct ResolvedWorkbenchCommandSurface {
	std::string commandId;
	WorkbenchCommandSurfaceBinding binding;
};

/*! 
	@brief Pure, window-local command registry shared by palettes, menus, activity, and keys.

	The registry owns command descriptors and their executor callback only. It does
	not know about HWNDs, CEditWnd, extension services, or named-pipe transports.
*/
class WorkbenchCommandRegistry final {
public:
	WorkbenchCommandRegistry() = default;
	WorkbenchCommandRegistry(const WorkbenchCommandRegistry&) = delete;
	WorkbenchCommandRegistry& operator=(const WorkbenchCommandRegistry&) = delete;

	[[nodiscard]] WorkbenchCommandRegistrationResult Register(
		WorkbenchCommandDescriptor descriptor, WorkbenchCommandExecutor executor = {});
	//! `Register` for a command whose executor needs the invocation's arguments.
	//! Deliberately a distinct name rather than an overload: `Register(d, {})`
	//! would otherwise become ambiguous, and which executor shape a command has
	//! is worth stating at the call site.
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterWithArguments(
		WorkbenchCommandDescriptor descriptor, WorkbenchCommandArgumentExecutor executor);
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterBuiltinCommands(
		WorkbenchBuiltinCommandExecutors executors = {});
	//! Registers the built-in Git provider's repository-creation commands
	//! (`git.init`, `git.clone`) and its branch commands (`git.checkout`,
	//! `git.checkoutDetached`, `git.branch`, `git.branchFrom`) and its working-
	//! tree commands (`git.stage`, `git.stageAll`, `git.unstage`,
	//! `git.unstageAll`, `git.clean`, `git.cleanAll`) and its commit commands
	//! (`git.commit`, `git.commitAmend`, `git.undoCommit`) and its remote
	//! commands (`git.fetch`, `git.fetchPrune`, `git.fetchAll`, `git.pull`,
	//! `git.pullRebase`, `git.push`, `git.sync`, `git.syncRebase`,
	//! `git.publish`) as one atomic batch, using upstream's own stable IDs,
	//! titles, and `when` clause. `git.init`/`git.clone` use a distinct
	//! always-available clause; see `MakeGitAlwaysAvailableDescriptor` in the
	//! implementation file.
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterGitCommands(
		WorkbenchGitCommandExecutors executors = {});
	//! Registers the Files Explorer's file-operation commands
	//! (`explorer.newFile`, `explorer.newFolder`, `renameFile`,
	//! `moveFileToTrash`, `deleteFile`, `copyFilePath`,
	//! `copyRelativeFilePath`, `revealFileInOS`) as one atomic batch, using
	//! upstream's own stable IDs and context-menu titles. Their Explorer-focus
	//! `when` conjuncts are context keys this native provider does not publish
	//! yet, so the clauses carry `workbenchReady` alone; see
	//! `MakeExplorerResourceDescriptor` in the implementation file.
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterExplorerCommands(
		WorkbenchExplorerCommandExecutors executors = {});
	//! Registers the update surfaces as one atomic batch: upstream's five Command
	//! Palette actions, its eight state-scoped `7_update` gear entries, and the
	//! title-bar entry `workbench.actions.updateIndicator`, using upstream's own
	//! stable IDs, titles, and `updateState` clauses.
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterUpdateCommands(
		WorkbenchUpdateCommandExecutors executors = {});
	[[nodiscard]] WorkbenchCommandRegistrationResult DisposeOwner(const WorkbenchCommandOwner& owner);
	[[nodiscard]] std::optional<WorkbenchCommandDescriptor> Find(std::string_view commandId) const;
	[[nodiscard]] std::optional<ResolvedWorkbenchCommandSurface> ResolveSurface(
		EWorkbenchCommandSurface surface, std::string_view slotId) const;
	//! Finds the stable descriptor owning an integer compatibility alias. The
	//! registry intentionally stores raw integers, never generated function enums.
	[[nodiscard]] std::optional<std::string> ResolveLegacyFunctionCode(std::int32_t functionCode) const;
	//! Returns owning descriptor copies in stable command-ID order for one surface.
	//! A command with multiple bindings for the surface appears exactly once.
	[[nodiscard]] std::vector<WorkbenchCommandDescriptor> EnumerateSurface(
		EWorkbenchCommandSurface surface) const;
	[[nodiscard]] WorkbenchCommandExecutionResult Execute(std::string_view commandId,
		const WorkbenchContextKeySnapshot& context) const noexcept;
	//! Executes with an arguments payload. A command bound to the argument-less
	//! executor ignores the payload, exactly as a VS Code handler that declares
	//! no parameters ignores the arguments it is passed; the two-argument form
	//! above is this one with an empty payload.
	[[nodiscard]] WorkbenchCommandExecutionResult Execute(std::string_view commandId,
		const WorkbenchContextKeySnapshot& context, std::string_view argumentsJson) const noexcept;
	//! Runs `workbench.actions.updateIndicator` by executing whichever command
	//! `ResolveUpdateIndicatorCommand` selects for the snapshot. A non-actionable
	//! state is `NotApplicable`, not `Failed`: the button should not have been
	//! visible, and pressing a stale one must not fabricate an update action.
	[[nodiscard]] WorkbenchCommandExecutionResult ExecuteUpdateIndicator(
		const WorkbenchContextKeySnapshot& context) const noexcept;
	[[nodiscard]] std::uint64_t Revision() const noexcept;

	[[nodiscard]] static bool IsValidCommandId(std::string_view value) noexcept;

private:
	struct Entry {
		WorkbenchCommandDescriptor descriptor;
		WorkbenchCommandExecutor executor;
		//! At most one of the two is ever bound. A command with neither is the
		//! sanctioned typed `Unsupported` boundary.
		WorkbenchCommandArgumentExecutor argumentExecutor;
	};
	//! Registers a whole batch or none of it. A conflict against an already
	//! registered command, or between two members of the batch, leaves the
	//! registry untouched.
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterAtomicBatch(std::vector<Entry> batch);
	mutable std::mutex m_mutex;
	std::uint64_t m_revision{};
	std::map<std::string, Entry, std::less<>> m_entries;
};

} // namespace workbench::commands
