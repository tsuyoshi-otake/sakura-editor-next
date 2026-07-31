/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/profiles/UserDataProfileRegistry.h"

#include <algorithm>
#include <utility>

namespace platform::profiles {
namespace {

template<class PairVector, class Key>
auto FindAssociation(PairVector& associations, const Key& key)
{
	return std::find_if(associations.begin(), associations.end(), [&key](const auto& association) {
		return association.first == key;
	});
}

template<class PairVector>
auto FindWorkspaceAssociation(PairVector& associations, const WorkspaceUri& workspaceUri)
{
	return std::find_if(associations.begin(), associations.end(), [&workspaceUri](const auto& association) {
		return ::platform::uri::UriIdentityService::IsEqual(association.first, workspaceUri);
	});
}

bool HasDuplicateWorkspaceUri(const std::vector<WorkspaceUri>& workspaceUris)
{
	for (auto current = workspaceUris.begin(); current != workspaceUris.end(); ++current) {
		if (std::any_of(workspaceUris.begin(), current, [&current](const auto& existing) {
			return ::platform::uri::UriIdentityService::IsEqual(existing, *current);
		})) {
			return true;
		}
	}
	return false;
}

template<class StringVector>
bool HasEmptyOrDuplicate(const StringVector& values)
{
	for (auto current = values.begin(); current != values.end(); ++current) {
		if (current->empty() || std::find(values.begin(), current, *current) != current) {
			return true;
		}
	}
	return false;
}

bool IsCreatableKind(UserDataProfileKind kind) noexcept
{
	return kind == UserDataProfileKind::Normal || kind == UserDataProfileKind::Transient;
}

} // namespace

UserDataProfileRegistry::UserDataProfileRegistry()
	: m_defaultProfileId(L"default")
{
	m_profiles.push_back({ m_defaultProfileId, L"Default", UserDataProfileKind::Default, {}, {} });
}

std::uint64_t UserDataProfileRegistry::Revision() const noexcept
{
	return m_revision;
}

const UserDataProfileId& UserDataProfileRegistry::DefaultProfileId() const noexcept
{
	return m_defaultProfileId;
}

std::vector<UserDataProfileDescriptor> UserDataProfileRegistry::ListProfiles() const
{
	return m_profiles;
}

std::optional<UserDataProfileDescriptor> UserDataProfileRegistry::FindProfile(const UserDataProfileId& profileId) const
{
	const auto index = FindProfileIndex(profileId);
	return index ? std::optional<UserDataProfileDescriptor>(m_profiles[*index]) : std::nullopt;
}

std::optional<UserDataProfileId> UserDataProfileRegistry::FindProfileByLegacyAlias(const std::wstring& legacyAlias) const
{
	const auto found = FindAssociation(m_legacyAliases, legacyAlias);
	return found == m_legacyAliases.end() ? std::nullopt : std::optional<UserDataProfileId>(found->second);
}

std::optional<UserDataProfileId> UserDataProfileRegistry::FindProfileForWorkspace(const WorkspaceUri& workspaceUri) const
{
	const auto found = FindWorkspaceAssociation(m_workspaceAssociations, workspaceUri);
	return found == m_workspaceAssociations.end() ? std::nullopt : std::optional<UserDataProfileId>(found->second);
}

std::optional<UserDataProfileId> UserDataProfileRegistry::FindProfileForEmptyWindow(const EmptyWindowId& emptyWindowId) const
{
	const auto found = FindAssociation(m_emptyWindowAssociations, emptyWindowId);
	return found == m_emptyWindowAssociations.end() ? std::nullopt : std::optional<UserDataProfileId>(found->second);
}

UserDataProfileOperationResult UserDataProfileRegistry::Create(UserDataProfileCreateRequest request)
{
	if (request.profileId.empty() || request.displayName.empty()
		|| !IsCreatableKind(request.kind) || HasEmptyOrDuplicate(request.legacyAliases)) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument, request.profileId);
	}
	if (FindProfileIndex(request.profileId)) {
		return Complete(UserDataProfileOperationStatus::DuplicateProfileId, request.profileId);
	}
	if (std::any_of(m_profiles.begin(), m_profiles.end(), [&request](const auto& profile) {
		return profile.displayName == request.displayName;
	})) {
		return Complete(UserDataProfileOperationStatus::DuplicateDisplayName, request.profileId);
	}
	for (const auto& alias : request.legacyAliases) {
		if (FindAssociation(m_legacyAliases, alias) != m_legacyAliases.end()) {
			return Complete(UserDataProfileOperationStatus::DuplicateLegacyAlias, request.profileId);
		}
	}

	m_profiles.push_back({ request.profileId, std::move(request.displayName), request.kind,
		std::move(request.legacyAliases), request.resourceInheritance });
	for (const auto& alias : m_profiles.back().legacyAliases) {
		m_legacyAliases.emplace_back(alias, m_profiles.back().profileId);
	}
	return Complete(UserDataProfileOperationStatus::Applied, m_profiles.back().profileId);
}

