/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "workbench/statusbar/IStatusbarVisibilityMementoStore.h"

#include <mutex>
#include <optional>
#include <string>

//! Control-process storage adapter for the profile/user key workbench.statusbar.hidden.
class CControlPlatformStatusbarVisibilityMementoStore final
	: public workbench::statusbar::IStatusbarVisibilityMementoStore {
public:
	CControlPlatformStatusbarVisibilityMementoStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId);

	[[nodiscard]] workbench::statusbar::StatusbarMementoLoadResult Load() override;
	[[nodiscard]] workbench::statusbar::StatusbarMementoSaveResult Save(
		const std::vector<std::string>& hiddenIds) override;

private:
	[[nodiscard]] std::optional<platform::storage::StorageAddress> Address() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;

	platform::controlipc::CEditorControlPlatformRuntime& m_runtime;
	const std::string m_canonicalProfileId;
	std::mutex m_mutex;
	std::optional<platform::controlipc::EditorControlStorageCacheCoordinates> m_coordinates;
	std::optional<std::string> m_canonicalPayload;
	bool m_invalidStoredMemento = false;
};
