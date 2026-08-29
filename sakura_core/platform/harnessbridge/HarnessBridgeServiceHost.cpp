/*! @file */
#include <sakura/harnessbridge/HarnessBridgeServiceHost.h>

#include <array>

namespace platform::harnessbridge {

namespace {

class UnsupportedSession final : public IHarnessBridgeSessionHandler {
public:
	HarnessBridgeTransportResult HandleFrame(const HarnessBridgeSessionContext&,
		const HarnessBridgeFrame& frame, std::vector<HarnessBridgeFrame>& responses) override
	{
		HarnessBridgeFields fields;
		const std::uint16_t status = static_cast<std::uint16_t>(EHarnessTerminalStatus::UnsupportedCapability);
		std::array<std::uint8_t, 2> bytes{ static_cast<std::uint8_t>(status), static_cast<std::uint8_t>(status >> 8) };
		(void)AddHarnessBridgeBytesField(fields, EHarnessBridgeFieldTag::TerminalStatus, bytes);
		const auto payload = EncodeHarnessBridgeFields(fields);
		if (!payload) return { false, EHarnessBridgeDisconnectReason::CallbackFailed, ERROR_INVALID_DATA };
		responses.push_back({ { kHarnessBridgeMajorVersion, kHarnessBridgeMinorVersion,
			EHarnessBridgeFrameKind::Error,
			EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Terminal,
			frame.header.requestId, frame.header.bridgeEpoch == 0 ? 1 : frame.header.bridgeEpoch }, *payload });
		return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
	}
};

class UnsupportedFactory final : public IHarnessBridgeSessionFactory {
public:
	std::unique_ptr<IHarnessBridgeSessionHandler> CreateSession(
		const HarnessBridgeSessionContext&, const HarnessBridgeFrame&) override
	{
		return std::make_unique<UnsupportedSession>();
	}
};

} // namespace

CHarnessBridgeServiceHost::CHarnessBridgeServiceHost(std::shared_ptr<IHarnessBridgeSessionFactory> factory)
	: m_broker(std::make_unique<CHarnessBridgeBroker>()),
	  m_factory(factory ? std::move(factory) : std::make_shared<UnsupportedFactory>())
{
}

CHarnessBridgeServiceHost::~CHarnessBridgeServiceHost()
{
	Stop();
}

EHarnessBridgeHostStartResult CHarnessBridgeServiceHost::Start(
	HarnessEditorEndpointDescriptor descriptor, std::wstring& diagnostic)
{
	std::lock_guard lock(m_mutex);
	if (m_state == EHarnessBridgeHostState::Accepting) {
		diagnostic.clear();
		return EHarnessBridgeHostStartResult::AlreadyStarted;
	}
	if (m_state == EHarnessBridgeHostState::Starting || m_state == EHarnessBridgeHostState::Stopping) {
		diagnostic = L"Harness Bridge host is busy";
		return EHarnessBridgeHostStartResult::EndpointFailed;
	}
	const HarnessBridgeEndpointReadRequirements requirements{ descriptor.endpointHash, 0, false };
	if (!ValidateHarnessBridgeEndpointDescriptor(descriptor, requirements)
		|| descriptor.lifecycle != EHarnessBridgeLifecycle::Starting) {
		diagnostic = L"Invalid Harness Bridge endpoint descriptor";
		return EHarnessBridgeHostStartResult::InvalidDescriptor;
	}
	m_state = EHarnessBridgeHostState::Starting;
	if (m_broker->Start() != EHarnessBrokerStatus::Accepted) {
		m_state = EHarnessBridgeHostState::Stopped;
		diagnostic = L"Harness Bridge broker failed to start";
		return EHarnessBridgeHostStartResult::BrokerFailed;
	}
	if (!m_endpoint.Create(descriptor, diagnostic)) {
		m_broker->Stop();
		m_state = EHarnessBridgeHostState::Stopped;
		return EHarnessBridgeHostStartResult::EndpointFailed;
	}
	m_pipe = std::make_unique<CHarnessBridgeNamedPipeServer>(m_factory);
	HarnessBridgeTransportOptions pipeOptions;
	pipeOptions.pipeName = descriptor.pipeName;
	pipeOptions.maximumSessions = 32;
	pipeOptions.maximumQueuedBytes = kHarnessBridgeMaximumFrameBytes;
	pipeOptions.readBufferBytes = 16 * 1024;
	pipeOptions.ioTimeout = std::chrono::seconds(5);
	const auto pipeStart = m_pipe->Start(pipeOptions);
	if (!pipeStart.success) {
		m_pipe.reset();
		m_endpoint.Close();
		m_broker->Stop();
		m_state = EHarnessBridgeHostState::Stopped;
		diagnostic = L"Harness Bridge named pipe failed to start";
		return EHarnessBridgeHostStartResult::EndpointFailed;
	}
	if (!m_endpoint.Publish(EHarnessBridgeLifecycle::Accepting, diagnostic)) {
		m_pipe->Stop();
		m_pipe.reset();
		m_endpoint.Close();
		m_broker->Stop();
		m_state = EHarnessBridgeHostState::Stopped;
		return EHarnessBridgeHostStartResult::EndpointFailed;
	}
	m_state = EHarnessBridgeHostState::Accepting;
	diagnostic.clear();
	return EHarnessBridgeHostStartResult::Started;
}

void CHarnessBridgeServiceHost::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_state == EHarnessBridgeHostState::Stopped) return;
	m_state = EHarnessBridgeHostState::Stopping;
	std::wstring ignored;
	(void)m_endpoint.Publish(EHarnessBridgeLifecycle::Stopping, ignored);
	if (m_pipe) {
		m_pipe->Stop();
		m_pipe.reset();
	}
	// Broker Stop closes the admission gate and terminalizes all known runs.
	m_broker->Stop();
	(void)m_endpoint.Publish(EHarnessBridgeLifecycle::Stopped, ignored);
	m_endpoint.Close();
	m_state = EHarnessBridgeHostState::Stopped;
}

EHarnessBridgeHostState CHarnessBridgeServiceHost::State() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_state;
}

} // namespace platform::harnessbridge
