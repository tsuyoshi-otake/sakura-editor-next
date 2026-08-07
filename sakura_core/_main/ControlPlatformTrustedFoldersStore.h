/*! @file
 * @brief Control-platform composition adapter for the Trusted Folders and Workspaces list.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/ITrustedFoldersStore.h"
#include "platform/controlipc/EditorControlPlatformRuntime.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

//! Composition-root adapter; workspace trust resolution sees only ITrustedFoldersStore.
namespace config {

/*!
	@brief Narrow test seam around the existing editor control-platform facade.

	Production construction binds these operations to one
	CEditorControlPlatformRuntime. The seam deliberately has no file or endpoint
	access, so tests can cover persistence terminal behavior without IPC.
*/
struct ControlPlatformTrustedFoldersStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<std::optional<platform::storage::StorageEntry>(const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	std::function<std::string()> operationIdFactory;
};

} // namespace config

//! Control-platform composition adapter kept global to match the other _main owners.
class CControlPlatformTrustedFoldersStore final : public config::ITrustedFoldersStore {
public:
	CControlPlatformTrustedFoldersStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId);
	CControlPlatformTrustedFoldersStore(
		std::string canonicalProfileId, config::ControlPlatformTrustedFoldersStoreDependencies dependencies);

	[[nodiscard]] config::TrustedFoldersLoadResult Load() override;
	[[nodiscard]] config::TrustedFoldersSaveResult Save(const config::TrustedFoldersSnapshot& snapshot) override;

private:
	struct CapturedState final {
		platform::controlipc::EditorControlStorageCacheCoordinates coordinates;
		std::optional<std::string> canonicalPayload;
	};

	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;
	[[nodiscard]] config::TrustedFoldersLoadResult CoordinateFailure(
		const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const;
	void RememberPersisted(const platform::controlipc::EditorControlStorageApplyResult& result,
		const std::string& canonicalPayload);

	const std::string m_canonicalProfileId;
	const config::ControlPlatformTrustedFoldersStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::optional<CapturedState> m_captured;
	//! Sticky until this adapter is discarded: a trust list that failed to decode must not be overwritten.
	bool m_invalidStoredList = false;
};
