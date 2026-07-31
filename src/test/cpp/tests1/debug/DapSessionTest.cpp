/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "debug/dap/DapSession.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace debug::dap;

class FakeTransport final : public IDapByteTransport {
public:
	bool Send(const std::string_view bytes) noexcept override
	{
		sent.emplace_back(bytes);
		if (onSend) onSend();
		return allowSend;
	}
	void Close() noexcept override { ++closeCount; }

	bool allowSend{ true };
	std::function<void()> onSend;
	std::vector<std::string> sent;
	std::size_t closeCount{};
};

class BlockingTransport final : public IDapByteTransport {
public:
	bool Send(const std::string_view bytes) noexcept override
	{
		try {
			std::unique_lock lock(m_mutex);
			m_sent.emplace_back(bytes);
			m_sendEntered = true;
			m_changed.notify_all();
			m_changed.wait(lock, [this] { return m_releaseSend; });
			return true;
		} catch (...) {
			return false;
		}
	}

	void Close() noexcept override
	{
		std::lock_guard lock(m_mutex);
		++m_closeCount;
	}

	[[nodiscard]] bool WaitForSendEntered(const std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		return m_changed.wait_for(lock, timeout, [this] { return m_sendEntered; });
	}

	void ReleaseSend() noexcept
	{
		std::lock_guard lock(m_mutex);
		m_releaseSend = true;
		m_changed.notify_all();
	}

	[[nodiscard]] std::size_t CloseCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		return m_closeCount;
	}

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::vector<std::string> m_sent;
	std::size_t m_closeCount{};
	bool m_sendEntered{};
	bool m_releaseSend{};
};

std::string Frame(const std::string_view body)
{
	return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

TEST(DapSession, StartsOnlyOnceAndRejectsWorkBeforeStart)
{
	FakeTransport transport;
	CDapSession session(transport);
	EXPECT_EQ(EDapSessionStatus::NotStarted, session.SendRequest({ .command = "threads" }).status);
	EXPECT_EQ(EDapSessionStatus::Started, session.Start().status);
	EXPECT_EQ(EDapSessionStatus::AlreadyStarted, session.Start().status);
	EXPECT_EQ(EDapSessionState::Running, session.Snapshot().state);
}

TEST(DapSession, SendsMonotonicRequestsAndCorrelatesResponses)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	const auto first = session.SendRequest({ .command = "initialize" });
	const auto second = session.SendRequest({ .command = "threads", .deadline = 100 });
	ASSERT_EQ(EDapSessionStatus::Sent, first.status);
	ASSERT_EQ(EDapSessionStatus::Sent, second.status);
	EXPECT_EQ(1U, *first.requestSequence);
	EXPECT_EQ(2U, *second.requestSequence);
	ASSERT_EQ(2U, transport.sent.size());
	std::vector<DapSessionNotification> seen;
	ASSERT_TRUE(session.Subscribe([&seen](const auto& change) { seen.push_back(change); }));
	EXPECT_EQ(EDapSessionStatus::Received, session.Feed(Frame(R"json({"seq":20,"type":"response","request_seq":1,"success":true,"command":"initialize"})json")).status);
	const auto snapshot = session.Snapshot();
	ASSERT_EQ(1U, snapshot.pendingRequests.size());
	EXPECT_EQ(2U, snapshot.pendingRequests[0].sequence);
	ASSERT_FALSE(seen.empty());
	EXPECT_EQ(EDapSessionNotificationKind::ResponseReceived, seen.back().kind);
}

