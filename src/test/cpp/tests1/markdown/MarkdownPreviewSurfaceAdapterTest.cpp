/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "markdown/MarkdownPreviewSurfaceAdapter.h"

#include <gtest/gtest.h>

namespace markdown {
namespace {

using workbench::rendering::EFrameSurfaceAdapterPhase;
using workbench::rendering::EFrameSurfaceAdapterStatus;

TEST(MarkdownPreviewSurfaceAdapter, OpensWithStableIdentityAndAllExplicitEpochs)
{
	MarkdownPreviewSurfaceAdapter adapter;
	ASSERT_TRUE(adapter.Open("markdown-host", true, 7, 11, 13, 17).Accepted());

	const auto snapshot = adapter.Snapshot();
	EXPECT_EQ(kMarkdownPreviewSurfaceId, snapshot.surfaceId);
	EXPECT_EQ("markdown-host", snapshot.hostId);
	EXPECT_EQ(7U, snapshot.surfaceLifetimeEpoch);
	EXPECT_EQ(11U, snapshot.layoutEpoch);
	EXPECT_EQ(13U, snapshot.deviceEpoch);
	EXPECT_EQ(17U, snapshot.contentGeneration);
	EXPECT_TRUE(snapshot.visible);
	EXPECT_EQ(EFrameSurfaceAdapterPhase::Idle, snapshot.phase);
}

TEST(MarkdownPreviewSurfaceAdapter, DoesNotPublishBeforeTheExplicitGdiBoundary)
{
	MarkdownPreviewSurfaceAdapter adapter(42);
	ASSERT_TRUE(adapter.Open("markdown-host", true).Accepted());
	ASSERT_TRUE(adapter.NotifyContent().Accepted());

	EXPECT_TRUE(adapter.HasPendingFrame());
	EXPECT_EQ(0U, adapter.Snapshot().committedRequestId);
	EXPECT_FALSE(adapter.Snapshot().hasLastGoodContent);

	const auto committed = adapter.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_EQ(2U, committed->contentGeneration);
	EXPECT_EQ(1U, committed->committedRequestId);
	EXPECT_TRUE(committed->hasLastGoodContent);
	EXPECT_FALSE(adapter.HasPendingFrame());
}

TEST(MarkdownPreviewSurfaceAdapter, StaleEpochKeepsLastGoodProjection)
{
	MarkdownPreviewSurfaceAdapter adapter(43);
	ASSERT_TRUE(adapter.Open("markdown-host", true).Accepted());
	ASSERT_TRUE(adapter.NotifyContent().Accepted());
	ASSERT_TRUE(adapter.CommitGdiFrame().has_value());
	const auto before = adapter.Snapshot();

	ASSERT_TRUE(adapter.NotifyDeviceEpoch(2).Accepted());
	EXPECT_TRUE(adapter.HasPendingFrame());
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);
	EXPECT_EQ(before.committedRequestId, adapter.Snapshot().committedRequestId);

	// The pending replacement is not publishable until the owner reaches the
	// next GDI boundary. The previous frame remains the observable last-good one.
	const auto replacement = adapter.CommitGdiFrame();
	ASSERT_TRUE(replacement.has_value());
	EXPECT_EQ(2U, replacement->deviceEpoch);
	EXPECT_EQ(2U, replacement->committedRequestId);
	EXPECT_TRUE(replacement->hasLastGoodContent);
}

TEST(MarkdownPreviewSurfaceAdapter, HostAndVisibilityEpochsFencePendingWork)
{
	MarkdownPreviewSurfaceAdapter adapter(44);
	ASSERT_TRUE(adapter.Open("markdown-host", true).Accepted());
	ASSERT_TRUE(adapter.NotifyContent().Accepted());
	ASSERT_TRUE(adapter.CommitGdiFrame().has_value());

	const auto oldHostEpoch = adapter.Snapshot().hostEpoch;
	ASSERT_TRUE(adapter.SetHost("secondary-markdown-host").Accepted());
	EXPECT_EQ(oldHostEpoch + 1, adapter.Snapshot().hostEpoch);
	EXPECT_EQ(1U, adapter.Snapshot().committedRequestId);
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);

	ASSERT_TRUE(adapter.SetVisible(false).Accepted());
	EXPECT_FALSE(adapter.Snapshot().visible);
	EXPECT_EQ(2U, adapter.Snapshot().visibilityEpoch);
	EXPECT_EQ(1U, adapter.Snapshot().committedRequestId);

	ASSERT_TRUE(adapter.NotifyLayout().Accepted());
	const auto committed = adapter.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_EQ("secondary-markdown-host", committed->hostId);
	EXPECT_FALSE(committed->visible);
	EXPECT_EQ(2U, committed->hostEpoch);
	EXPECT_EQ(2U, committed->visibilityEpoch);
}

TEST(MarkdownPreviewSurfaceAdapter, CloseIsTerminalAndReopenAdvancesLifetime)
{
	MarkdownPreviewSurfaceAdapter adapter(45);
	ASSERT_TRUE(adapter.Open("markdown-host", true).Accepted());
	ASSERT_TRUE(adapter.NotifyContent().Accepted());
	ASSERT_TRUE(adapter.Close().Accepted());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed, adapter.NotifyLayout().status);
	EXPECT_FALSE(adapter.CommitGdiFrame().has_value());

	ASSERT_TRUE(adapter.Open("markdown-host", true).Accepted());
	EXPECT_EQ(2U, adapter.Snapshot().surfaceLifetimeEpoch);
	EXPECT_EQ(0U, adapter.Snapshot().committedRequestId);
	EXPECT_FALSE(adapter.Snapshot().hasLastGoodContent);
}

} // namespace
} // namespace markdown
