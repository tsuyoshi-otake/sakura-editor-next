/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameDeviceDomainModel.h"
#include "workbench/rendering/FramePresentationSurfaceRegistry.h"
#include "workbench/rendering/FrameRuntimeTelemetry.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace workbench::rendering {

inline constexpr std::uint32_t kFrameMaximumNativeSurfaceDimension = 16384;

// This class is an owner-thread-bound native boundary. The
// FrameCoordinatorRuntime presentation owner calls it; it does not create a
// second thread and never waits for a compositor or worker.
enum class EFramePresentationOwnerState : std::uint8_t {
	Created,
	HardwareReady,
	WarpReady,
	SoftwareOnly,
	Recovering,
	Closing,
	Closed,
	Failed,
};

enum class EFramePresentationOwnerStatus : std::uint8_t {
	Succeeded,
	AlreadyInitialized,
	NotReady,
	Backoff,
	SkippedBackpressure,
	DeviceLost,
	Invalid,
	WrongThread,
	Closed,
	Failed,
};

struct FramePresentationOwnerOptions final {
	// Legacy single-target input is retained for source compatibility. Native
	// resources are no longer created from this option: a target must be
	// explicitly registered per stable surface below. This prevents an empty
	// global swapchain from ever being attached over UI-owned pixels.
	HWND targetWindow = nullptr;
	std::uint32_t width = 1;
	std::uint32_t height = 1;
	std::uint32_t bufferCount = 2;
	bool allowHardware = true;
	bool allowWarp = true;
	bool allowHardwareReprobe = true;
	bool forceSoftware = false;
	bool presentDoNotWait = true;
	bool enableDebugLayer = false;
	// Fault injection is deterministic and owner-thread-only. It is useful for
	// proving Hardware -> WARP -> software terminal transitions without a GPU.
	bool failHardwareCreation = false;
	bool failWarpCreation = false;
	bool failCompositionCreation = false;
	std::size_t maximumSoftwareBytes = 64u * 1024u * 1024u;
	std::size_t maximumNativeSurfaceBytes = 64u * 1024u * 1024u;
	std::size_t maximumSurfaceCount = 256;
};

//! Native target registration for one stable logical surface.
//!
//! The HWND remains owned by the UI layer. The presentation owner only keeps
//! the target reference while the registration is live; it never destroys the
//! HWND, posts to it, or waits for its UI thread. x/y are offsets in the
//! target's client coordinate space and are applied to that surface's visual.
struct FrameNativeSurfaceRegistration final {
	FramePresentationSurfaceSpec presentation{};
	HWND targetWindow = nullptr;
	LONG x = 0;
	LONG y = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return presentation.surfaceId != 0
			&& presentation.surfaceLifetimeEpoch != 0
			&& presentation.deviceEpoch != 0
			&& presentation.layoutEpoch != 0
			&& presentation.width != 0
			&& presentation.height != 0
			&& presentation.width <= kFrameMaximumNativeSurfaceDimension
			&& presentation.height <= kFrameMaximumNativeSurfaceDimension;
	}
};

using FrameNativeSurfaceSpec = FrameNativeSurfaceRegistration;

