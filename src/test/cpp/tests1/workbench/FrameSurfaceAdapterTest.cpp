/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameSurfaceAdapter.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace workbench::rendering {
namespace {

constexpr FrameSurfaceId kSurfaceId = 41;
constexpr std::string_view kHostId = "primary-editor";

FrameSurfaceAdapterRequest RequestFor(
	FrameSurfaceId surfaceId = kSurfaceId, std::uint64_t requestId = 1,
	std::uint64_t lifetimeEpoch = 7, std::uint64_t contentGeneration = 11,
	std::uint64_t layoutEpoch = 13, std::uint64_t deviceEpoch = 17,
	std::string_view hostId = kHostId, bool visible = true,
	std::uint64_t hostEpoch = 1, std::uint64_t visibilityEpoch = 1 )
{
	return {
		.surfaceId = surfaceId,
		.hostId = std::string(hostId),
		.surfaceLifetimeEpoch = lifetimeEpoch,
		.requestId = requestId,
		.contentGeneration = contentGeneration,
		.layoutEpoch = layoutEpoch,
		.deviceEpoch = deviceEpoch,
		.workClass = EFrameWorkClass::Visible,
		.visible = visible,
		.hostEpoch = hostEpoch,
		.visibilityEpoch = visibilityEpoch,
	};
}

TEST(FrameSurfaceAdapter, RejectsInvalidStableIdentityAndOpensWithExplicitProjection)
{
	FrameSurfaceAdapter invalid(0);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		invalid.Open(kHostId, true, 7, 13, 17, 11).status);

	FrameSurfaceAdapter adapter(kSurfaceId);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		adapter.Open("", true, 7, 13, 17, 11).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		adapter.Open(kHostId, true, 0, 13, 17, 11).status);

	const auto opened = adapter.Open(kHostId, true, 7, 13, 17, 11);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded, opened.status);
	const auto snapshot = adapter.Snapshot();
	EXPECT_EQ(kSurfaceId, snapshot.surfaceId);
	EXPECT_EQ(kHostId, snapshot.hostId);
	EXPECT_EQ(EFrameSurfaceAdapterPhase::Idle, snapshot.phase);
	EXPECT_TRUE(snapshot.visible);
	EXPECT_EQ(7U, snapshot.surfaceLifetimeEpoch);
	EXPECT_EQ(11U, snapshot.contentGeneration);
	EXPECT_EQ(13U, snapshot.layoutEpoch);
	EXPECT_EQ(17U, snapshot.deviceEpoch);
	EXPECT_EQ(1U, snapshot.hostEpoch);
	EXPECT_EQ(1U, snapshot.visibilityEpoch);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.SetHost(kHostId).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.SetVisible(true).status);
	EXPECT_EQ(1U, adapter.Snapshot().hostEpoch);
	EXPECT_EQ(1U, adapter.Snapshot().visibilityEpoch);
}

TEST(FrameSurfaceAdapter, RequestTicketCarriesAllEpochsHostAndVisibility)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());

	const auto result = adapter.Request(RequestFor());
	ASSERT_TRUE(result.Accepted());
	ASSERT_TRUE(result.ticket.has_value());
	const auto& ticket = *result.ticket;
	EXPECT_EQ(kSurfaceId, ticket.surfaceId);
	EXPECT_EQ(kHostId, ticket.hostId);
	EXPECT_EQ(7U, ticket.surfaceLifetimeEpoch);
	EXPECT_EQ(1U, ticket.requestId);
	EXPECT_EQ(11U, ticket.contentGeneration);
	EXPECT_EQ(13U, ticket.layoutEpoch);
	EXPECT_EQ(17U, ticket.deviceEpoch);
	EXPECT_TRUE(ticket.visible);
	EXPECT_EQ(1U, ticket.hostEpoch);
	EXPECT_EQ(1U, ticket.visibilityEpoch);
	EXPECT_TRUE(adapter.IsCurrent(ticket));
}

