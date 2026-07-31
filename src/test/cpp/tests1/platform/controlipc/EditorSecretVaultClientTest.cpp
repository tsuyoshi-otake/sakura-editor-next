/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include "platform/controlipc/EditorSecretVaultClient.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kGeneration = 47;
constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

ControlPlatformEndpointSnapshot Endpoint()
{
	return { GetCurrentProcessId(), kGeneration, ControlPlatformEndpointLifecycle::Accepting,
		kProfileHash, BuildControlPipeName(kProfileHash), std::string(kProfileId) };
}

ControlIpcFrame Reply(const ControlIpcFrame& request, EControlIpcKind kind, std::vector<std::uint8_t> payload)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, kGeneration }, std::move(payload) };
}

class CFakeEndpointReader final : public IEditorSecretVaultEndpointReader {
public:
	ControlPlatformEndpointDiscoveryResult Read(const ControlPlatformEndpointReadRequirements&) override
	{
		++readCount;
		return response;
	}
	void Close() noexcept override { closed = true; }

	ControlPlatformEndpointDiscoveryResult response{ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(), 0, {} };
	int readCount = 0;
	bool closed = false;
};

class CFakeChannel final : public IEditorSecretVaultChannel {
public:
	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		++connectCount;
		return connect;
	}
	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		seen.push_back(request);
		if (onExchange) return onExchange(request, responses, *this);
		responses = { SuccessfulReply(request) };
		return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
	}
	void Close() noexcept override
	{
		closed = true;
		++closeCount;
		if (onClose) onClose();
	}

	static ControlIpcFrame SuccessfulReply(const ControlIpcFrame& request)
	{
		if (request.header.kind == EControlIpcKind::Hello) return Reply(request, EControlIpcKind::HelloAck,
			*EncodeControlStorageHello(kProfileId));
		if (request.header.kind == EControlIpcKind::SecretCapabilityIssueRequest) {
			ControlSecretVaultCapabilityIssueResponse issued{};
			issued.capability.fill(0xA5);
			issued.lifetime = std::chrono::seconds(30);
			return Reply(request, EControlIpcKind::SecretCapabilityIssueResponse,
				*EncodeControlSecretVaultCapabilityIssueResponse(issued));
		}
		if (request.header.kind == EControlIpcKind::SecretGetRequest) return Reply(request, EControlIpcKind::SecretGetResponse,
			*EncodeControlSecretVaultGetResponse({ secrets::ESecretGetStatus::Found, 9, std::string("value") }));
		const auto applied = DecodeControlSecretVaultApplyRequest(request.payload);
		if (!applied) return Reply(request, EControlIpcKind::SecretApplyResponse, {});
		secrets::SecretChange change{
			std::string(kProfileId),
			{ applied->mutation.extensionId, applied->mutation.key },
			applied->mutation.kind == secrets::ESecretMutationKind::Set
				? secrets::ESecretChangeKind::Set : secrets::ESecretChangeKind::Delete,
			10,
		};
		const auto encoded = EncodeControlSecretVaultApplyResponse(
			{ secrets::ESecretMutationStatus::Succeeded, 10, false, std::move(change), {} });
		return Reply(request, EControlIpcKind::SecretApplyResponse, encoded.value_or(std::vector<std::uint8_t>{}));
	}

	ControlIpcTransportResult connect{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
	std::function<ControlIpcTransportResult(const ControlIpcFrame&, std::vector<ControlIpcFrame>&, CFakeChannel&)> onExchange;
	std::function<void()> onClose;
	std::vector<ControlIpcFrame> seen;
	int connectCount = 0;
	int closeCount = 0;
	bool closed = false;
};

EditorSecretVaultCallerIdentity Caller()
{
	return { "host-session", 3, "publisher.one" };
}

EditorSecretVaultClientOptions Options(std::function<std::unique_ptr<IEditorSecretVaultChannel>()> factory)
{
	return { std::string(kProfileId), kProfileHash, kGeneration, std::chrono::seconds(1), std::chrono::seconds(30), std::move(factory) };
}

std::function<std::unique_ptr<IEditorSecretVaultChannel>()> OneShotChannelFactory(
	std::unique_ptr<IEditorSecretVaultChannel> channel)
{
	auto slot = std::make_shared<std::unique_ptr<IEditorSecretVaultChannel>>(std::move(channel));
	return [slot] { return std::move(*slot); };
}

EditorSecretVaultApplyRequest StoreRequest()
{
	return { Caller(), { secrets::ESecretMutationKind::Set, "publisher.one", "key", "value", "operation-1", 9 } };
}

EditorSecretVaultApplyRequest DeleteRequest()
{
	return { Caller(), { secrets::ESecretMutationKind::Delete, "publisher.one", "key", {}, "operation-1", 9 } };
}

