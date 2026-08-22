/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameDeviceDomainModel.h"

#include <gtest/gtest.h>

namespace workbench::rendering {
namespace {

TEST(FrameDeviceDomainModel, DeviceLossAdvancesEpochOnceAndRejectsDuplicateSignals)
{
	FrameDeviceDomainModel model(7);
	const auto loss = model.NotifyDeviceLoss(7, EFrameDeviceFailureBoundary::Present, -1);
	EXPECT_TRUE(loss.Accepted());
	EXPECT_EQ(8u, model.DeviceEpoch());
	EXPECT_FALSE(model.SubmissionAllowed());

	EXPECT_EQ(EFrameDeviceTransitionStatus::Stale,
		model.NotifyDeviceLoss(7, EFrameDeviceFailureBoundary::ResizeBuffers, -2).status);
	EXPECT_EQ(EFrameDeviceTransitionStatus::InvalidState,
		model.NotifyDeviceLoss(8, EFrameDeviceFailureBoundary::CompositionCommit, -3).status);
	EXPECT_EQ(8u, model.DeviceEpoch());
	EXPECT_EQ(1u, model.Telemetry().lossDetections);
	EXPECT_EQ(1u, model.Telemetry().staleSignals);
	EXPECT_EQ(1u, model.Telemetry().duplicateLossSignals);
}

TEST(FrameDeviceDomainModel, HardwareFailureFallsBackToWarpWithoutIntermediateTerminal)
{
	FrameDeviceDomainModel model;
	ASSERT_TRUE(model.NotifyDeviceLoss(1, EFrameDeviceFailureBoundary::Present, -1).Accepted());
	ASSERT_TRUE(model.BeginQuiesce().Accepted());
	ASSERT_TRUE(model.BeginHardwareRecreation().Accepted());
	ASSERT_TRUE(model.CompleteHardwareRecreation(false).Accepted());
	EXPECT_EQ(EFrameDeviceState::CreatingWarp, model.State());
	ASSERT_TRUE(model.CompleteWarpCreation(true, 0).Accepted());
	EXPECT_EQ(EFrameDeviceState::WarpReady, model.State());
	EXPECT_TRUE(model.SubmissionAllowed());
	EXPECT_EQ(1u, model.Telemetry().warpCreationAttempts);
	EXPECT_EQ(1u, model.Telemetry().warpRecoveries);
}

TEST(FrameDeviceDomainModel, WarpFailureReachesCoherentSoftwareFallback)
{
	FrameDeviceDomainModel model;
	ASSERT_TRUE(model.NotifyDeviceLoss(1, EFrameDeviceFailureBoundary::ResizeBuffers, -1).Accepted());
	ASSERT_TRUE(model.BeginQuiesce().Accepted());
	ASSERT_TRUE(model.BeginHardwareRecreation().Accepted());
	ASSERT_TRUE(model.CompleteHardwareRecreation(false).Accepted());
	ASSERT_TRUE(model.CompleteWarpCreation(false, 0).Accepted());
	EXPECT_EQ(EFrameDeviceState::SoftwareOnly, model.State());
	EXPECT_FALSE(model.SubmissionAllowed());
	EXPECT_EQ(1u, model.Telemetry().softwareFallbacks);
}

TEST(FrameDeviceDomainModel, HardwareReprobeUsesBoundedExponentialBackoff)
{
	FrameDeviceDomainModel model;
	ASSERT_TRUE(model.NotifyDeviceLoss(1, EFrameDeviceFailureBoundary::Present, -1).Accepted());
	ASSERT_TRUE(model.BeginQuiesce().Accepted());
	ASSERT_TRUE(model.BeginHardwareRecreation().Accepted());
	ASSERT_TRUE(model.CompleteHardwareRecreation(false).Accepted());
	ASSERT_TRUE(model.CompleteWarpCreation(true, 10000).Accepted());
	EXPECT_EQ(11000u, model.NextHardwareProbeDeadlineMilliseconds());
	EXPECT_EQ(EFrameDeviceTransitionStatus::InvalidState, model.BeginHardwareProbe(10999).status);
	ASSERT_TRUE(model.BeginHardwareProbe(11000).Accepted());
	EXPECT_EQ(3u, model.DeviceEpoch());
	ASSERT_TRUE(model.CompleteHardwareProbe(false, 11000).Accepted());
	EXPECT_EQ(EFrameDeviceState::WarpReady, model.State());
	EXPECT_EQ(13000u, model.NextHardwareProbeDeadlineMilliseconds());
	ASSERT_TRUE(model.BeginHardwareProbe(13000).Accepted());
	ASSERT_TRUE(model.CompleteHardwareProbe(true, 13000).Accepted());
	EXPECT_EQ(EFrameDeviceState::HardwareReady, model.State());
	EXPECT_TRUE(model.SubmissionAllowed());
}

TEST(FrameDeviceDomainModel, CloseIsExplicitFromRecoveryAndRejectsLaterWork)
{
	FrameDeviceDomainModel model;
	ASSERT_TRUE(model.NotifyDeviceLoss(1, EFrameDeviceFailureBoundary::CompositionCommit, -1).Accepted());
	ASSERT_TRUE(model.BeginQuiesce().Accepted());
	ASSERT_TRUE(model.Close().Accepted());
	EXPECT_EQ(EFrameDeviceState::Closed, model.State());
	EXPECT_EQ(EFrameDeviceTransitionStatus::Closed, model.BeginHardwareRecreation().status);
	EXPECT_EQ(EFrameDeviceTransitionStatus::Closed, model.Close().status);
}

} // namespace
} // namespace workbench::rendering
