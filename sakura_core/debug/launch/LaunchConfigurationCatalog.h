/*! @file
 * @brief Pure, bounded launch-configuration catalog.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/serialization/JsoncDocument.h>
#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace debug::launch {

//! Catalog-specific limits are deliberately derived from the shared bounded
//! JSONC contract. They keep one accepted launch artifact inexpensive to copy
//! without introducing a second, incompatible JSON limit policy.
struct LaunchConfigurationCatalogLimits final {
	static constexpr std::size_t kMaximumConfigurations =
		platform::serialization::CJsoncDocument::kMaximumNodes / 256U;
	static constexpr std::size_t kMaximumCompounds =
		platform::serialization::CJsoncDocument::kMaximumNodes / 256U;
	static constexpr std::size_t kMaximumCompoundReferences =
		platform::serialization::CJsoncDocument::kMaximumNodes / 256U;
	static constexpr std::size_t kMaximumIdentifierLength =
		platform::serialization::CJsoncDocument::kMaximumObjectKeyLength / 256U;
};

enum class ELaunchConfigurationCatalogStatus : std::uint8_t {
	Applied,
	Cleared,
	StaleGeneration,
	StaleRevision,
	Stopped,
	InvalidSnapshot,
	InvalidDocument,
	InvalidResource,
	InvalidSchema,
	ConfigurationsMustBeArray,
	CompoundsMustBeArray,
	MaximumConfigurationsExceeded,
	MaximumCompoundsExceeded,
	MaximumCompoundReferencesExceeded,
	InvalidConfiguration,
	InvalidCompound,
	DuplicateName,
	DuplicateCompoundReference,
	UnknownCompoundReference,
};

enum class ELaunchConfigurationCatalogState : std::uint8_t {
	Empty,
	Ready,
	Stopped,
};

struct LaunchConfigurationCatalogResult final {
	ELaunchConfigurationCatalogStatus status = ELaunchConfigurationCatalogStatus::InvalidSnapshot;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ELaunchConfigurationCatalogStatus::Applied
			|| status == ELaunchConfigurationCatalogStatus::Cleared;
	}
};

//! Adapter-specific launch fields stay opaque. This catalog validates and
//! preserves them but does not interpret them as DAP requests or processes.
struct LaunchConfiguration final {
	std::wstring name;
	std::wstring type;
	std::wstring request;
	platform::serialization::JsoncValue::Object raw;
};

struct LaunchCompound final {
	std::wstring name;
	std::vector<std::wstring> configurations;
	platform::serialization::JsoncValue::Object raw;
};

//! A value snapshot of one accepted source. Callers receive copies, so no
//! consumer can mutate the catalog retained by the service.
struct LaunchConfigurationCatalog final {
	LaunchConfigurationCatalog(platform::uri::Uri sourceValue, std::uint64_t generationValue, std::uint64_t revisionValue)
		: source(std::move(sourceValue)), generation(generationValue), revision(revisionValue) {}

	platform::uri::Uri source;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::vector<LaunchConfiguration> configurations;
	std::vector<LaunchCompound> compounds;
};

struct LaunchConfigurationCatalogSnapshot final {
	ELaunchConfigurationCatalogState state = ELaunchConfigurationCatalogState::Empty;
	std::optional<LaunchConfigurationCatalog> catalog;
};

//! Pure semantic catalog over accepted workspace Launch documents. It owns no
//! I/O, window, adapter transport, session, or process lifetime. Every mutator
//! returns exactly one terminal result and commits only a fully validated value.
class CLaunchConfigurationCatalog final {
public:
	[[nodiscard]] LaunchConfigurationCatalogResult Apply(
		const workbench::workspace::LaunchDocumentSnapshot& snapshot);
	//! Clears the retained catalog under the same monotonic source fence used by
	//! Apply. A missing document is never an implicit clear operation.
	[[nodiscard]] LaunchConfigurationCatalogResult Clear(std::uint64_t generation, std::uint64_t revision) noexcept;
	[[nodiscard]] LaunchConfigurationCatalogResult Stop() noexcept;
	[[nodiscard]] LaunchConfigurationCatalogResult Dispose() noexcept { return Stop(); }

	[[nodiscard]] LaunchConfigurationCatalogSnapshot Snapshot() const;
	[[nodiscard]] std::optional<LaunchConfiguration> FindConfiguration(std::wstring_view name) const;
	[[nodiscard]] std::optional<LaunchCompound> FindCompound(std::wstring_view name) const;

private:
	[[nodiscard]] static LaunchConfigurationCatalogResult BuildCatalog(
		const workbench::workspace::WorkspaceArtifactDocument& document, LaunchConfigurationCatalog& catalog);
	[[nodiscard]] static bool IsValidResource(const platform::uri::Uri& resource) noexcept;

	mutable std::mutex m_mutex;
	ELaunchConfigurationCatalogState m_state = ELaunchConfigurationCatalogState::Empty;
	std::optional<LaunchConfigurationCatalog> m_catalog;
	std::uint64_t m_generation = 0;
	std::uint64_t m_revision = 0;
};

} // namespace debug::launch
