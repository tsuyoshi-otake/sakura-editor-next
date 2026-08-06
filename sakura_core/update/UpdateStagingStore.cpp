/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateStagingStore.h"

#include <sakura/serialization/JsoncDocument.h>

#include <ShlObj.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <system_error>
#include <variant>

namespace update {
namespace {

using platform::serialization::CJsoncDocument;
using platform::serialization::JsoncValue;

constexpr std::wstring_view kManifestFileName = L"update.json";
constexpr std::wstring_view kInstallLogFileName = L"install.log";

//! Long enough for a path plus a tag, short enough that a hostile manifest
//! cannot make the decoder allocate without bound before the parser's own limits
//! would have.
constexpr std::size_t kMaximumManifestBytes = 64 * 1024;

bool ToUtf8(std::wstring_view value, std::string& output)
{
	const int bytes = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes < 0 || (bytes == 0 && !value.empty())) return false;
	output.resize(static_cast<std::size_t>(bytes));
	if (bytes == 0) return true;
	return ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
		output.data(), bytes, nullptr, nullptr) == bytes;
}

void AppendJsonString(std::string& output, std::wstring_view value)
{
	std::string utf8;
	if (!ToUtf8(value, utf8)) utf8.clear();
	output.push_back('"');
	for (const char ch : utf8) {
		switch (ch) {
		case '"': output.append("\\\""); break;
		case '\\': output.append("\\\\"); break;
		case '\n': output.append("\\n"); break;
		case '\r': output.append("\\r"); break;
		case '\t': output.append("\\t"); break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20) {
				char escape[7] = {};
				std::snprintf(escape, sizeof(escape), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(ch)));
				output.append(escape);
			} else {
				output.push_back(ch);
			}
			break;
		}
	}
	output.push_back('"');
}

void AppendMember(std::string& output, std::string_view key, std::wstring_view value, bool& first)
{
	if (!first) output.push_back(',');
	first = false;
	output.push_back('"');
	output.append(key);
	output.append("\":");
	AppendJsonString(output, value);
}

const JsoncValue* Member(const JsoncValue::Object& object, std::wstring_view key) noexcept
{
	const auto it = object.find(key);
	return it == object.end() ? nullptr : &it->second;
}

std::wstring StringMember(const JsoncValue::Object& object, std::wstring_view key)
{
	const JsoncValue* member = Member(object, key);
	if (member == nullptr) return {};
	const auto* text = std::get_if<std::wstring>(&member->Value());
	return text == nullptr ? std::wstring() : *text;
}

} // namespace

std::string EncodeUpdateManifest(const UpdateManifest& manifest)
{
	std::string output = "{";
	bool first = true;
	AppendMember(output, "version", manifest.version.ToProductVersion(), first);
	AppendMember(output, "tagName", manifest.tagName, first);
	AppendMember(output, "installerPath", manifest.installerPath, first);
	AppendMember(output, "installDirectory", manifest.installDirectory, first);
	AppendMember(output, "sizeBytes", std::to_wstring(manifest.sizeBytes), first);
	AppendMember(output, "sha256", manifest.sha256, first);
	AppendMember(output, "releaseUrl", manifest.releaseUrl, first);
	AppendMember(output, "applyOnExit", manifest.applyOnExit ? L"true" : L"false", first);
	AppendMember(output, "lastFailure", manifest.lastFailure, first);
	output.push_back('}');
	return output;
}

std::optional<UpdateManifest> DecodeUpdateManifest(std::string_view utf8)
{
	if (utf8.size() > kMaximumManifestBytes) return std::nullopt;

	const auto parsed = CJsoncDocument::Parse(utf8);
	if (!parsed.Succeeded() || !parsed.value) return std::nullopt;
	const auto* object = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (object == nullptr) return std::nullopt;

	// Every scalar is written as a string, so one decoder covers them all and a
	// number that arrived as a number is simply not this manifest.
	const auto version = ParseProductVersion(StringMember(*object, L"version"));
	if (!version) return std::nullopt;

	UpdateManifest manifest;
	manifest.version = *version;
	manifest.tagName = StringMember(*object, L"tagName");
	manifest.installerPath = StringMember(*object, L"installerPath");
	manifest.installDirectory = StringMember(*object, L"installDirectory");
	manifest.sha256 = StringMember(*object, L"sha256");
	manifest.releaseUrl = StringMember(*object, L"releaseUrl");
	manifest.lastFailure = StringMember(*object, L"lastFailure");
	manifest.applyOnExit = StringMember(*object, L"applyOnExit") == L"true";

	const std::wstring size = StringMember(*object, L"sizeBytes");
	if (size.empty() || size.size() > 20) return std::nullopt;
	std::uint64_t sizeBytes = 0;
	for (const wchar_t ch : size) {
		if (ch < L'0' || ch > L'9') return std::nullopt;
		if (sizeBytes > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(ch - L'0')) / 10U) {
			return std::nullopt;
		}
		sizeBytes = sizeBytes * 10U + static_cast<std::uint64_t>(ch - L'0');
	}
	manifest.sizeBytes = sizeBytes;
	return manifest;
}

