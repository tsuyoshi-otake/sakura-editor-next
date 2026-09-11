/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpRuntimeSession.h"

namespace {
using namespace senp;
using namespace senp::effect;
using namespace std::chrono_literals;

OperationContext Context() { return { L"", 4, 2, 0, 1 }; }

class SenpRuntimeLifecycle : public ::testing::Test {
protected:
	CSenpRuntimeSession session{ 9 };
	CSenpRuntimeSession::Time now{};
	std::int64_t responseSequence{};
	Envelope Outgoing()
	{
		auto frame = session.TakeOutgoing();
		EXPECT_TRUE(frame.has_value());
		if (!frame) return {};
		auto decoded = Decode(frame->bytes);
		EXPECT_TRUE(decoded.has_value());
		return decoded.value_or(Envelope{});
	}
	void Receive(Message body)
	{
		auto bytes = Encode({ 2, ++responseSequence, 9, std::move(body) });
		ASSERT_TRUE(bytes.has_value());
		session.Receive(*bytes);
	}
	void ActivateSession()
	{
		ASSERT_EQ(session.Begin(L"test.extension", Context(), now + 1s, now).status, AdmissionStatus::Accepted);
		EXPECT_EQ(Outgoing().sequence, 1);
		Receive(Hello{ std::wstring(kAbi) });
		EXPECT_TRUE(std::holds_alternative<Ack>(Outgoing().body));
		const auto activation = Outgoing();
		ASSERT_TRUE(std::holds_alternative<Activate>(activation.body));
		Receive(EffectsMessage{ std::get<Activate>(activation.body).context, {} });
		EXPECT_TRUE(std::holds_alternative<Ack>(Outgoing().body));
		auto complete = session.TakeCompleted();
		ASSERT_TRUE(complete.has_value());
		EXPECT_TRUE(complete->activation);
		EXPECT_EQ(complete->status, InvocationStatus::EffectsReady);
		EXPECT_EQ(session.Phase(), RuntimePhase::Active);
	}
	InvocationAdmission Submit(CSenpRuntimeSession::Time deadline)
	{
		return session.Submit(Context(), TreeRequest{ L"issues", L"", L"" }, deadline, now);
	}
};

TEST_F(SenpRuntimeLifecycle, CancelBeforeSendDropsWorkWithoutCreatingASequenceGap)
{
	ActivateSession();
	const auto cancelled = Submit(now + 1s);
	const auto kept = Submit(now + 1s);
	ASSERT_EQ(cancelled.status, AdmissionStatus::Accepted);
	ASSERT_TRUE(session.Cancel(cancelled.operationId));
	EXPECT_FALSE(session.Cancel(cancelled.operationId));
	const auto sent = Outgoing();
	EXPECT_EQ(sent.sequence, 5);
	EXPECT_EQ(std::get<EventMessage>(sent.body).context.operationId, kept.operationId);
	EXPECT_EQ(session.QueuedBytes(), 0U);
	const auto result = session.TakeCompleted();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->status, InvocationStatus::Cancelled);
	EXPECT_FALSE(session.TakeCompleted());
}

TEST_F(SenpRuntimeLifecycle, CompletionCapacityIsReservedUntilTheCallerDrainsItsOutcome)
{
	ActivateSession();
	for (std::size_t i = 0; i < CSenpRuntimeSession::kMaximumPending; ++i)
		EXPECT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	EXPECT_EQ(Submit(now + 1s).status, AdmissionStatus::Busy);
	session.Expire(now + 1s);
	EXPECT_EQ(session.PendingCount(), 0U);
	EXPECT_EQ(session.CompletionCount(), CSenpRuntimeSession::kMaximumPending);
	EXPECT_EQ(session.QueuedBytes(), 0U);
	EXPECT_FALSE(session.TakeOutgoing());
	EXPECT_EQ(Submit(now + 2s).status, AdmissionStatus::Busy);
	ASSERT_TRUE(session.TakeCompleted());
	EXPECT_EQ(Submit(now + 2s).status, AdmissionStatus::Accepted);
	session.TransportFailed();
	EXPECT_EQ(session.CompletionCount(), CSenpRuntimeSession::kMaximumPending);
	session.TransportFailed();
	EXPECT_EQ(session.CompletionCount(), CSenpRuntimeSession::kMaximumPending);
}

