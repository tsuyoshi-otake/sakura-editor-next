/*! @file
	@brief Pure selected-user-data-profile bootstrap and resource locator contract.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ProfileAuthorityIdentity.h>
#include "platform/profiles/UserDataProfileRegistry.h"
#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platform::profiles {

struct UserDataProfileBootstrapRequest;
struct UserDataProfileBootstrapResult;

//! Pinned identity of the control endpoint that supplied a registry snapshot.
//! This is intentionally separate from the selected user-data profile identity.
struct ControlProfileAuthorityIdentity {
	std::string authorityId;
	std::uint64_t generation = 0;

	constexpr bool operator==(const ControlProfileAuthorityIdentity&) const = default;
};

//! Whether the selected profile uses its own opaque-ID namespace or the explicit
//! legacy default-root compatibility bridge. Normal and transient profiles must
//! always use the opaque-ID namespace.
enum class UserDataProfileResourceRootMode : unsigned char {
	ProfileIdNamespace,
	LegacyControlRootForDefault,
};

//! Every bootstrap request reaches a typed terminal result. Diagnostics do not
//! retain local paths, authority IDs, or user-selected display names.
enum class UserDataProfileBootstrapStatus : unsigned char {
	Resolved,
	InvalidControlAuthorityId,
	InvalidControlAuthorityGeneration,
	InvalidControlProfileRoot,
	InvalidRegistryDocument,
	InvalidRegistrySnapshot,
	InvalidProfileId,
	ProfileNotFound,
	InvalidEmptyWindowIdentity,
	InvalidResourceRootMode,
	InvalidResourceIdentity,
};

//! A stable empty-window token must be ``empty-window:`` followed by 8--128
//! ASCII letters, digits, ``-``, or ``_``. It must be durable across editor
//! process restarts; process-derived tokens such as ``editor-process:1234``
//! are rejected before profile selection.
[[nodiscard]] bool IsStableEmptyWindowIdentity(const EmptyWindowId& value) noexcept;

//! URI-only identities for resources belonging to one selected profile. There
//! is deliberately no secret URI or secret-content accessor: secret material
//! remains behind the vault capability boundary.
class UserDataProfileResourceUris final {
public:
	[[nodiscard]] const ::platform::uri::Uri& ProfileHome() const noexcept { return m_profileHome; }
	[[nodiscard]] const ::platform::uri::Uri& Settings() const noexcept { return m_settings; }
	[[nodiscard]] const ::platform::uri::Uri& Keybindings() const noexcept { return m_keybindings; }
	[[nodiscard]] const ::platform::uri::Uri& Snippets() const noexcept { return m_snippets; }
	[[nodiscard]] const ::platform::uri::Uri& Tasks() const noexcept { return m_tasks; }
	[[nodiscard]] const ::platform::uri::Uri& ExtensionsSelection() const noexcept { return m_extensionsSelection; }
	[[nodiscard]] const ::platform::uri::Uri& GlobalState() const noexcept { return m_globalState; }
	[[nodiscard]] const ::platform::uri::Uri& WorkingCopies() const noexcept { return m_workingCopies; }
	[[nodiscard]] const ::platform::uri::Uri& WorkbenchLayout() const noexcept { return m_workbenchLayout; }

private:
	UserDataProfileResourceUris(
		::platform::uri::Uri profileHome,
		::platform::uri::Uri settings,
		::platform::uri::Uri keybindings,
		::platform::uri::Uri snippets,
		::platform::uri::Uri tasks,
		::platform::uri::Uri extensionsSelection,
		::platform::uri::Uri globalState,
		::platform::uri::Uri workingCopies,
		::platform::uri::Uri workbenchLayout) noexcept;

	::platform::uri::Uri m_profileHome;
	::platform::uri::Uri m_settings;
	::platform::uri::Uri m_keybindings;
	::platform::uri::Uri m_snippets;
	::platform::uri::Uri m_tasks;
	::platform::uri::Uri m_extensionsSelection;
	::platform::uri::Uri m_globalState;
	::platform::uri::Uri m_workingCopies;
	::platform::uri::Uri m_workbenchLayout;

	friend class UserDataProfileBootstrapSnapshot;
	friend UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
		const UserDataProfileBootstrapRequest& request,
		const UserDataProfileRegistrySnapshot& registrySnapshot);
};

//! Pure input. ``controlProfileRoot`` is a normalized absolute Windows
//! directory already selected by the control authority; this locator neither
//! reads nor creates it. The named profile root is derived from profileId, not
//! from UserDataProfileDescriptor::displayName.
struct UserDataProfileBootstrapRequest {
	ControlProfileAuthorityIdentity controlAuthority;
	std::wstring controlProfileRoot;
	UserDataProfileResolveRequest selection;
	UserDataProfileResourceRootMode resourceRootMode = UserDataProfileResourceRootMode::ProfileIdNamespace;
};

//! Immutable-by-value composition input. The pinned authority stays distinct
//! from selectedProfile even when their textual values happen to match.
class UserDataProfileBootstrapSnapshot final {
public:
	[[nodiscard]] const ControlProfileAuthorityIdentity& ControlAuthority() const noexcept { return m_controlAuthority; }
	[[nodiscard]] std::uint64_t RegistryRevision() const noexcept { return m_registryRevision; }
	[[nodiscard]] UserDataProfileResolveSource SelectionSource() const noexcept { return m_selectionSource; }
	[[nodiscard]] const UserDataProfileDescriptor& SelectedProfile() const noexcept { return m_selectedProfile; }
	[[nodiscard]] const UserDataProfileId& SelectedProfileId() const noexcept { return m_selectedProfile.profileId; }
	[[nodiscard]] const UserDataProfileResourceUris& Resources() const noexcept { return m_resources; }

private:
	UserDataProfileBootstrapSnapshot(
		ControlProfileAuthorityIdentity controlAuthority,
		std::uint64_t registryRevision,
		UserDataProfileResolveSource selectionSource,
		UserDataProfileDescriptor selectedProfile,
		UserDataProfileResourceUris resources) noexcept;

	ControlProfileAuthorityIdentity m_controlAuthority;
	std::uint64_t m_registryRevision = 0;
	UserDataProfileResolveSource m_selectionSource = UserDataProfileResolveSource::DefaultProfile;
	UserDataProfileDescriptor m_selectedProfile;
	UserDataProfileResourceUris m_resources;

	friend UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
		const UserDataProfileBootstrapRequest& request,
		const UserDataProfileRegistrySnapshot& registrySnapshot);
};

struct UserDataProfileBootstrapResult {
	UserDataProfileBootstrapStatus status = UserDataProfileBootstrapStatus::InvalidRegistrySnapshot;
	std::optional<UserDataProfileBootstrapSnapshot> snapshot;

	[[nodiscard]] bool Resolved() const noexcept
	{
		return status == UserDataProfileBootstrapStatus::Resolved && snapshot.has_value();
	}
};

//! Resolves against a frozen registry snapshot. The snapshot is validated again
//! at this trust boundary, so malformed imported state fails closed.
[[nodiscard]] UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request,
	const UserDataProfileRegistrySnapshot& registrySnapshot);

//! Decodes one bounded immutable registry document obtained from the
//! generation-pinned control Profile RPC, then resolves it through the same
//! fail-closed snapshot boundary. The document contains metadata only.
[[nodiscard]] UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request,
	std::string_view registryDocument);

//! Captures a transient-inclusive value snapshot and resolves it without I/O.
[[nodiscard]] UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request,
	const UserDataProfileRegistry& registry);

} // namespace platform::profiles
