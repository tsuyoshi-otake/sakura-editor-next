/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameCoordinatorModel.h"
#include "workbench/rendering/LatestOnlyMailbox.h"

#include <gtest/gtest.h>

#include <limits>

namespace workbench::rendering {
namespace {

FrameSurfaceRequest RequestFor(
	FrameSurfaceId surfaceId, std::uint64_t requestId,
	EFrameWorkClass workClass = EFrameWorkClass::Visible,
	std::uint64_t lifetimeEpoch = 1, std::uint64_t layoutEpoch = 1,
	std::uint64_t deviceEpoch = 1, bool visible = true )
{
	return {
		.surfaceId = surfaceId,
		.surfaceLifetimeEpoch = lifetimeEpoch,
		.requestId = requestId,
		.contentGeneration = requestId,
		.layoutEpoch = layoutEpoch,
		.deviceEpoch = deviceEpoch,
		.workClass = workClass,
		.visible = visible,
	};
}

FrameWorkTicket AdvanceToPublishable( FrameCoordinatorModel& coordinator )
{
	auto ticket = coordinator.TakeNextCpuWork();
	EXPECT_TRUE(ticket.has_value());
	if( !ticket ) return {};
	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.CompleteCpu(*ticket).status);
	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.QueueGpu(*ticket).status);
	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.BeginGpu(*ticket).status);
	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.CompleteGpu(*ticket).status);
	return *ticket;
}

TEST(LatestOnlyMailbox, CoalescesWakeAndKeepsOnlyNewestPayload)
{
	LatestOnlyMailbox<int> mailbox;
	const auto first = mailbox.Publish(10);
	EXPECT_TRUE(first.accepted);
	EXPECT_FALSE(first.replaced);
	EXPECT_TRUE(first.wakeRequired);
	const auto second = mailbox.Publish(20);
	EXPECT_TRUE(second.accepted);
	EXPECT_TRUE(second.replaced);
	EXPECT_FALSE(second.wakeRequired);
	EXPECT_EQ(1u, mailbox.Depth());

	auto value = mailbox.Take();
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(20, *value);
	EXPECT_EQ(0u, mailbox.Depth());
	EXPECT_FALSE(mailbox.WakePending());
}

TEST(LatestOnlyMailbox, CloseRejectsLatePayloadAndOpenStartsEmpty)
{
	LatestOnlyMailbox<int> mailbox;
	EXPECT_TRUE(mailbox.Publish(1).accepted);
	mailbox.Close();
	EXPECT_TRUE(mailbox.Closed());
	EXPECT_FALSE(mailbox.Publish(2).accepted);
	EXPECT_FALSE(mailbox.Take().has_value());

	mailbox.Open();
	EXPECT_FALSE(mailbox.Closed());
	const auto publication = mailbox.Publish(3);
	EXPECT_TRUE(publication.accepted);
	EXPECT_TRUE(publication.wakeRequired);
	auto value = mailbox.Take();
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ(3, *value);
}

TEST(FrameCoordinatorModel, LatestOnlyMailboxReplacesPendingRequest)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(10, 1).Accepted());

	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.Request(RequestFor(10, 1)).status);
	EXPECT_EQ(EFrameOperationStatus::Replaced, coordinator.Request(RequestFor(10, 2)).status);
	const auto snapshot = coordinator.SurfaceSnapshot(10);
	ASSERT_TRUE(snapshot.has_value());
	EXPECT_EQ(1u, snapshot->pendingDepth);
	EXPECT_EQ(2u, snapshot->newestRequestId);
	EXPECT_EQ(1u, coordinator.Telemetry().replacedRequests);

	const auto ticket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(ticket.has_value());
	EXPECT_EQ(2u, ticket->requestId);
	EXPECT_EQ(EFrameOperationStatus::Stale, coordinator.Request(RequestFor(10, 2)).status);
}

