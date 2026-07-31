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
		|| m_entries.contains(problems.id) || m_entries.contains(output.id)
		|| conflicts(toggle) || conflicts(explorer) || conflicts(problems) || conflicts(output)) {
		return { EWorkbenchCommandRegistrationStatus::Conflict, m_revision };
	}
	m_entries.emplace(toggle.id, Entry{ toggle, std::move(executors.toggleSidebarVisibility) });
	m_entries.emplace(explorer.id, Entry{ explorer, std::move(executors.showExplorer) });
	m_entries.emplace(problems.id, Entry{ problems, std::move(executors.showProblems) });
	m_entries.emplace(output.id, Entry{ output, std::move(executors.toggleOutput) });
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
