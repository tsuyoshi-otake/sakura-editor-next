/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcTransport.h>
#include <sakura/controlipc/ControlIpcSecurity.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

using namespace std::chrono_literals;

std::wstring UniquePipeName()
{
	static std::atomic<unsigned long long> serial = 0;
	const auto value = (static_cast<unsigned long long>(::GetCurrentProcessId()) << 32) ^
		::GetTickCount64() ^ serial.fetch_add(1, std::memory_order_relaxed);
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring hash(64, L'a');
	for (std::size_t index = 0; index < 16; ++index) hash[48 + index] = digits[(value >> ((15 - index) * 4)) & 0xf];
	return BuildControlPipeName(hash);
}

ControlIpcFrame Request(std::uint64_t id = 41)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotRequest,
		EControlIpcFlags::Request, id, 7 }, { 0x13, 0x37 } };
}

ControlIpcFrame ResponseFor(const ControlIpcFrame& request)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotResponse,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, request.header.generation },
		{ 0x42 } };
}

ControlIpcNamedPipeOptions Options(std::wstring name, std::size_t maximumSessions = 2)
{
	ControlIpcNamedPipeOptions options;
	options.pipeName = std::move(name);
	options.maximumSessions = maximumSessions;
	options.maximumQueuedBytes = 64 * 1024;
	options.readBufferBytes = 7; // Force every normal frame through fragmented server reads.
	options.ioTimeout = 1500ms;
	return options;
}

template <class Predicate>
bool Eventually(Predicate&& predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) return true;
		std::this_thread::sleep_for(10ms);
	}
	return predicate();
}

class EchoHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		explicit Session(EchoHandler& value) : owner(value) {}
		~Session() override { owner.closed.fetch_add(1, std::memory_order_relaxed); }
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext& session, const ControlIpcFrame& frame) override
		{
			owner.calls.fetch_add(1, std::memory_order_relaxed);
			owner.lastSession.store(session.sessionId, std::memory_order_relaxed);
			return { { ResponseFor(frame) }, EControlIpcSessionDecision::KeepOpen };
		}
	private:
		EchoHandler& owner;
	};

	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override
	{
		opened.fetch_add(1, std::memory_order_relaxed);
		return std::make_unique<Session>(*this);
	}
	std::atomic<unsigned> opened = 0;
	std::atomic<unsigned> closed = 0;
	std::atomic<unsigned> calls = 0;
	std::atomic<std::uint64_t> lastSession = 0;
};

class ThrowingHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame&) override
		{
			throw 17;
		}
	};
	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override
	{
		return std::make_unique<Session>();
	}
};

class TwoPartHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame& frame) override
		{
			auto first = ResponseFor(frame);
			first.header.flags = EControlIpcFlags::Response;
			return { { std::move(first), ResponseFor(frame) }, EControlIpcSessionDecision::KeepOpen };
		}
	};
	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override { return std::make_unique<Session>(); }
};

class MisCorrelatedHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame& frame) override
		{
			auto response = ResponseFor(frame);
			++response.header.requestId;
			return { { std::move(response) }, EControlIpcSessionDecision::KeepOpen };
		}
	};
	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override { return std::make_unique<Session>(); }
};

class CallbackStopHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		explicit Session(CallbackStopHandler& value) : owner(value) {}
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame&) override
		{
			owner.entered.fetch_add(1, std::memory_order_acq_rel);
			const auto deadline = std::chrono::steady_clock::now() + 1s;
			while (owner.entered.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(1ms);
			}
			if (owner.server) owner.server->Stop();
			return { {}, EControlIpcSessionDecision::Close };
		}
	private:
		CallbackStopHandler& owner;
	};
	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override { return std::make_unique<Session>(*this); }
	CControlIpcNamedPipeServer* server = nullptr;
	std::atomic<unsigned> entered = 0;
};

} // namespace

