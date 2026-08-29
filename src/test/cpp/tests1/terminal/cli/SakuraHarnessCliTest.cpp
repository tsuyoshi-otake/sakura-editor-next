#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/cli/SakuraHarnessCli.h"

#include <array>
#include <string>
#include <vector>

namespace {

using namespace terminal::cli;
using namespace platform::harnessbridge;

class FakeHarnessBridge final : public ISakuraHarnessBridgeClient {
public:
	HarnessBridgeClientConnectResult Connect(const SakuraHarnessEnvironment& environment) override
	{
		++connectCalls;
		connectedEnvironment = environment;
		return connectResult;
	}

	HarnessBridgeClientOperationResult Execute(const EHarnessOperationKind operation,
		const std::span<const std::uint8_t> payload, const std::chrono::milliseconds timeout) override
	{
		++executeCalls;
		lastOperation = operation;
		lastPayload.assign(payload.begin(), payload.end());
		lastTimeout = timeout;
		return operationResult;
	}

	void Close() noexcept override { ++closeCalls; }

	HarnessBridgeClientConnectResult connectResult{ true, EHarnessTerminalStatus::Succeeded, 0 };
	HarnessBridgeClientOperationResult operationResult;
	SakuraHarnessEnvironment connectedEnvironment;
	EHarnessOperationKind lastOperation = EHarnessOperationKind::QueryOperation;
	std::vector<std::uint8_t> lastPayload;
	std::chrono::milliseconds lastTimeout{};
	int connectCalls = 0;
	int executeCalls = 0;
	int closeCalls = 0;
};

class FixedHarnessIds final : public ISakuraHarnessIdSource {
public:
	std::optional<HarnessOpaqueId> NextId() noexcept override
	{
		HarnessOpaqueId result;
		result.value[0] = nextByte++;
		return result;
	}

