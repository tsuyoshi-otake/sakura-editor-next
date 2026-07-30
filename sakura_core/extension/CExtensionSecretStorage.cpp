/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionSecretStorage.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>

#include <aclapi.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <windows.h>

namespace {

constexpr std::array<std::uint8_t, 8> StorageMagic = { 'S', 'A', 'K', 'S', 'E', 'C', '0', '1' };
constexpr std::array<std::uint8_t, 8> PlaintextMagic = { 'S', 'E', 'C', 'R', 'E', 'T', '0', '1' };
constexpr std::size_t MaximumKeyCharacters = 4 * 1024;
constexpr std::size_t MaximumValueCharacters = 1024 * 1024;
constexpr std::size_t MaximumStorageBytes = 8 * 1024 * 1024;

class UniqueHandle final {
public:
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
	~UniqueHandle() { Reset(); }
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.Release()) {}
	UniqueHandle& operator=(UniqueHandle&& other) noexcept
	{
		if (this != &other) Reset(other.Release());
		return *this;
	}
	HANDLE Get() const noexcept { return m_value; }
	explicit operator bool() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (*this) ::CloseHandle(m_value);
		m_value = value;
	}
	HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
private:
	HANDLE m_value = nullptr;
};

class AlgorithmHandle final {
public:
	~AlgorithmHandle() { if (m_value) ::BCryptCloseAlgorithmProvider(m_value, 0); }
	BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }
private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

class CurrentUserSecurity final {
public:
	~CurrentUserSecurity()
	{
		if (m_acl) ::LocalFree(m_acl);
	}

	DWORD Initialize()
	{
		HANDLE rawToken = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return ::GetLastError();
		UniqueHandle token(rawToken);
		DWORD userBytes = 0;
		::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &userBytes);
		if (userBytes == 0) return ::GetLastError();
		std::vector<std::uint8_t> userStorage(userBytes);
		if (!::GetTokenInformation(token.Get(), TokenUser, userStorage.data(), userBytes, &userBytes)) {
			return ::GetLastError();
		}
		auto* tokenUser = reinterpret_cast<TOKEN_USER*>(userStorage.data());
		EXPLICIT_ACCESSW access{};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = static_cast<wchar_t*>(tokenUser->User.Sid);
		const DWORD aclError = ::SetEntriesInAclW(1, &access, nullptr, &m_acl);
		if (aclError != ERROR_SUCCESS) return aclError;
		if (!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
			!::SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE)) {
			return ::GetLastError();
		}
		m_attributes.nLength = sizeof(m_attributes);
		m_attributes.lpSecurityDescriptor = &m_descriptor;
		m_attributes.bInheritHandle = FALSE;
		return ERROR_SUCCESS;
	}

	SECURITY_ATTRIBUTES* Attributes() noexcept { return &m_attributes; }
	PACL Acl() const noexcept { return m_acl; }

private:
	PACL m_acl = nullptr;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
};

SExtensionSecretStorageResult Success()
{
	return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
}

SExtensionSecretStorageResult Failure(
	EExtensionSecretStorageStatus status,
	DWORD error,
	std::wstring operation)
{
	return { false, status, error, L"SecretStorage " + std::move(operation) +
		L" failed (error " + std::to_wstring(error) + L")" };
}

bool IsValidExtensionId(std::wstring_view value)
{
	if (value.empty() || value.size() > 255 || value.front() == L'.' || value.back() == L'.') return false;
	bool hasSeparator = false;
	for (const wchar_t character : value) {
		if (character == L'.') hasSeparator = true;
		else if (!((character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') || character == L'-' || character == L'_')) return false;
	}
	return hasSeparator;
}

bool IsValidKey(std::wstring_view value)
{
	return !value.empty() && value.size() <= MaximumKeyCharacters &&
		value.find(L'\0') == std::wstring_view::npos;
}

bool ToUtf8(std::wstring_view value, std::string& result)
{
	result.clear();
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const int bytes = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return false;
	result.resize(bytes);
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) == bytes;
}

bool FromUtf8(std::string_view value, std::wstring& result)
{
	result.clear();
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const int characters = ::MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (characters <= 0) return false;
	result.resize(characters);
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), characters) == characters;
}

