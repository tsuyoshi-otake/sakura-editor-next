/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "debug/console/DebugConsoleModel.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using namespace debug::console;

TEST(DebugConsoleModel, FencesOldGenerationsAndRequiresExplicitDispose)
{
	CDebugConsoleModel model;
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.StartSession(0).status);
	EXPECT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(7).status);
	EXPECT_EQ(EDebugConsoleModelStatus::SessionActive, model.StartSession(7).status);
	EXPECT_EQ(EDebugConsoleModelStatus::StaleGeneration, model.StartSession(8).status);
	EXPECT_EQ(EDebugConsoleModelStatus::StaleGeneration, model.AppendOutput(6, EDebugConsoleOutputCategory::Stdout, "old").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(7, "same-client-operation", "old session").status);
	EXPECT_EQ(EDebugConsoleModelStatus::SessionDisposed, model.DisposeSession(7).status);
	EXPECT_EQ(EDebugConsoleModelStatus::SessionDisposed, model.AppendOutput(7, EDebugConsoleOutputCategory::Stdout, "late").status);
	EXPECT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(8).status);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(8, "same-client-operation", "new session").status);
	EXPECT_EQ(EDebugConsoleModelStatus::StaleGeneration, model.AppendOutput(7, EDebugConsoleOutputCategory::Stdout, "old").status);
}

TEST(DebugConsoleModel, AppendsStructuredTranscriptAndBoundsItDeterministically)
{
	DebugConsoleLimits limits;
	limits.maximumTranscriptEntries = 2;
	CDebugConsoleModel model(limits);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Stdout, "one").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Stderr, "two").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Important, "three").status);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(2U, snapshot.transcript.size());
	EXPECT_EQ(2U, snapshot.transcript[0].sequence);
	EXPECT_EQ(EDebugConsoleOutputCategory::Stderr, snapshot.transcript[0].category);
	EXPECT_EQ("three", snapshot.transcript[1].text);
	EXPECT_EQ(1U, snapshot.droppedTranscriptEntryCount);
}

TEST(DebugConsoleModel, ReplAllocatesIdsKeepsHistoryAndReplaysExactOperations)
{
	CDebugConsoleModel model;
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(4).status);
	const auto first = model.SubmitReplInput(4, "client-1", "counter + 1", 20);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, first.status);
	ASSERT_TRUE(first.requestId);
	const auto replay = model.SubmitReplInput(4, "client-1", "counter + 1", 20);
	EXPECT_EQ(EDebugConsoleModelStatus::Replayed, replay.status);
	EXPECT_EQ(first.requestId, replay.requestId);
	EXPECT_EQ(EDebugConsoleModelStatus::Conflict, model.SubmitReplInput(4, "client-1", "counter + 2", 20).status);
	EXPECT_EQ(EDebugConsoleModelStatus::Conflict, model.SubmitReplInput(4, "client-1", "counter + 1", 21).status);
	const auto second = model.SubmitReplInput(4, "client-2", "counter + 2");
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, second.status);
	EXPECT_EQ(2U, *second.requestId);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(2U, snapshot.history.size());
	EXPECT_EQ("counter + 1", snapshot.history[0].expression);
	EXPECT_EQ("counter + 2", snapshot.history[1].expression);
	ASSERT_EQ(2U, snapshot.pendingEvaluations.size());
	EXPECT_EQ("client-1", snapshot.pendingEvaluations[0].operationId);
}

TEST(DebugConsoleModel, RejectsInvalidInputAndCapsPendingAndHistory)
{
	DebugConsoleLimits limits;
	limits.maximumPendingEvaluations = 1;
	limits.maximumHistoryEntries = 1;
	limits.maximumExpressionLength = 3;
	CDebugConsoleModel model(limits);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.SubmitReplInput(1, "", "x").status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.SubmitReplInput(1, "a", "").status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.SubmitReplInput(1, "a", "long").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "a", "one").status);
	EXPECT_EQ(EDebugConsoleModelStatus::PendingLimitExceeded, model.SubmitReplInput(1, "b", "two").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, 1).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "b", "two").status);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(1U, snapshot.history.size());
	EXPECT_EQ("two", snapshot.history.front().expression);
	EXPECT_EQ(1U, snapshot.droppedHistoryEntryCount);
}

