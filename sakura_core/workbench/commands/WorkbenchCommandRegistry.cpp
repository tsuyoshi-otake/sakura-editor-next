/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/commands/ExplorerCommandIds.h"

#include <algorithm>
#include <array>
#include <utility>

namespace workbench::commands {
namespace {

const WorkbenchCommandOwner kBuiltinOwner{ "sakura.builtin", 1 };
//! Integer mirror of generated F_TOGGLE_LEFT_EXPLORER. Keep source/high-bit flags out of this pure boundary.
constexpr std::int32_t kLegacyToggleLeftExplorerFunctionCode = 30991;
//! Integer mirror of generated F_OPEN_WORKSPACE_FOLDER. Keep this pure boundary independent of generated headers.
constexpr std::int32_t kLegacyOpenWorkspaceFolderFunctionCode = 30997;
//! Integer mirrors of the Funccode_x.hsrc File command allocations.
constexpr std::int32_t kLegacyNewUntitledFileFunctionCode = 30101;
constexpr std::int32_t kLegacyOpenFileFunctionCode = 30102;
constexpr std::int32_t kLegacySaveFunctionCode = 30103;
constexpr std::int32_t kLegacySaveAsFunctionCode = 30104;
constexpr std::int32_t kLegacyNewWindowFunctionCode = 30110;
constexpr std::int32_t kLegacySaveAllFunctionCode = 30120;
constexpr std::int32_t kLegacyQuitFunctionCode = 30195;
constexpr std::int32_t kLegacyCloseWindowFunctionCode = 31320;
constexpr std::int32_t kLegacyOpenRecentFunctionCode = 29007;
constexpr std::int32_t kLegacyOpenWorkspaceFunctionCode = 31002;
constexpr std::int32_t kLegacyAddRootFolderFunctionCode = 31003;
constexpr std::int32_t kLegacySaveWorkspaceAsFunctionCode = 31004;
constexpr std::int32_t kLegacyDuplicateWorkspaceFunctionCode = 31005;
constexpr std::int32_t kLegacyCloseFolderFunctionCode = 31006;
constexpr std::int32_t kLegacyCloseActiveEditorFunctionCode = 31007;
constexpr std::int32_t kLegacyClearRecentFilesFunctionCode = 31008;

bool IsValidBinding(const WorkbenchCommandSurfaceBinding& binding) noexcept
{
	return !binding.slotId.empty() && binding.slotId.size() <= kMaxWorkbenchCommandIdLength
		&& binding.slotId.find('\0') == std::string::npos;
}

bool IsValidDescriptor(const WorkbenchCommandDescriptor& descriptor) noexcept
{
	if (!WorkbenchCommandRegistry::IsValidCommandId(descriptor.id) || descriptor.title.empty()
		|| descriptor.title.size() > kMaxWorkbenchCommandIdLength || !descriptor.owner.IsValid()
		|| descriptor.whenClause.size() > kMaxWorkbenchCommandExpressionLength
		|| descriptor.enablementClause.size() > kMaxWorkbenchCommandExpressionLength
		|| descriptor.surfaceBindings.size() > 32) {
		return false;
	}
	for (std::size_t index = 0; index < descriptor.surfaceBindings.size(); ++index) {
		if (!IsValidBinding(descriptor.surfaceBindings[index])) {
			return false;
		}
		for (std::size_t next = index + 1; next < descriptor.surfaceBindings.size(); ++next) {
			if (descriptor.surfaceBindings[index].surface == descriptor.surfaceBindings[next].surface
				&& descriptor.surfaceBindings[index].slotId == descriptor.surfaceBindings[next].slotId) {
				return false;
			}
		}
	}
	return true;
}

WorkbenchCommandDescriptor MakeToggleSidebarDescriptor()
{
	return {
		"workbench.action.toggleSidebarVisibility",
		"Toggle Primary Side Bar Visibility",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Layout,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.toggleSidebarVisibility.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.action.toggleSidebarVisibility.menu", kLegacyToggleLeftExplorerFunctionCode },
			{ EWorkbenchCommandSurface::ActivityBar, "workbench.action.toggleSidebarVisibility.activity", std::nullopt },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.action.toggleSidebarVisibility.key", kLegacyToggleLeftExplorerFunctionCode },
		},
	};
}

WorkbenchCommandDescriptor MakeExplorerDescriptor()
{
	return {
		"workbench.view.explorer",
		"Show Explorer",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Layout,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.view.explorer.palette", std::nullopt },
			{ EWorkbenchCommandSurface::ActivityBar, "workbench.view.explorer.activity", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeProblemsDescriptor()
{
	return {
		"workbench.actions.view.problems",
		"Problems",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Layout,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.actions.view.problems.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.actions.view.problems.menu", std::nullopt },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.actions.view.problems.key", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeOutputDescriptor()
{
	return {
		"workbench.action.output.toggleOutput",
		"Toggle Output",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Layout,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.output.toggleOutput.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.action.output.toggleOutput.menu", std::nullopt },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.action.output.toggleOutput.key", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeShowCommandsDescriptor()
{
	return {
		"workbench.action.showCommands",
		"Command Palette...",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.commandPalette", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeOpenSettingsDescriptor()
{
	return {
		"workbench.action.openSettings",
		"Settings",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::LegacyNative,
		{
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.settings", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeOpenFolderDescriptor()
{
	return {
		"workbench.action.files.openFolder",
		"Open Folder...",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.files.openFolder.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.action.files.openFolder.menu", kLegacyOpenWorkspaceFolderFunctionCode },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.action.files.openFolder.key", kLegacyOpenWorkspaceFolderFunctionCode },
		},
	};
}

WorkbenchCommandDescriptor MakeFileCommandDescriptor(
	std::string id, std::string title, std::string whenClause, std::string enablementClause,
	std::int32_t legacyFunctionCode)
{
	const std::string slotBase = id;
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		std::move(whenClause),
		std::move(enablementClause),
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, slotBase + ".palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, slotBase + ".menu", legacyFunctionCode },
			{ EWorkbenchCommandSurface::Keybinding, slotBase + ".key", legacyFunctionCode },
		},
	};
}

WorkbenchCommandDescriptor MakeNewUntitledFileDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.files.newUntitledFile", "New Text File",
		"workbenchReady", "workbenchReady", kLegacyNewUntitledFileFunctionCode);
}

WorkbenchCommandDescriptor MakeNewWindowDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.newWindow", "New Window",
		"workbenchReady", "workbenchReady", kLegacyNewWindowFunctionCode);
}

WorkbenchCommandDescriptor MakeOpenFileDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.files.openFile", "Open File...",
		"workbenchReady", "workbenchReady", kLegacyOpenFileFunctionCode);
}

WorkbenchCommandDescriptor MakeOpenWorkspaceDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.openWorkspace", "Open Workspace from File...",
		"workbenchReady", "workbenchReady", kLegacyOpenWorkspaceFunctionCode);
}

WorkbenchCommandDescriptor MakeOpenRecentDescriptor()
{
	// Empty history is an explicit cancellation/empty terminal of the owning
	// command. Keep the command enabled while the workbench is ready so that
	// keyboard and menu execution reach that terminal.
	return MakeFileCommandDescriptor("workbench.action.openRecent", "Open Recent",
		"workbenchReady", "workbenchReady", kLegacyOpenRecentFunctionCode);
}

WorkbenchCommandDescriptor MakeClearRecentFilesDescriptor()
{
	// Upstream contributes this as a static Open Recent entry that stays
	// enabled on an empty history, where it is a no-op success rather than a
	// disabled command.
	return MakeFileCommandDescriptor("workbench.action.clearRecentFiles", "Clear Recently Opened...",
		"workbenchReady", "workbenchReady", kLegacyClearRecentFilesFunctionCode);
}

WorkbenchCommandDescriptor MakeAddRootFolderDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.addRootFolder", "Add Folder to Workspace...",
		"workbenchReady", "workbenchReady", kLegacyAddRootFolderFunctionCode);
}

