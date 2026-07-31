/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/profiles/ProfileAuthorityStore.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#if defined(_WIN32)
#include "platform/security/CurrentUserSecurityAttributes.h"

#include <Aclapi.h>
#include <bcrypt.h>
#endif

#include <array>
#include <charconv>
#include <limits>
#include <utility>

namespace platform::profiles {
namespace {

constexpr std::string_view kRecordHeader = "SakuraProfileAuthority/v1";
constexpr std::string_view kRecordIdPrefix = "profileId=";
constexpr std::string_view kRecordGenerationPrefix = "generation=";
constexpr std::string_view kRecordChecksumPrefix = "checksum=";
constexpr std::size_t kMaximumRecordBytes = 1024;
constexpr std::size_t kChecksumCharacters = 16;
constexpr wchar_t kRecordFileName[] = L"profile-authority.v1";
constexpr wchar_t kLockFileName[] = L"profile-authority.lock";

[[nodiscard]] bool IsValidUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const wchar_t codeUnit = value[index];
		if (codeUnit == L'\0') return false;
		if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
			if (++index == value.size() || value[index] < 0xdc00 || value[index] > 0xdfff) return false;
		}
		else if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsValidUtf8(std::string_view value) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const unsigned char first = static_cast<unsigned char>(value[index++]);
		if (first <= 0x7f) continue;
		unsigned int continuationCount = 0;
		std::uint32_t codePoint = 0;
		std::uint32_t minimumCodePoint = 0;
		if ((first & 0xe0) == 0xc0) {
			continuationCount = 1; codePoint = first & 0x1f; minimumCodePoint = 0x80;
		}
		else if ((first & 0xf0) == 0xe0) {
			continuationCount = 2; codePoint = first & 0x0f; minimumCodePoint = 0x800;
		}
		else if ((first & 0xf8) == 0xf0) {
			continuationCount = 3; codePoint = first & 0x07; minimumCodePoint = 0x10000;
		}
		else return false;
		if (value.size() - index < continuationCount) return false;
		for (unsigned int continuation = 0; continuation < continuationCount; ++continuation) {
			const unsigned char next = static_cast<unsigned char>(value[index++]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		if (codePoint < minimumCodePoint || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
	}
	return true;
}

[[nodiscard]] std::string CanonicalPayload(const ProfileAuthorityProfileId& profileId, std::uint64_t generation)
{
	return std::string(kRecordHeader) + '\n' + std::string(kRecordIdPrefix) + profileId + '\n'
		+ std::string(kRecordGenerationPrefix) + std::to_string(generation) + '\n';
}

//! FNV-1a is used as an integrity checksum, not as a secret or identity. The
//! fixed-width canonical encoding detects accidental/torn or bit-flipped records
//! before the authority generation is trusted.
[[nodiscard]] std::string PayloadChecksum(std::string_view payload)
{
	std::uint64_t hash = 14695981039346656037ULL;
	for (const unsigned char byte : payload) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	static constexpr char kHex[] = "0123456789abcdef";
	std::string checksum(kChecksumCharacters, '0');
	for (std::size_t index = 0; index < checksum.size(); ++index) {
		const std::size_t shift = (checksum.size() - 1 - index) * 4;
		checksum[index] = kHex[(hash >> shift) & 0x0f];
	}
	return checksum;
}

[[nodiscard]] std::string SerializeRecord(const ProfileAuthorityProfileId& profileId, std::uint64_t generation)
{
	const auto payload = CanonicalPayload(profileId, generation);
	return payload + std::string(kRecordChecksumPrefix) + PayloadChecksum(payload) + '\n';
}

struct ParsedRecord {
	ProfileAuthorityProfileId profileId;
	std::uint64_t generation = 0;
};

[[nodiscard]] ProfileAuthorityStoreStatus ParseRecord(std::string_view bytes, ParsedRecord& parsed)
{
	if (bytes.size() > kMaximumRecordBytes) return ProfileAuthorityStoreStatus::RecordTooLarge;
	if (!IsValidUtf8(bytes)) return ProfileAuthorityStoreStatus::InvalidUtf8;
	if (!bytes.starts_with(kRecordHeader)) return ProfileAuthorityStoreStatus::UnsupportedSchema;
	if (bytes.size() <= kRecordHeader.size() || bytes[kRecordHeader.size()] != '\n') return ProfileAuthorityStoreStatus::UnsupportedSchema;

	std::array<std::string_view, 4> lines{};
	std::size_t lineCount = 0;
	std::size_t begin = 0;
	while (begin < bytes.size()) {
		const auto end = bytes.find('\n', begin);
		if (end == std::string_view::npos) return ProfileAuthorityStoreStatus::CorruptRecord;
		if (lineCount == lines.size()) return ProfileAuthorityStoreStatus::CorruptRecord;
		lines[lineCount++] = bytes.substr(begin, end - begin);
		begin = end + 1;
	}
	if (lineCount != lines.size() || lines[0] != kRecordHeader || !bytes.ends_with('\n')) return ProfileAuthorityStoreStatus::CorruptRecord;
	if (!lines[1].starts_with(kRecordIdPrefix) || !lines[2].starts_with(kRecordGenerationPrefix)
		|| !lines[3].starts_with(kRecordChecksumPrefix)) return ProfileAuthorityStoreStatus::CorruptRecord;

	const auto profileId = std::string(lines[1].substr(kRecordIdPrefix.size()));
	const auto generationText = lines[2].substr(kRecordGenerationPrefix.size());
	if (!IsCanonicalProfileAuthorityId(profileId) || generationText.empty() || generationText.front() == '0') return ProfileAuthorityStoreStatus::CorruptRecord;
	std::uint64_t generation = 0;
	const auto conversion = std::from_chars(generationText.data(), generationText.data() + generationText.size(), generation);
	if (conversion.ec != std::errc{} || conversion.ptr != generationText.data() + generationText.size() || generation == 0) {
		return ProfileAuthorityStoreStatus::CorruptRecord;
	}
	const auto checksum = lines[3].substr(kRecordChecksumPrefix.size());
	if (checksum.size() != kChecksumCharacters || !std::all_of(checksum.begin(), checksum.end(), [](char character) {
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	}) || checksum != PayloadChecksum(CanonicalPayload(profileId, generation))) {
		return ProfileAuthorityStoreStatus::CorruptRecord;
	}
	parsed = { std::move(profileId), generation };
	return ProfileAuthorityStoreStatus::Succeeded;
}

[[nodiscard]] std::wstring DiagnosticFor(ProfileAuthorityStoreStatus status)
{
	switch (status) {
	case ProfileAuthorityStoreStatus::InvalidArgument: return L"profile authority input is invalid";
	case ProfileAuthorityStoreStatus::MetadataDirectoryFailed: return L"profile authority metadata directory is unavailable";
	case ProfileAuthorityStoreStatus::SecurityFailed: return L"profile authority metadata security could not be established";
	case ProfileAuthorityStoreStatus::LockUnavailable: return L"another control owner holds the profile authority lock";
	case ProfileAuthorityStoreStatus::RecordReadFailed: return L"profile authority record could not be read";
	case ProfileAuthorityStoreStatus::RecordTooLarge: return L"profile authority record exceeds its size limit";
	case ProfileAuthorityStoreStatus::InvalidUtf8: return L"profile authority record is not valid UTF-8";
	case ProfileAuthorityStoreStatus::UnsupportedSchema: return L"profile authority record schema is unsupported";
	case ProfileAuthorityStoreStatus::CorruptRecord: return L"profile authority record is corrupt";
	case ProfileAuthorityStoreStatus::RandomFailed: return L"profile authority identifier could not be generated";
	case ProfileAuthorityStoreStatus::GenerationOverflow: return L"profile authority generation has reached its limit";
	case ProfileAuthorityStoreStatus::WriteFailed: return L"profile authority temporary write failed";
	case ProfileAuthorityStoreStatus::FlushFailed: return L"profile authority durable flush failed";
	case ProfileAuthorityStoreStatus::ReplaceFailed: return L"profile authority record replacement failed";
	case ProfileAuthorityStoreStatus::PlatformNotSupported: return L"profile authority persistence is unavailable on this platform";
	case ProfileAuthorityStoreStatus::UnexpectedFailure: return L"profile authority persistence failed unexpectedly";
	case ProfileAuthorityStoreStatus::Succeeded: break;
	}
	return {};
}

[[nodiscard]] ProfileAuthorityResult Failure(ProfileAuthorityStoreStatus status)
{
	return { status, {}, 0, DiagnosticFor(status) };
}

#if defined(_WIN32)

class Win32ProfileAuthorityStoreLock final : public IProfileAuthorityStoreLock {
public:
	explicit Win32ProfileAuthorityStoreLock(HANDLE handle) noexcept : m_handle(handle) {}
	~Win32ProfileAuthorityStoreLock() override
	{
		if (m_handle != INVALID_HANDLE_VALUE) ::CloseHandle(m_handle);
	}

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool WriteAll(HANDLE file, std::string_view bytes) noexcept
{
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const DWORD requested = static_cast<DWORD>((std::min)(bytes.size() - offset,
			static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD written = 0;
		if (!::WriteFile(file, bytes.data() + offset, requested, &written, nullptr) || written == 0) return false;
		offset += written;
	}
	return true;
}

[[nodiscard]] bool HexEncodeRandom(std::array<std::uint8_t, 16> random, std::wstring& output) noexcept
{
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
	static constexpr wchar_t kHex[] = L"0123456789abcdef";
	output.clear();
	output.reserve(random.size() * 2);
	for (const auto byte : random) {
		output.push_back(kHex[byte >> 4]);
		output.push_back(kHex[byte & 0x0f]);
	}
	return true;
}

[[nodiscard]] PACL CurrentUserDacl(platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	SECURITY_ATTRIBUTES* const attributes = security.Attributes();
	BOOL daclPresent = FALSE;
	BOOL daclDefaulted = FALSE;
	PACL dacl = nullptr;
	if (!attributes || !attributes->lpSecurityDescriptor
		|| !::GetSecurityDescriptorDacl(attributes->lpSecurityDescriptor, &daclPresent, &dacl, &daclDefaulted)
		|| !daclPresent || !dacl) {
		return nullptr;
	}
	return dacl;
}

[[nodiscard]] bool RestrictPathToCurrentUser(
	const std::filesystem::path& path,
	platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	PACL dacl = CurrentUserDacl(security);
	if (!dacl) return false;
	return ::SetNamedSecurityInfoW(
		const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
		nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
}

[[nodiscard]] bool RestrictHandleToCurrentUser(
	HANDLE handle,
	platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	PACL dacl = CurrentUserDacl(security);
	return dacl && ::SetSecurityInfo(handle, SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
		nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
}

class Win32ProfileAuthorityStoreBackend final : public IProfileAuthorityStoreBackend {
public:
	ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path& directory) override
	{
		if (directory.empty()) return ProfileAuthorityStoreStatus::InvalidArgument;
		platform::security::CurrentUserSecurityAttributes security;
		std::wstring ignoredDiagnostic;
		if (!security.Initialize(ignoredDiagnostic)) return ProfileAuthorityStoreStatus::SecurityFailed;
		if (::CreateDirectoryW(directory.c_str(), security.Attributes()) || ::GetLastError() == ERROR_ALREADY_EXISTS) {
			const DWORD attributes = ::GetFileAttributesW(directory.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				return ProfileAuthorityStoreStatus::MetadataDirectoryFailed;
			}
			return RestrictPathToCurrentUser(directory, security)
				? ProfileAuthorityStoreStatus::Succeeded : ProfileAuthorityStoreStatus::SecurityFailed;
		}
		return ProfileAuthorityStoreStatus::MetadataDirectoryFailed;
	}

	ProfileAuthorityStoreStatus AcquireExclusiveLock(
		const std::filesystem::path& lockPath,
		std::unique_ptr<IProfileAuthorityStoreLock>& lock) override
	{
		platform::security::CurrentUserSecurityAttributes security;
		std::wstring ignoredDiagnostic;
		if (!security.Initialize(ignoredDiagnostic)) return ProfileAuthorityStoreStatus::SecurityFailed;
		const HANDLE handle = ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | WRITE_DAC, 0,
			security.Attributes(), OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return ProfileAuthorityStoreStatus::LockUnavailable;
		if (!RestrictHandleToCurrentUser(handle, security)) {
			::CloseHandle(handle);
			return ProfileAuthorityStoreStatus::SecurityFailed;
		}
		lock = std::make_unique<Win32ProfileAuthorityStoreLock>(handle);
		return ProfileAuthorityStoreStatus::Succeeded;
	}

	ProfileAuthorityStoreStatus ReadRecord(
		const std::filesystem::path& recordPath,
		std::string& bytes,
		bool& exists) override
	{
		bytes.clear();
		exists = false;
		const HANDLE file = ::CreateFileW(recordPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return ::GetLastError() == ERROR_FILE_NOT_FOUND ? ProfileAuthorityStoreStatus::Succeeded : ProfileAuthorityStoreStatus::RecordReadFailed;
		}
		struct HandleCloser final { HANDLE handle; ~HandleCloser() { ::CloseHandle(handle); } } closer{ file };
		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) return ProfileAuthorityStoreStatus::RecordReadFailed;
		if (static_cast<unsigned long long>(size.QuadPart) > kMaximumRecordBytes) return ProfileAuthorityStoreStatus::RecordTooLarge;
		bytes.resize(static_cast<std::size_t>(size.QuadPart));
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			DWORD read = 0;
			if (!::ReadFile(file, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &read, nullptr)) return ProfileAuthorityStoreStatus::RecordReadFailed;
			if (read == 0) return ProfileAuthorityStoreStatus::CorruptRecord;
			offset += read;
		}
		exists = true;
		return ProfileAuthorityStoreStatus::Succeeded;
	}

	ProfileAuthorityStoreStatus GenerateOpaqueProfileId(ProfileAuthorityProfileId& profileId) override
	{
		std::array<std::uint8_t, 16> random{};
		if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return ProfileAuthorityStoreStatus::RandomFailed;
		static constexpr char kHex[] = "0123456789abcdef";
		profileId.clear();
		profileId.reserve(random.size() * 2);
		for (const auto byte : random) {
			profileId.push_back(kHex[byte >> 4]);
			profileId.push_back(kHex[byte & 0x0f]);
		}
		return ProfileAuthorityStoreStatus::Succeeded;
	}

	ProfileAuthorityStoreStatus WriteRecordAtomically(
		const std::filesystem::path& recordPath,
		std::string_view bytes) override
	{
		const auto parent = recordPath.parent_path();
		platform::security::CurrentUserSecurityAttributes security;
		std::wstring ignoredDiagnostic;
		if (!security.Initialize(ignoredDiagnostic)) return ProfileAuthorityStoreStatus::SecurityFailed;
		const DWORD recordAttributes = ::GetFileAttributesW(recordPath.c_str());
		if (recordAttributes != INVALID_FILE_ATTRIBUTES && !RestrictPathToCurrentUser(recordPath, security)) {
			return ProfileAuthorityStoreStatus::SecurityFailed;
		}
		if (recordAttributes == INVALID_FILE_ATTRIBUTES && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
			return ProfileAuthorityStoreStatus::SecurityFailed;
		}
		for (unsigned int attempt = 0; attempt != 8; ++attempt) {
			std::wstring randomSuffix;
			if (!HexEncodeRandom({}, randomSuffix)) return ProfileAuthorityStoreStatus::RandomFailed;
			const std::filesystem::path temporaryPath = parent / (std::wstring(kRecordFileName) + L".tmp." + randomSuffix);
			const HANDLE temporary = ::CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, security.Attributes(), CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
			if (temporary == INVALID_HANDLE_VALUE) continue;
			const bool wrote = WriteAll(temporary, bytes);
			const bool flushed = wrote && ::FlushFileBuffers(temporary) != FALSE;
			::CloseHandle(temporary);
			if (!wrote) {
				(void)::DeleteFileW(temporaryPath.c_str());
				return ProfileAuthorityStoreStatus::WriteFailed;
			}
			if (!flushed) {
				(void)::DeleteFileW(temporaryPath.c_str());
				return ProfileAuthorityStoreStatus::FlushFailed;
			}
			if (::ReplaceFileW(recordPath.c_str(), temporaryPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
				|| ::MoveFileExW(temporaryPath.c_str(), recordPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				return ProfileAuthorityStoreStatus::Succeeded;
			}
			(void)::DeleteFileW(temporaryPath.c_str());
			return ProfileAuthorityStoreStatus::ReplaceFailed;
		}
		return ProfileAuthorityStoreStatus::WriteFailed;
	}
};

#else

class UnsupportedProfileAuthorityStoreBackend final : public IProfileAuthorityStoreBackend {
public:
	ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path&) override { return ProfileAuthorityStoreStatus::PlatformNotSupported; }
	ProfileAuthorityStoreStatus AcquireExclusiveLock(const std::filesystem::path&, std::unique_ptr<IProfileAuthorityStoreLock>&) override { return ProfileAuthorityStoreStatus::PlatformNotSupported; }
	ProfileAuthorityStoreStatus ReadRecord(const std::filesystem::path&, std::string&, bool&) override { return ProfileAuthorityStoreStatus::PlatformNotSupported; }
	ProfileAuthorityStoreStatus GenerateOpaqueProfileId(ProfileAuthorityProfileId&) override { return ProfileAuthorityStoreStatus::PlatformNotSupported; }
	ProfileAuthorityStoreStatus WriteRecordAtomically(const std::filesystem::path&, std::string_view) override { return ProfileAuthorityStoreStatus::PlatformNotSupported; }
};

#endif

} // namespace

std::shared_ptr<IProfileAuthorityStoreBackend> CreateWin32ProfileAuthorityStoreBackend()
{
#if defined(_WIN32)
	return std::make_shared<Win32ProfileAuthorityStoreBackend>();
#else
	return std::make_shared<UnsupportedProfileAuthorityStoreBackend>();
#endif
}

ProfileAuthorityStore::ProfileAuthorityStore(
	std::filesystem::path profileDirectory,
	std::shared_ptr<IProfileAuthorityStoreBackend> backend)
	: m_profileDirectory(std::move(profileDirectory))
	, m_backend(backend ? std::move(backend) : CreateWin32ProfileAuthorityStoreBackend())
{
}

ProfileAuthorityResult ProfileAuthorityStore::Acquire(std::wstring_view legacyProfileAlias)
{
	try {
		if (m_profileDirectory.empty() || !m_backend || !IsValidUtf16(legacyProfileAlias)) {
			return Failure(ProfileAuthorityStoreStatus::InvalidArgument);
		}
		const auto metadataDirectory = BuildProfilePlatformMetadataDirectory(m_profileDirectory);
		const auto metadataStatus = m_backend->EnsureMetadataDirectory(metadataDirectory);
		if (metadataStatus != ProfileAuthorityStoreStatus::Succeeded) return Failure(metadataStatus);

		std::unique_ptr<IProfileAuthorityStoreLock> lock;
		const auto lockStatus = m_backend->AcquireExclusiveLock(metadataDirectory / kLockFileName, lock);
		if (lockStatus != ProfileAuthorityStoreStatus::Succeeded || !lock) {
			return Failure(lockStatus == ProfileAuthorityStoreStatus::Succeeded ? ProfileAuthorityStoreStatus::LockUnavailable : lockStatus);
		}

		std::string bytes;
		bool exists = false;
		const auto readStatus = m_backend->ReadRecord(metadataDirectory / kRecordFileName, bytes, exists);
		if (readStatus != ProfileAuthorityStoreStatus::Succeeded) return Failure(readStatus);

		ParsedRecord record;
		if (exists) {
			const auto parseStatus = ParseRecord(bytes, record);
			if (parseStatus != ProfileAuthorityStoreStatus::Succeeded) return Failure(parseStatus);
			if (record.generation == (std::numeric_limits<std::uint64_t>::max)()) return Failure(ProfileAuthorityStoreStatus::GenerationOverflow);
			++record.generation;
		}
		else {
			const auto randomStatus = m_backend->GenerateOpaqueProfileId(record.profileId);
			if (randomStatus != ProfileAuthorityStoreStatus::Succeeded || !IsCanonicalProfileAuthorityId(record.profileId)) {
				return Failure(randomStatus == ProfileAuthorityStoreStatus::Succeeded ? ProfileAuthorityStoreStatus::RandomFailed : randomStatus);
			}
			record.generation = 1;
		}

		const auto writeStatus = m_backend->WriteRecordAtomically(
			metadataDirectory / kRecordFileName, SerializeRecord(record.profileId, record.generation));
		if (writeStatus != ProfileAuthorityStoreStatus::Succeeded) return Failure(writeStatus);
		return { ProfileAuthorityStoreStatus::Succeeded, std::move(record.profileId), record.generation, {} };
	}
	catch (...) {
		return Failure(ProfileAuthorityStoreStatus::UnexpectedFailure);
	}
}

} // namespace platform::profiles
