/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/WorkbenchBootstrapContext.h"

#include "platform/profiles/UserDataProfileIdentity.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace workbench {
namespace {

constexpr std::size_t kMaximumWindowIdentityLength = 256;
constexpr std::size_t kMaximumFolderDisplayNameLength = 256;
constexpr std::size_t kMaximumUriTextLength = 4096;
constexpr std::size_t kMaximumWorkspaceFolders = 256;

bool IsValidUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto character = static_cast<std::uint32_t>(value[index]);
		if constexpr (sizeof(wchar_t) == 2) {
			if (character >= 0xd800 && character <= 0xdbff) {
				if (++index >= value.size()) return false;
				const auto trailing = static_cast<std::uint32_t>(value[index]);
				if (trailing < 0xdc00 || trailing > 0xdfff) return false;
			} else if (character >= 0xdc00 && character <= 0xdfff) {
				return false;
			}
		}
	}
	return true;
}

bool IsBoundedIdentity(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumWindowIdentityLength || !IsValidUtf16(value)) return false;
	return std::none_of(value.begin(), value.end(), [](wchar_t character) {
		return character <= 0x1f || character == 0x7f;
	});
}

bool IsBoundedFolderDisplayName(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumFolderDisplayNameLength || !IsValidUtf16(value)) return false;
	return std::none_of(value.begin(), value.end(), [](wchar_t character) {
		return character <= 0x1f || character == 0x7f;
	});
}

std::optional<std::wstring> DeriveFolderDisplayName(const platform::uri::Uri& uri)
{
	std::wstring_view path = uri.Path();
	while (path.size() > 1 && path.back() == L'/') path.remove_suffix(1);
	const auto separator = path.find_last_of(L'/');
	const auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
	if (!IsBoundedFolderDisplayName(name)) return std::nullopt;
	return std::wstring(name);
}

bool HasBoundedUriEncoding(const platform::uri::Uri& uri) noexcept
{
	auto underBound = [](std::wstring_view value) { return value.size() <= kMaximumUriTextLength / 12; };
	return underBound(uri.Scheme()) && underBound(uri.Authority()) && underBound(uri.Path())
		&& (!uri.Query() || underBound(*uri.Query())) && (!uri.Fragment() || underBound(*uri.Fragment()));
}

