/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformEndpoint.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr std::uint32_t kEndpointMagic = 0x50494353; // SCIP
//! Bump whenever the fixed shared-memory payload changes. Readers never
//! interpret a prior payload as this ABI.
constexpr std::uint32_t kEndpointVersion = 2;
constexpr std::size_t kProfileHashCharacters = 64;
constexpr std::size_t kPipeNameCharacters = 128;
constexpr std::wstring_view kControlPipePrefix = L"\\\\.\\pipe\\SakuraControl-";

class UniqueHandle final {
public:
	explicit UniqueHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~UniqueHandle()
	{
		if (m_value && m_value != INVALID_HANDLE_VALUE) {
			::CloseHandle(m_value);
		}
	}
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }

private:
	HANDLE m_value = nullptr;
};

template <std::size_t Count>
bool CopyExact(std::array<wchar_t, Count>& destination, std::wstring_view source) noexcept
{
	if (source.empty() || source.size() >= Count) {
		return false;
	}
	std::copy(source.begin(), source.end(), destination.begin());
	destination[source.size()] = L'\0';
	return true;
}

template <std::size_t Count>
std::optional<std::wstring> ReadExact(const std::array<wchar_t, Count>& source)
{
	const auto terminator = std::find(source.begin(), source.end(), L'\0');
	if (terminator == source.end() || terminator == source.begin() ||
		!std::all_of(std::next(terminator), source.end(), [](wchar_t character) { return character == L'\0'; })) {
		return std::nullopt;
	}
	return std::wstring(source.begin(), terminator);
}

template <std::size_t Count>
bool CopyExact(std::array<char, Count>& destination, std::string_view source) noexcept
{
	if (source.empty() || source.size() >= Count) {
		return false;
	}
	std::copy(source.begin(), source.end(), destination.begin());
	destination[source.size()] = '\0';
	return true;
}

template <std::size_t Count>
std::optional<std::string> ReadExact(const std::array<char, Count>& source)
{
	const auto terminator = std::find(source.begin(), source.end(), '\0');
	if (terminator == source.end() || terminator == source.begin() ||
		!std::all_of(std::next(terminator), source.end(), [](char character) { return character == '\0'; })) {
		return std::nullopt;
	}
	return std::string(source.begin(), terminator);
}

bool IsKnownLifecycle(ControlPlatformEndpointLifecycle lifecycle) noexcept
{
	return lifecycle == ControlPlatformEndpointLifecycle::Starting ||
		lifecycle == ControlPlatformEndpointLifecycle::Accepting ||
		lifecycle == ControlPlatformEndpointLifecycle::Stopping ||
		lifecycle == ControlPlatformEndpointLifecycle::Stopped;
}

bool IsLiveProcess(std::uint32_t processId) noexcept
{
	if (processId == 0) {
		return false;
	}
	UniqueHandle process(::OpenProcess(SYNCHRONIZE, FALSE, processId));
	if (!process.Get()) {
		return false;
	}
	return ::WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT;
}

bool HasExpectedEndpointIdentity(
	const ControlPlatformEndpointSnapshot& snapshot,
	std::wstring_view expectedProfileHash) noexcept
{
	return !expectedProfileHash.empty() && snapshot.profileHash == expectedProfileHash &&
		profiles::IsCanonicalProfileAuthorityId(snapshot.profileId) && snapshot.controlProcessId != 0 &&
		snapshot.generation != 0 && IsKnownLifecycle(snapshot.lifecycle) &&
		IsSafeControlPipeName(snapshot.pipeName) &&
		snapshot.pipeName.size() == kControlPipePrefix.size() + expectedProfileHash.size() &&
		snapshot.pipeName.starts_with(kControlPipePrefix) &&
		snapshot.pipeName.substr(kControlPipePrefix.size()) == expectedProfileHash;
}

} // namespace

struct CControlPlatformEndpoint::SharedBlock {
	struct Payload {
		std::uint32_t magic = 0;
		std::uint32_t version = 0;
		std::uint32_t controlProcessId = 0;
		std::uint32_t lifecycle = 0;
		std::uint64_t generation = 0;
		std::array<char, profiles::kCanonicalProfileAuthorityIdCharacters + 1> profileId{};
		std::array<wchar_t, kProfileHashCharacters + 1> profileHash{};
		std::array<wchar_t, kPipeNameCharacters> pipeName{};
	};

	alignas(4) volatile LONG sequence = 0;
	Payload payload{};
};

CControlPlatformEndpoint::~CControlPlatformEndpoint()
{
	Close();
}

