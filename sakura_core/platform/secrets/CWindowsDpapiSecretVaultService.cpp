/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/CWindowsDpapiSecretVaultService.h"

#include "platform/profiles/ProfileAuthorityIdentity.h"
#include "platform/security/CurrentUserSecurityAttributes.h"

#include <Aclapi.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <type_traits>
#include <utility>

namespace platform::secrets {
namespace {

constexpr std::array<std::uint8_t, 8> kEnvelopeMagic = { 'S', 'A', 'K', 'V', 'L', 'T', '0', '1' };
constexpr std::array<std::uint8_t, 8> kPlaintextMagic = { 'S', 'A', 'K', 'V', 'P', 'L', '0', '1' };
constexpr std::uint32_t kEnvelopeVersion = 1;
constexpr std::uint32_t kPlaintextVersion = 2;
constexpr std::size_t kEnvelopeHeaderBytes = kEnvelopeMagic.size() + sizeof(std::uint32_t) + sizeof(std::uint32_t);
constexpr std::size_t kMaximumPlaintextBytes = CWindowsDpapiSecretVaultService::kMaximumPersistedBytes - 512;

constexpr std::string_view kDiagnosticNotOpen = "secret vault is not open";
constexpr std::string_view kDiagnosticInvalidRequest = "secret vault request is invalid";
constexpr std::string_view kDiagnosticOperationCollision = "secret vault operationId payload collision";
constexpr std::string_view kDiagnosticPersistenceFailed = "secret vault persistence failed";
constexpr std::string_view kDiagnosticRevisionExhausted = "secret vault revision is exhausted";
constexpr std::string_view kDiagnosticLegacyImportInvalid = "secret vault legacy import is invalid";
constexpr std::string_view kDiagnosticLegacyImportCollision = "secret vault operationId payload collision";

void SecureErase(std::string& value) noexcept
{
	if (!value.empty()) {
		::SecureZeroMemory(value.data(), value.size());
	}
	value.clear();
}

void SecureErase(std::vector<std::uint8_t>& value) noexcept
{
	if (!value.empty()) {
		::SecureZeroMemory(value.data(), value.size());
	}
	value.clear();
}

class SensitiveBytes final {
public:
	std::vector<std::uint8_t> bytes;
	~SensitiveBytes() { SecureErase(bytes); }
	SensitiveBytes(const SensitiveBytes&) = delete;
	SensitiveBytes& operator=(const SensitiveBytes&) = delete;
	SensitiveBytes() = default;
};

template<typename T>
void AppendUnsigned(std::vector<std::uint8_t>& output, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		output.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

template<typename T>
bool ReadUnsigned(std::span<const std::uint8_t> input, std::size_t& offset, T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > input.size() || input.size() - offset < sizeof(T)) {
		return false;
	}
	value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		value |= static_cast<T>(input[offset++]) << (index * 8);
	}
	return true;
}

bool AppendString(std::vector<std::uint8_t>& output, std::string_view value, std::size_t maximumBytes,
	bool allowEmpty = true)
{
	if (value.size() > maximumBytes || value.size() > (std::numeric_limits<std::uint32_t>::max)()
		|| !IsValidSecretVaultUtf8(value, allowEmpty)) {
		return false;
	}
	AppendUnsigned<std::uint32_t>(output, static_cast<std::uint32_t>(value.size()));
	output.insert(output.end(), value.begin(), value.end());
	return output.size() <= kMaximumPlaintextBytes;
}

bool ReadString(std::span<const std::uint8_t> input, std::size_t& offset, std::string& value,
	std::size_t maximumBytes, bool allowEmpty = true)
{
	std::uint32_t byteCount = 0;
	if (!ReadUnsigned(input, offset, byteCount) || byteCount > maximumBytes || offset > input.size()
		|| byteCount > input.size() - offset) {
		return false;
	}
	value.assign(reinterpret_cast<const char*>(input.data() + offset), byteCount);
	offset += byteCount;
	if (!IsValidSecretVaultUtf8(value, allowEmpty)) {
		SecureErase(value);
		return false;
	}
	return true;
}

bool IsPersistedStatus(ESecretMutationStatus status) noexcept
{
	return status == ESecretMutationStatus::Succeeded || status == ESecretMutationStatus::Conflict
		|| status == ESecretMutationStatus::NotApplicable;
}

bool IsPersistedLegacyImportStatus(ELegacySecretVaultImportStatus status) noexcept
{
	return status == ELegacySecretVaultImportStatus::Succeeded
		|| status == ELegacySecretVaultImportStatus::AlreadyImported
		|| status == ELegacySecretVaultImportStatus::Conflict;
}

bool ProtectForProfile(std::span<const std::uint8_t> plaintext, std::string_view profileId,
	std::vector<std::uint8_t>& encrypted) noexcept
{
	if (plaintext.empty() || plaintext.size() > (std::numeric_limits<DWORD>::max)()
		|| profileId.size() > (std::numeric_limits<DWORD>::max)() - 40) {
		return false;
	}
	std::vector<std::uint8_t> entropy;
	static constexpr std::string_view kPrefix = "Sakura Editor NEXT/SecretVault/DPAPI/v1\0";
	entropy.reserve(kPrefix.size() + profileId.size());
	entropy.insert(entropy.end(), kPrefix.begin(), kPrefix.end());
	entropy.insert(entropy.end(), profileId.begin(), profileId.end());
	DATA_BLOB input{ static_cast<DWORD>(plaintext.size()), const_cast<BYTE*>(plaintext.data()) };
	DATA_BLOB optionalEntropy{ static_cast<DWORD>(entropy.size()), entropy.data() };
	DATA_BLOB output{};
	const BOOL protectedData = ::CryptProtectData(&input, L"Sakura Editor NEXT Secret Vault",
		&optionalEntropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
	SecureErase(entropy);
	if (!protectedData || !output.pbData || output.cbData == 0) {
		if (output.pbData) {
			::SecureZeroMemory(output.pbData, output.cbData);
			::LocalFree(output.pbData);
		}
		return false;
	}
	encrypted.assign(output.pbData, output.pbData + output.cbData);
	::SecureZeroMemory(output.pbData, output.cbData);
	::LocalFree(output.pbData);
	return encrypted.size() <= CWindowsDpapiSecretVaultService::kMaximumPersistedBytes - kEnvelopeHeaderBytes;
}

bool UnprotectForProfile(std::span<const std::uint8_t> encrypted, std::string_view profileId,
	std::vector<std::uint8_t>& plaintext) noexcept
{
	if (encrypted.empty() || encrypted.size() > (std::numeric_limits<DWORD>::max)()
		|| profileId.size() > (std::numeric_limits<DWORD>::max)() - 40) {
		return false;
	}
	std::vector<std::uint8_t> entropy;
	static constexpr std::string_view kPrefix = "Sakura Editor NEXT/SecretVault/DPAPI/v1\0";
	entropy.reserve(kPrefix.size() + profileId.size());
	entropy.insert(entropy.end(), kPrefix.begin(), kPrefix.end());
	entropy.insert(entropy.end(), profileId.begin(), profileId.end());
	DATA_BLOB input{ static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data()) };
	DATA_BLOB optionalEntropy{ static_cast<DWORD>(entropy.size()), entropy.data() };
	DATA_BLOB output{};
	const BOOL unprotectedData = ::CryptUnprotectData(&input, nullptr, &optionalEntropy,
		nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
	SecureErase(entropy);
	if (!unprotectedData || !output.pbData || output.cbData == 0
		|| output.cbData > kMaximumPlaintextBytes) {
		if (output.pbData) {
			::SecureZeroMemory(output.pbData, output.cbData);
			::LocalFree(output.pbData);
		}
		return false;
	}
	plaintext.assign(output.pbData, output.pbData + output.cbData);
	::SecureZeroMemory(output.pbData, output.cbData);
	::LocalFree(output.pbData);
	return true;
}

bool ApplyCurrentUserDacl(const std::filesystem::path& path,
	platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	SECURITY_ATTRIBUTES* const attributes = security.Attributes();
	BOOL present = FALSE;
	BOOL defaulted = FALSE;
	PACL dacl = nullptr;
	if (!attributes || !attributes->lpSecurityDescriptor
		|| !::GetSecurityDescriptorDacl(attributes->lpSecurityDescriptor, &present, &dacl, &defaulted)
		|| !present || !dacl) {
		return false;
	}
	return ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
		nullptr) == ERROR_SUCCESS;
}

bool ApplyCurrentUserDacl(HANDLE handle, platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	SECURITY_ATTRIBUTES* const attributes = security.Attributes();
	BOOL present = FALSE;
	BOOL defaulted = FALSE;
	PACL dacl = nullptr;
	if (!attributes || !attributes->lpSecurityDescriptor
		|| !::GetSecurityDescriptorDacl(attributes->lpSecurityDescriptor, &present, &dacl, &defaulted)
		|| !present || !dacl) {
		return false;
	}
	return ::SetSecurityInfo(handle, SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
		nullptr) == ERROR_SUCCESS;
}

class Win32WriterLock final : public IWindowsDpapiSecretVaultWriterLock {
public:
	explicit Win32WriterLock(HANDLE handle) noexcept : m_handle(handle) {}
	~Win32WriterLock() override
	{
		if (m_handle != INVALID_HANDLE_VALUE) {
			OVERLAPPED overlapped{};
			(void)::UnlockFileEx(m_handle, 0, MAXDWORD, MAXDWORD, &overlapped);
			::CloseHandle(m_handle);
		}
	}

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class Win32FileOperations final : public IWindowsDpapiSecretVaultFileOperations {
public:
	bool PrepareDirectory(const std::filesystem::path& directory) override
	{
		std::wstring unused;
		platform::security::CurrentUserSecurityAttributes security;
		if (!security.Initialize(unused)) {
			return false;
		}
		if (!::CreateDirectoryW(directory.c_str(), security.Attributes())) {
			const DWORD error = ::GetLastError();
			if (error != ERROR_ALREADY_EXISTS) {
				return false;
			}
			const DWORD attributes = ::GetFileAttributesW(directory.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				return false;
			}
		}
		return ApplyCurrentUserDacl(directory, security);
	}

	[[nodiscard]] WindowsDpapiSecretVaultWriterLockResult AcquireWriterLock(
		const std::filesystem::path& lockPath) override
	{
		std::wstring unused;
		platform::security::CurrentUserSecurityAttributes security;
		if (!security.Initialize(unused)) {
			return { EWindowsDpapiSecretVaultWriterLockStatus::IoError, nullptr };
		}
		HANDLE handle = ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | WRITE_DAC,
			FILE_SHARE_READ, security.Attributes(), OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			return { error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION
				? EWindowsDpapiSecretVaultWriterLockStatus::Busy
				: EWindowsDpapiSecretVaultWriterLockStatus::IoError, nullptr };
		}
		if (!ApplyCurrentUserDacl(handle, security)) {
			::CloseHandle(handle);
			return { EWindowsDpapiSecretVaultWriterLockStatus::IoError, nullptr };
		}
		OVERLAPPED overlapped{};
		if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
			MAXDWORD, MAXDWORD, &overlapped)) {
			const DWORD error = ::GetLastError();
			::CloseHandle(handle);
			return { error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION
				? EWindowsDpapiSecretVaultWriterLockStatus::Busy
				: EWindowsDpapiSecretVaultWriterLockStatus::IoError, nullptr };
		}
		return { EWindowsDpapiSecretVaultWriterLockStatus::Acquired,
			std::make_unique<Win32WriterLock>(handle) };
	}

	bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
		bool& found) override
	{
		found = false;
		HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}
		struct CloseFile final { HANDLE value; ~CloseFile() { ::CloseHandle(value); } } file{ handle };
		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(handle, &size) || size.QuadPart < 0
			|| size.QuadPart > static_cast<LONGLONG>(CWindowsDpapiSecretVaultService::kMaximumPersistedBytes)) {
			return false;
		}
		bytes.resize(static_cast<std::size_t>(size.QuadPart));
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - offset,
				static_cast<std::size_t>(1024 * 1024)));
			DWORD received = 0;
			if (!::ReadFile(handle, bytes.data() + offset, wanted, &received, nullptr) || received == 0) {
				SecureErase(bytes);
				return false;
			}
			offset += received;
		}
		found = true;
		return true;
	}

	bool WriteFileAtomically(const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes) override
	{
		if (bytes.empty() || bytes.size() > CWindowsDpapiSecretVaultService::kMaximumPersistedBytes) {
			return false;
		}
		std::wstring unused;
		platform::security::CurrentUserSecurityAttributes security;
		if (!security.Initialize(unused)) {
			return false;
		}
		std::array<std::uint8_t, 12> random{};
		if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
			return false;
		}
		static constexpr wchar_t kHex[] = L"0123456789abcdef";
		std::wstring suffix;
		suffix.reserve(random.size() * 2);
		for (const auto value : random) {
			suffix.push_back(kHex[value >> 4]);
			suffix.push_back(kHex[value & 0x0f]);
		}
		const auto temporary = path.parent_path() / (path.filename().wstring() + L"." + suffix + L".tmp");
		HANDLE handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security.Attributes(),
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return false;
		}
		bool written = true;
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - offset,
				static_cast<std::size_t>(1024 * 1024)));
			DWORD count = 0;
			if (!::WriteFile(handle, bytes.data() + offset, wanted, &count, nullptr) || count == 0) {
				written = false;
				break;
			}
			offset += count;
		}
		if (written && !::FlushFileBuffers(handle)) {
			written = false;
		}
		::CloseHandle(handle);
		if (!written) {
			(void)::DeleteFileW(temporary.c_str());
			return false;
		}
		if (!::MoveFileExW(temporary.c_str(), path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			(void)::DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}
};