bool Sha256Hex(std::string_view value, std::wstring& result)
{
	AlgorithmHandle algorithm;
	if (::BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
	std::array<std::uint8_t, 32> digest{};
	if (::BCryptHash(
		algorithm.Get(),
		nullptr,
		0,
		reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
		static_cast<ULONG>(value.size()),
		digest.data(),
		static_cast<ULONG>(digest.size())) < 0) return false;
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	result.clear();
	result.reserve(digest.size() * 2);
	for (const auto byte : digest) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	return true;
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
	bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
	bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
	bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

bool ReadU32(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint32_t& value)
{
	if (offset > bytes.size() || bytes.size() - offset < 4) return false;
	value = static_cast<std::uint32_t>(bytes[offset]) |
		(static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
		(static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
		(static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	offset += 4;
	return true;
}

std::vector<std::uint8_t> BuildEntropy(std::string_view extensionId)
{
	static constexpr std::string_view prefix = "sakura-editor-next/SecretStorage/v1\0";
	std::vector<std::uint8_t> entropy;
	entropy.reserve(prefix.size() + extensionId.size());
	entropy.insert(entropy.end(), prefix.begin(), prefix.end());
	entropy.insert(entropy.end(), extensionId.begin(), extensionId.end());
	return entropy;
}

bool Protect(
	std::span<const std::uint8_t> plaintext,
	std::span<std::uint8_t> entropy,
	std::vector<std::uint8_t>& encrypted,
	DWORD& error)
{
	DATA_BLOB input{ static_cast<DWORD>(plaintext.size()), const_cast<BYTE*>(plaintext.data()) };
	DATA_BLOB optionalEntropy{ static_cast<DWORD>(entropy.size()), entropy.data() };
	DATA_BLOB output{};
	if (!::CryptProtectData(&input, L"Sakura Editor NEXT SecretStorage", &optionalEntropy,
			nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		error = ::GetLastError();
		return false;
	}
	encrypted.assign(output.pbData, output.pbData + output.cbData);
	::SecureZeroMemory(output.pbData, output.cbData);
	::LocalFree(output.pbData);
	return true;
}

bool Unprotect(
	std::span<const std::uint8_t> encrypted,
	std::span<std::uint8_t> entropy,
	std::vector<std::uint8_t>& plaintext,
	DWORD& error)
{
	DATA_BLOB input{ static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data()) };
	DATA_BLOB optionalEntropy{ static_cast<DWORD>(entropy.size()), entropy.data() };
	DATA_BLOB output{};
	LPWSTR description = nullptr;
	if (!::CryptUnprotectData(&input, &description, &optionalEntropy, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		error = ::GetLastError();
		return false;
	}
	if (description) ::LocalFree(description);
	plaintext.assign(output.pbData, output.pbData + output.cbData);
	::SecureZeroMemory(output.pbData, output.cbData);
	::LocalFree(output.pbData);
	return true;
}

DWORD RestrictExistingDirectory(const std::filesystem::path& path, PACL acl)
{
	return ::SetNamedSecurityInfoW(
		const_cast<wchar_t*>(path.c_str()),
		SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
		nullptr,
		nullptr,
		acl,
		nullptr);
}

DWORD EnsureDirectory(const std::filesystem::path& path, CurrentUserSecurity& security)
{
	if (!::CreateDirectoryW(path.c_str(), security.Attributes())) {
		const DWORD error = ::GetLastError();
		if (error != ERROR_ALREADY_EXISTS) return error;
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
			return ERROR_DIRECTORY;
		}
	}
	return RestrictExistingDirectory(path, security.Acl());
}

bool WriteAll(HANDLE file, std::span<const std::uint8_t> bytes, DWORD& error)
{
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const DWORD request = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - offset, 1024 * 1024));
		DWORD written = 0;
		if (!::WriteFile(file, bytes.data() + offset, request, &written, nullptr) || written == 0) {
			error = ::GetLastError();
			return false;
		}
		offset += written;
	}
	return true;
}

bool ReadAll(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, DWORD& error)
{
	UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	if (!file) {
		error = ::GetLastError();
		return false;
	}
	LARGE_INTEGER size{};
	if (!::GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 ||
		size.QuadPart > static_cast<LONGLONG>(MaximumStorageBytes)) {
		error = ::GetLastError() == ERROR_SUCCESS ? ERROR_FILE_TOO_LARGE : ::GetLastError();
		return false;
	}
	bytes.resize(static_cast<std::size_t>(size.QuadPart));
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		DWORD read = 0;
		const DWORD request = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - offset, 1024 * 1024));
		if (!::ReadFile(file.Get(), bytes.data() + offset, request, &read, nullptr) || read == 0) {
			error = ::GetLastError();
			return false;
		}
		offset += read;
	}
	return true;
}