TEST(DebugConsoleModel, RejectsInvalidEnumsAndUtf8AcrossEveryTextIngress)
{
	CDebugConsoleModel model;
	const std::string invalidUtf8("\xc3\x28", 2);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument,
		model.AppendOutput(1, static_cast<EDebugConsoleOutputCategory>(255), "valid").status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument,
		model.AppendOutput(1, EDebugConsoleOutputCategory::Console, invalidUtf8).status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.SubmitReplInput(1, invalidUtf8, "expression").status);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.SubmitReplInput(1, "operation", invalidUtf8).status);
	const auto accepted = model.SubmitReplInput(1, "operation", "expression");
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, accepted.status);
	ASSERT_TRUE(accepted.requestId);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, model.CompleteEvaluation(1, *accepted.requestId, invalidUtf8).status);
	const auto invalidExpiry = model.ExpireEvaluations(1, 99, invalidUtf8);
	EXPECT_EQ(EDebugConsoleModelStatus::InvalidArgument, invalidExpiry.status);
	EXPECT_EQ(0U, invalidExpiry.expiredCount);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, *accepted.requestId, "valid").status);
}

TEST(DebugConsoleModel, FencesTranscriptHistoryAndEvaluationIdExhaustionWithoutWrapping)
{
	{
		DebugConsoleLimits limits;
		limits.maximumTranscriptEntries = 1;
		limits.maximumTranscriptSequence = 1;
		CDebugConsoleModel model(limits);
		ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "first").status);
		EXPECT_EQ(EDebugConsoleModelStatus::SequenceExhausted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "second").status);
		const auto snapshot = model.Snapshot();
		ASSERT_EQ(1U, snapshot.transcript.size());
		EXPECT_EQ(1U, snapshot.transcript.front().sequence);
		EXPECT_EQ("first", snapshot.transcript.front().text);
		EXPECT_EQ(0U, snapshot.nextTranscriptSequence);
		EXPECT_EQ(0U, snapshot.droppedTranscriptEntryCount);
	}
	{
		DebugConsoleLimits limits;
		limits.maximumEvaluationRequestId = 2;
		limits.maximumHistorySequence = 1;
		CDebugConsoleModel model(limits);
		ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "first", "first").status);
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, 1).status);
		EXPECT_EQ(EDebugConsoleModelStatus::SequenceExhausted, model.SubmitReplInput(1, "second", "second").status);
		const auto snapshot = model.Snapshot();
		ASSERT_EQ(1U, snapshot.history.size());
		EXPECT_EQ(1U, snapshot.history.front().sequence);
		EXPECT_EQ(2U, snapshot.nextEvaluationRequestId);
		EXPECT_EQ(0U, snapshot.nextHistorySequence);
	}
	{
		DebugConsoleLimits limits;
		limits.maximumEvaluationRequestId = 1;
		limits.maximumHistorySequence = 2;
		CDebugConsoleModel model(limits);
		ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "first", "first").status);
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, 1).status);
		EXPECT_EQ(EDebugConsoleModelStatus::SequenceExhausted, model.SubmitReplInput(1, "second", "second").status);
		EXPECT_EQ(0U, model.Snapshot().nextEvaluationRequestId);
	}
}

TEST(DebugConsoleModel, RecordsEveryExplicitEvaluationTerminalExactlyOnce)
{
	CDebugConsoleModel model;
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(11).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(11, "complete", "a").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(11, "fail", "b").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(11, "cancel", "c").status);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(11, 1, "value").status);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.FailEvaluation(11, 2, "bad expression").status);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.CancelEvaluation(11, 3, "user cancel").status);
	EXPECT_EQ(EDebugConsoleModelStatus::AlreadyTerminal, model.CompleteEvaluation(11, 1).status);
	EXPECT_EQ(EDebugConsoleModelStatus::UnknownRequest, model.CancelEvaluation(11, 99).status);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(3U, snapshot.completedEvaluations.size());
	EXPECT_EQ(EDebugConsoleEvaluationTerminal::Completed, snapshot.completedEvaluations[0].terminal);
	EXPECT_EQ(EDebugConsoleEvaluationTerminal::Failed, snapshot.completedEvaluations[1].terminal);
	EXPECT_EQ(EDebugConsoleEvaluationTerminal::Cancelled, snapshot.completedEvaluations[2].terminal);
	EXPECT_EQ("bad expression", snapshot.completedEvaluations[1].detail);
}