struct SubscriptionSlot final {
	explicit SubscriptionSlot(SecretChangeCallback listener) : callback(std::move(listener)) {}
	std::atomic_bool active = true;
	SecretChangeCallback callback;
};

class SecretVaultSubscription final : public ISecretVaultChangeSubscription {
public:
	SecretVaultSubscription(std::weak_ptr<CWindowsDpapiSecretVaultService::SubscriptionState> state,
		std::uint64_t subscriptionId) noexcept
		: m_state(std::move(state))
		, m_subscriptionId(subscriptionId)
	{
	}
	~SecretVaultSubscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;

private:
	std::weak_ptr<CWindowsDpapiSecretVaultService::SubscriptionState> m_state;
	std::uint64_t m_subscriptionId = 0;
};

void DeliverNotification(const std::shared_ptr<CWindowsDpapiSecretVaultService::SubscriptionState>& state,
	const SecretChange& change) noexcept;

} // namespace

struct CWindowsDpapiSecretVaultService::CompletedOperation {
	SecretMutationRequest request;
	SecretMutationResult result;
};

struct CWindowsDpapiSecretVaultService::CompletedLegacyImport {
	LegacySecretVaultImportRequest request;
	LegacySecretVaultImportResult result;
};

struct CWindowsDpapiSecretVaultService::SubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::size_t maximumSubscriptions = kMaximumSecretVaultSubscriptions;
	std::uint64_t nextSubscriptionId = 1;
	std::map<std::uint64_t, std::shared_ptr<SubscriptionSlot>> slots;
};

