/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameWindowTransaction.h"

#include <gtest/gtest.h>

namespace workbench::rendering {
namespace {

FrameWindowSurfaceSpec Surface(
	const EFrameWindowSurfaceRole role, const char* host,
	const bool visible = true)
{
	return {
		.surfaceId = FrameWindowSurfaceId(role),
		.hostId = host,
		.visible = visible,
	};
}

TEST(FrameWindowTransaction, SharesLayoutEpochWithoutAReadinessBarrier)
{
	FrameWindowTransaction transaction;
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::ActivityBar, "workbench.parts.activitybar")).Accepted());
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::PrimarySideBar, "workbench.parts.sidebar")).Accepted());
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::StatusBar, "workbench.parts.statusbar")).Accepted());

	const auto layout = transaction.BeginLayout();
	ASSERT_TRUE(layout.Accepted());
	EXPECT_EQ(3u, layout.acceptedSurfaceCount);
	EXPECT_EQ(2u, transaction.LayoutEpoch());

	const auto activity = transaction.CommitSurfaceGdi(
		FrameWindowSurfaceId(EFrameWindowSurfaceRole::ActivityBar));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ(2u, activity->layoutEpoch);
	EXPECT_FALSE(transaction.SurfaceSnapshot(
		FrameWindowSurfaceId(EFrameWindowSurfaceRole::PrimarySideBar))
		->hasLastGoodContent);

	const auto remaining = transaction.CommitGdiBoundary();
	ASSERT_EQ(2u, remaining.size());
	EXPECT_EQ(2u, remaining[0].layoutEpoch);
	EXPECT_EQ(2u, remaining[1].layoutEpoch);
	EXPECT_EQ(3u, transaction.Telemetry().committedSurfaces);
}

TEST(FrameWindowTransaction, ContentDamageIsIndependentAndLatestOnly)
{
	FrameWindowTransaction transaction;
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::Tabs, "workbench.parts.editor")).Accepted());
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::Editor, "workbench.parts.editor")).Accepted());

	const auto editorId = FrameWindowSurfaceId(EFrameWindowSurfaceRole::Editor);
	ASSERT_TRUE(transaction.NotifyContent(editorId).Accepted());
	ASSERT_TRUE(transaction.NotifyContent(editorId).Accepted());
	const auto committed = transaction.CommitGdiBoundary();
	ASSERT_EQ(1u, committed.size());
	EXPECT_EQ(editorId, committed.front().surfaceId);
	EXPECT_EQ(3u, committed.front().contentGeneration);
	EXPECT_EQ(1u, committed.front().layoutEpoch);
	EXPECT_FALSE(transaction.SurfaceSnapshot(
		FrameWindowSurfaceId(EFrameWindowSurfaceRole::Tabs))->hasLastGoodContent);
}

TEST(FrameWindowTransaction, DeviceEpochReprojectsEveryOpenSurface)
{
	FrameWindowTransaction transaction;
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::MarkdownPreview, "workbench.parts.editor")).Accepted());
	ASSERT_TRUE(transaction.OpenSurface(Surface(
		EFrameWindowSurfaceRole::Terminal, "workbench.panel.terminal")).Accepted());
	ASSERT_TRUE(transaction.BeginLayout().Accepted());
	ASSERT_EQ(2u, transaction.CommitGdiBoundary().size());

	const auto device = transaction.SetDeviceEpoch(2);
	ASSERT_TRUE(device.Accepted());
	EXPECT_EQ(2u, device.acceptedSurfaceCount);
	const auto committed = transaction.CommitGdiBoundary();
	ASSERT_EQ(2u, committed.size());
	EXPECT_EQ(2u, committed[0].deviceEpoch);
	EXPECT_EQ(2u, committed[1].deviceEpoch);
	EXPECT_TRUE(committed[0].hasLastGoodContent);
	EXPECT_TRUE(committed[1].hasLastGoodContent);
}

TEST(FrameWindowTransaction, CapacityAndLifetimeReopenAreExplicit)
{
	FrameWindowTransaction transaction(1);
	const auto activity = Surface(
		EFrameWindowSurfaceRole::ActivityBar, "workbench.parts.activitybar");
	ASSERT_TRUE(transaction.OpenSurface(activity, 1).Accepted());
	EXPECT_EQ(EFrameWindowTransactionStatus::Full,
		transaction.OpenSurface(Surface(
			EFrameWindowSurfaceRole::Tabs, "workbench.parts.editor")).status);
	ASSERT_TRUE(transaction.CloseSurface(activity.surfaceId).Accepted());
	EXPECT_EQ(EFrameWindowTransactionStatus::Invalid,
		transaction.OpenSurface(activity, 1).status);
	EXPECT_TRUE(transaction.OpenSurface(activity, 2).Accepted());
	EXPECT_EQ(2u, transaction.SurfaceSnapshot(activity.surfaceId)
		->surfaceLifetimeEpoch);
	EXPECT_TRUE(transaction.Close().Accepted());
	EXPECT_EQ(EFrameWindowTransactionStatus::Closed,
		transaction.BeginLayout().status);
}

} // namespace
} // namespace workbench::rendering