TEST(DapSession, DeliversServerRequestsAndEventsAndRetainsOnlyBoundedEvents)
{
	FakeTransport transport;
	DapSessionLimits limits;
	limits.maximumRecentEvents = 1;
	CDapSession session(transport, limits);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	std::vector<EDapSessionNotificationKind> kinds;
	ASSERT_TRUE(session.Subscribe([&kinds](const auto& change) { kinds.push_back(change.kind); }));
	EXPECT_EQ(EDapSessionStatus::Received, session.Feed(Frame(R"json({"seq":3,"type":"request","command":"runInTerminal"})json")
		+ Frame(R"json({"seq":4,"type":"event","event":"initialized"})json")
		+ Frame(R"json({"seq":5,"type":"event","event":"stopped"})json")).status);
	const auto snapshot = session.Snapshot();
	ASSERT_EQ(1U, snapshot.recentEvents.size());
	EXPECT_EQ("stopped", snapshot.recentEvents.front().event);
	EXPECT_NE(kinds.end(), std::find(kinds.begin(), kinds.end(), EDapSessionNotificationKind::ServerRequestReceived));
	EXPECT_NE(kinds.end(), std::find(kinds.begin(), kinds.end(), EDapSessionNotificationKind::EventReceived));
}

TEST(DapSession, InvalidCodecAndUnknownOrMismatchedResponsesFailAndFinalizePending)
{
	for (const auto& response : { std::string(R"json({"seq":3,"type":"response","request_seq":99,"success":true,"command":"threads"})json"),
		std::string(R"json({"seq":3,"type":"response","request_seq":1,"success":true,"command":"other"})json") }) {
		FakeTransport transport;
		CDapSession session(transport);
		ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
		ASSERT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "threads" }).status);
		EXPECT_EQ(EDapSessionStatus::ProtocolViolation, session.Feed(Frame(response)).status);
		EXPECT_EQ(EDapSessionState::Failed, session.Snapshot().state);
		EXPECT_TRUE(session.Snapshot().pendingRequests.empty());
		EXPECT_EQ(1U, transport.closeCount);
	}
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	// A header fragment without CRLFCRLF is still valid incomplete stream input.
	// Complete the malformed header so the codec can reject it deterministically.
	EXPECT_EQ(EDapSessionStatus::CodecFailed, session.Feed("not a DAP frame\r\n\r\n").status);
	EXPECT_EQ(EDapSessionOutcome::CodecFailure, session.Snapshot().outcome);
}

TEST(DapSession, CallerDrivenTimeoutAndCancellationFinalizeExactlyOnce)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	const auto timed = session.SendRequest({ .command = "one", .deadline = 9 });
	const auto cancelled = session.SendRequest({ .command = "two" });
	EXPECT_EQ(0U, session.Expire(8));
	EXPECT_EQ(1U, session.Expire(9));
	EXPECT_EQ(EDapSessionStatus::UnknownRequest, session.Cancel(*timed.requestSequence).status);
	EXPECT_EQ(EDapSessionStatus::Cancelled, session.Cancel(*cancelled.requestSequence).status);
	const auto snapshot = session.Snapshot();
	EXPECT_TRUE(snapshot.pendingRequests.empty());
	ASSERT_EQ(2U, snapshot.completedRequests.size());
	EXPECT_EQ(EDapRequestCompletionKind::TimedOut, snapshot.completedRequests[0].kind);
	EXPECT_EQ(EDapRequestCompletionKind::Cancelled, snapshot.completedRequests[1].kind);
}

TEST(DapSession, LateCancelledAndTimedOutResponsesAreSafelyIgnoredButUnknownDuplicatesFail)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	const auto cancelled = session.SendRequest({ .command = "cancelled" });
	const auto timedOut = session.SendRequest({ .command = "timed", .deadline = 5 });
	ASSERT_EQ(EDapSessionStatus::Cancelled, session.Cancel(*cancelled.requestSequence).status);
	ASSERT_EQ(1U, session.Expire(5));
	std::vector<EDapSessionNotificationKind> kinds;
	ASSERT_TRUE(session.Subscribe([&](const auto& change) { kinds.push_back(change.kind); }));
	EXPECT_EQ(EDapSessionStatus::Received, session.Feed(Frame(R"json({"seq":7,"type":"response","request_seq":1,"success":true,"command":"cancelled"})json")).status);
	EXPECT_EQ(EDapSessionStatus::Received, session.Feed(Frame(R"json({"seq":8,"type":"response","request_seq":2,"success":true,"command":"timed"})json")).status);
	EXPECT_EQ(EDapSessionState::Running, session.Snapshot().state);
	EXPECT_EQ(2U, std::count(kinds.begin(), kinds.end(), EDapSessionNotificationKind::LateResponseIgnored));
	EXPECT_EQ(EDapSessionStatus::ProtocolViolation, session.Feed(Frame(R"json({"seq":9,"type":"response","request_seq":1,"success":true,"command":"cancelled"})json")).status);
}

