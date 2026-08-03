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
	ExtensionHost,
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

//! Optional bindings supplied by the native composition root. Empty executors remain explicitly Unsupported.
struct WorkbenchBuiltinCommandExecutors {
	WorkbenchCommandExecutor showCommands;
	WorkbenchCommandExecutor openSettings;
	WorkbenchCommandExecutor openFolder;
	WorkbenchCommandExecutor showExtensions;
	WorkbenchCommandExecutor openGlobalKeybindings;
	WorkbenchCommandExecutor toggleSidebarVisibility;
	WorkbenchCommandExecutor showExplorer;
	WorkbenchCommandExecutor showProblems;
	WorkbenchCommandExecutor toggleOutput;
	WorkbenchCommandExecutor selectTheme;
	WorkbenchCommandExecutor selectFileIconTheme;
};

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
	[[nodiscard]] WorkbenchCommandRegistrationResult RegisterBuiltinCommands(
		WorkbenchBuiltinCommandExecutors executors = {});
	[[nodiscard]] WorkbenchCommandRegistrationResult DisposeOwner(const WorkbenchCommandOwner& owner);
	[[nodiscard]] std::optional<WorkbenchCommandDescriptor> Find(std::string_view commandId) const;
	[[nodiscard]] std::optional<ResolvedWorkbenchCommandSurface> ResolveSurface(
		EWorkbenchCommandSurface surface, std::string_view slotId) const;
	//! Returns owning descriptor copies in stable command-ID order for one surface.
	//! A command with multiple bindings for the surface appears exactly once.
	[[nodiscard]] std::vector<WorkbenchCommandDescriptor> EnumerateSurface(
		EWorkbenchCommandSurface surface) const;
	[[nodiscard]] WorkbenchCommandExecutionResult Execute(std::string_view commandId,
		const WorkbenchContextKeySnapshot& context) const noexcept;
	[[nodiscard]] std::uint64_t Revision() const noexcept;

	[[nodiscard]] static bool IsValidCommandId(std::string_view value) noexcept;

private:
	struct Entry {
		WorkbenchCommandDescriptor descriptor;
		WorkbenchCommandExecutor executor;
	};
	mutable std::mutex m_mutex;
	std::uint64_t m_revision{};
	std::map<std::string, Entry, std::less<>> m_entries;
};

} // namespace workbench::commands
