/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostSharedState.h"

#include <Aclapi.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSharedStateMagic = 0x58454853; // SHEX
constexpr std::uint32_t kSharedStateVersion = 1;
constexpr std::size_t kIdentityCharacters = 33;
constexpr std::size_t kPipeCharacters = 160;
constexpr std::size_t kDiagnosticBytes = 512;

class UniqueHandle final {
public:
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
	~UniqueHandle() { if (m_value) ::CloseHandle(m_value); }
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	HANDLE Get() const noexcept { return m_value; }
	HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
private:
	HANDLE m_value = nullptr;
};

class CurrentUserSecurity final {
public:
	~CurrentUserSecurity() { if (m_acl) ::LocalFree(m_acl); }

	bool Initialize(std::wstring& diagnostic)
	{
		HANDLE rawToken = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
			diagnostic = L"OpenProcessToken failed (error " + std::to_wstring(::GetLastError()) + L")";
			return false;
		}
		UniqueHandle token(rawToken);
		DWORD bytes = 0;
		::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
		if (bytes == 0) {
			diagnostic = L"GetTokenInformation size failed (error " + std::to_wstring(::GetLastError()) + L")";
			return false;
		}
		std::vector<std::uint8_t> storage(bytes);
		if (!::GetTokenInformation(token.Get(), TokenUser, storage.data(), bytes, &bytes)) {
			diagnostic = L"GetTokenInformation failed (error " + std::to_wstring(::GetLastError()) + L")";
			return false;
		}

		auto* user = reinterpret_cast<TOKEN_USER*>(storage.data());
		EXPLICIT_ACCESSW access{};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = static_cast<wchar_t*>(user->User.Sid);
		const DWORD result = ::SetEntriesInAclW(1, &access, nullptr, &m_acl);
		if (result != ERROR_SUCCESS) {
			diagnostic = L"SetEntriesInAcl failed (error " + std::to_wstring(result) + L")";
			return false;
		}
		if (!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
			!::SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE)) {
			diagnostic = L"Initialize shared-state security descriptor failed (error " +
				std::to_wstring(::GetLastError()) + L")";
			return false;
		}
		m_attributes = { sizeof(m_attributes), &m_descriptor, FALSE };
		return true;
	}

	SECURITY_ATTRIBUTES* Attributes() noexcept { return &m_attributes; }

private:
	PACL m_acl = nullptr;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
};

template <class Character, std::size_t Count>
void CopyBounded(std::array<Character, Count>& destination, std::basic_string_view<Character> source) noexcept
{
	const auto count = (std::min)(source.size(), Count - 1);
	std::copy_n(source.data(), count, destination.data());
	destination[count] = Character{};
}

std::wstring MappingNameFor(const std::filesystem::path& profileDirectory)
{
	const auto hash = CExtensionHostBroker::ComputeProfileHash(profileDirectory);
	return hash.empty() ? std::wstring{} : L"Local\\SakuraExtensionHostState-" + hash;
}

} // namespace

struct CExtensionHostSharedState::SharedBlock {
	struct Payload {
		std::uint32_t magic = 0;
		std::uint32_t version = 0;
		std::uint32_t state = 0;
		std::uint32_t hostProcessId = 0;
		std::uint32_t retryCount = 0;
		std::uint32_t leaseOwnerCount = 0;
		std::uint32_t leaseCount = 0;
		std::uint64_t generation = 0;
		std::array<wchar_t, kIdentityCharacters> profileHash{};
		std::array<wchar_t, kIdentityCharacters> bootId{};
		std::array<wchar_t, kPipeCharacters> pipeName{};
		std::array<char, kDiagnosticBytes> diagnostic{};
	};

	alignas(4) volatile LONG sequence = 0;
	Payload payload{};
};

CExtensionHostSharedState::~CExtensionHostSharedState()
{
	Close();
}