TEST(DapSession, EnforcesPendingCapacityAndTransitionsOnTransportFailure)
{
	FakeTransport transport;
	DapSessionLimits limits;
	limits.maximumPendingRequests = 1;
	CDapSession session(transport, limits);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	EXPECT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "one" }).status);
	EXPECT_EQ(EDapSessionStatus::PendingRequestLimitExceeded, session.SendRequest({ .command = "two" }).status);
	transport.allowSend = false;
	EXPECT_EQ(EDapSessionStatus::TransportFailed, session.NotifyTransportFailure().status);
	EXPECT_EQ(EDapSessionState::Failed, session.Snapshot().state);
	EXPECT_EQ(1U, transport.closeCount);
}

TEST(DapSession, ReportsOutboundSequenceExhaustionWithoutReusingOrMisclassifyingCapacity)
{
	FakeTransport transport;
	DapSessionLimits limits;
	limits.maximumPendingRequests = 2;
	limits.maximumOutgoingSequence = 1;
	CDapSession session(transport, limits);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);

	const auto first = session.SendRequest({ .command = "initialize" });
	const auto exhausted = session.SendRequest({ .command = "threads" });

	ASSERT_EQ(EDapSessionStatus::Sent, first.status);
	EXPECT_EQ(1U, *first.requestSequence);
	EXPECT_EQ(EDapSessionStatus::OutgoingSequenceExhausted, exhausted.status);
	EXPECT_EQ(0U, session.Snapshot().nextOutgoingSequence);
	ASSERT_EQ(1U, transport.sent.size());
}

TEST(DapSession, AcceptsFragmentedInputAndContainsListenerReentrancyAndExceptions)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	const auto request = session.SendRequest({ .command = "threads" });
	std::size_t calls{};
	ASSERT_TRUE(session.Subscribe([&](const auto&) {
		++calls;
		(void)session.Snapshot();
		if (calls == 1) (void)session.Cancel(*request.requestSequence);
		throw std::runtime_error("contained");
	}));
	const auto frame = Frame(R"json({"seq":8,"type":"event","event":"initialized"})json");
	for (const char character : frame) EXPECT_EQ(EDapSessionStatus::Received, session.Feed(std::string_view(&character, 1)).status);
	EXPECT_GE(calls, 1U);
	EXPECT_TRUE(session.Snapshot().pendingRequests.empty());
}

TEST(DapSession, ReentrantCloseDuringTransportSendReturnsTerminalInsteadOfSent)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	transport.onSend = [&session] { EXPECT_EQ(EDapSessionStatus::Closed, session.Close().status); };
	const auto sent = session.SendRequest({ .command = "initialize" });
	EXPECT_EQ(EDapSessionStatus::Closed, sent.status);
	EXPECT_TRUE(session.Snapshot().pendingRequests.empty());
	EXPECT_EQ(1U, transport.closeCount);
}

