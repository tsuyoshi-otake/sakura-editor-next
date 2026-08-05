/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "CAtomicFileStorageService.h"
#include <sakura/storage/StorageAuthorityFactory.h>
#include <sakura/security/CurrentUserSecurityAttributes.h>

#include <Windows.h>
#include <Aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <set>
#include <type_traits>

namespace platform::storage {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = { 'S', 'A', 'K', 'S', 'T', 'O', 'R', '1' };
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kSnapshotFixedBytes = 20;
constexpr std::size_t kSnapshotEntryFixedBytes = 26;

std::string Error(std::string_view operation, DWORD value)
{
	return std::string(operation) + " failed (error " + std::to_string(value) + ")";
}

bool IsUtf8(std::string_view value) noexcept
{
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool IsBoundedString(std::string_view value, bool allowEmpty = true) noexcept
{
	return (allowEmpty || !value.empty()) && value.size() <= CAtomicFileStorageService::kMaximumStringBytes &&
		value.find('\0') == std::string_view::npos && IsUtf8(value);
}

bool IsSecretOwner(std::string_view owner) noexcept
{
	return owner == "secret" || owner == "secrets" || owner.starts_with("secret.") || owner.starts_with("secrets.");
}

template<typename T>
void Append(std::vector<std::uint8_t>& output, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t i = 0; i < sizeof(T); ++i) output.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

template<typename T>
bool Read(std::span<const std::uint8_t> input, std::size_t& offset, T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > input.size() || input.size() - offset < sizeof(T)) return false;
	value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(input[offset++]) << (i * 8);
	return true;
}

bool AppendString(std::vector<std::uint8_t>& output, std::string_view value)
{
	if (!IsBoundedString(value) || value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
	Append<std::uint32_t>(output, static_cast<std::uint32_t>(value.size()));
	output.insert(output.end(), value.begin(), value.end());
	return output.size() <= CAtomicFileStorageService::kMaximumPersistedBytes;
}

bool ReadString(std::span<const std::uint8_t> input, std::size_t& offset, std::string& value)
{
	std::uint32_t length = 0;
	if (!Read(input, offset, length) || offset > input.size() || length > CAtomicFileStorageService::kMaximumStringBytes || length > input.size() - offset) return false;
	value.assign(reinterpret_cast<const char*>(input.data() + offset), length);
	offset += length;
	return IsBoundedString(value);
}

std::uint64_t Checksum(std::span<const std::uint8_t> input) noexcept
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const auto value : input) {
		hash ^= value;
		hash *= 1099511628211ull;
	}
	return hash;
}

bool IsScope(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(EStorageScope::Workspace); }
bool IsTarget(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(EStorageTarget::Machine); }
bool IsStatus(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(EStorageMutationStatus::NotApplicable); }

bool AppendAddress(std::vector<std::uint8_t>& output, const StorageAddress& address)
{
	return address.IsValid() && !IsSecretOwner(address.owner) &&
		(Append<std::uint8_t>(output, static_cast<std::uint8_t>(address.scope)), true) &&
		AppendString(output, address.scopeId) && AppendString(output, address.owner) && AppendString(output, address.key);
}

bool ReadAddress(std::span<const std::uint8_t> input, std::size_t& offset, StorageAddress& address)
{
	std::uint8_t scope = 0;
	if (!Read(input, offset, scope) || !IsScope(scope)) return false;
	address.scope = static_cast<EStorageScope>(scope);
	return ReadString(input, offset, address.scopeId) && ReadString(input, offset, address.owner) &&
		ReadString(input, offset, address.key) && address.IsValid() && !IsSecretOwner(address.owner);
}

bool AppendEntry(std::vector<std::uint8_t>& output, const StorageEntry& entry)
{
	return AppendAddress(output, entry.address) && IsTarget(static_cast<std::uint8_t>(entry.target)) &&
		(Append<std::uint8_t>(output, static_cast<std::uint8_t>(entry.target)), true) && AppendString(output, entry.value) &&
		(Append<std::uint64_t>(output, entry.revision), true);
}

bool ReadEntry(std::span<const std::uint8_t> input, std::size_t& offset, StorageEntry& entry)
{
	std::uint8_t target = 0;
	if (!ReadAddress(input, offset, entry.address) || !Read(input, offset, target) || !IsTarget(target) ||
		!ReadString(input, offset, entry.value) || !Read(input, offset, entry.revision) || entry.revision == 0) return false;
	entry.target = static_cast<EStorageTarget>(target);
	return true;
}

bool AppendMutation(std::vector<std::uint8_t>& output, const StorageMutation& mutation)
{
	if (!AppendAddress(output, mutation.address) || !IsTarget(static_cast<std::uint8_t>(mutation.target))) return false;
	Append<std::uint8_t>(output, static_cast<std::uint8_t>(mutation.target));
	Append<std::uint8_t>(output, mutation.value ? 1 : 0);
	return !mutation.value || AppendString(output, *mutation.value);
}

