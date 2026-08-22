/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/model/TerminalModel.h"
#include "terminal/window/TerminalRenderMapping.h"
#include "terminal/window/TerminalSurfaceAdapter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace terminal {
namespace {

using workbench::rendering::EFrameSurfaceAdapterStatus;

TEST(TerminalSurfaceAdapter, DirtyRowsStayLimitedToTheVisibleViewport)
{
	TerminalModel model(8, 4);
	const auto viewport = CalculateTerminalViewport(model, 2, 0);
	const auto mapped = MapDirtyRowsToViewport(model, viewport, { 0, 1, 2, 3 });

	// The projection receives only damage that can be painted in the current
	// viewport; offscreen model rows do not expand the paint work.
	ASSERT_EQ(2u, mapped.size());
	EXPECT_EQ(0u, mapped[0]);
	EXPECT_EQ(1u, mapped[1]);
}

TEST(TerminalSurfaceAdapter, RepeatedModelDrainsPublishOnlyTheNewestGeneration)
{
	TerminalSurfaceAdapter adapter(101);
	ASSERT_TRUE(adapter.Open("workbench.parts.panel", true, 7, 10, 20, 30).Accepted());

	for( std::size_t index = 0; index < 1024; ++index ) {
		ASSERT_TRUE(adapter.NotifyContent().Accepted());
	}
	EXPECT_TRUE(adapter.HasPendingFrame());
	EXPECT_EQ(1054u, adapter.Snapshot().contentGeneration);
	EXPECT_EQ(0u, adapter.Snapshot().committedRequestId);

	const auto committed = adapter.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_EQ(1054u, committed->contentGeneration);
	EXPECT_EQ(1024u, committed->committedRequestId);
	EXPECT_FALSE(adapter.HasPendingFrame());
	EXPECT_FALSE(adapter.CommitGdiFrame().has_value());
}

TEST(TerminalSurfaceAdapter, LayoutAndDeviceEpochsRetainTheLastGoodDib)
{
	TerminalSurfaceAdapter adapter(102);
	ASSERT_TRUE(adapter.Open("workbench.parts.panel", true, 11, 12, 13, 14).Accepted());
	ASSERT_TRUE(adapter.RequestCurrent().Accepted());
	ASSERT_TRUE(adapter.CommitGdiFrame().has_value());
	ASSERT_TRUE(adapter.Snapshot().hasLastGoodContent);

	ASSERT_TRUE(adapter.NotifyLayout().Accepted());
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);
	EXPECT_EQ(13u, adapter.Snapshot().layoutEpoch);
	ASSERT_TRUE(adapter.CommitGdiFrame().has_value());

	ASSERT_TRUE(adapter.NotifyDeviceEpoch(14).Accepted());
	EXPECT_TRUE(adapter.Snapshot().hasLastGoodContent);
	const auto device = adapter.CommitGdiFrame();
	ASSERT_TRUE(device.has_value());
	EXPECT_EQ(14u, device->deviceEpoch);
	EXPECT_TRUE(device->hasLastGoodContent);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.NotifyDeviceEpoch(13).status);
}

TEST(TerminalSurfaceAdapter, HostVisibilityAndLifetimeFenceLateProjection)
{
	TerminalSurfaceAdapter adapter(103);
	ASSERT_TRUE(adapter.Open("workbench.parts.panel", true, 21, 1, 1, 1).Accepted());
	ASSERT_TRUE(adapter.NotifyContent().Accepted());
	ASSERT_TRUE(adapter.SetHost("workbench.parts.auxiliarybar").Accepted());
	ASSERT_TRUE(adapter.SetVisible(false).Accepted());
	EXPECT_EQ(2u, adapter.Snapshot().hostEpoch);
	EXPECT_EQ(2u, adapter.Snapshot().visibilityEpoch);
	EXPECT_FALSE(adapter.Snapshot().visible);
	EXPECT_EQ("workbench.parts.auxiliarybar", adapter.Snapshot().hostId);
	ASSERT_TRUE(adapter.CommitGdiFrame().has_value());

	ASSERT_TRUE(adapter.Close().Accepted());
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Closed, adapter.NotifyContent().status);
	EXPECT_EQ(EFrameSurfaceAdapterStatus::Stale,
		adapter.Open("workbench.parts.panel", true, 21, 1, 1, 1).status);
	ASSERT_TRUE(adapter.Open("workbench.parts.panel", true, 22, 1, 1, 1).Accepted());
	EXPECT_EQ(22u, adapter.Snapshot().surfaceLifetimeEpoch);
}

} // namespace
} // namespace terminal