TEST(DapSession, ConcurrentCloseWaitsUntilPhysicalTransportSendCompletes)
{
	using namespace std::chrono_literals;

	BlockingTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);

	std::promise<DapSessionResult> sendPromise;
	auto sendFuture = sendPromise.get_future();
	std::thread sender([&] { sendPromise.set_value(session.SendRequest({ .command = "initialize" })); });

	const bool sendEntered = transport.WaitForSendEntered(2s);
	EXPECT_TRUE(sendEntered);
	if (!sendEntered) {
		transport.ReleaseSend();
		sender.join();
		return;
	}

	std::promise<void> closeStartedPromise;
	auto closeStarted = closeStartedPromise.get_future();
	std::promise<DapSessionResult> closePromise;
	auto closeFuture = closePromise.get_future();
	std::thread closer([&] {
		closeStartedPromise.set_value();
		closePromise.set_value(session.Close());
	});
	const auto closeStartedStatus = closeStarted.wait_for(2s);

	const auto prematureClose = closeStartedStatus == std::future_status::ready
		? closeFuture.wait_for(50ms) : std::future_status::deferred;
	const auto whileBlocked = session.Snapshot();
	transport.ReleaseSend();

	const auto sendReady = sendFuture.wait_for(2s);
	const auto closeReady = closeFuture.wait_for(2s);
	const auto sendResult = sendFuture.get();
	const auto closeResult = closeFuture.get();
	sender.join();
	closer.join();

	EXPECT_EQ(std::future_status::ready, closeStartedStatus);
	EXPECT_EQ(std::future_status::ready, sendReady);
	EXPECT_EQ(std::future_status::ready, closeReady);
	EXPECT_EQ(std::future_status::timeout, prematureClose);
	EXPECT_EQ(EDapSessionState::Running, whileBlocked.state);
	EXPECT_EQ(1U, whileBlocked.pendingRequests.size());
	EXPECT_EQ(EDapSessionStatus::Sent, sendResult.status);
	EXPECT_EQ(EDapSessionStatus::Closed, closeResult.status);
	EXPECT_EQ(1U, transport.CloseCount());
	EXPECT_EQ(EDapSessionState::Closed, session.Snapshot().state);
}

TEST(DapSession, ExternalStopWaitsForAStartedNotificationCallbackToFinish)
{
	using namespace std::chrono_literals;

	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);

	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool callbackEntered{};
	bool releaseCallback{};
	ASSERT_TRUE(session.Subscribe([&](const DapSessionNotification& notification) {
		if (notification.kind != EDapSessionNotificationKind::RequestSent) return;
		std::unique_lock lock(callbackMutex);
		callbackEntered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return releaseCallback; });
	}));

	std::promise<DapSessionResult> sendPromise;
	auto sendFuture = sendPromise.get_future();
	std::thread sender([&] { sendPromise.set_value(session.SendRequest({ .command = "initialize" })); });
	{
		std::unique_lock lock(callbackMutex);
		const auto entered = callbackChanged.wait_for(lock, 2s, [&] { return callbackEntered; });
		EXPECT_TRUE(entered);
		if (!entered) {
			releaseCallback = true;
			callbackChanged.notify_all();
		}
	}

	std::promise<void> stopStartedPromise;
	auto stopStartedFuture = stopStartedPromise.get_future();
	std::promise<DapSessionResult> stopPromise;
	auto stopFuture = stopPromise.get_future();
	std::thread stopper([&] {
		stopStartedPromise.set_value();
		stopPromise.set_value(session.Stop());
	});

	const auto stopStarted = stopStartedFuture.wait_for(2s);
	const auto prematureStop = stopStarted == std::future_status::ready ? stopFuture.wait_for(50ms) : std::future_status::deferred;
	{
		std::lock_guard lock(callbackMutex);
		releaseCallback = true;
		callbackChanged.notify_all();
	}

	const auto sendReady = sendFuture.wait_for(2s);
	const auto stopReady = stopFuture.wait_for(2s);
	const auto send = sendFuture.get();
	const auto stop = stopFuture.get();
	sender.join();
	stopper.join();

	EXPECT_EQ(std::future_status::ready, stopStarted);
	EXPECT_EQ(std::future_status::timeout, prematureStop);
	EXPECT_EQ(std::future_status::ready, sendReady);
	EXPECT_EQ(std::future_status::ready, stopReady);
	EXPECT_EQ(EDapSessionStatus::Sent, send.status);
	EXPECT_EQ(EDapSessionStatus::Stopped, stop.status);
	EXPECT_FALSE(stop.callbackDrainDeferred);
	EXPECT_EQ(EDapSessionState::Stopped, session.Snapshot().state);
}

