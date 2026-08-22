/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameCadenceSource.h"

#include <gtest/gtest.h>

#include <Windows.h>

namespace workbench::rendering {
namespace {

TEST(FrameCadenceSource, InvalidWindowStillPublishesExplicitEpoch)
{
	FrameCadenceSource source;
	const auto observation = source.Observe(nullptr);
	EXPECT_EQ(1u, observation.input.displayEpoch);
	EXPECT_FALSE(observation.monitorObserved);
	EXPECT_FALSE(observation.displayRateObserved);
	EXPECT_FALSE(observation.compositorRateObserved);
	EXPECT_TRUE(FrameCadence::Calculate(observation.input).valid);

	const auto beforeInvalidate = source.DisplayEpoch();
	source.Invalidate();
	EXPECT_GT(source.DisplayEpoch(), beforeInvalidate);
}

TEST(FrameCadenceSource, ReadsRealDesktopWindowWithoutCreatingAClock)
{
	FrameCadenceSource source;
	const auto observation = source.Observe(::GetDesktopWindow());
	EXPECT_NE(0u, observation.input.displayEpoch);
	if (observation.displayRateObserved) {
		EXPECT_GE(observation.input.displayRefreshRateHz, 1u);
		EXPECT_LE(observation.input.displayRefreshRateHz, 1000u);
	}
	if (observation.compositorRateObserved) {
		EXPECT_GE(observation.input.compositorRefreshRateHz, 1u);
		EXPECT_LE(observation.input.compositorRefreshRateHz, 1000u);
		EXPECT_NE(0u, observation.observedQpc);
	}
	const auto cadence = FrameCadence::Calculate(observation.input);
	EXPECT_TRUE(cadence.valid);
	EXPECT_GE(cadence.effectiveRefreshRateHz, 1u);
	EXPECT_LE(cadence.effectiveRefreshRateHz, 1000u);
}

TEST(FrameCadenceSource, InvalidateDoesNotResetTheEpochToAnImplicitZero)
{
	FrameCadenceSource source;
	const auto first = source.Observe(nullptr);
	ASSERT_EQ(1u, first.input.displayEpoch);
	source.Invalidate();
	const auto second = source.Observe(nullptr);
	EXPECT_GT(second.input.displayEpoch, first.input.displayEpoch);
	EXPECT_EQ(source.DisplayEpoch(), second.input.displayEpoch);
}

} // namespace
} // namespace workbench::rendering
