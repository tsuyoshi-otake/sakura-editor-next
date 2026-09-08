/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlPlatformRpcServerAdapter.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "platform/storage/CInMemoryStorageService.h"

namespace platform::controlipc {
namespace {
constexpr char kProfile[] = "0123456789abcdef0123456789abcdef";
constexpr ControlIpcSessionContext kPeer{ 31, 1701 };

ControlIpcFrame Request(EControlIpcKind kind = EControlIpcKind::SenpRequest)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Request, 41, 9 }, { 1, 0 } };
}

void Hello(IControlIpcSessionHandler& session, ControlIpcSessionContext peer = kPeer)
{
	auto frame = Request(EControlIpcKind::Hello);
	frame.header.generation = 0;
	frame.payload = *EncodeControlStorageHello(kProfile);
	const auto result = session.HandleFrame(peer, frame);
	ASSERT_EQ(1U, result.responseFrames.size());
	ASSERT_EQ(EControlIpcKind::HelloAck, result.responseFrames.front().header.kind);
}

void ExpectError(const ControlIpcFrameDispatchResult& result, EControlIpcTerminalStatus status,
	EControlIpcSessionDecision decision = EControlIpcSessionDecision::Close)
{
	ASSERT_EQ(1U, result.responseFrames.size());
	const auto& frame = result.responseFrames.front();
	EXPECT_EQ(EControlIpcKind::Error, frame.header.kind);
	EXPECT_EQ(EControlIpcFlags::Response | EControlIpcFlags::Terminal, frame.header.flags);
	EXPECT_EQ(9U, frame.header.generation);
	const auto error = DecodeControlIpcError(frame.payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(status, error->status);
	EXPECT_TRUE(error->diagnostic.empty());
	EXPECT_EQ(decision, result.decision);
}

enum class Failure { None, Refuse, FactoryThrow, DispatchThrow, Empty, Multiple,
	Kind, Generation, RequestId, Version, Flags, Oversize, MalformedError, Decision, Close };

class SenpHandler final : public IControlIpcFrameHandler {
public:
	explicit SenpHandler(Failure failure = Failure::None) : m_failure(failure) {}
	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext& context) override
	{
		++m_created;
		m_peer = context;
		if (m_failure == Failure::Refuse) return {};
		if (m_failure == Failure::FactoryThrow) throw std::runtime_error("private failure must not escape");
		return std::make_unique<Session>(*this, context);
	}
	unsigned Created() const noexcept { return m_created; }
	unsigned Destroyed() const noexcept { return m_destroyed; }
	unsigned Calls() const noexcept { return m_calls; }
	ControlIpcSessionContext Peer() const noexcept { return m_peer; }
private:
	class Session final : public IControlIpcSessionHandler {
	public:
		Session(SenpHandler& owner, ControlIpcSessionContext context) : m_owner(owner), m_context(context) {}
		~Session() override { ++m_owner.m_destroyed; }
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext& peer, const ControlIpcFrame& request) override
		{
			++m_owner.m_calls;
			EXPECT_EQ(m_context.sessionId, peer.sessionId);
			EXPECT_EQ(m_context.clientProcessId, peer.clientProcessId);
			if (m_owner.m_failure == Failure::DispatchThrow) throw std::runtime_error("private tool result");
			auto response = request;
			response.header.kind = EControlIpcKind::SenpResponse;
			response.header.flags = EControlIpcFlags::Response | EControlIpcFlags::Terminal;
			switch (m_owner.m_failure) {
			case Failure::Kind: response.header.kind = EControlIpcKind::ProfileResponse; break;
			case Failure::Generation: ++response.header.generation; break;
			case Failure::RequestId: ++response.header.requestId; break;
			case Failure::Version: ++response.header.majorVersion; break;
			case Failure::Flags: response.header.flags = EControlIpcFlags::Response; break;
			case Failure::Oversize: response.payload.resize(kControlIpcMaximumFrameBytes); break;
			case Failure::MalformedError: response.header.kind = EControlIpcKind::Error; break;
			default: break;
			}
			if (m_owner.m_failure == Failure::Empty) return {};
			if (m_owner.m_failure == Failure::Multiple) return { { response, response }, EControlIpcSessionDecision::KeepOpen };
			if (m_owner.m_failure == Failure::Decision) return { { response }, static_cast<EControlIpcSessionDecision>(99) };
			return { { std::move(response) }, m_owner.m_failure == Failure::Close
				? EControlIpcSessionDecision::Close : EControlIpcSessionDecision::KeepOpen };
		}
	private:
		SenpHandler& m_owner;
		const ControlIpcSessionContext m_context;
	};
	Failure m_failure;
	ControlIpcSessionContext m_peer{};
	unsigned m_created{}, m_destroyed{}, m_calls{};
};

