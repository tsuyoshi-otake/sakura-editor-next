/*! @file
 * @brief Pure catalog of accepted workspace task configurations.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace workbench::tasks {

//! The catalog can describe custom tasks but does not supply an execution bridge for them.
enum class ETaskExecutionKind : std::uint8_t {
	Shell,
	Process,
	Custom,
};

//! Bit flags explain why a displayed task must not be offered as runnable yet.
enum class ETaskUnsupportedCapability : std::uint8_t {
	None = 0,
	CustomExecution = 1 << 0,
	Dependencies = 1 << 1,
	Background = 1 << 2,
	ProblemMatcher = 1 << 3,
};

[[nodiscard]] constexpr ETaskUnsupportedCapability operator|(ETaskUnsupportedCapability left, ETaskUnsupportedCapability right) noexcept
{
	return static_cast<ETaskUnsupportedCapability>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool HasUnsupportedCapability(ETaskUnsupportedCapability value, ETaskUnsupportedCapability capability) noexcept
{
	return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(capability)) != 0;
}

struct TaskGroupMetadata final {
	std::wstring id;
	bool isDefault = false;
};

//! Presentation is declarative metadata only; the native terminal UI remains a later phase.
struct TaskPresentationMetadata final {
	std::optional<std::wstring> reveal;
	std::optional<std::wstring> panel;
	std::optional<bool> focus;
	std::optional<bool> clear;
	std::optional<bool> close;
};

//! An immutable value copied from one accepted WorkspaceArtifactDocument.
struct TaskConfigurationDefinition final {
	std::wstring label;
	ETaskExecutionKind executionKind = ETaskExecutionKind::Shell;
	//! The configured task type for Custom tasks; empty for Shell and Process.
	std::optional<std::wstring> customType;
	std::wstring command;
	std::vector<std::wstring> arguments;
	std::optional<std::wstring> workingDirectory;
	std::optional<TaskGroupMetadata> group;
	std::optional<TaskPresentationMetadata> presentation;
	std::vector<std::wstring> dependencies;
	std::optional<std::wstring> dependencyOrder;
	bool isBackground = false;
	std::vector<std::wstring> problemMatchers;
	ETaskUnsupportedCapability unsupportedCapabilities = ETaskUnsupportedCapability::None;
	//! Always populated for an accepted definition; optional only to keep this value type default-constructible while parsing.
	std::optional<platform::uri::Uri> sourceUri;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;

	[[nodiscard]] bool IsRunnable() const noexcept
	{
		return (executionKind == ETaskExecutionKind::Shell || executionKind == ETaskExecutionKind::Process)
			&& !command.empty() && unsupportedCapabilities == ETaskUnsupportedCapability::None;
	}
};

enum class ETaskConfigurationCatalogStatus : std::uint8_t {
	Applied,
	Cleared,
	StaleGeneration,
	StaleRevision,
	Stopped,
	InvalidSnapshot,
	InvalidArtifact,
	InvalidSchema,
	DuplicateLabel,
	MaximumTasksExceeded,
	EntryTooLarge,
};

struct TaskConfigurationCatalogResult final {
	ETaskConfigurationCatalogStatus status = ETaskConfigurationCatalogStatus::InvalidSnapshot;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ETaskConfigurationCatalogStatus::Applied || status == ETaskConfigurationCatalogStatus::Cleared;
	}
};

//! A copied value snapshot. Definitions are label-sorted for stable enumeration.
struct TaskConfigurationCatalogSnapshot final {
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	bool stopped = false;
	std::optional<platform::uri::Uri> sourceUri;
	std::vector<TaskConfigurationDefinition> definitions;
};

//! Thread-safe, pure task configuration catalog. It parses no files and starts no tasks.
class CTaskConfigurationCatalog final {
public:
	[[nodiscard]] TaskConfigurationCatalogResult Apply(const workspace::TasksDocumentSnapshot& snapshot);
	//! A known-absent selected task source clears the catalog with the caller's source fence.
	[[nodiscard]] TaskConfigurationCatalogResult Clear(std::uint64_t generation, std::uint64_t revision) noexcept;
	[[nodiscard]] TaskConfigurationCatalogResult Stop() noexcept;
	[[nodiscard]] TaskConfigurationCatalogSnapshot Snapshot() const;

private:
	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 0;
	std::uint64_t m_revision = 0;
	bool m_stopped = false;
	std::optional<std::wstring> m_sourceIdentity;
	std::optional<platform::uri::Uri> m_sourceUri;
	std::vector<TaskConfigurationDefinition> m_definitions;
};

} // namespace workbench::tasks