WorkbenchCommandDescriptor MakeSaveWorkspaceAsDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.saveWorkspaceAs", "Save Workspace As...",
		"workbenchReady", "workbenchReady", kLegacySaveWorkspaceAsFunctionCode);
}

WorkbenchCommandDescriptor MakeDuplicateWorkspaceDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.duplicateWorkspaceInNewWindow",
		"Duplicate Workspace in New Window", "workbenchReady", "workbenchReady",
		kLegacyDuplicateWorkspaceFunctionCode);
}

WorkbenchCommandDescriptor MakeSaveDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.files.save", "Save",
		"workbenchReady", "editorHasActiveEditor", kLegacySaveFunctionCode);
}

WorkbenchCommandDescriptor MakeSaveAsDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.files.saveAs", "Save As...",
		"workbenchReady", "editorHasActiveEditor", kLegacySaveAsFunctionCode);
}

WorkbenchCommandDescriptor MakeSaveAllDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.files.saveAll", "Save All",
		"workbenchReady", "editorHasActiveEditor", kLegacySaveAllFunctionCode);
}

WorkbenchCommandDescriptor MakeCloseActiveEditorDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.closeActiveEditor", "Close Active Editor",
		"workbenchReady", "editorHasActiveEditor", kLegacyCloseActiveEditorFunctionCode);
}

WorkbenchCommandDescriptor MakeCloseFolderDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.closeFolder", "Close Folder",
		"workbenchReady", "workbenchState == 'folder' || workbenchState == 'workspace'",
		kLegacyCloseFolderFunctionCode);
}

WorkbenchCommandDescriptor MakeCloseWindowDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.closeWindow", "Close Window",
		"workbenchReady", "workbenchReady", kLegacyCloseWindowFunctionCode);
}

WorkbenchCommandDescriptor MakeQuitDescriptor()
{
	return MakeFileCommandDescriptor("workbench.action.quit", "Quit",
		"workbenchReady", "workbenchReady", kLegacyQuitFunctionCode);
}

WorkbenchCommandDescriptor MakeOpenGlobalKeybindingsDescriptor()
{
	return {
		"workbench.action.openGlobalKeybindings",
		"Keyboard Shortcuts",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::LegacyNative,
		{
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.keybindings", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeColorThemeDescriptor()
{
	return {
		"workbench.action.selectTheme",
		"Preferences: Color Theme",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::LegacyNative,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.selectTheme.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.colorTheme", std::nullopt },
		},
	};
}

WorkbenchCommandDescriptor MakeShowNotificationsDescriptor()
{
	return {
		"notifications.showList", "Show Notifications", kBuiltinOwner,
		"workbenchReady", "workbenchReady", EWorkbenchCommandExecutorTarget::Editor, {}
	};
}

WorkbenchCommandDescriptor MakeHideNotificationsDescriptor()
{
	return {
		"notifications.hideList", "Hide Notifications", kBuiltinOwner,
		"workbenchReady", "workbenchReady", EWorkbenchCommandExecutorTarget::Editor, {}
	};
}

WorkbenchCommandDescriptor MakeToggleStatusbarDescriptor()
{
	return {
		"workbench.action.toggleStatusbarVisibility", "Toggle Status Bar Visibility", kBuiltinOwner,
		"workbenchReady", "workbenchReady", EWorkbenchCommandExecutorTarget::LegacyNative,
		{
			{ EWorkbenchCommandSurface::CommandPalette,
				"workbench.action.toggleStatusbarVisibility.palette", std::nullopt },
		}
	};
}

WorkbenchCommandDescriptor MakeMarkdownPreviewDescriptor(
	std::string id, std::string title, bool hasDefaultKeybinding)
{
	const auto slotBase = id;
	std::vector<WorkbenchCommandSurfaceBinding> bindings{
		{ EWorkbenchCommandSurface::CommandPalette, slotBase + ".palette", std::nullopt },
	};
	if (hasDefaultKeybinding) {
		bindings.push_back({ EWorkbenchCommandSurface::Keybinding, slotBase + ".key", std::nullopt });
	}
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		"workbenchReady",
		"editorHasActiveEditor",
		EWorkbenchCommandExecutorTarget::Editor,
		std::move(bindings),
	};
}

//! Upstream gates all four branch commands on `gitOpenRepositoryCount != 0`
//! (`extensions/git/package.json`, `menus.commandPalette`). The two remaining
//! conjuncts there, `config.git.enabled` and `!git.missing`, describe the
//! extension's own enablement and the absence of a git binary; neither is a
//! context key this native provider publishes yet, so the clause carries only
//! the conjunct backed by real state rather than a hard-coded `true`.
constexpr std::string_view kGitRepositoryWhenClause = "gitOpenRepositoryCount != 0";

//!
//! @brief The added conjunct for the selected-range commands.
//!
//! Upstream's Command Palette clause for `git.stageSelectedRanges` and
//! `git.unstageSelectedRanges` is the repository clause **and**
//! `isInDiffEditor`. That conjunct is not decoration: these commands act on the
//! selection of an open comparison, so listing them while no comparison is open
//! would offer an action with no operand.
//!
constexpr std::string_view kGitDiffEditorWhenClause = "gitOpenRepositoryCount != 0 && isInDiffEditor";

//!
//! @brief One of VS Code's API commands, which are callable but never listed.
//!
//! `ApiCommand` entries go into `CommandsRegistry` only; they have no
//! `MenuRegistry` contribution, no category, and no keybinding, so
//! `surfaceBindings` is deliberately empty. `IsValidDescriptor` accepts that -
//! a command with no surface is exactly what an API command is, and giving one
//! a Command Palette slot would put an argument-taking internal command in a
//! list where the user could invoke it with no arguments.
//!
//! The `when`/enablement clause is `workbenchReady` alone. Upstream imposes no
//! context condition on these at all; this native registry has no
//! unconditional clause, and `workbenchReady` is the weakest real one - it says
//! the workbench exists, which is the actual prerequisite for opening anything.
//!
WorkbenchCommandDescriptor MakeApiCommandDescriptor(std::string id, std::string title)
{
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{},
	};
}

WorkbenchCommandDescriptor MakeGitDescriptor(std::string id, std::string title)
{
	const auto slot = id + ".palette";
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		std::string(kGitRepositoryWhenClause),
		std::string(kGitRepositoryWhenClause),
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, slot, std::nullopt },
		},
	};
}