TEST(FrameSurfaceAdapter, CommitPublishesOnlyCurrentRequestAndRetainsLastGoodContent)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto request = adapter.Request(RequestFor());
	ASSERT_TRUE(request.ticket.has_value());

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Commit(*request.ticket).status);
	EXPECT_EQ(EFrameSurfaceAdapterPhase::Committed, adapter.Snapshot().phase);
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);
	EXPECT_EQ(1U, adapter.Snapshot().committedRequestId);
	EXPECT_FALSE(adapter.IsCurrent(*request.ticket));

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Commit(*request.ticket).status);
}

TEST(FrameSurfaceAdapter, LatestRequestReplacesPendingAndOldCommitIsStale)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto first = adapter.Request(RequestFor(kSurfaceId, 1));
	const auto second = adapter.Request(RequestFor(kSurfaceId, 2));
	ASSERT_TRUE(first.ticket.has_value());
	ASSERT_TRUE(second.ticket.has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Replaced, second.status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Commit(*first.ticket).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Commit(*second.ticket).status);
}

TEST(FrameSurfaceAdapter, RejectsWrongSurfaceAndEveryStaleEpoch)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());

	EXPECT_EQ(EFrameSurfaceAdapterStatus::UnknownSurface,
		adapter.Request(RequestFor(99)).status);
	auto invalidHostEpoch = RequestFor();
	invalidHostEpoch.hostEpoch = 0;
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		adapter.Request(invalidHostEpoch).status);
	auto invalidVisibilityEpoch = RequestFor();
	invalidVisibilityEpoch.visibilityEpoch = 0;
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		adapter.Request(invalidVisibilityEpoch).status);

	const auto accepted = adapter.Request(RequestFor());
	ASSERT_TRUE(accepted.ticket.has_value());
	const auto ticket = *accepted.ticket;

	using TicketMutator = void (*)(FrameSurfaceAdapterTicket&);
	const std::array<TicketMutator, 8> mutators{
		[](FrameSurfaceAdapterTicket& value) { value.surfaceLifetimeEpoch = 8; },
		[](FrameSurfaceAdapterTicket& value) { value.contentGeneration = 12; },
		[](FrameSurfaceAdapterTicket& value) { value.layoutEpoch = 14; },
		[](FrameSurfaceAdapterTicket& value) { value.deviceEpoch = 18; },
		[](FrameSurfaceAdapterTicket& value) { value.hostId = "secondary-editor"; },
		[](FrameSurfaceAdapterTicket& value) { value.visible = false; },
		[](FrameSurfaceAdapterTicket& value) { value.hostEpoch = 2; },
		[](FrameSurfaceAdapterTicket& value) { value.visibilityEpoch = 2; },
	};
	for (const auto mutate : mutators) {
		auto stale = ticket;
		mutate(stale);
		EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale, adapter.Commit(stale).status);
	}

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded, adapter.Commit(ticket).status);
}

TEST(FrameSurfaceAdapter, ContextChangesWithdrawRequestButPreserveLastGoodProjection)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto first = adapter.Request(RequestFor());
	ASSERT_TRUE(first.ticket.has_value());
	ASSERT_TRUE(adapter.Commit(*first.ticket).Accepted());

	const auto second = adapter.Request(RequestFor(kSurfaceId, 2));
	ASSERT_TRUE(second.ticket.has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.UpdateEpochs(12, 14, 18).status);
	EXPECT_EQ(EFrameSurfaceAdapterPhase::Committed, adapter.Snapshot().phase);
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Commit(*second.ticket).status);

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.SetHost("secondary-editor").status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded, adapter.SetVisible(false).status);
	EXPECT_EQ("secondary-editor", adapter.Snapshot().hostId);
	EXPECT_FALSE(adapter.Snapshot().visible);
	EXPECT_EQ(2U, adapter.Snapshot().hostEpoch);
	EXPECT_EQ(2U, adapter.Snapshot().visibilityEpoch);
}

