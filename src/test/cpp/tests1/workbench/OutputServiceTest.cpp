/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "workbench/output/OutputService.h"
#include "workbench/output/OutputServiceNotificationDispatcher.h"

namespace workbench::output {
namespace {

OutputOwner Owner(const char* id, const std::uint64_t generation = 1)
{
	return { .ownerId = id, .generation = generation };
}

OutputOperation Operation(std::string id, const std::optional<std::uint64_t> revision = std::nullopt)
{
	return { .operationId = std::move(id), .expectedRevision = revision };
}

OutputCreateChannelRequest Create(std::string operationId, OutputOwner owner, std::string channelId,
	const EOutputChannelKind kind = EOutputChannelKind::Output)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.label = "Test channel",
		.kind = kind,
		.metadata = { .languageId = std::string("plaintext"), .source = std::string("tests") },
	};
}

OutputTextMutationRequest Text(std::string operationId, OutputOwner owner, std::string channelId, std::string value)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner), .channelId = std::move(channelId), .text = std::move(value) };
}

OutputChannelMutationRequest Channel(std::string operationId, OutputOwner owner, std::string channelId)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner), .channelId = std::move(channelId) };
}

const OutputChannelSnapshot& FindChannel(const OutputServiceSnapshot& snapshot, const std::string_view id)
{
	const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(), [id](const auto& channel) {
		return channel.channelId == id;
	});
	EXPECT_NE(snapshot.channels.end(), found);
	return *found;
}

TEST(OutputServiceNotificationDispatcher, BoundsPendingDeliveryAndRunsCallbacksOutsideModelLock)
{
	using namespace std::chrono_literals;
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition,
		{ .maximumSubscriptions = 4, .maximumPendingNotifications = 1 });
	std::vector<OutputServiceChange> changes;
	std::promise<void> enteredPromise;
	auto enteredFuture = enteredPromise.get_future();
	std::promise<void> releasePromise;
	auto releaseFuture = releasePromise.get_future().share();
	bool callbackOutsideModelLock{};
	bool firstCallback{};

	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange& change) {
		if (!firstCallback) {
			firstCallback = true;
			if (modelMutex.try_lock()) {
				callbackOutsideModelLock = true;
				modelMutex.unlock();
			}
			enteredPromise.set_value();
			releaseFuture.wait();
		}
		changes.push_back(change);
	}));

	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("first.channel"), std::nullopt);
	}
	ASSERT_TRUE(shouldDrain);
	std::thread drainer([&dispatcher] { dispatcher.Drain(); });
	ASSERT_EQ(std::future_status::ready, enteredFuture.wait_for(2s));

	{
		std::lock_guard lock(modelMutex);
		EXPECT_FALSE(dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
			std::string("second.channel"), std::nullopt));
		EXPECT_FALSE(dispatcher.QueueLocked(3, EOutputChangeKind::ContentReplaced,
			std::string("dropped.channel"), std::nullopt));
		EXPECT_EQ(1U, dispatcher.DroppedNotificationCountLocked());
	}

	releasePromise.set_value();
	drainer.join();
	EXPECT_TRUE(callbackOutsideModelLock);
	ASSERT_EQ(2U, changes.size());
	EXPECT_EQ(1U, changes[0].revision);
	EXPECT_EQ(2U, changes[1].revision);
	EXPECT_FALSE(dispatcher.WaitForDrain());
}

TEST(OutputServiceNotificationDispatcher, ReentrantDeliveryRemainsFifoAndNonRecursive)
{
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	std::vector<std::uint64_t> revisions;
	std::size_t callbackDepth{};
	std::size_t maximumCallbackDepth{};
	bool reentrantQueueAccepted{};
	bool reentrantDrainReturned{};

	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange& change) {
		++callbackDepth;
		maximumCallbackDepth = std::max(maximumCallbackDepth, callbackDepth);
		revisions.push_back(change.revision);
		if (change.revision == 1) {
			{
				std::lock_guard lock(modelMutex);
				reentrantQueueAccepted = dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
					std::string("reentrant.channel"), std::nullopt);
			}
			// A callback may reenter the delivery boundary, but the active owner
			// must keep delivery iterative rather than recursively invoking itself.
			dispatcher.Drain();
			reentrantDrainReturned = true;
		}
		--callbackDepth;
	}));

	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("first.channel"), std::nullopt);
	}
	ASSERT_TRUE(shouldDrain);
	dispatcher.Drain();

	ASSERT_EQ(2U, revisions.size());
	EXPECT_EQ(1U, revisions[0]);
	EXPECT_EQ(2U, revisions[1]);
	EXPECT_FALSE(reentrantQueueAccepted);
	EXPECT_TRUE(reentrantDrainReturned);
	EXPECT_EQ(1U, maximumCallbackDepth);
}

TEST(OutputServiceNotificationDispatcher, ContainsThrowingListenerAndContinuesWithLaterEvents)
{
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	std::vector<std::uint64_t> revisions;

	ASSERT_TRUE(dispatcher.Subscribe([](const OutputServiceChange&) {
		throw std::runtime_error("expected dispatcher listener failure");
	}));
	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange& change) {
		revisions.push_back(change.revision);
	}));

	for (const auto revision : { 1U, 2U }) {
		bool shouldDrain{};
		{
			std::lock_guard lock(modelMutex);
			shouldDrain = dispatcher.QueueLocked(revision, EOutputChangeKind::ChannelCreated,
				std::string("throwing.channel"), std::nullopt);
		}
		EXPECT_TRUE(shouldDrain);
		if (shouldDrain) dispatcher.Drain();
	}

	ASSERT_EQ(2U, revisions.size());
	EXPECT_EQ(1U, revisions[0]);
	EXPECT_EQ(2U, revisions[1]);
}

TEST(OutputServiceNotificationDispatcher, ContainsListenerCopyFailureAndContinuesWithLaterSubscribers)
{
	struct CopyState final {
		bool throwOnCopy{};
	};
	struct ThrowingCopyListener final {
		std::shared_ptr<CopyState> state;

		ThrowingCopyListener(std::shared_ptr<CopyState> value)
			: state(std::move(value))
		{
		}
		ThrowingCopyListener(const ThrowingCopyListener& other)
			: state(other.state)
		{
			if (state->throwOnCopy) throw std::runtime_error("expected listener copy failure");
		}
		ThrowingCopyListener(ThrowingCopyListener&&) noexcept = default;
		void operator()(const OutputServiceChange&) const noexcept {}
	};

	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	const auto copyState = std::make_shared<CopyState>();
	ASSERT_TRUE(dispatcher.Subscribe(ThrowingCopyListener(copyState)));
	std::vector<std::uint64_t> revisions;
	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange& change) {
		revisions.push_back(change.revision);
	}));
	copyState->throwOnCopy = true;

	for (const auto revision : { 1U, 2U }) {
		bool shouldDrain{};
		{
			std::lock_guard lock(modelMutex);
			shouldDrain = dispatcher.QueueLocked(revision, EOutputChangeKind::ChannelCreated,
				std::string("copy-failure.channel"), std::nullopt);
		}
		EXPECT_TRUE(shouldDrain);
		dispatcher.Drain();
	}

	ASSERT_EQ(2U, revisions.size());
	EXPECT_EQ(1U, revisions[0]);
	EXPECT_EQ(2U, revisions[1]);
	{
		std::lock_guard lock(modelMutex);
		EXPECT_EQ(2U, dispatcher.DroppedNotificationCountLocked());
	}
}