//! Immutable CPU payload handed to the presentation-owner thread.
//!
//! The pixel vector is separately owned so a producer can build it on a
//! worker and transfer only a shared pointer through the bounded runtime
//! mailbox. The UI path therefore does not copy a frame or call a GPU API.
//! Pixels are BGRA premultiplied.  The default payload is a full-surface
//! buffer described by width/height/pitch; an empty dirtyRect means that
//! complete surface.  A producer that only changed a rectangle may set
//! compactDirtyPayload: its vector then starts at dirtyRect.left/top, has
//! dirtyRect.height rows, and pitch is the source row pitch (normally
//! dirtyRect.width * 4).  The owner still uploads the same destination box,
//! but never requires a full-surface CPU allocation for that path.
struct FrameNativeSurfaceFrame final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t displayEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t pitch = 0;
	RECT dirtyRect{};
	bool compactDirtyPayload = false;
	std::shared_ptr<const std::vector<std::uint8_t>> pixels;

	[[nodiscard]] bool IsValid() const noexcept
	{
		if (surfaceId == 0 || surfaceLifetimeEpoch == 0 || deviceEpoch == 0
			|| displayEpoch == 0 || layoutEpoch == 0 || requestId == 0
			|| width == 0 || height == 0
			|| width > kFrameMaximumNativeSurfaceDimension
			|| height > kFrameMaximumNativeSurfaceDimension
			|| width > (std::numeric_limits<std::uint32_t>::max)() / 4u
			|| !pixels) {
			return false;
		}
		const RECT full{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
		const bool fullSurface = dirtyRect.left == 0 && dirtyRect.top == 0
			&& dirtyRect.right == 0 && dirtyRect.bottom == 0;
		if (!fullSurface && (dirtyRect.left < full.left || dirtyRect.top < full.top
			|| dirtyRect.right > full.right || dirtyRect.bottom > full.bottom
			|| dirtyRect.left >= dirtyRect.right || dirtyRect.top >= dirtyRect.bottom)) {
			return false;
		}
		if (fullSurface && compactDirtyPayload) return false;
		const auto payloadWidth = fullSurface
			? width : static_cast<std::uint32_t>(dirtyRect.right - dirtyRect.left);
		const auto payloadHeight = fullSurface
			? height : static_cast<std::uint32_t>(dirtyRect.bottom - dirtyRect.top);
		if (payloadWidth > (std::numeric_limits<std::uint32_t>::max)() / 4u
			|| pitch == 0 || pitch < payloadWidth * 4u
			|| payloadHeight > (std::numeric_limits<std::size_t>::max)() / pitch) {
			return false;
		}
		const auto requiredBytes = static_cast<std::size_t>(pitch) * payloadHeight;
		return requiredBytes <= pixels->size();
	}

	[[nodiscard]] std::size_t PayloadBytes() const noexcept
	{
		const bool compact = compactDirtyPayload
			&& dirtyRect.right > dirtyRect.left && dirtyRect.bottom > dirtyRect.top;
		const auto rows = compact
			? static_cast<std::size_t>(dirtyRect.bottom - dirtyRect.top)
			: static_cast<std::size_t>(height);
		if (pitch == 0 || rows > (std::numeric_limits<std::size_t>::max)() / pitch) {
			return 0;
		}
		return static_cast<std::size_t>(pitch) * rows;
	}
};

struct FrameSoftwarePublication final {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t pitch = 0;
	std::uint64_t deviceEpoch = 0;
	std::vector<std::uint8_t> pixels;
};

struct FramePresentationOwnerTelemetry final {
	std::uint64_t hardwareCreationAttempts = 0;
	std::uint64_t hardwareCreationFailures = 0;
	std::uint64_t warpCreationAttempts = 0;
	std::uint64_t warpCreationFailures = 0;
	std::uint64_t presentCalls = 0;
	std::uint64_t presentSuccesses = 0;
	std::uint64_t presentBackpressureSkips = 0;
	std::uint64_t presentNotReadySkips = 0;
	std::uint64_t presentDeviceLosses = 0;
	std::uint64_t presentFailures = 0;
	std::int64_t lastPresentResult = 0;
	std::uint64_t lastPresentDurationMicroseconds = 0;
	std::uint64_t maximumPresentDurationMicroseconds = 0;
	std::uint64_t resizeCalls = 0;
	std::uint64_t compositionCommits = 0;
	std::uint64_t softwarePublications = 0;
	std::uint64_t nativeUnavailableFallbacks = 0;
	std::uint64_t nativeSurfaceRegistrations = 0;
	std::uint64_t nativeSurfaceResourceCreations = 0;
	std::uint64_t nativeSurfaceResourceFailures = 0;
	std::uint64_t nativeSurfaceUploadCalls = 0;
	std::uint64_t nativeSurfaceUploadBytes = 0;
	std::uint64_t nativeSurfaceDirtyRectUpdates = 0;
	std::uint64_t readbackObservations = 0;
	std::uint64_t readbackCompletions = 0;
	std::uint64_t readbackBytes = 0;
	std::uint64_t lastReadbackRequestId = 0;
	std::uint64_t deviceLossRecoveries = 0;
	std::uint64_t failedTransitions = 0;
};

struct FramePresentationOwnerResult final {
	EFramePresentationOwnerStatus status = EFramePresentationOwnerStatus::Invalid;
	EFramePresentationOwnerState state = EFramePresentationOwnerState::Failed;
	HRESULT hresult = S_OK;
	std::uint64_t deviceEpoch = 0;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFramePresentationOwnerStatus::Succeeded
			|| status == EFramePresentationOwnerStatus::AlreadyInitialized;
	}
};

//! Presentation-owner boundary for D3D11, DXGI, DirectComposition, and the
//! explicit software fallback.
//!
//! The native chain owns one resource set per explicitly registered stable
//! surface. SurfaceRegistry remains the logical epoch/readiness boundary;
//! callers must keep surface ids, readiness, and backpressure separate rather
//! than combining them into a giant framebuffer. A null target registration is
//! intentionally logical-only and therefore remains on the GDI fallback path.
//!
//! All methods that mutate state must be called on one owner thread. There is
//! no mutex or callback around BeginDraw/EndDraw, ResizeBuffers, Present1, or
//! DirectComposition Commit. Software publication is a depth-one immutable
//! mailbox; consumers pull it and are never invoked by the owner.
class FramePresentationOwner final {
public:
	explicit FramePresentationOwner(FramePresentationOwnerOptions options = {});
	~FramePresentationOwner() noexcept;