namespace {

void SecretVaultSubscription::Unsubscribe() noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			m_subscriptionId = 0;
			return;
		}
		std::scoped_lock lock(state->mutex);
		if (const auto found = state->slots.find(m_subscriptionId); found != state->slots.end()) {
			found->second->active.store(false, std::memory_order_release);
			state->slots.erase(found);
		}
		m_subscriptionId = 0;
	} catch (...) {
		m_subscriptionId = 0;
	}
}

bool SecretVaultSubscription::IsSubscribed() const noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			return false;
		}
		std::scoped_lock lock(state->mutex);
		const auto found = state->slots.find(m_subscriptionId);
		return !state->closed && found != state->slots.end()
			&& found->second->active.load(std::memory_order_acquire);
	} catch (...) {
		return false;
	}
}

void DeliverNotification(const std::shared_ptr<CWindowsDpapiSecretVaultService::SubscriptionState>& state,
	const SecretChange& change) noexcept
{
	std::vector<std::shared_ptr<SubscriptionSlot>> listeners;
	try {
		std::scoped_lock lock(state->mutex);
		if (state->closed) {
			return;
		}
		listeners.reserve(state->slots.size());
		for (const auto& [subscriptionId, slot] : state->slots) {
			(void)subscriptionId;
			listeners.emplace_back(slot);
		}
	} catch (...) {
		return;
	}
	for (const auto& listener : listeners) {
		if (!listener->active.load(std::memory_order_acquire)) {
			continue;
		}
		try {
			listener->callback(change);
		} catch (...) {
			// A listener cannot prevent another committed-event delivery.
		}
	}
}

} // namespace

std::shared_ptr<IWindowsDpapiSecretVaultFileOperations>
CreateWin32WindowsDpapiSecretVaultFileOperations()
{
	return std::make_shared<Win32FileOperations>();
}

CWindowsDpapiSecretVaultService::CWindowsDpapiSecretVaultService(std::filesystem::path metadataRoot,
	std::string canonicalProfileId, std::size_t maxCompletedOperations, std::size_t maxSubscriptions,
	std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> fileOperations)
	: m_metadataRoot(std::move(metadataRoot))
	, m_profileId(std::move(canonicalProfileId))
	, m_profileIdValid(profiles::IsCanonicalProfileAuthorityId(m_profileId))
	, m_maxCompletedOperations(std::clamp(maxCompletedOperations, std::size_t{ 1 },
		kMaximumCompletedOperations))
	, m_fileOperations(fileOperations ? std::move(fileOperations)
		: CreateWin32WindowsDpapiSecretVaultFileOperations())
	, m_subscriptionState(std::make_shared<SubscriptionState>())
{
	m_subscriptionState->maximumSubscriptions = std::clamp(maxSubscriptions, std::size_t{ 1 },
		kMaximumSecretVaultSubscriptions);
}

CWindowsDpapiSecretVaultService::~CWindowsDpapiSecretVaultService()
{
	(void)Stop();
}

WindowsDpapiSecretVaultCreateResult CWindowsDpapiSecretVaultService::Create(
	std::filesystem::path metadataRoot, std::string canonicalProfileId,
	std::size_t maxCompletedOperations, std::size_t maxSubscriptions,
	std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> fileOperations)
{
	auto service = std::make_unique<CWindowsDpapiSecretVaultService>(std::move(metadataRoot),
		std::move(canonicalProfileId), maxCompletedOperations, maxSubscriptions,
		std::move(fileOperations));
	auto open = service->Open();
	if (!open.Succeeded()) {
		return { std::move(open), nullptr };
	}
	return { std::move(open), std::move(service) };
}

std::filesystem::path CWindowsDpapiSecretVaultService::StatePath() const
{
	return m_metadataRoot / L"secret-vault-v1.bin";
}

WindowsDpapiSecretVaultOpenResult CWindowsDpapiSecretVaultService::Open()
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { EWindowsDpapiSecretVaultOpenStatus::Stopped, "secret vault is stopped" };
	}
	if (m_open) {
		return { EWindowsDpapiSecretVaultOpenStatus::AlreadyOpen, {} };
	}
	if (m_metadataRoot.empty() || !m_profileIdValid || !m_fileOperations) {
		return { EWindowsDpapiSecretVaultOpenStatus::InvalidArgument, "secret vault arguments are invalid" };
	}
	if (!m_fileOperations->PrepareDirectory(m_metadataRoot)) {
		return { EWindowsDpapiSecretVaultOpenStatus::IoError, "secret vault metadata root is unavailable" };
	}
	auto writerLock = m_fileOperations->AcquireWriterLock(m_metadataRoot / L"secret-vault-v1.lock");
	if (writerLock.status != EWindowsDpapiSecretVaultWriterLockStatus::Acquired || !writerLock.lock) {
		return { writerLock.status == EWindowsDpapiSecretVaultWriterLockStatus::Busy
			? EWindowsDpapiSecretVaultOpenStatus::WriterBusy : EWindowsDpapiSecretVaultOpenStatus::IoError,
			writerLock.status == EWindowsDpapiSecretVaultWriterLockStatus::Busy
				? "secret vault writer is busy" : "secret vault writer lock failed" };
	}
	m_writerLock = std::move(writerLock.lock);
	std::vector<std::uint8_t> bytes;
	bool found = false;
	if (!m_fileOperations->ReadFile(StatePath(), bytes, found)) {
		m_writerLock.reset();
		return { EWindowsDpapiSecretVaultOpenStatus::IoError, "secret vault state read failed" };
	}
	if (found) {
		auto decoded = Decode(bytes);
		if (!decoded.Succeeded()) {
			m_writerLock.reset();
			return decoded;
		}
	}
	m_open = true;
	return { EWindowsDpapiSecretVaultOpenStatus::Opened, {} };
}

bool CWindowsDpapiSecretVaultService::IsOpen() const noexcept
{
	try {
		std::scoped_lock lock(m_mutex);
		return m_open && !m_stopped;
	} catch (...) {
		return false;
	}
}

std::string_view CWindowsDpapiSecretVaultService::GetProfileId() const noexcept
{
	return m_profileId;
}

bool CWindowsDpapiSecretVaultService::IsLegacyMigrationComplete(std::string_view extensionId) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)) {
		return false;
	}
	std::scoped_lock lock(m_mutex);
	return m_open && !m_stopped && m_migrationCompleteExtensions.contains(canonicalExtensionId);
}

bool CWindowsDpapiSecretVaultService::IsAvailableLocked() const noexcept
{
	return m_open && !m_stopped && m_profileIdValid;
}

