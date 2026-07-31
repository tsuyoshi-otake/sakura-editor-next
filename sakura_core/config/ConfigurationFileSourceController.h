/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/JsoncConfigurationSource.h"
#include "platform/filesystem/IFileService.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace config {

//! Terminal outcome of one file-backed configuration document operation.  The
//! controller deliberately distinguishes a rejected external edit from a CAS
//! conflict: neither may replace the last accepted source model.
enum class EConfigurationFileSourceControllerStatus : std::uint8_t {
	Applied,
	NoChange,
	Replayed,
	ReadFailed,
	ParseFailed,
	IdentityConflict,
	Conflict,
	OperationIdConflict,
	ConfigurationRejected,
	OperationIdExhausted,
	NotTracked,
};

//! File-backed document result. Diagnostics are stable categories/messages only:
//! they never echo a resource URI or local filesystem path.
struct ConfigurationFileSourceControllerResult final {
	EConfigurationFileSourceControllerStatus status = EConfigurationFileSourceControllerStatus::ReadFailed;
	std::optional<platform::filesystem::EFileResultStatus> fileStatus;
	std::optional<JsoncConfigurationDiagnostic> jsoncDiagnostic;
	EConfigurationOutcome configurationOutcome = EConfigurationOutcome::Unsupported;
	bool resourceWasMissing = false;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EConfigurationFileSourceControllerStatus::Applied
			|| status == EConfigurationFileSourceControllerStatus::NoChange
			|| status == EConfigurationFileSourceControllerStatus::Replayed;
	}
};

//! Synchronous, bounded bridge from a single URI resource to one semantic
//! configuration source.  It borrows its filesystem and configuration service;
//! callers own their lifetimes and choose when to invoke Reload after a watch.
class CConfigurationFileSourceController final {
public:
	CConfigurationFileSourceController(
		platform::filesystem::IFileService& fileService,
		IConfigurationService& configurationService) noexcept;

	CConfigurationFileSourceController(const CConfigurationFileSourceController&) = delete;
	CConfigurationFileSourceController& operator=(const CConfigurationFileSourceController&) = delete;

	//! Loads (or reloads) `resource` under `documentKey`. A missing resource is
	//! an empty JSON object and therefore atomically clears earlier base and
	//! language contributions. Other read and parse failures retain the accepted
	//! revisions/model and never call IConfigurationService.
	[[nodiscard]] ConfigurationFileSourceControllerResult Reload(
		std::string_view documentKey,
		const ConfigurationSource& source,
		const platform::uri::Uri& resource);

	//! Atomically removes the tracked source contributions. Tracking disappears
	//! only after an Applied/NoChange/Replayed terminal result.
	[[nodiscard]] ConfigurationFileSourceControllerResult Remove(std::string_view documentKey);
	[[nodiscard]] ConfigurationFileSourceControllerResult Deactivate(std::string_view documentKey)
	{
		return Remove(documentKey);
	}

private:
	struct TrackedDocument final {
		ConfigurationSource source;
		std::wstring resourceIdentity;
		JsoncConfigurationSourceRevisions revisions {};
	};

	[[nodiscard]] std::optional<std::string> NextOperationId();
	[[nodiscard]] ConfigurationFileSourceControllerResult ApplyDocument(
		TrackedDocument& tracked,
		std::string_view utf8,
		bool resourceWasMissing);
	static bool IsSameIdentity(
		const TrackedDocument& tracked,
		const ConfigurationSource& source,
		const platform::uri::Uri& resource) noexcept;
	static void RememberAcceptedRevisions(
		TrackedDocument& tracked,
		const ConfigurationBatchResult& result);

	platform::filesystem::IFileService& m_fileService;
	IConfigurationService& m_configurationService;
	//! Runtime watch callbacks and workspace transitions can request a reload at
	//! the same time. Keep document identity/CAS bookkeeping serial here rather
	//! than asking every composition owner to duplicate that exclusion.
	std::mutex m_mutex;
	std::map<std::string, TrackedDocument, std::less<>> m_documents;
};

} // namespace config