TEST(FrameCoordinatorModel, ReadySurfaceDoesNotWaitForStalledSurface)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(1, 1).Accepted());
	ASSERT_TRUE(coordinator.RegisterSurface(2, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(1, 1, EFrameWorkClass::Interactive)).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(2, 1)).Accepted());

	const auto editorTicket = AdvanceToPublishable(coordinator);
	ASSERT_EQ(1u, editorTicket.surfaceId);
	const auto stalledTicket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(stalledTicket.has_value());
	ASSERT_EQ(2u, stalledTicket->surfaceId);

	const auto cohort = coordinator.AssembleCommit(1, 1, 8);
	ASSERT_EQ(1u, cohort.publications.size());
	EXPECT_EQ(1u, cohort.publications.front().ticket.surfaceId);
	ASSERT_EQ(1u, cohort.lateSurfaces.size());
	EXPECT_EQ(2u, cohort.lateSurfaces.front().surfaceId);
	EXPECT_FALSE(cohort.lateSurfaces.front().hasLastGoodContent);
	EXPECT_TRUE(coordinator.CompleteCommit(cohort).Accepted());

	const auto editor = coordinator.SurfaceSnapshot(1);
	const auto stalled = coordinator.SurfaceSnapshot(2);
	ASSERT_TRUE(editor && stalled);
	EXPECT_TRUE(editor->hasLastGoodContent);
	EXPECT_EQ(EFrameSurfacePhase::CpuRunning, stalled->phase);
}

TEST(FrameCoordinatorModel, NewRequestSupersedesCpuCompletionAndRequiresRetirement)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(7, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(7, 1)).Accepted());
	const auto first = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(coordinator.Request(RequestFor(7, 2)).Accepted());

	const auto completion = coordinator.CompleteCpu(*first);
	EXPECT_EQ(EFrameOperationStatus::Superseded, completion.status);
	EXPECT_EQ(EFrameSurfacePhase::Withdrawn, completion.phase);
	EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.RetireWithdrawn(*first).status);

	const auto second = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(2u, second->requestId);
	EXPECT_EQ(1u, coordinator.Telemetry().supersededWork);
}

TEST(FrameCoordinatorModel, AgingEventuallySchedulesBackgroundSurface)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(1, 1).Accepted());
	ASSERT_TRUE(coordinator.RegisterSurface(2, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(2, 1, EFrameWorkClass::Background)).Accepted());

	bool backgroundScheduled = false;
	for( std::uint64_t requestId = 1; requestId <= 32; ++requestId ) {
		ASSERT_TRUE(coordinator.Request(RequestFor(1, requestId, EFrameWorkClass::Interactive)).Accepted());
		auto ticket = coordinator.TakeNextCpuWork();
		ASSERT_TRUE(ticket.has_value());
		if( ticket->surfaceId == 2 ) {
			backgroundScheduled = true;
			break;
		}
		EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.CompleteCpu(*ticket).status);
		EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.QueueGpu(*ticket).status);
		EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.BeginGpu(*ticket).status);
		EXPECT_EQ(EFrameOperationStatus::Succeeded, coordinator.CompleteGpu(*ticket).status);
		auto cohort = coordinator.AssembleCommit(requestId, 1, 1);
		ASSERT_EQ(1u, cohort.publications.size());
		EXPECT_TRUE(coordinator.CompleteCommit(cohort).Accepted());
	}
	EXPECT_TRUE(backgroundScheduled);
}

