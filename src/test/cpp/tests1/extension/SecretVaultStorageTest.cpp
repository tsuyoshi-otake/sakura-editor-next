/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionSecretVaultStorage.h"
#include "extension/CExtensionWorkbenchDispatcher.h"
#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/controlipc/ControlStorageRpc.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace platform::controlipc;

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
		return { EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(), 0, {} };
	}
	void Close() noexcept override { closed = true; }
	bool closed = false;
};

class CFakeChannel final : public IEditorSecretVaultChannel {
public:
	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
	}
	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		seen.push_back(request);
		if (observed) observed->push_back(request);
		if (lostApplyResponse && request.header.kind == EControlIpcKind::SecretApplyRequest) {
			lostApplyResponse = false;
			return { false, EControlIpcTransportDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE, {} };
		}
		if (wrongRevokeTerminal && request.header.kind == EControlIpcKind::SecretCapabilityRevokeSessionRequest) {
			auto response = Reply(request, EControlIpcKind::SecretCapabilityRevokeSessionResponse,
				*EncodeControlSecretVaultCapabilityRevokeSessionResponse());
			++response.header.requestId;
			responses = { std::move(response) };
			return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
		}
		responses = { SuccessfulReply(request) };
		return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
	}
	void Close() noexcept override { closed = true; }

	static ControlIpcFrame SuccessfulReply(const ControlIpcFrame& request)
	{
		if (request.header.kind == EControlIpcKind::Hello) {
			return Reply(request, EControlIpcKind::HelloAck, *EncodeControlStorageHello(kProfileId));
		}
		if (request.header.kind == EControlIpcKind::SecretCapabilityIssueRequest) {
			ControlSecretVaultCapabilityIssueResponse issued{};
			issued.capability.fill(0xA5);
			issued.lifetime = std::chrono::seconds(30);
			return Reply(request, EControlIpcKind::SecretCapabilityIssueResponse,
				*EncodeControlSecretVaultCapabilityIssueResponse(issued));
		}
		if (request.header.kind == EControlIpcKind::SecretGetRequest) {
			return Reply(request, EControlIpcKind::SecretGetResponse,
				*EncodeControlSecretVaultGetResponse({ platform::secrets::ESecretGetStatus::NotFound, 7, std::nullopt }));
		}
		if (request.header.kind == EControlIpcKind::SecretCapabilityRevokeSessionRequest) {
			return Reply(request, EControlIpcKind::SecretCapabilityRevokeSessionResponse,
				*EncodeControlSecretVaultCapabilityRevokeSessionResponse());
		}
		const auto applied = DecodeControlSecretVaultApplyRequest(request.payload);
		if (!applied) return Reply(request, EControlIpcKind::SecretApplyResponse, {});
		platform::secrets::SecretChange change{
			std::string(kProfileId),
			{ applied->mutation.extensionId, applied->mutation.key },
			applied->mutation.kind == platform::secrets::ESecretMutationKind::Set
				? platform::secrets::ESecretChangeKind::Set : platform::secrets::ESecretChangeKind::Delete,
			8,
		};
		const auto encoded = EncodeControlSecretVaultApplyResponse(
			{ platform::secrets::ESecretMutationStatus::Succeeded, 8, false, std::move(change), {} });
		return Reply(request, EControlIpcKind::SecretApplyResponse, encoded.value_or(std::vector<std::uint8_t>{}));
	}

	bool lostApplyResponse = false;
	bool wrongRevokeTerminal = false;
	bool closed = false;
	std::vector<ControlIpcFrame>* observed = nullptr;
	std::vector<ControlIpcFrame> seen;
};

EditorSecretVaultCallerIdentity Caller()
{
	return { "host-session", 3, "publisher.one" };
}

EditorSecretVaultClientOptions Options(std::function<std::unique_ptr<IEditorSecretVaultChannel>()> factory)
{
	return { std::string(kProfileId), kProfileHash, kGeneration, std::chrono::seconds(1), std::chrono::seconds(30), std::move(factory) };
}

EditorSecretVaultApplyRequest StoreRequest(std::string value = {})
{
	return { Caller(), { platform::secrets::ESecretMutationKind::Set, "publisher.one", "key", std::move(value), "operation-1", 7 } };
}

class CCountingSecretStorage final : public IExtensionSecretStorage {
public:
	SExtensionSecretStorageResult Store(std::wstring_view, std::wstring_view, std::wstring_view) override
	{
		++storeCalls;
		if (fail) return Failure();
		return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
	}
	SExtensionSecretReadResult Get(std::wstring_view, std::wstring_view) override
	{
		++getCalls;
		if (fail) {
			SExtensionSecretReadResult result;
			static_cast<SExtensionSecretStorageResult&>(result) = Failure();
			return result;
		}
		return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {}, std::nullopt };
	}
	SExtensionSecretStorageResult Delete(std::wstring_view, std::wstring_view) override
	{
		++deleteCalls;
		if (fail) return Failure();
		return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
	}
	static SExtensionSecretStorageResult Failure()
	{
		// The dispatcher must redact even a backend diagnostic that violates its
		// own no-secret diagnostic contract.
		return { false, EExtensionSecretStorageStatus::IoError, ERROR_ACCESS_DENIED,
			L"backend accidentally echoed sentinel-secret" };
	}
	bool fail = false;
	int storeCalls = 0;
	int getCalls = 0;
	int deleteCalls = 0;
};

