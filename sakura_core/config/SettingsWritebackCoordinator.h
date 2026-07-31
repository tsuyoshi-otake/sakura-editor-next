/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/ConfigurationFileSourceController.h"
#include "config/editing/CJsoncConfigurationEditor.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace config {

//! A writeback request binds a textual edit to the one source controller that
//! owns the post-write resnapshot.  Callers cannot publish a document without
//! immediately reconciling the semantic configuration model from the file.
struct SettingsWritebackRequest final {
	editing::ConfigurationDocumentEditRequest edit;
	std::string documentKey;
	ConfigurationSource source;
};

enum class ESettingsWritebackStatus : std::uint8_t {
	Applied,
	NoChange,
	//! A conditional replace conflicted once and the requested edit was replayed
	//! against the current version before a successful resnapshot.
	Replayed,
	Conflict,
	InvalidRequest,
	EditRejected,
	ResnapshotRejected,
	Stopped,
	Failed,
};

//! Stable category-only result.  It deliberately carries the underlying typed
//! outcomes but no URI, path, key, or serialized setting value.
struct SettingsWritebackResult final {
	ESettingsWritebackStatus status = ESettingsWritebackStatus::Failed;
	std::size_t attempts = 0;
	editing::ConfigurationDocumentEditResult edit;
	std::optional<ConfigurationFileSourceControllerResult> resnapshot;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ESettingsWritebackStatus::Applied
			|| status == ESettingsWritebackStatus::NoChange
			|| status == ESettingsWritebackStatus::Replayed;
	}
};

//! Serial, bounded bridge between JSONC-preserving writeback and the existing
//! file-source controller.  The same controller is also the owner used by
//! advisory file watches, so both paths resnapshot rather than applying a
//! callback body or assuming the bytes just written are still current.
class CSettingsWritebackCoordinator final {
public:
	//! One initial conditional write and at most one fresh read/replay on CAS
	//! conflict.  Further contention is reported as Conflict, never overwritten.
	static constexpr std::size_t kMaximumAttempts = 2;

	CSettingsWritebackCoordinator(
		platform::filesystem::IFileService& fileService,
		CConfigurationFileSourceController& fileSources) noexcept;

	CSettingsWritebackCoordinator(const CSettingsWritebackCoordinator&) = delete;
	CSettingsWritebackCoordinator& operator=(const CSettingsWritebackCoordinator&) = delete;

	[[nodiscard]] SettingsWritebackResult Write(const SettingsWritebackRequest& request);
	//! Stop waits for an already-running synchronous Write to reach its terminal
	//! result.  Later writes perform no filesystem operation or resnapshot.
	[[nodiscard]] SettingsWritebackResult Stop() noexcept;
	[[nodiscard]] bool IsStopped() const noexcept;

private:
	[[nodiscard]] static bool IsValidRequest(const SettingsWritebackRequest& request) noexcept;
	[[nodiscard]] static SettingsWritebackResult Result(
		ESettingsWritebackStatus status,
		std::string diagnostic);

	CConfigurationFileSourceController& m_fileSources;
	editing::CJsoncConfigurationEditor m_editor;
	mutable std::mutex m_mutex;
	bool m_stopped = false;
};

} // namespace config