TEST(DapSession, ReentrantNotificationTerminalCallsReportDeferredCallbackDrain)
{
	for (const auto operation : { 0, 1 }) {
		FakeTransport transport;
		CDapSession session(transport);
		ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
		std::optional<DapSessionResult> terminal;
		ASSERT_TRUE(session.Subscribe([&](const DapSessionNotification& notification) {
			if (notification.kind != EDapSessionNotificationKind::RequestSent || terminal) return;
			terminal = operation == 0 ? session.Close() : session.Stop();
		}));

		EXPECT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "initialize" }).status);
		ASSERT_TRUE(terminal.has_value());
		EXPECT_TRUE(terminal->callbackDrainDeferred);
		EXPECT_EQ(operation == 0 ? EDapSessionStatus::Closed : EDapSessionStatus::Stopped, terminal->status);
		EXPECT_EQ(operation == 0 ? EDapSessionState::Closed : EDapSessionState::Stopped, session.Snapshot().state);
	}
}

TEST(DapSession, CloseAndStopAreTerminalAndCloseTransportOnlyOnce)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	ASSERT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "disconnect" }).status);
	EXPECT_EQ(EDapSessionStatus::Closed, session.Close().status);
	EXPECT_TRUE(session.Snapshot().pendingRequests.empty());
	EXPECT_EQ(EDapSessionStatus::Closed, session.SendRequest({ .command = "later" }).status);
	EXPECT_EQ(EDapSessionStatus::Stopped, session.Stop().status);
	EXPECT_EQ(EDapSessionStatus::Stopped, session.Stop().status);
	EXPECT_EQ(1U, transport.closeCount);
}

TEST(DapSession, StopDeliversCommittedTerminalNotificationsThenDropsSubscriptions)
{
	FakeTransport transport;
	CDapSession session(transport);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	ASSERT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "disconnect" }).status);
	std::vector<EDapSessionNotificationKind> observed;
	ASSERT_TRUE(session.Subscribe([&observed](const auto& change) { observed.push_back(change.kind); }));
	EXPECT_EQ(EDapSessionStatus::Stopped, session.Stop().status);
	EXPECT_NE(observed.end(), std::find(observed.begin(), observed.end(), EDapSessionNotificationKind::RequestRejected));
	EXPECT_NE(observed.end(), std::find(observed.begin(), observed.end(), EDapSessionNotificationKind::Stopped));
	EXPECT_EQ(std::nullopt, session.Subscribe([](const auto&) {}));
}

TEST(DapSession, CompletedHistoryRemainsObservableWhenNotificationQueueOverflows)
{
	FakeTransport transport;
	DapSessionLimits limits;
	limits.maximumPendingNotifications = 1;
	limits.maximumCompletedRequests = 2;
	CDapSession session(transport, limits);
	ASSERT_EQ(EDapSessionStatus::Started, session.Start().status);
	bool expanded{};
	ASSERT_TRUE(session.Subscribe([&](const auto& change) {
		if (expanded || change.kind != EDapSessionNotificationKind::RequestSent) return;
		expanded = true;
		const auto two = session.SendRequest({ .command = "two" });
		ASSERT_EQ(EDapSessionStatus::Sent, two.status);
		ASSERT_EQ(EDapSessionStatus::Cancelled, session.Cancel(*change.requestSequence).status);
		ASSERT_EQ(EDapSessionStatus::Cancelled, session.Cancel(*two.requestSequence).status);
	}));
	ASSERT_EQ(EDapSessionStatus::Sent, session.SendRequest({ .command = "one" }).status);
	const auto snapshot = session.Snapshot();
	ASSERT_EQ(2U, snapshot.completedRequests.size());
	EXPECT_EQ(EDapRequestCompletionKind::Cancelled, snapshot.completedRequests[0].kind);
	EXPECT_EQ(EDapRequestCompletionKind::Cancelled, snapshot.completedRequests[1].kind);
	EXPECT_GT(snapshot.droppedNotificationCount, 0U);
}

} // namespace
