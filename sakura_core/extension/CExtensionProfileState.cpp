/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionProfileState.h"

#include <Windows.h>
#include <picojson/picojson.h>

#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace {

const picojson::value* Find(const picojson::object& object, const char* key)
{
	const auto found = object.find(key);
	return found == object.end() ? nullptr : &found->second;
}

std::uint64_t HashPath(std::wstring_view path) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	for (const wchar_t character : path) {
		hash ^= static_cast<std::uint64_t>(character);
		hash *= 1099511628211ull;
	}
	return hash;
}

//! Serialize writers from separate editor processes for one profile.
class CProfileFileLock final {
public:
	explicit CProfileFileLock(const std::filesystem::path& path) noexcept
	{
		const std::wstring name = L"Local\\SakuraEditorExtensionProfile-" +
			std::to_wstring(HashPath(path.wstring()));
		m_handle = ::CreateMutexW(nullptr, FALSE, name.c_str());
		if (!m_handle) return;
		const DWORD result = ::WaitForSingleObject(m_handle, 2000);
		m_acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
	}

	~CProfileFileLock()
	{
		if (!m_handle) return;
		if (m_acquired) (void)::ReleaseMutex(m_handle);
		(void)::CloseHandle(m_handle);
	}

	CProfileFileLock(const CProfileFileLock&) = delete;
	CProfileFileLock& operator=(const CProfileFileLock&) = delete;
	[[nodiscard]] bool Acquired() const noexcept { return m_acquired; }

private:
	HANDLE m_handle = nullptr;
	bool m_acquired = false;
};

} // namespace

std::wstring CExtensionProfileState::CanonicalizeExtensionId(std::wstring_view extensionId) noexcept
{
	std::wstring canonical(extensionId);
	for (wchar_t& character : canonical) {
		if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character + (L'a' - L'A'));
	}
	return canonical;
}