TEST(DebugConsoleModel, ExpiresOnlyCallerSelectedDeadlines)
{
	CDebugConsoleModel model;
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(2).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(2, "a", "a", 9).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(2, "b", "b", 10).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(2, "c", "c").status);
	const auto staleExpiry = model.ExpireEvaluations(1, 99);
	EXPECT_EQ(EDebugConsoleModelStatus::StaleGeneration, staleExpiry.status);
	EXPECT_EQ(0U, staleExpiry.expiredCount);
	const auto firstExpiry = model.ExpireEvaluations(2, 9, "deadline");
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, firstExpiry.status);
	EXPECT_EQ(1U, firstExpiry.expiredCount);
	const auto secondExpiry = model.ExpireEvaluations(2, 10);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, secondExpiry.status);
	EXPECT_EQ(1U, secondExpiry.expiredCount);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(1U, snapshot.pendingEvaluations.size());
	EXPECT_EQ("c", snapshot.pendingEvaluations[0].expression);
	ASSERT_EQ(2U, snapshot.completedEvaluations.size());
	EXPECT_EQ(EDebugConsoleEvaluationTerminal::Expired, snapshot.completedEvaluations[0].terminal);
	EXPECT_EQ("deadline", snapshot.completedEvaluations[0].detail);
}

TEST(DebugConsoleModel, BoundsCompletedRecordsAndOperationRetentionTogether)
{
	DebugConsoleLimits limits;
	limits.maximumCompletedEvaluations = 1;
	CDebugConsoleModel model(limits);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "a", "one").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, 1).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "b", "two").status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.CompleteEvaluation(1, 2).status);
	EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(1, "a", "new").status);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(1U, snapshot.completedEvaluations.size());
	EXPECT_EQ("b", snapshot.completedEvaluations[0].operationId);
	EXPECT_EQ(1U, snapshot.droppedCompletedEvaluationCount);
}

TEST(DebugConsoleModel, DisposeAndStopCancelPendingAndFenceFurtherWork)
{
	CDebugConsoleModel model;
	std::vector<EDebugConsoleNotificationKind> notifications;
	ASSERT_TRUE(model.Subscribe([&](const DebugConsoleNotification& notification) { notifications.push_back(notification.kind); }));
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(3).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(3, "a", "a").status);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionDisposed, model.DisposeSession(3).status);
	ASSERT_EQ(1U, model.Snapshot().completedEvaluations.size());
	EXPECT_EQ(EDebugConsoleEvaluationTerminal::Cancelled, model.Snapshot().completedEvaluations[0].terminal);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(4).status);
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.SubmitReplInput(4, "b", "b").status);
	EXPECT_EQ(EDebugConsoleModelStatus::Stopped, model.Stop().status);
	EXPECT_TRUE(model.Snapshot().stopped);
	EXPECT_FALSE(model.Snapshot().activeSessionGeneration);
	EXPECT_NE(notifications.end(), std::find(notifications.begin(), notifications.end(), EDebugConsoleNotificationKind::Stopped));
	EXPECT_EQ(EDebugConsoleModelStatus::Stopped, model.AppendOutput(4, EDebugConsoleOutputCategory::Console, "late").status);
	EXPECT_EQ(EDebugConsoleModelStatus::Stopped, model.StartSession(5).status);
}

TEST(DebugConsoleModel, DeliversListenersOutsideLocksWithReentrancyAndExceptionContainment)
{
	CDebugConsoleModel model;
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	std::vector<EDebugConsoleNotificationKind> received;
	bool appendedReentrant{};
	ASSERT_TRUE(model.Subscribe([&](const DebugConsoleNotification& notification) {
		received.push_back(notification.kind);
		if (notification.kind == EDebugConsoleNotificationKind::OutputAppended && !appendedReentrant) {
			appendedReentrant = true;
			EXPECT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "reentrant").status);
		}
	}));
	ASSERT_TRUE(model.Subscribe([](const DebugConsoleNotification&) { throw std::runtime_error("host callback"); }));
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "first").status);
	const auto snapshot = model.Snapshot();
	ASSERT_EQ(2U, snapshot.transcript.size());
	EXPECT_EQ("first", snapshot.transcript[0].text);
	EXPECT_EQ("reentrant", snapshot.transcript[1].text);
	EXPECT_GE(received.size(), 2U);
}

