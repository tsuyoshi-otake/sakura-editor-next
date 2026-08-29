/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeBroker.h>

#include "platform/Windows11Platform.h"
#include "terminal/runtime/TerminalRuntimeService.h"

#include <Windows.h>

#include <chrono>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using platform::harnessbridge::CHarnessBridgeBroker;
using platform::harnessbridge::EHarnessBrokerStatus;
using platform::harnessbridge::EHarnessRunTerminalStatus;
using platform::harnessbridge::HarnessBridgeTargetDescriptor;
using platform::harnessbridge::HarnessEndpointRegistration;
using platform::harnessbridge::HarnessMessage;
using platform::harnessbridge::HarnessOpaqueId;
using platform::harnessbridge::HarnessRunResult;

class ScopedHandle final {
public:
	explicit ScopedHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~ScopedHandle() { if (m_value) ::CloseHandle(m_value); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] explicit operator bool() const noexcept { return m_value != nullptr; }

private:
	HANDLE m_value{};
};

bool IsConPtyUnavailableError(const std::uint32_t errorCode)
{
	return errorCode == ERROR_CALL_NOT_IMPLEMENTED
		|| errorCode == ERROR_NOT_SUPPORTED
		|| errorCode == ERROR_PROC_NOT_FOUND;
}

terminal::HarnessOperationId Operation(const std::uint8_t value)
{
	terminal::HarnessOperationId operation;
	operation.value[0] = value;
	return operation;
}

HarnessOpaqueId Id(const std::uint8_t value)
{
	HarnessOpaqueId id;
	id.value[0] = value;
	return id;
}

terminal::TerminalLaunchOptions CmdLaunch()
{
	wchar_t systemDirectory[MAX_PATH]{};
	const auto length = ::GetSystemDirectoryW(systemDirectory,
		static_cast<UINT>(std::size(systemDirectory)));
	terminal::TerminalLaunchOptions launch;
	if (length != 0 && length < std::size(systemDirectory)) {
		launch.executablePath.assign(systemDirectory, length);
		launch.executablePath += L"\\cmd.exe";
	}
	launch.arguments = { L"/d", L"/q" };
	launch.initialSize = { 80, 24 };
	return launch;
}

terminal::TerminalTargetCoordinate CoordinateBase()
{
	terminal::TerminalTargetCoordinate coordinate;
	coordinate.profileId = "acceptance-profile";
	coordinate.profileGeneration = terminal::ProfileAuthorityGeneration{ 1 };
	coordinate.editorId.value[0] = 1;
	coordinate.bridgeEpoch = terminal::BridgeEpoch{ 1 };
	coordinate.runtimeGeneration = terminal::TerminalRuntimeGeneration{ 1 };
	return coordinate;
}

HarnessBridgeTargetDescriptor Authority(const terminal::TerminalTargetCoordinate& coordinate)
{
	HarnessBridgeTargetDescriptor authority;
	authority.profileId = coordinate.profileId;
	authority.profileGeneration = coordinate.profileGeneration.value;
	authority.editorId = coordinate.editorId.value;
	authority.bridgeEpoch = coordinate.bridgeEpoch.value;
	authority.runtimeGeneration = coordinate.runtimeGeneration.value;
	authority.instanceGeneration = coordinate.instanceGeneration;
	authority.sessionId = coordinate.sessionId.value;
	authority.windowId = coordinate.windowId.value;
	authority.paneId = coordinate.paneId.value;
	authority.instanceId = coordinate.instanceId.value;
	return authority;
}

terminal::TerminalInputBatch EchoBatch(const std::uint8_t operation,
	const terminal::TerminalTargetCoordinate& target, const std::u16string_view marker)
{
	terminal::TerminalInputBatch batch;
	batch.operationId = Operation(operation);
	batch.target = target;
	batch.actions.push_back({ terminal::TerminalInputActionKind::LiteralText,
		u"echo " + std::u16string(marker), {} });
	batch.actions.push_back({ terminal::TerminalInputActionKind::NamedKey,
		{}, terminal::TerminalNamedKey::Enter });
	batch.deadline = std::chrono::steady_clock::now() + 2s;
	return batch;
}

bool Contains(const terminal::TerminalCaptureResult& capture, const std::u16string_view marker)
{
	for (const auto& line : capture.lines) {
		if (line.text.find(marker) != std::u16string::npos) return true;
	}
	return false;
}