class CRecordingVaultClient final : public IEditorSecretVaultClient {
public:
	EditorSecretVaultGetResult Get(const EditorSecretVaultGetRequest& request) override
	{
		getRequests.push_back(request);
		return { EEditorSecretVaultOutcome::NotFound, EControlIpcTerminalStatus::Succeeded, {}, {},
			{ platform::secrets::ESecretGetStatus::NotFound, 4, std::nullopt } };
	}
	EditorSecretVaultApplyResult Store(const EditorSecretVaultApplyRequest& request) override
	{
		storeRequests.push_back(request);
		return ApplyResult(storeRequests.size() == 1 && loseFirstStoreResponse
			? EEditorSecretVaultOutcome::RetryWithSameOperationId : EEditorSecretVaultOutcome::Succeeded, request);
	}
	EditorSecretVaultApplyResult Delete(const EditorSecretVaultApplyRequest& request) override
	{
		deleteRequests.push_back(request);
		return ApplyResult(EEditorSecretVaultOutcome::Succeeded, request);
	}
	EditorSecretVaultRevokeResult RevokeSession() override
	{
		++revokeCalls;
		return { revokeOutcome, EControlIpcTerminalStatus::Succeeded };
	}
	void Stop() noexcept override { stopped = true; }
	bool IsStopped() const noexcept override { return stopped; }

	static EditorSecretVaultApplyResult ApplyResult(
		EEditorSecretVaultOutcome outcome,
		const EditorSecretVaultApplyRequest& request)
	{
		std::optional<platform::secrets::SecretChange> change;
		if (outcome == EEditorSecretVaultOutcome::Succeeded) {
			change = platform::secrets::SecretChange{
				std::string(kProfileId),
				{ request.mutation.extensionId, request.mutation.key },
				request.mutation.kind == platform::secrets::ESecretMutationKind::Set
					? platform::secrets::ESecretChangeKind::Set : platform::secrets::ESecretChangeKind::Delete,
				5,
			};
		}
		return { outcome, EControlIpcTerminalStatus::Succeeded, {}, {},
			{ outcome == EEditorSecretVaultOutcome::Succeeded ? platform::secrets::ESecretMutationStatus::Succeeded
				: platform::secrets::ESecretMutationStatus::Failed, 5, false, std::move(change), {} } };
	}
	bool loseFirstStoreResponse = false;
	bool stopped = false;
	int revokeCalls = 0;
	EEditorSecretVaultOutcome revokeOutcome = EEditorSecretVaultOutcome::Succeeded;
	std::vector<EditorSecretVaultGetRequest> getRequests;
	std::vector<EditorSecretVaultApplyRequest> storeRequests;
	std::vector<EditorSecretVaultApplyRequest> deleteRequests;
};

TEST(CExtensionWorkbenchDispatcherSecretTest, OnlyExactSecretRequestsReachStorageAndKeysIsExplicitlyUnsupported)
{
	CCountingSecretStorage secrets;
	CExtensionContextKeys context;
	CExtensionCommandPalette commands;
	CExtensionStatusBar status;
	CExtensionNotificationCenter notifications;
	CExtensionViewRegistry views;
	CExtensionDiagnostics diagnostics;
	CExtensionQuickInput quickInput;
	CExtensionOutputChannel output;
	CExtensionProgressCenter progress;
	CExtensionWorkbenchDispatcher dispatcher(context, commands, status, notifications, views, secrets,
		diagnostics, quickInput, output, progress);
	const auto dispatch = [&dispatcher](std::string method, EExtensionRpcMessageKind kind) {
		return dispatcher.Dispatch({
			.eKind = kind,
			.sMethod = std::move(method),
			.sParamsJson = R"({"extensionId":"publisher.one","key":"key","value":""})",
		});
	};

	EXPECT_TRUE(dispatch("secrets/get", EExtensionRpcMessageKind::Request).success);
	EXPECT_TRUE(dispatch("secrets/store", EExtensionRpcMessageKind::Request).success);
	EXPECT_TRUE(dispatch("secrets/delete", EExtensionRpcMessageKind::Request).success);
	EXPECT_EQ(1, secrets.getCalls);
	EXPECT_EQ(1, secrets.storeCalls);
	EXPECT_EQ(1, secrets.deleteCalls);

	const auto keys = dispatch("secrets/keys", EExtensionRpcMessageKind::Request);
	EXPECT_FALSE(keys.success);
	EXPECT_EQ(-32601, keys.errorCode);
	EXPECT_NE(std::string::npos, keys.errorMessage.find("UnsupportedCapability"));
	for (const auto method : std::array<std::string_view, 6>{
		"secrets/get", "secrets/store", "secrets/delete", "secrets/keys", "secrets/get/suffix", "secrets/unknown" }) {
		(void)dispatch(std::string(method), EExtensionRpcMessageKind::Notification);
	}
	(void)dispatch("secrets/get/suffix", EExtensionRpcMessageKind::Request);
	(void)dispatch("secrets/unknown", EExtensionRpcMessageKind::Request);
	EXPECT_EQ(1, secrets.getCalls);
	EXPECT_EQ(1, secrets.storeCalls);
	EXPECT_EQ(1, secrets.deleteCalls);

	secrets.fail = true;
	const auto failed = dispatcher.Dispatch({
		.eKind = EExtensionRpcMessageKind::Request,
		.sMethod = "secrets/store",
		.sParamsJson = R"({"extensionId":"publisher.one","key":"token","value":"sentinel-secret"})",
	});
	EXPECT_FALSE(failed.success);
	EXPECT_EQ(-32020, failed.errorCode);
	EXPECT_EQ(std::string::npos, failed.errorMessage.find("sentinel-secret"));
	EXPECT_EQ(std::string::npos, failed.resultJson.find("sentinel-secret"));
}

