/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/uri/UriIdentity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace platform::profiles {

//! Durable profile identity.  The registry never changes an existing value.
using UserDataProfileId = std::wstring;
//! Workspace identity is a URI value, never a display path.  Comparisons are
//! delegated to UriIdentityService so Windows file URI aliases converge.
using WorkspaceUri = ::platform::uri::Uri;
using EmptyWindowId = std::wstring;

enum class UserDataProfileKind : unsigned char {
	Default,
	Normal,
	Transient,
};

//! Controls which resources a profile reads from its parent/default profile.
//! The flags describe policy only; resource I/O belongs to the profile storage adapter.
struct UserDataProfileResourceInheritance {
	bool settings = false;
	bool keybindings = false;
	bool tasks = false;
	bool snippets = false;
	bool extensions = false;
	bool globalState = false;

	constexpr bool operator==(const UserDataProfileResourceInheritance&) const = default;
};

//! An immutable snapshot.  Mutating a returned value does not mutate the registry.
struct UserDataProfileDescriptor {
	UserDataProfileId profileId;
	std::wstring displayName;
	UserDataProfileKind kind = UserDataProfileKind::Normal;
	std::vector<std::wstring> legacyAliases;
	UserDataProfileResourceInheritance resourceInheritance;

	bool operator==(const UserDataProfileDescriptor&) const = default;
};

enum class UserDataProfileOperationStatus : unsigned char {
	Applied,
	NoChange,
	InvalidArgument,
	ProfileNotFound,
	DuplicateProfileId,
	DuplicateDisplayName,
	DuplicateLegacyAlias,
	DefaultProfileProtected,
	AssociationNotFound,
	AssociationConflict,
};

//! Every mutating request reaches an explicit terminal state.  A successful
//! no-op deliberately retains the current revision.
struct UserDataProfileOperationResult {
	UserDataProfileOperationStatus status = UserDataProfileOperationStatus::InvalidArgument;
	std::uint64_t revision = 0;
	UserDataProfileId profileId;
	std::size_t removedTransientProfiles = 0;

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return status == UserDataProfileOperationStatus::Applied
			|| status == UserDataProfileOperationStatus::NoChange;
	}

	[[nodiscard]] constexpr bool Changed() const noexcept
	{
		return status == UserDataProfileOperationStatus::Applied;
	}
};

struct UserDataProfileCreateRequest {
	UserDataProfileId profileId;
	std::wstring displayName;
	UserDataProfileKind kind = UserDataProfileKind::Normal;
	std::vector<std::wstring> legacyAliases;
	UserDataProfileResourceInheritance resourceInheritance;
};

//! A legacy import only adds aliases and missing associations to an existing
//! profile.  It never overwrites a user-selected display name or association.
struct LegacyUserDataProfileMigrationRequest {
	UserDataProfileCreateRequest profile;
	std::vector<WorkspaceUri> workspaceUris;
	std::vector<EmptyWindowId> emptyWindowIds;
};

enum class UserDataProfileResolveSource : unsigned char {
	ExplicitProfile,
	WorkspaceAssociation,
	EmptyWindowAssociation,
	DefaultProfile,
};

enum class UserDataProfileResolveStatus : unsigned char {
	Resolved,
	ExplicitProfileNotFound,
};

struct UserDataProfileResolveRequest {
	std::optional<UserDataProfileId> explicitProfileId;
	std::optional<WorkspaceUri> workspaceUri;
	std::optional<EmptyWindowId> emptyWindowId;
};

struct UserDataProfileResolveResult {
	UserDataProfileResolveStatus status = UserDataProfileResolveStatus::ExplicitProfileNotFound;
	std::uint64_t revision = 0;
	UserDataProfileResolveSource source = UserDataProfileResolveSource::DefaultProfile;
	std::optional<UserDataProfileDescriptor> profile;

	[[nodiscard]] bool Resolved() const noexcept
	{
		return status == UserDataProfileResolveStatus::Resolved && profile.has_value();
	}
};

//! Complete registry state used only at the control-owned durable boundary.
//! Values are copies; they never grant mutable access to the registry.
struct UserDataProfileRegistrySnapshot {
	std::uint64_t revision = 0;
	UserDataProfileId defaultProfileId;
	std::vector<UserDataProfileDescriptor> profiles;
	std::vector<std::pair<WorkspaceUri, UserDataProfileId>> workspaceAssociations;
	std::vector<std::pair<EmptyWindowId, UserDataProfileId>> emptyWindowAssociations;

	bool operator==(const UserDataProfileRegistrySnapshot& other) const;
};