TEST(EditorSecretVaultClientTest, GetUsesFreshDiscoveryHelloIssueAndGet)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	std::vector<EControlIpcKind> seenKinds;
	bool closed = false;
	channel->onExchange = [&seenKinds](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
		seenKinds.emplace_back(request.header.kind);
		responses = { CFakeChannel::SuccessfulReply(request) };
		return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
	};
	channel->onClose = [&closed] { closed = true; };
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);

	const auto result = client.Get({ Caller(), "key" });

	EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, result.outcome);
	ASSERT_TRUE(result.result.value);
	EXPECT_EQ("value", *result.result.value);
	ASSERT_EQ(3u, seenKinds.size());
	EXPECT_EQ(EControlIpcKind::Hello, seenKinds[0]);
	EXPECT_EQ(EControlIpcKind::SecretCapabilityIssueRequest, seenKinds[1]);
	EXPECT_EQ(EControlIpcKind::SecretGetRequest, seenKinds[2]);
	EXPECT_TRUE(closed);
}

TEST(EditorSecretVaultClientTest, StoreAndDeleteUseCallerOperationIdWithoutListing)
{
	CFakeEndpointReader reader;
	std::vector<ControlIpcFrame> seen;
	int closeCount = 0;
	int factoryCalls = 0;
	CEditorSecretVaultClient client(Options([&factoryCalls, &seen, &closeCount] {
		++factoryCalls;
		auto channel = std::make_unique<CFakeChannel>();
		channel->onExchange = [&seen](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
			seen.emplace_back(request);
			responses = { CFakeChannel::SuccessfulReply(request) };
			return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
		};
		channel->onClose = [&closeCount] { ++closeCount; };
		return std::unique_ptr<IEditorSecretVaultChannel>(std::move(channel));
	}), reader);
	const auto store = StoreRequest();
	const auto remove = DeleteRequest();

	EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, client.Store(store).outcome);
	EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, client.Delete(remove).outcome);
	EXPECT_EQ(2, reader.readCount);
	EXPECT_EQ(2, factoryCalls);
	EXPECT_EQ(2, closeCount);
	ASSERT_EQ(6u, seen.size());
	EXPECT_EQ(EControlIpcKind::Hello, seen[0].header.kind);
	EXPECT_EQ(EControlIpcKind::SecretCapabilityIssueRequest, seen[1].header.kind);
	EXPECT_EQ(EControlIpcKind::SecretApplyRequest, seen[2].header.kind);
	EXPECT_EQ(EControlIpcKind::Hello, seen[3].header.kind);
	EXPECT_EQ(EControlIpcKind::SecretCapabilityIssueRequest, seen[4].header.kind);
	EXPECT_EQ(EControlIpcKind::SecretApplyRequest, seen[5].header.kind);

	const auto stored = DecodeControlSecretVaultApplyRequest(seen[2].payload);
	ASSERT_TRUE(stored);
	EXPECT_EQ(store.mutation.kind, stored->mutation.kind);
	EXPECT_EQ(store.mutation.operationId, stored->mutation.operationId);
	const auto deleted = DecodeControlSecretVaultApplyRequest(seen[5].payload);
	ASSERT_TRUE(deleted);
	EXPECT_EQ(remove.mutation.kind, deleted->mutation.kind);
	EXPECT_EQ(remove.mutation.operationId, deleted->mutation.operationId);
}

TEST(EditorSecretVaultClientTest, MutationConflictIsExplicitAndDoesNotRetry)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	auto* raw = channel.get();
	raw->onExchange = [](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
		if (request.header.kind == EControlIpcKind::SecretApplyRequest) {
			responses = { Reply(request, EControlIpcKind::SecretApplyResponse,
				*EncodeControlSecretVaultApplyResponse({ secrets::ESecretMutationStatus::Conflict, 9, false, std::nullopt, {} })) };
		}
		else responses = { CFakeChannel::SuccessfulReply(request) };
		return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
	};
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);

	const auto result = client.Store(StoreRequest());

	EXPECT_EQ(EEditorSecretVaultOutcome::Conflict, result.outcome);
	EXPECT_EQ(secrets::ESecretMutationStatus::Conflict, result.result.status);
}

TEST(EditorSecretVaultClientTest, ApplyTransportLossRequiresSameOperationId)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	auto* raw = channel.get();
	raw->onExchange = [](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
		if (request.header.kind == EControlIpcKind::SecretApplyRequest) return ControlIpcTransportResult{
			false, EControlIpcTransportDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE, {} };
		responses = { CFakeChannel::SuccessfulReply(request) };
		return ControlIpcTransportResult{
			true, EControlIpcTransportDisconnectReason::None, 0, {} };
	};
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
	const auto request = StoreRequest();

	const auto result = client.Store(request);

	EXPECT_EQ(EEditorSecretVaultOutcome::RetryWithSameOperationId, result.outcome);
	EXPECT_EQ("operation-1", request.mutation.operationId);
}