	std::uint8_t nextByte = 0x40;
};

SakuraHarnessEnvironment CompleteEnvironment()
{
	SakuraHarnessEnvironment result{ L"she1.0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		L"sht1.valid-target", L"shc1.valid-capability", std::nullopt };
	return result;
}

HarnessOpaqueId Id(const std::uint8_t first)
{
	HarnessOpaqueId result;
	result.value[0] = first;
	return result;
}

std::vector<std::wstring_view> Args(std::initializer_list<std::wstring_view> values)
{
	return { values.begin(), values.end() };
}

TEST(SakuraHarnessCli, VersionAndParserAreBridgeIndependent)
{
	FakeHarnessBridge bridge;
	FixedHarnessIds ids;
	const auto arguments = Args({ L"-V" });
	const auto result = RunSakuraHarnessCli(arguments, {}, bridge, ids);
	EXPECT_EQ(0, result.exitCode);
	EXPECT_NE(std::string::npos, result.stdoutText.find("\"version\":1"));
	EXPECT_NE(std::string::npos, result.stdoutText.find("\"product\":\"sakura-harness\""));
	EXPECT_TRUE(result.stderrText.empty());
	EXPECT_EQ(0, bridge.connectCalls);

	const auto parsed = ParseSakuraHarnessArguments(Args({ L"message", L"send", L"--to",
		L"01000000000000000000000000000000", L"--from",
		L"02000000000000000000000000000000", L"--type", L"event", L"--payload-stdin" }));
	ASSERT_EQ(SakuraHarnessParseOutcome::Parsed, parsed.outcome);
	EXPECT_EQ(SakuraHarnessCommandKind::MessageSend, parsed.command.kind);
}

TEST(SakuraHarnessCli, RegisterAndListUseBoundedPublicBrokerCodecs)
{
	FakeHarnessBridge bridge;
	FixedHarnessIds ids;
	bridge.operationResult.status = EHarnessTerminalStatus::Succeeded;
	const auto registerArguments = Args({ L"endpoint", L"register", L"--name", L"worker",
		L"--capabilities", L"message,manage-terminal" });
	const auto registered = RunSakuraHarnessCli(registerArguments, CompleteEnvironment(), bridge, ids);
	ASSERT_EQ(0, registered.exitCode);
	EXPECT_EQ(EHarnessOperationKind::RegisterEndpoint, bridge.lastOperation);
	HarnessEndpointRegistration registration;
	ASSERT_TRUE(DecodeHarnessBridgeEndpointRegistration(bridge.lastPayload, registration));
	EXPECT_EQ("worker", registration.displayName);
	EXPECT_TRUE(HasGrant(registration.grants, EHarnessGrant::Message));
	EXPECT_TRUE(HasGrant(registration.grants, EHarnessGrant::ManageTerminal));
	EXPECT_NE(std::string::npos, registered.stdoutText.find("\"endpoint_id\":\""));

	const auto listed = std::vector<HarnessEndpointInfo>{
		{ Id(0x11), "worker", "terminal", EHarnessGrant::Message },
	};
	bridge.operationResult.payload = *EncodeHarnessBridgeEndpointList(listed);
	const auto listArguments = Args({ L"endpoint", L"list" });
	const auto listResult = RunSakuraHarnessCli(listArguments, CompleteEnvironment(), bridge, ids);
	ASSERT_EQ(0, listResult.exitCode);
	EXPECT_EQ(EHarnessOperationKind::ListEndpoints, bridge.lastOperation);
	EXPECT_NE(std::string::npos, listResult.stdoutText.find("worker"));
	EXPECT_NE(std::string::npos, listResult.stdoutText.find("endpoint_ids"));
}

TEST(SakuraHarnessCli, MessageSendReceiveAndAckPreservePayloadAsHex)
{
	FakeHarnessBridge bridge;
	FixedHarnessIds ids;
	auto environment = CompleteEnvironment();
	environment.endpointId = Id(0x20);
	bridge.operationResult.status = EHarnessTerminalStatus::Succeeded;
	const std::array<std::uint8_t, 3> payload{ 0x00, 0x7f, 0xff };
	const auto sendArguments = Args({ L"message", L"send", L"--to",
		L"21000000000000000000000000000000", L"--from",
		L"20000000000000000000000000000000", L"--type", L"event", L"--payload-stdin" });
	const auto sent = RunSakuraHarnessCli(sendArguments, environment, bridge, ids, payload);
	ASSERT_EQ(0, sent.exitCode);
	HarnessMessage message;
	ASSERT_TRUE(DecodeHarnessBridgeMessage(bridge.lastPayload, message));
	EXPECT_EQ(EHarnessOperationKind::SendEndpointMessage, bridge.lastOperation);
	EXPECT_EQ(payload.size(), message.payload.size());
	EXPECT_TRUE(std::equal(payload.begin(), payload.end(), message.payload.begin()));
	EXPECT_EQ(environment.endpointId, message.sender);

	HarnessMessageDelivery delivery{ message, 2 };
	bridge.operationResult.payload = *EncodeHarnessBridgeDeliveries({ delivery });
	const auto receiveArguments = Args({ L"message", L"receive", L"--endpoint",
		L"20000000000000000000000000000000", L"--wait", L"1s" });
	const auto received = RunSakuraHarnessCli(receiveArguments, environment, bridge, ids);
	ASSERT_EQ(0, received.exitCode);
	EXPECT_EQ(EHarnessOperationKind::ReceiveMessages, bridge.lastOperation);
	EXPECT_NE(std::string::npos, received.stdoutText.find("\"payload_hex\":\"007fff\""));

	const auto ackArguments = Args({ L"message", L"ack", L"--endpoint",
		L"20000000000000000000000000000000", L"--message",
		L"40000000000000000000000000000000" });
	const auto acknowledged = RunSakuraHarnessCli(ackArguments, environment, bridge, ids);
	ASSERT_EQ(0, acknowledged.exitCode);
	HarnessEndpointId recipient;
	HarnessMessageId acknowledgedId;
	ASSERT_TRUE(DecodeHarnessBridgeAcknowledgeRequest(bridge.lastPayload, recipient, acknowledgedId));
	EXPECT_EQ(environment.endpointId, recipient);
	EXPECT_EQ(Id(0x40), acknowledgedId);
}

TEST(SakuraHarnessCli, RunPublishAndWaitHaveTypedStatusAndDeadline)
{
	FakeHarnessBridge bridge;
	FixedHarnessIds ids;
	bridge.operationResult.status = EHarnessTerminalStatus::Succeeded;
	const auto runArguments = Args({ L"run", L"publish", L"--run",
		L"55000000000000000000000000000000", L"--status", L"failed" });
	const auto published = RunSakuraHarnessCli(runArguments, CompleteEnvironment(), bridge, ids);
	ASSERT_EQ(0, published.exitCode);
	HarnessRunResult run;
	bool begin = true;
	ASSERT_TRUE(DecodeHarnessBridgeRunPublish(bridge.lastPayload, begin, run));
	EXPECT_FALSE(begin);
	EXPECT_EQ(EHarnessRunTerminalStatus::Failed, run.status);

	run.runId = Id(0x55);
	run.status = EHarnessRunTerminalStatus::Succeeded;
	bridge.operationResult.payload = *EncodeHarnessBridgeRun(run);
	const auto waitArguments = Args({ L"run", L"wait", L"--run",
		L"55000000000000000000000000000000", L"--timeout", L"2s" });
	const auto waited = RunSakuraHarnessCli(waitArguments, CompleteEnvironment(), bridge, ids);
	ASSERT_EQ(0, waited.exitCode);
	EXPECT_EQ(EHarnessOperationKind::WaitRun, bridge.lastOperation);
	EXPECT_EQ(std::chrono::seconds(2), bridge.lastTimeout);
	EXPECT_NE(std::string::npos, waited.stdoutText.find("\"run_status\":\"succeeded\""));
}

TEST(SakuraHarnessCli, UnsupportedAndOversizedInputFailClosed)
{
	FakeHarnessBridge bridge;
	FixedHarnessIds ids;
	const auto unsupportedArguments = Args({ L"run", L"cancel", L"--run",
		L"01000000000000000000000000000000" });
	const auto unsupported = RunSakuraHarnessCli(unsupportedArguments, {}, bridge, ids);
	EXPECT_EQ(10, unsupported.exitCode);
	EXPECT_EQ("sakura-harness: unsupported-operation\n", unsupported.stderrText);
	EXPECT_EQ(0, bridge.connectCalls);

	const std::vector<std::uint8_t> oversized(kSakuraHarnessMaximumInputBytes + 1, 0x41);
	auto environment = CompleteEnvironment();
	environment.endpointId = Id(0x20);
	const auto sendArguments = Args({ L"message", L"send", L"--to",
		L"21000000000000000000000000000000", L"--from",
		L"20000000000000000000000000000000", L"--type", L"event", L"--payload-stdin" });
	const auto result = RunSakuraHarnessCli(sendArguments, environment, bridge, ids, oversized);
	EXPECT_EQ(7, result.exitCode);
	EXPECT_EQ("sakura-harness: resource-exhausted\n", result.stderrText);
}

} // namespace
