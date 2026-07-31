/*! @file
 * @brief Folder-scoped coordinator for pure task configuration catalogs.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/tasks/TaskConfigurationCatalog.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace workbench::tasks {

//! A copied folder-scoped value.  An empty catalog means that the folder is
//! known but neither its `.vscode/tasks.json` nor the workspace fallback has
//! an accepted Tasks document.  It is not a request to use another folder.
struct FolderTaskCatalogSnapshot final {
	platform::uri::Uri folderUri;
	TaskConfigurationCatalogSnapshot catalog;
};

struct FolderTaskCatalogRegistrySnapshot final {
	//! The maximum current workspace/artifact generation observed at reconcile.
	std::uint64_t generation = 0;
	//! Advances only when a caller-visible slot, generation, or stopped state changes.
	std::uint64_t revision = 0;
	bool stopped = false;
	//! Sorted by canonical URI identity, never by workspace-folder input order.
	std::vector<FolderTaskCatalogSnapshot> folders;
};

enum class EFolderTaskCatalogRegistryStatus : std::uint8_t {
	Applied,
	NoChange,
	Stopped,
	TooManyFolders,
	InvalidFolder,
	DuplicateFolder,
	CatalogRejected,
	Exception,
};

struct FolderTaskCatalogRegistryResult final {
	EFolderTaskCatalogRegistryStatus status = EFolderTaskCatalogRegistryStatus::Exception;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EFolderTaskCatalogRegistryStatus::Applied
			|| status == EFolderTaskCatalogRegistryStatus::NoChange;
	}
};

//! Owns one pure CTaskConfigurationCatalog per explicit semantic workspace
//! folder.  It owns neither artifact-file reads/watchers nor the artifact
//! service lifecycle; callers reconcile it after an accepted topology or
//! artifact change.
class CFolderTaskCatalogRegistry final {
public:
	//! Rebuilds the complete folder set before publishing it.  One bounded
	//! `artifacts.TasksForFolders` snapshot supplies every exact folder selection
	//! and the matching artifact-service generation/stopped state.
	[[nodiscard]] FolderTaskCatalogRegistryResult Reconcile(
		const config::WorkspaceContextSnapshot& context,
		const workspace::CWorkspaceArtifactDocumentService& artifacts);
	//! Terminal: clears every slot.  Reconcile is rejected after Stop.
	[[nodiscard]] FolderTaskCatalogRegistryResult Stop() noexcept;
	[[nodiscard]] FolderTaskCatalogRegistrySnapshot Snapshot() const;
	//! Explicit folder lookup only; there is deliberately no global/default catalog.
	[[nodiscard]] std::optional<FolderTaskCatalogSnapshot> SnapshotForFolder(
		const platform::uri::Uri& folderUri) const;

private:
	struct Slot final {
		platform::uri::Uri folderUri;
		std::wstring selectionIdentity;
		std::uint64_t selectionGeneration = 0;
		std::uint64_t selectionRevision = 0;
		std::unique_ptr<CTaskConfigurationCatalog> catalog;
	};

	[[nodiscard]] static bool IsEquivalent(
		const std::map<std::wstring, Slot, std::less<>>& left,
		const std::map<std::wstring, Slot, std::less<>>& right) noexcept;
	[[nodiscard]] FolderTaskCatalogRegistrySnapshot SnapshotLocked() const;

	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 0;
	std::uint64_t m_revision = 0;
	bool m_stopped = false;
	std::map<std::wstring, Slot, std::less<>> m_slots;
};

} // namespace workbench::tasks
