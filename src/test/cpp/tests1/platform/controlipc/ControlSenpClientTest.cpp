/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/controlipc/ControlSenpClient.h"
#include "platform/controlipc/ControlStorageRpc.h"

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kAuthorityId[] = "0123456789abcdef0123456789abcdef";
constexpr char kOtherAuthorityId[] = "fedcba9876543210fedcba9876543210";
constexpr std::uint64_t kGeneration = 7;
constexpr wchar_t kProfile[] = L"github-profile";
constexpr wchar_t kExtension[] = L"sakura.github-pull-requests";

std::wstring Digest()
{
	return std::wstring(64, L'a');
}

ControlPlatformEndpointSnapshot Endpoint(std::uint64_t generation = kGeneration)
{
	return { ::GetCurrentProcessId(), generation, ControlPlatformEndpointLifecycle::Accepting,
		kProfileHash, BuildControlPipeName(kProfileHash), kAuthorityId };
}

ControlSenpRpcOwner Owner()
{
	return { kExtension, Digest(), 7, 11, 13 };
}

ControlSenpRpcRequest IssueGrant()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::IssueGrant;
	request.profileId = kProfile;
	request.owner = Owner();
	request.capabilities = static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead);
	return request;
}

ControlSenpRpcRequest StartRead(std::string grantId)
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::StartRead;
	request.profileId = kProfile;
	request.owner = Owner();
	request.grantId = std::move(grantId);
	request.readId = L"issues:open:1";
	request.toolId = L"github";
	request.toolOperation = L"repositoryRead";
	request.arguments = { { L"path", L"issues" } };
	return request;
}

std::vector<std::uint8_t> SenpPayload(const ControlSenpRpcResponse& response)
{
	const auto encoded = EncodeControlSenpRpcResponse(response);
	EXPECT_TRUE(encoded.has_value());
	if (!encoded) return {};
	auto fields = EncodeControlIpcFields(
		{ { static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload), *encoded } });
	EXPECT_TRUE(fields.has_value());
	return fields.value_or(std::vector<std::uint8_t>{});
}

ControlIpcFrame Terminal(EControlIpcKind kind, std::uint64_t generation, std::vector<std::uint8_t> payload)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, generation }, std::move(payload) };
}

ControlIpcFrame HelloAck(std::uint64_t generation = kGeneration, const char* profileId = kAuthorityId)
{
	const auto hello = EncodeControlStorageHello(profileId);
	EXPECT_TRUE(hello.has_value());
	return Terminal(EControlIpcKind::HelloAck, generation, hello.value_or(std::vector<std::uint8_t>{}));
}

ControlIpcFrame SenpAnswer(const ControlSenpRpcResponse& response, std::uint64_t generation = kGeneration)
{
	return Terminal(EControlIpcKind::SenpResponse, generation, SenpPayload(response));
}

ControlIpcFrame TerminalError(EControlIpcTerminalStatus status, std::uint64_t generation = kGeneration)
{
	auto payload = EncodeControlIpcError({ status, "refused" });
	EXPECT_TRUE(payload.has_value());
	return Terminal(EControlIpcKind::Error, generation, payload.value_or(std::vector<std::uint8_t>{}));
}

ControlSenpRpcResponse Granted(std::string grantId = "grant-1")
{
	ControlSenpRpcResponse response;
	response.status = EControlSenpRpcStatus::Succeeded;
	response.grantId = std::move(grantId);
	response.expiresAtMilliseconds = 1000;
	return response;
}

struct Script {
	std::deque<std::vector<ControlIpcFrame>> responses;
	std::vector<ControlIpcFrame> requests;
	ControlIpcTransportResult connectResult{ true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	int connectCalls = 0;
	int exchangeCalls = 0;
	int closeCalls = 0;
	//! Off only to prove that this client checks request correlation itself.
	bool honorRequestId = true;
};

class CScriptedEndpointReader final : public IControlPlatformEndpointReader {
public:
	std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements) override
	{
		++readCalls;
		lastRequirements = requirements;
		return endpoint;
	}

	std::optional<ControlPlatformEndpointSnapshot> endpoint = Endpoint();
	ControlPlatformEndpointReadRequirements lastRequirements;
	int readCalls = 0;
};

class CScriptedChannel final : public IControlPlatformClientChannel {
public:
	explicit CScriptedChannel(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		++m_script->connectCalls;
		return m_script->connectResult;
	}

	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		++m_script->exchangeCalls;
		m_script->requests.push_back(request);
		if (m_script->responses.empty()) {
			return { false, EControlIpcTransportDisconnectReason::IoError, 0, L"script exhausted" };
		}
		responses = std::move(m_script->responses.front());
		m_script->responses.pop_front();
		if (m_script->honorRequestId) {
			for (auto& response : responses) response.header.requestId = request.header.requestId;
		}
		return { true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	}

	void Close() noexcept override { ++m_script->closeCalls; }

private:
	std::shared_ptr<Script> m_script;
};