TEST(OutputServiceNotificationDispatcher, UnsubscribeDoesNotDrainActiveCallback)
{
	using namespace std::chrono_literals;
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	std::vector<std::uint64_t> revisions;
	std::promise<void> enteredPromise;
	auto enteredFuture = enteredPromise.get_future();
	std::promise<void> releasePromise;
	auto releaseFuture = releasePromise.get_future().share();
	std::optional<OutputServiceSubscriptionId> subscriptionId;

	subscriptionId = dispatcher.Subscribe([&](const OutputServiceChange& change) {
		revisions.push_back(change.revision);
		enteredPromise.set_value();
		releaseFuture.wait();
	});
	ASSERT_TRUE(subscriptionId);

	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("active.channel"), std::nullopt);
	}
	ASSERT_TRUE(shouldDrain);
	std::thread drainer([&dispatcher] { dispatcher.Drain(); });
	const auto callbackEntered = enteredFuture.wait_for(2s) == std::future_status::ready;
	EXPECT_TRUE(callbackEntered);

	std::promise<void> unsubscribePromise;
	auto unsubscribeFuture = unsubscribePromise.get_future();
	std::thread unsubscriber([&dispatcher, subscriptionId, &unsubscribePromise] {
		dispatcher.Unsubscribe(*subscriptionId);
		unsubscribePromise.set_value();
	});
	const auto unsubscribeReturnedWhileActive = unsubscribeFuture.wait_for(2s) == std::future_status::ready;
	EXPECT_TRUE(unsubscribeReturnedWhileActive);

	releasePromise.set_value();
	drainer.join();
	unsubscriber.join();

	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
			std::string("after-unsubscribe.channel"), std::nullopt);
	}
	EXPECT_TRUE(shouldDrain);
	dispatcher.Drain();
	ASSERT_EQ(1U, revisions.size());
	EXPECT_EQ(1U, revisions.front());
}

TEST(OutputServiceNotificationDispatcher, ExternalStopWaitsForCallbackAndRepeatedStopIsSafe)
{
	using namespace std::chrono_literals;
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	std::promise<void> enteredPromise;
	auto enteredFuture = enteredPromise.get_future();
	std::promise<void> releasePromise;
	auto releaseFuture = releasePromise.get_future().share();

	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange&) {
		enteredPromise.set_value();
		releaseFuture.wait();
	}));
	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("stop.channel"), std::nullopt);
	}
	ASSERT_TRUE(shouldDrain);
	std::thread drainer([&dispatcher] { dispatcher.Drain(); });
	const auto callbackEntered = enteredFuture.wait_for(2s) == std::future_status::ready;
	EXPECT_TRUE(callbackEntered);

	std::promise<void> stopStartedPromise;
	auto stopStartedFuture = stopStartedPromise.get_future();
	std::promise<bool> stopPromise;
	auto stopFuture = stopPromise.get_future();
	std::thread stopper([&dispatcher, &modelMutex, &stopStartedPromise, &stopPromise] {
		{
			std::lock_guard lock(modelMutex);
			dispatcher.StopLocked();
		}
		stopStartedPromise.set_value();
		stopPromise.set_value(dispatcher.WaitForDrain());
	});
	const auto stopStarted = stopStartedFuture.wait_for(2s) == std::future_status::ready;
	EXPECT_TRUE(stopStarted);
	const auto stopBlockedOnCallback = stopFuture.wait_for(100ms) == std::future_status::timeout;
	EXPECT_TRUE(stopBlockedOnCallback);

	releasePromise.set_value();
	drainer.join();
	stopper.join();
	EXPECT_FALSE(stopFuture.get());

	{
		std::lock_guard lock(modelMutex);
		dispatcher.StopLocked();
		EXPECT_FALSE(dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
			std::string("after-stop.channel"), std::nullopt));
	}
	EXPECT_FALSE(dispatcher.WaitForDrain());
}

TEST(OutputServiceNotificationDispatcher, CallbackOriginStopDefersDrainAndIsIdempotent)
{
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition);
	bool deferred{};
	bool queueAfterStop{};

	ASSERT_TRUE(dispatcher.Subscribe([&](const OutputServiceChange&) {
		{
			std::lock_guard lock(modelMutex);
			dispatcher.StopLocked();
			queueAfterStop = dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
				std::string("after-stop.channel"), std::nullopt);
		}
		deferred = dispatcher.WaitForDrain();
	}));
	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("callback-stop.channel"), std::nullopt);
	}
	ASSERT_TRUE(shouldDrain);
	dispatcher.Drain();

	EXPECT_TRUE(deferred);
	EXPECT_FALSE(queueAfterStop);
	EXPECT_FALSE(dispatcher.WaitForDrain());
	{
		std::lock_guard lock(modelMutex);
		dispatcher.StopLocked();
	}
	EXPECT_FALSE(dispatcher.WaitForDrain());
}

TEST(OutputServiceNotificationDispatcher, ZeroLimitsNormalizeToOne)
{
	std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher dispatcher(modelMutex, drainCondition,
		{ .maximumSubscriptions = 0, .maximumPendingNotifications = 0 });
	std::size_t callbacks{};
	ASSERT_TRUE(dispatcher.Subscribe([&callbacks](const OutputServiceChange&) { ++callbacks; }));
	EXPECT_FALSE(dispatcher.Subscribe([](const OutputServiceChange&) {}));

	bool shouldDrain{};
	{
		std::lock_guard lock(modelMutex);
		shouldDrain = dispatcher.QueueLocked(1, EOutputChangeKind::ChannelCreated,
			std::string("zero-limit.channel"), std::nullopt);
		EXPECT_TRUE(shouldDrain);
		EXPECT_FALSE(dispatcher.QueueLocked(2, EOutputChangeKind::ContentAppended,
			std::string("dropped.channel"), std::nullopt));
		EXPECT_EQ(1U, dispatcher.DroppedNotificationCountLocked());
	}
	dispatcher.Drain();
	EXPECT_EQ(1U, callbacks);
}

TEST(OutputService, ZeroAdvisoryLimitsUseFailClosedNonzeroDefaults)
{
	OutputServiceLimits limits;
	limits.maximumSubscriptions = 0;
	limits.maximumPendingNotifications = 0;
	OutputService service(limits);
	std::size_t notifications{};
	ASSERT_TRUE(service.Subscribe([&notifications](const OutputServiceChange&) { ++notifications; }));
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("zero-limits", Owner("zero-limits"), "zero-limits.channel")).status);
	EXPECT_EQ(1U, notifications);
	EXPECT_EQ(0U, service.Snapshot().droppedNotificationCount);
}

TEST(OutputService, CreatesDeterministicSnapshotAndShowPreservesFocusAsProjectionMetadata)
{
	OutputService service;
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-z", Owner("owner.z"), "z.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-a", Owner("owner.a"), "a.channel")).status);
	const auto created = service.Snapshot();
	ASSERT_EQ(2U, created.channels.size());
	EXPECT_EQ("a.channel", created.channels[0].channelId);
	EXPECT_EQ("z.channel", created.channels[1].channelId);
	EXPECT_EQ("z.channel", *created.activeChannelId);

	const OutputShowChannelRequest show{ .operation = Operation("show-z"), .owner = Owner("owner.z"), .channelId = "z.channel", .preserveFocus = true };
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show(show).status);
	const auto shown = service.Snapshot();
	EXPECT_EQ("z.channel", *shown.activeChannelId);
	EXPECT_TRUE(FindChannel(shown, "z.channel").visible);
	EXPECT_TRUE(FindChannel(shown, "z.channel").lastShowPreservedFocus);
}

