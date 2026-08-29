#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeBroker.h>

namespace platform::harnessbridge {
namespace {

HarnessOpaqueId Id(const std::uint8_t value)
{
	HarnessOpaqueId id;
	id.value[0] = value;
	return id;
}

HarnessEndpointRegistration Endpoint(const std::uint8_t value)
{
	return { Id(value), "endpoint-" + std::to_string(value), "same-session", EHarnessGrant::Message, 4 };
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

TEST(HarnessBridgeBroker, RegistersListsSendsDeduplicatesAndAcknowledges)
{
	CHarnessBridgeBroker broker({ 4, 4, 4, 1024, 8, 8, 4096 });
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, broker.Start());
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.RegisterEndpoint(Endpoint(1), Target(1)).status);
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.RegisterEndpoint(Endpoint(2), Target(2)).status);
	HarnessMessage message;
	message.messageId = Id(3);
	message.sender = Id(1);
	message.recipient = Id(2);
	message.type = "work";
	message.payload = { 1, 2, 3 };
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.SendEndpointMessage(message, Target(1)).status);
	EXPECT_EQ(EHarnessBrokerStatus::AccessDenied, broker.SendEndpointMessage(message, Target(2)).status);
	EXPECT_EQ(EHarnessBrokerStatus::Duplicate, broker.SendEndpointMessage(message, Target(1)).status);
	auto conflicting = message;
	conflicting.payload.push_back(4);
	EXPECT_EQ(EHarnessBrokerStatus::Conflict, broker.SendEndpointMessage(conflicting, Target(1)).status);
	EXPECT_EQ(EHarnessBrokerStatus::AccessDenied, broker.ReceiveMessages(Id(2), 1,
		std::chrono::steady_clock::now() + std::chrono::seconds(1), Target(1)).status);
	const auto received = broker.ReceiveMessages(Id(2), 1,
		std::chrono::steady_clock::now() + std::chrono::seconds(1), Target(2));
	ASSERT_EQ(EHarnessBrokerStatus::Succeeded, received.status);
	ASSERT_EQ(1u, received.messages.size());
	EXPECT_EQ(1u, received.messages[0].deliveryAttempt);
	EXPECT_EQ(message.payload, received.messages[0].message.payload);
	const auto redelivered = broker.ReceiveMessages(Id(2), 1,
		std::chrono::steady_clock::now() + std::chrono::seconds(1), Target(2));
	ASSERT_EQ(EHarnessBrokerStatus::Succeeded, redelivered.status);
	ASSERT_EQ(1u, redelivered.messages.size());
	EXPECT_EQ(2u, redelivered.messages[0].deliveryAttempt);
	EXPECT_EQ(message.messageId, redelivered.messages[0].message.messageId);
	EXPECT_EQ(EHarnessBrokerStatus::AccessDenied,
		broker.AcknowledgeMessage(Id(2), message.messageId, Target(1)).status);
	EXPECT_EQ(EHarnessBrokerStatus::Succeeded,
		broker.AcknowledgeMessage(Id(2), message.messageId, Target(2)).status);
	EXPECT_EQ(EHarnessBrokerStatus::Succeeded,
		broker.AcknowledgeMessage(Id(2), message.messageId, Target(2)).status);
}

TEST(HarnessBridgeBroker, PublishesExactlyOneTerminalRunResultAndStopsAdmission)
{
	CHarnessBridgeBroker broker;
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.Start());
	const auto run = Id(8);
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.BeginRun(run).status);
	const HarnessRunResult result{ run, EHarnessRunTerminalStatus::Succeeded, 0, 1 };
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, broker.PublishRunResult(result).status);
	EXPECT_EQ(EHarnessBrokerStatus::AlreadyTerminal, broker.PublishRunResult(result).status);
	const auto waited = broker.WaitRun(run, std::chrono::steady_clock::now() + std::chrono::seconds(1));
	ASSERT_EQ(EHarnessBrokerStatus::Succeeded, waited.status);
	ASSERT_TRUE(waited.run);
	EXPECT_EQ(EHarnessRunTerminalStatus::Succeeded, waited.run->status);
	broker.Stop();
	EXPECT_EQ(EHarnessBrokerState::Stopped, broker.State());
	EXPECT_EQ(EHarnessBrokerStatus::BrokerStopping, broker.PublishRunResult({ Id(9), EHarnessRunTerminalStatus::Failed, 1, 2 }).status);
}

} // namespace
} // namespace platform::harnessbridge
