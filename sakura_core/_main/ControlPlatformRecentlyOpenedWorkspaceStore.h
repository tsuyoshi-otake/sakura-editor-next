/*! @file
 * @brief Control-owned Profile/User persistence adapter for recent workspaces.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "workbench/recent/RecentlyOpenedWorkspaceService.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace workbench::recent {

struct ControlPlatformRecentlyOpenedWorkspaceStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<platform::controlipc::EditorControlStorageCacheWaitResult(std::chrono::milliseconds)> waitForStorageCacheReady;
	std::function<std::optional<platform::storage::StorageEntry>(const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	std::function<std::string()> operationIdFactory;
};

} // namespace workbench::recent

//! Composition root only: editor windows borrow the recent service and never the
//! control runtime or durable backend directly.
class CControlPlatformRecentlyOpenedWorkspaceStore final : public workbench::recent::IRecentlyOpenedWorkspaceStore {
public:
	CControlPlatformRecentlyOpenedWorkspaceStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId);
	CControlPlatformRecentlyOpenedWorkspaceStore(
		std::string canonicalProfileId, workbench::recent::ControlPlatformRecentlyOpenedWorkspaceStoreDependencies dependencies);

	workbench::recent::RecentlyOpenedWorkspaceStoreLoadResult Load() override;
	workbench::recent::RecentlyOpenedWorkspaceStoreSaveResult Save(std::string payload) override;

private:
	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;

	const std::string m_canonicalProfileId;
	const workbench::recent::ControlPlatformRecentlyOpenedWorkspaceStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::optional<platform::controlipc::EditorControlStorageCacheCoordinates> m_coordinates;
};