TEST(OutputService, FreshShowAlwaysPublishesRevealIntentButExactReplayDoesNot)
{
	OutputService service;
	const auto owner = Owner("extension.reveal");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", owner, "reveal.channel")).status);
	std::vector<OutputServiceChange> changes;
	ASSERT_TRUE(service.Subscribe([&changes](const OutputServiceChange& change) { changes.push_back(change); }));
	const OutputShowChannelRequest show{ .operation = Operation("show-first"), .owner = owner,
		.channelId = "reveal.channel", .preserveFocus = true };
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show(show).status);
	const auto afterFirst = service.Snapshot();
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show({ .operation = Operation("show-fresh"), .owner = owner,
		.channelId = "reveal.channel", .preserveFocus = true }).status);
	const auto afterFresh = service.Snapshot();
	EXPECT_EQ(afterFirst.revision + 1, afterFresh.revision);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.Show(show).status);
	EXPECT_EQ(afterFresh.revision, service.Snapshot().revision);
	ASSERT_EQ(2u, changes.size());
	EXPECT_EQ(EOutputChangeKind::ChannelShown, changes[0].kind);
	EXPECT_EQ(EOutputChangeKind::ChannelShown, changes[1].kind);
}

TEST(OutputService, AcceptedCommitFeedPublishesFreshSucceededCommitsOnceWithCopiedRequests)
{
	OutputService service;
	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, bootstrap->state);
	EXPECT_EQ(0U, bootstrap->cursor.sequence);

	const auto owner = Owner("feed.copies");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create", owner, "feed.channel")).status);
	OutputTextMutationRequest append = Text("append", owner, "feed.channel", "before");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(append).status);
	append.text = "after";
	append.channelId = "changed.after.commit";
	const OutputShowChannelRequest show{ .operation = Operation("show-first"), .owner = owner,
		.channelId = "feed.channel", .preserveFocus = true };
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show(show).status);
	ASSERT_EQ(EOutputOperationStatus::Replayed, service.Show(show).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show({ .operation = Operation("show-fresh"), .owner = owner,
		.channelId = "feed.channel", .preserveFocus = true }).status);

	ASSERT_EQ(4U, events.size());
	for (std::size_t index = 0; index < events.size(); ++index) {
		ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[index].state);
		ASSERT_TRUE(events[index].commit);
		EXPECT_EQ(bootstrap->cursor.sequence + index + 1, events[index].commit->sequence);
		EXPECT_EQ(EOutputOperationStatus::Succeeded, events[index].commit->result.status);
		EXPECT_EQ(events[index].commit->result.revision, events[index].commit->postCommitRevision);
		EXPECT_EQ(events[index].commit->sequence, events[index].cursor.sequence);
	}

	EXPECT_EQ(EOutputAcceptedCommitKind::CreateChannel, events[0].commit->kind);
	const auto& created = std::get<OutputCreateChannelRequest>(events[0].commit->data);
	EXPECT_EQ("feed.channel", created.channelId);
	EXPECT_EQ(owner, created.owner);
	EXPECT_EQ("create", created.operation.operationId);

	EXPECT_EQ(EOutputAcceptedCommitKind::AppendOutput, events[1].commit->kind);
	const auto& appended = std::get<OutputTextMutationRequest>(events[1].commit->data);
	EXPECT_EQ("before", appended.text);
	EXPECT_EQ("feed.channel", appended.channelId);

	EXPECT_EQ(EOutputAcceptedCommitKind::Show, events[2].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::Show, events[3].commit->kind);
	EXPECT_EQ("show-first", std::get<OutputShowChannelRequest>(events[2].commit->data).operation.operationId);
	EXPECT_EQ("show-fresh", std::get<OutputShowChannelRequest>(events[3].commit->data).operation.operationId);
}

TEST(OutputService, AcceptedCommitFeedMapsEveryAcceptedKindToItsRequestVariant)
{
	OutputService service;
	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);

	const auto owner = Owner("feed.kinds");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-output", owner, "kinds.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.AppendOutput(Text("append-output", owner, "kinds.output", "before")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.ReplaceOutput(Text("replace-output", owner, "kinds.output", "after")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-log", owner, "kinds.log", EOutputChannelKind::Log)).status);
	const OutputLogMutationRequest appendLog{
		.operation = Operation("append-log"),
		.owner = owner,
		.channelId = "kinds.log",
		.entries = { { .level = EOutputLogLevel::Warning, .message = "warning", .source = std::string("compiler") } },
	};
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendLog(appendLog).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.Clear(Channel("clear", owner, "kinds.log")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.Show({ .operation = Operation("show"), .owner = owner, .channelId = "kinds.output", .preserveFocus = true }).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.Hide(Channel("hide", owner, "kinds.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.Dispose(Channel("dispose", owner, "kinds.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.DisposeOwner({ .operation = Operation("dispose-owner"), .owner = owner }).status);

	const std::vector expectedKinds{
		EOutputAcceptedCommitKind::CreateChannel,
		EOutputAcceptedCommitKind::AppendOutput,
		EOutputAcceptedCommitKind::ReplaceOutput,
		EOutputAcceptedCommitKind::CreateChannel,
		EOutputAcceptedCommitKind::AppendLog,
		EOutputAcceptedCommitKind::Clear,
		EOutputAcceptedCommitKind::Show,
		EOutputAcceptedCommitKind::Hide,
		EOutputAcceptedCommitKind::Dispose,
		EOutputAcceptedCommitKind::DisposeOwner,
	};
	ASSERT_EQ(expectedKinds.size(), events.size());
	for (std::size_t index = 0; index < events.size(); ++index) {
		ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[index].state);
		ASSERT_TRUE(events[index].commit);
		EXPECT_EQ(expectedKinds[index], events[index].commit->kind);
		EXPECT_EQ(bootstrap->cursor.sequence + index + 1, events[index].commit->sequence);
		EXPECT_EQ(EOutputOperationStatus::Succeeded, events[index].commit->result.status);
	}

	ASSERT_TRUE(std::holds_alternative<OutputCreateChannelRequest>(events[0].commit->data));
	EXPECT_EQ("create-output", std::get<OutputCreateChannelRequest>(events[0].commit->data).operation.operationId);
	EXPECT_EQ("kinds.output", std::get<OutputCreateChannelRequest>(events[0].commit->data).channelId);
	ASSERT_TRUE(std::holds_alternative<OutputTextMutationRequest>(events[1].commit->data));
	EXPECT_EQ("append-output", std::get<OutputTextMutationRequest>(events[1].commit->data).operation.operationId);
	EXPECT_EQ("before", std::get<OutputTextMutationRequest>(events[1].commit->data).text);
	ASSERT_TRUE(std::holds_alternative<OutputTextMutationRequest>(events[2].commit->data));
	EXPECT_EQ("replace-output", std::get<OutputTextMutationRequest>(events[2].commit->data).operation.operationId);
	EXPECT_EQ("after", std::get<OutputTextMutationRequest>(events[2].commit->data).text);
	ASSERT_TRUE(std::holds_alternative<OutputCreateChannelRequest>(events[3].commit->data));
	EXPECT_EQ(EOutputChannelKind::Log, std::get<OutputCreateChannelRequest>(events[3].commit->data).kind);
	ASSERT_TRUE(std::holds_alternative<OutputLogMutationRequest>(events[4].commit->data));
	EXPECT_EQ("append-log", std::get<OutputLogMutationRequest>(events[4].commit->data).operation.operationId);
	EXPECT_EQ("warning", std::get<OutputLogMutationRequest>(events[4].commit->data).entries.front().message);
	ASSERT_TRUE(std::holds_alternative<OutputChannelMutationRequest>(events[5].commit->data));
	EXPECT_EQ("clear", std::get<OutputChannelMutationRequest>(events[5].commit->data).operation.operationId);
	ASSERT_TRUE(std::holds_alternative<OutputShowChannelRequest>(events[6].commit->data));
	EXPECT_TRUE(std::get<OutputShowChannelRequest>(events[6].commit->data).preserveFocus);
	ASSERT_TRUE(std::holds_alternative<OutputChannelMutationRequest>(events[7].commit->data));
	EXPECT_EQ("hide", std::get<OutputChannelMutationRequest>(events[7].commit->data).operation.operationId);
	ASSERT_TRUE(std::holds_alternative<OutputChannelMutationRequest>(events[8].commit->data));
	EXPECT_EQ("dispose", std::get<OutputChannelMutationRequest>(events[8].commit->data).operation.operationId);
	ASSERT_TRUE(std::holds_alternative<OutputDisposeOwnerRequest>(events[9].commit->data));
	EXPECT_EQ(owner, std::get<OutputDisposeOwnerRequest>(events[9].commit->data).owner);
}

TEST(OutputService, AcceptedCommitFeedPublishesOwnerGenerationReplacementAndDisposeOwnerTombstone)
{
	OutputService service;
	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);

	const auto oldOwner = Owner("feed.generations", 1);
	const auto newOwner = Owner("feed.generations", 2);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-old", oldOwner, "generation.channel")).status);
	const auto createNew = Create("create-new", newOwner, "generation.channel");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(createNew).status);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.CreateChannel(createNew).status);

	// A stale owner-generation request is rejected and must never become a feed commit.
	const OutputDisposeOwnerRequest disposeOld{ .operation = Operation("dispose-old"), .owner = oldOwner };
	EXPECT_EQ(EOutputOperationStatus::Conflict, service.DisposeOwner(disposeOld).status);

	const OutputDisposeOwnerRequest disposeNew{ .operation = Operation("dispose-new"), .owner = newOwner };
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.DisposeOwner(disposeNew).status);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.DisposeOwner(disposeNew).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		service.CreateChannel(Create("same-generation-after-dispose", newOwner, "same-generation.channel")).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		service.CreateChannel(Create("old-after-dispose", oldOwner, "old-generation.channel")).status);

	const auto newestOwner = Owner("feed.generations", 3);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-newest", newestOwner, "newest.channel")).status);

	ASSERT_EQ(4U, events.size());
	const std::vector expectedKinds{
		EOutputAcceptedCommitKind::CreateChannel,
		EOutputAcceptedCommitKind::CreateChannel,
		EOutputAcceptedCommitKind::DisposeOwner,
		EOutputAcceptedCommitKind::CreateChannel,
	};
	for (std::size_t index = 0; index < events.size(); ++index) {
		ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[index].state);
		ASSERT_TRUE(events[index].commit);
		EXPECT_EQ(expectedKinds[index], events[index].commit->kind);
		EXPECT_EQ(bootstrap->cursor.sequence + index + 1, events[index].commit->sequence);
		EXPECT_EQ(EOutputOperationStatus::Succeeded, events[index].commit->result.status);
	}
	ASSERT_TRUE(std::holds_alternative<OutputCreateChannelRequest>(events[0].commit->data));
	EXPECT_EQ(oldOwner, std::get<OutputCreateChannelRequest>(events[0].commit->data).owner);
	ASSERT_TRUE(std::holds_alternative<OutputCreateChannelRequest>(events[1].commit->data));
	EXPECT_EQ(newOwner, std::get<OutputCreateChannelRequest>(events[1].commit->data).owner);
	EXPECT_EQ("generation.channel", std::get<OutputCreateChannelRequest>(events[1].commit->data).channelId);
	ASSERT_TRUE(std::holds_alternative<OutputDisposeOwnerRequest>(events[2].commit->data));
	EXPECT_EQ(newOwner, std::get<OutputDisposeOwnerRequest>(events[2].commit->data).owner);
	ASSERT_TRUE(std::holds_alternative<OutputCreateChannelRequest>(events[3].commit->data));
	EXPECT_EQ(newestOwner, std::get<OutputCreateChannelRequest>(events[3].commit->data).owner);
}

