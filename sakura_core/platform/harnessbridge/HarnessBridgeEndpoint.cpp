/*! @file */
#include <sakura/harnessbridge/HarnessBridgeEndpoint.h>

#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <algorithm>
#include <cstring>

namespace platform::harnessbridge {
namespace {

constexpr std::size_t kProfileCapacity = 128;
constexpr std::size_t kPipeCapacity = 256;
constexpr std::uint32_t kSharedMagic = 0x44425048u; // HPBD

struct WireBlock final {
	std::uint32_t magic;
	std::uint16_t version;
	volatile LONG sequence;
	std::uint32_t lifecycle;
	std::uint64_t profileGeneration;
	std::uint64_t bridgeEpoch;
	std::uint64_t runtimeGeneration;
	std::uint32_t serverPid;
	std::uint64_t serverProcessCreationTime;
	std::uint16_t protocolMajor;
	std::uint16_t protocolMinor;
	std::uint8_t editorId[16];
	std::uint8_t bridgeId[16];
	wchar_t endpointHash[65];
	wchar_t profileId[kProfileCapacity];
	wchar_t pipeName[kPipeCapacity];
};

template<std::size_t N>
bool CopyString(wchar_t (&destination)[N], const std::wstring& value) noexcept
{
	if (value.empty() || value.size() >= N) return false;
	std::fill(std::begin(destination), std::end(destination), L'\0');
	std::copy(value.begin(), value.end(), destination);
	return true;
}

template<std::size_t N>
bool ReadString(const wchar_t (&source)[N], std::wstring& value) noexcept
{
	const auto* end = std::find(std::begin(source), std::end(source), L'\0');
	if (end == std::end(source)) return false;
	value.assign(source, end);
	return !value.empty();
}

bool IsLive(const std::uint32_t pid) noexcept
{
	if (!pid) return false;
	HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
	if (!process) return false;
	DWORD exitCode = STILL_ACTIVE;
	const bool ok = ::GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
	::CloseHandle(process);
	return ok;
}

bool MatchesCreationTime(const std::uint32_t pid, const std::uint64_t expected) noexcept
{
	if (!expected) return false;
	HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return false;
	FILETIME creation{}, exitTime{}, kernel{}, user{};
	const bool ok = ::GetProcessTimes(process, &creation, &exitTime, &kernel, &user) != FALSE;
	::CloseHandle(process);
	if (!ok) return false;
	ULARGE_INTEGER value{};
	value.LowPart = creation.dwLowDateTime;
	value.HighPart = creation.dwHighDateTime;
	return value.QuadPart == expected;
}

bool IsKnownLifecycle(const EHarnessBridgeLifecycle lifecycle) noexcept
{
	return lifecycle == EHarnessBridgeLifecycle::Starting || lifecycle == EHarnessBridgeLifecycle::Accepting
		|| lifecycle == EHarnessBridgeLifecycle::Stopping || lifecycle == EHarnessBridgeLifecycle::Stopped;
}

bool CopyDescriptor(const WireBlock& block, HarnessEditorEndpointDescriptor& descriptor) noexcept
{
	descriptor.descriptorVersion = block.version;
	descriptor.profileGeneration = block.profileGeneration;
	descriptor.bridgeEpoch = block.bridgeEpoch;
	descriptor.runtimeGeneration = block.runtimeGeneration;
	descriptor.lifecycle = static_cast<EHarnessBridgeLifecycle>(block.lifecycle);
	descriptor.serverPid = block.serverPid;
	descriptor.serverProcessCreationTime = block.serverProcessCreationTime;
	descriptor.protocolMajor = block.protocolMajor;
	descriptor.protocolMinor = block.protocolMinor;
	std::copy(std::begin(block.editorId), std::end(block.editorId), descriptor.editorId.begin());
	std::copy(std::begin(block.bridgeId), std::end(block.bridgeId), descriptor.bridgeId.begin());
	return ReadString(block.endpointHash, descriptor.endpointHash)
		&& ReadString(block.profileId, descriptor.profileId)
		&& ReadString(block.pipeName, descriptor.pipeName);
}

} // namespace

bool ValidateHarnessBridgeEndpointDescriptor(const HarnessEditorEndpointDescriptor& descriptor,
	const HarnessBridgeEndpointReadRequirements& requirements) noexcept
{
	if (descriptor.descriptorVersion != kHarnessBridgeEndpointDescriptorVersion
		|| descriptor.endpointHash.size() != 64
		|| !IsKnownLifecycle(descriptor.lifecycle)
		|| BuildHarnessPipeName(descriptor.endpointHash) != descriptor.pipeName
		|| !IsSafeHarnessPipeName(descriptor.pipeName)
		|| descriptor.profileId.empty() || descriptor.profileId.size() >= kProfileCapacity
		|| descriptor.profileId.find(L'\0') != std::wstring::npos
		|| descriptor.serverPid == 0 || descriptor.bridgeEpoch == 0 || descriptor.runtimeGeneration == 0
		|| descriptor.profileGeneration == 0 || descriptor.protocolMajor == 0) return false;
	if (std::all_of(descriptor.editorId.begin(), descriptor.editorId.end(), [](const auto b) { return b == 0; })
		|| std::all_of(descriptor.bridgeId.begin(), descriptor.bridgeId.end(), [](const auto b) { return b == 0; })) return false;
	if (!requirements.expectedEndpointHash.empty() && descriptor.endpointHash != requirements.expectedEndpointHash) return false;
	if (descriptor.bridgeEpoch < requirements.minimumBridgeEpoch) return false;
	return true;
}

CHarnessBridgeEndpointPublisher::~CHarnessBridgeEndpointPublisher()
{
	Close();
}

bool CHarnessBridgeEndpointPublisher::Create(const HarnessEditorEndpointDescriptor& descriptor,
	std::wstring& diagnostic)
{
	Close();
	HarnessBridgeEndpointReadRequirements requirements{ descriptor.endpointHash, 0, false };
	if (!ValidateHarnessBridgeEndpointDescriptor(descriptor, requirements)
		|| descriptor.lifecycle != EHarnessBridgeLifecycle::Starting) {
		diagnostic = L"Invalid Harness Bridge endpoint descriptor";
		return false;
	}
	const auto mappingName = BuildHarnessEndpointMappingName(descriptor.endpointHash);
	if (mappingName.empty()) { diagnostic = L"Invalid endpoint hash"; return false; }
	HarnessBridgeSecurityAttributes security;
	if (!security.Initialize(diagnostic)) return false;
	m_mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, security.Attributes(), PAGE_READWRITE,
		0, static_cast<DWORD>(sizeof(WireBlock)), mappingName.c_str());
	if (!m_mapping) { diagnostic = L"CreateFileMapping failed"; return false; }
	if (::GetLastError() == ERROR_ALREADY_EXISTS) {
		Close();
		diagnostic = L"Harness Bridge endpoint already exists";
		return false;
	}
	m_block = ::MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(WireBlock));
	if (!m_block) { Close(); diagnostic = L"MapViewOfFile failed"; return false; }
	std::memset(m_block, 0, sizeof(WireBlock));
	m_descriptor = descriptor;
	if (!Publish(EHarnessBridgeLifecycle::Starting, diagnostic)) { Close(); return false; }
	diagnostic.clear();
	return true;
}

