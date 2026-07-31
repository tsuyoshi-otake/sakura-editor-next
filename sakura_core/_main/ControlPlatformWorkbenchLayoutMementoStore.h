/*! @file
 * @brief Control-platform composition adapter for workbench layout persistence.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "workbench/layout/IWorkbenchLayoutMementoStore.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

//! Composition-root adapter; the workbench sees only IWorkbenchLayoutMementoStore.
namespace workbench::layout {

/*! 
	@brief Narrow test seam around the existing editor control-platform facade.

	Production construction binds these operations to one
	CEditorControlPlatformRuntime. The seam deliberately has no file or endpoint
	access, so tests can cover persistence terminal behavior without IPC.
*/
struct ControlPlatformWorkbenchLayoutMementoStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<std::optional<platform::storage::StorageEntry>(const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	std::function<std::string()> operationIdFactory;
};

} // namespace workbench::layout

//! Control-platform composition adapter kept global to match the other _main owners.
class CControlPlatformWorkbenchLayoutMementoStore final : public workbench::layout::IWorkbenchLayoutMementoStore {
public:
	CControlPlatformWorkbenchLayoutMementoStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId);
	CControlPlatformWorkbenchLayoutMementoStore(
		std::string canonicalProfileId, workbench::layout::ControlPlatformWorkbenchLayoutMementoStoreDependencies dependencies);

	[[nodiscard]] workbench::layout::WorkbenchLayoutMementoLoadResult Load() override;
	[[nodiscard]] workbench::layout::WorkbenchLayoutMementoSaveResult Save(
		const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot) override;

private:
	struct CapturedState final {
		platform::controlipc::EditorControlStorageCacheCoordinates coordinates;
		std::optional<std::string> canonicalPayload;
	};

	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;
	[[nodiscard]] workbench::layout::WorkbenchLayoutMementoLoadResult CoordinateFailure(
		const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const;
	void RememberPersisted(const platform::controlipc::EditorControlStorageApplyResult& result,
		const std::string& canonicalPayload);

	const std::string m_canonicalProfileId;
	const workbench::layout::ControlPlatformWorkbenchLayoutMementoStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::optional<CapturedState> m_captured;
	//! Sticky until this adapter is discarded: corrupt durable data must not be overwritten.
	bool m_invalidStoredMemento = false;
};