TEST(ControlIpcNamedPipeTransport, CurrentUserClientGetsFragmentedFrameRoundTripAfterVerifiedFirstRead)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name)).success);
	CControlIpcNamedPipeClient client;
	ASSERT_TRUE(client.Connect(name, ::GetCurrentProcessId(), 1500ms).success);

	std::vector<ControlIpcFrame> responses;
	ASSERT_TRUE(client.Exchange(Request(), responses, 1500ms).success);
	ASSERT_EQ(1u, responses.size());
	EXPECT_EQ(EControlIpcKind::StorageSnapshotResponse, responses[0].header.kind);
	EXPECT_EQ(41u, responses[0].header.requestId);
	EXPECT_EQ(std::vector<std::uint8_t>({ 0x42 }), responses[0].payload);
	EXPECT_EQ(1u, handler->calls.load(std::memory_order_relaxed));
	EXPECT_NE(0u, handler->lastSession.load(std::memory_order_relaxed));
	client.Close();
	server.Stop();
	EXPECT_EQ(1u, handler->opened.load(std::memory_order_relaxed));
	EXPECT_EQ(1u, handler->closed.load(std::memory_order_relaxed));
}

TEST(ControlIpcNamedPipeTransport, RejectsWrongExpectedServerProcessIdentity)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name)).success);
	CControlIpcNamedPipeClient client;
	const auto result = client.Connect(name, ::GetCurrentProcessId() + 1, 1000ms);
	EXPECT_FALSE(result.success);
	EXPECT_EQ(EControlIpcTransportDisconnectReason::AccessDenied, result.reason);
	server.Stop();
}

TEST(ControlIpcNamedPipeTransport, ExchangeWaitsForMatchingTerminalResponseAndRejectsMisCorrelation)
{
	CControlIpcNamedPipeServer server(std::make_shared<TwoPartHandler>());
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name)).success);
	CControlIpcNamedPipeClient client;
	ASSERT_TRUE(client.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	std::vector<ControlIpcFrame> responses;
	ASSERT_TRUE(client.Exchange(Request(), responses, 1000ms).success);
	ASSERT_EQ(2u, responses.size());
	EXPECT_FALSE(HasFlag(responses[0].header.flags, EControlIpcFlags::Terminal));
	EXPECT_TRUE(HasFlag(responses[1].header.flags, EControlIpcFlags::Terminal));
	client.Close();
	server.Stop();

	CControlIpcNamedPipeServer badServer(std::make_shared<MisCorrelatedHandler>());
	const auto badName = UniquePipeName();
	ASSERT_TRUE(badServer.Start(Options(badName)).success);
	ASSERT_TRUE(client.Connect(badName, ::GetCurrentProcessId(), 1000ms).success);
	EXPECT_FALSE(client.Exchange(Request(), responses, 1000ms).success);
	EXPECT_FALSE(client.IsConnected());
	badServer.Stop();
}

TEST(ControlIpcNamedPipeTransport, MalformedLengthClosesWithObservableProtocolError)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name)).success);
	HANDLE raw = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, raw);
	const std::array<std::uint8_t, 4> oversize{ 1, 0, 16, 0 }; // 1 MiB + 1, little endian.
	DWORD written = 0;
	ASSERT_TRUE(::WriteFile(raw, oversize.data(), static_cast<DWORD>(oversize.size()), &written, nullptr));
	const bool completedInTime = Eventually([&] {
		const auto completed = server.CompletedSessions();
		return !completed.empty();
	});
	::CloseHandle(raw);
	ASSERT_TRUE(completedInTime);
	const auto completed = server.CompletedSessions();
	ASSERT_FALSE(completed.empty());
	EXPECT_EQ(EControlIpcTransportDisconnectReason::ProtocolError, completed.back().reason);
	EXPECT_EQ(0u, handler->calls.load(std::memory_order_relaxed));
	server.Stop();
}

TEST(ControlIpcNamedPipeTransport, ServerAppliesConfiguredInboundFrameBound)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	auto options = Options(name);
	options.maximumQueuedBytes = 32; // Fixed header is 28 bytes; a five-byte payload is over this decoder bound.
	ASSERT_TRUE(server.Start(options).success);
	CControlIpcNamedPipeClient client;
	ASSERT_TRUE(client.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	auto oversizedForConfiguredBound = Request(333);
	oversizedForConfiguredBound.payload.assign(5, 0x55);
	ASSERT_TRUE(client.Send(oversizedForConfiguredBound, 1000ms).success);
	ASSERT_TRUE(Eventually([&] {
		const auto completed = server.CompletedSessions();
		return !completed.empty();
	}));
	const auto completed = server.CompletedSessions();
	ASSERT_FALSE(completed.empty());
	EXPECT_EQ(EControlIpcTransportDisconnectReason::ProtocolError, completed.back().reason);
	EXPECT_EQ(0u, handler->calls.load(std::memory_order_relaxed));
	client.Close();
	server.Stop();
}