bool CExtensionProfileState::IsSafeExtensionId(std::string_view extensionId) noexcept
{
	if (extensionId.empty() || extensionId.size() > 256) return false;
	for (const unsigned char character : extensionId) {
		if ((character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '.' || character == '-' || character == '_') {
			continue;
		}
		return false;
	}
	return true;
}

bool CExtensionProfileState::IsSafeExtensionId(std::wstring_view extensionId) noexcept
{
	if (extensionId.empty() || extensionId.size() > 256) return false;
	for (const wchar_t character : extensionId) {
		if ((character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') ||
			character == L'.' || character == L'-' || character == L'_') {
			continue;
		}
		return false;
	}
	return true;
}

CExtensionProfileState::Snapshot CExtensionProfileState::Load() const
{
	Snapshot snapshot;
	if (m_path.empty()) return snapshot;

	std::error_code fileError;
	const auto status = std::filesystem::status(m_path, fileError);
	if (fileError) {
		if (fileError == std::errc::no_such_file_or_directory) {
			snapshot.status = EStatus::Missing;
		} else {
			snapshot.status = EStatus::IoError;
		}
		return snapshot;
	}
	if (!std::filesystem::exists(status)) {
		snapshot.status = EStatus::Missing;
		return snapshot;
	}
	if (!std::filesystem::is_regular_file(status)) {
		snapshot.status = EStatus::Invalid;
		return snapshot;
	}

	const auto size = std::filesystem::file_size(m_path, fileError);
	if (fileError) {
		snapshot.status = EStatus::IoError;
		return snapshot;
	}
	if (size > kMaximumFileBytes || size > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
		snapshot.status = EStatus::Invalid;
		return snapshot;
	}

	std::ifstream input(m_path, std::ios::binary);
	if (!input) {
		snapshot.status = EStatus::IoError;
		return snapshot;
	}
	std::string json(static_cast<std::size_t>(size), '\0');
	if (!json.empty()) input.read(json.data(), static_cast<std::streamsize>(json.size()));
	if (!input && !input.eof()) {
		snapshot.status = EStatus::IoError;
		return snapshot;
	}

	picojson::value rootValue;
	const std::string parseError = picojson::parse(rootValue, json);
	if (!parseError.empty() || !rootValue.is<picojson::object>()) {
		snapshot.status = EStatus::Invalid;
		return snapshot;
	}
	const auto& root = rootValue.get<picojson::object>();
	const auto* version = Find(root, "version");
	const auto* extensions = Find(root, "extensions");
	if (!version || !version->is<double>() || version->get<double>() != static_cast<double>(kCurrentVersion) ||
		!extensions || !extensions->is<picojson::object>() ||
		extensions->get<picojson::object>().size() > kMaximumExtensionCount) {
		snapshot.status = EStatus::Invalid;
		return snapshot;
	}

	for (const auto& [key, value] : extensions->get<picojson::object>()) {
		if (!IsSafeExtensionId(key) || !value.is<bool>()) {
			snapshot.enabled.clear();
			snapshot.status = EStatus::Invalid;
			return snapshot;
		}
		std::wstring id;
		id.reserve(key.size());
		for (const char character : key) id.push_back(static_cast<wchar_t>(character));
		snapshot.enabled.emplace(CanonicalizeExtensionId(id), value.get<bool>());
	}
	snapshot.status = EStatus::Valid;
	return snapshot;
}

bool CExtensionProfileState::IsEnabled(
	const Snapshot& snapshot,
	std::wstring_view extensionId,
	bool defaultWhenAbsent) noexcept
{
	if (!IsSafeExtensionId(extensionId) || snapshot.status == EStatus::Invalid || snapshot.status == EStatus::IoError) {
		return false;
	}
	const auto found = snapshot.enabled.find(CanonicalizeExtensionId(extensionId));
	return found == snapshot.enabled.end() ? defaultWhenAbsent : found->second;
}

bool CExtensionProfileState::Write(const Snapshot& snapshot) const
{
	if (m_path.empty() || snapshot.status == EStatus::Invalid || snapshot.status == EStatus::IoError) return false;
	if (snapshot.enabled.size() > kMaximumExtensionCount) return false;
	std::error_code directoryError;
	if (!std::filesystem::create_directories(m_path.parent_path(), directoryError) && directoryError) return false;

	picojson::object extensions;
	for (const auto& [id, enabled] : snapshot.enabled) {
		std::string key;
		key.reserve(id.size());
		for (const wchar_t character : id) key.push_back(static_cast<char>(character));
		extensions[key] = picojson::value(enabled);
	}
	picojson::object root;
	root["version"] = picojson::value(static_cast<double>(kCurrentVersion));
	root["extensions"] = picojson::value(std::move(extensions));
	const std::string json = picojson::value(std::move(root)).serialize() + "\n";
	if (json.size() > kMaximumFileBytes) return false;

	const std::filesystem::path temporary = m_path.wstring() +
		L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
		std::to_wstring(::GetTickCount64());
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) return false;
		output.write(json.data(), static_cast<std::streamsize>(json.size()));
		output.flush();
		if (!output) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			return false;
		}
	}
	if (!::MoveFileExW(temporary.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		return false;
	}
	return true;
}

bool CExtensionProfileState::SetEnabled(std::wstring_view extensionId, bool enabled) const
{
	if (!IsSafeExtensionId(extensionId) || m_path.empty()) return false;
	CProfileFileLock lock(m_path);
	if (!lock.Acquired()) return false;
	Snapshot snapshot = Load();
	if (snapshot.status == EStatus::Invalid || snapshot.status == EStatus::IoError) return false;
	snapshot.enabled[CanonicalizeExtensionId(extensionId)] = enabled;
	snapshot.status = EStatus::Valid;
	return Write(snapshot);
}

bool CExtensionProfileState::Remove(std::wstring_view extensionId) const
{
	if (!IsSafeExtensionId(extensionId) || m_path.empty()) return false;
	CProfileFileLock lock(m_path);
	if (!lock.Acquired()) return false;
	Snapshot snapshot = Load();
	if (snapshot.status == EStatus::Invalid || snapshot.status == EStatus::IoError) return false;
	if (snapshot.status == EStatus::Missing) return true;
	snapshot.enabled.erase(CanonicalizeExtensionId(extensionId));
	return Write(snapshot);
}
