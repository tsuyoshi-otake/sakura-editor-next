/*! @file
    @brief Current-user security and endpoint naming for the Harness Bridge.
*/
#pragma once

#include <Windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace platform::harnessbridge {

inline constexpr std::wstring_view kHarnessBridgePipePrefix = L"\\\\.\\pipe\\SakuraHarness-";
inline constexpr std::wstring_view kHarnessBridgeMappingPrefix = L"Local\\SakuraHarnessEndpoint-";

//! Returns a lowercase SHA-256 identity; no source path is embedded in the name.
std::wstring ComputeHarnessEndpointHash(std::wstring_view profileIdentity,
	std::span<const std::uint8_t> editorIdentity, std::uint64_t bridgeEpoch);
std::wstring BuildHarnessPipeName(std::wstring_view endpointHash);
std::wstring BuildHarnessEndpointMappingName(std::wstring_view endpointHash);
bool IsSafeHarnessPipeName(std::wstring_view pipeName) noexcept;
bool IsSafeHarnessEndpointMappingName(std::wstring_view mappingName) noexcept;

//! The returned attributes grant access only to the current user and are non-inheritable.
class HarnessBridgeSecurityAttributes final {
public:
	HarnessBridgeSecurityAttributes() = default;
	~HarnessBridgeSecurityAttributes();
	HarnessBridgeSecurityAttributes(const HarnessBridgeSecurityAttributes&) = delete;
	HarnessBridgeSecurityAttributes& operator=(const HarnessBridgeSecurityAttributes&) = delete;

	bool Initialize(std::wstring& diagnostic);
	[[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept;

private:
	PACL m_acl = nullptr;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
	bool m_initialized = false;
};

bool VerifyHarnessCurrentUserOnlyDacl(HANDLE object, std::wstring& diagnostic);
//! Must be called after a bounded first read. Always reverts impersonation before return.
bool VerifyHarnessNamedPipeClientCurrentUser(HANDLE pipe, std::wstring& diagnostic);

} // namespace platform::harnessbridge