SecretGetResult CWindowsDpapiSecretVaultService::Get(std::string_view extensionId,
	std::string_view key) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
		|| !IsValidSecretVaultIdentifier(key, kMaximumSecretVaultKeyBytes)) {
		return { .status = ESecretGetStatus::Invalid };
	}
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretGetStatus::Stopped, .revision = m_revision };
	}
	if (!m_open || !m_profileIdValid) {
		return { .status = ESecretGetStatus::Invalid, .revision = m_revision };
	}
	const SecretAddress address{ .extensionId = std::move(canonicalExtensionId), .key = std::string(key) };
	const auto found = m_entries.find(address);
	if (found == m_entries.end()) {
		return { .status = ESecretGetStatus::NotFound, .revision = m_revision };
	}
	return { .status = ESecretGetStatus::Found, .revision = m_revision, .value = found->second.value };
}

std::optional<SecretMutationRequest> CWindowsDpapiSecretVaultService::CanonicalizeRequest(
	const SecretMutationRequest& request) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(request.extensionId, canonicalExtensionId)
		|| !IsValidSecretVaultIdentifier(request.key, kMaximumSecretVaultKeyBytes)
		|| !IsValidSecretVaultIdentifier(request.operationId, kMaximumSecretVaultOperationIdBytes)
		|| (request.kind != ESecretMutationKind::Set && request.kind != ESecretMutationKind::Delete)
		|| (request.kind == ESecretMutationKind::Set
			&& (!IsValidSecretVaultUtf8(request.value) || request.value.size() > kMaximumSecretVaultValueBytes))
		|| (request.kind == ESecretMutationKind::Delete && !request.value.empty())) {
		return std::nullopt;
	}
	SecretMutationRequest canonical = request;
	canonical.extensionId = std::move(canonicalExtensionId);
	return canonical;
}

std::optional<LegacySecretVaultImportRequest>
CWindowsDpapiSecretVaultService::CanonicalizeLegacyImportRequest(
	const LegacySecretVaultImportRequest& request) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(request.extensionId, canonicalExtensionId)
		|| !IsValidSecretVaultIdentifier(request.operationId,
			kMaximumSecretVaultOperationIdBytes)
		|| request.entries.size() > kMaximumEntries) {
		return std::nullopt;
	}
	LegacySecretVaultImportRequest canonical = request;
	canonical.extensionId = std::move(canonicalExtensionId);
	std::string previousKey;
	for (const auto& entry : canonical.entries) {
		if (!IsValidSecretVaultIdentifier(entry.key, kMaximumSecretVaultKeyBytes)
			|| !IsValidSecretVaultUtf8(entry.value)
			|| entry.value.size() > kMaximumSecretVaultValueBytes
			|| (!previousKey.empty() && entry.key <= previousKey)) {
			for (auto& scrub : canonical.entries) {
				SecureErase(scrub.value);
			}
			return std::nullopt;
		}
		previousKey = entry.key;
	}
	return canonical;
}

bool CWindowsDpapiSecretVaultService::RememberCompleted(
	std::map<std::string, CompletedOperation>& completed, std::deque<std::string>& order,
	const SecretMutationRequest& request, const SecretMutationResult& result) const
{
	if (completed.contains(request.operationId) || !IsPersistedStatus(result.status)) {
		return false;
	}
	completed.emplace(request.operationId, CompletedOperation{ request, result });
	order.emplace_back(request.operationId);
	while (order.size() > m_maxCompletedOperations) {
		const auto oldest = order.front();
		if (const auto found = completed.find(oldest); found != completed.end()) {
			SecureErase(found->second.request.value);
			completed.erase(found);
		}
		order.pop_front();
	}
	return completed.contains(request.operationId);
}

bool CWindowsDpapiSecretVaultService::RememberLegacyImport(
	std::map<std::string, CompletedLegacyImport>& completed, std::deque<std::string>& order,
	const LegacySecretVaultImportRequest& request, const LegacySecretVaultImportResult& result) const
{
	if (completed.contains(request.operationId) || !IsPersistedLegacyImportStatus(result.status)) {
		return false;
	}
	completed.emplace(request.operationId, CompletedLegacyImport{ request, result });
	order.emplace_back(request.operationId);
	while (order.size() > m_maxCompletedOperations) {
		const auto oldest = order.front();
		if (const auto found = completed.find(oldest); found != completed.end()) {
			for (auto& entry : found->second.request.entries) {
				SecureErase(entry.value);
			}
			completed.erase(found);
		}
		order.pop_front();
	}
	return completed.contains(request.operationId);
}