UserDataProfileOperationResult UserDataProfileRegistry::Rename(const UserDataProfileId& profileId, std::wstring displayName)
{
	const auto index = FindProfileIndex(profileId);
	if (!index) {
		return Complete(UserDataProfileOperationStatus::ProfileNotFound, profileId);
	}
	if (profileId == m_defaultProfileId) {
		return Complete(UserDataProfileOperationStatus::DefaultProfileProtected, profileId);
	}
	if (displayName.empty()) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument, profileId);
	}
	if (m_profiles[*index].displayName == displayName) {
		return Complete(UserDataProfileOperationStatus::NoChange, profileId);
	}
	if (std::any_of(m_profiles.begin(), m_profiles.end(), [&profileId, &displayName](const auto& profile) {
		return profile.profileId != profileId && profile.displayName == displayName;
	})) {
		return Complete(UserDataProfileOperationStatus::DuplicateDisplayName, profileId);
	}
	m_profiles[*index].displayName = std::move(displayName);
	return Complete(UserDataProfileOperationStatus::Applied, profileId);
}

UserDataProfileOperationResult UserDataProfileRegistry::Remove(const UserDataProfileId& profileId)
{
	const auto index = FindProfileIndex(profileId);
	if (!index) {
		return Complete(UserDataProfileOperationStatus::ProfileNotFound, profileId);
	}
	if (profileId == m_defaultProfileId) {
		return Complete(UserDataProfileOperationStatus::DefaultProfileProtected, profileId);
	}
	m_profiles.erase(m_profiles.begin() + static_cast<std::ptrdiff_t>(*index));
	m_legacyAliases.erase(std::remove_if(m_legacyAliases.begin(), m_legacyAliases.end(), [&profileId](const auto& alias) {
		return alias.second == profileId;
	}), m_legacyAliases.end());
	m_workspaceAssociations.erase(std::remove_if(m_workspaceAssociations.begin(), m_workspaceAssociations.end(), [&profileId](const auto& association) {
		return association.second == profileId;
	}), m_workspaceAssociations.end());
	m_emptyWindowAssociations.erase(std::remove_if(m_emptyWindowAssociations.begin(), m_emptyWindowAssociations.end(), [&profileId](const auto& association) {
		return association.second == profileId;
	}), m_emptyWindowAssociations.end());
	const auto removedTransientProfiles = EraseUnassociatedTransientProfiles();
	return Complete(UserDataProfileOperationStatus::Applied, profileId, removedTransientProfiles);
}