TEST(OutputService, AcceptedCommitFeedTreatsAnEvictedOperationIdAsFreshAgain)
{
	OutputServiceLimits limits;
	limits.maximumRememberedOperations = 1;
	OutputService service(limits);
	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);

	const auto owner = Owner("feed.eviction");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create", owner, "eviction.channel")).status);
	const OutputShowChannelRequest repeatableShow{ .operation = Operation("repeatable-show"), .owner = owner,
		.channelId = "eviction.channel", .preserveFocus = true };
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show(repeatableShow).status);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.Show(repeatableShow).status);
	ASSERT_EQ(2U, events.size());

	// This fresh accepted operation evicts repeatable-show from the bounded replay journal.
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.AppendOutput(Text("evict-show", owner, "eviction.channel", "content")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show(repeatableShow).status);

	ASSERT_EQ(4U, events.size());
	EXPECT_EQ(EOutputAcceptedCommitKind::CreateChannel, events[0].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::Show, events[1].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::AppendOutput, events[2].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::Show, events[3].commit->kind);
	EXPECT_EQ("repeatable-show", std::get<OutputShowChannelRequest>(events[1].commit->data).operation.operationId);
	EXPECT_EQ("repeatable-show", std::get<OutputShowChannelRequest>(events[3].commit->data).operation.operationId);
	EXPECT_EQ(events[1].commit->sequence + 2, events[3].commit->sequence);
	for (const auto& event : events) {
		EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, event.state);
		ASSERT_TRUE(event.commit);
		EXPECT_EQ(EOutputOperationStatus::Succeeded, event.commit->result.status);
	}
}

TEST(OutputService, AcceptedCommitFeedExcludesReplayRejectConflictStaleNotApplicableAndStoppedOperations)
{
	OutputServiceLimits limits;
	limits.maximumPayloadBytes = 4;
	OutputService service(limits);
	std::vector<OutputAcceptedCommitEvent> events;
	ASSERT_TRUE(service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	}));

	const auto owner = Owner("feed.terminals");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create", owner, "terminal.channel")).status);
	const auto append = Text("append", owner, "terminal.channel", "ok");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(append).status);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.AppendOutput(append).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		service.AppendOutput(Text("append", owner, "terminal.channel", "no")).status);

	const auto currentRevision = service.Snapshot().revision;
	EXPECT_EQ(EOutputOperationStatus::StaleRevision,
		service.AppendOutput({ .operation = Operation("stale", currentRevision - 1), .owner = owner,
			.channelId = "terminal.channel", .text = "x" }).status);
	EXPECT_EQ(EOutputOperationStatus::Rejected,
		service.AppendOutput(Text("rejected", owner, "terminal.channel", "12345")).status);
	EXPECT_EQ(EOutputOperationStatus::NotApplicable,
		service.Clear(Channel("missing", owner, "missing.channel")).status);

	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Stop().status);
	EXPECT_EQ(EOutputOperationStatus::Stopped,
		service.CreateChannel(Create("stopped", owner, "stopped.channel")).status);

	ASSERT_EQ(3U, events.size());
	ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[0].state);
	ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[1].state);
	ASSERT_TRUE(events[0].commit);
	ASSERT_TRUE(events[1].commit);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Stopped, events[2].state);
	EXPECT_FALSE(events[2].commit);
	EXPECT_EQ(EOutputAcceptedCommitKind::CreateChannel, events[0].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::AppendOutput, events[1].commit->kind);
	for (const auto& event : events) {
		if (!event.commit) continue;
		EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, event.state);
		EXPECT_EQ(EOutputOperationStatus::Succeeded, event.commit->result.status);
	}
}