//!
//! @brief `git.init` and `git.clone`'s descriptor: available with no repository open.
//!
//! Every other Git command in this batch requires an open repository
//! (`kGitRepositoryWhenClause`), which is exactly backwards for the two
//! commands whose entire purpose is to create that repository state. Upstream
//! gates `git.init` on `config.git.enabled && !git.missing &&
//! !operationInProgress` and `git.clone` on `config.git.enabled &&
//! !git.missing && remoteName != 'codespaces'`
//! (`extensions/git/package.json`, `menus.commandPalette`) - none of those
//! conjuncts are context keys this native provider publishes yet, so, like
//! `MakeApiCommandDescriptor`, the clause carries `workbenchReady` alone
//! rather than a fabricated conjunct. Unlike an API command, `git.init` and
//! `git.clone` are real Command Palette entries upstream, so they keep the
//! Command Palette surface binding an API command deliberately omits.
//!
WorkbenchCommandDescriptor MakeGitAlwaysAvailableDescriptor(std::string id, std::string title)
{
	const auto slot = id + ".palette";
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, slot, std::nullopt },
		},
	};
}

//! `MakeGitDescriptor` with the diff-editor conjunct added to both clauses.
WorkbenchCommandDescriptor MakeGitDiffEditorDescriptor(std::string id, std::string title)
{
	const auto slot = id + ".palette";
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		std::string(kGitDiffEditorWhenClause),
		std::string(kGitDiffEditorWhenClause),
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, slot, std::nullopt },
		},
	};
}

//!
//! @brief Which surfaces one Explorer file-operation command carries.
//!
//! Upstream splits these across three shapes (`fileActions.contribution.ts`):
//! `explorer.newFile`/`explorer.newFolder` are Explorer-context-menu commands
//! with no keybinding and no Command Palette entry; `renameFile` (F2),
//! `moveFileToTrash` (Delete), and `deleteFile` (Shift+Delete) add a
//! keybinding but still no palette entry, because with no Explorer selection
//! they have no operand; `copyFilePath` (Shift+Alt+C), `copyRelativeFilePath`
//! (Ctrl+K Ctrl+Shift+C), and `revealFileInOS` (Shift+Alt+R) also appear in
//! the Command Palette, where upstream retitles them against the active file
//! ("Copy Path of Active File"). This registry carries one title per command,
//! so the palette slot reuses the context-menu title; the palette-variant
//! retitle is a recorded simplification in `CLAUDE.md`, not an accident.
//!
enum class EExplorerCommandSurfaces : std::uint8_t {
	MenuOnly,
	MenuAndKey,
	MenuKeyAndPalette,
};

//!
//! @brief One of the Explorer's resource-scoped file-operation commands.
//!
//! Upstream gates these on Files Explorer context keys -
//! `filesExplorerFocus && foldersViewVisible && !inputFocus`, resource
//! writability, and root/trash-capability negations
//! (`fileActions.contribution.ts`). None of those conjuncts are context keys
//! this native provider publishes yet, so, exactly as
//! `MakeGitAlwaysAvailableDescriptor` documents for `git.init`/`git.clone`,
//! the clause carries `workbenchReady` alone rather than a fabricated
//! conjunct.
//!
WorkbenchCommandDescriptor MakeExplorerResourceDescriptor(
	std::string id, std::string title, EExplorerCommandSurfaces surfaces)
{
	std::vector<WorkbenchCommandSurfaceBinding> bindings;
	bindings.push_back({ EWorkbenchCommandSurface::Menu, id + ".menu", std::nullopt });
	if (surfaces != EExplorerCommandSurfaces::MenuOnly) {
		bindings.push_back({ EWorkbenchCommandSurface::Keybinding, id + ".key", std::nullopt });
	}
	if (surfaces == EExplorerCommandSurfaces::MenuKeyAndPalette) {
		bindings.push_back({ EWorkbenchCommandSurface::CommandPalette, id + ".palette", std::nullopt });
	}
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		std::move(bindings),
	};
}

//! One of the four commands contributed to the Files Explorer ViewTitle.
//! Unlike resource commands, these actions have no operand in their command
//! payload; the view resolves its current selection/root when invoked.
WorkbenchCommandDescriptor MakeExplorerViewTitleDescriptor(
	std::string id, std::string title)
{
	const auto slot = id + ".viewTitle";
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::ViewTitle, slot, std::nullopt },
		},
	};
}

//! `updateState == '<state>'`, the exact shape upstream's `when` clauses take.
//! The state strings carry upstream's spaces; quoting is what makes
//! `'checking for updates'` one operand rather than three tokens.
std::string UpdateStateWhenClause(std::string_view state)
{
	return "updateState == '" + std::string(state) + "'";
}

//! One of upstream's Command Palette update actions
//! (`update.contribution.ts`). Each is `f1: true` with a precondition on one
//! `updateState` value, so it is a palette entry that simply is not listed in
//! any other state.
WorkbenchCommandDescriptor MakeUpdatePaletteDescriptor(
	std::string id, std::string title, std::string_view state)
{
	const auto slot = id + ".palette";
	auto clause = UpdateStateWhenClause(state);
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		clause,
		clause,
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::CommandPalette, slot, std::nullopt },
		},
	};
}

//!
//! @brief One entry of the gear menu's `7_update` group.
//!
//! Upstream contributes these to `MenuId.GlobalActivity` in group `7_update`,
//! each gated on a single `updateState`. The menu slot names the group so the
//! native gear menu can order them the way upstream orders them, instead of
//! inferring an order from the command IDs.
//!
//! `actionable` distinguishes upstream's two kinds of entry. An actionable one
//! carries the same enablement as its `when` clause; a progress one carries
//! upstream's `precondition: false`, so it is listed and greyed out - the entry
//! is how the user learns the update is downloading, and hiding it would just
//! make the gear menu look idle while it is not.
//!
WorkbenchCommandDescriptor MakeUpdateMenuDescriptor(
	std::string id, std::string title, std::string_view state, bool actionable)
{
	const auto slot = "workbench.manage.7_update." + id;
	auto clause = UpdateStateWhenClause(state);
	auto enablement = actionable ? clause : std::string("false");
	return {
		std::move(id),
		std::move(title),
		kBuiltinOwner,
		std::move(clause),
		std::move(enablement),
		EWorkbenchCommandExecutorTarget::Editor,
		{
			{ EWorkbenchCommandSurface::Menu, slot, std::nullopt },
		},
	};
}

} // namespace