ControlSenpClientOptions Options(std::shared_ptr<Script> script)
{
	ControlSenpClientOptions options;
	options.profileId = kAuthorityId;
	options.profileHash = kProfileHash;
	options.exchangeDeadline = std::chrono::milliseconds(10);
	options.channelFactory = [script = std::move(script)] {
		return std::make_unique<CScriptedChannel>(script);
	};
	return options;
}

//! The decoded SENP request the client actually put on the wire.
std::optional<ControlSenpRpcRequest> SentRequest(const ControlIpcFrame& frame)
{
	auto fields = DecodeControlIpcFields(frame.payload);
	if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || fields.fields.size() != 1
		|| fields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload)) {
		return std::nullopt;
	}
	return DecodeControlSenpRpcRequest(fields.fields.front().value);
}

} // namespace

TEST(ControlSenpClient, KeepsOneAuthenticatedConnectionForEverySenpOperation)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	script->responses.push_back({ SenpAnswer(Granted()) });
	script->responses.push_back({ SenpAnswer([] {
		ControlSenpRpcResponse response;
		response.status = EControlSenpRpcStatus::Succeeded;
		return response;
	}()) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);

	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	const auto connected = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::Connected, connected.outcome);
	EXPECT_TRUE(connected.IsConnected());
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(kGeneration, client.PinnedGeneration());
	EXPECT_EQ(1u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->connectCalls);

	const auto issued = client.Execute(IssueGrant());
	ASSERT_TRUE(issued.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, issued.response.status);
	EXPECT_EQ("grant-1", issued.response.grantId);

	const auto started = client.Execute(StartRead(issued.response.grantId));
	ASSERT_TRUE(started.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started.response.status);

	// One connection, one hello, and both operations on that same channel.
	EXPECT_EQ(1, script->connectCalls);
	EXPECT_EQ(0, script->closeCalls);
	EXPECT_EQ(3, script->exchangeCalls);
	ASSERT_EQ(3u, script->requests.size());
	EXPECT_EQ(EControlIpcKind::Hello, script->requests[0].header.kind);
	EXPECT_EQ(0u, script->requests[0].header.generation);
	for (std::size_t index = 1; index < script->requests.size(); ++index) {
		const auto& frame = script->requests[index];
		EXPECT_EQ(EControlIpcKind::SenpRequest, frame.header.kind);
		EXPECT_EQ(EControlIpcFlags::Request, frame.header.flags);
		EXPECT_EQ(kGeneration, frame.header.generation);
		EXPECT_NE(0u, frame.header.requestId);
	}
	EXPECT_NE(script->requests[1].header.requestId, script->requests[2].header.requestId);

	const auto sent = SentRequest(script->requests[2]);
	ASSERT_TRUE(sent);
	EXPECT_EQ(EControlSenpRpcOperation::StartRead, sent->operation);
	EXPECT_EQ("grant-1", sent->grantId);
	EXPECT_EQ(std::wstring(L"repositoryRead"), sent->toolOperation);
	EXPECT_EQ(Owner(), sent->owner);

	// A second Connect neither opens a channel nor mints a new connection epoch.
	const auto again = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::AlreadyConnected, again.outcome);
	EXPECT_EQ(1, script->connectCalls);
	EXPECT_EQ(1u, client.ConnectionEpoch());
}

TEST(ControlSenpClient, RefusesEverySenpOperationWithoutAConnectionAndAfterItIsGone)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);

	const auto early = client.Execute(IssueGrant());
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, early.outcome);
	EXPECT_FALSE(early.Answered());
	EXPECT_EQ(0, script->connectCalls);
	EXPECT_EQ(0, reader.readCalls);

	ASSERT_TRUE(client.Connect().IsConnected());
	client.Disconnect();
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.PinnedGeneration());
	EXPECT_EQ(1, script->closeCalls);
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, client.Execute(IssueGrant()).outcome);

	client.Stop();
	EXPECT_EQ(EControlSenpClientState::Stopped, client.State());
	EXPECT_EQ(EControlSenpClientOutcome::Stopped, client.Execute(IssueGrant()).outcome);
	EXPECT_EQ(EControlSenpClientOutcome::Stopped, client.Connect().outcome);
	EXPECT_EQ(1, script->connectCalls);
}

TEST(ControlSenpClient, DropsTheConnectionWhenAnAnswerIsLostSoNoGrantSurvivesUnproven)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// The script is exhausted, so the exchange reports a transport loss.
	const auto lost = client.Execute(IssueGrant());
	EXPECT_EQ(EControlSenpClientOutcome::ConnectionLost, lost.outcome);
	EXPECT_EQ(EControlIpcTransportDisconnectReason::IoError, lost.transportReason);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.PinnedGeneration());
	EXPECT_EQ(1, script->closeCalls);

	// Nothing reconnects behind the caller's back.
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, client.Execute(IssueGrant()).outcome);
	EXPECT_EQ(1, script->connectCalls);
	EXPECT_EQ(1u, client.ConnectionEpoch());
}

