/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/controlipc/ControlIpcTransport.h>

#if __has_include("platform/controlipc/ControlIpcNamedPipeTransport.h")
#error "sakura_controlipc_transport_tests can reach the removed private transport contract"
#endif

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace platform::controlipc;
using namespace std::chrono_literals;

std::wstring UniquePipeName()
{
	static std::atomic<unsigned long long> serial = 0;
	const auto value = (static_cast<unsigned long long>(::GetCurrentProcessId()) << 32)
		^ ::GetTickCount64() ^ serial.fetch_add(1, std::memory_order_relaxed);
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring hash(64, L'a');
	for (std::size_t index = 0; index < 16; ++index) {
		hash[48 + index] = digits[(value >> ((15 - index) * 4)) & 0xf];
	}
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

ControlIpcNamedPipeOptions Options(std::wstring name)
{
	ControlIpcNamedPipeOptions options;
	options.pipeName = std::move(name);
	options.maximumSessions = 2;
	options.maximumQueuedBytes = 64 * 1024;
	options.readBufferBytes = 7;
	options.ioTimeout = 1500ms;
	return options;
}

class EchoHandler final : public IControlIpcFrameHandler {
public:
	class Session final : public IControlIpcSessionHandler {
	public:
		ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame& frame) override
		{
			return { { ResponseFor(frame) }, EControlIpcSessionDecision::KeepOpen };
		}
	};

	std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext&) override
	{
		return std::make_unique<Session>();
	}
};

bool RoundTripUsesOnlyThePublicTransportContract()
{
	auto handler = std::make_shared<EchoHandler>();
	CControlIpcNamedPipeServer server(handler);
	const auto name = UniquePipeName();
	if (!server.Start(Options(name)).success) return false;
	CControlIpcNamedPipeClient client;
	if (!client.Connect(name, ::GetCurrentProcessId(), 1500ms).success) return false;
	std::vector<ControlIpcFrame> responses;
	const bool exchanged = client.Exchange(Request(), responses, 1500ms).success
		&& responses.size() == 1
		&& responses.front().header.kind == EControlIpcKind::StorageSnapshotResponse
		&& responses.front().header.requestId == 41;
	client.Close();
	server.Stop();
	return exchanged && !server.IsRunning();
}

bool InvalidOptionsAreProtocolTerminals()
{
	CControlIpcNamedPipeServer server(std::make_shared<EchoHandler>());
	ControlIpcNamedPipeOptions options;
	options.pipeName = L"not-a-control-pipe";
	const auto invalidName = server.Start(options);
	if (invalidName.success || invalidName.reason != EControlIpcTransportDisconnectReason::ProtocolError) return false;
	options.pipeName = UniquePipeName();
	options.maximumSessions = 0;
	const auto invalidSessions = server.Start(options);
	return !invalidSessions.success && invalidSessions.reason == EControlIpcTransportDisconnectReason::ProtocolError;
}

bool InvalidDeadlineIsTimeoutTerminal()
{
	CControlIpcNamedPipeClient client;
	const auto result = client.Connect(UniquePipeName(), ::GetCurrentProcessId(), 0ms);
	return !result.success && result.reason == EControlIpcTransportDisconnectReason::DeadlineExceeded;
}

bool MissingHandlerIsCallbackTerminal()
{
	CControlIpcNamedPipeServer server(nullptr);
	const auto result = server.Start(Options(UniquePipeName()));
	return !result.success && result.reason == EControlIpcTransportDisconnectReason::CallbackFailed;
}

bool StopIsIdempotentAndWithdrawsTheServer()
{
	CControlIpcNamedPipeServer server(std::make_shared<EchoHandler>());
	if (!server.Start(Options(UniquePipeName())).success) return false;
	server.Stop();
	server.Stop();
	return !server.IsRunning() && server.ActiveSessionCount() == 0 && server.CompletedSessions().empty();
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept : m_name(name), m_run(run) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	const std::string_view m_name;
	bool (*const m_run)();
};

constexpr std::array kTests{
	TestCase{ "RoundTripUsesOnlyThePublicTransportContract", RoundTripUsesOnlyThePublicTransportContract },
	TestCase{ "InvalidOptionsAreProtocolTerminals", InvalidOptionsAreProtocolTerminals },
	TestCase{ "InvalidDeadlineIsTimeoutTerminal", InvalidDeadlineIsTimeoutTerminal },
	TestCase{ "MissingHandlerIsCallbackTerminal", MissingHandlerIsCallbackTerminal },
	TestCase{ "StopIsIdempotentAndWithdrawsTheServer", StopIsIdempotentAndWithdrawsTheServer },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "ControlIpcTransportContract.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "ControlIpcTransportContract." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
