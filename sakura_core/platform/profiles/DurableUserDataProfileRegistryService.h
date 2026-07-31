/*! @file @brief Control-owned persistence and selection boundary for user profiles. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/profiles/UserDataProfileRegistryCodec.h"
#include "platform/storage/IStorageService.h"

#include <optional>

namespace platform::profiles {

enum class DurableUserDataProfileRegistryStatus : unsigned char {
	Loaded,
	NotFound,
	Saved,
	Replayed,
	Conflict,
	StorageFailure,
	CorruptPreserved,
	UnsupportedPreserved,
	InvalidArgument,
	DuplicateProfileId,
	DuplicateDisplayName,
	AssociationConflict,
};

struct DurableUserDataProfileRegistryResult {
	DurableUserDataProfileRegistryStatus status = DurableUserDataProfileRegistryStatus::StorageFailure;
	std::uint64_t storageRevision = 0;
	std::uint64_t registryRevision = 0;
	bool replayed = false;
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == DurableUserDataProfileRegistryStatus::Loaded || status == DurableUserDataProfileRegistryStatus::NotFound
			|| status == DurableUserDataProfileRegistryStatus::Saved || status == DurableUserDataProfileRegistryStatus::Replayed;
	}
};

//! The only storage entry touched by this adapter. It stores no secrets and no
//! extension implementation data; callers must serialize access in the control process.
class DurableUserDataProfileRegistryService final {
public:
	DurableUserDataProfileRegistryService(UserDataProfileRegistry& registry, ::platform::storage::IStorageService& storage) noexcept;
	[[nodiscard]] DurableUserDataProfileRegistryResult Load();
	[[nodiscard]] DurableUserDataProfileRegistryResult Save(
		std::string operationId, std::optional<std::uint64_t> expectedStorageRevision = std::nullopt);
	[[nodiscard]] std::string ExportPortableDocument() const;
	[[nodiscard]] DurableUserDataProfileRegistryResult ImportPortableDocument(
		std::string_view document, std::string operationId, std::optional<std::uint64_t> expectedStorageRevision = std::nullopt);
	[[nodiscard]] UserDataProfileResolveResult Resolve(const UserDataProfileResolveRequest& request) const;

private:
	[[nodiscard]] static ::platform::storage::StorageAddress Address();
	[[nodiscard]] DurableUserDataProfileRegistryResult Persist(std::string operationId,
		std::optional<std::uint64_t> expectedStorageRevision);

	UserDataProfileRegistry& m_registry;
	::platform::storage::IStorageService& m_storage;
};

} // namespace platform::profiles