UpdateStagingStore::UpdateStagingStore(std::filesystem::path root)
	: m_root(std::move(root))
{
}

std::filesystem::path UpdateStagingStore::DefaultRoot()
{
	PWSTR localAppData = nullptr;
	if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData))
		|| localAppData == nullptr) {
		if (localAppData != nullptr) ::CoTaskMemFree(localAppData);
		return {};
	}
	std::filesystem::path root(localAppData);
	::CoTaskMemFree(localAppData);
	root /= L"sakura-editor-next";
	root /= L"update";
	return root;
}

std::filesystem::path UpdateStagingStore::ManifestPath() const
{
	return m_root / kManifestFileName;
}

std::filesystem::path UpdateStagingStore::BuildDirectory(const UpdateVersion& version) const
{
	return m_root / version.ToProductVersion();
}

std::optional<UpdateManifest> UpdateStagingStore::ReadManifest()
{
	if (m_root.empty()) return std::nullopt;

	std::error_code error;
	const auto path = ManifestPath();
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > kMaximumManifestBytes) return std::nullopt;

	std::ifstream stream(path, std::ios::binary);
	if (!stream) return std::nullopt;
	std::string contents(static_cast<std::size_t>(size), '\0');
	stream.read(contents.data(), static_cast<std::streamsize>(size));
	if (stream.gcount() != static_cast<std::streamsize>(size)) return std::nullopt;
	return DecodeUpdateManifest(contents);
}

bool UpdateStagingStore::WriteManifest(const UpdateManifest& manifest)
{
	if (m_root.empty()) return false;

	std::error_code error;
	std::filesystem::create_directories(m_root, error);
	if (error) return false;

	const std::string contents = EncodeUpdateManifest(manifest);

	// Write beside the manifest and rename over it, so a process that dies
	// mid-write leaves the previous manifest intact rather than a truncated one
	// that would decode to nothing and silently discard a staged update.
	const auto temporary = ManifestPath().replace_extension(L".tmp");
	{
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream) return false;
		stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		if (!stream) return false;
	}
	std::filesystem::rename(temporary, ManifestPath(), error);
	if (error) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

void UpdateStagingStore::ClearManifest() noexcept
{
	if (m_root.empty()) return;
	std::error_code error;
	std::filesystem::remove(ManifestPath(), error);
}

std::optional<std::wstring> UpdateStagingStore::StoreInstaller(
	const UpdateVersion& version, std::wstring_view assetName, const std::vector<std::uint8_t>& bytes)
{
	if (m_root.empty() || assetName.empty()) return std::nullopt;
	// The asset name comes from the feed and is about to become a path component.
	if (assetName.find_first_of(L"\\/:*?\"<>|") != std::wstring_view::npos) return std::nullopt;

	std::error_code error;
	const auto directory = BuildDirectory(version);
	std::filesystem::create_directories(directory, error);
	if (error) return std::nullopt;

	const auto path = directory / assetName;
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream) return std::nullopt;
		if (!bytes.empty()) {
			stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		if (!stream) {
			stream.close();
			std::filesystem::remove(path, error);
			return std::nullopt;
		}
	}
	return path.wstring();
}

bool UpdateStagingStore::InstallerMatches(std::wstring_view installerPath, std::uint64_t sizeBytes)
{
	if (installerPath.empty()) return false;
	std::error_code error;
	const std::filesystem::path path(installerPath);
	const auto size = std::filesystem::file_size(path, error);
	if (error) return false;
	return size == sizeBytes;
}

std::wstring UpdateStagingStore::InstallLogPath(const UpdateVersion& version)
{
	if (m_root.empty()) return {};
	return (BuildDirectory(version) / kInstallLogFileName).wstring();
}

void UpdateStagingStore::RemoveOtherStagedBuilds(const UpdateVersion& keep) noexcept
{
	if (m_root.empty()) return;

	std::error_code error;
	const auto keepName = BuildDirectory(keep).filename();
	std::filesystem::directory_iterator it(m_root, error);
	if (error) return;

	// Collected first: removing while iterating invalidates the iterator, and a
	// cleanup that throws away someone else's directory entry is worse than one
	// that leaves an abandoned download behind.
	std::vector<std::filesystem::path> stale;
	for (const auto& entry : it) {
		std::error_code entryError;
		if (!entry.is_directory(entryError) || entryError) continue;
		if (entry.path().filename() == keepName) continue;
		stale.push_back(entry.path());
	}
	for (const auto& path : stale) {
		std::error_code removeError;
		std::filesystem::remove_all(path, removeError);
	}
}

} // namespace update
