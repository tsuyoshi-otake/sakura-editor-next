/*! @file
 * @brief Control-platform composition adapter for the per-workspace Workspace Trust memento.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/IWorkspaceTrustMementoStore.h"
#include "platform/controlipc/EditorControlPlatformRuntime.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

//! Composition-root adapter; the trust prompts see only IWorkspaceTrustMementoStore.
namespace config {

/*!
	@brief Narrow test seam around the existing editor control-platform facade.

	Deliberately identical in shape to
	@c ControlPlatformTrustedFoldersStoreDependencies: the two adapters differ in
	what they address and in nothing else, and a second seam shape would make that
	difference harder to see rather than easier.
*/
struct ControlPlatformWorkspaceTrustMementoStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<std::optional<platform::storage::StorageEntry>(const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	std::function<std::string()> operationIdFactory;
};

} // namespace config

/*!
	@brief Control-platform composition adapter kept global to match the other _main owners.

	Unlike its Trusted Folders sibling, this record is addressed at
	@c EStorageScope::Workspace, keyed by the bounded canonical workspace identity
	the composition root already mints for working-copy persistence. That is what
	makes the record per-workspace: the same profile answers the startup prompt
	separately for every folder or @c .code-workspace it opens.

	@par An empty window has no workspace identity, and none is invented.
	@c workspaceScopeId is therefore optional, and every operation on an adapter
	without one resolves @c NoWorkspaceScope. Substituting the profile ID would
	make one empty window's answer silence the prompt in every other empty window;
	substituting a PID would make the record unrecoverable on the next launch.
	Neither is a per-workspace record, so neither is written.
 */
class CControlPlatformWorkspaceTrustMementoStore final : public config::IWorkspaceTrustMementoStore {
public:
	CControlPlatformWorkspaceTrustMementoStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime,
		std::string canonicalProfileId, std::optional<std::string> workspaceScopeId);
	CControlPlatformWorkspaceTrustMementoStore(
		std::string canonicalProfileId, std::optional<std::string> workspaceScopeId,
		config::ControlPlatformWorkspaceTrustMementoStoreDependencies dependencies);

	[[nodiscard]] config::WorkspaceTrustMementoLoadResult Load() override;
	[[nodiscard]] config::WorkspaceTrustMementoSaveResult Save(
		const config::WorkspaceTrustMemento& memento) override;

private:
	struct CapturedState final {
		platform::controlipc::EditorControlStorageCacheCoordinates coordinates;
		std::optional<std::string> canonicalPayload;
	};

	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;
	[[nodiscard]] config::WorkspaceTrustMementoLoadResult CoordinateFailure(
		const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const;
	void RememberPersisted(const platform::controlipc::EditorControlStorageApplyResult& result,
		const std::string& canonicalPayload);

	const std::string m_canonicalProfileId;
	const std::optional<std::string> m_workspaceScopeId;
	const config::ControlPlatformWorkspaceTrustMementoStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::optional<CapturedState> m_captured;
	//! Sticky until this adapter is discarded: a memento that failed to decode must not be overwritten.
	bool m_invalidStoredMemento = false;
};
