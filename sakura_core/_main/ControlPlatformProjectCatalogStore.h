/*! @file
 * @brief Control-owned Profile/User persistence adapter for saved Projects.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "workbench/projects/ProjectCatalogService.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace workbench::projects {

struct ControlPlatformProjectCatalogStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<platform::controlipc::EditorControlStorageCacheWaitResult(std::chrono::milliseconds)> waitForStorageCacheReady;
	std::function<std::optional<platform::storage::StorageEntry>(
		const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	std::function<std::string()> operationIdFactory;
};

} // namespace workbench::projects

class CControlPlatformProjectCatalogStore final : public workbench::projects::IProjectCatalogStore {
public:
	CControlPlatformProjectCatalogStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime,
		std::string canonicalProfileId);
	CControlPlatformProjectCatalogStore(
		std::string canonicalProfileId,
		workbench::projects::ControlPlatformProjectCatalogStoreDependencies dependencies);

	workbench::projects::ProjectCatalogStoreLoadResult Load() override;
	workbench::projects::ProjectCatalogStoreSaveResult Save(std::string payload) override;

private:
	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;

	const std::string m_canonicalProfileId;
	const workbench::projects::ControlPlatformProjectCatalogStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::optional<platform::controlipc::EditorControlStorageCacheCoordinates> m_coordinates;
};