bool CExtensionHostSharedState::CreateForBroker(
	const std::filesystem::path& profileDirectory,
	std::wstring& diagnostic)
{
	Close();
	m_mappingName = MappingNameFor(profileDirectory);
	if (m_mappingName.empty()) {
		diagnostic = L"Cannot compute extension host shared-state identity";
		return false;
	}
	CurrentUserSecurity security;
	if (!security.Initialize(diagnostic)) return false;
	UniqueHandle mapping(::CreateFileMappingW(
		INVALID_HANDLE_VALUE, security.Attributes(), PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(SharedBlock)), m_mappingName.c_str()));
	if (!mapping.Get()) {
		diagnostic = L"Create extension host shared state failed (error " +
			std::to_wstring(::GetLastError()) + L")";
		return false;
	}
	if (::GetLastError() == ERROR_ALREADY_EXISTS) {
		diagnostic = L"Extension host shared state is already owned";
		return false;
	}
	auto* block = static_cast<SharedBlock*>(::MapViewOfFile(
		mapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedBlock)));
	if (!block) {
		diagnostic = L"Map extension host shared state failed (error " +
			std::to_wstring(::GetLastError()) + L")";
		return false;
	}
	m_mapping = mapping.Release();
	m_block = block;
	m_writer = true;
	std::memset(m_block, 0, sizeof(*m_block));
	diagnostic.clear();
	return true;
}

bool CExtensionHostSharedState::OpenForEditor(
	const std::filesystem::path& profileDirectory,
	std::wstring& diagnostic)
{
	Close();
	m_mappingName = MappingNameFor(profileDirectory);
	if (m_mappingName.empty()) {
		diagnostic = L"Cannot compute extension host shared-state identity";
		return false;
	}
	UniqueHandle mapping(::OpenFileMappingW(FILE_MAP_READ, FALSE, m_mappingName.c_str()));
	if (!mapping.Get()) {
		diagnostic = L"Open extension host shared state failed (error " +
			std::to_wstring(::GetLastError()) + L")";
		return false;
	}
	auto* block = static_cast<SharedBlock*>(::MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, sizeof(SharedBlock)));
	if (!block) {
		diagnostic = L"Map extension host shared state failed (error " +
			std::to_wstring(::GetLastError()) + L")";
		return false;
	}
	m_mapping = mapping.Release();
	m_block = block;
	m_writer = false;
	diagnostic.clear();
	return true;
}

void CExtensionHostSharedState::Close() noexcept
{
	if (m_block) ::UnmapViewOfFile(m_block);
	if (m_mapping) ::CloseHandle(m_mapping);
	m_block = nullptr;
	m_mapping = nullptr;
	m_writer = false;
	m_mappingName.clear();
}

void CExtensionHostSharedState::Publish(const SExtensionHostBrokerSnapshot& snapshot) noexcept
{
	if (!m_writer || !m_block) return;
	SharedBlock::Payload payload{};
	payload.magic = kSharedStateMagic;
	payload.version = kSharedStateVersion;
	payload.state = static_cast<std::uint32_t>(snapshot.state);
	payload.hostProcessId = snapshot.hostProcessId;
	payload.retryCount = snapshot.retryCount;
	payload.leaseOwnerCount = static_cast<std::uint32_t>((std::min)(
		snapshot.leaseOwnerCount, static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
	payload.leaseCount = static_cast<std::uint32_t>((std::min)(
		snapshot.leaseCount, static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
	payload.generation = snapshot.generation;
	CopyBounded(payload.profileHash, std::wstring_view(snapshot.profileHash));
	CopyBounded(payload.bootId, std::wstring_view(snapshot.bootId));
	CopyBounded(payload.pipeName, std::wstring_view(snapshot.pipeName));
	CopyBounded(payload.diagnostic, std::string_view(snapshot.lastDiagnostic));

	::InterlockedIncrement(&m_block->sequence);
	::MemoryBarrier();
	m_block->payload = payload;
	::MemoryBarrier();
	::InterlockedIncrement(&m_block->sequence);
}

std::optional<SExtensionHostBrokerSnapshot> CExtensionHostSharedState::Read() const noexcept
{
	if (!m_block) return std::nullopt;
	for (int attempt = 0; attempt < 16; ++attempt) {
		const LONG before = m_block->sequence;
		if ((before & 1) != 0) {
			::SwitchToThread();
			continue;
		}
		::MemoryBarrier();
		const auto payload = m_block->payload;
		::MemoryBarrier();
		const LONG after = m_block->sequence;
		if (before != after || (after & 1) != 0) continue;
		if (payload.magic != kSharedStateMagic || payload.version != kSharedStateVersion ||
			payload.state > static_cast<std::uint32_t>(EExtensionHostState::Stopped)) {
			return std::nullopt;
		}
		return SExtensionHostBrokerSnapshot{
			static_cast<EExtensionHostState>(payload.state), payload.generation, payload.hostProcessId,
			payload.retryCount, payload.leaseOwnerCount, payload.leaseCount,
			payload.profileHash.data(), payload.bootId.data(), payload.pipeName.data(), payload.diagnostic.data(),
		};
	}
	return std::nullopt;
}