TEST_F(SenpRuntimeLifecycle, LateAndDuplicateResultsNeverPublishEffectsAfterCancellation)
{
	ActivateSession();
	const auto operation = Submit(now + 1s);
	const auto sent = std::get<EventMessage>(Outgoing().body);
	ASSERT_TRUE(session.Cancel(operation.operationId));
	const auto response = Encode({ 2, ++responseSequence, 9, EffectsMessage{
		sent.context, { OpenDocument{ L"issue:1" } } } });
	ASSERT_TRUE(response);
	session.Receive(*response);
	session.Receive(*response);
	EXPECT_EQ(session.CompletionCount(), 1U);
	const auto result = session.TakeCompleted();
	ASSERT_TRUE(result);
	EXPECT_EQ(result->status, InvocationStatus::Cancelled);
	EXPECT_TRUE(result->effects.empty());
	EXPECT_TRUE(std::holds_alternative<Ack>(Outgoing().body));
	EXPECT_TRUE(std::holds_alternative<Ack>(Outgoing().body));
	EXPECT_EQ(session.Phase(), RuntimePhase::Active);
}

TEST_F(SenpRuntimeLifecycle, ChangedReplayIsAProtocolFaultAndFinalizesOtherPendingWork)
{
	ActivateSession();
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	const auto sent = std::get<EventMessage>(Outgoing().body);
	Receive(EffectsMessage{ sent.context, {} });
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	const auto changed = Encode({ 2, responseSequence, 9, EffectsMessage{
		sent.context, { OpenDocument{ L"issue:changed" } } } });
	ASSERT_TRUE(changed);
	session.Receive(*changed);
	EXPECT_EQ(session.Phase(), RuntimePhase::Stopped);
	EXPECT_EQ(session.PendingCount(), 0U);
	EXPECT_EQ(session.CompletionCount(), 2U);
	EXPECT_EQ(session.TakeCompleted()->status, InvocationStatus::ProtocolError);
	EXPECT_EQ(session.TakeCompleted()->status, InvocationStatus::ProtocolError);
	EXPECT_FALSE(session.TakeOutgoing());
}

TEST_F(SenpRuntimeLifecycle, StaleGenerationAndMismatchedContextNeverPublishEffects)
{
	for (int kind = 0; kind < 5; ++kind) {
		CSenpRuntimeSession local{ 9 };
		ASSERT_EQ(local.Begin(L"test.extension", Context(), now + 1s, now).status, AdmissionStatus::Accepted);
		ASSERT_TRUE(local.TakeOutgoing());
		local.Receive(*Encode({ 2, 1, 9, Hello{ std::wstring(kAbi) } }));
		ASSERT_TRUE(local.TakeOutgoing());
		const auto activate = Decode(local.TakeOutgoing()->bytes);
		auto context = std::get<Activate>(activate->body).context;
		auto generation = 9;
		if (kind == 0) ++generation;
		if (kind == 1) ++context.ownerGeneration;
		if (kind == 2) ++context.accountGeneration;
		if (kind == 3) ++context.workspaceRevision;
		if (kind == 4) ++context.requestGeneration;
		local.Receive(*Encode({ 2, 2, generation, EffectsMessage{ context, { OpenDocument{ L"bad" } } } }));
		EXPECT_EQ(local.Phase(), RuntimePhase::Stopped);
		const auto result = local.TakeCompleted();
		ASSERT_TRUE(result);
		EXPECT_EQ(result->status, InvocationStatus::ProtocolError);
		EXPECT_TRUE(result->effects.empty());
	}
}

TEST_F(SenpRuntimeLifecycle, DisableDrainsAcceptedWorkAndAcknowledgesLateResultsWhileStopping)
{
	ActivateSession();
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	const auto inFlight = std::get<EventMessage>(Outgoing().body);
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	session.Stop(StopReason::Disabled);
	session.Stop(StopReason::Disabled);
	EXPECT_EQ(session.Phase(), RuntimePhase::Stopping);
	EXPECT_EQ(session.PendingCount(), 0U);
	EXPECT_EQ(session.CompletionCount(), 2U);
	Receive(EffectsMessage{ inFlight.context, { OpenDocument{ L"stale" } } });
	EXPECT_EQ(session.CompletionCount(), 2U);
	EXPECT_TRUE(std::holds_alternative<Deactivate>(Outgoing().body));
	Receive(Stopped{ StopReason::Disabled });
	EXPECT_EQ(session.Phase(), RuntimePhase::Stopped);
	EXPECT_EQ(session.QueuedBytes(), 0U);
	EXPECT_FALSE(session.TakeOutgoing());
	for (int i = 0; i < 2; ++i) EXPECT_EQ(session.TakeCompleted()->status, InvocationStatus::Cancelled);
}

