/*! @file
    @brief Lifecycle owner for the editor-side Harness Bridge broker.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeBroker.h>
#include <sakura/harnessbridge/HarnessBridgeEndpoint.h>
#include <sakura/harnessbridge/HarnessBridgeTransport.h>

#include <memory>
#include <mutex>
#include <string>

namespace platform::harnessbridge {

enum class EHarnessBridgeHostState : std::uint8_t {
	Stopped,
	Starting,
	Accepting,
	Stopping,
};

enum class EHarnessBridgeHostStartResult : std::uint8_t {
	Started,
	AlreadyStarted,
	InvalidDescriptor,
	BrokerFailed,
	EndpointFailed,
};

//! Composes endpoint publication, named-pipe admission, and the bounded
//! in-process broker. The injected factory owns capability/terminal dispatch;
//! this owner never owns HWND, terminal model, or Control IPC state.
class CHarnessBridgeServiceHost final {
public:
	explicit CHarnessBridgeServiceHost(std::shared_ptr<IHarnessBridgeSessionFactory> factory = {});
	~CHarnessBridgeServiceHost();
	CHarnessBridgeServiceHost(const CHarnessBridgeServiceHost&) = delete;
	CHarnessBridgeServiceHost& operator=(const CHarnessBridgeServiceHost&) = delete;

	[[nodiscard]] EHarnessBridgeHostStartResult Start(
		HarnessEditorEndpointDescriptor descriptor, std::wstring& diagnostic);
	void Stop() noexcept;
	[[nodiscard]] EHarnessBridgeHostState State() const noexcept;
	[[nodiscard]] CHarnessBridgeBroker& Broker() noexcept { return *m_broker; }
	[[nodiscard]] const CHarnessBridgeBroker& Broker() const noexcept { return *m_broker; }

private:
	mutable std::mutex m_mutex;
	std::unique_ptr<CHarnessBridgeBroker> m_broker;
	std::shared_ptr<IHarnessBridgeSessionFactory> m_factory;
	std::unique_ptr<CHarnessBridgeNamedPipeServer> m_pipe;
	CHarnessBridgeEndpointPublisher m_endpoint;
	EHarnessBridgeHostState m_state = EHarnessBridgeHostState::Stopped;
};

} // namespace platform::harnessbridge
