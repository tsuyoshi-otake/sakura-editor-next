/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"

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
	return MakeFileCommandDescriptor("workbench.action.closeActiveEditor", "Close Editor",
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
	return MakeFileCommandDescriptor("workbench.action.quit", "Exit",
		"workbenchReady", "workbenchReady", kLegacyQuitFunctionCode);
}

WorkbenchCommandDescriptor MakeExtensionsDescriptor()
{
	return {
		"workbench.view.extensions",
		"Extensions",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::Layout,
		{
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.extensions", std::nullopt },
		},
	};
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

WorkbenchCommandDescriptor MakeFileIconThemeDescriptor()
{
	return {
		"workbench.action.selectIconTheme",
		"Preferences: File Icon Theme",
		kBuiltinOwner,
		"workbenchReady",
		"workbenchReady",
		EWorkbenchCommandExecutorTarget::LegacyNative,
		{
			{ EWorkbenchCommandSurface::CommandPalette, "workbench.action.selectIconTheme.palette", std::nullopt },
			{ EWorkbenchCommandSurface::Menu, "workbench.manage.fileIconTheme", std::nullopt },
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

} // namespace

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
	m_entries.emplace(commandId, Entry{ std::move(descriptor), std::move(executor) });
	return { EWorkbenchCommandRegistrationStatus::Succeeded, ++m_revision };
}

WorkbenchCommandRegistrationResult WorkbenchCommandRegistry::RegisterBuiltinCommands(
	WorkbenchBuiltinCommandExecutors executors)
{
	auto builtins = std::array{
		std::pair{ MakeToggleSidebarDescriptor(), std::move(executors.toggleSidebarVisibility) },
		std::pair{ MakeExplorerDescriptor(), std::move(executors.showExplorer) },
		std::pair{ MakeProblemsDescriptor(), std::move(executors.showProblems) },
		std::pair{ MakeOutputDescriptor(), std::move(executors.toggleOutput) },
		std::pair{ MakeShowCommandsDescriptor(), std::move(executors.showCommands) },
		std::pair{ MakeOpenSettingsDescriptor(), std::move(executors.openSettings) },
		std::pair{ MakeOpenFolderDescriptor(), std::move(executors.openFolder) },
		std::pair{ MakeExtensionsDescriptor(), std::move(executors.showExtensions) },
		std::pair{ MakeOpenGlobalKeybindingsDescriptor(), std::move(executors.openGlobalKeybindings) },
		std::pair{ MakeColorThemeDescriptor(), std::move(executors.selectTheme) },
		std::pair{ MakeFileIconThemeDescriptor(), std::move(executors.selectFileIconTheme) },
		std::pair{ MakeNewUntitledFileDescriptor(), std::move(executors.newUntitledFile) },
		std::pair{ MakeNewWindowDescriptor(), std::move(executors.newWindow) },
		std::pair{ MakeOpenFileDescriptor(), std::move(executors.openFile) },
		std::pair{ MakeOpenWorkspaceDescriptor(), std::move(executors.openWorkspace) },
		std::pair{ MakeOpenRecentDescriptor(), std::move(executors.openRecent) },
		std::pair{ MakeAddRootFolderDescriptor(), std::move(executors.addRootFolder) },
		std::pair{ MakeSaveWorkspaceAsDescriptor(), std::move(executors.saveWorkspaceAs) },
		std::pair{ MakeDuplicateWorkspaceDescriptor(), std::move(executors.duplicateWorkspaceInNewWindow) },
		std::pair{ MakeSaveDescriptor(), std::move(executors.save) },
		std::pair{ MakeSaveAsDescriptor(), std::move(executors.saveAs) },
		std::pair{ MakeSaveAllDescriptor(), std::move(executors.saveAll) },
		std::pair{ MakeCloseActiveEditorDescriptor(), std::move(executors.closeActiveEditor) },
		std::pair{ MakeCloseFolderDescriptor(), std::move(executors.closeFolder) },
		std::pair{ MakeCloseWindowDescriptor(), std::move(executors.closeWindow) },
		std::pair{ MakeQuitDescriptor(), std::move(executors.quit) },
		std::pair{ MakeShowNotificationsDescriptor(), std::move(executors.showNotifications) },
		std::pair{ MakeHideNotificationsDescriptor(), std::move(executors.hideNotifications) },
		std::pair{ MakeToggleStatusbarDescriptor(), std::move(executors.toggleStatusbarVisibility) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.showPreview", "Markdown: Open Preview", false),
			std::move(executors.markdownShowPreview) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.showPreviewToSide", "Markdown: Open Preview to the Side", true),
			std::move(executors.markdownShowPreviewToSide) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.showLockedPreviewToSide", "Markdown: Open Locked Preview to the Side", false),
			std::move(executors.markdownShowLockedPreviewToSide) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.showSource", "Markdown: Show Source", false),
			std::move(executors.markdownShowSource) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.showPreviewSecuritySelector", "Markdown: Change Preview Security Settings", false),
			std::move(executors.markdownShowPreviewSecuritySelector) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.preview.refresh", "Markdown: Refresh Preview", false),
			std::move(executors.markdownPreviewRefresh) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.preview.toggleLock", "Markdown: Toggle Preview Locking", false),
			std::move(executors.markdownPreviewToggleLock) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.reopenAsPreview", "Markdown: Reopen Editor With Preview", false),
			std::move(executors.markdownReopenAsPreview) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.reopenAsSource", "Markdown: Reopen Editor With Text Editor", false),
			std::move(executors.markdownReopenAsSource) },
		std::pair{ MakeMarkdownPreviewDescriptor("markdown.togglePreview", "Markdown: Toggle Preview", true),
			std::move(executors.markdownTogglePreview) },
	};
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
	for (const auto& [descriptor, executor] : builtins) {
		(void)executor;
		if (m_entries.contains(descriptor.id) || conflicts(descriptor)) {
			return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
		}
	}
	for (std::size_t left = 0; left < builtins.size(); ++left) {
		for (std::size_t right = left + 1; right < builtins.size(); ++right) {
			for (const auto& leftBinding : builtins[left].first.surfaceBindings) {
				for (const auto& rightBinding : builtins[right].first.surfaceBindings) {
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
	for (auto& [descriptor, executor] : builtins) {
		const std::string commandId = descriptor.id;
		m_entries.emplace(commandId, Entry{ std::move(descriptor), std::move(executor) });
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
		if (entry.descriptor.executorTarget == EWorkbenchCommandExecutorTarget::None || !entry.executor) {
			return { EWorkbenchCommandExecutionStatus::Unsupported, "executor target is not bound" };
		}
		return entry.executor();
	} catch (...) {
		return { EWorkbenchCommandExecutionStatus::Failed, "executor threw" };
	}
}

std::uint64_t WorkbenchCommandRegistry::Revision() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_revision;
}

} // namespace workbench::commands