UserDataProfileOperationResult UserDataProfileRegistry::SetLegacyAliases(
	const UserDataProfileId& profileId, std::vector<std::wstring> legacyAliases)
{
	const auto index = FindProfileIndex(profileId);
	if (!index) {
		return Complete(UserDataProfileOperationStatus::ProfileNotFound, profileId);
	}
	if (HasEmptyOrDuplicate(legacyAliases)) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument, profileId);
	}
	if (m_profiles[*index].legacyAliases == legacyAliases) {
		return Complete(UserDataProfileOperationStatus::NoChange, profileId);
	}
	for (const auto& alias : legacyAliases) {
		const auto found = FindAssociation(m_legacyAliases, alias);
		if (found != m_legacyAliases.end() && found->second != profileId) {
			return Complete(UserDataProfileOperationStatus::DuplicateLegacyAlias, profileId);
		}
	}
	m_legacyAliases.erase(std::remove_if(m_legacyAliases.begin(), m_legacyAliases.end(), [&profileId](const auto& alias) {
		return alias.second == profileId;
	}), m_legacyAliases.end());
	m_profiles[*index].legacyAliases = std::move(legacyAliases);
	for (const auto& alias : m_profiles[*index].legacyAliases) {
		m_legacyAliases.emplace_back(alias, profileId);
	}
	return Complete(UserDataProfileOperationStatus::Applied, profileId);
}

UserDataProfileOperationResult UserDataProfileRegistry::AssociateWorkspace(
	const UserDataProfileId& profileId, WorkspaceUri workspaceUri)
{
	if (!FindProfileIndex(profileId)) {
		return Complete(UserDataProfileOperationStatus::ProfileNotFound, profileId);
	}
	const auto found = FindWorkspaceAssociation(m_workspaceAssociations, workspaceUri);
	if (found != m_workspaceAssociations.end() && found->second == profileId) {
		return Complete(UserDataProfileOperationStatus::NoChange, profileId);
	}
	if (found == m_workspaceAssociations.end()) {
		m_workspaceAssociations.emplace_back(std::move(workspaceUri), profileId);
	}
	else {
		found->second = profileId;
	}
	return Complete(UserDataProfileOperationStatus::Applied, profileId, EraseUnassociatedTransientProfiles());
}

UserDataProfileOperationResult UserDataProfileRegistry::AssociateEmptyWindow(
	const UserDataProfileId& profileId, EmptyWindowId emptyWindowId)
{
	if (emptyWindowId.empty()) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument, profileId);
	}
	if (!FindProfileIndex(profileId)) {
		return Complete(UserDataProfileOperationStatus::ProfileNotFound, profileId);
	}
	const auto found = FindAssociation(m_emptyWindowAssociations, emptyWindowId);
	if (found != m_emptyWindowAssociations.end() && found->second == profileId) {
		return Complete(UserDataProfileOperationStatus::NoChange, profileId);
	}
	if (found == m_emptyWindowAssociations.end()) {
		m_emptyWindowAssociations.emplace_back(std::move(emptyWindowId), profileId);
	}
	else {
		found->second = profileId;
	}
	return Complete(UserDataProfileOperationStatus::Applied, profileId, EraseUnassociatedTransientProfiles());
}

UserDataProfileOperationResult UserDataProfileRegistry::DisassociateWorkspace(const WorkspaceUri& workspaceUri)
{
	const auto found = FindWorkspaceAssociation(m_workspaceAssociations, workspaceUri);
	if (found == m_workspaceAssociations.end()) {
		return Complete(UserDataProfileOperationStatus::AssociationNotFound);
	}
	const auto profileId = found->second;
	m_workspaceAssociations.erase(found);
	return Complete(UserDataProfileOperationStatus::Applied, profileId, EraseUnassociatedTransientProfiles());
}

UserDataProfileOperationResult UserDataProfileRegistry::DisassociateEmptyWindow(const EmptyWindowId& emptyWindowId)
{
	if (emptyWindowId.empty()) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument);
	}
	const auto found = FindAssociation(m_emptyWindowAssociations, emptyWindowId);
	if (found == m_emptyWindowAssociations.end()) {
		return Complete(UserDataProfileOperationStatus::AssociationNotFound);
	}
	const auto profileId = found->second;
	m_emptyWindowAssociations.erase(found);
	return Complete(UserDataProfileOperationStatus::Applied, profileId, EraseUnassociatedTransientProfiles());
}