bool CWindowsDpapiSecretVaultService::PersistCandidate(
	const std::map<SecretAddress, SecretEntry>& entries,
	const std::map<std::string, CompletedOperation>& completed,
	const std::deque<std::string>& completedOrder,
	const std::map<std::string, std::uint64_t>& migrationCompleteExtensions,
	const std::map<std::string, CompletedLegacyImport>& completedLegacyImports,
	const std::deque<std::string>& completedLegacyImportOrder,
	std::uint64_t revision) const
{
	if (!m_fileOperations || entries.size() > kMaximumEntries
		|| completed.size() != completedOrder.size()
		|| completed.size() > m_maxCompletedOperations
		|| migrationCompleteExtensions.size() > kMaximumEntries
		|| completedLegacyImports.size() != completedLegacyImportOrder.size()
		|| completedLegacyImports.size() > m_maxCompletedOperations) {
		return false;
	}
	for (const auto& [operationId, operation] : completedLegacyImports) {
		(void)operation;
		if (completed.contains(operationId)) {
			return false;
		}
	}
	SensitiveBytes plaintext;
	plaintext.bytes.reserve(1024);
	plaintext.bytes.insert(plaintext.bytes.end(), kPlaintextMagic.begin(), kPlaintextMagic.end());
	AppendUnsigned<std::uint32_t>(plaintext.bytes, kPlaintextVersion);
	if (!AppendString(plaintext.bytes, m_profileId, kMaximumSecretVaultProfileIdBytes, false)) {
		return false;
	}
	AppendUnsigned<std::uint64_t>(plaintext.bytes, revision);
	AppendUnsigned<std::uint32_t>(plaintext.bytes, static_cast<std::uint32_t>(entries.size()));
	AppendUnsigned<std::uint32_t>(plaintext.bytes, static_cast<std::uint32_t>(completedOrder.size()));
	AppendUnsigned<std::uint32_t>(plaintext.bytes,
		static_cast<std::uint32_t>(migrationCompleteExtensions.size()));
	AppendUnsigned<std::uint32_t>(plaintext.bytes,
		static_cast<std::uint32_t>(completedLegacyImportOrder.size()));
	for (const auto& [address, entry] : entries) {
		(void)address;
		if (!entry.address.IsValid() || entry.revision == 0 || entry.revision > revision
			|| !AppendString(plaintext.bytes, entry.address.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| !AppendString(plaintext.bytes, entry.address.key, kMaximumSecretVaultKeyBytes, false)
			|| !AppendString(plaintext.bytes, entry.value, kMaximumSecretVaultValueBytes)
			|| plaintext.bytes.size() > kMaximumPlaintextBytes) {
			return false;
		}
		AppendUnsigned<std::uint64_t>(plaintext.bytes, entry.revision);
	}
	for (const auto& operationId : completedOrder) {
		const auto found = completed.find(operationId);
		if (found == completed.end()) {
			return false;
		}
		const auto& operation = found->second;
		const auto canonicalRequest = CanonicalizeRequest(operation.request);
		if (!canonicalRequest || !(*canonicalRequest == operation.request)
			|| !IsPersistedStatus(operation.result.status)
			|| operation.result.revision > revision
			|| !AppendString(plaintext.bytes, operation.request.operationId,
				kMaximumSecretVaultOperationIdBytes, false)) {
			return false;
		}
		AppendUnsigned<std::uint8_t>(plaintext.bytes, static_cast<std::uint8_t>(operation.request.kind));
		if (!AppendString(plaintext.bytes, operation.request.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| !AppendString(plaintext.bytes, operation.request.key, kMaximumSecretVaultKeyBytes, false)
			|| !AppendString(plaintext.bytes, operation.request.value, kMaximumSecretVaultValueBytes)
			|| (operation.request.kind == ESecretMutationKind::Delete && !operation.request.value.empty())) {
			return false;
		}
		AppendUnsigned<std::uint8_t>(plaintext.bytes, operation.request.expectedRevision ? 1 : 0);
		if (operation.request.expectedRevision) {
			AppendUnsigned<std::uint64_t>(plaintext.bytes, *operation.request.expectedRevision);
		}
		AppendUnsigned<std::uint8_t>(plaintext.bytes, static_cast<std::uint8_t>(operation.result.status));
		AppendUnsigned<std::uint64_t>(plaintext.bytes, operation.result.revision);
		if (plaintext.bytes.size() > kMaximumPlaintextBytes) {
			return false;
		}
	}
	for (const auto& [extensionId, completedRevision] : migrationCompleteExtensions) {
		std::string canonicalExtensionId;
		if (!CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
			|| canonicalExtensionId != extensionId || completedRevision == 0
			|| completedRevision > revision
			|| !AppendString(plaintext.bytes, extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)) {
			return false;
		}
		AppendUnsigned<std::uint64_t>(plaintext.bytes, completedRevision);
	}
	for (const auto& operationId : completedLegacyImportOrder) {
		const auto found = completedLegacyImports.find(operationId);
		if (found == completedLegacyImports.end()) {
			return false;
		}
		const auto& operation = found->second;
		const auto canonicalRequest = CanonicalizeLegacyImportRequest(operation.request);
		if (!canonicalRequest || !(*canonicalRequest == operation.request)
			|| !IsPersistedLegacyImportStatus(operation.result.status)
			|| operation.result.revision > revision
			|| !AppendString(plaintext.bytes, operation.request.operationId,
				kMaximumSecretVaultOperationIdBytes, false)
			|| !AppendString(plaintext.bytes, operation.request.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| operation.request.entries.size() > kMaximumEntries) {
			return false;
		}
		AppendUnsigned<std::uint32_t>(plaintext.bytes,
			static_cast<std::uint32_t>(operation.request.entries.size()));
		for (const auto& entry : operation.request.entries) {
			if (!AppendString(plaintext.bytes, entry.key, kMaximumSecretVaultKeyBytes, false)
				|| !AppendString(plaintext.bytes, entry.value, kMaximumSecretVaultValueBytes)) {
				return false;
			}
		}
		AppendUnsigned<std::uint8_t>(plaintext.bytes, operation.request.expectedRevision ? 1 : 0);
		if (operation.request.expectedRevision) {
			AppendUnsigned<std::uint64_t>(plaintext.bytes, *operation.request.expectedRevision);
		}
		AppendUnsigned<std::uint8_t>(plaintext.bytes,
			static_cast<std::uint8_t>(operation.result.status));
		AppendUnsigned<std::uint64_t>(plaintext.bytes, operation.result.revision);
		if (plaintext.bytes.size() > kMaximumPlaintextBytes) {
			return false;
		}
	}

	std::vector<std::uint8_t> ciphertext;
	if (!ProtectForProfile(plaintext.bytes, m_profileId, ciphertext)) {
		SecureErase(ciphertext);
		return false;
	}
	std::vector<std::uint8_t> envelope;
	envelope.reserve(kEnvelopeHeaderBytes + ciphertext.size());
	envelope.insert(envelope.end(), kEnvelopeMagic.begin(), kEnvelopeMagic.end());
	AppendUnsigned<std::uint32_t>(envelope, kEnvelopeVersion);
	AppendUnsigned<std::uint32_t>(envelope, static_cast<std::uint32_t>(ciphertext.size()));
	envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
	const bool written = envelope.size() <= kMaximumPersistedBytes
		&& m_fileOperations->WriteFileAtomically(StatePath(), envelope);
	SecureErase(ciphertext);
	SecureErase(envelope);
	return written;
}

WindowsDpapiSecretVaultOpenResult CWindowsDpapiSecretVaultService::Decode(
	std::span<const std::uint8_t> encryptedEnvelope)
{
	if (encryptedEnvelope.size() < kEnvelopeHeaderBytes
		|| encryptedEnvelope.size() > kMaximumPersistedBytes
		|| !std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), encryptedEnvelope.begin())) {
		return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault envelope is corrupt" };
	}
	std::size_t offset = kEnvelopeMagic.size();
	std::uint32_t version = 0;
	std::uint32_t ciphertextBytes = 0;
	if (!ReadUnsigned(encryptedEnvelope, offset, version)
		|| !ReadUnsigned(encryptedEnvelope, offset, ciphertextBytes)
		|| version != kEnvelopeVersion
		|| ciphertextBytes == 0 || ciphertextBytes != encryptedEnvelope.size() - offset) {
		return { version != kEnvelopeVersion ? EWindowsDpapiSecretVaultOpenStatus::UnsupportedFormat
			: EWindowsDpapiSecretVaultOpenStatus::CorruptData,
			version != kEnvelopeVersion ? "secret vault envelope format is unsupported"
			: "secret vault envelope is corrupt" };
	}
	SensitiveBytes plaintext;
	if (!UnprotectForProfile(encryptedEnvelope.subspan(offset, ciphertextBytes), m_profileId,
		plaintext.bytes)) {
		return { EWindowsDpapiSecretVaultOpenStatus::CryptoError, "secret vault decryption failed" };
	}
	const auto plain = std::span<const std::uint8_t>(plaintext.bytes);
	if (plain.size() < kPlaintextMagic.size() + sizeof(std::uint32_t)
		|| !std::equal(kPlaintextMagic.begin(), kPlaintextMagic.end(), plain.begin())) {
		return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault plaintext is corrupt" };
	}
	offset = kPlaintextMagic.size();
	std::uint32_t plainVersion = 0;
	std::string storedProfileId;
	std::uint64_t revision = 0;
	std::uint32_t entryCount = 0;
	std::uint32_t completedCount = 0;
	std::uint32_t migrationCount = 0;
	std::uint32_t completedLegacyImportCount = 0;
	if (!ReadUnsigned(plain, offset, plainVersion)) {
		return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault plaintext is corrupt" };
	}
	if (plainVersion != kPlaintextVersion) {
		return { EWindowsDpapiSecretVaultOpenStatus::UnsupportedFormat,
			"secret vault plaintext format is unsupported" };
	}
	if (!ReadString(plain, offset, storedProfileId, kMaximumSecretVaultProfileIdBytes, false)
		|| storedProfileId != m_profileId
		|| !ReadUnsigned(plain, offset, revision)
		|| !ReadUnsigned(plain, offset, entryCount)
		|| !ReadUnsigned(plain, offset, completedCount)
		|| !ReadUnsigned(plain, offset, migrationCount)
		|| !ReadUnsigned(plain, offset, completedLegacyImportCount)
		|| entryCount > kMaximumEntries || completedCount > m_maxCompletedOperations
		|| migrationCount > kMaximumEntries
		|| completedLegacyImportCount > m_maxCompletedOperations) {
		SecureErase(storedProfileId);
		return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault profile binding or bounds are invalid" };
	}
	SecureErase(storedProfileId);
	std::map<SecretAddress, SecretEntry> entries;
	for (std::uint32_t index = 0; index < entryCount; ++index) {
		SecretEntry entry;
		if (!ReadString(plain, offset, entry.address.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| !ReadString(plain, offset, entry.address.key, kMaximumSecretVaultKeyBytes, false)
			|| !ReadString(plain, offset, entry.value, kMaximumSecretVaultValueBytes)
			|| !ReadUnsigned(plain, offset, entry.revision) || !entry.address.IsValid()
			|| entry.revision == 0 || entry.revision > revision
			|| !entries.emplace(entry.address, std::move(entry)).second) {
			for (auto& [address, value] : entries) { (void)address; SecureErase(value.value); }
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault entry is invalid" };
		}
	}
	std::map<std::string, CompletedOperation> completed;
	std::deque<std::string> completedOrder;
	for (std::uint32_t index = 0; index < completedCount; ++index) {
		CompletedOperation operation;
		std::uint8_t kind = 0;
		std::uint8_t hasExpected = 0;
		std::uint8_t status = 0;
		if (!ReadString(plain, offset, operation.request.operationId,
				kMaximumSecretVaultOperationIdBytes, false)
			|| !ReadUnsigned(plain, offset, kind)
			|| kind > static_cast<std::uint8_t>(ESecretMutationKind::Delete)
			|| !ReadString(plain, offset, operation.request.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| !ReadString(plain, offset, operation.request.key, kMaximumSecretVaultKeyBytes, false)
			|| !ReadString(plain, offset, operation.request.value, kMaximumSecretVaultValueBytes)
			|| !ReadUnsigned(plain, offset, hasExpected) || hasExpected > 1) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault replay record is invalid" };
		}
		operation.request.kind = static_cast<ESecretMutationKind>(kind);
		if (hasExpected) {
			std::uint64_t expected = 0;
			if (!ReadUnsigned(plain, offset, expected)) {
				return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault replay record is invalid" };
			}
			operation.request.expectedRevision = expected;
		}
		if (!ReadUnsigned(plain, offset, status)
			|| !ReadUnsigned(plain, offset, operation.result.revision)
			|| status > static_cast<std::uint8_t>(ESecretMutationStatus::Failed)) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault replay result is invalid" };
		}
		operation.result.status = static_cast<ESecretMutationStatus>(status);
		const auto canonicalRequest = CanonicalizeRequest(operation.request);
		if (!canonicalRequest || !(*canonicalRequest == operation.request)
			|| !IsPersistedStatus(operation.result.status)
			|| operation.result.revision > revision
			|| (operation.result.status == ESecretMutationStatus::Succeeded
				&& operation.result.revision == 0)
			|| completed.contains(operation.request.operationId)) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault replay record is invalid" };
		}
		const auto operationId = operation.request.operationId;
		completed.emplace(operationId, std::move(operation));
		completedOrder.emplace_back(operationId);
	}
	std::map<std::string, std::uint64_t> migrationCompleteExtensions;
	for (std::uint32_t index = 0; index < migrationCount; ++index) {
		std::string extensionId;
		std::uint64_t completedRevision = 0;
		std::string canonicalExtensionId;
		if (!ReadString(plain, offset, extensionId, kMaximumSecretVaultExtensionIdBytes, false)
			|| !ReadUnsigned(plain, offset, completedRevision)
			|| !CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
			|| canonicalExtensionId != extensionId || completedRevision == 0
			|| completedRevision > revision
			|| !migrationCompleteExtensions.emplace(std::move(extensionId), completedRevision).second) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
				"secret vault migration marker is invalid" };
		}
	}
	std::map<std::string, CompletedLegacyImport> completedLegacyImports;
	std::deque<std::string> completedLegacyImportOrder;
	for (std::uint32_t index = 0; index < completedLegacyImportCount; ++index) {
		CompletedLegacyImport operation;
		std::uint32_t importedEntryCount = 0;
		std::uint8_t hasExpected = 0;
		std::uint8_t status = 0;
		if (!ReadString(plain, offset, operation.request.operationId,
				kMaximumSecretVaultOperationIdBytes, false)
			|| !ReadString(plain, offset, operation.request.extensionId,
				kMaximumSecretVaultExtensionIdBytes, false)
			|| !ReadUnsigned(plain, offset, importedEntryCount)
			|| importedEntryCount > kMaximumEntries) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
				"secret vault legacy import record is invalid" };
		}
		operation.request.entries.reserve(importedEntryCount);
		for (std::uint32_t entryIndex = 0; entryIndex < importedEntryCount; ++entryIndex) {
			LegacySecretVaultEntry entry;
			if (!ReadString(plain, offset, entry.key, kMaximumSecretVaultKeyBytes, false)
				|| !ReadString(plain, offset, entry.value, kMaximumSecretVaultValueBytes)) {
				return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
					"secret vault legacy import record is invalid" };
			}
			operation.request.entries.emplace_back(std::move(entry));
		}
		if (!ReadUnsigned(plain, offset, hasExpected) || hasExpected > 1) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
				"secret vault legacy import record is invalid" };
		}
		if (hasExpected) {
			std::uint64_t expected = 0;
			if (!ReadUnsigned(plain, offset, expected)) {
				return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
					"secret vault legacy import record is invalid" };
			}
			operation.request.expectedRevision = expected;
		}
		if (!ReadUnsigned(plain, offset, status)
			|| !ReadUnsigned(plain, offset, operation.result.revision)
			|| status > static_cast<std::uint8_t>(ELegacySecretVaultImportStatus::Failed)) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
				"secret vault legacy import result is invalid" };
		}
		operation.result.status = static_cast<ELegacySecretVaultImportStatus>(status);
		const auto canonicalRequest = CanonicalizeLegacyImportRequest(operation.request);
		if (!canonicalRequest || !(*canonicalRequest == operation.request)
			|| !IsPersistedLegacyImportStatus(operation.result.status)
			|| operation.result.revision > revision
			|| (operation.result.status == ELegacySecretVaultImportStatus::Succeeded
				&& operation.result.revision == 0)
			|| completedLegacyImports.contains(operation.request.operationId)
			|| completed.contains(operation.request.operationId)) {
			return { EWindowsDpapiSecretVaultOpenStatus::CorruptData,
				"secret vault legacy import record is invalid" };
		}
		const auto operationId = operation.request.operationId;
		completedLegacyImports.emplace(operationId, std::move(operation));
		completedLegacyImportOrder.emplace_back(operationId);
	}
	if (offset != plain.size()) {
		return { EWindowsDpapiSecretVaultOpenStatus::CorruptData, "secret vault plaintext has trailing data" };
	}
	m_revision = revision;
	m_entries = std::move(entries);
	m_completedOperations = std::move(completed);
	m_completedOperationOrder = std::move(completedOrder);
	m_migrationCompleteExtensions = std::move(migrationCompleteExtensions);
	m_completedLegacyImports = std::move(completedLegacyImports);
	m_completedLegacyImportOrder = std::move(completedLegacyImportOrder);
	return { EWindowsDpapiSecretVaultOpenStatus::Opened, {} };
}

