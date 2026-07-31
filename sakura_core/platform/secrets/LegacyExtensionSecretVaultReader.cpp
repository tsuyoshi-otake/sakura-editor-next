/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/LegacyExtensionSecretVaultReader.h"

#include "platform/secrets/SecretVaultTypes.h"

#include <array>
#include <set>

#include <bcrypt.h>
#include <dpapi.h>

namespace platform::secrets {
namespace {

constexpr std::array<std::uint8_t, 8> kStorageMagic = { 'S', 'A', 'K', 'S', 'E', 'C', '0', '1' };
constexpr std::array<std::uint8_t, 8> kPlaintextMagic = { 'S', 'E', 'C', 'R', 'E', 'T', '0', '1' };
constexpr std::size_t kMaximumLegacyStorageBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumLegacyPlaintextBytes =
	kPlaintextMagic.size() + 2 * sizeof(std::uint32_t)
	+ kMaximumSecretVaultKeyBytes + kMaximumSecretVaultValueBytes;
//! Bounds the complete encrypted namespace before any file is decrypted.  The
//! allowance above the plaintext limit covers one DPAPI envelope per entry.
constexpr std::uintmax_t kMaximumLegacyNamespaceStorageBytes = 16 * 1024 * 1024;

class CAlgorithmHandle final {
public:
	~CAlgorithmHandle() { if (m_value) ::BCryptCloseAlgorithmProvider(m_value, 0); }
	BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }

private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

class CUniqueHandle final {
public:
	explicit CUniqueHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~CUniqueHandle() { if (m_value && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value); }
	CUniqueHandle(const CUniqueHandle&) = delete;
	CUniqueHandle& operator=(const CUniqueHandle&) = delete;

	explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }
	HANDLE Get() const noexcept { return m_value; }

private:
	HANDLE m_value;
};

LegacyExtensionSecretVaultReadResult Result(ELegacyExtensionSecretVaultReadStatus status, DWORD error = ERROR_SUCCESS)
{
	return { .status = status, .errorCode = error };
}

void SecureClear(std::vector<std::uint8_t>& value) noexcept
{
	if (!value.empty()) ::SecureZeroMemory(value.data(), value.size());
	value.clear();
}

void SecureClear(std::string& value) noexcept
{
	if (!value.empty()) ::SecureZeroMemory(value.data(), value.size());
	value.clear();
}

void SecureClear(std::vector<LegacyExtensionSecretVaultEntry>& entries) noexcept
{
	for (auto& entry : entries) {
		SecureClear(entry.key);
		SecureClear(entry.value);
	}
	entries.clear();
}

bool IsNotFound(DWORD error) noexcept
{
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool ReadU32(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint32_t& value) noexcept
{
	if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) return false;
	value = static_cast<std::uint32_t>(bytes[offset])
		| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
		| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
		| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	offset += sizeof(value);
	return true;
}

bool Sha256Hex(std::string_view value, std::wstring& result)
{
	CAlgorithmHandle algorithm;
	if (::BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
	std::array<std::uint8_t, 32> digest{};
	if (::BCryptHash(algorithm.Get(), nullptr, 0,
		reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()),
		digest.data(), static_cast<ULONG>(digest.size())) < 0) return false;
	static constexpr wchar_t kDigits[] = L"0123456789abcdef";
	result.clear();
	result.reserve(digest.size() * 2);
	for (const auto byte : digest) {
		result.push_back(kDigits[byte >> 4]);
		result.push_back(kDigits[byte & 0x0f]);
	}
	return true;
}

std::vector<std::uint8_t> BuildEntropy(std::string_view extensionId)
{
	static constexpr std::string_view kPrefix = "sakura-editor-next/SecretStorage/v1\0";
	std::vector<std::uint8_t> entropy;
	entropy.reserve(kPrefix.size() + extensionId.size());
	entropy.insert(entropy.end(), kPrefix.begin(), kPrefix.end());
	entropy.insert(entropy.end(), extensionId.begin(), extensionId.end());
	return entropy;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, DWORD& error)
{
	CUniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	if (!file) {
		error = ::GetLastError();
		return false;
	}
	LARGE_INTEGER size{};
	if (!::GetFileSizeEx(file.Get(), &size)) {
		error = ::GetLastError();
		return false;
	}
	if (size.QuadPart < 0) {
		error = ERROR_INVALID_DATA;
		return false;
	}
	if (size.QuadPart > static_cast<LONGLONG>(kMaximumLegacyStorageBytes)) {
		error = ERROR_FILE_TOO_LARGE;
		return false;
	}
	bytes.resize(static_cast<std::size_t>(size.QuadPart));
	for (std::size_t offset = 0; offset < bytes.size();) {
		const DWORD request = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - offset, 1024 * 1024));
		DWORD read = 0;
		if (!::ReadFile(file.Get(), bytes.data() + offset, request, &read, nullptr) || read == 0) {
			error = ::GetLastError();
			return false;
		}
		offset += read;
	}
	return true;
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) noexcept
{
	const auto relative = child.lexically_relative(parent);
	return !relative.empty() && relative.native() != L".."
		&& (relative.begin() == relative.end() || *relative.begin() != L"..");
}

