/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"

#include <algorithm>
#include <utility>

namespace workbench::commands {
namespace {

const WorkbenchCommandOwner kBuiltinOwner{ "sakura.builtin", 1 };
//! Integer mirror of generated F_TOGGLE_LEFT_EXPLORER. Keep source/high-bit flags out of this pure boundary.
constexpr std::int32_t kLegacyToggleLeftExplorerFunctionCode = 30991;

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
			{ EWorkbenchCommandSurface::Menu, "workbench.action.toggleSidebarVisibility.menu", std::nullopt },
			{ EWorkbenchCommandSurface::ActivityBar, "workbench.action.toggleSidebarVisibility.activity", std::nullopt },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.action.toggleSidebarVisibility.key", std::nullopt },
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
			{ EWorkbenchCommandSurface::Menu, "workbench.view.explorer.menu", kLegacyToggleLeftExplorerFunctionCode },
			{ EWorkbenchCommandSurface::ActivityBar, "workbench.view.explorer.activity", std::nullopt },
			{ EWorkbenchCommandSurface::Keybinding, "workbench.view.explorer.key", kLegacyToggleLeftExplorerFunctionCode },
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
	const auto toggle = MakeToggleSidebarDescriptor();
	const auto explorer = MakeExplorerDescriptor();
	const auto problems = MakeProblemsDescriptor();
	const auto output = MakeOutputDescriptor();
	const auto showCommands = MakeShowCommandsDescriptor();
	const auto openSettings = MakeOpenSettingsDescriptor();
	const auto extensions = MakeExtensionsDescriptor();
	const auto openGlobalKeybindings = MakeOpenGlobalKeybindingsDescriptor();
	const auto colorTheme = MakeColorThemeDescriptor();
	const auto fileIconTheme = MakeFileIconThemeDescriptor();
	std::lock_guard lock(m_mutex);
	const auto conflicts = [&](const WorkbenchCommandDescriptor& requested) {
		for (const auto& [id, entry] : m_entries) {
			(void)id;
			for (const auto& registered : entry.descriptor.surfaceBindings) {
				for (const auto& binding : requested.surfaceBindings) {
					if (registered.surface == binding.surface && registered.slotId == binding.slotId) return true;
				}
			}
		}
		return false;
	};
	if (m_entries.contains(toggle.id) || m_entries.contains(explorer.id)
		|| m_entries.contains(problems.id) || m_entries.contains(output.id) || m_entries.contains(colorTheme.id)
		|| m_entries.contains(fileIconTheme.id) || m_entries.contains(showCommands.id)
		|| m_entries.contains(openSettings.id) || m_entries.contains(extensions.id)
		|| m_entries.contains(openGlobalKeybindings.id)
		|| conflicts(toggle) || conflicts(explorer) || conflicts(problems) || conflicts(output)
		|| conflicts(colorTheme) || conflicts(fileIconTheme) || conflicts(showCommands)
		|| conflicts(openSettings) || conflicts(extensions) || conflicts(openGlobalKeybindings)) {
		return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
	}
	m_entries.emplace(toggle.id, Entry{ toggle, std::move(executors.toggleSidebarVisibility) });
	m_entries.emplace(explorer.id, Entry{ explorer, std::move(executors.showExplorer) });
	m_entries.emplace(problems.id, Entry{ problems, std::move(executors.showProblems) });
	m_entries.emplace(output.id, Entry{ output, std::move(executors.toggleOutput) });
	m_entries.emplace(showCommands.id, Entry{ showCommands, std::move(executors.showCommands) });
	m_entries.emplace(openSettings.id, Entry{ openSettings, std::move(executors.openSettings) });
	m_entries.emplace(extensions.id, Entry{ extensions, std::move(executors.showExtensions) });
	m_entries.emplace(openGlobalKeybindings.id, Entry{ openGlobalKeybindings, std::move(executors.openGlobalKeybindings) });
	m_entries.emplace(colorTheme.id, Entry{ colorTheme, std::move(executors.selectTheme) });
	m_entries.emplace(fileIconTheme.id, Entry{ fileIconTheme, std::move(executors.selectFileIconTheme) });
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