std::wstring ToLowerInvariant(std::wstring_view value)
{
	std::wstring result(value);
	for (auto& character : result) character = character <= 0x7f
		? (character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a') : character)
		: static_cast<wchar_t>(std::towlower(character));
	return result;
}

std::optional<platform::uri::Uri> Canonicalize(const platform::uri::Uri& uri)
{
	if (!HasBoundedUriEncoding(uri)) return std::nullopt;
	const bool isFile = ToLowerInvariant(uri.Scheme()) == L"file";
	const bool localFileAuthority = isFile && (uri.Authority().empty() || ToLowerInvariant(uri.Authority()) == L"localhost");
	auto canonical = platform::uri::Uri::FromComponents(
		ToLowerInvariant(uri.Scheme()), localFileAuthority ? std::wstring{} : uri.Authority(), uri.Path(),
		uri.Query(), uri.Fragment(), isFile ? true : uri.HasAuthority());
	return canonical ? std::move(canonical.value) : std::nullopt;
}

bool IsWorkspaceResourceUri(const platform::uri::Uri& uri) noexcept
{
	return !uri.Path().empty() && !uri.Query() && !uri.Fragment();
}

bool IsGeneralResourceUri(const platform::uri::Uri& uri) noexcept
{
	return !uri.Scheme().empty() && !uri.Path().empty();
}

void AppendIdentityPart(std::wstring& target, std::wstring_view value)
{
	target += std::to_wstring(value.size());
	target += L':';
	target += value;
}

std::optional<std::wstring> MakeIdentity(config::EWorkspaceKind kind, std::wstring_view windowId,
	const std::optional<std::wstring>& configKey, const std::vector<std::wstring>& folderKeys)
{
	std::wstring identity;
	switch (kind) {
	case config::EWorkspaceKind::Empty:
		identity = L"empty:";
		AppendIdentityPart(identity, windowId);
		break;
	case config::EWorkspaceKind::Folder:
		if (configKey || folderKeys.size() != 1) return std::nullopt;
		identity = L"folder:";
		AppendIdentityPart(identity, folderKeys.front());
		break;
	case config::EWorkspaceKind::Workspace:
		if (!configKey) return std::nullopt;
		identity = L"workspace:";
		AppendIdentityPart(identity, *configKey);
		break;
	}
	return identity.size() <= kMaximumUriTextLength * 4 ? std::optional<std::wstring>(std::move(identity)) : std::nullopt;
}

bool IsProfileShapeValid(const platform::profiles::ProfileBootstrapSnapshot& profile)
{
	if (!platform::profiles::IsCanonicalProfileAuthorityId(profile.ProfileId()) || profile.AuthorityGeneration() == 0) return false;
	const auto& resources = profile.Resources();
	return IsWorkspaceResourceUri(resources.ProfileHome())
		&& IsWorkspaceResourceUri(resources.Settings())
		&& IsWorkspaceResourceUri(resources.Tasks())
		&& IsWorkspaceResourceUri(resources.Keybindings())
		&& IsWorkspaceResourceUri(resources.Snippets())
		&& IsWorkspaceResourceUri(resources.GlobalStorage());
}

bool IsValidUserDataProfileKind(platform::profiles::UserDataProfileKind kind) noexcept
{
	return kind == platform::profiles::UserDataProfileKind::Default
		|| kind == platform::profiles::UserDataProfileKind::Normal
		|| kind == platform::profiles::UserDataProfileKind::Transient;
}

bool IsUserDataDescriptorShapeValid(const platform::profiles::UserDataProfileDescriptor& descriptor) noexcept
{
	if (!platform::profiles::IsOpaqueUserDataProfileId(descriptor.profileId) || descriptor.displayName.empty()
		|| descriptor.displayName.size() > 1024 || !IsValidUtf16(descriptor.displayName)
		|| !IsValidUserDataProfileKind(descriptor.kind)) return false;
	return std::all_of(descriptor.legacyAliases.begin(), descriptor.legacyAliases.end(), [](const std::wstring& alias) {
		return !alias.empty() && alias.size() <= 1024 && IsValidUtf16(alias);
	});
}

bool IsUserDataResourcesShapeValid(const platform::profiles::UserDataProfileResourceUris& resources) noexcept
{
	return IsWorkspaceResourceUri(resources.ProfileHome())
		&& IsWorkspaceResourceUri(resources.Settings())
		&& IsWorkspaceResourceUri(resources.Keybindings())
		&& IsWorkspaceResourceUri(resources.Snippets())
		&& IsWorkspaceResourceUri(resources.Tasks())
		&& IsWorkspaceResourceUri(resources.GlobalState())
		&& IsWorkspaceResourceUri(resources.WorkingCopies())
		&& IsWorkspaceResourceUri(resources.WorkbenchLayout())
		&& HasBoundedUriEncoding(resources.ProfileHome())
		&& HasBoundedUriEncoding(resources.Settings())
		&& HasBoundedUriEncoding(resources.Keybindings())
		&& HasBoundedUriEncoding(resources.Snippets())
		&& HasBoundedUriEncoding(resources.Tasks())
		&& HasBoundedUriEncoding(resources.GlobalState())
		&& HasBoundedUriEncoding(resources.WorkingCopies())
		&& HasBoundedUriEncoding(resources.WorkbenchLayout());
}

bool IsUserDataProfileShapeValid(const platform::profiles::UserDataProfileBootstrapSnapshot& profile,
	const platform::profiles::ProfileBootstrapSnapshot& controlProfile) noexcept
{
	const auto& authority = profile.ControlAuthority();
	if (!platform::profiles::IsCanonicalProfileAuthorityId(authority.authorityId)
		|| authority.generation == 0 || authority.authorityId != controlProfile.ProfileId()
		|| authority.generation != controlProfile.AuthorityGeneration()) return false;
	const auto& descriptor = profile.SelectedProfile();
	return IsUserDataDescriptorShapeValid(descriptor)
		&& profile.SelectedProfileId() == descriptor.profileId
		&& IsUserDataResourcesShapeValid(profile.Resources());
}

WorkbenchBootstrapResult Failed(EWorkbenchBootstrapStatus status)
{
	return { status, std::nullopt };
}

} // namespace