bool CHarnessBridgeEndpointPublisher::Publish(const EHarnessBridgeLifecycle lifecycle, std::wstring& diagnostic)
{
	if (!m_block || !m_mapping) { diagnostic = L"Endpoint is closed"; return false; }
	if (lifecycle != EHarnessBridgeLifecycle::Starting && lifecycle != EHarnessBridgeLifecycle::Accepting
		&& lifecycle != EHarnessBridgeLifecycle::Stopping && lifecycle != EHarnessBridgeLifecycle::Stopped) {
		diagnostic = L"Invalid endpoint lifecycle";
		return false;
	}
	const auto block = static_cast<WireBlock*>(m_block);
	if (::InterlockedIncrement(&block->sequence) % 2 == 0) ::InterlockedIncrement(&block->sequence);
	block->magic = kSharedMagic;
	block->version = kHarnessBridgeEndpointDescriptorVersion;
	block->lifecycle = static_cast<std::uint32_t>(lifecycle);
	block->profileGeneration = m_descriptor.profileGeneration;
	block->bridgeEpoch = m_descriptor.bridgeEpoch;
	block->runtimeGeneration = m_descriptor.runtimeGeneration;
	block->serverPid = m_descriptor.serverPid;
	block->serverProcessCreationTime = m_descriptor.serverProcessCreationTime;
	block->protocolMajor = m_descriptor.protocolMajor;
	block->protocolMinor = m_descriptor.protocolMinor;
	std::copy(m_descriptor.editorId.begin(), m_descriptor.editorId.end(), std::begin(block->editorId));
	std::copy(m_descriptor.bridgeId.begin(), m_descriptor.bridgeId.end(), std::begin(block->bridgeId));
	if (!CopyString(block->endpointHash, m_descriptor.endpointHash)
		|| !CopyString(block->profileId, m_descriptor.profileId)
		|| !CopyString(block->pipeName, m_descriptor.pipeName)) {
		diagnostic = L"Endpoint descriptor string is too long";
		::InterlockedIncrement(&block->sequence);
		return false;
	}
	::MemoryBarrier();
	::InterlockedIncrement(&block->sequence);
	diagnostic.clear();
	return true;
}