terminal::TerminalCaptureResult Capture(terminal::CTerminalRuntimeService& runtime,
	const std::uint8_t operation, const terminal::TerminalTargetCoordinate& target,
	const std::optional<terminal::TerminalCaptureCursor>& since = std::nullopt)
{
	terminal::TerminalCaptureRequest request;
	request.operationId = Operation(operation);
	request.target = target;
	request.startLine = -1000;
	request.endLine = 1000;
	request.since = since;
	request.deadline = std::chrono::steady_clock::now() + 1s;
	return runtime.Capture(request);
}

bool WaitForMarker(terminal::CTerminalRuntimeService& runtime,
	const terminal::TerminalInstanceId instance,
	const terminal::TerminalTargetCoordinate& target,
	const std::u16string_view marker,
	terminal::TerminalCaptureResult& capture)
{
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deadline) {
		static_cast<void>(runtime.DrainOutput(instance));
		capture = Capture(runtime, 90, target);
		if (capture.code == terminal::TerminalCaptureResultCode::Succeeded
			&& Contains(capture, marker)) return true;
		std::this_thread::sleep_for(10ms);
	}
	return false;
}

HarnessEndpointRegistration Endpoint(const std::uint8_t id, const char* name)
{
	return { Id(id), name, "acceptance", platform::harnessbridge::EHarnessGrant::Message, 8 };
}

