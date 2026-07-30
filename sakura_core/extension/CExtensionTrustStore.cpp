/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionTrustStore.h"

#include "util/string_ex.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uintmax_t kMaximumTrustFileBytes = 1024 * 1024;

struct TrustEntry {
	std::wstring extensionId;
	std::wstring version;
	std::wstring path;
	auto operator<=>(const TrustEntry&) const = default;
};

bool IsValid(std::wstring_view extensionId, std::wstring_view version, std::wstring_view path) noexcept
{
	return !extensionId.empty() && extensionId.size() <= 255 &&
		!version.empty() && version.size() <= 128 && !path.empty() && path.size() <= 32767 &&
		extensionId.find(L'\0') == std::wstring_view::npos &&
		version.find(L'\0') == std::wstring_view::npos && path.find(L'\0') == std::wstring_view::npos;
}

std::wstring NormalizePath(std::wstring_view path)
{
	std::error_code error;
	auto normalized = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
	if (error) normalized = std::filesystem::absolute(std::filesystem::path(path), error);
	if (error) return {};
	auto value = normalized.lexically_normal().wstring();
	std::ranges::transform(value, value.begin(), [](wchar_t character) { return static_cast<wchar_t>(::towlower(character)); });
	return value;
}

bool ReadEntries(const std::filesystem::path& file, std::vector<TrustEntry>& entries)
{
	entries.clear();
	std::error_code error;
	if (!std::filesystem::exists(file, error)) return !error;
	const auto size = std::filesystem::file_size(file, error);
	if (error || size > kMaximumTrustFileBytes) return false;
	std::ifstream input(file, std::ios::binary);
	if (!input) return false;
	std::string json(static_cast<std::size_t>(size), '\0');
	if (size != 0 && !input.read(json.data(), static_cast<std::streamsize>(json.size()))) return false;
	picojson::value root;
	if (!picojson::parse(root, json).empty() || !root.is<picojson::object>()) return false;
	const auto& object = root.get<picojson::object>();
	const auto version = object.find("formatVersion");
	const auto trusted = object.find("trusted");
	if (version == object.end() || !version->second.is<double>() || version->second.get<double>() != 1 ||
		trusted == object.end() || !trusted->second.is<picojson::array>()) return false;
	for (const auto& value : trusted->second.get<picojson::array>()) {
		if (!value.is<picojson::object>()) return false;
		const auto& item = value.get<picojson::object>();
		const auto id = item.find("extensionId");
		const auto itemVersion = item.find("version");
		const auto path = item.find("path");
		if (id == item.end() || !id->second.is<std::string>() ||
			itemVersion == item.end() || !itemVersion->second.is<std::string>() ||
			path == item.end() || !path->second.is<std::string>()) return false;
		TrustEntry entry{ u8stowcs(id->second.get<std::string>()), u8stowcs(itemVersion->second.get<std::string>()),
			u8stowcs(path->second.get<std::string>()) };
		if (!IsValid(entry.extensionId, entry.version, entry.path)) return false;
		entries.emplace_back(std::move(entry));
	}
	return true;
}

bool WriteEntries(const std::filesystem::path& file, const std::vector<TrustEntry>& entries)
{
	picojson::array trusted;
	trusted.reserve(entries.size());
	for (const auto& entry : entries) {
		picojson::object item;
		item["extensionId"] = picojson::value(wcstou8s(entry.extensionId));
		item["path"] = picojson::value(wcstou8s(entry.path));
		item["version"] = picojson::value(wcstou8s(entry.version));
		trusted.emplace_back(std::move(item));
	}
	picojson::object root;
	root["formatVersion"] = picojson::value(1.0);
	root["trusted"] = picojson::value(std::move(trusted));
	const auto json = picojson::value(std::move(root)).serialize();
	std::error_code error;
	std::filesystem::create_directories(file.parent_path(), error);
	if (error) return false;
	const auto temporary = file.wstring() + L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
		std::to_wstring(::GetCurrentThreadId());
	{
		std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
		if (!output || !output.write(json.data(), static_cast<std::streamsize>(json.size()))) return false;
		output.flush();
		if (!output) return false;
	}
	if (!::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		std::filesystem::remove(temporary, error);
		return false;
	}
	return true;
}

} // namespace

CExtensionTrustStore::CExtensionTrustStore(std::filesystem::path storageFile)
	: m_storageFile(std::move(storageFile))
{
}

bool CExtensionTrustStore::IsTrusted(
	std::wstring_view extensionId,
	std::wstring_view version,
	std::wstring_view extensionPath) const
{
	if (!IsValid(extensionId, version, extensionPath)) return false;
	const auto normalizedPath = NormalizePath(extensionPath);
	if (normalizedPath.empty()) return false;
	std::lock_guard lock(m_mutex);
	std::vector<TrustEntry> entries;
	if (!ReadEntries(m_storageFile, entries)) return false;
	return std::ranges::find(entries, TrustEntry{ std::wstring(extensionId), std::wstring(version), normalizedPath }) != entries.end();
}

bool CExtensionTrustStore::Grant(
	std::wstring_view extensionId,
	std::wstring_view version,
	std::wstring_view extensionPath)
{
	if (!IsValid(extensionId, version, extensionPath)) return false;
	TrustEntry entry{ std::wstring(extensionId), std::wstring(version), NormalizePath(extensionPath) };
	if (entry.path.empty()) return false;
	std::lock_guard lock(m_mutex);
	std::vector<TrustEntry> entries;
	if (!ReadEntries(m_storageFile, entries)) entries.clear();
	if (std::ranges::find(entries, entry) == entries.end()) entries.push_back(std::move(entry));
	std::ranges::sort(entries);
	return WriteEntries(m_storageFile, entries);
}

bool CExtensionTrustStore::RevokeAll() noexcept
{
	std::lock_guard lock(m_mutex);
	std::error_code error;
	const bool removed = std::filesystem::remove(m_storageFile, error);
	return !error && (removed || !std::filesystem::exists(m_storageFile, error));
}