TEST(FrameCoordinatorModel, DeviceResetRejectsOldTicketsAndRequiresRepublish)
{
	FrameCoordinatorModel coordinator(3);
	ASSERT_TRUE(coordinator.RegisterSurface(5, 9).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(5, 1, EFrameWorkClass::Visible, 9, 4, 3)).Accepted());
	const auto oldTicket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(oldTicket.has_value());

	EXPECT_TRUE(coordinator.ResetDevice(4).Accepted());
	EXPECT_EQ(4u, coordinator.DeviceEpoch());
	EXPECT_EQ(EFrameOperationStatus::Stale, coordinator.CompleteCpu(*oldTicket).status);
	const auto snapshot = coordinator.SurfaceSnapshot(5);
	ASSERT_TRUE(snapshot.has_value());
	EXPECT_EQ(EFrameSurfacePhase::Idle, snapshot->phase);
	EXPECT_FALSE(snapshot->hasLastGoodContent);

	EXPECT_EQ(EFrameOperationStatus::Stale,
		coordinator.Request(RequestFor(5, 2, EFrameWorkClass::Visible, 9, 4, 3)).status);
	EXPECT_TRUE(coordinator.Request(RequestFor(5, 2, EFrameWorkClass::Visible, 9, 4, 4)).Accepted());
}

TEST(FrameCoordinatorModel, ClosedLifetimeRejectsStaleCompletionAndCanReopenWithNewEpoch)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(3, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(3, 1)).Accepted());
	const auto ticket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(ticket.has_value());
	EXPECT_TRUE(coordinator.CloseSurface(3, 1).Accepted());
	EXPECT_EQ(EFrameOperationStatus::Closed, coordinator.CompleteCpu(*ticket).status);
	EXPECT_EQ(EFrameOperationStatus::Stale, coordinator.RegisterSurface(3, 1).status);
	EXPECT_TRUE(coordinator.RegisterSurface(3, 2).Accepted());
	EXPECT_EQ(EFrameOperationStatus::Stale, coordinator.Request(RequestFor(3, 2)).status);
	EXPECT_TRUE(coordinator.Request(RequestFor(3, 2, EFrameWorkClass::Visible, 2)).Accepted());
}

TEST(FrameCoordinatorModel, CommitRejectsMixedLayoutEpochWithoutBlockingCurrentLayout)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(1, 1).Accepted());
	ASSERT_TRUE(coordinator.RegisterSurface(2, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(1, 1, EFrameWorkClass::Interactive, 1, 2)).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(2, 1, EFrameWorkClass::Visible, 1, 1)).Accepted());

	const auto current = AdvanceToPublishable(coordinator);
	ASSERT_EQ(1u, current.surfaceId);
	const auto oldLayout = AdvanceToPublishable(coordinator);
	ASSERT_EQ(2u, oldLayout.surfaceId);

	const auto cohort = coordinator.AssembleCommit(1, 2, 8);
	ASSERT_EQ(1u, cohort.publications.size());
	EXPECT_EQ(1u, cohort.publications.front().ticket.surfaceId);
	ASSERT_EQ(1u, cohort.lateSurfaces.size());
	EXPECT_EQ(2u, cohort.lateSurfaces.front().surfaceId);
	EXPECT_TRUE(coordinator.CompleteCommit(cohort).Accepted());
}

TEST(FrameCoordinatorModel, ForgedMixedEpochAndDuplicateCohortsAreRejectedAtomically)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(1, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(1, 1)).Accepted());
	AdvanceToPublishable(coordinator);

	auto mixedEpoch = coordinator.AssembleCommit(1, 1, 8);
	ASSERT_EQ(1u, mixedEpoch.publications.size());
	mixedEpoch.publications.front().ticket.layoutEpoch = 2;
	EXPECT_EQ(EFrameOperationStatus::Invalid, coordinator.CompleteCommit(mixedEpoch).status);
	EXPECT_EQ(EFrameSurfacePhase::Publishable, coordinator.SurfaceSnapshot(1)->phase);

	auto duplicate = coordinator.AssembleCommit(1, 1, 8);
	duplicate.publications.push_back(duplicate.publications.front());
	EXPECT_EQ(EFrameOperationStatus::Invalid, coordinator.CompleteCommit(duplicate).status);
	EXPECT_EQ(EFrameSurfacePhase::Publishable, coordinator.SurfaceSnapshot(1)->phase);
}