TEST(OutputService, AcceptedCommitFeedPreservesOrderingAndBootstrapCursor)
{
	OutputService service;
	const auto owner = Owner("feed.order");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("before-subscribe", owner, "before.channel")).status);

	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, bootstrap->state);
	ASSERT_EQ(1U, bootstrap->snapshot.channels.size());
	EXPECT_EQ("before.channel", bootstrap->snapshot.channels.front().channelId);
	EXPECT_EQ(bootstrap->snapshot.revision, service.Snapshot().revision);

	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("second", owner, "second.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("third", owner, "third.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.Show({ .operation = Operation("show-third"), .owner = owner, .channelId = "third.channel" }).status);

	ASSERT_EQ(3U, events.size());
	std::uint64_t previousSequence = bootstrap->cursor.sequence;
	std::uint64_t previousRevision = bootstrap->snapshot.revision;
	for (const auto& event : events) {
		ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, event.state);
		ASSERT_TRUE(event.commit);
		EXPECT_EQ(previousSequence + 1, event.commit->sequence);
		EXPECT_EQ(event.commit->sequence, event.cursor.sequence);
		EXPECT_GT(event.commit->postCommitRevision, previousRevision);
		previousSequence = event.commit->sequence;
		previousRevision = event.commit->postCommitRevision;
	}
}

TEST(OutputService, AcceptedCommitFeedBootstrapIsAtomicAndHasNoConcurrentCommitGap)
{
	OutputServiceLimits limits;
	limits.maximumChannels = 64;
	limits.maximumAcceptedCommitFeedEntries = 64;
	OutputService service(limits);
	std::mutex advisoryMutex;
	std::condition_variable advisoryChanged;
	bool advisoryEntered{};
	bool releaseAdvisory{};
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange& change) {
		if (change.kind != EOutputChangeKind::ChannelCreated || !change.channelId || *change.channelId != "race.first") return;
		std::unique_lock lock(advisoryMutex);
		advisoryEntered = true;
		advisoryChanged.notify_all();
		advisoryChanged.wait(lock, [&] { return releaseAdvisory; });
	}));

	std::thread writer([&] {
		ASSERT_EQ(EOutputOperationStatus::Succeeded,
			service.CreateChannel(Create("race-first", Owner("feed.race"), "race.first")).status);
		for (std::size_t index = 0; index < 16; ++index) {
			EXPECT_EQ(EOutputOperationStatus::Succeeded,
				service.CreateChannel(Create("race-" + std::to_string(index), Owner("feed.race"),
					"race." + std::to_string(index))).status);
		}
	});
	{
		std::unique_lock lock(advisoryMutex);
		ASSERT_TRUE(advisoryChanged.wait_for(lock, std::chrono::seconds(2), [&] { return advisoryEntered; }));
	}

	std::vector<OutputAcceptedCommitEvent> events;
	const auto bootstrap = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(bootstrap);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, bootstrap->state);
	ASSERT_TRUE(std::any_of(bootstrap->snapshot.channels.begin(), bootstrap->snapshot.channels.end(), [](const auto& channel) {
		return channel.channelId == "race.first";
	}));

	{
		std::lock_guard lock(advisoryMutex);
		releaseAdvisory = true;
		advisoryChanged.notify_all();
	}
	writer.join();
	ASSERT_EQ(16U, events.size());
	std::uint64_t expectedSequence = bootstrap->cursor.sequence + 1;
	for (const auto& event : events) {
		ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, event.state);
		ASSERT_TRUE(event.commit);
		EXPECT_EQ(expectedSequence++, event.commit->sequence);
	}
}

TEST(OutputService, AcceptedCommitFeedPublishesAnExplicitGapOnBoundedOverflowAndRebootstrapRecovers)
{
	using namespace std::chrono_literals;
	OutputServiceLimits limits;
	limits.maximumChannels = 16;
	limits.maximumAcceptedCommitFeedEntries = 1;
	OutputService service(limits);
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	std::vector<OutputAcceptedCommitEvent> events;
	std::uint64_t firstSequence{};
	const auto firstBootstrap = service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent& event) {
		{
			std::lock_guard lock(callbackMutex);
			events.push_back(event);
		}
		if (event.state != EOutputAcceptedCommitFeedState::Live || !event.commit
			|| event.commit->sequence != firstSequence + 1) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	});
	ASSERT_TRUE(firstBootstrap);
	firstSequence = firstBootstrap->cursor.sequence;

	std::thread first([&] {
		EXPECT_EQ(EOutputOperationStatus::Succeeded,
			service.CreateChannel(Create("gap-first", Owner("feed.gap"), "gap.first")).status);
	});
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("gap-second", Owner("feed.gap"), "gap.second")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("gap-third", Owner("feed.gap"), "gap.third")).status);
	EXPECT_EQ(0U, service.Snapshot().droppedNotificationCount);

	{
		std::lock_guard lock(callbackMutex);
		release = true;
		callbackChanged.notify_all();
	}
	first.join();

	ASSERT_EQ(2U, events.size());
	ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[0].state);
	ASSERT_TRUE(events[0].commit);
	EXPECT_EQ(firstBootstrap->cursor.sequence + 1, events[0].commit->sequence);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Gap, events[1].state);
	EXPECT_FALSE(events[1].commit);
	EXPECT_EQ(2U, events[1].missingFromSequence);
	EXPECT_EQ(2U, events[1].missingToSequence);
	EXPECT_EQ(2U, events[1].cursor.sequence);

	service.UnsubscribeAcceptedCommits(firstBootstrap->subscriptionId);
	service.UnsubscribeAcceptedCommits(firstBootstrap->subscriptionId);
	std::vector<OutputAcceptedCommitEvent> recoveredEvents;
	const auto recovered = service.SubscribeAcceptedCommits([&recoveredEvents](const OutputAcceptedCommitEvent& event) {
		recoveredEvents.push_back(event);
	});
	ASSERT_TRUE(recovered);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, recovered->state);
	EXPECT_EQ(3U, recovered->cursor.sequence);
	EXPECT_EQ(3U, recovered->snapshot.channels.size());
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("gap-recovered", Owner("feed.gap"), "gap.recovered")).status);
	ASSERT_EQ(1U, recoveredEvents.size());
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, recoveredEvents.front().state);
	ASSERT_TRUE(recoveredEvents.front().commit);
	EXPECT_EQ(4U, recoveredEvents.front().commit->sequence);
}

TEST(OutputService, AcceptedCommitFeedCallbacksRunOutsideTheLockReenterSafelyAndContainListenerExceptions)
{
	OutputService service;
	const auto owner = Owner("feed.reentrant");
	std::vector<OutputAcceptedCommitEvent> events;
	std::size_t callbackDepth{};
	std::size_t maximumCallbackDepth{};
	std::optional<OutputOperationResult> nestedResult;
	std::optional<OutputServiceSnapshot> observedSnapshot;
	const auto recorder = service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent& event) {
		++callbackDepth;
		maximumCallbackDepth = std::max(maximumCallbackDepth, callbackDepth);
		events.push_back(event);
		if (event.state == EOutputAcceptedCommitFeedState::Live && event.commit
			&& event.commit->kind == EOutputAcceptedCommitKind::CreateChannel) {
			observedSnapshot = service.Snapshot();
			nestedResult = service.AppendOutput(Text("nested", owner, "reentrant.channel", "nested"));
		}
		--callbackDepth;
	});
	ASSERT_TRUE(recorder);
	ASSERT_TRUE(service.SubscribeAcceptedCommits([](const OutputAcceptedCommitEvent&) {
		throw std::runtime_error("accepted-feed listener failure");
	}));

	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create", owner, "reentrant.channel")).status);
	ASSERT_TRUE(observedSnapshot);
	EXPECT_EQ(1U, observedSnapshot->channels.size());
	EXPECT_EQ("reentrant.channel", observedSnapshot->channels.front().channelId);
	ASSERT_TRUE(nestedResult);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, nestedResult->status);
	EXPECT_EQ(1U, maximumCallbackDepth);
	ASSERT_EQ(2U, events.size());
	ASSERT_TRUE(events[0].commit);
	ASSERT_TRUE(events[1].commit);
	EXPECT_EQ(EOutputAcceptedCommitKind::CreateChannel, events[0].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitKind::AppendOutput, events[1].commit->kind);
	EXPECT_EQ(events[0].commit->sequence + 1, events[1].commit->sequence);
}