void CHarnessBridgeEndpointPublisher::Close() noexcept
{
	if (m_block) ::UnmapViewOfFile(m_block);
	if (m_mapping) ::CloseHandle(m_mapping);
	m_block = nullptr;
	m_mapping = nullptr;
	m_descriptor = {};
}

CHarnessBridgeEndpointReader::~CHarnessBridgeEndpointReader()
{
	Close();
}

HarnessBridgeEndpointReadResult CHarnessBridgeEndpointReader::Read(
	const std::wstring& mappingName, const HarnessBridgeEndpointReadRequirements& requirements)
{
	HarnessBridgeEndpointReadResult result;
	if (!IsSafeHarnessEndpointMappingName(mappingName)) {
		result.disposition = EHarnessBridgeEndpointDisposition::InvalidDescriptor;
		return result;
	}
	for (int attempt = 0; attempt < 2; ++attempt) {
		if (!m_mapping) {
			m_mapping = ::OpenFileMappingW(FILE_MAP_READ | READ_CONTROL, FALSE, mappingName.c_str());
			if (!m_mapping) {
				const auto error = ::GetLastError();
				result.disposition = error == ERROR_FILE_NOT_FOUND ? EHarnessBridgeEndpointDisposition::NotPublished
					: error == ERROR_ACCESS_DENIED ? EHarnessBridgeEndpointDisposition::AccessDenied
					: EHarnessBridgeEndpointDisposition::ResourceOrIoFailure;
				return result;
			}
			std::wstring diagnostic;
			if (!VerifyHarnessCurrentUserOnlyDacl(m_mapping, diagnostic)) {
				Close();
				result.disposition = EHarnessBridgeEndpointDisposition::SecurityRejected;
				return result;
			}
			m_view = ::MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, sizeof(WireBlock));
			if (!m_view) { Close(); result.disposition = EHarnessBridgeEndpointDisposition::ResourceOrIoFailure; return result; }
		}
		HarnessEditorEndpointDescriptor descriptor;
		bool copied = false;
		const auto* block = static_cast<const WireBlock*>(m_view);
		for (int read = 0; read < 16; ++read) {
			const LONG before = block->sequence;
			if (before & 1) continue;
			WireBlock copy{};
			std::memcpy(&copy, block, sizeof(copy));
			::MemoryBarrier();
			if (before != block->sequence || (copy.sequence & 1) || copy.magic != kSharedMagic) continue;
			copied = CopyDescriptor(copy, descriptor);
			break;
		}
		if (!copied) {
			result.disposition = EHarnessBridgeEndpointDisposition::Busy;
			return result;
		}
		if (!ValidateHarnessBridgeEndpointDescriptor(descriptor, requirements)) {
			Close();
			if (attempt == 0) continue;
			result.disposition = EHarnessBridgeEndpointDisposition::InvalidDescriptor;
			return result;
		}
		if (descriptor.lifecycle != EHarnessBridgeLifecycle::Accepting) {
			result.disposition = descriptor.lifecycle == EHarnessBridgeLifecycle::Stopped
				? EHarnessBridgeEndpointDisposition::Closed : EHarnessBridgeEndpointDisposition::NotAccepting;
			return result;
		}
	if (requirements.requireLiveServer && (!IsLive(descriptor.serverPid)
		|| !MatchesCreationTime(descriptor.serverPid, descriptor.serverProcessCreationTime))) {
			Close();
			result.disposition = EHarnessBridgeEndpointDisposition::DeadOrStale;
			return result;
		}
		result.disposition = EHarnessBridgeEndpointDisposition::Discovered;
		result.descriptor = std::move(descriptor);
		return result;
	}
	result.disposition = EHarnessBridgeEndpointDisposition::InvalidDescriptor;
	return result;
}

void CHarnessBridgeEndpointReader::Close() noexcept
{
	if (m_view) ::UnmapViewOfFile(m_view);
	if (m_mapping) ::CloseHandle(m_mapping);
	m_view = nullptr;
	m_mapping = nullptr;
}

} // namespace platform::harnessbridge
