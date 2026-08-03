/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"

#include "workbench/workspace/WorkspaceFolderLimits.h"

#include "platform/uri/UriIdentity.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <utility>

namespace workbench::workspace {
namespace {

constexpr std::size_t kMaximumUriComponentLength = 4096U;
constexpr std::size_t kMaximumFolderDisplayNameLength = 256U;

WorkspaceConfigurationParseResult Failed(EWorkspaceConfigurationDiagnosticCode code, const char* message)
{
	WorkspaceConfigurationParseResult result;
	result.diagnostics.push_back({ code, message });
	return result;
}

std::wstring ToLowerInvariant(std::wstring_view value)
{
	std::wstring result(value);
	for (auto& character : result) character = character <= 0x7f
		? (character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a') : character)
		: static_cast<wchar_t>(std::towlower(character));
	return result;
}

bool HasBoundedUriEncoding(const platform::uri::Uri& uri) noexcept
{
	auto bounded = [](std::wstring_view value) { return value.size() <= kMaximumUriComponentLength; };
	return bounded(uri.Scheme()) && bounded(uri.Authority()) && bounded(uri.Path())
		&& (!uri.Query() || bounded(*uri.Query())) && (!uri.Fragment() || bounded(*uri.Fragment()));
}

bool IsWorkspaceResourceUri(const platform::uri::Uri& uri) noexcept
{
	return !uri.Scheme().empty() && !uri.Path().empty() && !uri.Query() && !uri.Fragment();
}

std::wstring NormalizeUriPath(std::wstring_view path)
{
	const bool absolute = !path.empty() && path.front() == L'/';
	const bool trailingSlash = path.size() > 1 && path.back() == L'/';
	std::vector<std::wstring_view> components;
	for (std::size_t begin = 0; begin <= path.size();) {
		const auto end = path.find(L'/', begin);
		const auto component = path.substr(begin, end == std::wstring_view::npos ? path.size() - begin : end - begin);
		if (!component.empty() && component != L".") {
			if (component == L"..") {
				if (!components.empty() && components.back() != L"..") components.pop_back();
				else if (!absolute) components.push_back(component);
			} else components.push_back(component);
		}
		if (end == std::wstring_view::npos) break;
		begin = end + 1;
	}
	std::wstring result = absolute ? L"/" : L"";
	for (std::size_t index = 0; index < components.size(); ++index) {
		if (index != 0) result.push_back(L'/');
		result += components[index];
	}
	if (trailingSlash && !result.empty() && result.back() != L'/') result.push_back(L'/');
	return result;
}

bool IsWindowsDriveRoot(std::wstring_view path) noexcept
{
	return path.size() == 4U && path[0] == L'/'
		&& ((path[1] >= L'A' && path[1] <= L'Z') || (path[1] >= L'a' && path[1] <= L'z'))
		&& path[2] == L':' && path[3] == L'/';
}

bool IsUncShareRoot(std::wstring_view authority, std::wstring_view path) noexcept
{
	if (authority.empty() || path.size() < 3U || path.front() != L'/') return false;
	const auto separator = path.find(L'/', 1U);
	return separator != std::wstring_view::npos && separator == path.size() - 1U && separator > 1U;
}

std::wstring RemoveTrailingFolderSeparator(bool isFile, std::wstring_view authority, std::wstring path)
{
	while (path.size() > 1U && path.back() == L'/') {
		if ((isFile && IsWindowsDriveRoot(path)) || (isFile && IsUncShareRoot(authority, path))) break;
		path.pop_back();
	}
	return path;
}

std::optional<platform::uri::Uri> NormalizeFolderResource(const platform::uri::Uri& uri)
{
	if (!HasBoundedUriEncoding(uri)) return std::nullopt;
	const std::wstring scheme = ToLowerInvariant(uri.Scheme());
	const bool isFile = scheme == L"file";
	std::wstring path = NormalizeUriPath(uri.Path());
	path = RemoveTrailingFolderSeparator(isFile, uri.Authority(), std::move(path));
	auto result = platform::uri::Uri::FromComponents(
		scheme, uri.Authority(), std::move(path), uri.Query(), uri.Fragment(), isFile ? true : uri.HasAuthority());
	return result ? std::move(result.value) : std::nullopt;
}

std::optional<platform::uri::Uri> Canonicalize(const platform::uri::Uri& uri)
{
	auto normalized = NormalizeFolderResource(uri);
	if (!normalized) return std::nullopt;
	const bool isFile = normalized->Scheme() == L"file";
	const bool localFileAuthority = isFile
		&& (normalized->Authority().empty() || ToLowerInvariant(normalized->Authority()) == L"localhost");
	auto result = platform::uri::Uri::FromComponents(
		normalized->Scheme(), localFileAuthority ? std::wstring {} : normalized->Authority(), normalized->Path(),
		normalized->Query(), normalized->Fragment(), isFile ? true : normalized->HasAuthority());
	return result ? std::move(result.value) : std::nullopt;
}

bool IsValidDisplayName(std::wstring_view value) noexcept
{
	return !value.empty() && value.size() <= kMaximumFolderDisplayNameLength
		&& std::none_of(value.begin(), value.end(), [](wchar_t character) { return character <= 0x1f || character == 0x7f; });
}

std::optional<std::wstring> DeriveDisplayName(const platform::uri::Uri& uri)
{
	std::wstring_view path = uri.Path();
	while (path.size() > 1 && path.back() == L'/') path.remove_suffix(1);
	const auto separator = path.find_last_of(L'/');
	const auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
	if (!IsValidDisplayName(name)) return std::nullopt;
	return std::wstring(name);
}

bool IsWindowsAbsolutePath(std::wstring_view value) noexcept
{
	return value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z'))
		&& value[1] == L':' && (value[2] == L'/' || value[2] == L'\\');
}

bool IsWindowsUncPath(std::wstring_view value) noexcept
{
	if (value.size() < 5U || (value[0] != L'\\' && value[0] != L'/')
		|| (value[1] != L'\\' && value[1] != L'/')) return false;
	const auto serverEnd = value.find_first_of(L"\\/", 2U);
	return serverEnd != std::wstring_view::npos && serverEnd > 2U
		&& serverEnd + 1U < value.size()
		&& value.find_first_not_of(L"\\/", serverEnd + 1U) != std::wstring_view::npos;
}

std::optional<platform::uri::Uri> ResolvePath(const platform::uri::Uri& workspaceConfigUri, std::wstring path)
{
	if (path.empty() || path.size() > kMaximumUriComponentLength) return std::nullopt;
	if (IsWindowsAbsolutePath(path) || IsWindowsUncPath(path)) {
		if (IsWindowsUncPath(path)) {
			// Workspace JSON conventionally persists UNC paths with '/' separators,
			// while Uri::FromWindowsPath accepts Win32's canonical '\\server' form.
			std::replace(path.begin(), path.end(), L'/', L'\\');
		}
		auto absolute = platform::uri::Uri::FromWindowsPath(path);
		if (!absolute) return std::nullopt;
		return Canonicalize(*absolute.value);
	}
	std::replace(path.begin(), path.end(), L'\\', L'/');
	std::wstring resolved;
	if (!path.empty() && path.front() == L'/') {
		resolved = NormalizeUriPath(path);
	} else {
		const auto separator = workspaceConfigUri.Path().find_last_of(L'/');
		if (separator == std::wstring::npos) return std::nullopt;
		resolved.assign(workspaceConfigUri.Path().substr(0, separator + 1));
		resolved += path;
		resolved = NormalizeUriPath(resolved);
	}
	auto result = platform::uri::Uri::FromComponents(
		workspaceConfigUri.Scheme(), workspaceConfigUri.Authority(), std::move(resolved), std::nullopt, std::nullopt,
		workspaceConfigUri.HasAuthority());
	return result ? Canonicalize(*result.value) : std::nullopt;
}

std::optional<platform::uri::Uri> ResolveFolder(
	const platform::serialization::JsoncValue::Object& object,
	const platform::uri::Uri& workspaceConfigUri,
	EWorkspaceConfigurationDiagnosticCode& failure)
{
	const auto path = object.find(L"path");
	const auto uri = object.find(L"uri");
	if ((path == object.end()) == (uri == object.end())) {
		failure = EWorkspaceConfigurationDiagnosticCode::FolderMustSpecifyExactlyOneLocation;
		return std::nullopt;
	}
	if (path != object.end()) {
		const auto* value = std::get_if<std::wstring>(&path->second.Value());
		if (!value) { failure = EWorkspaceConfigurationDiagnosticCode::FolderPathMustBeString; return std::nullopt; }
		auto resolved = ResolvePath(workspaceConfigUri, *value);
		if (!resolved || !IsWorkspaceResourceUri(*resolved)) {
			failure = EWorkspaceConfigurationDiagnosticCode::InvalidFolderUri;
			return std::nullopt;
		}
		return resolved;
	}
	const auto* value = std::get_if<std::wstring>(&uri->second.Value());
	if (!value) { failure = EWorkspaceConfigurationDiagnosticCode::FolderUriMustBeString; return std::nullopt; }
	auto parsed = platform::uri::Uri::Parse(*value);
	if (!parsed) { failure = EWorkspaceConfigurationDiagnosticCode::InvalidFolderUri; return std::nullopt; }
	auto canonical = Canonicalize(*parsed.value);
	if (!canonical || !IsWorkspaceResourceUri(*canonical)) {
		failure = EWorkspaceConfigurationDiagnosticCode::InvalidFolderUri;
		return std::nullopt;
	}
	return canonical;
}

} // namespace

WorkspaceConfigurationParseResult CWorkspaceConfigurationDocumentParser::Parse(
	std::string_view utf8, const platform::uri::Uri& workspaceConfigUri)
{
	auto canonicalConfigUri = Canonicalize(workspaceConfigUri);
	if (!canonicalConfigUri || !IsWorkspaceResourceUri(*canonicalConfigUri)) return Failed(EWorkspaceConfigurationDiagnosticCode::InvalidFolderUri, "workspace configuration resource is invalid");
	auto parsed = platform::serialization::CJsoncDocument::Parse(utf8);
	if (!parsed.Succeeded()) return Failed(EWorkspaceConfigurationDiagnosticCode::JsoncParseFailed, "workspace JSONC document could not be parsed");
	const auto* root = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
	if (!root) return Failed(EWorkspaceConfigurationDiagnosticCode::RootMustBeObject, "workspace document root must be an object");

	WorkspaceConfigurationDocument document;
	const auto folders = root->find(L"folders");
	if (folders != root->end()) {
		const auto* entries = std::get_if<platform::serialization::JsoncValue::Array>(&folders->second.Value());
		if (!entries) return Failed(EWorkspaceConfigurationDiagnosticCode::FoldersMustBeArray, "workspace folders must be an array");
		std::set<std::wstring, std::less<>> identities;
		for (const auto& entry : *entries) {
			const auto* folder = std::get_if<platform::serialization::JsoncValue::Object>(&entry.Value());
			if (!folder) return Failed(EWorkspaceConfigurationDiagnosticCode::FolderMustBeObject, "workspace folder must be an object");
			for (const auto& [key, ignored] : *folder) {
				(void)ignored;
				if (key != L"path" && key != L"uri" && key != L"name") {
					return Failed(EWorkspaceConfigurationDiagnosticCode::UnsupportedFolderMember, "workspace folder contains an unsupported member");
				}
			}
			EWorkspaceConfigurationDiagnosticCode failure = EWorkspaceConfigurationDiagnosticCode::InvalidFolderUri;
			auto uri = ResolveFolder(*folder, *canonicalConfigUri, failure);
			if (!uri) return Failed(failure, "workspace folder location is invalid");
			std::optional<std::wstring> displayName;
			if (const auto name = folder->find(L"name"); name != folder->end()) {
				const auto* value = std::get_if<std::wstring>(&name->second.Value());
				if (!value) return Failed(EWorkspaceConfigurationDiagnosticCode::FolderNameMustBeString, "workspace folder name must be a string");
				if (value->empty()) displayName = DeriveDisplayName(*uri);
				else {
					if (!IsValidDisplayName(*value)) return Failed(EWorkspaceConfigurationDiagnosticCode::InvalidFolderName, "workspace folder name is invalid");
					displayName = *value;
				}
			} else displayName = DeriveDisplayName(*uri);
			if (!displayName) return Failed(EWorkspaceConfigurationDiagnosticCode::InvalidFolderName, "workspace folder name is invalid");
			auto identity = platform::uri::UriIdentityService::MakeComparisonKey(*uri);
			if (!identities.emplace(std::move(identity)).second) {
				// First occurrence owns the descriptor. The warning deliberately has no URI text.
				continue;
			}
			document.folders.push_back({ std::move(*uri), std::move(*displayName) });
		}
		if (document.folders.size() > kMaximumWorkspaceFolders) {
			return Failed(EWorkspaceConfigurationDiagnosticCode::MaximumFoldersExceeded, "workspace folder limit exceeded");
		}
	}

	if (const auto settings = root->find(L"settings"); settings != root->end()) {
		const auto* object = std::get_if<platform::serialization::JsoncValue::Object>(&settings->second.Value());
		if (!object) return Failed(EWorkspaceConfigurationDiagnosticCode::SettingsMustBeObject, "workspace settings must be an object");
		document.settings = *object;
	}
	for (const auto [key, member] : { std::pair { L"tasks", EWorkspaceFileMember::Tasks }, std::pair { L"launch", EWorkspaceFileMember::Launch }, std::pair { L"extensions", EWorkspaceFileMember::Extensions } }) {
		if (const auto value = root->find(key); value != root->end()) document.fileMembers.push_back({ member, value->second });
	}

	WorkspaceConfigurationParseResult result;
	result.document = std::move(document);
	// Report duplicate locations after all fatal validation so callers can retain
	// the usable first-wins document while publishing a path-free warning.
	if (folders != root->end()) {
		const auto* entries = std::get_if<platform::serialization::JsoncValue::Array>(&folders->second.Value());
		if (entries && result.document->folders.size() < entries->size()) result.diagnostics.push_back({ EWorkspaceConfigurationDiagnosticCode::DuplicateFolderUri, "duplicate workspace folder ignored" });
	}
	return result;
}

std::optional<platform::uri::Uri> CWorkspaceConfigurationDocumentParser::NormalizeFolderUri(
	const platform::uri::Uri& uri)
{
	return NormalizeFolderResource(uri);
}

std::optional<platform::uri::Uri> CWorkspaceConfigurationDocumentParser::ResolveFolderPath(
	const platform::uri::Uri& workspaceConfigUri, std::wstring_view path)
{
	auto canonicalConfigUri = Canonicalize(workspaceConfigUri);
	if (!canonicalConfigUri || !IsWorkspaceResourceUri(*canonicalConfigUri)) return std::nullopt;
	return ResolvePath(*canonicalConfigUri, std::wstring(path));
}

} // namespace workbench::workspace