TEST(DebugConsoleModel, BoundsReentrantNotificationQueueAndSubscriptions)
{
	DebugConsoleLimits limits;
	limits.maximumPendingNotifications = 1;
	limits.maximumSubscriptions = 1;
	CDebugConsoleModel model(limits);
	ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
	ASSERT_TRUE(model.Subscribe([&](const DebugConsoleNotification& notification) {
		if (notification.kind == EDebugConsoleNotificationKind::OutputAppended && notification.transcriptSequence && *notification.transcriptSequence == 1) {
			ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "two").status);
			ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "three").status);
		}
	}));
	EXPECT_FALSE(model.Subscribe([](const DebugConsoleNotification&) {}));
	ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "one").status);
	EXPECT_EQ(1U, model.Snapshot().droppedNotificationCount);
}

TEST(DebugConsoleModel, StopWaitsForCrossThreadCallbackAndDefersReentrantStopToItsDispatcher)
{
	{
		CDebugConsoleModel model;
		std::mutex gateMutex;
		std::condition_variable gateCondition;
		bool callbackEntered{};
		bool releaseCallback{};
		bool stopInvoked{};
		bool stopReturned{};
		bool secondStopInvoked{};
		bool secondStopReturned{};
		EDebugConsoleModelStatus appendStatus{ EDebugConsoleModelStatus::InvalidArgument };
		DebugConsoleModelResult stopResult;
		DebugConsoleModelResult secondStopResult;
		ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
		ASSERT_TRUE(model.Subscribe([&](const DebugConsoleNotification& notification) {
			if (notification.kind != EDebugConsoleNotificationKind::OutputAppended) return;
			std::unique_lock lock(gateMutex);
			callbackEntered = true;
			gateCondition.notify_all();
			gateCondition.wait(lock, [&] { return releaseCallback; });
		}));
		std::thread appendThread([&] {
			appendStatus = model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "block").status;
		});
		{
			std::unique_lock lock(gateMutex);
			gateCondition.wait(lock, [&] { return callbackEntered; });
		}
		std::thread stopThread([&] {
			{
				std::lock_guard lock(gateMutex);
				stopInvoked = true;
			}
			gateCondition.notify_all();
			const auto result = model.Stop();
			{
				std::lock_guard lock(gateMutex);
				stopResult = result;
				stopReturned = true;
			}
			gateCondition.notify_all();
		});
		{
			std::unique_lock lock(gateMutex);
			gateCondition.wait(lock, [&] { return stopInvoked; });
			EXPECT_FALSE(gateCondition.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopReturned; }));
		}
		EXPECT_TRUE(model.Snapshot().stopped);
		std::thread secondStopThread([&] {
			{
				std::lock_guard lock(gateMutex);
				secondStopInvoked = true;
			}
			gateCondition.notify_all();
			const auto result = model.Stop();
			{
				std::lock_guard lock(gateMutex);
				secondStopResult = result;
				secondStopReturned = true;
			}
			gateCondition.notify_all();
		});
		{
			std::unique_lock lock(gateMutex);
			gateCondition.wait(lock, [&] { return secondStopInvoked; });
			EXPECT_FALSE(gateCondition.wait_for(lock, std::chrono::milliseconds(50), [&] { return secondStopReturned; }));
		}
		{
			std::lock_guard lock(gateMutex);
			releaseCallback = true;
		}
		gateCondition.notify_all();
		appendThread.join();
		stopThread.join();
		secondStopThread.join();
		EXPECT_EQ(EDebugConsoleModelStatus::Accepted, appendStatus);
		EXPECT_EQ(EDebugConsoleModelStatus::Stopped, stopResult.status);
		EXPECT_EQ(EDebugConsoleModelStatus::Stopped, secondStopResult.status);
	}
	{
		CDebugConsoleModel model;
		EDebugConsoleModelStatus reentrantStopStatus{ EDebugConsoleModelStatus::InvalidArgument };
		ASSERT_EQ(EDebugConsoleModelStatus::SessionStarted, model.StartSession(1).status);
		ASSERT_TRUE(model.Subscribe([&](const DebugConsoleNotification& notification) {
			if (notification.kind == EDebugConsoleNotificationKind::OutputAppended) reentrantStopStatus = model.Stop().status;
		}));
		ASSERT_EQ(EDebugConsoleModelStatus::Accepted, model.AppendOutput(1, EDebugConsoleOutputCategory::Console, "reentrant stop").status);
		EXPECT_EQ(EDebugConsoleModelStatus::StopNotificationDrainDeferred, reentrantStopStatus);
		EXPECT_TRUE(model.Snapshot().stopped);
	}
}

} // namespace