TEST(OutputService, AcceptedCommitFeedUnsubscribeIsIdempotentAndStaleIdsCannotRemoveAnotherListener)
{
	OutputService service;
	std::size_t firstCalls{};
	std::size_t secondCalls{};
	const auto first = service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent&) { ++firstCalls; });
	ASSERT_TRUE(first);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("first", Owner("feed.unsubscribe"), "unsubscribe.first")).status);
	EXPECT_EQ(1U, firstCalls);

	service.UnsubscribeAcceptedCommits(first->subscriptionId);
	service.UnsubscribeAcceptedCommits(first->subscriptionId);
	service.UnsubscribeAcceptedCommits(first->subscriptionId + 1000);
	const auto second = service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent&) { ++secondCalls; });
	ASSERT_TRUE(second);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("second", Owner("feed.unsubscribe"), "unsubscribe.second")).status);
	EXPECT_EQ(1U, firstCalls);
	EXPECT_EQ(1U, secondCalls);
	service.UnsubscribeAcceptedCommits(second->subscriptionId);
	service.UnsubscribeAcceptedCommits(first->subscriptionId);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("third", Owner("feed.unsubscribe"), "unsubscribe.third")).status);
	EXPECT_EQ(1U, secondCalls);
}

TEST(OutputService, ReentrantAcceptedCommitFeedStopDefersItsOwnDrainAndPublishesStoppedTerminal)
{
	OutputService service;
	std::vector<EOutputAcceptedCommitFeedState> states;
	std::optional<OutputOperationResult> stopResult;
	const auto subscription = service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent& event) {
		states.push_back(event.state);
		if (event.state == EOutputAcceptedCommitFeedState::Live && !stopResult) stopResult = service.Stop();
	});
	ASSERT_TRUE(subscription);
	EXPECT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create", Owner("feed.stop.reentrant"), "stop.reentrant")).status);
	ASSERT_TRUE(stopResult);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, stopResult->status);
	EXPECT_TRUE(stopResult->callbackDrainDeferred);
	EXPECT_TRUE(service.Snapshot().stopped);
	ASSERT_EQ(2U, states.size());
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, states[0]);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Stopped, states[1]);
	EXPECT_FALSE(service.SubscribeAcceptedCommits([](const OutputAcceptedCommitEvent&) {}));
	service.UnsubscribeAcceptedCommits(subscription->subscriptionId);
}

TEST(OutputService, AdvisoryCallbackStopDrainsAcceptedCommitBeforeStoppedTerminal)
{
	OutputService service;
	std::vector<OutputAcceptedCommitEvent> events;
	ASSERT_TRUE(service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	}));
	std::optional<OutputOperationResult> stopResult;
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange&) {
		stopResult = service.Stop();
	}));

	EXPECT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("advisory-stop", Owner("feed.stop.advisory"), "stop.advisory")).status);
	ASSERT_TRUE(stopResult);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, stopResult->status);
	EXPECT_TRUE(stopResult->callbackDrainDeferred);
	ASSERT_EQ(2U, events.size());
	ASSERT_EQ(EOutputAcceptedCommitFeedState::Live, events[0].state);
	ASSERT_TRUE(events[0].commit);
	EXPECT_EQ(EOutputAcceptedCommitKind::CreateChannel, events[0].commit->kind);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Stopped, events[1].state);
	EXPECT_FALSE(events[1].commit);
}

TEST(OutputService, ExternalAcceptedCommitFeedStopWaitsForCallbacksAndDrainsStoppedTerminal)
{
	using namespace std::chrono_literals;
	OutputService service;
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	std::vector<OutputAcceptedCommitEvent> events;
	ASSERT_TRUE(service.SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
		if (event.state != EOutputAcceptedCommitFeedState::Live) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	}));

	std::thread creator([&] {
		EXPECT_EQ(EOutputOperationStatus::Succeeded,
			service.CreateChannel(Create("create", Owner("feed.stop.external"), "stop.external")).status);
	});
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	std::promise<OutputOperationResult> stopPromise;
	auto stopFuture = stopPromise.get_future();
	std::thread stopper([&] { stopPromise.set_value(service.Stop()); });
	EXPECT_EQ(std::future_status::timeout, stopFuture.wait_for(50ms));
	{
		std::lock_guard lock(callbackMutex);
		release = true;
		callbackChanged.notify_all();
	}
	EXPECT_EQ(std::future_status::ready, stopFuture.wait_for(2s));
	creator.join();
	stopper.join();
	const auto stopResult = stopFuture.get();
	EXPECT_EQ(EOutputOperationStatus::Succeeded, stopResult.status);
	EXPECT_FALSE(stopResult.callbackDrainDeferred);
	ASSERT_EQ(2U, events.size());
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, events[0].state);
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Stopped, events[1].state);
	EXPECT_FALSE(events[1].commit);
}

TEST(OutputService, IdleAcceptedCommitFeedStopDeliversStoppedTerminal)
{
	OutputService service;
	std::vector<OutputAcceptedCommitEvent> events;
	const auto subscription = service.SubscribeAcceptedCommits([&events](const OutputAcceptedCommitEvent& event) {
		events.push_back(event);
	});
	ASSERT_TRUE(subscription);
	const auto stopped = service.Stop();
	EXPECT_EQ(EOutputOperationStatus::Succeeded, stopped.status);
	EXPECT_FALSE(stopped.callbackDrainDeferred);
	ASSERT_EQ(1U, events.size());
	EXPECT_EQ(EOutputAcceptedCommitFeedState::Stopped, events.front().state);
	EXPECT_FALSE(events.front().commit);
}