TEST(HarnessOrchestrationAcceptance, TwoHiddenConPtyHarnessesReceiveBoundedWorkAndTerminalResults)
{
	if (!platform::SupportsWindows11Features(platform::QueryWindowsBuild())) {
		GTEST_SKIP() << "ConPTY orchestration acceptance requires Windows 11.";
	}
	const auto launch = CmdLaunch();
	if (launch.executablePath.empty()) GTEST_SKIP() << "cmd.exe is unavailable.";

	terminal::TerminalRuntimeServiceDependencies dependencies;
	dependencies.createSession = [](terminal::TerminalSessionCallbacks callbacks) {
		return std::make_unique<terminal::CTerminalSession>(
			terminal::CreateConPtyTerminalBackend(), std::move(callbacks));
	};
	dependencies.coordinateBase = CoordinateBase();
	terminal::CTerminalRuntimeService runtime(std::move(dependencies));

	terminal::TerminalSessionCreateRequest create;
	create.operationId = Operation(1);
	create.name = "acceptance";
	create.launch = launch;
	const auto first = runtime.CreateSession(create);
	if (first.code == terminal::TerminalRuntimeOperationCode::InternalError
		&& first.instanceId) {
		const auto* failed = runtime.Instance(*first.instanceId);
		if (failed && failed->Snapshot().errorCode != 0
			&& IsConPtyUnavailableError(failed->Snapshot().errorCode)) {
			GTEST_SKIP() << "ConPTY is unavailable.";
		}
	}
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, first.code);
	ASSERT_TRUE(first.sessionId && first.paneId && first.instanceId);

	terminal::TerminalPaneSplitRequest split;
	split.operationId = Operation(2);
	split.paneId = *first.paneId;
	split.orientation = terminal::TerminalPaneOrientation::Vertical;
	split.launch = launch;
	const auto second = runtime.SplitPane(split);
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, second.code);
	ASSERT_TRUE(second.paneId && second.instanceId);

	const auto* firstInstance = runtime.Instance(*first.instanceId);
	const auto* secondInstance = runtime.Instance(*second.instanceId);
	ASSERT_NE(nullptr, firstInstance);
	ASSERT_NE(nullptr, secondInstance);
	const auto firstTarget = firstInstance->Snapshot().coordinate;
	const auto secondTarget = secondInstance->Snapshot().coordinate;
	const auto firstIdentity = runtime.GetProcessIdentity(firstTarget);
	const auto secondIdentity = runtime.GetProcessIdentity(secondTarget);
	ASSERT_TRUE(firstIdentity && secondIdentity);
	ScopedHandle firstProcess(::OpenProcess(SYNCHRONIZE, FALSE, firstIdentity->processId));
	ScopedHandle secondProcess(::OpenProcess(SYNCHRONIZE, FALSE, secondIdentity->processId));
	ASSERT_TRUE(firstProcess);
	ASSERT_TRUE(secondProcess);

	terminal::TerminalPaneSelectRequest select;
	select.operationId = Operation(3);
	select.paneId = *first.paneId;
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, runtime.SelectPane(select).code);

	const auto firstBatch = EchoBatch(4, firstTarget, u"SAKURA_ACCEPTANCE_FIRST");
	const auto secondBatch = EchoBatch(5, secondTarget, u"SAKURA_ACCEPTANCE_SECOND");
	terminal::TerminalInputResult firstInput;
	terminal::TerminalInputResult secondInput;
	std::thread firstWriter([&] { firstInput = runtime.QueueInputBatch(firstBatch); });
	std::thread secondWriter([&] { secondInput = runtime.QueueInputBatch(secondBatch); });
	firstWriter.join();
	secondWriter.join();
	ASSERT_EQ(terminal::TerminalInputResultCode::Accepted, firstInput.code);
	ASSERT_EQ(terminal::TerminalInputResultCode::Accepted, secondInput.code);

	terminal::TerminalCaptureResult firstCapture;
	terminal::TerminalCaptureResult secondCapture;
	ASSERT_TRUE(WaitForMarker(runtime, *first.instanceId, firstTarget,
		u"SAKURA_ACCEPTANCE_FIRST", firstCapture));
	ASSERT_TRUE(WaitForMarker(runtime, *second.instanceId, secondTarget,
		u"SAKURA_ACCEPTANCE_SECOND", secondCapture));
	EXPECT_FALSE(Contains(firstCapture, u"SAKURA_ACCEPTANCE_SECOND"));
	EXPECT_FALSE(Contains(secondCapture, u"SAKURA_ACCEPTANCE_FIRST"));

	const auto deltaCursor = firstCapture.nextCursor;
	ASSERT_EQ(terminal::TerminalInputResultCode::Accepted,
		runtime.QueueInputBatch(EchoBatch(6, firstTarget, u"SAKURA_DELTA_ONLY")).code);
	terminal::TerminalCaptureResult deltaCapture;
	const auto deltaDeadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < deltaDeadline) {
		static_cast<void>(runtime.DrainOutput(*first.instanceId));
		deltaCapture = Capture(runtime, 91, firstTarget, deltaCursor);
		if (deltaCapture.code == terminal::TerminalCaptureResultCode::Succeeded
			&& Contains(deltaCapture, u"SAKURA_DELTA_ONLY")) break;
		std::this_thread::sleep_for(10ms);
	}
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, deltaCapture.code);
	ASSERT_TRUE(Contains(deltaCapture, u"SAKURA_DELTA_ONLY"));
	EXPECT_FALSE(Contains(deltaCapture, u"SAKURA_ACCEPTANCE_SECOND"));
	const auto upToDate = Capture(runtime, 92, firstTarget, deltaCapture.nextCursor);
	EXPECT_EQ(terminal::TerminalCaptureResultCode::Succeeded, upToDate.code);
	EXPECT_TRUE(upToDate.lines.empty());

	CHarnessBridgeBroker broker;
	ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.Start());
	const auto firstAuthority = Authority(firstTarget);
	const auto secondAuthority = Authority(secondTarget);
	ASSERT_EQ(EHarnessBrokerStatus::Accepted,
		broker.RegisterEndpoint(Endpoint(10, "coordinator"), firstAuthority).status);
	ASSERT_EQ(EHarnessBrokerStatus::Accepted,
		broker.RegisterEndpoint(Endpoint(11, "first"), firstAuthority).status);
	ASSERT_EQ(EHarnessBrokerStatus::Accepted,
		broker.RegisterEndpoint(Endpoint(12, "second"), secondAuthority).status);

	HarnessMessage firstMessage;
	firstMessage.messageId = Id(20);
	firstMessage.runId = Id(30);
	firstMessage.sender = Id(10);
	firstMessage.recipient = Id(11);
	firstMessage.type = "work";
	firstMessage.payload = { 1, 2, 3 };
	firstMessage.deadline = std::chrono::steady_clock::now() + 2s;
	auto secondMessage = firstMessage;
	secondMessage.messageId = Id(21);
	secondMessage.runId = Id(31);
	secondMessage.recipient = Id(12);
	secondMessage.payload = { 4, 5, 6 };
	EHarnessBrokerStatus firstSend{};
	EHarnessBrokerStatus secondSend{};
	std::thread firstMessageWriter([&] {
		firstSend = broker.SendEndpointMessage(firstMessage, firstAuthority).status;
	});
	std::thread secondMessageWriter([&] {
		secondSend = broker.SendEndpointMessage(secondMessage, firstAuthority).status;
	});
	firstMessageWriter.join();
	secondMessageWriter.join();
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, firstSend);
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, secondSend);
	EXPECT_EQ(EHarnessBrokerStatus::Duplicate,
		broker.SendEndpointMessage(firstMessage, firstAuthority).status);

	const auto firstDelivery = broker.ReceiveMessages(Id(11), 1,
		std::chrono::steady_clock::now() + 1s, firstAuthority);
	ASSERT_EQ(EHarnessBrokerStatus::Succeeded, firstDelivery.status);
	ASSERT_EQ(1u, firstDelivery.messages.size());
	EXPECT_EQ(1u, firstDelivery.messages.front().deliveryAttempt);
	const auto firstRedelivery = broker.ReceiveMessages(Id(11), 1,
		std::chrono::steady_clock::now() + 1s, firstAuthority);
	ASSERT_EQ(1u, firstRedelivery.messages.size());
	EXPECT_EQ(2u, firstRedelivery.messages.front().deliveryAttempt);
	EXPECT_EQ(EHarnessBrokerStatus::Succeeded,
		broker.AcknowledgeMessage(Id(11), firstMessage.messageId, firstAuthority).status);
	const auto secondDelivery = broker.ReceiveMessages(Id(12), 1,
		std::chrono::steady_clock::now() + 1s, secondAuthority);
	ASSERT_EQ(1u, secondDelivery.messages.size());
	EXPECT_EQ(EHarnessBrokerStatus::Succeeded,
		broker.AcknowledgeMessage(Id(12), secondMessage.messageId, secondAuthority).status);

	for (const auto run : { Id(30), Id(31), Id(32), Id(33) }) {
		ASSERT_EQ(EHarnessBrokerStatus::Accepted, broker.BeginRun(run).status);
	}
	terminal::TerminalPaneCloseRequest closeSecond;
	closeSecond.operationId = Operation(7);
	closeSecond.paneId = *second.paneId;
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, runtime.ClosePane(closeSecond).code);
	const auto secondClosed = runtime.WaitForInstanceClose(*second.instanceId,
		std::chrono::steady_clock::now() + 5s);
	ASSERT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed, secondClosed.status);
	ASSERT_TRUE(secondClosed.outcome && secondClosed.outcome->IsQuiescent());
	EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(secondProcess.Get(), 0));

	EHarnessBrokerStatus firstRunStatus{};
	EHarnessBrokerStatus secondRunStatus{};
	std::thread firstResult([&] {
		firstRunStatus = broker.PublishRunResult({
			Id(30), EHarnessRunTerminalStatus::Succeeded, 0, 1 }).status;
	});
	std::thread secondResult([&] {
		secondRunStatus = broker.PublishRunResult({
			Id(31), EHarnessRunTerminalStatus::HarnessExited, 1, 2 }).status;
	});
	firstResult.join();
	secondResult.join();
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, firstRunStatus);
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, secondRunStatus);
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, broker.PublishRunResult({
		Id(32), EHarnessRunTerminalStatus::Cancelled, 0, 3 }).status);
	EXPECT_EQ(EHarnessBrokerStatus::Accepted, broker.PublishRunResult({
		Id(33), EHarnessRunTerminalStatus::TimedOut, 0, 4 }).status);
	for (const auto expected : {
		HarnessRunResult{ Id(30), EHarnessRunTerminalStatus::Succeeded, 0, 1 },
		HarnessRunResult{ Id(31), EHarnessRunTerminalStatus::HarnessExited, 1, 2 },
		HarnessRunResult{ Id(32), EHarnessRunTerminalStatus::Cancelled, 0, 3 },
		HarnessRunResult{ Id(33), EHarnessRunTerminalStatus::TimedOut, 0, 4 } }) {
		const auto waited = broker.WaitRun(expected.runId,
			std::chrono::steady_clock::now() + 1s);
		ASSERT_EQ(EHarnessBrokerStatus::Succeeded, waited.status);
		ASSERT_TRUE(waited.run);
		EXPECT_EQ(expected.status, waited.run->status);
	}
	EXPECT_EQ(EHarnessBrokerStatus::AlreadyTerminal, broker.PublishRunResult({
		Id(30), EHarnessRunTerminalStatus::Failed, 1, 5 }).status);
	broker.Stop();

	terminal::TerminalSessionCloseRequest closeFirst;
	closeFirst.operationId = Operation(8);
	closeFirst.sessionId = *first.sessionId;
	ASSERT_EQ(terminal::TerminalRuntimeOperationCode::Succeeded, runtime.CloseSession(closeFirst).code);
	const auto firstClosed = runtime.WaitForInstanceClose(*first.instanceId,
		std::chrono::steady_clock::now() + 5s);
	ASSERT_EQ(terminal::TerminalInstanceCloseWaitStatus::Closed, firstClosed.status);
	ASSERT_TRUE(firstClosed.outcome && firstClosed.outcome->IsQuiescent());
	EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(firstProcess.Get(), 0));
}

} // namespace
