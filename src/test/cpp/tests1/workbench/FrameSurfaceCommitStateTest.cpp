/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameSurfaceCommitState.h"

#include <gtest/gtest.h>

namespace workbench::rendering {
namespace {

TEST(FrameSurfaceCommitState, LayoutNotificationDoesNotPublishBeforeTheGdiBoundary)
{
	FrameSurfaceCommitState state(1);
	ASSERT_TRUE(state.Open("workbench.parts.sidebar", true).Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	EXPECT_TRUE(state.HasPendingFrame());
	EXPECT_EQ(0u, state.Snapshot().committedRequestId);

	const auto committed = state.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_FALSE(state.HasPendingFrame());
	EXPECT_EQ(1u, committed->committedRequestId);
	EXPECT_EQ(2u, committed->layoutEpoch);
}

TEST(FrameSurfaceCommitState, RepeatedLayoutPublishesOnlyTheLatestTicket)
{
	FrameSurfaceCommitState state(2);
	ASSERT_TRUE(state.Open("workbench.parts.sidebar", true).Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());

	const auto committed = state.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_EQ(2u, committed->committedRequestId);
	EXPECT_EQ(3u, committed->layoutEpoch);
	EXPECT_FALSE(state.CommitGdiFrame().has_value());
}

TEST(FrameSurfaceCommitState, HostAndVisibilityChangesFenceAPreviousLayout)
{
	FrameSurfaceCommitState state(3);
	ASSERT_TRUE(state.Open("workbench.parts.sidebar", true).Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	ASSERT_TRUE(state.SetHost("workbench.parts.auxiliarybar").Accepted());
	EXPECT_FALSE(state.CommitGdiFrame().has_value());
	EXPECT_EQ(0u, state.Snapshot().committedRequestId);

	ASSERT_TRUE(state.NotifyLayout().Accepted());
	ASSERT_TRUE(state.SetVisible(false).Accepted());
	EXPECT_FALSE(state.CommitGdiFrame().has_value());
	EXPECT_EQ(0u, state.Snapshot().committedRequestId);

	ASSERT_TRUE(state.NotifyLayout().Accepted());
	const auto committed = state.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_FALSE(committed->visible);
	EXPECT_EQ("workbench.parts.auxiliarybar", committed->hostId);
}

TEST(FrameSurfaceCommitState, CloseDiscardsPendingWorkAndIsTerminalForTheLifetime)
{
	FrameSurfaceCommitState state(4);
	ASSERT_TRUE(state.Open("workbench.parts.sidebar", true).Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	ASSERT_TRUE(state.Close().Accepted());
	EXPECT_FALSE(state.HasPendingFrame());
	EXPECT_FALSE(state.CommitGdiFrame().has_value());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed, state.NotifyLayout().status);
}

TEST(FrameSurfaceCommitState, UnchangedProjectionDoesNotWithdrawPendingFrame)
{
	FrameSurfaceCommitState state(5);
	ASSERT_TRUE(state.Open("workbench.parts.sidebar", true).Accepted());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	ASSERT_TRUE(state.SetHost("workbench.parts.sidebar").Accepted());
	ASSERT_TRUE(state.SetVisible(true).Accepted());
	EXPECT_TRUE(state.HasPendingFrame());
	EXPECT_TRUE(state.CommitGdiFrame().has_value());
}

TEST(FrameSurfaceCommitState, ContentAndDeviceEpochsFencePreviousTickets)
{
	FrameSurfaceCommitState state(6);
	ASSERT_TRUE(state.Open("editor", true, 4, 10, 20, 30).Accepted());
	ASSERT_TRUE(state.RequestCurrent().Accepted());
	ASSERT_TRUE(state.NotifyContent().Accepted());
	const auto content = state.CommitGdiFrame();
	ASSERT_TRUE(content.has_value());
	EXPECT_EQ(31u, content->contentGeneration);
	EXPECT_EQ(20u, content->deviceEpoch);

	ASSERT_TRUE(state.NotifyDeviceEpoch(21).Accepted());
	const auto device = state.CommitGdiFrame();
	ASSERT_TRUE(device.has_value());
	EXPECT_EQ(21u, device->deviceEpoch);
	EXPECT_TRUE(device->hasLastGoodContent);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		state.NotifyDeviceEpoch(20).status);
}

} // namespace
} // namespace workbench::rendering
