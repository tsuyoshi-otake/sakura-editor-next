#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeAuthenticatedSession.h>

#include <array>
#include <algorithm>
#include <memory>

namespace platform::harnessbridge {
namespace {

HarnessBridgeTargetDescriptor Target()
{
	HarnessBridgeTargetDescriptor target;
	target.profileId = "default";
	target.profileGeneration = 2;
	target.editorId[0] = 3;
	target.bridgeEpoch = 4;
	target.runtimeGeneration = 5;
	target.instanceGeneration = 6;
	target.sessionId = 7;
	target.windowId = 8;
	target.paneId = 9;
	target.instanceId = 10;
	return target;
}

HarnessBridgeFrame Hello(const HarnessCapabilityCredential& credential,
	const HarnessBridgeTargetDescriptor& target, const std::array<std::uint8_t, 16>& nonce)
{
	const auto encodedTarget = EncodeHarnessTargetEnvironment(target);
	EXPECT_TRUE(encodedTarget);
	std::string targetBytes;
	for (const auto value : *encodedTarget) targetBytes.push_back(static_cast<char>(value));
	HarnessBridgeFields fields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload),
			std::vector<std::uint8_t>(credential.id.value.begin(), credential.id.value.end()) },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::CurrentTarget),
			std::vector<std::uint8_t>(targetBytes.begin(), targetBytes.end()) },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::ClientNonce),
			std::vector<std::uint8_t>(nonce.begin(), nonce.end()) },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Grants), { 7, 0, 0, 0 } },
	};
	const auto payload = EncodeHarnessBridgeFields(fields);
	EXPECT_TRUE(payload);
	return { { kHarnessBridgeMajorVersion, kHarnessBridgeMinorVersion,
		EHarnessBridgeFrameKind::Hello, EHarnessBridgeFrameFlags::Request, 11, 0 }, *payload };
}

HarnessBridgeFrame Authenticate(const std::array<std::uint8_t, 32>& digest)
{
	HarnessBridgeFields fields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::AuthenticationDigest),
			std::vector<std::uint8_t>(digest.begin(), digest.end()) },
	};
	const auto payload = EncodeHarnessBridgeFields(fields);
	EXPECT_TRUE(payload);
	return { { kHarnessBridgeMajorVersion, kHarnessBridgeMinorVersion,
		EHarnessBridgeFrameKind::Authenticate, EHarnessBridgeFrameFlags::Request, 11, 4 }, *payload };
}

std::vector<std::uint8_t> Transcript(const HarnessBridgeTargetDescriptor& target,
	const std::array<std::uint8_t, 16>& bridgeId, const std::array<std::uint8_t, 16>& clientNonce,
	const std::array<std::uint8_t, 16>& serverNonce)
{
	return BuildHarnessBridgeAuthenticationTranscript(11, 42, bridgeId, target,
		clientNonce, serverNonce, EHarnessGrant::Message | EHarnessGrant::ConsoleRead | EHarnessGrant::SendInput);
}

class FakeDispatcher final : public IHarnessBridgeOperationDispatcher {
public:
	HarnessBridgeOperationResponseDto Dispatch(const HarnessBridgeSessionContext&,
		const HarnessBridgeOperationRequestDto& request) override
	{
		++dispatchCount;
		lastOperation = request.operation;
		return { EHarnessTerminalStatus::Succeeded, { 0x41 } };
	}
	void Cancel(std::uint64_t) noexcept override {}
	int dispatchCount = 0;
	EHarnessOperationKind lastOperation = EHarnessOperationKind::QueryOperation;
};

