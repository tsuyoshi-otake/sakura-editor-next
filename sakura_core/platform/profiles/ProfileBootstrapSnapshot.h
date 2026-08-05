/*! @file
	@brief Immutable, no-I/O resource identity snapshot for the legacy profile-directory bridge.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/profiles/ProfileAuthorityIdentity.h"
#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace platform::profiles {

//! Distinguishes the temporary legacy directory bridge from persisted registry data.
enum class ProfileBootstrapSnapshotSource : unsigned char {
	LegacyDirectoryBridge,
};

//! Every bootstrap attempt finishes in one typed state. No diagnostic value is
//! carried so opaque IDs and local paths cannot leak through this boundary.
enum class ProfileBootstrapSnapshotStatus : unsigned char {
	Resolved,
	InvalidProfileId,
	InvalidAuthorityGeneration,
	InvalidProfileDirectory,
	InvalidResourceIdentity,
};

struct ProfileBootstrapSnapshotResult;

//! Creates URI-only resource identities from a control-owned authority identity
//! and its already-resolved legacy directory. This performs neither I/O nor
//! directory creation and never substitutes a profile registry snapshot.
[[nodiscard]] ProfileBootstrapSnapshotResult ResolveProfileBootstrapSnapshot(
	std::string profileId,
	std::uint64_t authorityGeneration,
	std::wstring_view profileDirectory);

//! URI identities needed by the first platform consumers. These values are
//! deliberately not a public raw-path representation.
class ProfileBootstrapResourceUris final {
public:
	[[nodiscard]] const ::platform::uri::Uri& ProfileHome() const noexcept { return m_profileHome; }
	[[nodiscard]] const ::platform::uri::Uri& Settings() const noexcept { return m_settings; }
	[[nodiscard]] const ::platform::uri::Uri& Tasks() const noexcept { return m_tasks; }
	[[nodiscard]] const ::platform::uri::Uri& Keybindings() const noexcept { return m_keybindings; }
	[[nodiscard]] const ::platform::uri::Uri& Snippets() const noexcept { return m_snippets; }
	[[nodiscard]] const ::platform::uri::Uri& ExtensionsManifest() const noexcept { return m_extensionsManifest; }
	[[nodiscard]] const ::platform::uri::Uri& ExtensionsInstallHome() const noexcept { return m_extensionsInstallHome; }
	[[nodiscard]] const ::platform::uri::Uri& GlobalStorage() const noexcept { return m_globalStorage; }

private:
	ProfileBootstrapResourceUris(
		::platform::uri::Uri profileHome,
		::platform::uri::Uri settings,
		::platform::uri::Uri tasks,
		::platform::uri::Uri keybindings,
		::platform::uri::Uri snippets,
		::platform::uri::Uri extensionsManifest,
		::platform::uri::Uri extensionsInstallHome,
		::platform::uri::Uri globalStorage) noexcept;

	::platform::uri::Uri m_profileHome;
	::platform::uri::Uri m_settings;
	::platform::uri::Uri m_tasks;
	::platform::uri::Uri m_keybindings;
	::platform::uri::Uri m_snippets;
	::platform::uri::Uri m_extensionsManifest;
	::platform::uri::Uri m_extensionsInstallHome;
	::platform::uri::Uri m_globalStorage;

	friend class ProfileBootstrapSnapshot;
	friend ProfileBootstrapSnapshotResult ResolveProfileBootstrapSnapshot(
		std::string profileId,
		std::uint64_t authorityGeneration,
		std::wstring_view profileDirectory);
};

//! Immutable-by-value bridge result. It is intentionally not a user-data
//! profile descriptor: no registry has been read or persisted by this bridge.
class ProfileBootstrapSnapshot final {
public:
	[[nodiscard]] const std::string& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] std::uint64_t AuthorityGeneration() const noexcept { return m_authorityGeneration; }
	[[nodiscard]] ProfileBootstrapSnapshotSource Source() const noexcept { return ProfileBootstrapSnapshotSource::LegacyDirectoryBridge; }
	[[nodiscard]] std::uint64_t RegistryRevision() const noexcept { return 0; }
	[[nodiscard]] const ProfileBootstrapResourceUris& Resources() const noexcept { return m_resources; }

private:
	ProfileBootstrapSnapshot(
		std::string profileId,
		std::uint64_t authorityGeneration,
		ProfileBootstrapResourceUris resources) noexcept;

	std::string m_profileId;
	std::uint64_t m_authorityGeneration = 0;
	ProfileBootstrapResourceUris m_resources;

	friend ProfileBootstrapSnapshotResult ResolveProfileBootstrapSnapshot(
		std::string profileId,
		std::uint64_t authorityGeneration,
		std::wstring_view profileDirectory);
};

//! A snapshot is present if and only if status is Resolved.
struct ProfileBootstrapSnapshotResult {
	ProfileBootstrapSnapshotStatus status = ProfileBootstrapSnapshotStatus::InvalidProfileDirectory;
	std::optional<ProfileBootstrapSnapshot> snapshot;

	[[nodiscard]] bool Resolved() const noexcept
	{
		return status == ProfileBootstrapSnapshotStatus::Resolved && snapshot.has_value();
	}
};

} // namespace platform::profiles
