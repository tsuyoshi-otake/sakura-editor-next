/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/profiles/UserDataProfileBootstrap.h"

#include "platform/profiles/UserDataProfileRegistryCodec.h"

#include <algorithm>
#include <utility>

namespace platform::profiles {
namespace {

constexpr std::size_t kMaximumControlProfileRootCharacters = 4096;
constexpr std::size_t kMaximumProfileIdCharacters = 128;
constexpr std::size_t kMaximumProfiles = 256;
constexpr std::size_t kMaximumAssociations = 1024;

bool IsSeparator(wchar_t character) noexcept
{
	return character == L'\\' || character == L'/';
}

bool IsDotComponent(std::wstring_view path) noexcept
{
	std::size_t componentStart = 0;
	while (componentStart < path.size()) {
		while (componentStart < path.size() && IsSeparator(path[componentStart])) ++componentStart;
		const auto componentEnd = path.find_first_of(L"\\/", componentStart);
		const auto component = path.substr(componentStart, componentEnd - componentStart);
		if (component == L"." || component == L"..") return true;
		if (componentEnd == std::wstring_view::npos) break;
		componentStart = componentEnd + 1;
	}
	return false;
}

bool IsNormalizedAbsoluteWindowsDirectory(std::wstring_view directory) noexcept
{
	if (directory.empty() || directory.size() > kMaximumControlProfileRootCharacters || IsDotComponent(directory)) return false;
	if (directory.size() >= 4 && directory[0] == L'\\' && directory[1] == L'\\'
		&& (directory[2] == L'?' || directory[2] == L'.') && IsSeparator(directory[3])) return false;

	const bool drivePath = directory.size() >= 3
		&& ((directory[0] >= L'A' && directory[0] <= L'Z') || (directory[0] >= L'a' && directory[0] <= L'z'))
		&& directory[1] == L':' && IsSeparator(directory[2]);
	const bool uncPath = directory.size() > 2 && directory[0] == L'\\' && directory[1] == L'\\';
	if (!drivePath && !uncPath) return false;
	for (std::size_t index = drivePath ? 3 : 2; index < directory.size(); ++index) {
		if (IsSeparator(directory[index]) && index + 1 < directory.size() && IsSeparator(directory[index + 1])) return false;
	}
	return true;
}

bool IsOpaqueProfileId(const UserDataProfileId& value) noexcept
{
	if (value.empty() || value.size() > kMaximumProfileIdCharacters) return false;
	for (const wchar_t character : value) {
		if (!((character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')
			|| (character >= L'0' && character <= L'9') || character == L'-' || character == L'_')) return false;
	}
	return true;
}

bool IsKnownProfileKind(UserDataProfileKind kind) noexcept
{
	return kind == UserDataProfileKind::Default || kind == UserDataProfileKind::Normal || kind == UserDataProfileKind::Transient;
}

std::wstring JoinWindowsPath(std::wstring_view directory, std::wstring_view child)
{
	std::wstring result(directory);
	if (!result.empty() && !IsSeparator(result.back())) result.push_back(L'\\');
	result.append(child);
	return result;
}

UserDataProfileBootstrapResult Failed(UserDataProfileBootstrapStatus status)
{
	return { status, std::nullopt };
}

bool IsValidSnapshot(const UserDataProfileRegistrySnapshot& snapshot) noexcept
{
	if (!IsOpaqueProfileId(snapshot.defaultProfileId) || snapshot.profiles.empty() || snapshot.profiles.size() > kMaximumProfiles
		|| snapshot.workspaceAssociations.size() > kMaximumAssociations || snapshot.emptyWindowAssociations.size() > kMaximumAssociations) return false;

	bool foundDefault = false;
	for (auto current = snapshot.profiles.begin(); current != snapshot.profiles.end(); ++current) {
		if (!IsOpaqueProfileId(current->profileId) || current->displayName.empty() || current->displayName.size() > 1024
			|| !IsKnownProfileKind(current->kind)) return false;
		if (std::any_of(snapshot.profiles.begin(), current, [&current](const auto& existing) {
			return existing.profileId == current->profileId || existing.displayName == current->displayName;
		})) return false;
		if (current->profileId == snapshot.defaultProfileId) {
			if (current->kind != UserDataProfileKind::Default || foundDefault) return false;
			foundDefault = true;
		} else if (current->kind == UserDataProfileKind::Default) return false;
	}
	if (!foundDefault) return false;

	auto containsProfile = [&snapshot](const UserDataProfileId& profileId) {
		return std::any_of(snapshot.profiles.begin(), snapshot.profiles.end(), [&profileId](const auto& profile) {
			return profile.profileId == profileId;
		});
	};
	for (auto current = snapshot.workspaceAssociations.begin(); current != snapshot.workspaceAssociations.end(); ++current) {
		if (!containsProfile(current->second)
			|| std::any_of(snapshot.workspaceAssociations.begin(), current, [&current](const auto& existing) {
				return ::platform::uri::UriIdentityService::IsEqual(existing.first, current->first);
			})) return false;
	}
	for (auto current = snapshot.emptyWindowAssociations.begin(); current != snapshot.emptyWindowAssociations.end(); ++current) {
		if (!IsStableEmptyWindowIdentity(current->first) || !containsProfile(current->second)
			|| std::any_of(snapshot.emptyWindowAssociations.begin(), current, [&current](const auto& existing) {
				return existing.first == current->first;
			})) return false;
	}
	return true;
}

std::optional<UserDataProfileDescriptor> FindProfile(
	const UserDataProfileRegistrySnapshot& snapshot, const UserDataProfileId& profileId)
{
	const auto found = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [&profileId](const auto& profile) {
		return profile.profileId == profileId;
	});
	return found == snapshot.profiles.end() ? std::nullopt : std::optional<UserDataProfileDescriptor>(*found);
}

std::optional<UserDataProfileId> FindWorkspaceProfile(
	const UserDataProfileRegistrySnapshot& snapshot, const WorkspaceUri& workspaceUri)
{
	const auto found = std::find_if(snapshot.workspaceAssociations.begin(), snapshot.workspaceAssociations.end(), [&workspaceUri](const auto& association) {
		return ::platform::uri::UriIdentityService::IsEqual(association.first, workspaceUri);
	});
	return found == snapshot.workspaceAssociations.end() ? std::nullopt : std::optional<UserDataProfileId>(found->second);
}

std::optional<UserDataProfileId> FindEmptyWindowProfile(
	const UserDataProfileRegistrySnapshot& snapshot, const EmptyWindowId& emptyWindowId)
{
	const auto found = std::find_if(snapshot.emptyWindowAssociations.begin(), snapshot.emptyWindowAssociations.end(), [&emptyWindowId](const auto& association) {
		return association.first == emptyWindowId;
	});
	return found == snapshot.emptyWindowAssociations.end() ? std::nullopt : std::optional<UserDataProfileId>(found->second);
}

} // namespace

bool IsStableEmptyWindowIdentity(const EmptyWindowId& value) noexcept
{
	constexpr std::wstring_view kPrefix = L"empty-window:";
	if (value.size() < kPrefix.size() + 8 || value.size() > kPrefix.size() + 128 || value.substr(0, kPrefix.size()) != kPrefix) return false;
	for (const wchar_t character : value.substr(kPrefix.size())) {
		if (!((character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')
			|| (character >= L'0' && character <= L'9') || character == L'-' || character == L'_')) return false;
	}
	return true;
}

UserDataProfileResourceUris::UserDataProfileResourceUris(
	::platform::uri::Uri profileHome, ::platform::uri::Uri settings, ::platform::uri::Uri keybindings,
	::platform::uri::Uri snippets, ::platform::uri::Uri tasks, ::platform::uri::Uri extensionsSelection,
	::platform::uri::Uri globalState, ::platform::uri::Uri workingCopies, ::platform::uri::Uri workbenchLayout) noexcept
	: m_profileHome(std::move(profileHome))
	, m_settings(std::move(settings))
	, m_keybindings(std::move(keybindings))
	, m_snippets(std::move(snippets))
	, m_tasks(std::move(tasks))
	, m_extensionsSelection(std::move(extensionsSelection))
	, m_globalState(std::move(globalState))
	, m_workingCopies(std::move(workingCopies))
	, m_workbenchLayout(std::move(workbenchLayout))
{
}

UserDataProfileBootstrapSnapshot::UserDataProfileBootstrapSnapshot(
	ControlProfileAuthorityIdentity controlAuthority, std::uint64_t registryRevision,
	UserDataProfileResolveSource selectionSource, UserDataProfileDescriptor selectedProfile,
	UserDataProfileResourceUris resources) noexcept
	: m_controlAuthority(std::move(controlAuthority))
	, m_registryRevision(registryRevision)
	, m_selectionSource(selectionSource)
	, m_selectedProfile(std::move(selectedProfile))
	, m_resources(std::move(resources))
{
}

UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request, const UserDataProfileRegistrySnapshot& registrySnapshot)
{
	if (!IsCanonicalProfileAuthorityId(request.controlAuthority.authorityId)) {
		return Failed(UserDataProfileBootstrapStatus::InvalidControlAuthorityId);
	}
	if (request.controlAuthority.generation == 0) {
		return Failed(UserDataProfileBootstrapStatus::InvalidControlAuthorityGeneration);
	}
	if (!IsNormalizedAbsoluteWindowsDirectory(request.controlProfileRoot)) {
		return Failed(UserDataProfileBootstrapStatus::InvalidControlProfileRoot);
	}
	if (!IsValidSnapshot(registrySnapshot)) return Failed(UserDataProfileBootstrapStatus::InvalidRegistrySnapshot);
	if (request.selection.explicitProfileId && !IsOpaqueProfileId(*request.selection.explicitProfileId)) {
		return Failed(UserDataProfileBootstrapStatus::InvalidProfileId);
	}
	if (request.selection.emptyWindowId && !IsStableEmptyWindowIdentity(*request.selection.emptyWindowId)) {
		return Failed(UserDataProfileBootstrapStatus::InvalidEmptyWindowIdentity);
	}

	UserDataProfileResolveSource selectionSource = UserDataProfileResolveSource::DefaultProfile;
	std::optional<UserDataProfileDescriptor> selectedProfile;
	if (request.selection.explicitProfileId) {
		selectionSource = UserDataProfileResolveSource::ExplicitProfile;
		selectedProfile = FindProfile(registrySnapshot, *request.selection.explicitProfileId);
		if (!selectedProfile) return Failed(UserDataProfileBootstrapStatus::ProfileNotFound);
	} else if (request.selection.workspaceUri) {
		if (const auto profileId = FindWorkspaceProfile(registrySnapshot, *request.selection.workspaceUri)) {
			selectionSource = UserDataProfileResolveSource::WorkspaceAssociation;
			selectedProfile = FindProfile(registrySnapshot, *profileId);
		}
	}
	if (!selectedProfile && request.selection.emptyWindowId) {
		if (const auto profileId = FindEmptyWindowProfile(registrySnapshot, *request.selection.emptyWindowId)) {
			selectionSource = UserDataProfileResolveSource::EmptyWindowAssociation;
			selectedProfile = FindProfile(registrySnapshot, *profileId);
		}
	}
	if (!selectedProfile) selectedProfile = FindProfile(registrySnapshot, registrySnapshot.defaultProfileId);
	if (!selectedProfile) return Failed(UserDataProfileBootstrapStatus::InvalidRegistrySnapshot);

	std::wstring profileRoot;
	if (request.resourceRootMode == UserDataProfileResourceRootMode::ProfileIdNamespace) {
		profileRoot = JoinWindowsPath(request.controlProfileRoot, L"user-data-profiles\\" + selectedProfile->profileId);
	} else if (request.resourceRootMode == UserDataProfileResourceRootMode::LegacyControlRootForDefault
		&& selectedProfile->kind == UserDataProfileKind::Default && selectedProfile->profileId == registrySnapshot.defaultProfileId) {
		profileRoot = request.controlProfileRoot;
	} else {
		return Failed(UserDataProfileBootstrapStatus::InvalidResourceRootMode);
	}

	auto profileHome = ::platform::uri::Uri::FromWindowsPath(profileRoot);
	auto settings = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L"settings.json"));
	auto keybindings = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L"keybindings.json"));
	auto snippets = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L"snippets"));
	auto tasks = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L"tasks.json"));
	auto extensionsSelection = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L"extensions.json"));
	auto globalState = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L".sakura-platform\\globalState"));
	auto workingCopies = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L".sakura-platform\\workingCopies"));
	auto workbenchLayout = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileRoot, L".sakura-platform\\workbench\\layout.json"));
	if (!profileHome || !settings || !keybindings || !snippets || !tasks || !extensionsSelection || !globalState || !workingCopies || !workbenchLayout) {
		return Failed(UserDataProfileBootstrapStatus::InvalidResourceIdentity);
	}

	UserDataProfileResourceUris resources(
		std::move(*profileHome.value), std::move(*settings.value), std::move(*keybindings.value),
		std::move(*snippets.value), std::move(*tasks.value), std::move(*extensionsSelection.value),
		std::move(*globalState.value), std::move(*workingCopies.value), std::move(*workbenchLayout.value));
	return { UserDataProfileBootstrapStatus::Resolved,
		UserDataProfileBootstrapSnapshot(request.controlAuthority, registrySnapshot.revision, selectionSource,
			std::move(*selectedProfile), std::move(resources)) };
}

UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request, std::string_view registryDocument)
{
	const auto decoded = DecodeUserDataProfileRegistryDocument(registryDocument);
	if (!decoded.Decoded()) return Failed(UserDataProfileBootstrapStatus::InvalidRegistryDocument);
	return ResolveUserDataProfileBootstrap(request, decoded.snapshot);
}

UserDataProfileBootstrapResult ResolveUserDataProfileBootstrap(
	const UserDataProfileBootstrapRequest& request, const UserDataProfileRegistry& registry)
{
	return ResolveUserDataProfileBootstrap(request, registry.Snapshot(true));
}

} // namespace platform::profiles