TEST(OutputService, AcceptedCommitFeedDestructionWaitsForBorrowedCallbackAndStaleUnsubscribeIsSafe)
{
	using namespace std::chrono_literals;
	std::unique_ptr<OutputService> owned = std::make_unique<OutputService>();
	OutputService* service = owned.get();
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	ASSERT_TRUE(service->SubscribeAcceptedCommits([&](const OutputAcceptedCommitEvent& event) {
		if (event.state != EOutputAcceptedCommitFeedState::Live) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	}));
	const auto subscriptionId = service->SubscribeAcceptedCommits([](const OutputAcceptedCommitEvent&) {})->subscriptionId;

	std::thread creator([service] {
		EXPECT_EQ(EOutputOperationStatus::Succeeded,
			service->CreateChannel(Create("create", Owner("feed.destroy"), "destroy.channel")).status);
	});
	{
		std::unique_lock lock(callbackMutex);
		ASSERT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	service->UnsubscribeAcceptedCommits(subscriptionId);
	service->UnsubscribeAcceptedCommits(subscriptionId);

	std::promise<void> destroyedPromise;
	auto destroyedFuture = destroyedPromise.get_future();
	std::thread destroyer([service = std::move(owned), &destroyedPromise]() mutable {
		service.reset();
		destroyedPromise.set_value();
	});
	EXPECT_EQ(std::future_status::timeout, destroyedFuture.wait_for(50ms));
	{
		std::lock_guard lock(callbackMutex);
		release = true;
		callbackChanged.notify_all();
	}
	creator.join();
	EXPECT_EQ(std::future_status::ready, destroyedFuture.wait_for(2s));
	destroyer.join();
}

TEST(OutputService, AcceptedCommitFeedOverflowDoesNotUseTheAdvisoryNotificationDropCounter)
{
	using namespace std::chrono_literals;
	OutputServiceLimits limits;
	limits.maximumChannels = 16;
	limits.maximumPendingNotifications = 1;
	limits.maximumAcceptedCommitFeedEntries = 16;
	OutputService service(limits);
	std::vector<OutputAcceptedCommitEvent> feedEvents;
	ASSERT_TRUE(service.SubscribeAcceptedCommits([&feedEvents](const OutputAcceptedCommitEvent& event) {
		feedEvents.push_back(event);
	}));
	std::mutex advisoryMutex;
	std::condition_variable advisoryChanged;
	bool entered{};
	bool release{};
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange& change) {
		if (change.kind != EOutputChangeKind::ChannelCreated || !change.channelId || *change.channelId != "advisory.first") return;
		std::unique_lock lock(advisoryMutex);
		entered = true;
		advisoryChanged.notify_all();
		advisoryChanged.wait(lock, [&] { return release; });
	}));

	std::thread writer([&] {
		EXPECT_EQ(EOutputOperationStatus::Succeeded,
			service.CreateChannel(Create("advisory-first", Owner("feed.advisory"), "advisory.first")).status);
	});
	{
		std::unique_lock lock(advisoryMutex);
		ASSERT_TRUE(advisoryChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	for (std::size_t index = 0; index < 4; ++index) {
		EXPECT_EQ(EOutputOperationStatus::Succeeded,
			service.CreateChannel(Create("advisory-" + std::to_string(index), Owner("feed.advisory"),
				"advisory." + std::to_string(index))).status);
	}
	{
		std::lock_guard lock(advisoryMutex);
		release = true;
		advisoryChanged.notify_all();
	}
	writer.join();

	const auto snapshot = service.Snapshot();
	EXPECT_GT(snapshot.droppedNotificationCount, 0U);
	ASSERT_EQ(5U, feedEvents.size());
	std::uint64_t expectedSequence{};
	for (const auto& event : feedEvents) {
		EXPECT_EQ(EOutputAcceptedCommitFeedState::Live, event.state);
		ASSERT_TRUE(event.commit);
		EXPECT_EQ(++expectedSequence, event.commit->sequence);
	}
}

TEST(OutputService, ReturnsExplicitTerminalsAndReplaysOnlyTheExactOperation)
{
	OutputServiceLimits limits;
	limits.maximumPayloadBytes = 5;
	limits.maximumTextBytesPerChannel = 5;
	OutputService service(limits);
	const auto owner = Owner("extension.sample", 4);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", owner, "sample.output")).status);
	const auto before = service.Snapshot();

	EXPECT_EQ(EOutputOperationStatus::Rejected, service.AppendOutput(Text("bad-payload", owner, "sample.output", "123456")).status);
	EXPECT_EQ(EOutputOperationReason::PayloadLimitExceeded, service.AppendOutput(Text("bad-payload-2", owner, "sample.output", "123456")).reason);
	EXPECT_EQ(EOutputOperationStatus::StaleRevision, service.AppendOutput({ .operation = Operation("stale", 1), .owner = owner, .channelId = "sample.output", .text = "ok" }).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict, service.AppendOutput(Text("wrong-owner", Owner("extension.sample", 5), "sample.output", "ok")).status);
	EXPECT_EQ(EOutputOperationStatus::Rejected, service.AppendLog({ .operation = Operation("wrong-kind"), .owner = owner, .channelId = "sample.output", .entries = { { .message = "log" } } }).status);
	EXPECT_EQ(EOutputOperationStatus::NotApplicable, service.Dispose(Channel("missing", owner, "missing.channel")).status);

	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(Text("replay", owner, "sample.output", "ok")).status);
	EXPECT_EQ(EOutputOperationStatus::Replayed, service.AppendOutput(Text("replay", owner, "sample.output", "ok")).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict, service.AppendOutput(Text("replay", owner, "sample.output", "no")).status);
	EXPECT_EQ(before.revision + 1, service.Snapshot().revision);
}

TEST(OutputService, TruncatesAtUtf8CharacterBoundariesAndRejectsOversizedMutationWithoutChangingState)
{
	OutputServiceLimits limits;
	limits.maximumPayloadBytes = 8;
	limits.maximumTextBytesPerChannel = 5;
	OutputService service(limits);
	const auto owner = Owner("extension.output");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", owner, "output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(Text("first", owner, "output", "abc")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(Text("second", owner, "output", "def")).status);
	const auto truncated = FindChannel(service.Snapshot(), "output");
	EXPECT_EQ("bcdef", truncated.text);
	EXPECT_EQ(1U, truncated.droppedCharacterCount);

	const auto before = service.Snapshot();
	const auto invalid = service.AppendOutput(Text("invalid", owner, "output", std::string("a\0b", 3)));
	EXPECT_EQ(EOutputOperationStatus::Rejected, invalid.status);
	EXPECT_EQ(EOutputOperationReason::InvalidPayload, invalid.reason);
	const auto after = service.Snapshot();
	EXPECT_EQ(before.revision, after.revision);
	EXPECT_EQ(FindChannel(before, "output").text, FindChannel(after, "output").text);
}

TEST(OutputService, ReplaceClearAndHideHaveExplicitStateTransitions)
{
	OutputService service;
	const auto owner = Owner("extension.lifecycle");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", owner, "lifecycle.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendOutput(Text("append", owner, "lifecycle.output", "before")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.ReplaceOutput(Text("replace", owner, "lifecycle.output", "after")).status);
	EXPECT_EQ("after", FindChannel(service.Snapshot(), "lifecycle.output").text);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Clear(Channel("clear", owner, "lifecycle.output")).status);
	EXPECT_TRUE(FindChannel(service.Snapshot(), "lifecycle.output").text.empty());
	EXPECT_EQ(EOutputOperationStatus::NotApplicable, service.Clear(Channel("clear-empty", owner, "lifecycle.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show({ .operation = Operation("show"), .owner = owner, .channelId = "lifecycle.output" }).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Hide(Channel("hide", owner, "lifecycle.output")).status);
	EXPECT_FALSE(FindChannel(service.Snapshot(), "lifecycle.output").visible);
	EXPECT_EQ(EOutputOperationStatus::NotApplicable, service.Hide(Channel("hide-again", owner, "lifecycle.output")).status);
}

TEST(OutputService, KeepsStructuredLogsSeparateFromPlainOutputAndProjectsThemDeterministically)
{
	OutputService service;
	const auto owner = Owner("extension.log");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-log", owner, "sample.log", EOutputChannelKind::Log)).status);
	const OutputLogMutationRequest append{
		.operation = Operation("append-log"),
		.owner = owner,
		.channelId = "sample.log",
		.entries = {
			{ .level = EOutputLogLevel::Warning, .message = "first", .source = std::string("compiler") },
			{ .level = EOutputLogLevel::Error, .message = "second" },
		},
	};
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.AppendLog(append).status);
	const auto snapshot = service.Snapshot();
	const auto& channel = FindChannel(snapshot, "sample.log");
	EXPECT_TRUE(channel.text.empty());
	ASSERT_EQ(2U, channel.logEntries.size());
	EXPECT_EQ(EOutputLogLevel::Warning, channel.logEntries[0].level);
	EXPECT_EQ("[Warning] compiler: first\n[Error] second\n", channel.projectedText);
	EXPECT_EQ(EOutputOperationStatus::Rejected, service.ReplaceOutput(Text("replace-log", owner, "sample.log", "plain")).status);
}

TEST(OutputService, ExactOwnerDisposeCannotRemoveNewGenerationAndActiveChannelFallsBackDeterministically)
{
	OutputService service;
	const auto oldOwner = Owner("extension.one", 1);
	const auto otherOwner = Owner("extension.two", 1);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-a", oldOwner, "a.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-b", otherOwner, "b.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show({ .operation = Operation("show-a"), .owner = oldOwner, .channelId = "a.channel" }).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Show({ .operation = Operation("show-b"), .owner = otherOwner, .channelId = "b.channel" }).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict, service.DisposeOwner({ .operation = Operation("dispose-stale"), .owner = Owner("extension.one", 2) }).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Dispose(Channel("dispose-b", otherOwner, "b.channel")).status);
	const auto fallback = service.Snapshot();
	ASSERT_TRUE(fallback.activeChannelId);
	EXPECT_EQ("a.channel", *fallback.activeChannelId);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.DisposeOwner({ .operation = Operation("dispose-a-owner"), .owner = oldOwner }).status);
	EXPECT_TRUE(service.Snapshot().channels.empty());
	EXPECT_FALSE(service.Snapshot().activeChannelId);
	EXPECT_EQ(EOutputOperationStatus::Conflict, service.CreateChannel(Create("late-old-generation", oldOwner, "old.again")).status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("new-generation", Owner("extension.one", 2), "new.channel")).status);
}