bool ReadMutation(std::span<const std::uint8_t> input, std::size_t& offset, StorageMutation& mutation)
{
	std::uint8_t target = 0;
	std::uint8_t hasValue = 0;
	if (!ReadAddress(input, offset, mutation.address) || !Read(input, offset, target) || !IsTarget(target) ||
		!Read(input, offset, hasValue) || hasValue > 1) return false;
	mutation.target = static_cast<EStorageTarget>(target);
	if (hasValue) {
		std::string value;
		if (!ReadString(input, offset, value)) return false;
		mutation.value = std::move(value);
	}
	return true;
}

bool AppendBatch(std::vector<std::uint8_t>& output, const StorageChangeBatch& batch)
{
	if (batch.generation == 0 || batch.revision == 0 || batch.changes.size() > CAtomicFileStorageService::kMaximumItems) return false;
	Append<std::uint64_t>(output, batch.generation);
	Append<std::uint64_t>(output, batch.baseRevision);
	Append<std::uint64_t>(output, batch.revision);
	Append<std::uint32_t>(output, static_cast<std::uint32_t>(batch.changes.size()));
	for (const auto& change : batch.changes) {
		if (!AppendAddress(output, change.address) || !IsTarget(static_cast<std::uint8_t>(change.target))) return false;
		Append<std::uint8_t>(output, static_cast<std::uint8_t>(change.target));
		Append<std::uint8_t>(output, change.entry ? 1 : 0);
		if (change.entry && !AppendEntry(output, *change.entry)) return false;
	}
	return output.size() <= CAtomicFileStorageService::kMaximumPersistedBytes;
}

bool FitsControlWire(const StorageChangeBatch& batch)
{
	std::vector<std::uint8_t> encoded;
	return AppendBatch(encoded, batch) && encoded.size() <= kMaximumStorageMutationPayloadBytes;
}

bool ReadBatch(std::span<const std::uint8_t> input, std::size_t& offset, StorageChangeBatch& batch)
{
	std::uint32_t count = 0;
	if (!Read(input, offset, batch.generation) || batch.generation == 0 || !Read(input, offset, batch.baseRevision) ||
		!Read(input, offset, batch.revision) || batch.revision == 0 || !Read(input, offset, count) ||
		batch.baseRevision >= batch.revision || count > CAtomicFileStorageService::kMaximumItems) return false;
	batch.changes.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) {
		StorageChange change;
		std::uint8_t target = 0, hasEntry = 0;
		if (!ReadAddress(input, offset, change.address) || !Read(input, offset, target) || !IsTarget(target) ||
			!Read(input, offset, hasEntry) || hasEntry > 1) return false;
		change.target = static_cast<EStorageTarget>(target);
		if (hasEntry) {
			StorageEntry entry;
			if (!ReadEntry(input, offset, entry) || !(entry.address == change.address) || entry.target != change.target) return false;
			change.entry = std::move(entry);
		}
		batch.changes.emplace_back(std::move(change));
	}
	return true;
}

bool InitializeCurrentUserSecurity(platform::security::CurrentUserSecurityAttributes& security, std::string& diagnostic)
{
	std::wstring wideDiagnostic;
	if (security.Initialize(wideDiagnostic)) return true;
	diagnostic = "Initialize current-user storage security failed";
	return false;
}

PACL CurrentUserDacl(platform::security::CurrentUserSecurityAttributes& security) noexcept
{
	SECURITY_ATTRIBUTES* const attributes = security.Attributes();
	BOOL present = FALSE;
	BOOL defaulted = FALSE;
	PACL dacl = nullptr;
	return attributes && attributes->lpSecurityDescriptor
		&& ::GetSecurityDescriptorDacl(attributes->lpSecurityDescriptor, &present, &dacl, &defaulted)
		&& present && dacl ? dacl : nullptr;
}

bool RestrictPathToCurrentUser(const std::filesystem::path& path, platform::security::CurrentUserSecurityAttributes& security,
	std::string& diagnostic) noexcept
{
	PACL dacl = CurrentUserDacl(security);
	if (!dacl) { diagnostic = "current-user storage DACL is unavailable"; return false; }
	const DWORD result = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
	if (result == ERROR_SUCCESS) return true;
	diagnostic = Error("Protect storage path DACL", result);
	return false;
}