std::uint32_t ResolveBuiltinWorkbenchCommandTitleResourceId(std::string_view commandId) noexcept
{
	// This is presentation metadata only: the registry never loads language resources,
	// so model-only callers still observe stable identifiers and fallback titles.
	static constexpr std::pair<std::string_view, std::uint32_t> kTitles[] = {
		{"workbench.action.toggleSidebarVisibility", STR_WORKBENCH_COMMAND_TOGGLE_SIDEBAR},
		{"workbench.view.explorer", STR_WORKBENCH_COMMAND_EXPLORER},
		{"workbench.actions.view.problems", STR_WORKBENCH_COMMAND_PROBLEMS},
		{"workbench.action.output.toggleOutput", STR_WORKBENCH_COMMAND_OUTPUT},
		{"workbench.action.showCommands", STR_WORKBENCH_COMMAND_SHOW_COMMANDS},
		{"workbench.action.openSettings", STR_WORKBENCH_COMMAND_OPEN_SETTINGS},
		{"workbench.action.files.openFolder", STR_WORKBENCH_COMMAND_OPEN_FOLDER},
		{"workbench.action.openGlobalKeybindings", STR_WORKBENCH_COMMAND_OPEN_GLOBAL_KEYBINDINGS},
		{"workbench.action.selectTheme", STR_WORKBENCH_COMMAND_COLOR_THEME},
		{"workbench.action.files.newUntitledFile", STR_WORKBENCH_COMMAND_NEW_FILE},
		{"workbench.action.newWindow", STR_WORKBENCH_COMMAND_NEW_WINDOW},
		{"workbench.action.files.openFile", STR_WORKBENCH_COMMAND_OPEN_FILE},
		{"workbench.action.openWorkspace", STR_WORKBENCH_COMMAND_OPEN_WORKSPACE},
		{"workbench.action.openRecent", STR_WORKBENCH_COMMAND_OPEN_RECENT},
		{"workbench.action.clearRecentFiles", STR_WORKBENCH_COMMAND_CLEAR_RECENT},
		{"workbench.action.addRootFolder", STR_WORKBENCH_COMMAND_ADD_ROOT_FOLDER},
		{"workbench.action.saveWorkspaceAs", STR_WORKBENCH_COMMAND_SAVE_WORKSPACE_AS},
		{"workbench.action.duplicateWorkspaceInNewWindow", STR_WORKBENCH_COMMAND_DUPLICATE_WORKSPACE},
		{"workbench.action.files.save", STR_WORKBENCH_COMMAND_SAVE},
		{"workbench.action.files.saveAs", STR_WORKBENCH_COMMAND_SAVE_AS},
		{"workbench.action.files.saveAll", STR_WORKBENCH_COMMAND_SAVE_ALL},
		{"workbench.action.closeActiveEditor", STR_WORKBENCH_COMMAND_CLOSE_EDITOR},
		{"workbench.action.closeFolder", STR_WORKBENCH_COMMAND_CLOSE_FOLDER},
		{"workbench.action.closeWindow", STR_WORKBENCH_COMMAND_CLOSE_WINDOW},
		{"workbench.action.quit", STR_WORKBENCH_COMMAND_QUIT},
		{"notifications.showList", STR_WORKBENCH_COMMAND_SHOW_NOTIFICATIONS},
		{"notifications.hideList", STR_WORKBENCH_COMMAND_HIDE_NOTIFICATIONS},
		{"workbench.action.toggleStatusbarVisibility", STR_WORKBENCH_COMMAND_TOGGLE_STATUSBAR},
		{"markdown.showPreview", STR_WORKBENCH_COMMAND_MARKDOWN_PREVIEW},
		{"markdown.showPreviewToSide", STR_WORKBENCH_COMMAND_MARKDOWN_PREVIEW_SIDE},
		{"markdown.showLockedPreviewToSide", STR_WORKBENCH_COMMAND_MARKDOWN_LOCKED_SIDE},
		{"markdown.showSource", STR_WORKBENCH_COMMAND_MARKDOWN_SOURCE},
		{"markdown.showPreviewSecuritySelector", STR_WORKBENCH_COMMAND_MARKDOWN_SECURITY},
		{"markdown.preview.refresh", STR_WORKBENCH_COMMAND_MARKDOWN_REFRESH},
		{"markdown.preview.toggleLock", STR_WORKBENCH_COMMAND_MARKDOWN_TOGGLE_LOCK},
		{"markdown.reopenAsPreview", STR_WORKBENCH_COMMAND_MARKDOWN_REOPEN_PREVIEW},
		{"markdown.reopenAsSource", STR_WORKBENCH_COMMAND_MARKDOWN_REOPEN_SOURCE},
		{"markdown.togglePreview", STR_WORKBENCH_COMMAND_MARKDOWN_TOGGLE},
		{"git.init", STR_WORKBENCH_COMMAND_GIT_INIT}, {"git.clone", STR_WORKBENCH_COMMAND_GIT_CLONE},
		{"git.cloneRecursive", STR_WORKBENCH_COMMAND_GIT_CLONE_RECURSIVE}, {"git.checkout", STR_WORKBENCH_COMMAND_GIT_CHECKOUT},
		{"git.checkoutDetached", STR_WORKBENCH_COMMAND_GIT_CHECKOUT_DETACHED}, {"git.branch", STR_WORKBENCH_COMMAND_GIT_BRANCH},
		{"git.branchFrom", STR_WORKBENCH_COMMAND_GIT_BRANCH_FROM}, {"git.openChange", STR_WORKBENCH_COMMAND_GIT_OPEN_CHANGE},
		{"git.stage", STR_WORKBENCH_COMMAND_GIT_STAGE}, {"git.stageAll", STR_WORKBENCH_COMMAND_GIT_STAGE_ALL},
		{"git.unstage", STR_WORKBENCH_COMMAND_GIT_UNSTAGE}, {"git.unstageAll", STR_WORKBENCH_COMMAND_GIT_UNSTAGE_ALL},
		{"git.clean", STR_WORKBENCH_COMMAND_GIT_CLEAN}, {"git.cleanAll", STR_WORKBENCH_COMMAND_GIT_CLEAN_ALL},
		{"git.commit", STR_WORKBENCH_COMMAND_GIT_COMMIT}, {"git.commitAmend", STR_WORKBENCH_COMMAND_GIT_COMMIT_AMEND},
		{"git.undoCommit", STR_WORKBENCH_COMMAND_GIT_UNDO_COMMIT}, {"git.stageSelectedRanges", STR_WORKBENCH_COMMAND_GIT_STAGE_RANGES},
		{"git.unstageSelectedRanges", STR_WORKBENCH_COMMAND_GIT_UNSTAGE_RANGES}, {"git.fetch", STR_WORKBENCH_COMMAND_GIT_FETCH},
		{"git.fetchPrune", STR_WORKBENCH_COMMAND_GIT_FETCH_PRUNE}, {"git.fetchAll", STR_WORKBENCH_COMMAND_GIT_FETCH_ALL},
		{"git.pull", STR_WORKBENCH_COMMAND_GIT_PULL}, {"git.pullRebase", STR_WORKBENCH_COMMAND_GIT_PULL_REBASE},
		{"git.push", STR_WORKBENCH_COMMAND_GIT_PUSH}, {"git.sync", STR_WORKBENCH_COMMAND_GIT_SYNC},
		{"git.syncRebase", STR_WORKBENCH_COMMAND_GIT_SYNC_REBASE}, {"git.publish", STR_WORKBENCH_COMMAND_GIT_PUBLISH},
		{"explorer.newFile", STR_WORKBENCH_COMMAND_EXPLORER_NEW_FILE}, {"explorer.newFolder", STR_WORKBENCH_COMMAND_EXPLORER_NEW_FOLDER},
		{"explorer.refresh", STR_WORKBENCH_COMMAND_EXPLORER_REFRESH}, {"explorer.collapseFolders", STR_WORKBENCH_COMMAND_EXPLORER_COLLAPSE},
		{"renameFile", STR_WORKBENCH_COMMAND_EXPLORER_RENAME}, {"moveFileToTrash", STR_WORKBENCH_COMMAND_EXPLORER_TRASH},
		{"deleteFile", STR_WORKBENCH_COMMAND_EXPLORER_DELETE}, {"copyFilePath", STR_WORKBENCH_COMMAND_EXPLORER_COPY_PATH},
		{"copyRelativeFilePath", STR_WORKBENCH_COMMAND_EXPLORER_COPY_RELATIVE}, {"revealFileInOS", STR_WORKBENCH_COMMAND_EXPLORER_REVEAL},
		{"update.checkForUpdate", STR_WORKBENCH_COMMAND_UPDATE_CHECK}, {"update.downloadUpdate", STR_WORKBENCH_COMMAND_UPDATE_DOWNLOAD},
		{"update.installUpdate", STR_WORKBENCH_COMMAND_UPDATE_INSTALL}, {"update.restartToUpdate", STR_WORKBENCH_COMMAND_UPDATE_RESTART},
		{"update.showUpdateInfo", STR_WORKBENCH_COMMAND_UPDATE_INFO}, {"update.checking", STR_WORKBENCH_COMMAND_UPDATE_CHECKING},
		{"update.downloading", STR_WORKBENCH_COMMAND_UPDATE_DOWNLOADING}, {"update.updating", STR_WORKBENCH_COMMAND_UPDATE_UPDATING},
		{"update.cancelling", STR_WORKBENCH_COMMAND_UPDATE_CANCELLING}, {"update", STR_WORKBENCH_COMMAND_UPDATE_INDICATOR},
	};
	for (const auto& [id, resourceId] : kTitles) if (id == commandId) return resourceId;
	return 0;
}