TEST(OutputService, RetainsDisposedOwnerFencesWithinAnExplicitLifetimeCapacity)
{
	OutputServiceLimits limits;
	limits.maximumOwners = 1;
	OutputService service(limits);
	const auto original = Owner("extension.original", 4);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-original", original, "original.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.DisposeOwner({ .operation = Operation("dispose-original"), .owner = original }).status);

	// The disposed identity consumes the configured lifetime slot. The model must
	// surface capacity instead of evicting its generation fence.
	const auto capacity = service.CreateChannel(Create("create-other", Owner("extension.other"), "other.channel"));
	EXPECT_EQ(EOutputOperationStatus::Rejected, capacity.status);
	EXPECT_EQ(EOutputOperationReason::OwnerLimitExceeded, capacity.reason);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		service.CreateChannel(Create("late-original", original, "late.channel")).status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("newer-original", Owner("extension.original", 5), "newer.channel")).status);
}

TEST(OutputService, OneOwnerMayCreateMultipleChannelsAndANewerGenerationAtomicallyReplacesItsOldChannels)
{
	OutputService service;
	const auto firstGeneration = Owner("extension.multiple", 3);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-one", firstGeneration, "multiple.one")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-two", firstGeneration, "multiple.two")).status);
	ASSERT_EQ(2U, service.Snapshot().channels.size());

	EXPECT_EQ(EOutputOperationStatus::Succeeded,
		service.CreateChannel(Create("create-new", Owner("extension.multiple", 4), "multiple.one")).status);
	const auto adopted = service.Snapshot();
	ASSERT_EQ(1U, adopted.channels.size());
	EXPECT_EQ("multiple.one", adopted.channels.front().channelId);
	EXPECT_EQ(4U, adopted.channels.front().owner.generation);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		service.CreateChannel(Create("late-old", firstGeneration, "multiple.old")).status);
}

TEST(OutputService, DeliversOrderedNotificationsOutsideTheLockAndContainsListenerFaults)
{
	OutputService service;
	std::vector<std::uint64_t> revisions;
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange& change) { revisions.push_back(change.revision); }));
	ASSERT_TRUE(service.Subscribe([](const OutputServiceChange&) { throw std::runtime_error("expected listener failure"); }));
	EOutputOperationStatus nestedStatus = EOutputOperationStatus::Rejected;
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange& change) {
		if (change.channelId && *change.channelId == "one.channel") {
			nestedStatus = service.CreateChannel(Create("nested", Owner("nested"), "nested.channel")).status;
		}
	}));
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-one", Owner("one"), "one.channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create-two", Owner("two"), "two.channel")).status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, nestedStatus);
	ASSERT_EQ(3U, revisions.size());
	EXPECT_LT(revisions[0], revisions[1]);
	EXPECT_LT(revisions[1], revisions[2]);
}

TEST(OutputService, StopClearsStateAndSubscribersAndRejectsAllLaterWork)
{
	OutputService service;
	ASSERT_TRUE(service.Subscribe([](const OutputServiceChange&) {}));
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", Owner("owner"), "channel")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded, service.Stop().status);
	const auto stopped = service.Snapshot();
	EXPECT_TRUE(stopped.stopped);
	EXPECT_TRUE(stopped.channels.empty());
	EXPECT_FALSE(stopped.activeChannelId);
	EXPECT_EQ(EOutputOperationStatus::Stopped, service.CreateChannel(Create("after-stop", Owner("owner"), "other")).status);
	EXPECT_FALSE(service.Subscribe([](const OutputServiceChange&) {}));
	EXPECT_EQ(EOutputOperationStatus::Succeeded, service.Stop().status);
}

TEST(OutputService, ExternalAndRepeatedStopWaitForTheStartedListenerDispatch)
{
	using namespace std::chrono_literals;
	OutputService service;
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange& change) {
		if (change.kind != EOutputChangeKind::ChannelCreated) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	}));
	std::promise<OutputOperationResult> createPromise;
	auto createFuture = createPromise.get_future();
	std::thread creator([&] { createPromise.set_value(service.CreateChannel(Create("create", Owner("owner"), "channel"))); });
	{
		std::unique_lock lock(callbackMutex);
		EXPECT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	std::promise<OutputOperationResult> firstPromise;
	std::promise<OutputOperationResult> secondPromise;
	std::promise<void> firstStartedPromise;
	std::promise<void> secondStartedPromise;
	auto firstFuture = firstPromise.get_future();
	auto secondFuture = secondPromise.get_future();
	auto firstStarted = firstStartedPromise.get_future();
	auto secondStarted = secondStartedPromise.get_future();
	std::thread first([&] { firstStartedPromise.set_value(); firstPromise.set_value(service.Stop()); });
	std::thread second([&] { secondStartedPromise.set_value(); secondPromise.set_value(service.Stop()); });
	EXPECT_EQ(std::future_status::ready, firstStarted.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, secondStarted.wait_for(2s));
	EXPECT_EQ(std::future_status::timeout, firstFuture.wait_for(50ms));
	EXPECT_EQ(std::future_status::timeout, secondFuture.wait_for(50ms));
	{
		std::lock_guard lock(callbackMutex);
		release = true;
		callbackChanged.notify_all();
	}
	EXPECT_EQ(std::future_status::ready, createFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, firstFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, secondFuture.wait_for(2s));
	const auto create = createFuture.get();
	const auto firstStop = firstFuture.get();
	const auto secondStop = secondFuture.get();
	creator.join();
	first.join();
	second.join();
	EXPECT_EQ(EOutputOperationStatus::Succeeded, create.status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, firstStop.status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, secondStop.status);
	EXPECT_FALSE(firstStop.callbackDrainDeferred);
	EXPECT_FALSE(secondStop.callbackDrainDeferred);
}

TEST(OutputService, ReentrantListenerStopDefersItsOwnDrainWithoutDeadlock)
{
	OutputService service;
	std::optional<OutputOperationResult> stopResult;
	ASSERT_TRUE(service.Subscribe([&](const OutputServiceChange&) {
		if (!stopResult) stopResult = service.Stop();
	}));
	EXPECT_EQ(EOutputOperationStatus::Succeeded, service.CreateChannel(Create("create", Owner("owner"), "channel")).status);
	ASSERT_TRUE(stopResult);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, stopResult->status);
	EXPECT_TRUE(stopResult->callbackDrainDeferred);
	EXPECT_TRUE(service.Snapshot().stopped);
}

} // namespace
} // namespace workbench::output
