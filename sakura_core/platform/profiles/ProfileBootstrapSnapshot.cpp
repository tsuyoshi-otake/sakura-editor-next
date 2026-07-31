/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/profiles/ProfileBootstrapSnapshot.h"

#include <utility>

namespace platform::profiles {
namespace {

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
	if (directory.empty() || IsDotComponent(directory)) return false;
	if (directory.size() >= 4 && directory[0] == L'\\' && directory[1] == L'\\'
		&& (directory[2] == L'?' || directory[2] == L'.') && IsSeparator(directory[3])) {
		return false;
	}

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

std::wstring JoinWindowsPath(std::wstring_view directory, std::wstring_view child)
{
	std::wstring result(directory);
	if (!result.empty() && !IsSeparator(result.back())) result.push_back(L'\\');
	result.append(child);
	return result;
}

ProfileBootstrapSnapshotResult Failed(ProfileBootstrapSnapshotStatus status)
{
	return { status, std::nullopt };
}

} // namespace

ProfileBootstrapResourceUris::ProfileBootstrapResourceUris(
	::platform::uri::Uri profileHome,
	::platform::uri::Uri settings,
	::platform::uri::Uri tasks,
	::platform::uri::Uri keybindings,
	::platform::uri::Uri snippets,
	::platform::uri::Uri extensionsManifest,
	::platform::uri::Uri extensionsInstallHome,
	::platform::uri::Uri globalStorage) noexcept
	: m_profileHome(std::move(profileHome))
	, m_settings(std::move(settings))
	, m_tasks(std::move(tasks))
	, m_keybindings(std::move(keybindings))
	, m_snippets(std::move(snippets))
	, m_extensionsManifest(std::move(extensionsManifest))
	, m_extensionsInstallHome(std::move(extensionsInstallHome))
	, m_globalStorage(std::move(globalStorage))
{
}

ProfileBootstrapSnapshot::ProfileBootstrapSnapshot(
	std::string profileId,
	std::uint64_t authorityGeneration,
	ProfileBootstrapResourceUris resources) noexcept
	: m_profileId(std::move(profileId))
	, m_authorityGeneration(authorityGeneration)
	, m_resources(std::move(resources))
{
}

ProfileBootstrapSnapshotResult ResolveProfileBootstrapSnapshot(
	std::string profileId,
	std::uint64_t authorityGeneration,
	std::wstring_view profileDirectory)
{
	if (!IsCanonicalProfileAuthorityId(profileId)) {
		return Failed(ProfileBootstrapSnapshotStatus::InvalidProfileId);
	}
	if (authorityGeneration == 0) {
		return Failed(ProfileBootstrapSnapshotStatus::InvalidAuthorityGeneration);
	}
	if (!IsNormalizedAbsoluteWindowsDirectory(profileDirectory)) {
		return Failed(ProfileBootstrapSnapshotStatus::InvalidProfileDirectory);
	}

	auto profileHome = ::platform::uri::Uri::FromWindowsPath(profileDirectory);
	auto settings = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"settings.json"));
	auto tasks = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"tasks.json"));
	auto keybindings = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"keybindings.json"));
	auto snippets = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"snippets"));
	auto extensionsManifest = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"extensions.json"));
	auto extensionsInstallHome = ::platform::uri::Uri::FromWindowsPath(JoinWindowsPath(profileDirectory, L"extensions"));
	auto globalStorage = ::platform::uri::Uri::FromWindowsPath(
		JoinWindowsPath(profileDirectory, L".sakura-platform\\globalStorage"));
	if (!profileHome || !settings || !tasks || !keybindings || !snippets || !extensionsManifest || !extensionsInstallHome || !globalStorage) {
		return Failed(ProfileBootstrapSnapshotStatus::InvalidResourceIdentity);
	}

	ProfileBootstrapResourceUris resources(
		std::move(*profileHome.value), std::move(*settings.value), std::move(*tasks.value),
		std::move(*keybindings.value), std::move(*snippets.value), std::move(*extensionsManifest.value),
		std::move(*extensionsInstallHome.value), std::move(*globalStorage.value));
	return { ProfileBootstrapSnapshotStatus::Resolved,
		ProfileBootstrapSnapshot(std::move(profileId), authorityGeneration, std::move(resources)) };
}

} // namespace platform::profiles