std::optional<std::string> ResolveUpdateIndicatorCommand(const WorkbenchContextKeySnapshot& context)
{
	const auto found = context.values.find("updateState");
	if (found == context.values.end()) return std::nullopt;
	const auto* state = std::get_if<std::string>(&found->second);
	if (state == nullptr) return std::nullopt;
	// Upstream's `updateTitleBarEntry.ts` maps exactly these three states to
	// exactly these three commands. Every other state - `idle`, `disabled`, the
	// four progress states - has no entry at all, so it resolves to nothing
	// rather than to a nearest-looking action.
	if (*state == "available for download") return std::string("update.downloadNow");
	if (*state == "downloaded") return std::string("update.install");
	if (*state == "ready") return std::string("update.restart");
	return std::nullopt;
}

bool WorkbenchCommandRegistry::IsValidCommandId(std::string_view value) noexcept
{
	return !value.empty() && value.size() <= kMaxWorkbenchCommandIdLength && value.find('\0') == std::string_view::npos;
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::Register(
	WorkbenchCommandDescriptor descriptor, WorkbenchCommandExecutor executor)
{
	if (!IsValidDescriptor(descriptor)) {
		return { EWorkbenchCommandRegistrationStatus::Invalid, Revision() };
	}
	std::lock_guard lock(m_mutex);
	const auto conflicts = [&](const WorkbenchCommandDescriptor& requested) {
		for (const auto& [id, entry] : m_entries) {
			(void)id;
			for (const auto& registered : entry.descriptor.surfaceBindings) {
				for (const auto& binding : requested.surfaceBindings) {
					if (registered.surface == binding.surface && registered.slotId == binding.slotId) return true;
					if (registered.legacyFunctionCode && binding.legacyFunctionCode
						&& *registered.legacyFunctionCode == *binding.legacyFunctionCode) return true;
				}
			}
		}
		return false;
	};
	if (m_entries.contains(descriptor.id) || conflicts(descriptor)) {
		return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
	}
	const std::string commandId = descriptor.id;
	m_entries.emplace(commandId, Entry{ std::move(descriptor), std::move(executor), {} });
	return { EWorkbenchCommandRegistrationStatus::Succeeded, ++m_revision };
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterWithArguments(
	WorkbenchCommandDescriptor descriptor, WorkbenchCommandArgumentExecutor executor)
{
	if (!IsValidDescriptor(descriptor)) {
		return { EWorkbenchCommandRegistrationStatus::Invalid, Revision() };
	}
	std::vector<Entry> batch;
	batch.push_back(Entry{ std::move(descriptor), {}, std::move(executor) });
	return RegisterAtomicBatch(std::move(batch));
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterBuiltinCommands(
	WorkbenchBuiltinCommandExecutors executors)
{
	std::vector<Entry> builtins{
		Entry{ MakeToggleSidebarDescriptor(), std::move(executors.toggleSidebarVisibility), {} },
		Entry{ MakeExplorerDescriptor(), std::move(executors.showExplorer), {} },
		Entry{ MakeProblemsDescriptor(), std::move(executors.showProblems), {} },
		Entry{ MakeOutputDescriptor(), std::move(executors.toggleOutput), {} },
		Entry{ MakeShowCommandsDescriptor(), std::move(executors.showCommands), {} },
		Entry{ MakeOpenSettingsDescriptor(), std::move(executors.openSettings), {} },
		Entry{ MakeOpenFolderDescriptor(), std::move(executors.openFolder), {} },
		Entry{ MakeOpenGlobalKeybindingsDescriptor(), std::move(executors.openGlobalKeybindings), {} },
		Entry{ MakeColorThemeDescriptor(), std::move(executors.selectTheme), {} },
		Entry{ MakeNewUntitledFileDescriptor(), std::move(executors.newUntitledFile), {} },
		Entry{ MakeNewWindowDescriptor(), std::move(executors.newWindow), {} },
		Entry{ MakeOpenFileDescriptor(), std::move(executors.openFile), {} },
		Entry{ MakeOpenWorkspaceDescriptor(), std::move(executors.openWorkspace), {} },
		Entry{ MakeOpenRecentDescriptor(), std::move(executors.openRecent), {} },
		Entry{ MakeClearRecentFilesDescriptor(), std::move(executors.clearRecentFiles), {} },
		Entry{ MakeAddRootFolderDescriptor(), std::move(executors.addRootFolder), {} },
		Entry{ MakeSaveWorkspaceAsDescriptor(), std::move(executors.saveWorkspaceAs), {} },
		Entry{ MakeDuplicateWorkspaceDescriptor(), std::move(executors.duplicateWorkspaceInNewWindow), {} },
		Entry{ MakeSaveDescriptor(), std::move(executors.save), {} },
		Entry{ MakeSaveAsDescriptor(), std::move(executors.saveAs), {} },
		Entry{ MakeSaveAllDescriptor(), std::move(executors.saveAll), {} },
		Entry{ MakeCloseActiveEditorDescriptor(), std::move(executors.closeActiveEditor), {} },
		Entry{ MakeCloseFolderDescriptor(), std::move(executors.closeFolder), {} },
		Entry{ MakeCloseWindowDescriptor(), std::move(executors.closeWindow), {} },
		Entry{ MakeQuitDescriptor(), std::move(executors.quit), {} },
		Entry{ MakeShowNotificationsDescriptor(), std::move(executors.showNotifications), {} },
		Entry{ MakeHideNotificationsDescriptor(), std::move(executors.hideNotifications), {} },
		Entry{ MakeToggleStatusbarDescriptor(), std::move(executors.toggleStatusbarVisibility), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.showPreview", "Markdown: Open Preview", false),
			std::move(executors.markdownShowPreview), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.showPreviewToSide", "Markdown: Open Preview to the Side", true),
			std::move(executors.markdownShowPreviewToSide), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.showLockedPreviewToSide", "Markdown: Open Locked Preview to the Side", false),
			std::move(executors.markdownShowLockedPreviewToSide), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.showSource", "Markdown: Show Source", false),
			std::move(executors.markdownShowSource), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.showPreviewSecuritySelector", "Markdown: Change Preview Security Settings", false),
			std::move(executors.markdownShowPreviewSecuritySelector), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.preview.refresh", "Markdown: Refresh Preview", false),
			std::move(executors.markdownPreviewRefresh), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.preview.toggleLock", "Markdown: Toggle Locked Preview", false),
			std::move(executors.markdownPreviewToggleLock), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.reopenAsPreview", "Markdown: Reopen with Preview", false),
			std::move(executors.markdownReopenAsPreview), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.reopenAsSource", "Markdown: Reopen with Text Editor", false),
			std::move(executors.markdownReopenAsSource), {} },
		Entry{ MakeMarkdownPreviewDescriptor("markdown.togglePreview", "Markdown: Toggle Preview", true),
			std::move(executors.markdownTogglePreview), {} },
		// Titles are upstream's own `ApiCommand` descriptions, verbatim.
		Entry{ MakeApiCommandDescriptor("vscode.diff",
				   "Opens the provided resources in the diff editor to compare their contents."),
			{}, std::move(executors.vscodeDiff) },
		Entry{ MakeApiCommandDescriptor("vscode.open", "Opens the provided resource in the editor."),
			{}, std::move(executors.vscodeOpen) },
		Entry{ MakeApiCommandDescriptor("vscode.openFolder", "Opens a folder as a workspace."),
			{}, std::move(executors.vscodeOpenFolder) },
	};
	return RegisterAtomicBatch(std::move(builtins));
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterGitCommands(
	WorkbenchGitCommandExecutors executors)
{
	// Command IDs, contribution shapes, and enablement follow upstream. The
	// descriptor keeps its English fallback while the composition root resolves
	// the registered title resource through the selected language DLL.
	// (`command.init`, `command.clone`, `command.cloneRecursive`, `command.checkout`,
	// `command.checkoutDetached`, `command.branch`,
	// `command.branchFrom`, `command.openChange`, `command.stage`, `command.stageAll`,
	// `command.unstage`, `command.unstageAll`, `command.clean`,
	// `command.cleanAll`, `command.commit`, `command.commitAmend`,
	// `command.undoCommit`, `command.stageSelectedRanges`,
	// `command.unstageSelectedRanges`, `command.fetch`, `command.fetchPrune`,
	// `command.fetchAll`, `command.pull`, `command.pullRebase`, `command.push`,
	// `command.sync`, `command.syncRebase`, `command.publish`), prefixed with the
	// `Git` category `package.json` declares for every one of them.
	std::vector<Entry> commands{
		Entry{ MakeGitAlwaysAvailableDescriptor("git.init", "Git: Initialize Repository"),
			{}, std::move(executors.init) },
		Entry{ MakeGitAlwaysAvailableDescriptor("git.clone", "Git: Clone"), std::move(executors.clone), {} },
		Entry{ MakeGitAlwaysAvailableDescriptor("git.cloneRecursive", "Git: Clone (Recursive)"),
			std::move(executors.cloneRecursive), {} },
		Entry{ MakeGitDescriptor("git.checkout", "Git: Checkout to..."), std::move(executors.checkout), {} },
		Entry{ MakeGitDescriptor("git.checkoutDetached", "Git: Checkout to (Detached)..."),
			std::move(executors.checkoutDetached), {} },
		Entry{ MakeGitDescriptor("git.branch", "Git: Create Branch..."), std::move(executors.branch), {} },
		Entry{ MakeGitDescriptor("git.branchFrom", "Git: Create Branch From..."), std::move(executors.branchFrom), {} },
		Entry{ MakeGitDescriptor("git.openChange", "Git: Open Changes"), {}, std::move(executors.openChange) },
		Entry{ MakeGitDescriptor("git.stage", "Git: Stage Changes"), {}, std::move(executors.stage) },
		Entry{ MakeGitDescriptor("git.stageAll", "Git: Stage All Changes"), std::move(executors.stageAll), {} },
		Entry{ MakeGitDescriptor("git.unstage", "Git: Unstage Changes"), {}, std::move(executors.unstage) },
		Entry{ MakeGitDescriptor("git.unstageAll", "Git: Unstage All Changes"), std::move(executors.unstageAll), {} },
		Entry{ MakeGitDescriptor("git.clean", "Git: Discard Changes"), {}, std::move(executors.clean) },
		Entry{ MakeGitDescriptor("git.cleanAll", "Git: Discard All Changes"), std::move(executors.cleanAll), {} },
		Entry{ MakeGitDescriptor("git.commit", "Git: Commit"), {}, std::move(executors.commit) },
		Entry{ MakeGitDescriptor("git.commitAmend", "Git: Amend Commit"), std::move(executors.commitAmend), {} },
		Entry{ MakeGitDescriptor("git.undoCommit", "Git: Undo Last Commit"), std::move(executors.undoCommit), {} },
		Entry{ MakeGitDiffEditorDescriptor("git.stageSelectedRanges", "Git: Stage Selected Ranges"),
			std::move(executors.stageSelectedRanges), {} },
		Entry{ MakeGitDiffEditorDescriptor("git.unstageSelectedRanges", "Git: Unstage Selected Ranges"),
			std::move(executors.unstageSelectedRanges), {} },
		Entry{ MakeGitDescriptor("git.fetch", "Git: Fetch"), std::move(executors.fetch), {} },
		Entry{ MakeGitDescriptor("git.fetchPrune", "Git: Fetch (Prune)"), std::move(executors.fetchPrune), {} },
		Entry{ MakeGitDescriptor("git.fetchAll", "Git: Fetch from All Remotes"), std::move(executors.fetchAll), {} },
		Entry{ MakeGitDescriptor("git.pull", "Git: Pull"), std::move(executors.pull), {} },
		Entry{ MakeGitDescriptor("git.pullRebase", "Git: Pull (Rebase)"), std::move(executors.pullRebase), {} },
		Entry{ MakeGitDescriptor("git.push", "Git: Push"), std::move(executors.push), {} },
		Entry{ MakeGitDescriptor("git.sync", "Git: Sync"), std::move(executors.sync), {} },
		Entry{ MakeGitDescriptor("git.syncRebase", "Git: Sync (Rebase)"), std::move(executors.syncRebase), {} },
		Entry{ MakeGitDescriptor("git.publish", "Git: Publish Branch..."), std::move(executors.publish), {} },
		Entry{ MakeGitDescriptor("git.refresh", "Git: Refresh"), std::move(executors.refresh), {} },
		Entry{ MakeGitAlwaysAvailableDescriptor("git.showOutput", "Git: Show Git Output"),
			std::move(executors.showOutput), {} },
		Entry{ MakeGitDescriptor("git.copyCommitId", "Git: Copy Commit Hash"), {},
			std::move(executors.copyCommitId) },
		Entry{ MakeGitDescriptor("git.copyCommitMessage", "Git: Copy Commit Message"), {},
			std::move(executors.copyCommitMessage) },
	};
	return RegisterAtomicBatch(std::move(commands));
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterExplorerCommands(
	WorkbenchExplorerCommandExecutors executors)
{
	// IDs, contribution shapes, and command semantics follow upstream. The native
	// composition resolves each registered title through Sakura's selected language
	// DLL, retaining the descriptor's English text only as a headless fallback.
	// and the labels from `fileActions.ts` (`NEW_FILE_LABEL`, `NEW_FOLDER_LABEL`,
	// `TRIGGER_RENAME_LABEL`, `MOVE_FILE_TO_TRASH_LABEL`), the rename/delete IDs
	// and "Delete Permanently" from `fileActions.contribution.ts`, "Copy Path"/
	// "Copy Relative Path" from `fileConstants.ts`'s commands, and the Windows
	// branch of `REVEAL_IN_OS_LABEL` from the electron-browser contribution.
	// Every entry is resource-scoped, so each binds the argument executor.
	std::vector<Entry> commands{
		Entry{ MakeExplorerViewTitleDescriptor(
				std::string(kCreateFileFromExplorerCommandId), "New File..."),
			std::move(executors.createFileFromExplorer), {} },
		Entry{ MakeExplorerViewTitleDescriptor(
				std::string(kCreateFolderFromExplorerCommandId), "New Folder..."),
			std::move(executors.createFolderFromExplorer), {} },
		Entry{ MakeExplorerViewTitleDescriptor(
				std::string(kRefreshFilesExplorerCommandId), "Refresh Explorer"),
			std::move(executors.refreshFilesExplorer), {} },
		Entry{ MakeExplorerViewTitleDescriptor(
				std::string(kCollapseExplorerFoldersCommandId), "Collapse Folders in Explorer"),
			std::move(executors.collapseExplorerFolders), {} },
		Entry{ MakeExplorerResourceDescriptor("explorer.newFile", "New File...",
				EExplorerCommandSurfaces::MenuOnly),
			{}, std::move(executors.newFile) },
		Entry{ MakeExplorerResourceDescriptor("explorer.newFolder", "New Folder...",
				EExplorerCommandSurfaces::MenuOnly),
			{}, std::move(executors.newFolder) },
		Entry{ MakeExplorerResourceDescriptor("renameFile", "Rename...",
				EExplorerCommandSurfaces::MenuAndKey),
			{}, std::move(executors.renameFile) },
		Entry{ MakeExplorerResourceDescriptor("moveFileToTrash", "Delete",
				EExplorerCommandSurfaces::MenuAndKey),
			{}, std::move(executors.moveFileToTrash) },
		Entry{ MakeExplorerResourceDescriptor("deleteFile", "Delete Permanently",
				EExplorerCommandSurfaces::MenuAndKey),
			{}, std::move(executors.deleteFile) },
		Entry{ MakeExplorerResourceDescriptor("copyFilePath", "Copy Path",
				EExplorerCommandSurfaces::MenuKeyAndPalette),
			{}, std::move(executors.copyFilePath) },
		Entry{ MakeExplorerResourceDescriptor("copyRelativeFilePath", "Copy Relative Path",
				EExplorerCommandSurfaces::MenuKeyAndPalette),
			{}, std::move(executors.copyRelativeFilePath) },
		Entry{ MakeExplorerResourceDescriptor("revealFileInOS", "Reveal in File Explorer",
				EExplorerCommandSurfaces::MenuKeyAndPalette),
			{}, std::move(executors.revealFileInOS) },
	};
	return RegisterAtomicBatch(std::move(commands));
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterUpdateCommands(
	WorkbenchUpdateCommandExecutors executors)
{
	// Command IDs, state gates, and menu shape follow upstream. User-facing titles
	// are selected at render time from the language resource. The trailing `(1)` on
	// three gear entries is upstream's literal text, not a placeholder: it is the
	// badge count VS Code shows beside the gear, spelled out in the menu label.
	std::vector<Entry> commands{
		Entry{ MakeUpdatePaletteDescriptor("update.checkForUpdate", "Check for Updates...", "idle"),
			executors.checkForUpdates, {} },
		Entry{ MakeUpdatePaletteDescriptor("update.downloadUpdate", "Download Update", "available for download"),
			executors.downloadUpdate, {} },
		Entry{ MakeUpdatePaletteDescriptor("update.installUpdate", "Install Update", "downloaded"),
			executors.applyUpdate, {} },
		Entry{ MakeUpdatePaletteDescriptor("update.restartToUpdate", "Restart to Update", "ready"),
			executors.quitAndInstall, {} },
		// Deliberately gated on `workbenchReady` rather than on a state. What this
		// command shows is what the editor currently knows about updating, which
		// is a meaningful answer in every state - including `disabled` and a state
		// that just failed, where it is the only way to read the diagnostic.
		Entry{ WorkbenchCommandDescriptor{
				   "update.showUpdateInfo", "Show Update Info", kBuiltinOwner,
				   "workbenchReady", "workbenchReady", EWorkbenchCommandExecutorTarget::Editor,
				   { { EWorkbenchCommandSurface::CommandPalette, "update.showUpdateInfo.palette", std::nullopt } } },
			std::move(executors.showUpdateInfo), {} },

		Entry{ MakeUpdateMenuDescriptor("update.check", "Check for Updates...", "idle", true),
			std::move(executors.checkForUpdates), {} },
		Entry{ MakeUpdateMenuDescriptor("update.checking", "Checking for Updates...", "checking for updates", false),
			{}, {} },
		Entry{ MakeUpdateMenuDescriptor("update.downloadNow", "Download Update (1)", "available for download", true),
			std::move(executors.downloadUpdate), {} },
		Entry{ MakeUpdateMenuDescriptor("update.downloading", "Downloading Update...", "downloading", false),
			{}, {} },
		Entry{ MakeUpdateMenuDescriptor("update.install", "Install Update... (1)", "downloaded", true),
			std::move(executors.applyUpdate), {} },
		Entry{ MakeUpdateMenuDescriptor("update.updating", "Installing Update...", "updating", false), {}, {} },
		Entry{ MakeUpdateMenuDescriptor("update.cancelling", "Cancelling Update...", "cancelling", false), {}, {} },
		Entry{ MakeUpdateMenuDescriptor("update.restart", "Restart to Update (1)", "ready", true),
			std::move(executors.quitAndInstall), {} },

		// The title-bar entry itself. No executor: `Execute` delegates it to
		// whichever of the three actionable commands the current state selects.
		Entry{ WorkbenchCommandDescriptor{
				   std::string(kUpdateIndicatorCommandId), "Update", kBuiltinOwner,
				   std::string(kUpdateIndicatorWhenClause), std::string(kUpdateIndicatorWhenClause),
				   EWorkbenchCommandExecutorTarget::Editor,
				   { { EWorkbenchCommandSurface::Menu, "workbench.titleBar.update", std::nullopt } } },
			{}, {} },
	};
	return RegisterAtomicBatch(std::move(commands));
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterAtomicBatch(std::vector<Entry> builtins)
{
	// Resource identifiers are presentation metadata for bundled commands. Assign
	// them at this common registration boundary so every built-in family (core,
	// Git, Explorer, and update) participates without making extension commands
	// or the registry itself depend on the currently selected language.
	for (auto& entry : builtins) {
		if (entry.descriptor.titleResourceId == 0) {
			entry.descriptor.titleResourceId =
				ResolveBuiltinWorkbenchCommandTitleResourceId(entry.descriptor.id);
		}
	}
	std::lock_guard lock(m_mutex);
	const auto conflicts = [&](const WorkbenchCommandDescriptor& requested) {
		for (const auto& [id, entry] : m_entries) {
			(void)id;
			for (const auto& registered : entry.descriptor.surfaceBindings) {
				for (const auto& binding : requested.surfaceBindings) {
					if (registered.surface == binding.surface && registered.slotId == binding.slotId) return true;
					if (registered.legacyFunctionCode && binding.legacyFunctionCode
						&& *registered.legacyFunctionCode == *binding.legacyFunctionCode) return true;
				}
			}
		}
		return false;
	};
	for (const auto& entry : builtins) {
		if (m_entries.contains(entry.descriptor.id) || conflicts(entry.descriptor)) {
			return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
		}
	}
	for (std::size_t left = 0; left < builtins.size(); ++left) {
		for (std::size_t right = left + 1; right < builtins.size(); ++right) {
			for (const auto& leftBinding : builtins[left].descriptor.surfaceBindings) {
				for (const auto& rightBinding : builtins[right].descriptor.surfaceBindings) {
					if (leftBinding.surface == rightBinding.surface && leftBinding.slotId == rightBinding.slotId) {
						return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
					}
					if (leftBinding.legacyFunctionCode && rightBinding.legacyFunctionCode
						&& *leftBinding.legacyFunctionCode == *rightBinding.legacyFunctionCode) {
						return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
					}
				}
			}
		}
	}
	for (auto& entry : builtins) {
		const std::string commandId = entry.descriptor.id;
		m_entries.emplace(commandId, std::move(entry));
	}
	return { EWorkbenchCommandRegistrationStatus::Succeeded, ++m_revision };
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::DisposeOwner(const WorkbenchCommandOwner& owner)
{
	if (!owner.IsValid()) {
		return { EWorkbenchCommandRegistrationStatus::Invalid, Revision() };
	}
	std::lock_guard lock(m_mutex);
	bool removed = false;
	for (auto iterator = m_entries.begin(); iterator != m_entries.end();) {
		if (iterator->second.descriptor.owner == owner) {
			iterator = m_entries.erase(iterator);
			removed = true;
		} else {
			++iterator;
		}
	}
	return removed ? WorkbenchCommandRegistrationResult{ EWorkbenchCommandRegistrationStatus::Succeeded, ++m_revision }
		: WorkbenchCommandRegistrationResult{ EWorkbenchCommandRegistrationStatus::NotApplicable, m_revision };
}

std::optional<WorkbenchCommandDescriptor> WorkbenchCommandRegistry::Find(std::string_view commandId) const
{
	std::lock_guard lock(m_mutex);
	const auto found = m_entries.find(commandId);
	return found == m_entries.end() ? std::nullopt : std::optional<WorkbenchCommandDescriptor>(found->second.descriptor);
}

std::optional<ResolvedWorkbenchCommandSurface> WorkbenchCommandRegistry::ResolveSurface(
	EWorkbenchCommandSurface surface, std::string_view slotId) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& [id, entry] : m_entries) {
		for (const auto& binding : entry.descriptor.surfaceBindings) {
			if (binding.surface == surface && binding.slotId == slotId) {
				return ResolvedWorkbenchCommandSurface{ id, binding };
			}
		}
	}
	return std::nullopt;
}

std::optional<std::string> WorkbenchCommandRegistry::ResolveLegacyFunctionCode(std::int32_t functionCode) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& [id, entry] : m_entries) {
		for (const auto& binding : entry.descriptor.surfaceBindings) {
			if (binding.legacyFunctionCode && *binding.legacyFunctionCode == functionCode) {
				return id;
			}
		}
	}
	return std::nullopt;
}

std::vector<WorkbenchCommandDescriptor> WorkbenchCommandRegistry::EnumerateSurface(
	EWorkbenchCommandSurface surface) const
{
	std::lock_guard lock(m_mutex);
	std::vector<WorkbenchCommandDescriptor> descriptors;
	descriptors.reserve(m_entries.size());
	for (const auto& [id, entry] : m_entries) {
		(void)id;
		const auto hasSurface = std::any_of(entry.descriptor.surfaceBindings.begin(),
			entry.descriptor.surfaceBindings.end(), [surface](const WorkbenchCommandSurfaceBinding& binding) {
				return binding.surface == surface;
			});
		if (hasSurface) descriptors.push_back(entry.descriptor);
	}
	return descriptors;
}

WorkbenchCommandExecutionResult WorkbenchCommandRegistry::Execute(std::string_view commandId,
	const WorkbenchContextKeySnapshot& context) const noexcept
{
	return Execute(commandId, context, std::string_view{});
}

WorkbenchCommandExecutionResult WorkbenchCommandRegistry::Execute(std::string_view commandId,
	const WorkbenchContextKeySnapshot& context, std::string_view argumentsJson) const noexcept
{
	Entry entry;
	{
		std::lock_guard lock(m_mutex);
		const auto found = m_entries.find(commandId);
		if (found == m_entries.end()) {
			return { EWorkbenchCommandExecutionStatus::UnknownCommand, "unknown command" };
		}
		entry = found->second;
	}
	try {
		if (!WorkbenchWhenClauseEvaluator::Evaluate(entry.descriptor.whenClause, context)) {
			return { EWorkbenchCommandExecutionStatus::NotApplicable, "when clause did not match" };
		}
		if (!WorkbenchWhenClauseEvaluator::Evaluate(entry.descriptor.enablementClause, context)) {
			return { EWorkbenchCommandExecutionStatus::Disabled, "enablement clause did not match" };
		}
		// The title-bar indicator is a delegating command, exactly as upstream's
		// `updateTitleBarEntry.ts` is: its `run` picks `update.downloadNow`,
		// `update.install`, or `update.restart` from the state it is showing. It
		// therefore has no executor of its own, and resolving the target here
		// rather than in the title bar keeps the button from carrying a second
		// copy of the update state. The target is never the indicator itself, so
		// this cannot recurse.
		if (entry.descriptor.id == kUpdateIndicatorCommandId && !entry.executor && !entry.argumentExecutor) {
			return ExecuteUpdateIndicator(context);
		}
		if (entry.descriptor.executorTarget == EWorkbenchCommandExecutorTarget::None
			|| (!entry.executor && !entry.argumentExecutor)) {
			return { EWorkbenchCommandExecutionStatus::Unsupported, "executor target is not bound" };
		}
		if (entry.argumentExecutor) {
			return entry.argumentExecutor(argumentsJson);
		}
		return entry.executor();
	} catch (...) {
		return { EWorkbenchCommandExecutionStatus::Failed, "executor threw" };
	}
}

WorkbenchCommandExecutionResult WorkbenchCommandRegistry::ExecuteUpdateIndicator(
	const WorkbenchContextKeySnapshot& context) const noexcept
{
	const auto target = ResolveUpdateIndicatorCommand(context);
	if (!target) {
		// The button is only drawn in an actionable state, so arriving here means
		// the state changed between paint and click. Reporting NotApplicable
		// leaves the stale press with no effect rather than picking whichever
		// update action looks closest.
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "update state is not actionable" };
	}
	return Execute(*target, context, std::string_view{});
}

std::uint64_t WorkbenchCommandRegistry::Revision() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_revision;
}

} // namespace workbench::commands