TEST(ControlIpcNamedPipeTransport, ContainsHandlerExceptionAndSettlesTheSession)
{
	CControlIpcNamedPipeServer server(std::make_shared<ThrowingHandler>());
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name)).success);
	CControlIpcNamedPipeClient client;
	ASSERT_TRUE(client.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	ASSERT_TRUE(client.Send(Request(), 1000ms).success);
	ASSERT_TRUE(Eventually([&] {
		const auto completed = server.CompletedSessions();
		return !completed.empty();
	}));
	const auto completed = server.CompletedSessions();
	EXPECT_EQ(EControlIpcTransportDisconnectReason::CallbackFailed, completed.back().reason);
	client.Close();
	server.Stop();
}

TEST(ControlIpcNamedPipeTransport, CapsSessionsAndStopCancelsPendingAcceptPromptly)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name, 1)).success);
	CControlIpcNamedPipeClient first;
	ASSERT_TRUE(first.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	ASSERT_TRUE(Eventually([&] { return server.ActiveSessionCount() == 1; }));
	CControlIpcNamedPipeClient second;
	(void)second.Connect(name, ::GetCurrentProcessId(), 1000ms);
	EXPECT_TRUE(Eventually([&] { return server.RejectedSessionCount() >= 1; }));
	const auto started = std::chrono::steady_clock::now();
	std::thread firstStop([&] { server.Stop(); });
	std::thread secondStop([&] { server.Stop(); });
	firstStop.join();
	secondStop.join();
	EXPECT_LT(std::chrono::steady_clock::now() - started, 1200ms);
	EXPECT_EQ(0u, server.ActiveSessionCount());
	first.Close();
	second.Close();
}

TEST(ControlIpcNamedPipeTransport, SimultaneousCallbackStopRequestsOnlyCancelUntilExternalJoin)
{
	auto handler = std::make_shared<CallbackStopHandler>();
	CControlIpcNamedPipeServer server(handler);
	handler->server = &server;
	const auto name = UniquePipeName();
	ASSERT_TRUE(server.Start(Options(name, 2)).success);
	CControlIpcNamedPipeClient first;
	CControlIpcNamedPipeClient second;
	ASSERT_TRUE(first.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	ASSERT_TRUE(second.Connect(name, ::GetCurrentProcessId(), 1000ms).success);
	ASSERT_TRUE(first.Send(Request(501), 1000ms).success);
	ASSERT_TRUE(second.Send(Request(502), 1000ms).success);
	ASSERT_TRUE(Eventually([&] { return handler->entered.load(std::memory_order_acquire) == 2; }));
	server.Stop();
	EXPECT_EQ(0u, server.ActiveSessionCount());
	first.Close();
	second.Close();
}

TEST(ControlIpcNamedPipeTransport, RejectsInvalidOptionsDeadlinesAndFailedRebindLeavesServerStopped)
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer first(handler);
	const auto name = UniquePipeName();
	ASSERT_TRUE(first.Start(Options(name)).success);
	CControlIpcNamedPipeServer second(handler);
	EXPECT_FALSE(second.Start(Options(name)).success);
	EXPECT_FALSE(second.IsRunning());
	CControlIpcNamedPipeClient client;
	EXPECT_FALSE(client.Connect(name, ::GetCurrentProcessId(), 0ms).success);
	EXPECT_FALSE(client.Connect(name, ::GetCurrentProcessId(), 61s).success);
	EXPECT_FALSE(client.Send(Request(), 0ms).success);
	auto invalidExchange = Request();
	invalidExchange.header.flags = EControlIpcFlags::Response;
	std::vector<ControlIpcFrame> ignored;
	EXPECT_FALSE(client.Exchange(invalidExchange, ignored, 1000ms).success);
	first.Stop();
}

} // namespace platform::controlipc
