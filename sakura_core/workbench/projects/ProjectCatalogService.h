/*! @file
 * @brief Profile-scoped saved Project catalog.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace workbench::projects {

inline constexpr std::size_t kMaximumProjects = 64;
inline constexpr std::uint32_t kProjectCatalogSchemaVersion = 1;

enum class EProjectKind : std::uint8_t {
	Folder,
	Workspace,
};

//! A Project is a saved navigation entry. It never owns or replaces the active
//! WorkspaceContext; opening its URI goes through the existing workspace path.
struct ProjectEntry final {
	EProjectKind kind{ EProjectKind::Folder };
	platform::uri::Uri uri;
	std::optional<std::wstring> label;
};

enum class EProjectCatalogOutcome : std::uint8_t {
	Succeeded,
	Failed,
};

enum class EProjectCatalogState : std::uint8_t {
	Unloaded,
	Ready,
	Unavailable,
};

struct ProjectCatalogResult final {
	EProjectCatalogOutcome outcome{ EProjectCatalogOutcome::Failed };
	std::string diagnostic;
};

enum class EProjectCatalogStoreLoadStatus : std::uint8_t {
	Succeeded,
	NotFound,
	Unavailable,
	Failed,
};

struct ProjectCatalogStoreLoadResult final {
	EProjectCatalogStoreLoadStatus status{ EProjectCatalogStoreLoadStatus::Failed };
	std::optional<std::string> payload;
	std::string diagnostic;
};

enum class EProjectCatalogStoreSaveStatus : std::uint8_t {
	Succeeded,
	Unavailable,
	Conflict,
	Failed,
};

struct ProjectCatalogStoreSaveResult final {
	EProjectCatalogStoreSaveStatus status{ EProjectCatalogStoreSaveStatus::Failed };
	std::string diagnostic;
};

class IProjectCatalogStore {
public:
	virtual ~IProjectCatalogStore() = default;
	virtual ProjectCatalogStoreLoadResult Load() = 0;
	virtual ProjectCatalogStoreSaveResult Save(std::string payload) = 0;
};

class IProjectCatalogService {
public:
	virtual ~IProjectCatalogService() = default;
	virtual ProjectCatalogResult Load() = 0;
	[[nodiscard]] virtual EProjectCatalogState State() const noexcept = 0;
	[[nodiscard]] virtual std::vector<ProjectEntry> Snapshot() const = 0;
	//! Promote only a Folder or Workspace which has crossed the workbench ready
	//! boundary. Reopening an existing Project updates it without reordering it.
	virtual ProjectCatalogResult RecordSuccessfulOpen(ProjectEntry entry) = 0;
	virtual ProjectCatalogResult Remove(const platform::uri::Uri& uri) = 0;
};

class CProjectCatalogService final : public IProjectCatalogService {
public:
	explicit CProjectCatalogService(std::unique_ptr<IProjectCatalogStore> store) noexcept;

	ProjectCatalogResult Load() override;
	[[nodiscard]] EProjectCatalogState State() const noexcept override { return m_state; }
	[[nodiscard]] std::vector<ProjectEntry> Snapshot() const override;
	ProjectCatalogResult RecordSuccessfulOpen(ProjectEntry entry) override;
	ProjectCatalogResult Remove(const platform::uri::Uri& uri) override;

	[[nodiscard]] static std::optional<ProjectEntry> Normalize(ProjectEntry entry);

private:
	[[nodiscard]] static std::optional<std::vector<ProjectEntry>> Decode(
		std::string_view payload, std::string& diagnostic);
	[[nodiscard]] static std::optional<std::string> Encode(
		const std::vector<ProjectEntry>& entries);
	[[nodiscard]] static bool SameIdentity(
		const platform::uri::Uri& left, const platform::uri::Uri& right) noexcept;
	[[nodiscard]] ProjectCatalogStoreSaveResult SaveEntries(
		const std::vector<ProjectEntry>& entries);

	std::unique_ptr<IProjectCatalogStore> m_store;
	std::vector<ProjectEntry> m_entries;
	EProjectCatalogState m_state{ EProjectCatalogState::Unloaded };
};

} // namespace workbench::projects