bool RestrictHandleToCurrentUser(HANDLE handle, platform::security::CurrentUserSecurityAttributes& security,
	std::string& diagnostic) noexcept
{
	PACL dacl = CurrentUserDacl(security);
	if (!dacl) { diagnostic = "current-user storage DACL is unavailable"; return false; }
	const DWORD result = ::SetSecurityInfo(handle, SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
	if (result == ERROR_SUCCESS) return true;
	diagnostic = Error("Protect storage handle DACL", result);
	return false;
}

class Win32WriterLock final : public IAtomicFileStorageWriterLock {
public:
	explicit Win32WriterLock(HANDLE value) noexcept : m_value(value) {}
	~Win32WriterLock() override
	{
		if (m_value != INVALID_HANDLE_VALUE) {
			OVERLAPPED overlapped{};
			(void)::UnlockFileEx(m_value, 0, MAXDWORD, MAXDWORD, &overlapped);
			::CloseHandle(m_value);
		}
	}
private:
	HANDLE m_value = INVALID_HANDLE_VALUE;
};

class Win32FileOperations final : public IAtomicFileStorageFileOperations {
public:
	bool PrepareDirectory(const std::filesystem::path& directory, std::string& diagnostic) override
	{
		platform::security::CurrentUserSecurityAttributes security;
		if (!InitializeCurrentUserSecurity(security, diagnostic)) return false;
		if (!::CreateDirectoryW(directory.c_str(), security.Attributes())) {
			const DWORD error = ::GetLastError();
			if (error != ERROR_ALREADY_EXISTS) { diagnostic = Error("Create storage directory", error); return false; }
			const DWORD attributes = ::GetFileAttributesW(directory.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) { diagnostic = "storage root is not a directory"; return false; }
		}
		return RestrictPathToCurrentUser(directory, security, diagnostic);
	}

	AtomicFileStorageWriterLockResult AcquireWriterLock(const std::filesystem::path& path) override
	{
		platform::security::CurrentUserSecurityAttributes security;
		std::string diagnostic;
		if (!InitializeCurrentUserSecurity(security, diagnostic)) return { EAtomicFileStorageWriterLockStatus::IoError, nullptr, std::move(diagnostic) };
		HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | WRITE_DAC, FILE_SHARE_READ, security.Attributes(), OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			return { error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? EAtomicFileStorageWriterLockStatus::Busy : EAtomicFileStorageWriterLockStatus::IoError,
				nullptr, Error("Open storage writer lock", error) };
		}
		if (!RestrictHandleToCurrentUser(handle, security, diagnostic)) {
			::CloseHandle(handle);
			return { EAtomicFileStorageWriterLockStatus::IoError, nullptr, std::move(diagnostic) };
		}
		OVERLAPPED overlapped{};
		if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped)) {
			const DWORD error = ::GetLastError();
			::CloseHandle(handle);
			return { error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION ? EAtomicFileStorageWriterLockStatus::Busy : EAtomicFileStorageWriterLockStatus::IoError,
				nullptr, Error("Acquire storage writer lock", error) };
		}
		return { EAtomicFileStorageWriterLockStatus::Acquired, std::make_unique<Win32WriterLock>(handle), {} };
	}

	bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, bool& found, std::string& diagnostic) override
	{
		found = false;
		HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
			diagnostic = Error("Open storage state", error);
			return false;
		}
		struct File final { HANDLE value; ~File() { ::CloseHandle(value); } } file{ handle };
		LARGE_INTEGER size{};
		if (!::GetFileSizeEx(handle, &size)) { diagnostic = Error("Read storage state size", ::GetLastError()); return false; }
		if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(CAtomicFileStorageService::kMaximumPersistedBytes)) { diagnostic = "storage state exceeds maximum size"; return false; }
		bytes.resize(static_cast<std::size_t>(size.QuadPart));
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>(1024 * 1024)));
			DWORD read = 0;
			if (!::ReadFile(handle, bytes.data() + offset, wanted, &read, nullptr) || read == 0) { diagnostic = Error("Read storage state", ::GetLastError()); return false; }
			offset += read;
		}
		found = true;
		return true;
	}

	bool WriteFileAtomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& diagnostic) override
	{
		platform::security::CurrentUserSecurityAttributes security;
		if (!InitializeCurrentUserSecurity(security, diagnostic)) return false;
		std::array<std::uint8_t, 8> random{};
		if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) { diagnostic = "Generate storage temporary filename failed"; return false; }
		static constexpr wchar_t hex[] = L"0123456789abcdef";
		std::wstring suffix;
		for (const auto value : random) { suffix.push_back(hex[value >> 4]); suffix.push_back(hex[value & 15]); }
		const auto temporary = path.parent_path() / (path.filename().wstring() + L"." + suffix + L".tmp");
		HANDLE handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security.Attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) { diagnostic = Error("Create storage temporary file", ::GetLastError()); return false; }
		bool written = true;
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>(1024 * 1024)));
			DWORD count = 0;
			if (!::WriteFile(handle, bytes.data() + offset, wanted, &count, nullptr) || count == 0) { diagnostic = Error("Write storage temporary file", ::GetLastError()); written = false; break; }
			offset += count;
		}
		if (written && !::FlushFileBuffers(handle)) { diagnostic = Error("Flush storage temporary file", ::GetLastError()); written = false; }
		::CloseHandle(handle);
		if (!written) { (void)::DeleteFileW(temporary.c_str()); return false; }
		if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			diagnostic = Error("Commit storage state", ::GetLastError());
			(void)::DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}
};

struct SubscriptionSlot {
	explicit SubscriptionSlot(StorageChangeCallback listener) : callback(std::move(listener)) {}
	std::atomic_bool active = true;
	StorageChangeCallback callback;
};