bool CControlPlatformEndpoint::CreateForControl(const std::filesystem::path& profileDirectory, std::wstring& diagnostic)
{
	Close();
	m_profileHash = ComputeCanonicalProfileHash(profileDirectory);
	m_mappingName = BuildControlEndpointMappingName(m_profileHash);
	if (m_profileHash.empty() || m_mappingName.empty()) {
		diagnostic = L"Cannot derive control IPC endpoint identity from profile";
		Close();
		return false;
	}
	CurrentUserSecurityAttributes security;
	if (!security.Initialize(diagnostic)) {
		Close();
		return false;
	}
	static_assert(sizeof(SharedBlock) <= (std::numeric_limits<DWORD>::max)());
	UniqueHandle mapping(::CreateFileMappingW(
		INVALID_HANDLE_VALUE,
		security.Attributes(),
		PAGE_READWRITE,
		0,
		static_cast<DWORD>(sizeof(SharedBlock)),
		m_mappingName.c_str()));
	if (!mapping.Get()) {
		diagnostic = L"Create control IPC endpoint mapping failed (error " + std::to_wstring(::GetLastError()) + L")";
		Close();
		return false;
	}
	if (::GetLastError() == ERROR_ALREADY_EXISTS) {
		diagnostic = L"Control IPC endpoint mapping is already owned";
		Close();
		return false;
	}
	auto* block = static_cast<SharedBlock*>(::MapViewOfFile(
		mapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedBlock)));
	if (!block) {
		diagnostic = L"Map control IPC endpoint failed (error " + std::to_wstring(::GetLastError()) + L")";
		Close();
		return false;
	}
	m_mapping = mapping.Release();
	m_block = block;
	m_writer = true;
	std::memset(m_block, 0, sizeof(*m_block));
	diagnostic.clear();
	return true;
}

bool CControlPlatformEndpoint::OpenForEditor(const std::filesystem::path& profileDirectory, std::wstring& diagnostic)
{
	auto result = OpenForEditorDetailed(profileDirectory);
	diagnostic = std::move(result.diagnostic);
	return result.disposition == EControlPlatformEndpointDiscoveryDisposition::Discovered;
}

ControlPlatformEndpointDiscoveryResult CControlPlatformEndpoint::OpenForEditorDetailed(
	const std::filesystem::path& profileDirectory)
{
	Close();
	m_profileHash = ComputeCanonicalProfileHash(profileDirectory);
	m_mappingName = BuildControlEndpointMappingName(m_profileHash);
	if (m_profileHash.empty() || m_mappingName.empty()) {
		ControlPlatformEndpointDiscoveryResult result;
		result.disposition = EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor;
		result.diagnostic = L"Cannot derive control IPC endpoint identity from profile";
		Close();
		return result;
	}
	UniqueHandle mapping(::OpenFileMappingW(FILE_MAP_READ | READ_CONTROL, FALSE, m_mappingName.c_str()));
	if (!mapping.Get()) {
		const DWORD error = ::GetLastError();
		ControlPlatformEndpointDiscoveryResult result;
		result.disposition = error == ERROR_FILE_NOT_FOUND
			? EControlPlatformEndpointDiscoveryDisposition::NotPublished
			: error == ERROR_ACCESS_DENIED
				? EControlPlatformEndpointDiscoveryDisposition::AccessDenied
				: EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
		result.errorCode = error;
		result.diagnostic = L"Open control IPC endpoint mapping failed (error " + std::to_wstring(error) + L")";
		Close();
		return result;
	}
	std::wstring diagnostic;
	if (!VerifyCurrentUserOnlyDacl(mapping.Get(), diagnostic)) {
		ControlPlatformEndpointDiscoveryResult result;
		result.disposition = EControlPlatformEndpointDiscoveryDisposition::SecurityRejected;
		result.diagnostic = std::move(diagnostic);
		Close();
		return result;
	}
	auto* block = static_cast<SharedBlock*>(::MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, sizeof(SharedBlock)));
	if (!block) {
		const DWORD error = ::GetLastError();
		ControlPlatformEndpointDiscoveryResult result;
		result.disposition = error == ERROR_ACCESS_DENIED
			? EControlPlatformEndpointDiscoveryDisposition::AccessDenied
			: EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
		result.errorCode = error;
		result.diagnostic = L"Map control IPC endpoint failed (error " + std::to_wstring(error) + L")";
		Close();
		return result;
	}
	m_mapping = mapping.Release();
	m_block = block;
	m_writer = false;
	return { EControlPlatformEndpointDiscoveryDisposition::Discovered, std::nullopt, ERROR_SUCCESS, {} };
}

void CControlPlatformEndpoint::Close() noexcept
{
	std::unique_lock lock(m_publishMutex);
	if (m_block) {
		::UnmapViewOfFile(m_block);
	}
	if (m_mapping) {
		::CloseHandle(m_mapping);
	}
	m_mapping = nullptr;
	m_block = nullptr;
	m_writer = false;
	m_mappingName.clear();
	m_profileHash.clear();
}