TEST(FrameCoordinatorModel, RequestIdExhaustionIsAnExplicitTerminalForThatLifetime)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(1, 1).Accepted());
	auto request = RequestFor(1, (std::numeric_limits<std::uint64_t>::max)());
	request.contentGeneration = 1;
	ASSERT_TRUE(coordinator.Request(request).Accepted());

	request.requestId = 1;
	request.contentGeneration = 2;
	EXPECT_EQ(EFrameOperationStatus::Exhausted, coordinator.Request(request).status);
	EXPECT_TRUE(coordinator.CloseSurface(1, 1).Accepted());
	EXPECT_TRUE(coordinator.RegisterSurface(1, 2).Accepted());
	request.surfaceLifetimeEpoch = 2;
	EXPECT_TRUE(coordinator.Request(request).Accepted());
}

TEST(FrameCoordinatorModel, CloseDuringGpuWorkWaitsForExplicitWithdrawalRetirement)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(8, 1).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(8, 1)).Accepted());
	const auto ticket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(ticket.has_value());
	ASSERT_TRUE(coordinator.CompleteCpu(*ticket).Accepted());
	ASSERT_TRUE(coordinator.QueueGpu(*ticket).Accepted());
	ASSERT_TRUE(coordinator.BeginGpu(*ticket).Accepted());

	const auto close = coordinator.CloseSurface(8, 1);
	EXPECT_EQ(EFrameOperationStatus::Busy, close.status);
	EXPECT_EQ(EFrameSurfacePhase::Closing, close.phase);
	EXPECT_EQ(EFrameOperationStatus::Closed, coordinator.Request(RequestFor(8, 2)).status);

	const auto completion = coordinator.CompleteGpu(*ticket);
	EXPECT_EQ(EFrameOperationStatus::Superseded, completion.status);
	EXPECT_EQ(EFrameSurfacePhase::Withdrawn, completion.phase);
	EXPECT_TRUE(coordinator.RetireWithdrawn(*ticket).Accepted());
	const auto snapshot = coordinator.SurfaceSnapshot(8);
	ASSERT_TRUE(snapshot.has_value());
	EXPECT_EQ(EFrameSurfacePhase::Closed, snapshot->phase);
	EXPECT_FALSE(snapshot->closeRequested);
}

TEST(FrameCoordinatorModel, OwnerShutdownCanFinalizeAnInFlightGpuSurface)
{
	FrameCoordinatorModel coordinator;
	ASSERT_TRUE(coordinator.RegisterSurface(9, 4).Accepted());
	ASSERT_TRUE(coordinator.Request(RequestFor(9, 1, EFrameWorkClass::Visible, 4)).Accepted());
	const auto ticket = coordinator.TakeNextCpuWork();
	ASSERT_TRUE(ticket.has_value());
	ASSERT_TRUE(coordinator.CompleteCpu(*ticket).Accepted());
	ASSERT_TRUE(coordinator.QueueGpu(*ticket).Accepted());
	ASSERT_TRUE(coordinator.BeginGpu(*ticket).Accepted());
	ASSERT_EQ(EFrameSurfacePhase::Closing, coordinator.CloseSurface(9, 4).phase);

	const auto finalized = coordinator.FinalizeCloseSurface(9, 4);
	EXPECT_EQ(EFrameOperationStatus::Succeeded, finalized.status);
	EXPECT_EQ(EFrameSurfacePhase::Closed, finalized.phase);
	const auto snapshot = coordinator.SurfaceSnapshot(9);
	ASSERT_TRUE(snapshot.has_value());
	EXPECT_EQ(EFrameSurfacePhase::Closed, snapshot->phase);
	EXPECT_EQ(0u, snapshot->pendingDepth);
	EXPECT_EQ(0u, snapshot->activeRequestId);
	EXPECT_FALSE(snapshot->hasLastGoodContent);
	EXPECT_FALSE(snapshot->closeRequested);
	EXPECT_EQ(EFrameOperationStatus::Closed, coordinator.CompleteGpu(*ticket).status);
}

} // namespace
} // namespace workbench::rendering