class Subscription final : public IStorageChangeSubscription {
public:
	Subscription(std::weak_ptr<CAtomicFileStorageService::SubscriptionState> state, std::uint64_t id) noexcept : m_state(std::move(state)), m_id(id) {}
	~Subscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;
private:
	std::weak_ptr<CAtomicFileStorageService::SubscriptionState> m_state;
	std::uint64_t m_id = 0;
};

} // namespace

struct CAtomicFileStorageService::CompletedOperation { StorageMutationRequest request; StorageMutationResult result; };
struct CAtomicFileStorageService::SubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::uint64_t nextId = 1;
	std::map<std::uint64_t, std::shared_ptr<SubscriptionSlot>> slots;
};

namespace {

void Subscription::Unsubscribe() noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_id == 0) { m_id = 0; return; }
		std::scoped_lock lock(state->mutex);
		if (const auto found = state->slots.find(m_id); found != state->slots.end()) { found->second->active.store(false); state->slots.erase(found); }
		m_id = 0;
	} catch (...) { m_id = 0; }
}

bool Subscription::IsSubscribed() const noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_id == 0) return false;
		std::scoped_lock lock(state->mutex);
		const auto found = state->slots.find(m_id);
		return !state->closed && found != state->slots.end() && found->second->active.load();
	} catch (...) { return false; }
}

void Deliver(const std::shared_ptr<CAtomicFileStorageService::SubscriptionState>& state, const StorageChangeBatch& batch)
{
	std::vector<std::shared_ptr<SubscriptionSlot>> slots;
	{
		std::scoped_lock lock(state->mutex);
		if (state->closed) return;
		for (const auto& [id, slot] : state->slots) { (void)id; slots.emplace_back(slot); }
	}
	for (const auto& slot : slots) if (slot->active.load()) try { slot->callback(batch); } catch (...) { }
}

} // namespace

std::shared_ptr<IAtomicFileStorageFileOperations> CreateWin32AtomicFileStorageFileOperations()
{
	return std::make_shared<Win32FileOperations>();
}

CAtomicFileStorageService::CAtomicFileStorageService(std::filesystem::path directory, std::uint64_t generation,
	std::size_t maxCompletedOperations, std::shared_ptr<IAtomicFileStorageFileOperations> fileOperations)
	: m_directory(std::move(directory))
	, m_generation(generation == 0 ? 1 : generation)
	, m_maxCompletedOperations((std::min)(kMaximumCompletedOperations, (std::max)(std::size_t{ 1 }, maxCompletedOperations)))
	, m_fileOperations(fileOperations ? std::move(fileOperations) : CreateWin32AtomicFileStorageFileOperations())
	, m_subscriptionState(std::make_shared<SubscriptionState>())
{
}

std::shared_ptr<IStorageAuthority> CreateAtomicFileStorageAuthority(
	const std::filesystem::path& directory, std::uint64_t generation, std::size_t maxCompletedOperations)
{
	return std::make_shared<CAtomicFileStorageService>(directory, generation, maxCompletedOperations);
}

CAtomicFileStorageService::~CAtomicFileStorageService()
{
	Close();
	std::scoped_lock lock(m_subscriptionState->mutex);
	m_subscriptionState->closed = true;
	m_subscriptionState->slots.clear();
}

std::filesystem::path CAtomicFileStorageService::StatePath() const { return m_directory / L"storage-v1.bin"; }

AtomicFileStorageOpenResult CAtomicFileStorageService::Open()
{
	std::scoped_lock lock(m_mutex);
	if (m_open) return { EAtomicFileStorageOpenStatus::AlreadyOpen, {} };
	if (m_directory.empty() || !m_fileOperations) return { EAtomicFileStorageOpenStatus::InvalidArgument, "storage directory and file operations are required" };
	std::string diagnostic;
	if (!m_fileOperations->PrepareDirectory(m_directory, diagnostic)) return { EAtomicFileStorageOpenStatus::IoError, std::move(diagnostic) };
	auto writerLock = m_fileOperations->AcquireWriterLock(m_directory / L"storage-v1.lock");
	if (writerLock.status != EAtomicFileStorageWriterLockStatus::Acquired || !writerLock.lock) {
		return { writerLock.status == EAtomicFileStorageWriterLockStatus::Busy ? EAtomicFileStorageOpenStatus::WriterBusy : EAtomicFileStorageOpenStatus::IoError,
			std::move(writerLock.diagnostic) };
	}
	m_writerLock = std::move(writerLock.lock);
	std::vector<std::uint8_t> bytes;
	bool found = false;
	if (!m_fileOperations->ReadFile(StatePath(), bytes, found, diagnostic)) {
		m_writerLock.reset();
		return { diagnostic == "storage state exceeds maximum size" ? EAtomicFileStorageOpenStatus::CorruptData : EAtomicFileStorageOpenStatus::IoError, std::move(diagnostic) };
	}
	if (found) {
		auto decoded = Decode(bytes);
		if (!decoded.Succeeded()) { m_writerLock.reset(); return decoded; }
	}
	m_open = true;
	return { EAtomicFileStorageOpenStatus::Opened, {} };
}