bool CControlPlatformEndpoint::Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic)
{
	std::unique_lock lock(m_publishMutex);
	if (!m_writer || !m_block) {
		diagnostic = L"Control IPC endpoint is not owned by this process";
		return false;
	}
	if (snapshot.controlProcessId != ::GetCurrentProcessId() ||
		!HasExpectedEndpointIdentity(snapshot, m_profileHash)) {
		diagnostic = L"Control IPC endpoint snapshot is invalid";
		return false;
	}
	SharedBlock::Payload payload{};
	payload.magic = kEndpointMagic;
	payload.version = kEndpointVersion;
	payload.controlProcessId = snapshot.controlProcessId;
	payload.lifecycle = static_cast<std::uint32_t>(snapshot.lifecycle);
	payload.generation = snapshot.generation;
	if (!CopyExact(payload.profileId, snapshot.profileId) ||
		!CopyExact(payload.profileHash, snapshot.profileHash) || !CopyExact(payload.pipeName, snapshot.pipeName)) {
		diagnostic = L"Control IPC endpoint snapshot exceeds ABI bounds";
		return false;
	}
	::InterlockedIncrement(&m_block->sequence);
	::MemoryBarrier();
	m_block->payload = payload;
	::MemoryBarrier();
	::InterlockedIncrement(&m_block->sequence);
	diagnostic.clear();
	return true;
}

std::optional<ControlPlatformEndpointSnapshot> CControlPlatformEndpoint::Read(
	const ControlPlatformEndpointReadRequirements& requirements) const
{
	auto result = ReadDetailed(requirements);
	return std::move(result.snapshot);
}

ControlPlatformEndpointDiscoveryResult CControlPlatformEndpoint::ReadDetailed(
	const ControlPlatformEndpointReadRequirements& requirements) const
{
	std::shared_lock lock(m_publishMutex);
	if (!m_block) {
		return { EControlPlatformEndpointDiscoveryDisposition::Closed, std::nullopt, ERROR_INVALID_HANDLE,
			L"Control IPC endpoint mapping is closed" };
	}
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
		if (before != after || (after & 1) != 0) {
			continue;
		}
		if (payload.magic != kEndpointMagic || payload.version != kEndpointVersion) {
			return { payload.magic == 0 && payload.version == 0
				? EControlPlatformEndpointDiscoveryDisposition::NotPublished
				: EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi,
				std::nullopt, ERROR_INVALID_DATA, L"Control IPC endpoint ABI is not published or unsupported" };
		}
		const auto profileHash = ReadExact(payload.profileHash);
		const auto pipeName = ReadExact(payload.pipeName);
		const auto profileId = ReadExact(payload.profileId);
		if (!profileId || !profileHash || !pipeName) {
			return { EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi,
				std::nullopt, ERROR_INVALID_DATA, L"Control IPC endpoint ABI strings are malformed" };
		}
		ControlPlatformEndpointSnapshot snapshot{
			.controlProcessId = payload.controlProcessId,
			.generation = payload.generation,
			.lifecycle = static_cast<ControlPlatformEndpointLifecycle>(payload.lifecycle),
			.profileHash = std::move(*profileHash),
			.pipeName = std::move(*pipeName),
			.profileId = std::move(*profileId),
		};
		const auto disposition = ClassifySnapshot(snapshot, m_profileHash, requirements);
		if (disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
			return { disposition, std::nullopt, ERROR_INVALID_DATA, L"Control IPC endpoint snapshot is not usable" };
		}
		return { disposition, std::optional<ControlPlatformEndpointSnapshot>(std::move(snapshot)), ERROR_SUCCESS, {} };
	}
	return { EControlPlatformEndpointDiscoveryDisposition::Busy, std::nullopt, ERROR_RETRY,
		L"Control IPC endpoint publication remained busy" };
}

bool CControlPlatformEndpoint::IsSnapshotUsable(
	const ControlPlatformEndpointSnapshot& snapshot,
	std::wstring_view expectedProfileHash,
	const ControlPlatformEndpointReadRequirements& requirements) noexcept
{
	return ClassifySnapshot(snapshot, expectedProfileHash, requirements) ==
		EControlPlatformEndpointDiscoveryDisposition::Discovered;
}

EControlPlatformEndpointDiscoveryDisposition CControlPlatformEndpoint::ClassifySnapshot(
	const ControlPlatformEndpointSnapshot& snapshot,
	std::wstring_view expectedProfileHash,
	const ControlPlatformEndpointReadRequirements& requirements) noexcept
{
	if (!HasExpectedEndpointIdentity(snapshot, expectedProfileHash)) {
		return EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi;
	}
	if (snapshot.lifecycle != ControlPlatformEndpointLifecycle::Accepting) {
		return EControlPlatformEndpointDiscoveryDisposition::NotAccepting;
	}
	if (snapshot.generation < requirements.minimumGeneration ||
		(requirements.requireLiveControlProcess && !IsLiveProcess(snapshot.controlProcessId))) {
		return EControlPlatformEndpointDiscoveryDisposition::DeadOrStale;
	}
	return EControlPlatformEndpointDiscoveryDisposition::Discovered;
}

} // namespace platform::controlipc
