/*! @file
	@brief Durable, opaque profile identity and control-authority generation.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace platform::profiles {

//! Stable control-owned metadata root shared by the authority and durable
//! platform-storage composition. Keeping this path policy here prevents the
//! process layer from duplicating the authority store's on-disk layout.
inline constexpr wchar_t kProfilePlatformMetadataDirectoryName[] = L".sakura-platform";

[[nodiscard]] inline std::filesystem::path BuildProfilePlatformMetadataDirectory(
	const std::filesystem::path& profileDirectory)
{
	return profileDirectory / kProfilePlatformMetadataDirectoryName;
}

//! An opaque UTF-8 identifier. It is random at first creation and immutable thereafter.
using ProfileAuthorityProfileId = std::string;

//! Every acquisition reaches one terminal status. Diagnostics deliberately never
//! contain a legacy alias, profile path, endpoint name, PID, or timestamp.
enum class ProfileAuthorityStoreStatus : unsigned char {
	Succeeded,
	InvalidArgument,
	MetadataDirectoryFailed,
	SecurityFailed,
	LockUnavailable,
	RecordReadFailed,
	RecordTooLarge,
	InvalidUtf8,
	UnsupportedSchema,
	CorruptRecord,
	RandomFailed,
	GenerationOverflow,
	WriteFailed,
	FlushFailed,
	ReplaceFailed,
	PlatformNotSupported,
	UnexpectedFailure,
};

struct ProfileAuthorityResult {
	ProfileAuthorityStoreStatus status = ProfileAuthorityStoreStatus::UnexpectedFailure;
	ProfileAuthorityProfileId profileId;
	std::uint64_t authorityGeneration = 0;
	std::wstring diagnostic;

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return status == ProfileAuthorityStoreStatus::Succeeded;
	}
};

//! Keeps a deny-sharing writer lock alive until the control owner has either
//! observed an existing record or durably replaced it.
class IProfileAuthorityStoreLock {
public:
	virtual ~IProfileAuthorityStoreLock() = default;
};

//! Narrow durable-I/O seam. Production uses the Win32 implementation supplied
//! by ProfileAuthorityStore; tests can inject a deterministic in-memory backend
//! to exercise every write and recovery terminal state.
class IProfileAuthorityStoreBackend {
public:
	virtual ~IProfileAuthorityStoreBackend() = default;

	virtual ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path& directory) = 0;
	virtual ProfileAuthorityStoreStatus AcquireExclusiveLock(
		const std::filesystem::path& lockPath,
		std::unique_ptr<IProfileAuthorityStoreLock>& lock) = 0;
	virtual ProfileAuthorityStoreStatus ReadRecord(
		const std::filesystem::path& recordPath,
		std::string& bytes,
		bool& exists) = 0;
	virtual ProfileAuthorityStoreStatus GenerateOpaqueProfileId(ProfileAuthorityProfileId& profileId) = 0;
	virtual ProfileAuthorityStoreStatus WriteRecordAtomically(
		const std::filesystem::path& recordPath,
		std::string_view bytes) = 0;
};

//! Creates the Windows backend. It creates new metadata files and directories
//! with a protected current-user-only DACL, holds a deny-sharing lock, writes a
//! same-volume temporary file, flushes it, and then replaces a checksummed,
//! canonical active record.
[[nodiscard]] std::shared_ptr<IProfileAuthorityStoreBackend> CreateWin32ProfileAuthorityStoreBackend();

//! Control-process-owned persistence for exactly one legacy profile directory.
//!
//! The directory is the migration anchor: moving/renaming that directory together
//! with its .sakura-platform metadata retains the generated ID. A legacy alias is
//! accepted only for caller compatibility and validation; it is deliberately not
//! persisted or used as identity. Copying a directory without its metadata is a
//! new profile and receives a new opaque ID.
class ProfileAuthorityStore final {
public:
	explicit ProfileAuthorityStore(
		std::filesystem::path profileDirectory,
		std::shared_ptr<IProfileAuthorityStoreBackend> backend = {});
	~ProfileAuthorityStore() = default;
	ProfileAuthorityStore(const ProfileAuthorityStore&) = delete;
	ProfileAuthorityStore& operator=(const ProfileAuthorityStore&) = delete;

	//! Resolves/creates the immutable profile ID and commits a nonzero authority
	//! generation before returning it. The caller must publish or use the returned
	//! generation only after this method succeeds.
	[[nodiscard]] ProfileAuthorityResult Acquire(std::wstring_view legacyProfileAlias = {});

private:
	std::filesystem::path m_profileDirectory;
	std::shared_ptr<IProfileAuthorityStoreBackend> m_backend;
};

} // namespace platform::profiles
