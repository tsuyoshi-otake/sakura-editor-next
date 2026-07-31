/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/FolderTaskCatalogRegistry.h"

#include "platform/uri/UriIdentity.h"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace workbench::tasks {
namespace {

constexpr std::size_t kMaximumWorkspaceFolders = 64U;

FolderTaskCatalogRegistryResult Failure(EFolderTaskCatalogRegistryStatus status, const char* diagnostic) noexcept
{
	return { status, diagnostic };
}

std::uint64_t EffectiveGeneration(
	const config::WorkspaceContextSnapshot& workspace,
	const workspace::WorkspaceArtifactDocumentServiceSnapshot& artifacts) noexcept
{
	return (std::max)(workspace.generation, artifacts.generation);
}

} // namespace

FolderTaskCatalogRegistryResult CFolderTaskCatalogRegistry::Reconcile(
	const config::WorkspaceContextSnapshot& context,
	const workspace::CWorkspaceArtifactDocumentService& artifacts)
{
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EFolderTaskCatalogRegistryStatus::Stopped, "folder task catalog registry is stopped");
	}

	try {
		std::vector<std::pair<std::wstring, platform::uri::Uri>> folders;
		std::set<std::wstring, std::less<>> folderIdentities;
		if (context.kind != config::EWorkspaceKind::Empty) {
			if (context.folders.size() > kMaximumWorkspaceFolders) {
				return Failure(EFolderTaskCatalogRegistryStatus::TooManyFolders, "workspace folder limit exceeded");
			}
			folders.reserve(context.folders.size());
			for (const auto& folder : context.folders) {
				const auto identity = platform::uri::UriIdentityService::MakeComparisonKey(folder.uri);
				if (identity.empty()) return Failure(EFolderTaskCatalogRegistryStatus::InvalidFolder, "workspace folder identity is invalid");
				if (!folderIdentities.emplace(identity).second) {
					return Failure(EFolderTaskCatalogRegistryStatus::DuplicateFolder, "workspace contains duplicate folder identities");
				}
				folders.emplace_back(identity, folder.uri);
			}
		}

		std::vector<platform::uri::Uri> folderUris;
		folderUris.reserve(folders.size());
		for (const auto& [identity, folderUri] : folders) {
			static_cast<void>(identity);
			folderUris.push_back(folderUri);
		}
		const auto batch = artifacts.TasksForFolders(folderUris);
		if (!batch.Succeeded() || batch.documents.size() != folders.size()) {
			return Failure(EFolderTaskCatalogRegistryStatus::CatalogRejected, "tasks batch snapshot was not accepted");
		}

		std::map<std::wstring, Slot, std::less<>> nextSlots;
		for (std::size_t index = 0; index < folders.size(); ++index) {
			const auto& [identity, folderUri] = folders[index];
			Slot slot {
				.folderUri = folderUri,
				.catalog = std::make_unique<CTaskConfigurationCatalog>(),
			};
			const auto& selected = batch.documents[index];
			if (selected.document) {
				slot.selectionIdentity = platform::uri::UriIdentityService::MakeComparisonKey(selected.document->resource);
				slot.selectionGeneration = selected.document->generation;
				slot.selectionRevision = selected.document->revision;
				if (slot.selectionIdentity.empty()) {
					return Failure(EFolderTaskCatalogRegistryStatus::CatalogRejected, "selected tasks document identity is invalid");
				}
				const auto applied = slot.catalog->Apply(selected);
				if (!applied.Succeeded()) {
					return { EFolderTaskCatalogRegistryStatus::CatalogRejected, applied.diagnostic };
				}
			}
			if (!nextSlots.emplace(identity, std::move(slot)).second) {
				return Failure(EFolderTaskCatalogRegistryStatus::DuplicateFolder, "workspace contains duplicate folder identities");
			}
		}

		const auto effectiveGeneration = EffectiveGeneration(context, batch.service);
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EFolderTaskCatalogRegistryStatus::Stopped, "folder task catalog registry is stopped");
		if (m_generation == effectiveGeneration && IsEquivalent(m_slots, nextSlots)) {
			return { EFolderTaskCatalogRegistryStatus::NoChange, {} };
		}
		m_generation = effectiveGeneration;
		++m_revision;
		m_slots = std::move(nextSlots);
		return { EFolderTaskCatalogRegistryStatus::Applied, {} };
	} catch (const std::exception& exception) {
		return { EFolderTaskCatalogRegistryStatus::Exception, exception.what() };
	} catch (...) {
		return Failure(EFolderTaskCatalogRegistryStatus::Exception, "unknown folder task catalog registry failure");
	}
}

FolderTaskCatalogRegistryResult CFolderTaskCatalogRegistry::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopped) return Failure(EFolderTaskCatalogRegistryStatus::Stopped, "folder task catalog registry is stopped");
	m_stopped = true;
	for (auto& [identity, slot] : m_slots) {
		static_cast<void>(identity);
		static_cast<void>(slot.catalog->Stop());
	}
	m_slots.clear();
	++m_revision;
	return { EFolderTaskCatalogRegistryStatus::Applied, "folder task catalog registry stopped" };
}

FolderTaskCatalogRegistrySnapshot CFolderTaskCatalogRegistry::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked();
}

std::optional<FolderTaskCatalogSnapshot> CFolderTaskCatalogRegistry::SnapshotForFolder(
	const platform::uri::Uri& folderUri) const
{
	const auto identity = platform::uri::UriIdentityService::MakeComparisonKey(folderUri);
	if (identity.empty()) return std::nullopt;
	std::lock_guard lock(m_mutex);
	const auto found = m_slots.find(identity);
	if (found == m_slots.end()) return std::nullopt;
	return FolderTaskCatalogSnapshot { found->second.folderUri, found->second.catalog->Snapshot() };
}

bool CFolderTaskCatalogRegistry::IsEquivalent(
	const std::map<std::wstring, Slot, std::less<>>& left,
	const std::map<std::wstring, Slot, std::less<>>& right) noexcept
{
	if (left.size() != right.size()) return false;
	for (const auto& [identity, leftSlot] : left) {
		const auto found = right.find(identity);
		if (found == right.end()) return false;
		const auto& rightSlot = found->second;
		// URI text is deliberately excluded: equivalent canonical folder identity
		// is not a topology change.  Selection metadata is the public catalog fence.
		if (leftSlot.selectionIdentity != rightSlot.selectionIdentity
			|| leftSlot.selectionGeneration != rightSlot.selectionGeneration
			|| leftSlot.selectionRevision != rightSlot.selectionRevision) return false;
	}
	return true;
}

FolderTaskCatalogRegistrySnapshot CFolderTaskCatalogRegistry::SnapshotLocked() const
{
	FolderTaskCatalogRegistrySnapshot snapshot;
	snapshot.generation = m_generation;
	snapshot.revision = m_revision;
	snapshot.stopped = m_stopped;
	snapshot.folders.reserve(m_slots.size());
	for (const auto& [identity, slot] : m_slots) {
		static_cast<void>(identity);
		snapshot.folders.push_back({ slot.folderUri, slot.catalog->Snapshot() });
	}
	return snapshot;
}

} // namespace workbench::tasks