TEST_F(SenpRuntimeLifecycle, HandshakeAndActivationDeadlinesHaveExplicitTerminalOwnership)
{
	ASSERT_EQ(session.Begin(L"test.extension", Context(), now + 1s, now).status, AdmissionStatus::Accepted);
	EXPECT_EQ(session.NextDeadline(), now + 1s);
	session.Expire(now + 1s);
	EXPECT_EQ(session.Phase(), RuntimePhase::Stopped);
	EXPECT_EQ(session.TakeCompleted()->status, InvocationStatus::TimedOut);
	EXPECT_FALSE(session.NextDeadline());
	EXPECT_FALSE(session.TakeOutgoing());
	EXPECT_EQ(Submit(now + 2s).status, AdmissionStatus::Unavailable);
	session.TransportFailed();
	EXPECT_FALSE(session.TakeCompleted());
}

TEST_F(SenpRuntimeLifecycle, InvalidLifetimeOwnerAndQueueBudgetRejectBeforeAdmission)
{
	ActivateSession();
	EXPECT_EQ(Submit(now).status, AdmissionStatus::InvalidRequest);
	EXPECT_EQ(Submit(now + 11s).status, AdmissionStatus::InvalidRequest);
	auto owner = Context();
	++owner.ownerGeneration;
	EXPECT_EQ(session.Submit(owner, TreeRequest{ L"issues", L"", L"" }, now + 1s, now).status,
		AdmissionStatus::InvalidRequest);
	EXPECT_EQ(session.PendingCount(), 0U);
	ToolCompleted large{ L"read:1", CompletionStatus::Succeeded, std::wstring(65536, L'x'), L"" };
	for (std::size_t i = 0; i < 17; ++i) {
		const auto result = session.Submit(Context(), large, now + 1s, now);
		EXPECT_EQ(result.status, i < 16 ? AdmissionStatus::Accepted : AdmissionStatus::Busy);
		EXPECT_LE(session.QueuedBytes(), kMaximumQueuedBytes);
	}
	session.TransportFailed();
	EXPECT_EQ(session.PendingCount(), 0U);
}

TEST_F(SenpRuntimeLifecycle, SequenceGapsAndUnexpectedDirectionsStopRatherThanRestart)
{
	ActivateSession();
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	session.Receive(*Encode({ 2, responseSequence + 2, 9, Hello{ std::wstring(kAbi) } }));
	EXPECT_EQ(session.Phase(), RuntimePhase::Stopped);
	EXPECT_EQ(session.TakeCompleted()->status, InvocationStatus::ProtocolError);
	EXPECT_EQ(session.Begin(L"test.extension", Context(), now + 1s, now).status, AdmissionStatus::Unavailable);
}

TEST_F(SenpRuntimeLifecycle, RevocationClearsEffectsWaitingForTheConsumerWithoutDuplicatingOutcomes)
{
	ActivateSession();
	ASSERT_EQ(Submit(now + 1s).status, AdmissionStatus::Accepted);
	const auto sent = std::get<EventMessage>(Outgoing().body);
	Receive(EffectsMessage{ sent.context, { OpenDocument{ L"private:detail" } } });
	ASSERT_EQ(session.CompletionCount(), 1U);
	session.Stop(StopReason::Disabled);
	const auto result = session.TakeCompleted();
	ASSERT_TRUE(result);
	EXPECT_EQ(result->status, InvocationStatus::Cancelled);
	EXPECT_TRUE(result->effects.empty());
	EXPECT_FALSE(session.TakeCompleted());
}

TEST_F(SenpRuntimeLifecycle, EscapedPayloadsHitTheByteBudgetBeforeTheCountBudget)
{
	ActivateSession();
	ToolCompleted large{ L"read:1", CompletionStatus::Succeeded, std::wstring(65536, wchar_t{ 1 }), L"" };
	for (int i = 0; i < 10; ++i)
		ASSERT_EQ(session.Submit(Context(), large, now + 1s, now).status, AdmissionStatus::Accepted);
	EXPECT_EQ(session.Submit(Context(), large, now + 1s, now).status, AdmissionStatus::Busy);
	EXPECT_EQ(session.PendingCount(), 10U);
	EXPECT_LE(session.QueuedBytes(), kMaximumQueuedBytes);
	EXPECT_GT(session.QueuedBytes(), kMaximumQueuedBytes - 400000U);
	session.TransportFailed();
	EXPECT_EQ(session.QueuedBytes(), 0U);
	EXPECT_EQ(session.CompletionCount(), 10U);
}

} // namespace