SecretMutationResult CWindowsDpapiSecretVaultService::Apply(const SecretMutationRequest& request)
{
	auto canonicalRequest = CanonicalizeRequest(request);
	if (!canonicalRequest) {
		std::scoped_lock lock(m_mutex);
		return { .status = m_stopped ? ESecretMutationStatus::Stopped : ESecretMutationStatus::Invalid,
			.revision = m_revision, .diagnostic = m_stopped ? std::string{} : std::string(kDiagnosticInvalidRequest) };
	}
	struct RequestWiper final {
		SecretMutationRequest& request;
		~RequestWiper() { SecureErase(request.value); }
	} requestWiper{ *canonicalRequest };

	std::unique_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretMutationStatus::Stopped, .revision = m_revision };
	}
	if (!IsAvailableLocked()) {
		return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticNotOpen) };
	}
	if (const auto completed = m_completedOperations.find(canonicalRequest->operationId);
		completed != m_completedOperations.end()) {
		if (!(completed->second.request == *canonicalRequest)) {
			return { .status = ESecretMutationStatus::Invalid, .revision = m_revision,
				.diagnostic = std::string(kDiagnosticOperationCollision) };
		}
		auto replay = completed->second.result;
		replay.replayed = true;
		replay.change.reset();
		return replay;
	}
	if (m_completedLegacyImports.contains(canonicalRequest->operationId)) {
		return { .status = ESecretMutationStatus::Invalid, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticOperationCollision) };
	}

	auto wipeCompleted = [](auto& completed) noexcept {
		for (auto& [operationId, operation] : completed) {
			(void)operationId;
			SecureErase(operation.request.value);
		}
	};
	if (canonicalRequest->expectedRevision && *canonicalRequest->expectedRevision != m_revision) {
		SecretMutationResult conflict{ .status = ESecretMutationStatus::Conflict, .revision = m_revision };
		auto completed = m_completedOperations;
		auto order = m_completedOperationOrder;
		if (!RememberCompleted(completed, order, *canonicalRequest, conflict)
			|| !PersistCandidate(m_entries, completed, order, m_migrationCompleteExtensions,
				m_completedLegacyImports, m_completedLegacyImportOrder, m_revision)) {
			wipeCompleted(completed);
			return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
				.diagnostic = std::string(kDiagnosticPersistenceFailed) };
		}
		wipeCompleted(m_completedOperations);
		m_completedOperations = std::move(completed);
		m_completedOperationOrder = std::move(order);
		return conflict;
	}

	const SecretAddress address{ .extensionId = canonicalRequest->extensionId, .key = canonicalRequest->key };
	const auto found = m_entries.find(address);
	const bool present = found != m_entries.end();
	const bool effective = canonicalRequest->kind == ESecretMutationKind::Set
		? !present || found->second.value != canonicalRequest->value : present;
	if (!effective) {
		SecretMutationResult noChange{ .status = ESecretMutationStatus::NotApplicable, .revision = m_revision };
		auto completed = m_completedOperations;
		auto order = m_completedOperationOrder;
		if (!RememberCompleted(completed, order, *canonicalRequest, noChange)
			|| !PersistCandidate(m_entries, completed, order, m_migrationCompleteExtensions,
				m_completedLegacyImports, m_completedLegacyImportOrder, m_revision)) {
			wipeCompleted(completed);
			return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
				.diagnostic = std::string(kDiagnosticPersistenceFailed) };
		}
		wipeCompleted(m_completedOperations);
		m_completedOperations = std::move(completed);
		m_completedOperationOrder = std::move(order);
		return noChange;
	}
	if (m_revision == (std::numeric_limits<std::uint64_t>::max)()) {
		return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticRevisionExhausted) };
	}

	const std::uint64_t nextRevision = m_revision + 1;
	auto entries = m_entries;
	if (canonicalRequest->kind == ESecretMutationKind::Set) {
		entries.insert_or_assign(address, SecretEntry{ .address = address, .value = canonicalRequest->value,
			.revision = nextRevision });
	} else {
		entries.erase(address);
	}
	const SecretChange change{ .profileId = m_profileId, .address = address,
		.kind = canonicalRequest->kind == ESecretMutationKind::Set ? ESecretChangeKind::Set
			: ESecretChangeKind::Delete, .revision = nextRevision };
	SecretMutationResult succeeded{ .status = ESecretMutationStatus::Succeeded, .revision = nextRevision,
		.change = change };
	auto completed = m_completedOperations;
	auto order = m_completedOperationOrder;
	if (!RememberCompleted(completed, order, *canonicalRequest, succeeded)
		|| !PersistCandidate(entries, completed, order, m_migrationCompleteExtensions,
			m_completedLegacyImports, m_completedLegacyImportOrder, nextRevision)) {
		for (auto& [candidateAddress, entry] : entries) { (void)candidateAddress; SecureErase(entry.value); }
		wipeCompleted(completed);
		return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticPersistenceFailed) };
	}
	for (auto& [oldAddress, entry] : m_entries) { (void)oldAddress; SecureErase(entry.value); }
	wipeCompleted(m_completedOperations);
	m_entries = std::move(entries);
	m_completedOperations = std::move(completed);
	m_completedOperationOrder = std::move(order);
	m_revision = nextRevision;
	const bool drain = EnqueueNotification(change);
	lock.unlock();
	if (drain) {
		DrainNotifications();
	}
	return succeeded;
}