UserDataProfileOperationResult UserDataProfileRegistry::ApplyLegacyMigration(LegacyUserDataProfileMigrationRequest request)
{
	const auto& profile = request.profile;
	if (profile.profileId.empty() || profile.displayName.empty() || !IsCreatableKind(profile.kind)
		|| HasEmptyOrDuplicate(profile.legacyAliases) || HasDuplicateWorkspaceUri(request.workspaceUris)
		|| HasEmptyOrDuplicate(request.emptyWindowIds)) {
		return Complete(UserDataProfileOperationStatus::InvalidArgument, profile.profileId);
	}
	const auto profileIndex = FindProfileIndex(profile.profileId);
	if (!profileIndex && std::any_of(m_profiles.begin(), m_profiles.end(), [&profile](const auto& existing) {
		return existing.displayName == profile.displayName;
	})) {
		return Complete(UserDataProfileOperationStatus::DuplicateDisplayName, profile.profileId);
	}
	for (const auto& alias : profile.legacyAliases) {
		const auto found = FindAssociation(m_legacyAliases, alias);
		if (found != m_legacyAliases.end() && found->second != profile.profileId) {
			return Complete(UserDataProfileOperationStatus::DuplicateLegacyAlias, profile.profileId);
		}
	}
	for (const auto& workspaceUri : request.workspaceUris) {
		const auto found = FindWorkspaceAssociation(m_workspaceAssociations, workspaceUri);
		if (found != m_workspaceAssociations.end() && found->second != profile.profileId) {
			return Complete(UserDataProfileOperationStatus::AssociationConflict, profile.profileId);
		}
	}
	for (const auto& emptyWindowId : request.emptyWindowIds) {
		const auto found = FindAssociation(m_emptyWindowAssociations, emptyWindowId);
		if (found != m_emptyWindowAssociations.end() && found->second != profile.profileId) {
			return Complete(UserDataProfileOperationStatus::AssociationConflict, profile.profileId);
		}
	}

	bool changed = false;
	if (!profileIndex) {
		m_profiles.push_back({ profile.profileId, profile.displayName, profile.kind, {}, profile.resourceInheritance });
		changed = true;
	}
	const auto currentIndex = FindProfileIndex(profile.profileId);
	for (const auto& alias : profile.legacyAliases) {
		if (FindAssociation(m_legacyAliases, alias) == m_legacyAliases.end()) {
			m_profiles[*currentIndex].legacyAliases.emplace_back(alias);
			m_legacyAliases.emplace_back(alias, profile.profileId);
			changed = true;
		}
	}
	for (const auto& workspaceUri : request.workspaceUris) {
		if (FindWorkspaceAssociation(m_workspaceAssociations, workspaceUri) == m_workspaceAssociations.end()) {
			m_workspaceAssociations.emplace_back(workspaceUri, profile.profileId);
			changed = true;
		}
	}
	for (const auto& emptyWindowId : request.emptyWindowIds) {
		if (FindAssociation(m_emptyWindowAssociations, emptyWindowId) == m_emptyWindowAssociations.end()) {
			m_emptyWindowAssociations.emplace_back(emptyWindowId, profile.profileId);
			changed = true;
		}
	}
	return Complete(changed ? UserDataProfileOperationStatus::Applied : UserDataProfileOperationStatus::NoChange, profile.profileId);
}

UserDataProfileOperationResult UserDataProfileRegistry::CleanupTransientProfiles()
{
	const auto removedTransientProfiles = EraseUnassociatedTransientProfiles();
	return Complete(removedTransientProfiles == 0 ? UserDataProfileOperationStatus::NoChange : UserDataProfileOperationStatus::Applied,
		{}, removedTransientProfiles);
}

