#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace platform::harnessbridge {
namespace {

void PutU16(std::vector<std::uint8_t>& bytes, const std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutU32(std::vector<std::uint8_t>& bytes, const std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void PutU64(std::vector<std::uint8_t>& bytes, const std::uint64_t value)
{
	for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void PutId(std::vector<std::uint8_t>& bytes, const HarnessOpaqueId& id)
{
	bytes.insert(bytes.end(), id.value.begin(), id.value.end());
}

std::vector<std::uint8_t> RegistrationPayload(const HarnessOpaqueId& endpoint, const char* name)
{
	std::vector<std::uint8_t> payload{ 1 };
	const auto nameLength = std::strlen(name);
	PutId(payload, endpoint);
	PutU16(payload, static_cast<std::uint16_t>(nameLength));
	payload.insert(payload.end(), name, name + nameLength);
	PutU16(payload, 6);
	payload.insert(payload.end(), { 's', 'c', 'o', 'p', 'e', '1' });
	PutU32(payload, static_cast<std::uint32_t>(EHarnessGrant::Message));
	PutU32(payload, 2);
	return payload;
}

HarnessBridgeTargetDescriptor Target(const std::uint8_t value)
{
	HarnessBridgeTargetDescriptor target;
	target.profileId = "profile";
	target.profileGeneration = 1;
	target.editorId[0] = value;
	target.bridgeEpoch = 1;
	target.runtimeGeneration = 1;
	target.instanceGeneration = 1;
	target.sessionId = value;
	target.windowId = value;
	target.paneId = value;
	target.instanceId = value;
	return target;
}

class NoopTmuxRuntime : public terminal::tmux::ITmuxRuntimePort {
public:
	[[nodiscard]] terminal::tmux::TmuxRuntimeSnapshot Snapshot() const override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult CreateSession(const terminal::tmux::TmuxCreateSessionRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult CreateTerminalWindow(const terminal::tmux::TmuxCreateWindowRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult SplitWindow(const terminal::tmux::TmuxSplitWindowRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult SelectWindow(const terminal::tmux::TmuxSelectRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult SelectPane(const terminal::tmux::TmuxSelectRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult ClosePane(const terminal::tmux::TmuxCloseRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult CloseWindow(const terminal::tmux::TmuxCloseRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult CloseSession(const terminal::tmux::TmuxCloseRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult SendKeys(const terminal::tmux::TmuxInputBatch&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxCaptureResult CapturePane(const terminal::tmux::TmuxCaptureRequest&) override { return {}; }
	[[nodiscard]] terminal::tmux::TmuxRuntimeResult WaitFor(const terminal::tmux::TmuxWaitRequest&) override { return {}; }
};

class BlockingTmuxRuntime final : public NoopTmuxRuntime {
public:
	[[nodiscard]] terminal::tmux::TmuxRuntimeSnapshot Snapshot() const override
	{
		std::unique_lock lock(m_mutex);
		m_entered = true;
		m_condition.notify_all();
		m_condition.wait(lock, [this] { return m_release; });
		return {};
	}
	void WaitUntilEntered()
	{
		std::unique_lock lock(m_mutex);
		m_condition.wait(lock, [this] { return m_entered; });
	}
	void Release()
	{
		std::lock_guard lock(m_mutex);
		m_release = true;
		m_condition.notify_all();
	}

private:
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_condition;
	mutable bool m_entered = false;
	mutable bool m_release = false;
};

TEST(HarnessBridgeOperationDispatcher, TmuxArgvAndResponseCodecsAreBoundedAndRoundTrip)
{
	const std::vector<std::string> argv{ "-V" };
	const auto encoded = EncodeHarnessBridgeTmuxArgv(argv);
	ASSERT_TRUE(encoded);
	const auto decoded = DecodeHarnessBridgeTmuxArgv(*encoded);
	EXPECT_EQ(EHarnessBridgePayloadDecodeOutcome::Decoded, decoded.outcome);
	EXPECT_EQ(argv, decoded.argv);
	EXPECT_FALSE(EncodeHarnessBridgeTmuxArgv({}));
	std::vector<std::uint8_t> oversized{ 1, 1, 0, 0xff, 0xff };
	EXPECT_NE(EHarnessBridgePayloadDecodeOutcome::Decoded, DecodeHarnessBridgeTmuxArgv(oversized).outcome);

	const HarnessBridgeTmuxResponse response{ 3, "stdout", "stderr" };
	const auto encodedResponse = EncodeHarnessBridgeTmuxResponse(response);
	ASSERT_TRUE(encodedResponse);
	const auto decodedResponse = DecodeHarnessBridgeTmuxResponse(*encodedResponse);
	ASSERT_TRUE(decodedResponse);
	EXPECT_EQ(response.exitCode, decodedResponse->exitCode);
	EXPECT_EQ(response.stdoutText, decodedResponse->stdoutText);
	EXPECT_EQ(response.stderrText, decodedResponse->stderrText);
}

TEST(HarnessBridgeOperationDispatcher, ExecutesTmuxThroughInjectedRuntimeAndMapsUnknown)
{
	NoopTmuxRuntime runtime;
	CHarnessBridgeOperationDispatcher dispatcher(nullptr, &runtime);
	HarnessBridgeOperationRequestDto request;
	request.operation = EHarnessOperationKind::ExecuteTmux;
	request.operationId.value[0] = 1;
	request.payload = *EncodeHarnessBridgeTmuxArgv({ "-V" });
	request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	const auto response = dispatcher.Dispatch({}, request);
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, response.status);
	const auto decoded = DecodeHarnessBridgeTmuxResponse(response.payload);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(0, decoded->exitCode);

	request.operation = static_cast<EHarnessOperationKind>(0x7fff);
	request.payload.clear();
	EXPECT_EQ(EHarnessTerminalStatus::OperationUnknown, dispatcher.Dispatch({}, request).status);
	request.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
	EXPECT_EQ(EHarnessTerminalStatus::DeadlineExceeded, dispatcher.Dispatch({}, request).status);
}

TEST(HarnessBridgeOperationDispatcher, CancellationBecomesTerminalAfterRuntimeCallQuiesces)
{
	BlockingTmuxRuntime runtime;
	CHarnessBridgeOperationDispatcher dispatcher(nullptr, &runtime);
	HarnessBridgeOperationRequestDto request;
	request.operation = EHarnessOperationKind::ExecuteTmux;
	request.operationId.value[0] = 2;
	request.payload = *EncodeHarnessBridgeTmuxArgv({ "list-sessions" });
	request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	HarnessBridgeOperationResponseDto response;
	std::thread worker([&] { response = dispatcher.Dispatch({}, request); });
	runtime.WaitUntilEntered();
	dispatcher.Cancel(request.operationId.value[0]);
	runtime.Release();
	worker.join();
	EXPECT_EQ(EHarnessTerminalStatus::Cancelled, response.status);
}

TEST(HarnessBridgeOperationDispatcher, DispatchesTypedBrokerRegistrationAndListing)
{
	CHarnessBridgeBroker broker;
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.Start());
	CHarnessBridgeOperationDispatcher dispatcher(&broker, nullptr);
	HarnessOpaqueId endpoint;
	endpoint.value[0] = 7;
	HarnessBridgeOperationRequestDto registerRequest;
	registerRequest.operation = EHarnessOperationKind::RegisterEndpoint;
	registerRequest.operationId.value[0] = 1;
	registerRequest.target = Target(1);
	registerRequest.payload = RegistrationPayload(endpoint, "test");
	registerRequest.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, dispatcher.Dispatch({}, registerRequest).status);
	HarnessOpaqueId recipient;
	recipient.value[0] = 8;
	registerRequest.operationId.value[0] = 3;
	registerRequest.target = Target(2);
	registerRequest.payload = RegistrationPayload(recipient, "peer");
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, dispatcher.Dispatch({}, registerRequest).status);

	HarnessBridgeOperationRequestDto listRequest = registerRequest;
	listRequest.operation = EHarnessOperationKind::ListEndpoints;
	listRequest.operationId.value[0] = 2;
	listRequest.payload.clear();
	const auto listed = dispatcher.Dispatch({}, listRequest);
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, listed.status);
	EXPECT_FALSE(listed.payload.empty());

	HarnessOpaqueId messageId;
	messageId.value[0] = 9;
	HarnessOpaqueId runId;
	runId.value[0] = 10;
	HarnessBridgeOperationRequestDto sendRequest;
	sendRequest.operation = EHarnessOperationKind::SendEndpointMessage;
	sendRequest.operationId.value[0] = 4;
	sendRequest.target = Target(1);
	sendRequest.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	sendRequest.payload = { 1 };
	PutId(sendRequest.payload, messageId);
	PutId(sendRequest.payload, runId);
	PutId(sendRequest.payload, endpoint);
	PutId(sendRequest.payload, recipient);
	PutId(sendRequest.payload, messageId);
	sendRequest.payload.push_back(0);
	PutU16(sendRequest.payload, 4);
	sendRequest.payload.insert(sendRequest.payload.end(), { 't', 'e', 's', 't' });
	PutU32(sendRequest.payload, 2);
	sendRequest.payload.insert(sendRequest.payload.end(), { 1, 2 });
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, dispatcher.Dispatch({}, sendRequest).status);

	HarnessBridgeOperationRequestDto beginRequest;
	beginRequest.operation = EHarnessOperationKind::PublishRun;
	beginRequest.operationId.value[0] = 5;
	beginRequest.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	beginRequest.payload = { 1, 0 };
	PutId(beginRequest.payload, runId);
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, dispatcher.Dispatch({}, beginRequest).status);
	HarnessBridgeOperationRequestDto publishRequest = beginRequest;
	publishRequest.operationId.value[0] = 6;
	publishRequest.payload = { 1, 1 };
	PutId(publishRequest.payload, runId);
	publishRequest.payload.push_back(0);
	PutU32(publishRequest.payload, 0);
	PutU64(publishRequest.payload, 1);
	EXPECT_EQ(EHarnessTerminalStatus::Succeeded, dispatcher.Dispatch({}, publishRequest).status);
	broker.Stop();
}

} // namespace
} // namespace platform::harnessbridge