TEST(CEditorSecretVaultClientContractTest, EmptyStoreAndLostResponseReplayPreserveTheExactOperationId)
{
	CFakeEndpointReader reader;
	std::vector<ControlIpcFrame> observed;
	int factoryCalls = 0;
	CEditorSecretVaultClient client(Options([&] {
		auto channel = std::make_unique<CFakeChannel>();
		channel->lostApplyResponse = factoryCalls++ == 0;
		channel->observed = &observed;
		return std::unique_ptr<IEditorSecretVaultChannel>(std::move(channel));
	}), reader);

	const auto request = StoreRequest();
	const auto first = client.Store(request);
	EXPECT_EQ(EEditorSecretVaultOutcome::RetryWithSameOperationId, first.outcome);
	const auto replay = client.Store(request);
	EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, replay.outcome);
	ASSERT_EQ(2, factoryCalls);
	std::vector<platform::secrets::SecretMutationRequest> mutations;
	for (const auto& frame : observed) {
		if (frame.header.kind != EControlIpcKind::SecretApplyRequest) continue;
		const auto decoded = DecodeControlSecretVaultApplyRequest(frame.payload);
		ASSERT_TRUE(decoded.has_value());
		mutations.push_back(decoded->mutation);
	}
	ASSERT_EQ(2u, mutations.size());
	EXPECT_TRUE(mutations[0].value.empty());
	EXPECT_EQ("operation-1", mutations[0].operationId);
	EXPECT_EQ(mutations[0].operationId, mutations[1].operationId);
}

TEST(CEditorSecretVaultClientContractTest, RevokeSessionRequiresAnExactTerminalResponse)
{
	{
		CFakeEndpointReader reader;
		CEditorSecretVaultClient client(Options([] {
			return std::unique_ptr<IEditorSecretVaultChannel>(std::make_unique<CFakeChannel>());
		}), reader);
		const auto result = client.RevokeSession();
		EXPECT_EQ(EEditorSecretVaultOutcome::Succeeded, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::Succeeded, result.terminalStatus);
	}
	{
	CFakeEndpointReader reader;
	CEditorSecretVaultClient client(Options([] {
		auto channel = std::make_unique<CFakeChannel>();
		channel->wrongRevokeTerminal = true;
		return std::unique_ptr<IEditorSecretVaultChannel>(std::move(channel));
	}), reader);

	const auto result = client.RevokeSession();
	EXPECT_EQ(EEditorSecretVaultOutcome::Failed, result.outcome);
	EXPECT_EQ(EControlIpcTerminalStatus::ProtocolError, result.terminalStatus);
	}
}

TEST(CExtensionSecretVaultStorageTest, EmptyStoreReplaysExactlyOnceAndClearSessionMapsTerminalResult)
{
	auto fake = std::make_unique<CRecordingVaultClient>();
	auto* raw = fake.get();
	raw->loseFirstStoreResponse = true;
	CExtensionSecretVaultStorage storage(std::move(fake));
	ASSERT_TRUE(storage.BindSession("host-session", 3).success);
	EXPECT_TRUE(storage.Store(L"Publisher.One", L"key", L"").success);
	ASSERT_EQ(1u, raw->getRequests.size());
	ASSERT_EQ(2u, raw->storeRequests.size());
	EXPECT_TRUE(raw->storeRequests[0].mutation.value.empty());
	EXPECT_EQ(raw->storeRequests[0].mutation.operationId, raw->storeRequests[1].mutation.operationId);
	EXPECT_FALSE(raw->storeRequests[0].mutation.operationId.empty());
	EXPECT_TRUE(storage.ClearSession().success);
	EXPECT_EQ(1, raw->revokeCalls);

	raw->revokeOutcome = EEditorSecretVaultOutcome::Stopped;
	ASSERT_TRUE(storage.BindSession("host-session", 3).success);
	const auto stopped = storage.ClearSession();
	EXPECT_FALSE(stopped.success);
	EXPECT_EQ(EExtensionSecretStorageStatus::Stopped, stopped.status);
}

} // namespace
