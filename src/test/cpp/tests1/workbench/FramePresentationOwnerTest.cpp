/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FramePresentationOwner.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace workbench::rendering {
namespace {

TEST(FramePresentationOwner, ForcedSoftwarePublishesBoundedImmutableFrame)
{
	FramePresentationOwnerOptions options;
	options.forceSoftware = true;
	options.allowHardware = false;
	options.allowWarp = false;
	options.maximumSoftwareBytes = 64;
	FramePresentationOwner owner(options);

	ASSERT_TRUE(owner.Initialize().Accepted());
	EXPECT_EQ(EFramePresentationOwnerState::SoftwareOnly, owner.State());
	EXPECT_EQ(EFrameDeviceState::SoftwareOnly, owner.DeviceState());
	EXPECT_FALSE(owner.SubmissionAllowed());
	EXPECT_EQ(2u, owner.DeviceEpoch());
	EXPECT_FALSE(owner.HasD2DResources());

	const std::array<std::uint8_t, 16> pixels{
		0, 1, 2, 3, 4, 5, 6, 7,
		8, 9, 10, 11, 12, 13, 14, 15,
	};
	const auto publish = owner.PublishSoftwareFrame(2, 2, 8, pixels);
	EXPECT_EQ(EFramePresentationOwnerStatus::Succeeded, publish.status);
	const auto publication = owner.TakeSoftwarePublication();
	ASSERT_TRUE(publication);
	EXPECT_EQ(2u, publication->width);
	EXPECT_EQ(2u, publication->height);
	EXPECT_EQ(8u, publication->pitch);
	EXPECT_EQ(16u, publication->pixels.size());
	EXPECT_EQ(pixels.size(), owner.LastSoftwarePublication().pixels.size());

	EXPECT_EQ(EFramePresentationOwnerStatus::Invalid,
		owner.PublishSoftwareFrame(0, 2, 8, pixels).status);
	EXPECT_EQ(EFramePresentationOwnerStatus::Invalid,
		owner.PublishSoftwareFrame(2, 2, 8, std::span<const std::uint8_t>{pixels.data(), 8}).status);
	EXPECT_EQ(EFramePresentationOwnerStatus::NotReady, owner.Present().status);
	EXPECT_EQ(EFramePresentationOwnerStatus::NotReady, owner.CommitComposition().status);

	EXPECT_TRUE(owner.Close().Accepted());
	EXPECT_EQ(EFramePresentationOwnerState::Closed, owner.State());
	EXPECT_EQ(EFramePresentationOwnerStatus::Closed, owner.Close().status);
}

TEST(FramePresentationOwner, HardwareAndWarpFaultsReachSoftwareTerminal)
{
	FramePresentationOwnerOptions options;
	options.failHardwareCreation = true;
	options.failWarpCreation = true;
	FramePresentationOwner owner(options);

	const auto initialized = owner.Initialize();
	ASSERT_TRUE(initialized.Accepted());
	EXPECT_EQ(EFramePresentationOwnerState::SoftwareOnly, owner.State());
	EXPECT_EQ(EFrameDeviceState::SoftwareOnly, owner.DeviceState());
	EXPECT_FALSE(owner.SubmissionAllowed());
	EXPECT_GE(owner.Telemetry().hardwareCreationFailures, 2u);
	EXPECT_GE(owner.Telemetry().warpCreationFailures, 1u);
	EXPECT_EQ(1u, owner.DeviceTelemetry().lossDetections);
	EXPECT_EQ(1u, owner.DeviceTelemetry().softwareFallbacks);

	EXPECT_EQ(EFramePresentationOwnerStatus::Backoff, owner.Tick(999).status);
	const auto reprobe = owner.Tick(1000);
	EXPECT_EQ(EFramePresentationOwnerStatus::Succeeded, reprobe.status);
	EXPECT_EQ(EFramePresentationOwnerState::SoftwareOnly, owner.State());
	EXPECT_GE(owner.DeviceTelemetry().hardwareProbeAttempts, 1u);
	EXPECT_EQ(EFramePresentationOwnerStatus::Backoff, owner.Tick(1001).status);

	EXPECT_TRUE(owner.Close().Accepted());
}

TEST(FramePresentationOwner, OwnerThreadIsStableAndCloseIsTerminal)
{
	FramePresentationOwnerOptions options;
	options.forceSoftware = true;
	options.allowHardware = false;
	options.allowWarp = false;
	FramePresentationOwner owner(options);
	ASSERT_TRUE(owner.Initialize().Accepted());

	EFramePresentationOwnerStatus workerStatus = EFramePresentationOwnerStatus::Succeeded;
	std::thread worker([&] {
		workerStatus = owner.Tick(0).status;
	});
	worker.join();
	EXPECT_EQ(EFramePresentationOwnerStatus::WrongThread, workerStatus);

	EXPECT_TRUE(owner.BeginClose().Accepted());
	EXPECT_EQ(EFramePresentationOwnerState::Closed, owner.State());
	const std::array<std::uint8_t, 4> pixels{};
	EXPECT_EQ(EFramePresentationOwnerStatus::Closed, owner.PublishSoftwareFrame(
		1, 1, 4, pixels).status);
}

TEST(FramePresentationOwner, NativeSurfaceUploadUsesExplicitTargetAndDirtyRect)
{
	// A native target is intentionally opt-in. The owner must not create a
	// global composition swapchain before a stable surface has supplied its HWND.
	FramePresentationOwnerOptions options;
	FramePresentationOwner owner(options);
	ASSERT_TRUE(owner.Initialize().Accepted());
	EXPECT_FALSE(owner.NativePresentationAvailable());

	const auto instance = ::GetModuleHandleW(nullptr);
	static constexpr wchar_t kClassName[] = L"SakuraFrameNativeSurfaceTest";
	static std::once_flag registerOnce;
	std::call_once(registerOnce, [&] {
		WNDCLASSW windowClass{};
		windowClass.hInstance = instance;
		windowClass.lpfnWndProc = &::DefWindowProcW;
		windowClass.lpszClassName = kClassName;
		(void)::RegisterClassW(&windowClass);
	});
	const HWND target = ::CreateWindowExW(
		WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
		-32000, -32000, 2, 2, nullptr, nullptr, instance, nullptr);
	if (target == nullptr) {
		GTEST_SKIP() << "hidden test HWND could not be created";
	}
	::ShowWindow(target, SW_SHOWNOACTIVATE);

	const auto registration = FrameNativeSurfaceRegistration{
		.presentation = FramePresentationSurfaceSpec{
			.surfaceId = 701,
			.surfaceLifetimeEpoch = 1,
			.deviceEpoch = owner.DeviceEpoch(),
			.layoutEpoch = 1,
			.width = 2,
			.height = 2,
			.visible = true,
		},
		.targetWindow = target,
		.x = 0,
		.y = 0,
	};
	ASSERT_TRUE(owner.RegisterNativeSurface(registration).Accepted());
	if (!owner.NativeSurfaceResourceAvailable(701)) {
		::DestroyWindow(target);
		GTEST_SKIP() << "D3D/DComp native surface unavailable on this machine";
	}
	const auto secondRegistration = FrameNativeSurfaceRegistration{
		.presentation = FramePresentationSurfaceSpec{
			.surfaceId = 702,
			.surfaceLifetimeEpoch = 1,
			.deviceEpoch = owner.DeviceEpoch(),
			.layoutEpoch = 1,
			.width = 2,
			.height = 2,
			.visible = true,
		},
		.targetWindow = target,
		.x = 1,
		.y = 1,
	};
	ASSERT_TRUE(owner.RegisterNativeSurface(secondRegistration).Accepted());
	EXPECT_TRUE(owner.NativeSurfaceResourceAvailable(702));

	const auto pixels = std::make_shared<const std::vector<std::uint8_t>>(
		std::vector<std::uint8_t>{
			0, 0, 255, 255, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0,
		});
	const auto frame = FrameNativeSurfaceFrame{
		.surfaceId = 701,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = owner.DeviceEpoch(),
		.displayEpoch = 1,
		.layoutEpoch = 1,
		.requestId = 1,
		.width = 2,
		.height = 2,
		.pitch = 8,
		.dirtyRect = RECT{ 0, 0, 1, 1 },
		.pixels = pixels,
	};
	const auto present = owner.PresentNativeSurface(frame);
	EXPECT_TRUE(present.status == EFramePresentationOwnerStatus::Succeeded
		|| present.status == EFramePresentationOwnerStatus::SkippedBackpressure
		|| present.status == EFramePresentationOwnerStatus::DeviceLost);
	EXPECT_EQ(1u, owner.Telemetry().nativeSurfaceUploadCalls);
	EXPECT_GE(owner.Telemetry().presentCalls, 1u);
	EXPECT_EQ(1u, owner.Telemetry().nativeSurfaceDirtyRectUpdates);

	EXPECT_TRUE(owner.CloseNativeSurface(702, 1).Accepted());
	EXPECT_TRUE(owner.CloseNativeSurface(701, 1).Accepted());
	::DestroyWindow(target);
	EXPECT_TRUE(owner.Close().Accepted());
}

TEST(FramePresentationSurfaceRegistry, KeepsIslandsAndLifetimesIndependent)
{
	FramePresentationSurfaceRegistry registry(2);
	const FramePresentationSurfaceSpec first{
		.surfaceId = 1,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 1,
		.width = 100,
		.height = 80,
		.visible = true,
	};
	const FramePresentationSurfaceSpec second{
		.surfaceId = 2,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 1,
		.width = 50,
		.height = 40,
		.visible = true,
	};
	ASSERT_TRUE(registry.Register(first).Accepted());
	ASSERT_TRUE(registry.Register(second).Accepted());
	EXPECT_EQ(EFramePresentationSurfaceStatus::Full, registry.Register({
		.surfaceId = 3,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 1,
		.width = 10,
		.height = 10,
	}).status);

	ASSERT_TRUE(registry.MarkBackpressure(1, 1).Accepted());
	EXPECT_EQ(EFramePresentationSurfaceState::Backpressured,
		registry.Snapshot(1)->state);
	EXPECT_EQ(EFramePresentationSurfaceState::Ready,
		registry.Snapshot(2)->state);
	ASSERT_TRUE(registry.MarkPresented(1, 1, 1, 7).Accepted());
	EXPECT_TRUE(registry.Snapshot(1)->hasLastGoodContent);
	EXPECT_EQ(EFramePresentationSurfaceStatus::Stale,
		registry.MarkPresented(1, 1, 1, 6).status);

	ASSERT_TRUE(registry.Close(1, 1).Accepted());
	EXPECT_EQ(EFramePresentationSurfaceStatus::Closed, registry.Close(1, 1).status);
	ASSERT_TRUE(registry.Register({
		.surfaceId = 1,
		.surfaceLifetimeEpoch = 2,
		.deviceEpoch = 2,
		.width = 120,
		.height = 90,
	}).Accepted());
	EXPECT_EQ(EFramePresentationSurfaceStatus::Stale,
		registry.MarkPresented(1, 1, 1, 8).status);
	EXPECT_EQ(EFramePresentationSurfaceState::Ready,
		registry.Snapshot(1)->state);
}

TEST(FramePresentationSurfaceRegistry, ReprojectsDeviceWithoutDiscardingLastGoodPixels)
{
	FramePresentationSurfaceRegistry registry;
	ASSERT_TRUE(registry.Register({
		.surfaceId = 11,
		.surfaceLifetimeEpoch = 4,
		.deviceEpoch = 7,
		.layoutEpoch = 3,
		.width = 640,
		.height = 480,
		.visible = true,
	}).Accepted());
	ASSERT_TRUE(registry.MarkGdiFallback(11, 4, 7, 5, 9, true).Accepted());

	ASSERT_TRUE(registry.ReprojectDevice(11, 4, 8, false).Accepted());
	const auto gdi = registry.Snapshot(11);
	ASSERT_TRUE(gdi.has_value());
	EXPECT_EQ(8u, gdi->deviceEpoch);
	EXPECT_EQ(EFramePresentationSurfaceState::GdiFallback, gdi->state);
	EXPECT_EQ(5u, gdi->layoutEpoch);
	EXPECT_EQ(9u, gdi->lastPresentedRequestId);
	EXPECT_TRUE(gdi->hasLastGoodContent);
	EXPECT_EQ(EFramePresentationSurfaceStatus::Stale,
		registry.ReprojectDevice(11, 4, 8, false).status);
	EXPECT_EQ(EFramePresentationSurfaceStatus::Stale,
		registry.MarkGdiFallback(11, 4, 7, 6, 10, true).status);

	ASSERT_TRUE(registry.Register({
		.surfaceId = 12,
		.surfaceLifetimeEpoch = 1,
		.deviceEpoch = 7,
		.layoutEpoch = 1,
		.width = 320,
		.height = 200,
	}).Accepted());
	ASSERT_TRUE(registry.MarkBackpressure(12, 1).Accepted());
	ASSERT_TRUE(registry.ReprojectDevice(12, 1, 8, true).Accepted());
	EXPECT_EQ(EFramePresentationSurfaceState::SoftwareOnly,
		registry.Snapshot(12)->state);
}

} // namespace
} // namespace workbench::rendering
