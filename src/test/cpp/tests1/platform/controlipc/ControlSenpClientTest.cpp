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
	request.SetOperation(EControlSenpRpcOperation::IssueGrant);
	request.SetProfileId(kProfile);
	request.SetOwner(Owner());
	request.SetCapabilities(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead));
	return request;
}

ControlSenpRpcRequest StartRead(std::string grantId)
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::StartRead);
	request.SetProfileId(kProfile);
	request.SetOwner(Owner());
	request.SetGrantId(std::move(grantId));
	request.SetReadId(L"issues:open:1");
	request.SetToolId(L"github");
	request.SetToolOperation(L"repositoryRead");
	request.SetArguments({ { L"path", L"issues" } });
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
	response.SetStatus(EControlSenpRpcStatus::Succeeded);
	response.SetGrantId(std::move(grantId));
	response.SetExpiresAtMilliseconds(1000);
	return response;
}

//! Drives one fake channel's canned answers and counts what the client did to
//! it. Private state with accessors/mutators for the same reason as every
//! other test fake in this subsystem; the connect/exchange/close behavior
//! moved here from CScriptedChannel so that class stays a thin adapter.
class Script {
public:
	void QueueResponse(std::vector<ControlIpcFrame> frames) { m_responses.push_back(std::move(frames)); }
	void SetConnectResult(ControlIpcTransportResult result) { m_connectResult = std::move(result); }
	//! Off only to prove that this client checks request correlation itself.
	void SetHonorRequestId(bool value) noexcept { m_honorRequestId = value; }

	ControlIpcTransportResult Connect()
	{
		++m_connectCalls;
		return m_connectResult;
	}

	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses)
	{
		++m_exchangeCalls;
		m_requests.push_back(request);
		if (m_responses.empty()) {
			return { false, EControlIpcTransportDisconnectReason::IoError, 0, L"script exhausted" };
		}
		responses = std::move(m_responses.front());
		m_responses.pop_front();
		if (m_honorRequestId) {
			for (auto& response : responses) response.header.requestId = request.header.requestId;
		}
		return { true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	}

	void Close() noexcept { ++m_closeCalls; }

	[[nodiscard]] int ConnectCalls() const noexcept { return m_connectCalls; }
	[[nodiscard]] int ExchangeCalls() const noexcept { return m_exchangeCalls; }
	[[nodiscard]] int CloseCalls() const noexcept { return m_closeCalls; }
	[[nodiscard]] const std::vector<ControlIpcFrame>& Requests() const noexcept { return m_requests; }

private:
	std::deque<std::vector<ControlIpcFrame>> m_responses;
	std::vector<ControlIpcFrame> m_requests;
	ControlIpcTransportResult m_connectResult{ true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	int m_connectCalls = 0;
	int m_exchangeCalls = 0;
	int m_closeCalls = 0;
	bool m_honorRequestId = true;
};

class CScriptedEndpointReader final : public IControlPlatformEndpointReader {
public:
	std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements) override
	{
		++m_readCalls;
		m_lastRequirements = requirements;
		return m_endpoint;
	}

	void SetEndpoint(std::optional<ControlPlatformEndpointSnapshot> value) { m_endpoint = std::move(value); }
	[[nodiscard]] const std::optional<ControlPlatformEndpointSnapshot>& CurrentEndpoint() const noexcept
	{
		return m_endpoint;
	}
	[[nodiscard]] const ControlPlatformEndpointReadRequirements& LastRequirements() const noexcept
	{
		return m_lastRequirements;
	}
	[[nodiscard]] int ReadCalls() const noexcept { return m_readCalls; }

private:
	std::optional<ControlPlatformEndpointSnapshot> m_endpoint = Endpoint();
	ControlPlatformEndpointReadRequirements m_lastRequirements;
	int m_readCalls = 0;
};

class CScriptedChannel final : public IControlPlatformClientChannel {
public:
	explicit CScriptedChannel(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		return m_script->Connect();
	}

	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		return m_script->Exchange(request, responses);
	}

	void Close() noexcept override { m_script->Close(); }