std::unique_ptr<CControlPlatformRpcServerAdapter> Adapter(std::shared_ptr<SenpHandler> senp = {})
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(9);
	auto profiles = std::make_shared<profiles::ControlUserDataProfileRegistry>(storage);
	return std::make_unique<CControlPlatformRpcServerAdapter>(
		ControlStorageRpcSessionIdentity{ kProfile, 9 }, storage, profiles, std::move(senp));
}

TEST(ControlPlatformSenpRpc, HelloPrecedesOneLazySessionAndPipeDestructionReleasesIt)
{
	auto handler = std::make_shared<SenpHandler>();
	auto adapter = Adapter(handler);
	auto session = adapter->CreateSession(kPeer);
	ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::InvalidRequest, EControlIpcSessionDecision::KeepOpen);
	EXPECT_EQ(0U, handler->Created());
	Hello(*session);
	EXPECT_EQ(0U, handler->Created());
	for (int call = 0; call < 2; ++call) {
		const auto result = session->HandleFrame(kPeer, Request());
		ASSERT_EQ(1U, result.responseFrames.size());
		EXPECT_EQ(EControlIpcKind::SenpResponse, result.responseFrames.front().header.kind);
		EXPECT_EQ(41U, result.responseFrames.front().header.requestId);
		EXPECT_EQ(EControlIpcSessionDecision::KeepOpen, result.decision);
	}
	EXPECT_EQ(1U, handler->Created());
	EXPECT_EQ(2U, handler->Calls());
	EXPECT_EQ(kPeer.sessionId, handler->Peer().sessionId);
	EXPECT_EQ(kPeer.clientProcessId, handler->Peer().clientProcessId);
	session.reset();
	EXPECT_EQ(1U, handler->Destroyed());
}

TEST(ControlPlatformSenpRpc, MissingCompositionIsExplicitlyUnsupportedWithoutBreakingStorage)
{
	auto adapter = Adapter();
	auto session = adapter->CreateSession(kPeer);
	Hello(*session);
	ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::UnsupportedVersion, EControlIpcSessionDecision::KeepOpen);
	auto request = Request(EControlIpcKind::StorageSnapshotRequest);
	request.payload.clear();
	const auto result = session->HandleFrame(kPeer, request);
	ASSERT_EQ(1U, result.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::StorageSnapshotResponse, result.responseFrames.front().header.kind);
}

TEST(ControlPlatformSenpRpc, InvalidEnvelopeOrPeerCannotCreateOrRetryAHandler)
{
	for (int variant = 0; variant < 8; ++variant) {
		SCOPED_TRACE(variant);
		auto handler = std::make_shared<SenpHandler>();
		auto adapter = Adapter(handler);
		auto session = adapter->CreateSession(kPeer);
		Hello(*session);
		auto peer = kPeer;
		auto request = Request();
		auto status = EControlIpcTerminalStatus::InvalidRequest;
		switch (variant) {
		case 0: ++peer.sessionId; status = EControlIpcTerminalStatus::AccessDenied; break;
		case 1: ++peer.clientProcessId; status = EControlIpcTerminalStatus::AccessDenied; break;
		case 2: ++request.header.generation; status = EControlIpcTerminalStatus::GenerationMismatch; break;
		case 3: ++request.header.majorVersion; status = EControlIpcTerminalStatus::UnsupportedVersion; break;
		case 4: request.header.requestId = 0; break;
		case 5: request.header.flags = EControlIpcFlags::Request | EControlIpcFlags::Terminal; break;
		case 6: request.header.flags = EControlIpcFlags::Response; break;
		case 7: request.payload.resize(kControlIpcMaximumFrameBytes); break;
		}
		ExpectError(session->HandleFrame(peer, request), status);
		ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::ServerStopping);
		EXPECT_EQ(0U, handler->Created());
	}
}