void CAtomicFileStorageService::Close() noexcept
{
	try {
		std::scoped_lock lock(m_mutex);
		m_open = false;
		m_writerLock.reset();
		m_revision = 0;
		m_entries.clear();
		m_completedOperations.clear();
		m_completedOperationOrder.clear();
	} catch (...) { }
}

bool CAtomicFileStorageService::IsOpen() const noexcept
{
	try { std::scoped_lock lock(m_mutex); return m_open; } catch (...) { return false; }
}

StorageMutationResult CAtomicFileStorageService::ValidateRequest(const StorageMutationRequest& request) const
{
	if (request.operationId.empty() || request.operationId.size() > 256 || !IsBoundedString(request.operationId, false)) return { EStorageMutationStatus::Failed, m_revision, false, "storage operationId is invalid" };
	if (request.mutations.empty() || request.mutations.size() > kMaximumItems) return { request.mutations.empty() ? EStorageMutationStatus::NotApplicable : EStorageMutationStatus::Failed, m_revision, false, "storage mutation batch has invalid item count" };
	std::set<StorageAddress> addresses;
	std::size_t requestBytes = sizeof(std::uint32_t) + request.operationId.size() + sizeof(std::uint8_t)
		+ (request.expectedRevision ? sizeof(std::uint64_t) : 0) + sizeof(std::uint32_t);
	for (const auto& mutation : request.mutations) {
		if (!mutation.address.IsValid() || IsSecretOwner(mutation.address.owner) || !IsBoundedString(mutation.address.scopeId) ||
			!IsBoundedString(mutation.address.owner, false) || !IsBoundedString(mutation.address.key, false) ||
			!IsTarget(static_cast<std::uint8_t>(mutation.target)) || (mutation.value && !IsBoundedString(*mutation.value))) return { EStorageMutationStatus::Failed, m_revision, false, "storage address, target, or value is invalid" };
		if (!addresses.emplace(mutation.address).second) return { EStorageMutationStatus::Failed, m_revision, false, "storage batch contains a duplicate address" };
		requestBytes += 15 + mutation.address.scopeId.size() + mutation.address.owner.size() + mutation.address.key.size()
			+ (mutation.value ? sizeof(std::uint32_t) + mutation.value->size() : 0);
	}
	if (requestBytes > kMaximumStorageMutationPayloadBytes) return { EStorageMutationStatus::Failed, m_revision, false, "storage mutation exceeds control IPC limit" };
	return { EStorageMutationStatus::Succeeded, m_revision };
}

bool CAtomicFileStorageService::AppendCompletedOperation(std::vector<std::uint8_t>& output,
	const CompletedOperation& operation)
{
	if (!AppendString(output, operation.request.operationId)) return false;
	Append<std::uint8_t>(output, operation.request.expectedRevision ? 1 : 0);
	if (operation.request.expectedRevision) Append<std::uint64_t>(output, *operation.request.expectedRevision);
	if (operation.request.mutations.size() > kMaximumItems) return false;
	Append<std::uint32_t>(output, static_cast<std::uint32_t>(operation.request.mutations.size()));
	for (const auto& mutation : operation.request.mutations) {
		if (!AppendMutation(output, mutation)) return false;
	}
	const auto& result = operation.result;
	if (!IsStatus(static_cast<std::uint8_t>(result.status)) || !AppendString(output, result.diagnostic)) return false;
	Append<std::uint8_t>(output, static_cast<std::uint8_t>(result.status));
	Append<std::uint64_t>(output, result.revision);
	Append<std::uint8_t>(output, result.changeBatch ? 1 : 0);
	return !result.changeBatch || AppendBatch(output, *result.changeBatch);
}

bool CAtomicFileStorageService::RememberCompleted(std::map<std::string, CompletedOperation>& completed,
	std::deque<std::string>& order, const StorageMutationRequest& request, const StorageMutationResult& result,
	std::string& diagnostic) const
{
	if (request.operationId.empty() || completed.contains(request.operationId)) return true;
	completed.emplace(request.operationId, CompletedOperation{ request, result });
	order.emplace_back(request.operationId);

	std::map<std::string, std::size_t> encodedSizes;
	std::size_t encodedBytes = 0;
	for (const auto& operationId : order) {
		const auto found = completed.find(operationId);
		if (found == completed.end()) {
			diagnostic = "storage completed-operation order is corrupt";
			return false;
		}
		std::vector<std::uint8_t> encoded;
		if (!AppendCompletedOperation(encoded, found->second) || encoded.size() > kMaximumCompletedPayloadBytes
			|| encodedBytes > (std::numeric_limits<std::size_t>::max)() - encoded.size()) {
			diagnostic = "storage completed operation exceeds the durable replay budget";
			return false;
		}
		encodedBytes += encoded.size();
		encodedSizes.emplace(operationId, encoded.size());
	}
	while (order.size() > m_maxCompletedOperations || encodedBytes > kMaximumCompletedPayloadBytes) {
		const auto oldest = order.front();
		const auto size = encodedSizes.find(oldest);
		if (size == encodedSizes.end() || size->second > encodedBytes) {
			diagnostic = "storage completed-operation accounting is corrupt";
			return false;
		}
		encodedBytes -= size->second;
		encodedSizes.erase(size);
		completed.erase(oldest);
		order.pop_front();
	}
	if (!completed.contains(request.operationId)) {
		diagnostic = "storage operation cannot fit the durable replay budget";
		return false;
	}
	return true;
}