bool BuildPaths(
	const std::filesystem::path& root,
	std::wstring_view extensionId,
	std::wstring_view key,
	std::filesystem::path& extensionDirectory,
	std::filesystem::path& secretPath,
	std::string& extensionUtf8)
{
	std::string keyUtf8;
	if (!ToUtf8(extensionId, extensionUtf8) || !ToUtf8(key, keyUtf8)) return false;
	std::wstring extensionHash;
	std::wstring keyHash;
	if (!Sha256Hex(extensionUtf8, extensionHash)) return false;
	std::string identity = extensionUtf8;
	identity.push_back('\0');
	identity.append(keyUtf8);
	if (!Sha256Hex(identity, keyHash)) return false;
	extensionDirectory = root / extensionHash;
	secretPath = extensionDirectory / (keyHash + L".bin");
	return true;
}

SExtensionSecretStorageResult WriteStorageFile(
	const std::filesystem::path& path,
	std::span<const std::uint8_t> bytes,
	CurrentUserSecurity& security)
{
	std::array<std::uint8_t, 8> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return Failure(EExtensionSecretStorageStatus::CryptoError, ERROR_GEN_FAILURE, L"generate temporary name");
	}
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring suffix;
	for (const auto byte : random) {
		suffix.push_back(digits[byte >> 4]);
		suffix.push_back(digits[byte & 0x0f]);
	}
	const auto temporary = path.parent_path() / (path.filename().wstring() + L"." + suffix + L".tmp");
	UniqueHandle file(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security.Attributes(), CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file) return Failure(EExtensionSecretStorageStatus::IoError, ::GetLastError(), L"create temporary file");
	DWORD error = ERROR_SUCCESS;
	if (!WriteAll(file.Get(), bytes, error) || !::FlushFileBuffers(file.Get())) {
		if (error == ERROR_SUCCESS) error = ::GetLastError();
		file.Reset();
		::DeleteFileW(temporary.c_str());
		return Failure(EExtensionSecretStorageStatus::IoError, error, L"write encrypted file");
	}
	file.Reset();
	if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error = ::GetLastError();
		::DeleteFileW(temporary.c_str());
		return Failure(EExtensionSecretStorageStatus::IoError, error, L"commit encrypted file");
	}
	return Success();
}

SExtensionSecretStorageResult DecodeStorage(
	std::span<const std::uint8_t> storage,
	std::string_view extensionId,
	std::wstring& key,
	std::wstring& value)
{
	if (storage.size() < StorageMagic.size() + 4 ||
		!std::equal(StorageMagic.begin(), StorageMagic.end(), storage.begin())) {
		return Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_INVALID_DATA, L"read header");
	}
	std::size_t offset = StorageMagic.size();
	std::uint32_t encryptedBytes = 0;
	if (!ReadU32(storage, offset, encryptedBytes) || encryptedBytes != storage.size() - offset) {
		return Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_INVALID_DATA, L"read encrypted length");
	}
	auto entropy = BuildEntropy(extensionId);
	std::vector<std::uint8_t> plaintext;
	DWORD cryptoError = ERROR_SUCCESS;
	if (!Unprotect(storage.subspan(offset), entropy, plaintext, cryptoError)) {
		return Failure(EExtensionSecretStorageStatus::CryptoError, cryptoError, L"decrypt");
	}
	auto clearPlaintext = [&] {
		if (!plaintext.empty()) ::SecureZeroMemory(plaintext.data(), plaintext.size());
	};
	if (plaintext.size() < PlaintextMagic.size() + 8 ||
		!std::equal(PlaintextMagic.begin(), PlaintextMagic.end(), plaintext.begin())) {
		clearPlaintext();
		return Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_INVALID_DATA, L"validate plaintext");
	}
	offset = PlaintextMagic.size();
	std::uint32_t keyBytes = 0;
	std::uint32_t valueBytes = 0;
	if (!ReadU32(plaintext, offset, keyBytes) || !ReadU32(plaintext, offset, valueBytes) ||
		offset > plaintext.size() || keyBytes > plaintext.size() - offset ||
		valueBytes != plaintext.size() - offset - keyBytes) {
		clearPlaintext();
		return Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_INVALID_DATA, L"validate payload lengths");
	}
	const auto keyView = std::string_view(reinterpret_cast<const char*>(plaintext.data() + offset), keyBytes);
	offset += keyBytes;
	const auto valueView = std::string_view(reinterpret_cast<const char*>(plaintext.data() + offset), valueBytes);
	if (!FromUtf8(keyView, key) || !FromUtf8(valueView, value)) {
		clearPlaintext();
		return Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_NO_UNICODE_TRANSLATION, L"decode payload");
	}
	clearPlaintext();
	return Success();
}

} // namespace