TEST(ControlSenpClient, KeepsTheConnectionOnlyForTheTerminalErrorsThePeerKeepsOpen)
{
	struct Case {
		EControlIpcTerminalStatus status;
		bool keepsConnection;
	};
	for (const auto& testCase : { Case{ EControlIpcTerminalStatus::InvalidRequest, true },
			Case{ EControlIpcTerminalStatus::UnsupportedVersion, true },
			Case{ EControlIpcTerminalStatus::AccessDenied, false },
			Case{ EControlIpcTerminalStatus::InternalError, false } }) {
		auto script = std::make_shared<Script>();
		script->responses.push_back({ HelloAck() });
		script->responses.push_back({ TerminalError(testCase.status) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		ASSERT_TRUE(client.Connect().IsConnected());

		const auto refused = client.Execute(IssueGrant());
		EXPECT_FALSE(refused.Answered());
		EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, refused.outcome);
		EXPECT_EQ(testCase.status, refused.terminalStatus);
		EXPECT_EQ(testCase.keepsConnection ? EControlSenpClientState::Connected
			: EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(testCase.keepsConnection ? 0 : 1, script->closeCalls);
	}
}

TEST(ControlSenpClient, TreatsARefusalAsAnAnswerButAGenerationChangeAsALostConnection)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	ControlSenpRpcResponse unauthorized;
	unauthorized.status = EControlSenpRpcStatus::Unauthorized;
	script->responses.push_back({ SenpAnswer(unauthorized) });
	script->responses.push_back({ SenpAnswer(Granted(), kGeneration + 1) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// A refusal is a normal terminal answer: the connection stays authenticated.
	const auto refused = client.Execute(IssueGrant());
	ASSERT_TRUE(refused.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, refused.response.status);
	EXPECT_TRUE(refused.response.grantId.empty());
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(0, script->closeCalls);

	// A moved generation is a different control process state; the answer is refused.
	const auto moved = client.Execute(IssueGrant());
	EXPECT_FALSE(moved.Answered());
	EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, moved.outcome);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(1, script->closeCalls);
}

TEST(ControlSenpClient, RefusesAnAnswerThatDoesNotCorrelateWithTheRequestItSent)
{
	auto script = std::make_shared<Script>();
	script->honorRequestId = false;
	auto uncorrelated = HelloAck();
	uncorrelated.header.requestId = 99;
	script->responses.push_back({ std::move(uncorrelated) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	// Hello itself is correlated, so a fake that never correlates fails there first.
	const auto connected = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, connected.outcome);
	EXPECT_EQ(EControlIpcTerminalStatus::ProtocolError, connected.terminalStatus);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->closeCalls);
}

TEST(ControlSenpClient, RefusesAHelloThatDoesNotProveTheEndpointProfileAndGeneration)
{
	{
		auto script = std::make_shared<Script>();
		script->responses.push_back({ HelloAck(kGeneration, kOtherAuthorityId) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		const auto result = client.Connect();
		EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, result.terminalStatus);
		EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(0u, client.ConnectionEpoch());
	}
	{
		auto script = std::make_shared<Script>();
		script->responses.push_back({ HelloAck(kGeneration + 1) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		const auto result = client.Connect();
		EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, result.outcome);
		EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(0u, client.PinnedGeneration());
	}
}

TEST(ControlSenpClient, NeverPinsAGenerationOlderThanTheOneItAlreadyAuthenticated)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());
	EXPECT_EQ(0u, reader.lastRequirements.minimumGeneration);
	client.Disconnect();

	// A rolled-back control process may not reclaim this client's grants.
	reader.endpoint = Endpoint(kGeneration - 1);
	const auto rolledBack = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, rolledBack.outcome);
	EXPECT_EQ(kGeneration, reader.lastRequirements.minimumGeneration);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(1u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->connectCalls);
}

TEST(ControlSenpClient, RefusesAnIncoherentRequestWithoutTouchingTheConnection)
{
	auto script = std::make_shared<Script>();
	script->responses.push_back({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// An IssueGrant may not carry read members; the payload codec refuses it.
	auto widened = IssueGrant();
	widened.readId = L"issues:open:1";
	const auto refused = client.Execute(widened);
	EXPECT_EQ(EControlSenpClientOutcome::InvalidRequest, refused.outcome);
	EXPECT_EQ(1, script->exchangeCalls);
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(0, script->closeCalls);
}

TEST(ControlSenpClient, ReportsAnUnavailableEndpointWithoutCreatingAChannel)
{
	auto script = std::make_shared<Script>();
	CScriptedEndpointReader reader;
	reader.endpoint = std::nullopt;
	CControlSenpClient client(Options(script), reader);

	const auto unavailable = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::EndpointUnavailable, unavailable.outcome);
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::NotPublished, unavailable.discoveryDisposition);
	EXPECT_EQ(0, script->connectCalls);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());

	reader.endpoint = Endpoint();
	reader.endpoint->profileId = kOtherAuthorityId;
	const auto mismatched = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::EndpointUnavailable, mismatched.outcome);
	EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, mismatched.terminalStatus);
	EXPECT_EQ(0, script->connectCalls);
	EXPECT_EQ(0u, client.ConnectionEpoch());
}

} // namespace platform::controlipc
