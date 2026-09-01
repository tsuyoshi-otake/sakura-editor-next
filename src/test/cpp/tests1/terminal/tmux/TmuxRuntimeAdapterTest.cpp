/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/tmux/TmuxRuntimeAdapter.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace terminal::tmux {

class TmuxWaitChannelServiceTestProbe final {
public:
	[[nodiscard]] static std::size_t PendingWaiterCount(
		const TmuxWaitChannelService& service, const std::string_view channel) noexcept
	{
		return service.PendingWaiterCountForTesting(channel);
	}

	[[nodiscard]] static std::size_t PendingLockerCount(
		const TmuxWaitChannelService& service, const std::string_view channel) noexcept
	{
		return service.PendingLockerCountForTesting(channel);
	}
};

} // namespace terminal::tmux

namespace {

using namespace terminal;
using namespace terminal::tmux;
using namespace std::chrono_literals;

template<typename Observe>
bool WaitForObservedCount(Observe&& observe, const std::size_t expected,
	const std::chrono::steady_clock::time_point deadline)
{
	do {
		if (observe() == expected) return true;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return observe() == expected;
}

template<typename Id>
Id MakeId(const std::uint64_t value)
{
	Id id;
	id.value = value;
	return id;
}

class FakeRuntimeService final : public ITerminalRuntimeService {
public:
	FakeRuntimeService()
	{
		collection.revision = MakeId<TerminalTopologyRevision>(1);
		collection.activeSession = MakeId<TerminalSessionId>(1);
		runtime::topology::TerminalSessionSnapshot session;
		session.id = MakeId<TerminalSessionId>(1);
		session.name = "dev";
		session.activeWindow = MakeId<TerminalWindowId>(2);
		runtime::topology::TerminalWindowSnapshot window;
		window.id = MakeId<TerminalWindowId>(2);
		window.name = "main";
		window.activePane = MakeId<TerminalPaneId>(3);
		runtime::topology::TerminalPaneSnapshot pane;
		pane.id = MakeId<TerminalPaneId>(3);
		pane.instanceId = MakeId<TerminalInstanceId>(7);
		pane.sessionId = session.id;
		pane.windowId = window.id;
		window.panes.push_back(pane);
		window.root.kind = runtime::topology::TerminalLayoutNodeKind::Pane;
		window.root.pane = { pane.id, pane.instanceId };
		session.order.push_back(window.id);
		session.windows.push_back(window);
		collection.order.push_back(session.id);
		collection.sessions.push_back(session);

		instance.coordinate.runtimeGeneration = MakeId<TerminalRuntimeGeneration>(5);
		instance.coordinate.sessionId = session.id;
		instance.coordinate.windowId = window.id;
		instance.coordinate.paneId = pane.id;
		instance.coordinate.instanceId = pane.instanceId;
		instance.coordinate.instanceGeneration = 9;
		instance.state = TerminalInstanceState::Running;
		instance.sessionState = TerminalSessionState::Running;
		instance.columns = 80;
		instance.rows = 24;
		instance.scrollbackSize = 12;
		instance.scrollbackLimit = 1000;
		instance.processName = L"pwsh";
		instance.sequenceTitle = L"PowerShell";
	}

	TerminalCreateResult CreateInstance(const TerminalCreateRequest&) override { return {}; }
	TerminalTopologyResult CreateSession(const TerminalSessionCreateRequest& request) override
	{
		lastSession = request;
		return TopologyResult(10, 11, 12, 13);
	}
	TerminalTopologyResult CreateTerminalWindow(const TerminalWindowCreateRequest& request) override
	{
		lastWindow = request;
		return TopologyResult(10, 11, 14, 15);
	}
	TerminalTopologyResult SplitPane(const TerminalPaneSplitRequest& request) override
	{
		lastSplit = request;
		return TopologyResult(10, 11, 16, 17);
	}
	TerminalTopologyResult SelectWindow(const TerminalWindowSelectRequest& request) override
	{
		lastSelectedWindow = request;
		return SuccessTopology();
	}
	TerminalTopologyResult SelectPane(const TerminalPaneSelectRequest& request) override
	{
		lastSelectedPane = request;
		return SuccessTopology();
	}
	TerminalTopologyResult ClosePane(const TerminalPaneCloseRequest& request) override
	{
		lastClosedPane = request;
		return SuccessTopology();
	}
	TerminalTopologyResult CloseWindow(const TerminalWindowCloseRequest& request) override
	{
		lastClosedWindow = request;
		return SuccessTopology();
	}
	TerminalTopologyResult CloseSession(const TerminalSessionCloseRequest& request) override
	{
		lastClosedSession = request;
		return SuccessTopology();
	}

	TerminalInputResult QueueInputBatch(const TerminalInputBatch& request) override
	{
		std::lock_guard lock(mutex);
		lastInput = request;
		++inputCalls;
		TerminalInputResult result;
		result.code = TerminalInputResultCode::Accepted;
		return result;
	}
	TerminalCaptureResult Capture(const TerminalCaptureRequest& request) override
	{
		lastCapture = request;
		TerminalCaptureResult result;
		result.code = captureCode;
		result.gap = captureGap;
		result.truncated = captureTruncated;
		result.lines = { { 0, 0, false, false, u"hello" }, { 1, 1, true, false, u"\u4e16\u754c" } };
		return result;
	}
	TerminalSnapshotResult Snapshot(const TerminalSnapshotRequest&) const override { return {}; }
	TerminalResizeResult Resize(const TerminalResizeRequest&) override { return {}; }
	TerminalInstanceDrainResult DrainOutput(TerminalInstanceId) override { return {}; }
	TerminalSubscription Subscribe(TerminalRuntimeEventCallback) override { return {}; }
	void BeginClose() noexcept override { closing = true; }
	TerminalRuntimeCloseResult WaitForClose(std::chrono::steady_clock::time_point) noexcept override
	{
		return { TerminalRuntimeCloseWaitStatus::Closed };
	}

	static TerminalTopologyResult SuccessTopology()
	{
		TerminalTopologyResult result;
		result.code = TerminalRuntimeOperationCode::Succeeded;
		result.revision = MakeId<TerminalTopologyRevision>(1);
		return result;
	}
	static TerminalTopologyResult TopologyResult(
		const std::uint64_t revision, const std::uint64_t session, const std::uint64_t window,
		const std::uint64_t pane)
	{
		TerminalTopologyResult result = SuccessTopology();
		result.revision = MakeId<TerminalTopologyRevision>(revision);
		result.sessionId = MakeId<TerminalSessionId>(session);
		result.windowId = MakeId<TerminalWindowId>(window);
		result.paneId = MakeId<TerminalPaneId>(pane);
		result.instanceId = MakeId<TerminalInstanceId>(pane + 100);
		return result;
	}

	runtime::topology::TerminalCollectionSnapshot collection;
	TerminalInstanceSnapshot instance;
	TerminalSessionCreateRequest lastSession;
	TerminalWindowCreateRequest lastWindow;
	TerminalPaneSplitRequest lastSplit;
	TerminalWindowSelectRequest lastSelectedWindow;
	TerminalPaneSelectRequest lastSelectedPane;
	TerminalPaneCloseRequest lastClosedPane;
	TerminalWindowCloseRequest lastClosedWindow;
	TerminalSessionCloseRequest lastClosedSession;
	TerminalInputBatch lastInput;
	TerminalCaptureRequest lastCapture;
	TerminalCaptureResultCode captureCode{ TerminalCaptureResultCode::Succeeded };
	bool captureGap{};
	bool captureTruncated{};
	std::atomic<int> inputCalls{};
	bool closing{};
	mutable std::mutex mutex;
};

TmuxRuntimeAdapter MakeAdapter(FakeRuntimeService& runtime, TmuxWaitChannelService& wait)
{
	return TmuxRuntimeAdapter(runtime,
		[&runtime] { return std::optional(runtime.collection); },
		[&runtime](const TerminalInstanceId id) -> std::optional<TerminalInstanceSnapshot> {
			return id == runtime.instance.coordinate.instanceId ? std::optional(runtime.instance) : std::nullopt;
		}, wait);
}

TEST(TmuxWaitChannelService, SignalReleasesAllWaitersAndPendingSignalIsConsumed)
{
	TmuxWaitChannelService service;
	TmuxWaitResult first;
	TmuxWaitResult second;
	std::thread waiter1([&] {
		first = service.Wait("ready", std::chrono::steady_clock::now() + 2s);
	});
	std::thread waiter2([&] {
		second = service.Wait("ready", std::chrono::steady_clock::now() + 2s);
	});
	const auto waitersRegistered = WaitForObservedCount(
		[&] { return TmuxWaitChannelServiceTestProbe::PendingWaiterCount(service, "ready"); },
		2, std::chrono::steady_clock::now() + 2s);
	if (!waitersRegistered) {
		service.BeginShutdown();
		waiter1.join();
		waiter2.join();
	}
	ASSERT_TRUE(waitersRegistered);
	EXPECT_TRUE(service.Signal("ready").Succeeded());
	waiter1.join();
	waiter2.join();
	EXPECT_TRUE(first.Succeeded());
	EXPECT_TRUE(second.Succeeded());

	EXPECT_TRUE(service.Signal("pending").Succeeded());
	EXPECT_TRUE(service.Wait("pending", std::chrono::steady_clock::now() + 100ms).Succeeded());
	EXPECT_EQ(TmuxWaitCode::DeadlineExceeded,
		service.Wait("pending", std::chrono::steady_clock::now() + 20ms).code);
}

TEST(TmuxWaitChannelService, LockTransfersInFifoOrderAndShutdownWakesBlockedWaiter)
{
	TmuxWaitChannelService service;
	ASSERT_TRUE(service.Lock("lock", std::chrono::steady_clock::now() + 100ms).Succeeded());
	TmuxWaitResult first;
	TmuxWaitResult second;
	std::thread locker1([&] { first = service.Lock("lock", std::chrono::steady_clock::now() + 2s); });
	const auto firstRegistered = WaitForObservedCount(
		[&] { return TmuxWaitChannelServiceTestProbe::PendingLockerCount(service, "lock"); },
		1, std::chrono::steady_clock::now() + 2s);
	if (!firstRegistered) {
		service.BeginShutdown();
		locker1.join();
		FAIL() << "First locker did not register before its deadline.";
	}
	std::thread locker2([&] { second = service.Lock("lock", std::chrono::steady_clock::now() + 2s); });
	const auto secondRegistered = WaitForObservedCount(
		[&] { return TmuxWaitChannelServiceTestProbe::PendingLockerCount(service, "lock"); },
		2, std::chrono::steady_clock::now() + 2s);
	if (!secondRegistered) {
		service.BeginShutdown();
		locker1.join();
		locker2.join();
		FAIL() << "Second locker did not register behind the first locker.";
	}
	if (!service.Unlock("lock").Succeeded()) {
		service.BeginShutdown();
		locker1.join();
		locker2.join();
		FAIL() << "Initial lock could not transfer to the first queued locker.";
	}
	locker1.join();
	if (!first.Succeeded()) {
		service.BeginShutdown();
		locker2.join();
		FAIL() << "First queued locker did not receive the lock.";
	}
	if (!service.Unlock("lock").Succeeded()) {
		service.BeginShutdown();
		locker2.join();
		FAIL() << "First queued locker could not transfer to the second locker.";
	}
	locker2.join();
	EXPECT_TRUE(second.Succeeded());
	ASSERT_TRUE(service.Lock("shutdown", std::chrono::steady_clock::now() + 100ms).Succeeded());
	TmuxWaitResult stopped;
	std::thread blocked([&] { stopped = service.Lock("shutdown", std::chrono::steady_clock::now() + 2s); });
	const auto blockedRegistered = WaitForObservedCount(
		[&] { return TmuxWaitChannelServiceTestProbe::PendingLockerCount(service, "shutdown"); },
		1, std::chrono::steady_clock::now() + 2s);
	service.BeginShutdown();
	blocked.join();
	ASSERT_TRUE(blockedRegistered);
	EXPECT_EQ(TmuxWaitCode::Stopped, stopped.code);
}

TEST(TmuxRuntimeAdapter, MapsSnapshotTopologyInputAndCaptureThroughRuntimeService)
{
	FakeRuntimeService runtime;
	TmuxWaitChannelService wait;
	auto adapter = MakeAdapter(runtime, wait);
	const auto snapshot = adapter.Snapshot();
	ASSERT_EQ(1u, snapshot.sessions.size());
	ASSERT_EQ(1u, snapshot.sessions.front().windows.size());
	ASSERT_EQ(1u, snapshot.sessions.front().windows.front().panes.size());
	const auto& pane = snapshot.sessions.front().windows.front().panes.front();
	EXPECT_EQ(7u, pane.instanceId.value);
	EXPECT_EQ("pwsh", pane.currentCommand);
	EXPECT_EQ("PowerShell", pane.title);
	EXPECT_EQ(80u, pane.width);
	EXPECT_EQ(24u, pane.height);
	EXPECT_TRUE(pane.coordinate.runtimeGeneration.IsValid());

	TmuxInputBatch input;
	input.target.paneId = pane.id;
	input.target.coordinate = pane.coordinate;
	input.expectedRevision = snapshot.revision;
	input.tokens = { { TmuxInputTokenKind::NamedKey, "Enter" }, { TmuxInputTokenKind::NamedKey, "C-c" } };
	input.repeatCount = 2;
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, adapter.SendKeys(input).code);
	EXPECT_EQ(1, runtime.inputCalls.load());
	ASSERT_EQ(2u, runtime.lastInput.actions.size());
	EXPECT_EQ(TerminalInputActionKind::NamedKey, runtime.lastInput.actions[0].kind);
	EXPECT_EQ(TerminalNamedKey::Enter, runtime.lastInput.actions[0].key);
	EXPECT_EQ(TerminalInputActionKind::LiteralText, runtime.lastInput.actions[1].kind);
	ASSERT_EQ(1u, runtime.lastInput.actions[1].text.size());
	EXPECT_EQ(u'\x03', runtime.lastInput.actions[1].text.front());
	EXPECT_EQ(2, runtime.lastInput.repeatCount);
	EXPECT_TRUE(runtime.lastInput.operationId.IsValid());

	TmuxCaptureRequest capture;
	capture.target.paneId = pane.id;
	capture.target.coordinate = pane.coordinate;
	capture.expectedRevision = snapshot.revision;
	const auto captured = adapter.CapturePane(capture);
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, captured.code);
	EXPECT_TRUE(captured.complete);
	ASSERT_EQ(2u, captured.lines.size());
	EXPECT_EQ("hello", captured.lines[0].text);
	EXPECT_EQ("\xE4\xB8\x96\xE7\x95\x8C", captured.lines[1].text);
	EXPECT_EQ(1u, runtime.lastCapture.startLine.value_or(1));