private:
	std::shared_ptr<Script> m_script;
};

ControlSenpClientOptions Options(std::shared_ptr<Script> script)
{
	ControlSenpClientOptions options;
	options.SetProfileId(kAuthorityId);
	options.SetProfileHash(kProfileHash);
	options.SetExchangeDeadline(std::chrono::milliseconds(10));
	options.SetChannelFactory([script = std::move(script)] {
		return std::make_unique<CScriptedChannel>(script);
	});
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
	script->QueueResponse({ HelloAck() });
	script->QueueResponse({ SenpAnswer(Granted()) });
	script->QueueResponse({ SenpAnswer([] {
		ControlSenpRpcResponse response;
		response.SetStatus(EControlSenpRpcStatus::Succeeded);
		return response;
	}()) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);

	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	const auto connected = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::Connected, connected.Outcome());
	EXPECT_TRUE(connected.IsConnected());
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(kGeneration, client.PinnedGeneration());
	EXPECT_EQ(1u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->ConnectCalls());

	const auto issued = client.Execute(IssueGrant());
	ASSERT_TRUE(issued.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, issued.Response().Status());
	EXPECT_EQ("grant-1", issued.Response().GrantId());

	const auto started = client.Execute(StartRead(issued.Response().GrantId()));
	ASSERT_TRUE(started.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started.Response().Status());

	// One connection, one hello, and both operations on that same channel.
	EXPECT_EQ(1, script->ConnectCalls());
	EXPECT_EQ(0, script->CloseCalls());
	EXPECT_EQ(3, script->ExchangeCalls());
	ASSERT_EQ(3u, script->Requests().size());
	EXPECT_EQ(EControlIpcKind::Hello, script->Requests()[0].header.kind);
	EXPECT_EQ(0u, script->Requests()[0].header.generation);
	for (std::size_t index = 1; index < script->Requests().size(); ++index) {
		const auto& frame = script->Requests()[index];
		EXPECT_EQ(EControlIpcKind::SenpRequest, frame.header.kind);
		EXPECT_EQ(EControlIpcFlags::Request, frame.header.flags);
		EXPECT_EQ(kGeneration, frame.header.generation);
		EXPECT_NE(0u, frame.header.requestId);
	}
	EXPECT_NE(script->Requests()[1].header.requestId, script->Requests()[2].header.requestId);

	const auto sent = SentRequest(script->Requests()[2]);
	ASSERT_TRUE(sent);
	EXPECT_EQ(EControlSenpRpcOperation::StartRead, sent->Operation());
	EXPECT_EQ("grant-1", sent->GrantId());
	EXPECT_EQ(std::wstring(L"repositoryRead"), sent->ToolOperation());
	EXPECT_EQ(Owner(), sent->Owner());

	// A second Connect neither opens a channel nor mints a new connection epoch.
	const auto again = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::AlreadyConnected, again.Outcome());
	EXPECT_EQ(1, script->ConnectCalls());
	EXPECT_EQ(1u, client.ConnectionEpoch());
}

TEST(ControlSenpClient, RefusesEverySenpOperationWithoutAConnectionAndAfterItIsGone)
{
	auto script = std::make_shared<Script>();
	script->QueueResponse({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);

	const auto early = client.Execute(IssueGrant());
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, early.Outcome());
	EXPECT_FALSE(early.Answered());
	EXPECT_EQ(0, script->ConnectCalls());
	EXPECT_EQ(0, reader.ReadCalls());

	ASSERT_TRUE(client.Connect().IsConnected());
	client.Disconnect();
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.PinnedGeneration());
	EXPECT_EQ(1, script->CloseCalls());
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, client.Execute(IssueGrant()).Outcome());

	client.Stop();
	EXPECT_EQ(EControlSenpClientState::Stopped, client.State());
	EXPECT_EQ(EControlSenpClientOutcome::Stopped, client.Execute(IssueGrant()).Outcome());
	EXPECT_EQ(EControlSenpClientOutcome::Stopped, client.Connect().Outcome());
	EXPECT_EQ(1, script->ConnectCalls());
}