LegacySecretVaultImportResult CWindowsDpapiSecretVaultService::ImportLegacy(
	const LegacySecretVaultImportRequest& request)
{
	auto canonicalRequest = CanonicalizeLegacyImportRequest(request);
	if (!canonicalRequest) {
		std::scoped_lock lock(m_mutex);
		return { .status = m_stopped ? ELegacySecretVaultImportStatus::Stopped
			: ELegacySecretVaultImportStatus::Invalid, .revision = m_revision,
			.diagnostic = m_stopped ? std::string{} : std::string(kDiagnosticLegacyImportInvalid) };
	}
	struct LegacyRequestWiper final {
		LegacySecretVaultImportRequest& request;
		~LegacyRequestWiper()
		{
			for (auto& entry : request.entries) {
				SecureErase(entry.value);
			}
		}
	} requestWiper{ *canonicalRequest };

	auto wipeLegacyImports = [](auto& completed) noexcept {
		for (auto& [operationId, operation] : completed) {
			(void)operationId;
			for (auto& entry : operation.request.entries) {
				SecureErase(entry.value);
			}
		}
	};
	std::unique_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ELegacySecretVaultImportStatus::Stopped, .revision = m_revision };
	}
	if (!IsAvailableLocked()) {
		return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticNotOpen) };
	}
	if (const auto completed = m_completedLegacyImports.find(canonicalRequest->operationId);
		completed != m_completedLegacyImports.end()) {
		if (!(completed->second.request == *canonicalRequest)) {
			return { .status = ELegacySecretVaultImportStatus::Invalid, .revision = m_revision,
				.diagnostic = std::string(kDiagnosticLegacyImportCollision) };
		}
		auto replay = completed->second.result;
		replay.replayed = true;
		return replay;
	}
	if (m_completedOperations.contains(canonicalRequest->operationId)) {
		return { .status = ELegacySecretVaultImportStatus::Invalid, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticLegacyImportCollision) };
	}

	auto persistResultOnly = [&](const LegacySecretVaultImportResult& result)
		-> std::optional<LegacySecretVaultImportResult> {
		auto completed = m_completedLegacyImports;
		auto order = m_completedLegacyImportOrder;
		if (!RememberLegacyImport(completed, order, *canonicalRequest, result)
			|| !PersistCandidate(m_entries, m_completedOperations, m_completedOperationOrder,
				m_migrationCompleteExtensions, completed, order, m_revision)) {
			wipeLegacyImports(completed);
			return std::nullopt;
		}
		wipeLegacyImports(m_completedLegacyImports);
		m_completedLegacyImports = std::move(completed);
		m_completedLegacyImportOrder = std::move(order);
		return result;
	};
	if (canonicalRequest->expectedRevision && *canonicalRequest->expectedRevision != m_revision) {
		const LegacySecretVaultImportResult conflict{
			.status = ELegacySecretVaultImportStatus::Conflict, .revision = m_revision };
		if (const auto persisted = persistResultOnly(conflict)) {
			return *persisted;
		}
		return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticPersistenceFailed) };
	}
	if (m_migrationCompleteExtensions.contains(canonicalRequest->extensionId)) {
		const LegacySecretVaultImportResult alreadyImported{
			.status = ELegacySecretVaultImportStatus::AlreadyImported, .revision = m_revision };
		if (const auto persisted = persistResultOnly(alreadyImported)) {
			return *persisted;
		}
		return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticPersistenceFailed) };
	}
	if (m_revision == (std::numeric_limits<std::uint64_t>::max)()) {
		return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticRevisionExhausted) };
	}
	for (const auto& imported : canonicalRequest->entries) {
		const SecretAddress address{ .extensionId = canonicalRequest->extensionId, .key = imported.key };
		if (const auto current = m_entries.find(address); current != m_entries.end()
			&& current->second.value != imported.value) {
			const LegacySecretVaultImportResult conflict{
				.status = ELegacySecretVaultImportStatus::Conflict, .revision = m_revision };
			if (const auto persisted = persistResultOnly(conflict)) {
				return *persisted;
			}
			return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
				.diagnostic = std::string(kDiagnosticPersistenceFailed) };
		}
	}

	const std::uint64_t nextRevision = m_revision + 1;
	auto entries = m_entries;
	std::vector<SecretChange> changes;
	changes.reserve(canonicalRequest->entries.size());
	for (const auto& imported : canonicalRequest->entries) {
		const SecretAddress address{ .extensionId = canonicalRequest->extensionId, .key = imported.key };
		if (entries.contains(address)) {
			continue;
		}
		entries.emplace(address, SecretEntry{ .address = address, .value = imported.value,
			.revision = nextRevision });
		changes.emplace_back(SecretChange{ .profileId = m_profileId, .address = address,
			.kind = ESecretChangeKind::Set, .revision = nextRevision });
	}
	auto migrationCompleteExtensions = m_migrationCompleteExtensions;
	migrationCompleteExtensions.emplace(canonicalRequest->extensionId, nextRevision);
	const LegacySecretVaultImportResult succeeded{
		.status = ELegacySecretVaultImportStatus::Succeeded, .revision = nextRevision };
	auto completed = m_completedLegacyImports;
	auto order = m_completedLegacyImportOrder;
	if (!RememberLegacyImport(completed, order, *canonicalRequest, succeeded)
		|| !PersistCandidate(entries, m_completedOperations, m_completedOperationOrder,
			migrationCompleteExtensions, completed, order, nextRevision)) {
		for (auto& [address, entry] : entries) { (void)address; SecureErase(entry.value); }
		wipeLegacyImports(completed);
		return { .status = ELegacySecretVaultImportStatus::Failed, .revision = m_revision,
			.diagnostic = std::string(kDiagnosticPersistenceFailed) };
	}
	for (auto& [address, entry] : m_entries) { (void)address; SecureErase(entry.value); }
	wipeLegacyImports(m_completedLegacyImports);
	m_entries = std::move(entries);
	m_migrationCompleteExtensions = std::move(migrationCompleteExtensions);
	m_completedLegacyImports = std::move(completed);
	m_completedLegacyImportOrder = std::move(order);
	m_revision = nextRevision;
	bool drain = false;
	for (const auto& change : changes) {
		drain = EnqueueNotification(change) || drain;
	}
	lock.unlock();
	if (drain) {
		DrainNotifications();
	}
	return succeeded;
}