UserDataProfileResolveResult UserDataProfileRegistry::Resolve(const UserDataProfileResolveRequest& request) const
{
	auto resolved = [this](const UserDataProfileId& profileId, UserDataProfileResolveSource source) {
		const auto profile = FindProfile(profileId);
		return UserDataProfileResolveResult{ profile ? UserDataProfileResolveStatus::Resolved : UserDataProfileResolveStatus::ExplicitProfileNotFound,
			m_revision, source, profile };
	};
	if (request.explicitProfileId) {
		return resolved(*request.explicitProfileId, UserDataProfileResolveSource::ExplicitProfile);
	}
	if (request.workspaceUri) {
		if (const auto profileId = FindProfileForWorkspace(*request.workspaceUri)) {
			return resolved(*profileId, UserDataProfileResolveSource::WorkspaceAssociation);
		}
	}
	if (request.emptyWindowId) {
		if (const auto profileId = FindProfileForEmptyWindow(*request.emptyWindowId)) {
			return resolved(*profileId, UserDataProfileResolveSource::EmptyWindowAssociation);
		}
	}
	return resolved(m_defaultProfileId, UserDataProfileResolveSource::DefaultProfile);
}

UserDataProfileRegistrySnapshot UserDataProfileRegistry::Snapshot(bool includeTransientProfiles) const
{
	UserDataProfileRegistrySnapshot snapshot;
	snapshot.revision = m_revision;
	snapshot.defaultProfileId = m_defaultProfileId;
	for (const auto& profile : m_profiles) {
		if (includeTransientProfiles || profile.kind != UserDataProfileKind::Transient) {
			snapshot.profiles.push_back(profile);
		}
	}
	const auto isPersisted = [&snapshot](const UserDataProfileId& profileId) {
		return std::any_of(snapshot.profiles.begin(), snapshot.profiles.end(), [&profileId](const auto& profile) {
			return profile.profileId == profileId;
		});
	};
	for (const auto& association : m_workspaceAssociations) {
		if (isPersisted(association.second)) snapshot.workspaceAssociations.push_back(association);
	}
	for (const auto& association : m_emptyWindowAssociations) {
		if (isPersisted(association.second)) snapshot.emptyWindowAssociations.push_back(association);
	}
	return snapshot;
}

UserDataProfileSnapshotStatus UserDataProfileRegistry::ReplaceSnapshot(UserDataProfileRegistrySnapshot snapshot)
{
	if (!IsValidSnapshot(snapshot)) return UserDataProfileSnapshotStatus::Invalid;

	std::vector<std::pair<std::wstring, UserDataProfileId>> aliases;
	for (const auto& profile : snapshot.profiles) {
		for (const auto& alias : profile.legacyAliases) aliases.emplace_back(alias, profile.profileId);
	}
	m_revision = snapshot.revision;
	m_defaultProfileId = std::move(snapshot.defaultProfileId);
	m_profiles = std::move(snapshot.profiles);
	m_legacyAliases = std::move(aliases);
	m_workspaceAssociations = std::move(snapshot.workspaceAssociations);
	m_emptyWindowAssociations = std::move(snapshot.emptyWindowAssociations);
	return UserDataProfileSnapshotStatus::Applied;
}

std::optional<UserDataProfileRegistry::ProfileIndex> UserDataProfileRegistry::FindProfileIndex(const UserDataProfileId& profileId) const
{
	const auto found = std::find_if(m_profiles.begin(), m_profiles.end(), [&profileId](const auto& profile) {
		return profile.profileId == profileId;
	});
	if (found == m_profiles.end()) {
		return std::nullopt;
	}
	return static_cast<ProfileIndex>(std::distance(m_profiles.begin(), found));
}

bool UserDataProfileRegistry::HasAssociation(const UserDataProfileId& profileId) const
{
	const auto isAssociated = [&profileId](const auto& association) { return association.second == profileId; };
	return std::any_of(m_workspaceAssociations.begin(), m_workspaceAssociations.end(), isAssociated)
		|| std::any_of(m_emptyWindowAssociations.begin(), m_emptyWindowAssociations.end(), isAssociated);
}

std::size_t UserDataProfileRegistry::EraseUnassociatedTransientProfiles()
{
	std::vector<UserDataProfileId> removedProfileIds;
	for (const auto& profile : m_profiles) {
		if (profile.kind == UserDataProfileKind::Transient && !HasAssociation(profile.profileId)) {
			removedProfileIds.emplace_back(profile.profileId);
		}
	}
	for (const auto& profileId : removedProfileIds) {
		const auto index = FindProfileIndex(profileId);
		if (index) {
			m_profiles.erase(m_profiles.begin() + static_cast<std::ptrdiff_t>(*index));
		}
		m_legacyAliases.erase(std::remove_if(m_legacyAliases.begin(), m_legacyAliases.end(), [&profileId](const auto& alias) {
			return alias.second == profileId;
		}), m_legacyAliases.end());
	}
	return removedProfileIds.size();
}