TEST(ControlPlatformSenpRpc, FactoryFailureIsTerminalAndNeverRetried)
{
	for (const auto failure : { Failure::Refuse, Failure::FactoryThrow }) {
		auto handler = std::make_shared<SenpHandler>(failure);
		auto adapter = Adapter(handler);
		auto session = adapter->CreateSession(kPeer);
		Hello(*session);
		ExpectError(session->HandleFrame(kPeer, Request()), failure == Failure::Refuse
			? EControlIpcTerminalStatus::ResourceExhausted : EControlIpcTerminalStatus::InternalError);
		ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::ServerStopping);
		EXPECT_EQ(1U, handler->Created());
		EXPECT_EQ(0U, handler->Calls());
	}
}

TEST(ControlPlatformSenpRpc, DispatchExceptionAndInvalidTerminalDestroyConnectionOwnerOnce)
{
	for (const auto failure : { Failure::DispatchThrow, Failure::Empty, Failure::Multiple,
		Failure::Kind, Failure::Generation, Failure::RequestId, Failure::Version, Failure::Flags, Failure::Oversize,
		Failure::MalformedError, Failure::Decision }) {
		SCOPED_TRACE(static_cast<int>(failure));
		auto handler = std::make_shared<SenpHandler>(failure);
		auto adapter = Adapter(handler);
		auto session = adapter->CreateSession(kPeer);
		Hello(*session);
		ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::InternalError);
		EXPECT_EQ(1U, handler->Destroyed());
		ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::ServerStopping);
		session.reset();
		EXPECT_EQ(1U, handler->Created());
		EXPECT_EQ(1U, handler->Calls());
		EXPECT_EQ(1U, handler->Destroyed());
	}
}

TEST(ControlPlatformSenpRpc, ExplicitCloseReleasesSessionBeforeReturningTerminal)
{
	auto handler = std::make_shared<SenpHandler>(Failure::Close);
	auto adapter = Adapter(handler);
	auto session = adapter->CreateSession(kPeer);
	Hello(*session);
	const auto result = session->HandleFrame(kPeer, Request());
	EXPECT_EQ(EControlIpcSessionDecision::Close, result.decision);
	EXPECT_EQ(1U, handler->Destroyed());
	ExpectError(session->HandleFrame(kPeer, Request()), EControlIpcTerminalStatus::ServerStopping);
	EXPECT_EQ(1U, handler->Calls());
}

TEST(ControlPlatformSenpRpc, IndependentConnectionsRetainTheirOwnLifetimeAndStopFencesDispatch)
{
	auto handler = std::make_shared<SenpHandler>();
	auto adapter = Adapter(handler);
	auto first = adapter->CreateSession(kPeer);
	const ControlIpcSessionContext otherPeer{ 32, 1702 };
	auto second = adapter->CreateSession(otherPeer);
	Hello(*first);
	Hello(*second, otherPeer);
	(void)first->HandleFrame(kPeer, Request());
	(void)second->HandleFrame(otherPeer, Request());
	first.reset();
	EXPECT_EQ(1U, handler->Destroyed());
	(void)second->HandleFrame(otherPeer, Request());
	EXPECT_EQ(3U, handler->Calls());
	ASSERT_TRUE(adapter->BeginStopping());
	EXPECT_EQ(nullptr, adapter->CreateSession(kPeer));
	ExpectError(second->HandleFrame(otherPeer, Request()), EControlIpcTerminalStatus::ServerStopping);
	EXPECT_EQ(2U, handler->Destroyed());
	EXPECT_EQ(3U, handler->Calls());
	adapter.reset();
	second.reset();
	EXPECT_EQ(2U, handler->Destroyed());
}

} // namespace
} // namespace platform::controlipc