bool CAtomicFileStorageService::PersistCandidate(const std::map<StorageAddress, StorageEntry>& entries,
	const std::map<std::string, CompletedOperation>& completed, const std::deque<std::string>& completedOrder,
	std::uint64_t revision, std::string& diagnostic) const
{
	if (entries.size() > kMaximumItems || completed.size() != completedOrder.size()) { diagnostic = "storage candidate exceeds durable limits"; return false; }
	std::vector<std::uint8_t> payload;
	payload.reserve(1024);
	std::size_t snapshotBytes = kSnapshotFixedBytes;
	for (const auto& [address, entry] : entries) {
		(void)address;
		snapshotBytes += kSnapshotEntryFixedBytes + entry.address.scopeId.size() + entry.address.owner.size() + entry.address.key.size() + entry.value.size();
		if (snapshotBytes > kMaximumStorageSnapshotPayloadBytes || !AppendEntry(payload, entry)) { diagnostic = "storage state exceeds control IPC snapshot limit"; return false; }
	}
	for (const auto& operationId : completedOrder) {
		const auto found = completed.find(operationId);
		if (found == completed.end()) { diagnostic = "storage completed-operation order is corrupt"; return false; }
		if (!AppendCompletedOperation(payload, found->second)) {
			diagnostic = "cannot encode storage completed operation";
			return false;
		}
	}
	if (payload.size() > kMaximumPersistedBytes - 64) { diagnostic = "storage state exceeds durable size limit"; return false; }
	std::vector<std::uint8_t> checksumInput;
	Append<std::uint32_t>(checksumInput, kFormatVersion);
	Append<std::uint64_t>(checksumInput, m_generation);
	Append<std::uint64_t>(checksumInput, revision);
	Append<std::uint32_t>(checksumInput, static_cast<std::uint32_t>(entries.size()));
	Append<std::uint32_t>(checksumInput, static_cast<std::uint32_t>(completedOrder.size()));
	Append<std::uint32_t>(checksumInput, static_cast<std::uint32_t>(payload.size()));
	checksumInput.insert(checksumInput.end(), payload.begin(), payload.end());
	std::vector<std::uint8_t> bytes(kMagic.begin(), kMagic.end());
	bytes.insert(bytes.end(), checksumInput.begin(), checksumInput.begin() + 32);
	Append<std::uint64_t>(bytes, Checksum(checksumInput));
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	return m_fileOperations->WriteFileAtomically(StatePath(), bytes, diagnostic);
}

