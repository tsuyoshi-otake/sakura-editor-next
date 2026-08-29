/*! @file
    @brief Versioned Harness Bridge child-environment descriptors.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeCapability.h>
#include <sakura/harnessbridge/HarnessBridgeProtocol.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace platform::harnessbridge {

inline constexpr std::wstring_view kHarnessEndpointEnvironmentName = L"SAKURA_HARNESS_ENDPOINT_V1";
inline constexpr std::wstring_view kHarnessTargetEnvironmentName = L"SAKURA_TERMINAL_TARGET_V1";
inline constexpr std::wstring_view kHarnessCapabilityEnvironmentName = L"SAKURA_HARNESS_CAPABILITY_V1";

[[nodiscard]] std::optional<std::wstring> EncodeHarnessEndpointEnvironment(
	std::wstring_view endpointHash);
[[nodiscard]] std::optional<std::wstring> DecodeHarnessEndpointEnvironment(
	std::wstring_view encoded);

[[nodiscard]] std::optional<std::wstring> EncodeHarnessTargetEnvironment(
	const HarnessBridgeTargetDescriptor& descriptor);
[[nodiscard]] std::optional<HarnessBridgeTargetDescriptor> DecodeHarnessTargetEnvironment(
	std::wstring_view encoded);

[[nodiscard]] std::optional<std::wstring> EncodeHarnessCapabilityEnvironment(
	const HarnessCapabilityCredential& credential);
[[nodiscard]] std::optional<HarnessCapabilityCredential> DecodeHarnessCapabilityEnvironment(
	std::wstring_view encoded);

} // namespace platform::harnessbridge