TEST(FrameSurfaceAdapter, HostEpochRejectsTicketAfterHostReturnsToOriginalId)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto old = adapter.Request(RequestFor());
	ASSERT_TRUE(old.ticket.has_value());

	ASSERT_TRUE(adapter.SetHost("secondary-editor").Accepted());
	EXPECT_EQ(2U, adapter.Snapshot().hostEpoch);
	ASSERT_TRUE(adapter.SetHost(kHostId).Accepted());
	EXPECT_EQ(3U, adapter.Snapshot().hostEpoch);

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Commit(*old.ticket).status);
	const auto current = adapter.Request(RequestFor(kSurfaceId, 2, 7, 11, 13, 17,
		kHostId, true, 3, 1));
	ASSERT_TRUE(current.ticket.has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Commit(*current.ticket).status);
}

TEST(FrameSurfaceAdapter, VisibilityEpochRejectsTicketAfterVisibilityReturnsToOriginalState)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto old = adapter.Request(RequestFor());
	ASSERT_TRUE(old.ticket.has_value());

	ASSERT_TRUE(adapter.SetVisible(false).Accepted());
	EXPECT_EQ(2U, adapter.Snapshot().visibilityEpoch);
	ASSERT_TRUE(adapter.SetVisible(true).Accepted());
	EXPECT_EQ(3U, adapter.Snapshot().visibilityEpoch);

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Commit(*old.ticket).status);
	const auto current = adapter.Request(RequestFor(kSurfaceId, 2, 7, 11, 13, 17,
		kHostId, true, 1, 3));
	ASSERT_TRUE(current.ticket.has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Commit(*current.ticket).status);
}

TEST(FrameSurfaceAdapter, RejectsEpochRegressionAndClosedUpdates)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.UpdateEpochs(10, 14, 17).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Invalid,
		adapter.UpdateEpochs(12, 0, 18).status);

	ASSERT_EQ(EFrameSurfaceAdapterStatus::Succeeded, adapter.Close(7).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed,
		adapter.UpdateEpochs(12, 14, 18).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed, adapter.SetVisible(false).status);
}

TEST(FrameSurfaceAdapter, CloseIsTerminalForLifetimeAndReopenRequiresNewEpoch)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());
	const auto old = adapter.Request(RequestFor());
	ASSERT_TRUE(old.ticket.has_value());

	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded, adapter.Close(7).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed,
		adapter.Commit(*old.ticket).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale, adapter.Open(kHostId, true, 7, 13, 17, 11).status);

	ASSERT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Open("secondary-editor", false, 8, 14, 18, 12).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale, adapter.Commit(*old.ticket).status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale, adapter.Close(7).status);

	const auto current = adapter.Request(RequestFor(kSurfaceId, 1, 8, 12, 14, 18,
		"secondary-editor", false));
	ASSERT_TRUE(current.ticket.has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Succeeded,
		adapter.Commit(*current.ticket).status);
}

TEST(FrameSurfaceAdapter, RequestIdsAreMonotonicAndExhaustionIsExplicit)
{
	FrameSurfaceAdapter adapter(kSurfaceId);
	ASSERT_TRUE(adapter.Open(kHostId, true, 7, 13, 17, 11).Accepted());

	const auto maximum = RequestFor(kSurfaceId,
		(std::numeric_limits<std::uint64_t>::max)());
	ASSERT_TRUE(adapter.Request(maximum).Accepted());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Exhausted,
		adapter.Request(RequestFor(kSurfaceId, 1)).status);

	ASSERT_TRUE(adapter.Close(7).Accepted());
	ASSERT_TRUE(adapter.Open(kHostId, true, 8, 13, 17, 11).Accepted());
	EXPECT_TRUE(adapter.Request(RequestFor(kSurfaceId, 1, 8)).Accepted());
}

} // namespace
} // namespace workbench::rendering