TEST(ControlSenpClient, DropsTheConnectionWhenAnAnswerIsLostSoNoGrantSurvivesUnproven)
{
	auto script = std::make_shared<Script>();
	script->QueueResponse({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// The script is exhausted, so the exchange reports a transport loss.
	const auto lost = client.Execute(IssueGrant());
	EXPECT_EQ(EControlSenpClientOutcome::ConnectionLost, lost.Outcome());
	EXPECT_EQ(EControlIpcTransportDisconnectReason::IoError, lost.TransportReason());
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.PinnedGeneration());
	EXPECT_EQ(1, script->CloseCalls());

	// Nothing reconnects behind the caller's back.
	EXPECT_EQ(EControlSenpClientOutcome::NotConnected, client.Execute(IssueGrant()).Outcome());
	EXPECT_EQ(1, script->ConnectCalls());
	EXPECT_EQ(1u, client.ConnectionEpoch());
}

TEST(ControlSenpClient, KeepsTheConnectionOnlyForTheTerminalErrorsThePeerKeepsOpen)
{
	// Private fields with a constructor + accessors, matching every other DTO
	// in this subsystem, rather than a plain-field local struct.
	class Case {
	public:
		Case(EControlIpcTerminalStatus status, bool keepsConnection) :
			m_status(status), m_keepsConnection(keepsConnection)
		{
		}

		[[nodiscard]] EControlIpcTerminalStatus Status() const noexcept { return m_status; }
		[[nodiscard]] bool KeepsConnection() const noexcept { return m_keepsConnection; }

	private:
		EControlIpcTerminalStatus m_status;
		bool m_keepsConnection;
	};
	for (const auto& testCase : { Case{ EControlIpcTerminalStatus::InvalidRequest, true },
			Case{ EControlIpcTerminalStatus::UnsupportedVersion, true },
			Case{ EControlIpcTerminalStatus::AccessDenied, false },
			Case{ EControlIpcTerminalStatus::InternalError, false } }) {
		auto script = std::make_shared<Script>();
		script->QueueResponse({ HelloAck() });
		script->QueueResponse({ TerminalError(testCase.Status()) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		ASSERT_TRUE(client.Connect().IsConnected());

		const auto refused = client.Execute(IssueGrant());
		EXPECT_FALSE(refused.Answered());
		EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, refused.Outcome());
		EXPECT_EQ(testCase.Status(), refused.TerminalStatus());
		EXPECT_EQ(testCase.KeepsConnection() ? EControlSenpClientState::Connected
			: EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(testCase.KeepsConnection() ? 0 : 1, script->CloseCalls());
	}
}

TEST(ControlSenpClient, TreatsARefusalAsAnAnswerButAGenerationChangeAsALostConnection)
{
	auto script = std::make_shared<Script>();
	script->QueueResponse({ HelloAck() });
	ControlSenpRpcResponse unauthorized;
	unauthorized.SetStatus(EControlSenpRpcStatus::Unauthorized);
	script->QueueResponse({ SenpAnswer(unauthorized) });
	script->QueueResponse({ SenpAnswer(Granted(), kGeneration + 1) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// A refusal is a normal terminal answer: the connection stays authenticated.
	const auto refused = client.Execute(IssueGrant());
	ASSERT_TRUE(refused.Answered());
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, refused.Response().Status());
	EXPECT_TRUE(refused.Response().GrantId().empty());
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(0, script->CloseCalls());

	// A moved generation is a different control process state; the answer is refused.
	const auto moved = client.Execute(IssueGrant());
	EXPECT_FALSE(moved.Answered());
	EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, moved.Outcome());
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(1, script->CloseCalls());
}

TEST(ControlSenpClient, RefusesAnAnswerThatDoesNotCorrelateWithTheRequestItSent)
{
	auto script = std::make_shared<Script>();
	script->SetHonorRequestId(false);
	auto uncorrelated = HelloAck();
	uncorrelated.header.requestId = 99;
	script->QueueResponse({ std::move(uncorrelated) });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	// Hello itself is correlated, so a fake that never correlates fails there first.
	const auto connected = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, connected.Outcome());
	EXPECT_EQ(EControlIpcTerminalStatus::ProtocolError, connected.TerminalStatus());
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(0u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->CloseCalls());
}

TEST(ControlSenpClient, RefusesAHelloThatDoesNotProveTheEndpointProfileAndGeneration)
{
	{
		auto script = std::make_shared<Script>();
		script->QueueResponse({ HelloAck(kGeneration, kOtherAuthorityId) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		const auto result = client.Connect();
		EXPECT_EQ(EControlSenpClientOutcome::ProtocolError, result.Outcome());
		EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, result.TerminalStatus());
		EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(0u, client.ConnectionEpoch());
	}
	{
		auto script = std::make_shared<Script>();
		script->QueueResponse({ HelloAck(kGeneration + 1) });
		CScriptedEndpointReader reader;
		CControlSenpClient client(Options(script), reader);
		const auto result = client.Connect();
		EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, result.Outcome());
		EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
		EXPECT_EQ(0u, client.PinnedGeneration());
	}
}

TEST(ControlSenpClient, NeverPinsAGenerationOlderThanTheOneItAlreadyAuthenticated)
{
	auto script = std::make_shared<Script>();
	script->QueueResponse({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());
	EXPECT_EQ(0u, reader.LastRequirements().minimumGeneration);
	client.Disconnect();

	// A rolled-back control process may not reclaim this client's grants.
	reader.SetEndpoint(Endpoint(kGeneration - 1));
	const auto rolledBack = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::GenerationChanged, rolledBack.Outcome());
	EXPECT_EQ(kGeneration, reader.LastRequirements().minimumGeneration);
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());
	EXPECT_EQ(1u, client.ConnectionEpoch());
	EXPECT_EQ(1, script->ConnectCalls());
}

TEST(ControlSenpClient, RefusesAnIncoherentRequestWithoutTouchingTheConnection)
{
	auto script = std::make_shared<Script>();
	script->QueueResponse({ HelloAck() });
	CScriptedEndpointReader reader;
	CControlSenpClient client(Options(script), reader);
	ASSERT_TRUE(client.Connect().IsConnected());

	// An IssueGrant may not carry read members; the payload codec refuses it.
	auto widened = IssueGrant();
	widened.SetReadId(L"issues:open:1");
	const auto refused = client.Execute(widened);
	EXPECT_EQ(EControlSenpClientOutcome::InvalidRequest, refused.Outcome());
	EXPECT_EQ(1, script->ExchangeCalls());
	EXPECT_EQ(EControlSenpClientState::Connected, client.State());
	EXPECT_EQ(0, script->CloseCalls());
}

TEST(ControlSenpClient, ReportsAnUnavailableEndpointWithoutCreatingAChannel)
{
	auto script = std::make_shared<Script>();
	CScriptedEndpointReader reader;
	reader.SetEndpoint(std::nullopt);
	CControlSenpClient client(Options(script), reader);

	const auto unavailable = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::EndpointUnavailable, unavailable.Outcome());
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::NotPublished, unavailable.DiscoveryDisposition());
	EXPECT_EQ(0, script->ConnectCalls());
	EXPECT_EQ(EControlSenpClientState::Disconnected, client.State());

	auto mismatchedEndpoint = Endpoint();
	mismatchedEndpoint.profileId = kOtherAuthorityId;
	reader.SetEndpoint(mismatchedEndpoint);
	const auto mismatched = client.Connect();
	EXPECT_EQ(EControlSenpClientOutcome::EndpointUnavailable, mismatched.Outcome());
	EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, mismatched.TerminalStatus());
	EXPECT_EQ(0, script->ConnectCalls());
	EXPECT_EQ(0u, client.ConnectionEpoch());
}

} // namespace platform::controlipc