CExtensionSecretStorage::CExtensionSecretStorage(std::filesystem::path rootDirectory)
	: m_rootDirectory(std::move(rootDirectory))
{
}

SExtensionSecretStorageResult CExtensionSecretStorage::Store(
	std::wstring_view extensionId,
	std::wstring_view key,
	std::wstring_view value)
{
	if (!IsValidExtensionId(extensionId) || !IsValidKey(key) || value.size() > MaximumValueCharacters ||
		value.find(L'\0') != std::wstring_view::npos || m_rootDirectory.empty()) {
		return Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER, L"validate arguments");
	}
	std::lock_guard lock(m_mutex);
	CurrentUserSecurity security;
	DWORD error = security.Initialize();
	if (error != ERROR_SUCCESS) return Failure(EExtensionSecretStorageStatus::IoError, error, L"create ACL");
	if ((error = EnsureDirectory(m_rootDirectory, security)) != ERROR_SUCCESS) {
		return Failure(EExtensionSecretStorageStatus::IoError, error, L"create root directory");
	}
	std::filesystem::path extensionDirectory;
	std::filesystem::path secretPath;
	std::string extensionUtf8;
	if (!BuildPaths(m_rootDirectory, extensionId, key, extensionDirectory, secretPath, extensionUtf8)) {
		return Failure(EExtensionSecretStorageStatus::CryptoError, ERROR_INVALID_DATA, L"derive namespace");
	}
	if ((error = EnsureDirectory(extensionDirectory, security)) != ERROR_SUCCESS) {
		return Failure(EExtensionSecretStorageStatus::IoError, error, L"create extension directory");
	}

	std::string keyUtf8;
	std::string valueUtf8;
	if (!ToUtf8(key, keyUtf8) || !ToUtf8(value, valueUtf8)) {
		return Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_NO_UNICODE_TRANSLATION, L"encode value");
	}
	std::vector<std::uint8_t> plaintext;
	plaintext.reserve(PlaintextMagic.size() + 8 + keyUtf8.size() + valueUtf8.size());
	plaintext.insert(plaintext.end(), PlaintextMagic.begin(), PlaintextMagic.end());
	AppendU32(plaintext, static_cast<std::uint32_t>(keyUtf8.size()));
	AppendU32(plaintext, static_cast<std::uint32_t>(valueUtf8.size()));
	plaintext.insert(plaintext.end(), keyUtf8.begin(), keyUtf8.end());
	plaintext.insert(plaintext.end(), valueUtf8.begin(), valueUtf8.end());
	if (!valueUtf8.empty()) ::SecureZeroMemory(valueUtf8.data(), valueUtf8.size());

	auto entropy = BuildEntropy(extensionUtf8);
	std::vector<std::uint8_t> encrypted;
	if (!Protect(plaintext, entropy, encrypted, error)) {
		::SecureZeroMemory(plaintext.data(), plaintext.size());
		return Failure(EExtensionSecretStorageStatus::CryptoError, error, L"encrypt");
	}
	::SecureZeroMemory(plaintext.data(), plaintext.size());
	std::vector<std::uint8_t> storage(StorageMagic.begin(), StorageMagic.end());
	AppendU32(storage, static_cast<std::uint32_t>(encrypted.size()));
	storage.insert(storage.end(), encrypted.begin(), encrypted.end());
	return WriteStorageFile(secretPath, storage, security);
}