std::unique_ptr<ISecretVaultChangeSubscription> CWindowsDpapiSecretVaultService::Subscribe(
	SecretChangeCallback callback)
{
	if (!callback) {
		return nullptr;
	}
	std::scoped_lock vaultLock(m_mutex);
	if (!IsAvailableLocked()) {
		return nullptr;
	}
	std::scoped_lock subscriptionLock(m_subscriptionState->mutex);
	if (m_subscriptionState->closed
		|| m_subscriptionState->slots.size() >= m_subscriptionState->maximumSubscriptions
		|| m_subscriptionState->nextSubscriptionId == 0) {
		return nullptr;
	}
	const auto subscriptionId = m_subscriptionState->nextSubscriptionId++;
	m_subscriptionState->slots.emplace(subscriptionId,
		std::make_shared<SubscriptionSlot>(std::move(callback)));
	return std::make_unique<SecretVaultSubscription>(m_subscriptionState, subscriptionId);
}

bool CWindowsDpapiSecretVaultService::EnqueueNotification(const SecretChange& change) noexcept
{
	try {
		std::scoped_lock lock(m_notificationMutex);
		m_notificationQueue.push_back(change);
		if (m_dispatchingNotifications) {
			return false;
		}
		m_dispatchingNotifications = true;
		return true;
	} catch (...) {
		return false;
	}
}

void CWindowsDpapiSecretVaultService::DrainNotifications() noexcept
{
	for (;;) {
		SecretChange change;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) {
				m_dispatchingNotifications = false;
				return;
			}
			change = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		DeliverNotification(m_subscriptionState, change);
	}
}

ESecretVaultStopStatus CWindowsDpapiSecretVaultService::Stop() noexcept
{
	try {
		std::scoped_lock vaultLock(m_mutex);
		if (m_stopped) {
			return ESecretVaultStopStatus::AlreadyStopped;
		}
		m_stopped = true;
		m_open = false;
		{
			std::scoped_lock subscriptionLock(m_subscriptionState->mutex);
			m_subscriptionState->closed = true;
			for (const auto& [subscriptionId, slot] : m_subscriptionState->slots) {
				(void)subscriptionId;
				slot->active.store(false, std::memory_order_release);
			}
			m_subscriptionState->slots.clear();
		}
		{
			std::scoped_lock notificationLock(m_notificationMutex);
			m_notificationQueue.clear();
			m_dispatchingNotifications = false;
		}
		for (auto& [address, entry] : m_entries) {
			(void)address;
			SecureErase(entry.value);
		}
		for (auto& [operationId, operation] : m_completedOperations) {
			(void)operationId;
			SecureErase(operation.request.value);
		}
		for (auto& [operationId, operation] : m_completedLegacyImports) {
			(void)operationId;
			for (auto& entry : operation.request.entries) {
				SecureErase(entry.value);
			}
		}
		m_entries.clear();
		m_completedOperations.clear();
		m_completedOperationOrder.clear();
		m_migrationCompleteExtensions.clear();
		m_completedLegacyImports.clear();
		m_completedLegacyImportOrder.clear();
		m_revision = 0;
		m_writerLock.reset();
		return ESecretVaultStopStatus::Stopped;
	} catch (...) {
		m_stopped = true;
		return ESecretVaultStopStatus::Stopped;
	}
}

} // namespace platform::secrets