TEST(HarnessBridgeAuthenticatedSession, PerformsChallengeHmacReadyAndDeduplicatedDispatch)
{
	const auto target = Target();
	HarnessCapabilityContext context;
	context.bridgeEpoch = target.bridgeEpoch;
	context.runtimeGeneration = target.runtimeGeneration;
	context.sessionId = target.sessionId;
	context.paneId = target.paneId;
	// Production issues this credential before CreateProcess returns. The
	// authenticated session separately proves that each connecting descendant
	// belongs to the terminal instance's job.
	context.processId = 0;
	context.processCreationTime = 0;
	context.profileId = target.profileId;
	context.profileGeneration = target.profileGeneration;
	context.editorId = target.editorId;
	context.instanceGeneration = target.instanceGeneration;
	context.windowId = target.windowId;
	context.instanceId = target.instanceId;
	CHarnessBridgeCapabilityStore store;
	const auto credential = store.Issue(context,
		EHarnessGrant::Message | EHarnessGrant::ConsoleRead | EHarnessGrant::SendInput);
	ASSERT_TRUE(credential);
	HarnessBridgeSessionFences fences{ target, {} };
	fences.bridgeId[0] = 12;
	HarnessBridgePeerProcessCallbacks callbacks;
	callbacks.queryCreationTime = [](std::uint32_t pid) -> std::optional<std::uint64_t> {
		return pid == 42 ? std::optional<std::uint64_t>{ 99 } : std::nullopt;
	};
	callbacks.isJobMember = [](const HarnessBridgeTargetDescriptor&, std::uint32_t pid,
		std::uint64_t creation) { return pid == 42 && creation == 99; };
	auto dispatcher = std::make_shared<FakeDispatcher>();
	CHarnessBridgeAuthenticatedSession session(fences, store, callbacks, dispatcher);
	const HarnessBridgeSessionContext peer{ 123, 42 };
	std::array<std::uint8_t, 16> clientNonce{};
	clientNonce[0] = 21;
	std::vector<HarnessBridgeFrame> responses;
	EXPECT_TRUE(session.HandleFrame(peer, Hello(*credential, target, clientNonce), responses).success);
	ASSERT_EQ(EHarnessBridgeAuthenticationState::ChallengeSent, session.State());
	ASSERT_EQ(1u, responses.size());
	const auto challengeFields = DecodeHarnessBridgeFields(responses[0].payload);
	ASSERT_EQ(EHarnessBridgeFieldDecodeOutcome::Decoded, challengeFields.outcome);
	const auto* serverField = FindHarnessBridgeField(challengeFields.fields, EHarnessBridgeFieldTag::ServerNonce);
	ASSERT_NE(nullptr, serverField);
	ASSERT_EQ(16u, serverField->value.size());
	std::array<std::uint8_t, 16> serverNonce{};
	std::copy(serverField->value.begin(), serverField->value.end(), serverNonce.begin());
	const auto digest = store.ComputeAuthenticationDigest(credential->id,
		Transcript(target, fences.bridgeId, clientNonce, serverNonce), std::chrono::steady_clock::now());
	ASSERT_TRUE(digest);
	responses.clear();
	EXPECT_TRUE(session.HandleFrame(peer, Authenticate(*digest), responses).success);
	ASSERT_EQ(EHarnessBridgeAuthenticationState::Ready, session.State());
	ASSERT_EQ(1u, responses.size());

	HarnessOpaqueId operationId;
	operationId.value[0] = 88;
	HarnessBridgeFields fields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::OperationKind), { static_cast<std::uint8_t>(EHarnessOperationKind::Capture), static_cast<std::uint8_t>(static_cast<std::uint16_t>(EHarnessOperationKind::Capture) >> 8) } },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::OperationId), { operationId.value.begin(), operationId.value.end() } },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::TimeoutMs), { 0xe8, 0x03, 0, 0 } },
	};
	const auto operationPayload = EncodeHarnessBridgeFields(fields);
	ASSERT_TRUE(operationPayload);
	const HarnessBridgeFrame operation{ { kHarnessBridgeMajorVersion, kHarnessBridgeMinorVersion,
		EHarnessBridgeFrameKind::OperationRequest, EHarnessBridgeFrameFlags::Request, 12, 4 }, *operationPayload };
	responses.clear();
	EXPECT_TRUE(session.HandleFrame(peer, operation, responses).success);
	ASSERT_EQ(1, dispatcher->dispatchCount);
	ASSERT_EQ(EHarnessOperationKind::Capture, dispatcher->lastOperation);
	responses.clear();
	EXPECT_TRUE(session.HandleFrame(peer, operation, responses).success);
	EXPECT_EQ(1, dispatcher->dispatchCount);
}

TEST(HarnessBridgeAuthenticatedSession, AuthenticationFailureIsTerminalAndClosed)
{
	const auto target = Target();
	HarnessCapabilityContext context{ target.bridgeEpoch, target.runtimeGeneration, target.sessionId,
		target.paneId, 42, 99 };
	CHarnessBridgeCapabilityStore store;
	const auto credential = store.Issue(context, EHarnessGrant::ConsoleRead);
	ASSERT_TRUE(credential);
	HarnessBridgeSessionFences fences{ target, {} };
	fences.bridgeId[0] = 13;
	HarnessBridgePeerProcessCallbacks callbacks{
		[](std::uint32_t) -> std::optional<std::uint64_t> { return 99; },
		[](const HarnessBridgeTargetDescriptor&, std::uint32_t, std::uint64_t) { return true; },
	};
	CHarnessBridgeAuthenticatedSession session(fences, store, callbacks, nullptr);
	std::vector<HarnessBridgeFrame> responses;
	std::array<std::uint8_t, 16> nonce{};
	nonce[0] = 1;
	ASSERT_TRUE(session.HandleFrame({ 1, 42 }, Hello(*credential, target, nonce), responses).success);
	std::array<std::uint8_t, 32> badDigest{};
	responses.clear();
	EXPECT_TRUE(session.HandleFrame({ 1, 42 }, Authenticate(badDigest), responses).success);
	EXPECT_EQ(EHarnessBridgeAuthenticationState::Closed, session.State());
	ASSERT_EQ(1u, responses.size());
	EXPECT_EQ(EHarnessBridgeFrameKind::Error, responses[0].header.kind);
	EXPECT_NE(EHarnessBridgeFrameFlags::None, responses[0].header.flags & EHarnessBridgeFrameFlags::Terminal);
}

} // namespace
} // namespace platform::harnessbridge