	FramePresentationOwner(const FramePresentationOwner&) = delete;
	FramePresentationOwner& operator=(const FramePresentationOwner&) = delete;
	FramePresentationOwner(FramePresentationOwner&&) = delete;
	FramePresentationOwner& operator=(FramePresentationOwner&&) = delete;

	//! Claims the current thread and creates the hardware/fallback domain.
	[[nodiscard]] FramePresentationOwnerResult Initialize() noexcept;
	[[nodiscard]] FramePresentationOwnerResult Resize(
		std::uint32_t width, std::uint32_t height) noexcept;
	//! Registers one logical surface and, when a target HWND and GPU domain are
	//! available, creates that surface's native resources on the owner thread.
	[[nodiscard]] FramePresentationOwnerResult RegisterNativeSurface(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	//! Updates target/size/position for an existing lifetime. Resource resize and
	//! visual attachment are owner-thread operations; a failed native update
	//! leaves the logical surface available for the GDI fallback.
	[[nodiscard]] FramePresentationOwnerResult UpdateNativeSurface(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	//! Closes one surface's native resource set without destroying its HWND.
	[[nodiscard]] FramePresentationOwnerResult CloseNativeSurface(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch) noexcept;
	//! Presents one already prepared swapchain frame. No waitable-object wait is
	//! performed; DO_NOT_WAIT reports WAS_STILL_DRAWING as backpressure.
	[[nodiscard]] FramePresentationOwnerResult Present() noexcept;
	//! Uploads and presents one immutable surface payload. All DXGI/D3D/DComp
	//! calls happen on the owner thread; the caller only transfers ownership of
	//! the immutable payload through FrameCoordinatorRuntime.
	[[nodiscard]] FramePresentationOwnerResult PresentNativeSurface(
		const FrameNativeSurfaceFrame& frame) noexcept;
	//! Records a completed asynchronous readback observation. The owner never
	//! maps a staging resource or waits for the GPU here.
	[[nodiscard]] FramePresentationOwnerResult ObserveReadback(
		const FrameReadbackObservation& observation) noexcept;
	//! Commits the DirectComposition visual tree when an HWND target exists.
	[[nodiscard]] FramePresentationOwnerResult CommitComposition() noexcept;
	//! Publishes a bounded copy for the coherent software terminal path.
	[[nodiscard]] FramePresentationOwnerResult PublishSoftwareFrame(
		std::uint32_t width, std::uint32_t height, std::uint32_t pitch,
		std::span<const std::uint8_t> pixels) noexcept;
	//! Performs only explicit, caller-driven hardware reprobe work. There is no
	//! fixed timer, compositor wait, DwmFlush, or WaitForCommitCompletion.
	[[nodiscard]] FramePresentationOwnerResult Tick(
		std::uint64_t nowMilliseconds) noexcept;
	//! Deterministic device-loss boundary used by tests and fault injection.
	[[nodiscard]] FramePresentationOwnerResult InjectDeviceLoss(
		EFrameDeviceFailureBoundary boundary, long failureCode) noexcept;

	//! Pulls the latest software publication without invoking consumer code on
	//! the owner. At most one immutable publication is retained.
	[[nodiscard]] std::shared_ptr<const FrameSoftwarePublication>
		TakeSoftwarePublication() noexcept;
	[[nodiscard]] FramePresentationOwnerResult BeginClose() noexcept;
	[[nodiscard]] FramePresentationOwnerResult Close() noexcept;

	[[nodiscard]] EFramePresentationOwnerState State() const noexcept { return m_state; }
	[[nodiscard]] EFrameDeviceState DeviceState() const noexcept { return m_deviceDomain.State(); }
	[[nodiscard]] std::uint64_t DeviceEpoch() const noexcept { return m_deviceDomain.DeviceEpoch(); }
	[[nodiscard]] bool SubmissionAllowed() const noexcept
	{
		return (m_state == EFramePresentationOwnerState::HardwareReady
			|| m_state == EFramePresentationOwnerState::WarpReady)
			&& m_deviceDomain.SubmissionAllowed();
	}
	[[nodiscard]] HRESULT LastHresult() const noexcept { return m_lastHresult; }
	[[nodiscard]] const FramePresentationOwnerTelemetry& Telemetry() const noexcept
	{
		return m_telemetry;
	}
	[[nodiscard]] const FrameDeviceDomainTelemetry& DeviceTelemetry() const noexcept
	{
		return m_deviceDomain.Telemetry();
	}
	[[nodiscard]] bool HasD2DResources() const noexcept;
	//! True only when a DirectComposition target is attached to the HWND. A
	//! device/swap chain without a target is an explicit GDI/software bridge,
	//! not a visible native presentation path.
	[[nodiscard]] bool NativePresentationAvailable() const noexcept
	{
		return m_nativePresentationAvailable;
	}
	[[nodiscard]] bool NativeSurfaceResourceAvailable(
		FrameSurfaceId surfaceId) const noexcept;
	[[nodiscard]] std::size_t NativeSurfaceCount() const noexcept
	{
		return m_nativeSurfaceRegistrations.size();
	}
	[[nodiscard]] const FrameSoftwarePublication& LastSoftwarePublication() const noexcept
	{
		return m_lastSoftwarePublication;
	}
	[[nodiscard]] FramePresentationSurfaceRegistry& SurfaceRegistry() noexcept
	{
		return m_surfaceRegistry;
	}
	[[nodiscard]] const FramePresentationSurfaceRegistry& SurfaceRegistry() const noexcept
	{
		return m_surfaceRegistry;
	}

private:
	struct GpuResources;
	struct NativeTargetResources;
	struct NativeSurfaceResources;

	[[nodiscard]] bool IsOwnerThread() const noexcept;
	[[nodiscard]] bool ClaimOwnerThread() noexcept;
	[[nodiscard]] FramePresentationOwnerResult Result(
		EFramePresentationOwnerStatus status, HRESULT hresult = S_OK) const noexcept;
	[[nodiscard]] bool CreateHardwareResources(GpuResources& resources) noexcept;
	[[nodiscard]] bool CreateWarpResources(GpuResources& resources) noexcept;
	[[nodiscard]] bool CreateGpuResources(
		GpuResources& resources, bool warp, HRESULT& failure) noexcept;
	[[nodiscard]] bool CreateCompositionResources(
		GpuResources& resources, HRESULT& failure) noexcept;
	[[nodiscard]] bool CreateNativeSurfaceResources(
		const FrameNativeSurfaceRegistration& registration,
		NativeSurfaceResources& resources, HRESULT& failure) noexcept;
	[[nodiscard]] bool EnsureNativeSurfaceResources(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	void RecreateNativeSurfaceResources() noexcept;
	void ReleaseNativeSurfaceVisual(NativeSurfaceResources& resources) noexcept;
	void ReleaseNativeSurfaceResources() noexcept;
	void RefreshNativePresentationAvailability() noexcept;
	[[nodiscard]] bool CreateD2DResources(
		GpuResources& resources, HRESULT& failure) noexcept;
	[[nodiscard]] bool AdoptGpuResources(GpuResources&& resources) noexcept;
	[[nodiscard]] bool IsDeviceLoss(HRESULT failure) const noexcept;
	[[nodiscard]] FramePresentationOwnerResult RecoverFromDeviceLoss(
		EFrameDeviceFailureBoundary boundary, long failureCode,
		std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] FramePresentationOwnerResult ProbeHardware(
		std::uint64_t nowMilliseconds) noexcept;
	void ReleaseGpuResources() noexcept;
	void SetFallbackState() noexcept;
	void SetFailed(HRESULT failure) noexcept;

	FramePresentationOwnerOptions m_options;
	std::unique_ptr<GpuResources> m_gpu;
	FrameDeviceDomainModel m_deviceDomain;
	FramePresentationSurfaceRegistry m_surfaceRegistry;
	EFramePresentationOwnerState m_state = EFramePresentationOwnerState::Created;
	std::thread::id m_ownerThreadId{};
	std::uint32_t m_width = 1;
	std::uint32_t m_height = 1;
	bool m_nativePresentationAvailable = false;
	HRESULT m_lastHresult = S_OK;
	FramePresentationOwnerTelemetry m_telemetry;
	// C++20's atomic shared_ptr specialization keeps the depth-one software
	// publication mailbox lock-free at the ownership boundary without the
	// deprecated free-function atomic_load/store/exchange overloads.
	std::atomic<std::shared_ptr<const FrameSoftwarePublication>> m_softwareMailbox;
	FrameSoftwarePublication m_lastSoftwarePublication;
	std::unordered_map<FrameSurfaceId, FrameNativeSurfaceRegistration>
		m_nativeSurfaceRegistrations;
	std::unordered_map<FrameSurfaceId, std::unique_ptr<NativeSurfaceResources>>
		m_nativeSurfaces;
	std::unordered_map<HWND, std::unique_ptr<NativeTargetResources>>
		m_nativeTargets;
};

} // namespace workbench::rendering