bool CanonicalPathWithin(const std::filesystem::path& path, const std::filesystem::path& parent) noexcept
{
	std::error_code error;
	const auto canonical = std::filesystem::weakly_canonical(path, error);
	return !error && IsWithin(canonical, parent);
}

LegacyExtensionSecretVaultReadResult DecodeStorage(
	std::vector<std::uint8_t>& storage,
	std::string_view extensionId,
	LegacyExtensionSecretVaultEntry& entry)
{
	if (storage.size() < kStorageMagic.size() + sizeof(std::uint32_t)
		|| !std::equal(kStorageMagic.begin(), kStorageMagic.end(), storage.begin())) {
		return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}
	std::size_t offset = kStorageMagic.size();
	std::uint32_t encryptedBytes = 0;
	if (!ReadU32(storage, offset, encryptedBytes) || encryptedBytes != storage.size() - offset) {
		return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}

	auto entropy = BuildEntropy(extensionId);
	DATA_BLOB input{ static_cast<DWORD>(encryptedBytes), storage.data() + offset };
	DATA_BLOB optionalEntropy{ static_cast<DWORD>(entropy.size()), entropy.data() };
	DATA_BLOB output{};
	LPWSTR description = nullptr;
	if (!::CryptUnprotectData(&input, &description, &optionalEntropy, nullptr, nullptr,
		CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		const DWORD error = ::GetLastError();
		if (description) ::LocalFree(description);
		SecureClear(entropy);
		return Result(ELegacyExtensionSecretVaultReadStatus::CryptoError, error);
	}
	if (description) ::LocalFree(description);
	SecureClear(entropy);
	if (!output.pbData || output.cbData == 0 || output.cbData > kMaximumLegacyPlaintextBytes) {
		if (output.pbData) {
			::SecureZeroMemory(output.pbData, output.cbData);
			::LocalFree(output.pbData);
		}
		return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData,
			output.cbData > kMaximumLegacyPlaintextBytes ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA);
	}
	std::vector<std::uint8_t> plaintext(output.pbData, output.pbData + output.cbData);
	::SecureZeroMemory(output.pbData, output.cbData);
	::LocalFree(output.pbData);

	auto finish = [&](ELegacyExtensionSecretVaultReadStatus status, DWORD error) {
		SecureClear(plaintext);
		return Result(status, error);
	};
	if (plaintext.size() < kPlaintextMagic.size() + 2 * sizeof(std::uint32_t)
		|| !std::equal(kPlaintextMagic.begin(), kPlaintextMagic.end(), plaintext.begin())) {
		return finish(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}
	offset = kPlaintextMagic.size();
	std::uint32_t keyBytes = 0;
	std::uint32_t valueBytes = 0;
	if (!ReadU32(plaintext, offset, keyBytes) || !ReadU32(plaintext, offset, valueBytes)
		|| offset > plaintext.size() || keyBytes > plaintext.size() - offset
		|| valueBytes != plaintext.size() - offset - keyBytes
		|| keyBytes > kMaximumSecretVaultKeyBytes || valueBytes > kMaximumSecretVaultValueBytes) {
		return finish(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}

	entry.key.assign(reinterpret_cast<const char*>(plaintext.data() + offset), keyBytes);
	offset += keyBytes;
	entry.value.assign(reinterpret_cast<const char*>(plaintext.data() + offset), valueBytes);
	SecureClear(plaintext);
	if (!IsValidSecretVaultIdentifier(entry.key, kMaximumSecretVaultKeyBytes)
		|| !IsValidSecretVaultUtf8(entry.value)) {
		SecureClear(entry.key);
		SecureClear(entry.value);
		return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}
	return Result(ELegacyExtensionSecretVaultReadStatus::Success);
}

} // namespace

CLegacyExtensionSecretVaultReader::CLegacyExtensionSecretVaultReader(std::filesystem::path legacyBasePath)
	: m_legacyBasePath(std::move(legacyBasePath))
{
}

LegacyExtensionSecretVaultReadResult CLegacyExtensionSecretVaultReader::ReadAll(std::string_view extensionId) const
{
	std::string canonicalExtensionId;
	if (m_legacyBasePath.empty() || !CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
		|| canonicalExtensionId != extensionId) {
		return Result(ELegacyExtensionSecretVaultReadStatus::InvalidArgument, ERROR_INVALID_PARAMETER);
	}

	const DWORD rootAttributes = ::GetFileAttributesW(m_legacyBasePath.c_str());
	if (rootAttributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD error = ::GetLastError();
		return IsNotFound(error) ? Result(ELegacyExtensionSecretVaultReadStatus::Absent)
			: Result(ELegacyExtensionSecretVaultReadStatus::IoError, error);
	}
	if (!(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return Result(ELegacyExtensionSecretVaultReadStatus::InvalidArgument, ERROR_DIRECTORY);
	}

	std::error_code pathError;
	const auto canonicalRoot = std::filesystem::weakly_canonical(m_legacyBasePath, pathError);
	if (pathError) return Result(ELegacyExtensionSecretVaultReadStatus::IoError, static_cast<DWORD>(pathError.value()));
	std::wstring extensionHash;
	if (!Sha256Hex(canonicalExtensionId, extensionHash)) {
		return Result(ELegacyExtensionSecretVaultReadStatus::CryptoError, ERROR_GEN_FAILURE);
	}
	const auto extensionDirectory = canonicalRoot / extensionHash;
	const DWORD extensionAttributes = ::GetFileAttributesW(extensionDirectory.c_str());
	if (extensionAttributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD error = ::GetLastError();
		return IsNotFound(error) ? Result(ELegacyExtensionSecretVaultReadStatus::Absent)
			: Result(ELegacyExtensionSecretVaultReadStatus::IoError, error);
	}
	if (!(extensionAttributes & FILE_ATTRIBUTE_DIRECTORY) || (extensionAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		|| !CanonicalPathWithin(extensionDirectory, canonicalRoot)) {
		return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
	}

	std::vector<std::filesystem::path> secretFiles;
	std::uintmax_t totalStorageBytes = 0;
	for (std::filesystem::directory_iterator iterator(extensionDirectory, pathError), end;
		!pathError && iterator != end; iterator.increment(pathError)) {
		std::error_code typeError;
		if (!iterator->is_regular_file(typeError)) {
			if (typeError) return Result(ELegacyExtensionSecretVaultReadStatus::IoError, static_cast<DWORD>(typeError.value()));
			continue;
		}
		if (iterator->path().extension() != L".bin") continue;
		const DWORD attributes = ::GetFileAttributesW(iterator->path().c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return Result(ELegacyExtensionSecretVaultReadStatus::IoError, ::GetLastError());
		if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !CanonicalPathWithin(iterator->path(), extensionDirectory)) {
			return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA);
		}
		std::error_code sizeError;
		const auto storageBytes = iterator->file_size(sizeError);
		if (sizeError) {
			return Result(ELegacyExtensionSecretVaultReadStatus::IoError, static_cast<DWORD>(sizeError.value()));
		}
		if (storageBytes > kMaximumLegacyStorageBytes
			|| storageBytes > kMaximumLegacyNamespaceStorageBytes - totalStorageBytes) {
			return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_FILE_TOO_LARGE);
		}
		totalStorageBytes += storageBytes;
		secretFiles.emplace_back(iterator->path());
		if (secretFiles.size() > kMaximumLegacyExtensionSecretEntries) {
			return Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_FILE_TOO_LARGE);
		}
	}
	if (pathError) return Result(ELegacyExtensionSecretVaultReadStatus::IoError, static_cast<DWORD>(pathError.value()));
	if (secretFiles.empty()) return Result(ELegacyExtensionSecretVaultReadStatus::Empty);
	std::sort(secretFiles.begin(), secretFiles.end());

	LegacyExtensionSecretVaultReadResult result = Result(ELegacyExtensionSecretVaultReadStatus::Success);
	std::set<std::string> keys;
	std::size_t totalSecretBytes = 0;
	for (const auto& path : secretFiles) {
		std::vector<std::uint8_t> storage;
		DWORD error = ERROR_SUCCESS;
		if (!ReadFileBytes(path, storage, error)) {
			SecureClear(result.entries);
			return Result(error == ERROR_FILE_TOO_LARGE
				? ELegacyExtensionSecretVaultReadStatus::CorruptData
				: ELegacyExtensionSecretVaultReadStatus::IoError, error);
		}
		LegacyExtensionSecretVaultEntry entry;
		auto decoded = DecodeStorage(storage, canonicalExtensionId, entry);
		SecureClear(storage);
		if (decoded.status != ELegacyExtensionSecretVaultReadStatus::Success
			|| !keys.emplace(entry.key).second
			|| entry.key.size() > kMaximumLegacyExtensionSecretBytes - totalSecretBytes
			|| entry.value.size() > kMaximumLegacyExtensionSecretBytes - totalSecretBytes - entry.key.size()) {
			SecureClear(entry.key);
			SecureClear(entry.value);
			SecureClear(result.entries);
			return decoded.status == ELegacyExtensionSecretVaultReadStatus::Success
				? Result(ELegacyExtensionSecretVaultReadStatus::CorruptData, ERROR_INVALID_DATA) : decoded;
		}
		totalSecretBytes += entry.key.size() + entry.value.size();
		result.entries.emplace_back(std::move(entry));
	}
	std::sort(result.entries.begin(), result.entries.end(), [](const auto& left, const auto& right) {
		return left.key < right.key;
	});
	return result;
}

} // namespace platform::secrets