bool WorkbenchBootstrapResult::Resolved() const noexcept
{
	return status == EWorkbenchBootstrapStatus::Resolved && context.has_value();
}

WorkbenchBootstrapContext::WorkbenchBootstrapContext(
	platform::profiles::ProfileBootstrapSnapshot controlProfile,
	platform::profiles::UserDataProfileBootstrapSnapshot userDataProfile,
	std::wstring windowInstanceIdentity,
	config::WorkspaceContextSnapshot workspace,
	std::optional<platform::uri::Uri> initialDocumentUri,
	std::optional<platform::uri::Uri> terminalLaunchDirectoryUri) noexcept
	: m_controlProfile(std::move(controlProfile))
	, m_userDataProfile(std::move(userDataProfile))
	, m_windowInstanceIdentity(std::move(windowInstanceIdentity))
	, m_workspace(std::move(workspace))
	, m_initialDocumentUri(std::move(initialDocumentUri))
	, m_terminalLaunchDirectoryUri(std::move(terminalLaunchDirectoryUri))
{
}

WorkbenchBootstrapResult ResolveWorkbenchBootstrapContext(WorkbenchBootstrapRequest request)
{
	if (!IsProfileShapeValid(request.controlProfile)) return Failed(EWorkbenchBootstrapStatus::InvalidProfileSnapshot);
	if (!IsUserDataProfileShapeValid(request.userDataProfile, request.controlProfile)) {
		return Failed(EWorkbenchBootstrapStatus::InvalidUserDataProfileSnapshot);
	}
	if (!IsBoundedIdentity(request.windowInstanceIdentity)) return Failed(EWorkbenchBootstrapStatus::InvalidWindowInstanceIdentity);
	if (request.explicitFolderUri && (request.explicitWorkspaceConfigUri || !request.workspaceFolders.empty())) {
		return Failed(EWorkbenchBootstrapStatus::InvalidWorkspaceShape);
	}
	if (!request.explicitWorkspaceConfigUri && !request.workspaceFolders.empty()) {
		return Failed(EWorkbenchBootstrapStatus::InvalidWorkspaceShape);
	}
	if (request.workspaceFolders.size() > kMaximumWorkspaceFolders) return Failed(EWorkbenchBootstrapStatus::InvalidWorkspaceShape);

	std::optional<platform::uri::Uri> folder;
	if (request.explicitFolderUri) {
		folder = Canonicalize(*request.explicitFolderUri);
		if (!folder || !IsWorkspaceResourceUri(*folder)) return Failed(EWorkbenchBootstrapStatus::InvalidFolderUri);
	}
	std::optional<platform::uri::Uri> workspaceConfig;
	if (request.explicitWorkspaceConfigUri) {
		workspaceConfig = Canonicalize(*request.explicitWorkspaceConfigUri);
		if (!workspaceConfig || !IsWorkspaceResourceUri(*workspaceConfig)) return Failed(EWorkbenchBootstrapStatus::InvalidWorkspaceConfigUri);
	}
	std::optional<platform::uri::Uri> initialDocument;
	if (request.initialDocumentUri) {
		initialDocument = Canonicalize(*request.initialDocumentUri);
		if (!initialDocument || !IsGeneralResourceUri(*initialDocument)) return Failed(EWorkbenchBootstrapStatus::InvalidInitialDocumentUri);
	}
	std::optional<platform::uri::Uri> terminalDirectory;
	if (request.terminalLaunchDirectoryUri) {
		terminalDirectory = Canonicalize(*request.terminalLaunchDirectoryUri);
		if (!terminalDirectory || !IsWorkspaceResourceUri(*terminalDirectory)) return Failed(EWorkbenchBootstrapStatus::InvalidTerminalLaunchDirectoryUri);
	}

	config::WorkspaceContextSnapshot workspace;
	workspace.generation = 1;
	workspace.revision = 0;
	workspace.trust = config::EWorkspaceTrustState::Unknown;
	if (folder) {
		const auto key = platform::uri::UriIdentityService::MakeComparisonKey(*folder);
		const auto displayName = DeriveFolderDisplayName(*folder);
		if (!displayName) return Failed(EWorkbenchBootstrapStatus::InvalidFolderDisplayName);
		const auto identity = MakeIdentity(config::EWorkspaceKind::Folder, request.windowInstanceIdentity, std::nullopt, { key });
		if (key.empty() || !identity) return Failed(EWorkbenchBootstrapStatus::InvalidFolderUri);
		workspace.kind = config::EWorkspaceKind::Folder;
		workspace.folders.push_back({ std::move(*folder), std::move(*displayName) });
		workspace.workspaceIdentityKey = std::move(*identity);
	} else if (workspaceConfig) {
		std::vector<std::wstring> folderKeys;
		workspace.folders.reserve(request.workspaceFolders.size());
		for (const auto& descriptor : request.workspaceFolders) {
			auto canonical = Canonicalize(descriptor.uri);
			if (!canonical || !IsWorkspaceResourceUri(*canonical)) return Failed(EWorkbenchBootstrapStatus::InvalidFolderUri);
			if (!IsBoundedFolderDisplayName(descriptor.displayName)) {
				return Failed(EWorkbenchBootstrapStatus::InvalidFolderDisplayName);
			}
			const auto key = platform::uri::UriIdentityService::MakeComparisonKey(*canonical);
			if (key.empty() || std::find(folderKeys.begin(), folderKeys.end(), key) != folderKeys.end()) {
				return Failed(EWorkbenchBootstrapStatus::DuplicateWorkspaceFolderIdentity);
			}
			folderKeys.push_back(key);
			workspace.folders.push_back({ std::move(*canonical), descriptor.displayName });
		}
		const auto configKey = platform::uri::UriIdentityService::MakeComparisonKey(*workspaceConfig);
		const auto identity = MakeIdentity(config::EWorkspaceKind::Workspace, request.windowInstanceIdentity, configKey, folderKeys);
		if (configKey.empty() || !identity) return Failed(EWorkbenchBootstrapStatus::InvalidWorkspaceConfigUri);
		workspace.kind = config::EWorkspaceKind::Workspace;
		workspace.workspaceConfigUri = std::move(*workspaceConfig);
		workspace.workspaceIdentityKey = std::move(*identity);
	} else {
		const auto identity = MakeIdentity(config::EWorkspaceKind::Empty, request.windowInstanceIdentity, std::nullopt, {});
		if (!identity) return Failed(EWorkbenchBootstrapStatus::InvalidWindowInstanceIdentity);
		workspace.kind = config::EWorkspaceKind::Empty;
		workspace.workspaceIdentityKey = std::move(*identity);
	}

	return { EWorkbenchBootstrapStatus::Resolved,
		WorkbenchBootstrapContext(std::move(request.controlProfile), std::move(request.userDataProfile),
			std::move(request.windowInstanceIdentity), std::move(workspace),
			std::move(initialDocument), std::move(terminalDirectory)) };
}

} // namespace workbench