	TmuxCreateWindowRequest createWindow;
	createWindow.sessionId = MakeId<TerminalSessionId>(1);
	createWindow.expectedRevision = snapshot.revision;
	createWindow.detached = true;
	createWindow.name = "logs";
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, adapter.CreateTerminalWindow(createWindow).code);
	EXPECT_TRUE(runtime.lastWindow.operationId.IsValid());
	EXPECT_TRUE(runtime.lastWindow.detached);
}

TEST(TmuxRuntimeAdapter, RevisionFenceAndCaptureBoundedStateArePreserved)
{
	FakeRuntimeService runtime;
	TmuxWaitChannelService wait;
	auto adapter = MakeAdapter(runtime, wait);
	TmuxCreateSessionRequest stale;
	stale.expectedRevision = MakeId<TerminalTopologyRevision>(99);
	const auto rejected = adapter.CreateSession(stale);
	EXPECT_EQ(TmuxRuntimeCode::TopologyChanged, rejected.code);
	EXPECT_EQ(0u, runtime.lastSession.operationId.value[0]);

	const auto snapshot = adapter.Snapshot();
	const auto pane = snapshot.sessions.front().windows.front().panes.front();
	TmuxCaptureRequest capture;
	capture.target.paneId = pane.id;
	capture.target.coordinate = pane.coordinate;
	capture.expectedRevision = snapshot.revision;
	runtime.captureGap = true;
	runtime.captureTruncated = true;
	const auto bounded = adapter.CapturePane(capture);
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, bounded.code);
	EXPECT_TRUE(bounded.gap);
	EXPECT_TRUE(bounded.truncated);
	EXPECT_FALSE(bounded.complete);

	TmuxWaitRequest signal;
	signal.operation = TmuxWaitOperation::Signal;
	signal.channel = "adapter";
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, adapter.WaitFor(signal).code);
	TmuxWaitRequest consume;
	consume.operation = TmuxWaitOperation::Wait;
	consume.channel = "adapter";
	consume.deadline = std::chrono::steady_clock::now() + 100ms;
	EXPECT_EQ(TmuxRuntimeCode::Succeeded, adapter.WaitFor(consume).code);
	adapter.BeginShutdown();
	EXPECT_EQ(TmuxRuntimeCode::Stopped, adapter.WaitFor(signal).code);
}

} // namespace
