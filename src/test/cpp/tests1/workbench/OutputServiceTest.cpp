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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "workbench/output/OutputService.h"

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