AtomicFileStorageOpenResult CAtomicFileStorageService::Decode(std::span<const std::uint8_t> bytes)
{
	if (bytes.size() < 48 || bytes.size() > kMaximumPersistedBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return { EAtomicFileStorageOpenStatus::CorruptData, "storage header is invalid" };
	std::size_t offset = kMagic.size();
	std::uint32_t version = 0, entryCount = 0, completedCount = 0, payloadLength = 0;
	std::uint64_t storedGeneration = 0, revision = 0, checksum = 0;
	if (!Read(bytes, offset, version) || !Read(bytes, offset, storedGeneration) || !Read(bytes, offset, revision) ||
		!Read(bytes, offset, entryCount) || !Read(bytes, offset, completedCount) || !Read(bytes, offset, payloadLength) || !Read(bytes, offset, checksum)) return { EAtomicFileStorageOpenStatus::CorruptData, "storage header is truncated" };
	if (version != kFormatVersion) return { EAtomicFileStorageOpenStatus::UnsupportedFormat, "storage format version is unsupported" };
	if (storedGeneration == 0 || storedGeneration > m_generation || entryCount > kMaximumItems
		|| completedCount > m_maxCompletedOperations || payloadLength != bytes.size() - offset) {
		return { EAtomicFileStorageOpenStatus::CorruptData, "storage header has invalid bounds" };
	}
	std::vector<std::uint8_t> checksumInput;
	checksumInput.insert(checksumInput.end(), bytes.begin() + kMagic.size(), bytes.begin() + kMagic.size() + 32);
	checksumInput.insert(checksumInput.end(), bytes.begin() + offset, bytes.end());
	if (Checksum(checksumInput) != checksum) return { EAtomicFileStorageOpenStatus::CorruptData, "storage checksum mismatch" };
	std::map<StorageAddress, StorageEntry> entries;
	for (std::uint32_t i = 0; i < entryCount; ++i) { StorageEntry entry; if (!ReadEntry(bytes, offset, entry) || entry.revision > revision || !entries.emplace(entry.address, std::move(entry)).second) return { EAtomicFileStorageOpenStatus::CorruptData, "storage entry is invalid" }; }
	std::map<std::string, CompletedOperation> completed;
	std::deque<std::string> completedOrder;
	for (std::uint32_t i = 0; i < completedCount; ++i) {
		CompletedOperation operation;
		std::uint8_t hasExpected = 0, status = 0, hasBatch = 0;
		std::uint32_t mutationCount = 0;
		if (!ReadString(bytes, offset, operation.request.operationId) || operation.request.operationId.empty() || operation.request.operationId.size() > 256 ||
			!Read(bytes, offset, hasExpected) || hasExpected > 1) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation is invalid" };
		if (hasExpected) { std::uint64_t expected = 0; if (!Read(bytes, offset, expected)) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation is truncated" }; operation.request.expectedRevision = expected; }
		if (!Read(bytes, offset, mutationCount) || mutationCount > kMaximumItems) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation mutation count is invalid" };
		operation.request.mutations.reserve(mutationCount);
		for (std::uint32_t index = 0; index < mutationCount; ++index) { StorageMutation mutation; if (!ReadMutation(bytes, offset, mutation)) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation mutation is invalid" }; operation.request.mutations.emplace_back(std::move(mutation)); }
		if (ValidateRequest(operation.request).status == EStorageMutationStatus::Failed) return { EAtomicFileStorageOpenStatus::CorruptData, "storage completed request violates storage limits" };
		if (!ReadString(bytes, offset, operation.result.diagnostic) || !Read(bytes, offset, status) || !IsStatus(status) || !Read(bytes, offset, operation.result.revision) || operation.result.revision > revision || !Read(bytes, offset, hasBatch) || hasBatch > 1 || (hasBatch != (status == static_cast<std::uint8_t>(EStorageMutationStatus::Succeeded)))) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation result is invalid" };
		operation.result.status = static_cast<EStorageMutationStatus>(status);
		if (hasBatch) {
			StorageChangeBatch batch;
			if (!ReadBatch(bytes, offset, batch) || !FitsControlWire(batch) || batch.generation != storedGeneration
				|| batch.revision != operation.result.revision || batch.baseRevision == std::numeric_limits<std::uint64_t>::max()
				|| batch.baseRevision + 1 != batch.revision || batch.changes.empty()) {
				return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation change batch is invalid" };
			}
			std::set<StorageAddress> changedAddresses;
			for (const auto& change : batch.changes) {
				if (!changedAddresses.emplace(change.address).second
					|| (change.entry && change.entry->revision != batch.revision)) {
					return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation change batch is invalid" };
				}
			}
			// Replayed operations must describe the currently authoritative service generation.
			batch.generation = m_generation;
			operation.result.changeBatch = std::move(batch);
		}
		if (!completed.emplace(operation.request.operationId, operation).second) return { EAtomicFileStorageOpenStatus::CorruptData, "storage operation ID is duplicated" };
		completedOrder.emplace_back(operation.request.operationId);
	}
	if (offset != bytes.size()) return { EAtomicFileStorageOpenStatus::CorruptData, "storage payload has trailing data" };
	m_revision = revision;
	m_entries = std::move(entries);
	m_completedOperations = std::move(completed);
	m_completedOperationOrder = std::move(completedOrder);
	return { EAtomicFileStorageOpenStatus::Opened, {} };
}

StorageMutationResult CAtomicFileStorageService::Apply(const StorageMutationRequest& request)
{
	std::unique_lock lock(m_mutex);
	if (!m_open) return { EStorageMutationStatus::Failed, m_revision, false, "durable storage is not open" };
	if (const auto completed = m_completedOperations.find(request.operationId); completed != m_completedOperations.end()) {
		if (!(completed->second.request == request)) return { EStorageMutationStatus::Failed, m_revision, false, "storage operationId was reused with a different request" };
		auto replay = completed->second.result;
		replay.replayed = true;
		return replay;
	}
	auto validation = ValidateRequest(request);
	if (validation.status != EStorageMutationStatus::Succeeded) {
		if (validation.status == EStorageMutationStatus::NotApplicable) {
			auto completed = m_completedOperations;
			auto order = m_completedOperationOrder;
			std::string diagnostic;
			if (!RememberCompleted(completed, order, request, validation, diagnostic)) {
				return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
			}
			if (!PersistCandidate(m_entries, completed, order, m_revision, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
			m_completedOperations = std::move(completed);
			m_completedOperationOrder = std::move(order);
		}
		return validation;
	}
	if (request.expectedRevision && *request.expectedRevision != m_revision) {
		StorageMutationResult conflict{ EStorageMutationStatus::Conflict, m_revision, false, "storage revision conflict" };
		auto completed = m_completedOperations; auto order = m_completedOperationOrder;
		std::string diagnostic;
		if (!RememberCompleted(completed, order, request, conflict, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		if (!PersistCandidate(m_entries, completed, order, m_revision, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		m_completedOperations = std::move(completed); m_completedOperationOrder = std::move(order);
		return conflict;
	}
	std::vector<StorageChange> changes;
	for (const auto& mutation : request.mutations) {
		const auto existing = m_entries.find(mutation.address);
		if (!mutation.value) { if (existing != m_entries.end()) changes.push_back({ mutation.address, existing->second.target, std::nullopt }); continue; }
		if (existing != m_entries.end() && existing->second.target == mutation.target && existing->second.value == *mutation.value) continue;
		changes.push_back({ mutation.address, mutation.target, StorageEntry{ mutation.address, mutation.target, *mutation.value, 0 } });
	}
	if (changes.empty()) {
		StorageMutationResult noChange{ EStorageMutationStatus::NotApplicable, m_revision, false, "storage mutation does not change state" };
		auto completed = m_completedOperations; auto order = m_completedOperationOrder;
		std::string diagnostic;
		if (!RememberCompleted(completed, order, request, noChange, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		if (!PersistCandidate(m_entries, completed, order, m_revision, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		m_completedOperations = std::move(completed); m_completedOperationOrder = std::move(order);
		return noChange;
	}
	if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
		StorageMutationResult exhausted{ EStorageMutationStatus::Failed, m_revision, false,
			"storage revision cannot advance without losing monotonicity" };
		auto completed = m_completedOperations;
		auto order = m_completedOperationOrder;
		std::string diagnostic;
		if (!RememberCompleted(completed, order, request, exhausted, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		if (!PersistCandidate(m_entries, completed, order, m_revision, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
		m_completedOperations = std::move(completed);
		m_completedOperationOrder = std::move(order);
		return exhausted;
	}
	const std::uint64_t nextRevision = m_revision + 1;
	auto entries = m_entries;
	for (auto& change : changes) {
		if (!change.entry) entries.erase(change.address);
		else { change.entry->revision = nextRevision; entries.insert_or_assign(change.address, *change.entry); }
	}
	StorageChangeBatch batch{ m_generation, m_revision, nextRevision, std::move(changes) };
	if (!FitsControlWire(batch)) return { EStorageMutationStatus::Failed, m_revision, false, "storage change batch exceeds control IPC limit" };
	StorageMutationResult success{ EStorageMutationStatus::Succeeded, nextRevision, false, {}, batch };
	auto completed = m_completedOperations; auto order = m_completedOperationOrder;
	std::string diagnostic;
	if (!RememberCompleted(completed, order, request, success, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
	if (!PersistCandidate(entries, completed, order, nextRevision, diagnostic)) return { EStorageMutationStatus::Failed, m_revision, false, std::move(diagnostic) };
	m_entries = std::move(entries); m_completedOperations = std::move(completed); m_completedOperationOrder = std::move(order); m_revision = nextRevision;
	const bool startDrain = EnqueueNotification(*success.changeBatch);
	lock.unlock();
	if (startDrain) DrainNotifications();
	return success;
}

StorageSnapshot CAtomicFileStorageService::Snapshot() const
{
	std::scoped_lock lock(m_mutex);
	StorageSnapshot snapshot{ m_generation, m_revision, {} };
	if (!m_open) {
		snapshot.revision = 0;
		return snapshot;
	}
	snapshot.entries.reserve(m_entries.size());
	for (const auto& [address, entry] : m_entries) { (void)address; snapshot.entries.emplace_back(entry); }
	return snapshot;
}

std::optional<StorageEntry> CAtomicFileStorageService::Find(const StorageAddress& address) const
{
	std::scoped_lock lock(m_mutex);
	if (!m_open) return std::nullopt;
	const auto found = m_entries.find(address);
	return found == m_entries.end() ? std::nullopt : std::optional<StorageEntry>{ found->second };
}

std::unique_ptr<IStorageChangeSubscription> CAtomicFileStorageService::Subscribe(StorageChangeCallback callback)
{
	if (!callback) return nullptr;
	std::scoped_lock lock(m_subscriptionState->mutex);
	if (m_subscriptionState->closed) return nullptr;
	if (m_subscriptionState->nextId == 0) return nullptr;
	const std::uint64_t id = m_subscriptionState->nextId++;
	if (!m_subscriptionState->slots.emplace(id, std::make_shared<SubscriptionSlot>(std::move(callback))).second) return nullptr;
	return std::make_unique<Subscription>(m_subscriptionState, id);
}

bool CAtomicFileStorageService::EnqueueNotification(const StorageChangeBatch& batch)
{
	std::scoped_lock lock(m_notificationMutex);
	m_notificationQueue.push_back(batch);
	if (m_dispatchingNotifications) return false;
	m_dispatchingNotifications = true;
	return true;
}

void CAtomicFileStorageService::DrainNotifications()
{
	for (;;) {
		StorageChangeBatch batch;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) { m_dispatchingNotifications = false; return; }
			batch = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		Deliver(m_subscriptionState, batch);
	}
}

} // namespace platform::storage