enum class UserDataProfileSnapshotStatus : unsigned char {
	Applied,
	Invalid,
};

//! Control-process-owned, in-memory profile registry.  This class intentionally
//! has no Win32, JSON, INI, filesystem, or UI dependency.  A storage adapter owns
//! persistence and serializes mutations from editor processes before calling it.
class UserDataProfileRegistry final {
public:
	UserDataProfileRegistry();
	~UserDataProfileRegistry() = default;
	UserDataProfileRegistry(const UserDataProfileRegistry&) = delete;
	UserDataProfileRegistry& operator=(const UserDataProfileRegistry&) = delete;

	[[nodiscard]] std::uint64_t Revision() const noexcept;
	[[nodiscard]] const UserDataProfileId& DefaultProfileId() const noexcept;
	[[nodiscard]] std::vector<UserDataProfileDescriptor> ListProfiles() const;
	[[nodiscard]] std::optional<UserDataProfileDescriptor> FindProfile(const UserDataProfileId& profileId) const;
	[[nodiscard]] std::optional<UserDataProfileId> FindProfileByLegacyAlias(const std::wstring& legacyAlias) const;
	[[nodiscard]] std::optional<UserDataProfileId> FindProfileForWorkspace(const WorkspaceUri& workspaceUri) const;
	[[nodiscard]] std::optional<UserDataProfileId> FindProfileForEmptyWindow(const EmptyWindowId& emptyWindowId) const;

	[[nodiscard]] UserDataProfileOperationResult Create(UserDataProfileCreateRequest request);
	[[nodiscard]] UserDataProfileOperationResult Rename(const UserDataProfileId& profileId, std::wstring displayName);
	[[nodiscard]] UserDataProfileOperationResult Remove(const UserDataProfileId& profileId);
	[[nodiscard]] UserDataProfileOperationResult SetLegacyAliases(
		const UserDataProfileId& profileId, std::vector<std::wstring> legacyAliases);
	[[nodiscard]] UserDataProfileOperationResult AssociateWorkspace(
		const UserDataProfileId& profileId, WorkspaceUri workspaceUri);
	[[nodiscard]] UserDataProfileOperationResult AssociateEmptyWindow(
		const UserDataProfileId& profileId, EmptyWindowId emptyWindowId);
	[[nodiscard]] UserDataProfileOperationResult DisassociateWorkspace(const WorkspaceUri& workspaceUri);
	[[nodiscard]] UserDataProfileOperationResult DisassociateEmptyWindow(const EmptyWindowId& emptyWindowId);
	[[nodiscard]] UserDataProfileOperationResult ApplyLegacyMigration(LegacyUserDataProfileMigrationRequest request);
	[[nodiscard]] UserDataProfileOperationResult CleanupTransientProfiles();
	[[nodiscard]] UserDataProfileResolveResult Resolve(const UserDataProfileResolveRequest& request) const;
	//! Produces a durable/portable view. Transient profiles and their associations
	//! are deliberately absent unless a control-only rollback snapshot requests them.
	[[nodiscard]] UserDataProfileRegistrySnapshot Snapshot(bool includeTransientProfiles = false) const;
	//! Validates a whole replacement before changing any observable registry state.
	[[nodiscard]] UserDataProfileSnapshotStatus ReplaceSnapshot(UserDataProfileRegistrySnapshot snapshot);

private:
	using ProfileIndex = std::size_t;

	[[nodiscard]] std::optional<ProfileIndex> FindProfileIndex(const UserDataProfileId& profileId) const;
	[[nodiscard]] bool HasAssociation(const UserDataProfileId& profileId) const;
	[[nodiscard]] std::size_t EraseUnassociatedTransientProfiles();
	[[nodiscard]] bool IsValidSnapshot(const UserDataProfileRegistrySnapshot& snapshot) const;
	[[nodiscard]] UserDataProfileOperationResult Complete(
		UserDataProfileOperationStatus status, const UserDataProfileId& profileId = {},
		std::size_t removedTransientProfiles = 0);

	std::uint64_t m_revision = 0;
	UserDataProfileId m_defaultProfileId;
	std::vector<UserDataProfileDescriptor> m_profiles;
	std::vector<std::pair<std::wstring, UserDataProfileId>> m_legacyAliases;
	std::vector<std::pair<WorkspaceUri, UserDataProfileId>> m_workspaceAssociations;
	std::vector<std::pair<EmptyWindowId, UserDataProfileId>> m_emptyWindowAssociations;
};

} // namespace platform::profiles