bool UserDataProfileRegistry::IsValidSnapshot(const UserDataProfileRegistrySnapshot& snapshot) const
{
	if (snapshot.defaultProfileId.empty() || snapshot.profiles.empty() || snapshot.profiles.size() > 256
		|| snapshot.workspaceAssociations.size() > 1024 || snapshot.emptyWindowAssociations.size() > 1024) return false;
	std::vector<UserDataProfileId> profileIds;
	std::vector<std::wstring> displayNames;
	std::vector<std::wstring> aliases;
	bool foundDefault = false;
	for (const auto& profile : snapshot.profiles) {
		if (profile.profileId.empty() || profile.displayName.empty() || profile.profileId.size() > 256
			|| profile.displayName.size() > 1024 || HasEmptyOrDuplicate(profile.legacyAliases)) return false;
		if (std::find(profileIds.begin(), profileIds.end(), profile.profileId) != profileIds.end()
			|| std::find(displayNames.begin(), displayNames.end(), profile.displayName) != displayNames.end()) return false;
		profileIds.push_back(profile.profileId);
		displayNames.push_back(profile.displayName);
		if (profile.profileId == snapshot.defaultProfileId) {
			if (profile.kind != UserDataProfileKind::Default || foundDefault) return false;
			foundDefault = true;
		} else if (profile.kind == UserDataProfileKind::Default) return false;
		for (const auto& alias : profile.legacyAliases) {
			if (std::find(aliases.begin(), aliases.end(), alias) != aliases.end()) return false;
			aliases.push_back(alias);
		}
	}
	if (!foundDefault) return false;
	if (HasDuplicateWorkspaceUri([&snapshot] {
		std::vector<WorkspaceUri> result;
		result.reserve(snapshot.workspaceAssociations.size());
		for (const auto& association : snapshot.workspaceAssociations) result.push_back(association.first);
		return result;
	}())) return false;
	std::vector<EmptyWindowId> emptyWindowIds;
	for (const auto& association : snapshot.workspaceAssociations) {
		if (association.second.empty() || std::find(profileIds.begin(), profileIds.end(), association.second) == profileIds.end()) return false;
	}
	for (const auto& association : snapshot.emptyWindowAssociations) {
		if (association.first.empty() || association.second.empty()
			|| std::find(profileIds.begin(), profileIds.end(), association.second) == profileIds.end()
			|| std::find(emptyWindowIds.begin(), emptyWindowIds.end(), association.first) != emptyWindowIds.end()) return false;
		emptyWindowIds.push_back(association.first);
	}
	return true;
}

bool UserDataProfileRegistrySnapshot::operator==(const UserDataProfileRegistrySnapshot& other) const
{
	if (revision != other.revision || defaultProfileId != other.defaultProfileId || profiles != other.profiles
		|| workspaceAssociations.size() != other.workspaceAssociations.size()
		|| emptyWindowAssociations != other.emptyWindowAssociations) return false;
	for (std::size_t index = 0; index < workspaceAssociations.size(); ++index) {
		const auto& left = workspaceAssociations[index];
		const auto& right = other.workspaceAssociations[index];
		if (left.second != right.second || !::platform::uri::UriIdentityService::IsEqual(left.first, right.first)) return false;
	}
	return true;
}

UserDataProfileOperationResult UserDataProfileRegistry::Complete(
	UserDataProfileOperationStatus status, const UserDataProfileId& profileId, std::size_t removedTransientProfiles)
{
	if (status == UserDataProfileOperationStatus::Applied) {
		++m_revision;
	}
	return { status, m_revision, profileId, removedTransientProfiles };
}

} // namespace platform::profiles
