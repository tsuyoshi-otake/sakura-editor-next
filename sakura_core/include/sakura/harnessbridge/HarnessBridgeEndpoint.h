/*! @file
    @brief Independent shared endpoint descriptor for the Harness Bridge.
*/
#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace platform::harnessbridge {

inline constexpr std::uint16_t kHarnessBridgeEndpointDescriptorVersion = 1;

enum class EHarnessBridgeLifecycle : std::uint32_t {
	Starting,
	Accepting,
	Stopping,
	Stopped,
};

struct HarnessEditorEndpointDescriptor final {
	std::uint16_t descriptorVersion = kHarnessBridgeEndpointDescriptorVersion;
	std::wstring endpointHash;
	std::wstring profileId;
	std::array<std::uint8_t, 16> editorId{};
	std::array<std::uint8_t, 16> bridgeId{};
	std::uint64_t profileGeneration = 0;
	std::uint64_t bridgeEpoch = 0;
	std::uint64_t runtimeGeneration = 0;
	EHarnessBridgeLifecycle lifecycle = EHarnessBridgeLifecycle::Starting;
	std::uint32_t serverPid = 0;
	std::uint64_t serverProcessCreationTime = 0;
	std::wstring pipeName;
	std::uint16_t protocolMajor = 1;
	std::uint16_t protocolMinor = 0;
};

enum class EHarnessBridgeEndpointDisposition : std::uint8_t {
	Discovered,
	NotPublished,
	NotAccepting,
	DeadOrStale,
	Busy,
	InvalidDescriptor,
	Closed,
	AccessDenied,
	SecurityRejected,
	UnsupportedOrMalformedAbi,
	ResourceOrIoFailure,
};

struct HarnessBridgeEndpointReadRequirements final {
	std::wstring expectedEndpointHash;
	std::uint64_t minimumBridgeEpoch = 0;
	bool requireLiveServer = true;
};

struct HarnessBridgeEndpointReadResult final {
	EHarnessBridgeEndpointDisposition disposition = EHarnessBridgeEndpointDisposition::Closed;
	std::optional<HarnessEditorEndpointDescriptor> descriptor;
};

class CHarnessBridgeEndpointPublisher final {
public:
	CHarnessBridgeEndpointPublisher() = default;
	~CHarnessBridgeEndpointPublisher();
	CHarnessBridgeEndpointPublisher(const CHarnessBridgeEndpointPublisher&) = delete;
	CHarnessBridgeEndpointPublisher& operator=(const CHarnessBridgeEndpointPublisher&) = delete;

	[[nodiscard]] bool Create(const HarnessEditorEndpointDescriptor& descriptor, std::wstring& diagnostic);
	[[nodiscard]] bool Publish(EHarnessBridgeLifecycle lifecycle, std::wstring& diagnostic);
	void Close() noexcept;
	[[nodiscard]] bool IsOpen() const noexcept { return m_mapping != nullptr; }

private:
	HANDLE m_mapping = nullptr;
	void* m_block = nullptr;
	HarnessEditorEndpointDescriptor m_descriptor;
};

class CHarnessBridgeEndpointReader final {
public:
	CHarnessBridgeEndpointReader() = default;
	~CHarnessBridgeEndpointReader();
	CHarnessBridgeEndpointReader(const CHarnessBridgeEndpointReader&) = delete;
	CHarnessBridgeEndpointReader& operator=(const CHarnessBridgeEndpointReader&) = delete;

	[[nodiscard]] HarnessBridgeEndpointReadResult Read(
		const std::wstring& mappingName,
		const HarnessBridgeEndpointReadRequirements& requirements);
	void Close() noexcept;

private:
	HANDLE m_mapping = nullptr;
	void* m_view = nullptr;
};

[[nodiscard]] bool ValidateHarnessBridgeEndpointDescriptor(
	const HarnessEditorEndpointDescriptor& descriptor,
	const HarnessBridgeEndpointReadRequirements& requirements) noexcept;

} // namespace platform::harnessbridge