TEST(EditorSecretVaultClientTest, RejectsMalformedCallerAndProfileOrGenerationMismatchBeforeConnect)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	auto* raw = channel.get();
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
	auto bad = StoreRequest();
	bad.caller.canonicalExtensionId = "Publisher.One";
	EXPECT_EQ(EEditorSecretVaultOutcome::Failed, client.Store(bad).outcome);
	EXPECT_EQ(0, raw->connectCount);

	CFakeEndpointReader mismatchedReader;
	mismatchedReader.response.snapshot->generation = kGeneration + 1;
	auto second = std::make_unique<CFakeChannel>();
	auto* secondRaw = second.get();
	CEditorSecretVaultClient mismatched(Options(OneShotChannelFactory(std::move(second))), mismatchedReader);
	EXPECT_EQ(EEditorSecretVaultOutcome::Unavailable, mismatched.Store(StoreRequest()).outcome);
	EXPECT_EQ(0, secondRaw->connectCount);
}

TEST(EditorSecretVaultClientTest, RejectsMalformedErrorAndWrongGenerationTerminalResponses)
{
	{
		CFakeEndpointReader reader;
		auto channel = std::make_unique<CFakeChannel>();
		channel->onExchange = [](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
			responses = { CFakeChannel::SuccessfulReply(request), CFakeChannel::SuccessfulReply(request) };
			return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
		};
		CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
		EXPECT_EQ(EEditorSecretVaultOutcome::Failed, client.Get({ Caller(), "key" }).outcome);
	}
	{
		CFakeEndpointReader reader;
		auto channel = std::make_unique<CFakeChannel>();
		channel->onExchange = [](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
			if (request.header.kind == EControlIpcKind::Hello) {
				responses = { Reply(request, EControlIpcKind::Error,
					*EncodeControlIpcError({ EControlIpcTerminalStatus::AccessDenied, "denied" })) };
			}
			else responses = { CFakeChannel::SuccessfulReply(request) };
			return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
		};
		CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
		const auto result = client.Get({ Caller(), "key" });
		EXPECT_EQ(EEditorSecretVaultOutcome::Failed, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::AccessDenied, result.terminalStatus);
	}
	{
		CFakeEndpointReader reader;
		auto channel = std::make_unique<CFakeChannel>();
		channel->onExchange = [](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
			if (request.header.kind == EControlIpcKind::Hello) {
				auto response = CFakeChannel::SuccessfulReply(request);
				response.header.generation = kGeneration + 1;
				responses = { std::move(response) };
			}
			else responses = { CFakeChannel::SuccessfulReply(request) };
			return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
		};
		CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
		const auto result = client.Get({ Caller(), "key" });
		EXPECT_EQ(EEditorSecretVaultOutcome::Failed, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::GenerationMismatch, result.terminalStatus);
	}
}

TEST(EditorSecretVaultClientTest, SerializesOneExchangeAtATime)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	CEditorSecretVaultClient* clientAddress = nullptr;
	EEditorSecretVaultOutcome nestedOutcome = EEditorSecretVaultOutcome::Failed;
	channel->onExchange = [&clientAddress, &nestedOutcome](const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
		if (request.header.kind == EControlIpcKind::Hello) nestedOutcome = clientAddress->Get({ Caller(), "key" }).outcome;
		responses = { CFakeChannel::SuccessfulReply(request) };
		return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
	};
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
	clientAddress = &client;

	EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, client.Get({ Caller(), "key" }).outcome);
	EXPECT_EQ(EEditorSecretVaultOutcome::OperationInFlight, nestedOutcome);
}

TEST(EditorSecretVaultClientTest, StopIsTerminalAndClosesActiveChannel)
{
	CFakeEndpointReader reader;
	auto channel = std::make_unique<CFakeChannel>();
	auto* raw = channel.get();
	bool closed = false;
	raw->onClose = [&closed] { closed = true; };
	CEditorSecretVaultClient* clientAddress = nullptr;
	raw->onExchange = [&clientAddress](const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses, CFakeChannel&) {
		if (request.header.kind == EControlIpcKind::SecretApplyRequest) {
			clientAddress->Stop();
			return ControlIpcTransportResult{ false, EControlIpcTransportDisconnectReason::Stopped, ERROR_OPERATION_ABORTED, {} };
		}
		responses = { CFakeChannel::SuccessfulReply(request) };
		return ControlIpcTransportResult{ true, EControlIpcTransportDisconnectReason::None, 0, {} };
	};
	CEditorSecretVaultClient client(Options(OneShotChannelFactory(std::move(channel))), reader);
	clientAddress = &client;

	EXPECT_EQ(EEditorSecretVaultOutcome::Stopped, client.Store(StoreRequest()).outcome);
	EXPECT_TRUE(closed);
	EXPECT_TRUE(reader.closed);
	EXPECT_EQ(EEditorSecretVaultOutcome::Stopped, client.Delete(DeleteRequest()).outcome);
}

} // namespace
} // namespace platform::controlipc