SExtensionSecretReadResult CExtensionSecretStorage::Get(std::wstring_view extensionId, std::wstring_view key)
{
	SExtensionSecretReadResult result;
	if (!IsValidExtensionId(extensionId) || !IsValidKey(key) || m_rootDirectory.empty()) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER, L"validate arguments");
		return result;
	}
	std::lock_guard lock(m_mutex);
	std::filesystem::path extensionDirectory;
	std::filesystem::path secretPath;
	std::string extensionUtf8;
	if (!BuildPaths(m_rootDirectory, extensionId, key, extensionDirectory, secretPath, extensionUtf8)) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::CryptoError, ERROR_INVALID_DATA, L"derive namespace");
		return result;
	}
	std::vector<std::uint8_t> storage;
	DWORD error = ERROR_SUCCESS;
	if (!ReadAll(secretPath, storage, error)) {
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			static_cast<SExtensionSecretStorageResult&>(result) = Success();
			return result;
		}
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::IoError, error, L"read encrypted file");
		return result;
	}
	std::wstring storedKey;
	std::wstring value;
	auto decoded = DecodeStorage(storage, extensionUtf8, storedKey, value);
	if (!decoded.success || storedKey != key) {
		if (decoded.success) decoded = Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_INVALID_DATA, L"validate key");
		if (!value.empty()) ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
		static_cast<SExtensionSecretStorageResult&>(result) = std::move(decoded);
		return result;
	}
	static_cast<SExtensionSecretStorageResult&>(result) = Success();
	result.value = std::move(value);
	return result;
}

SExtensionSecretStorageResult CExtensionSecretStorage::Delete(
	std::wstring_view extensionId,
	std::wstring_view key)
{
	if (!IsValidExtensionId(extensionId) || !IsValidKey(key) || m_rootDirectory.empty()) {
		return Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER, L"validate arguments");
	}
	std::lock_guard lock(m_mutex);
	std::filesystem::path extensionDirectory;
	std::filesystem::path secretPath;
	std::string extensionUtf8;
	if (!BuildPaths(m_rootDirectory, extensionId, key, extensionDirectory, secretPath, extensionUtf8)) {
		return Failure(EExtensionSecretStorageStatus::CryptoError, ERROR_INVALID_DATA, L"derive namespace");
	}
	if (::DeleteFileW(secretPath.c_str())) return Success();
	const DWORD error = ::GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return Success();
	return Failure(EExtensionSecretStorageStatus::IoError, error, L"delete encrypted file");
}

SExtensionSecretKeysResult CExtensionSecretStorage::Keys(std::wstring_view extensionId)
{
	SExtensionSecretKeysResult result;
	if (!IsValidExtensionId(extensionId) || m_rootDirectory.empty()) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER, L"validate arguments");
		return result;
	}
	std::lock_guard lock(m_mutex);
	std::filesystem::path extensionDirectory;
	std::filesystem::path ignoredPath;
	std::string extensionUtf8;
	if (!BuildPaths(m_rootDirectory, extensionId, L"enumeration", extensionDirectory, ignoredPath, extensionUtf8)) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::CryptoError, ERROR_INVALID_DATA, L"derive namespace");
		return result;
	}
	std::error_code directoryError;
	if (!std::filesystem::is_directory(extensionDirectory, directoryError)) {
		if (!directoryError || directoryError.value() == ERROR_FILE_NOT_FOUND ||
			directoryError.value() == ERROR_PATH_NOT_FOUND) {
			static_cast<SExtensionSecretStorageResult&>(result) = Success();
			return result;
		}
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::IoError, static_cast<DWORD>(directoryError.value()), L"enumerate directory");
		return result;
	}
	for (std::filesystem::directory_iterator it(extensionDirectory, directoryError), end;
		!directoryError && it != end; it.increment(directoryError)) {
		if (!it->is_regular_file() || it->path().extension() != L".bin") continue;
		std::vector<std::uint8_t> storage;
		DWORD error = ERROR_SUCCESS;
		if (!ReadAll(it->path(), storage, error)) {
			static_cast<SExtensionSecretStorageResult&>(result) =
				Failure(EExtensionSecretStorageStatus::IoError, error, L"enumerate encrypted file");
			return result;
		}
		std::wstring key;
		std::wstring value;
		auto decoded = DecodeStorage(storage, extensionUtf8, key, value);
		if (!value.empty()) ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
		if (!decoded.success) {
			static_cast<SExtensionSecretStorageResult&>(result) = std::move(decoded);
			return result;
		}
		result.keys.emplace_back(std::move(key));
	}
	if (directoryError) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::IoError, static_cast<DWORD>(directoryError.value()), L"enumerate directory");
		return result;
	}
	std::sort(result.keys.begin(), result.keys.end());
	static_cast<SExtensionSecretStorageResult&>(result) = Success();
	return result;
}
