#pragma once

#include "FrameBackpressure.h"
#include "FrameCadence.h"
#include "FrameCoordinatorModel.h"
#include "FramePresentationOwner.h"
#include "FrameRuntimeTelemetry.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace workbench::rendering {

// Runtime boundary for the pure FrameCoordinatorModel.  The runtime owns one
// presentation thread; callers only enqueue bounded commands and wake events.
enum class EFrameCoordinatorRuntimeState : std::uint8_t {
	Created,
	Running,
	Closing,
	Stopped,
	Failed,
};

enum class EFrameCoordinatorRuntimeStatus : std::uint8_t {
	Succeeded,
	Replaced,
	QueueFull,
	Invalid,
	Stale,
	Busy,
	Closed,
	NotStarted,
	SelfWait,
	Failed,
};

struct FrameCoordinatorRuntimeResult {
	EFrameCoordinatorRuntimeStatus status = EFrameCoordinatorRuntimeStatus::Invalid;
	EFrameCoordinatorRuntimeState state = EFrameCoordinatorRuntimeState::Failed;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameCoordinatorRuntimeStatus::Succeeded ||
			status == EFrameCoordinatorRuntimeStatus::Replaced;
	}
};

struct FrameCoordinatorRuntimeOptions {
	// This bounds Register/Close/Request/Reset commands.  Request commands are
	// replaced in place for the same surface, so the queue is latest-only per
	// surface while remaining globally bounded.
	std::size_t maxControlQueueDepth = 64;
	// CPU work is pulled by a bounded worker pool.  The owner never invokes
	// consumer code, so one slow surface cannot stall scheduling or teardown.
	std::size_t maxCpuWorkQueueDepth = 64;
	// Software publication is deliberately bounded per explicit tick.  No
	// periodic timer is created by this runtime.
	std::size_t maxPublicationsPerTick = 64;
	// Native presentation resources are created and destroyed on the same owner
	// thread as the coordinator. A null target is a valid GDI-fallback bridge;
	// it never creates or attaches an empty DirectComposition swapchain.
	FramePresentationOwnerOptions presentationOwner{};
	// Cadence is supplied by the event/compositor source. The runtime records
	// the selected interval but never creates a timer or waits for it.
	FrameCadenceInput cadence{};
	// Owner-path admission limits. Interactive work is promoted to the
	// reserved lane when it reaches the publication boundary.
	FrameBackpressureOptions backpressure{};
};

struct FrameGdiSurfaceCommit final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	bool visible = false;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return surfaceId != 0 && surfaceLifetimeEpoch != 0 && deviceEpoch != 0
			&& layoutEpoch != 0 && requestId != 0 && width != 0 && height != 0;
	}
};

//! Explicit owner-thread native presentation request.
//!
//! CPU publication is a pull mailbox and does not imply that a swap chain is
//! ready. A native adapter calls PresentSurface after it has consumed and
//! rendered the immutable cohort; the owner then performs one nonblocking
//! Present1(DXGI_PRESENT_DO_NOT_WAIT) attempt for this surface.
struct FrameNativePresentationRequest final {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t displayEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t requestId = 0;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return surfaceId != 0 && surfaceLifetimeEpoch != 0
			&& deviceEpoch != 0 && displayEpoch != 0
			&& layoutEpoch != 0 && requestId != 0;
	}
};

struct FrameCoordinatorRuntimeSnapshot {
	EFrameCoordinatorRuntimeState state = EFrameCoordinatorRuntimeState::Failed;
	bool closeRequested = false;
	bool ownerThreadRunning = false;
	std::size_t controlQueueDepth = 0;
	std::size_t cpuWorkQueueDepth = 0;
	bool publicationPending = false;
	bool tickPending = false;
	std::uint64_t requestedTickCount = 0;
	std::uint64_t processedTickCount = 0;
	std::uint64_t replacedPublications = 0;
	std::uint64_t processedDeviceFailures = 0;
	EFramePresentationOwnerState presentationState = EFramePresentationOwnerState::Created;
	std::uint64_t presentationDeviceEpoch = 0;
};

class FrameCoordinatorRuntime final {
public:
	explicit FrameCoordinatorRuntime(FrameCoordinatorRuntimeOptions options = {});
	~FrameCoordinatorRuntime();

	FrameCoordinatorRuntime(const FrameCoordinatorRuntime&) = delete;
	FrameCoordinatorRuntime& operator=(const FrameCoordinatorRuntime&) = delete;
	FrameCoordinatorRuntime(FrameCoordinatorRuntime&&) = delete;
	FrameCoordinatorRuntime& operator=(FrameCoordinatorRuntime&&) = delete;

	// These methods never wait for the owner thread.  A command is either
	// accepted, replaces the pending request for its surface, or is rejected
	// because the bounded queue is full/closed.
	[[nodiscard]] FrameCoordinatorRuntimeResult RegisterSurface(
		FrameSurfaceId surfaceId,
		std::uint64_t surfaceLifetimeEpoch) noexcept;
	//! Atomically registers the coordinator identity and owner-thread surface.
	[[nodiscard]] FrameCoordinatorRuntimeResult RegisterPresentedSurface(
		const FramePresentationSurfaceSpec& spec) noexcept;
	//! Registers an explicit per-surface HWND/size/position target. Native
	//! resources are created only by the presentation-owner thread.
	[[nodiscard]] FrameCoordinatorRuntimeResult RegisterPresentedSurface(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult UpdatePresentedSurface(
		const FrameNativeSurfaceRegistration& registration) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult CloseSurface(
		FrameSurfaceId surfaceId,
		std::uint64_t surfaceLifetimeEpoch) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult Request(const FrameSurfaceRequest& request) noexcept;
	// Called by a CPU-only worker after its immutable plan is ready. The worker
	// never touches FrameCoordinatorModel; the owner consumes this bounded
	// completion command and performs CompleteCpu/GPU state transitions.
	[[nodiscard]] FrameCoordinatorRuntimeResult SubmitCpuCompletion(const FrameWorkTicket& ticket) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult ResetDevice(std::uint64_t deviceEpoch) noexcept;
	//! Enqueues one deterministic device-domain failure at its real owner-thread
	//! boundary. Recovery and surface reprojection never execute on the caller.
	[[nodiscard]] FrameCoordinatorRuntimeResult InjectPresentationFailure(
		EFrameDeviceFailureBoundary boundary, long failureCode) noexcept;
	//! Latest-only notification that UI-owned GDI pixels reached their commit
	//! boundary. No pixels are copied and no compositor wait occurs.
	[[nodiscard]] FrameCoordinatorRuntimeResult RecordGdiFallback(
		const FrameGdiSurfaceCommit& commit) noexcept;
	//! Enqueues one explicit nonblocking native Present attempt. This is
	//! intentionally separate from TakePublication(): a CPU mailbox replacement
	//! never consumes or fabricates swap-chain readiness.
	[[nodiscard]] FrameCoordinatorRuntimeResult PresentSurface(
		const FrameNativePresentationRequest& request) noexcept;
	//! Transfers an immutable BGRA payload through a depth-bounded, per-surface
	//! latest-wins mailbox. The runtime copies only the shared pointer and never
	//! touches the GPU or waits on the caller's thread.
	[[nodiscard]] FrameCoordinatorRuntimeResult SubmitNativeSurfaceFrame(
		std::shared_ptr<const FrameNativeSurfaceFrame> frame) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult SubmitNativeSurfaceFrame(
		FrameNativeSurfaceFrame frame) noexcept;
	//! Records an adapter-provided asynchronous readback observation. The
	//! runtime never performs a synchronous GPU readback.
	[[nodiscard]] FrameCoordinatorRuntimeResult RecordReadbackObservation(
		const FrameReadbackObservation& observation) noexcept;
	//! Publishes a new display/compositor cadence epoch. This only updates the
	//! event-source observation and wakes the owner; it never starts a timer or
	//! waits for a compositor tick.
	[[nodiscard]] FrameCoordinatorRuntimeResult UpdateCadence(
		const FrameCadenceInput& cadence) noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult NotifyDisplayCadence(
		const FrameCadenceInput& cadence) noexcept
	{
		return UpdateCadence(cadence);
	}

	// Explicit wake only.  Multiple wakes before the owner observes them are
	// coalesced; there is no fixed-period timer in this runtime.
	[[nodiscard]] FrameCoordinatorRuntimeResult Tick() noexcept;

	// Both boundaries are pull mailboxes.  No producer/consumer callback ever
	// executes on the presentation owner thread.
	[[nodiscard]] std::optional<FrameWorkTicket> TakeCpuWork() noexcept;
	[[nodiscard]] std::shared_ptr<const FrameCommitCohort> TakePublication() noexcept;

	// BeginClose only changes the close state and wakes the owner.  It never
	// joins.  Call Wait from an external owner/finalizer to join deterministically.
	[[nodiscard]] FrameCoordinatorRuntimeResult BeginClose() noexcept;
	[[nodiscard]] FrameCoordinatorRuntimeResult Wait() noexcept;

	[[nodiscard]] FrameCoordinatorRuntimeSnapshot Snapshot() const noexcept;
	[[nodiscard]] std::optional<FrameSurfaceSnapshot> SurfaceSnapshot(FrameSurfaceId surfaceId) const noexcept;
	[[nodiscard]] std::optional<FramePresentationSurfaceSnapshot> PresentationSurfaceSnapshot(
		FrameSurfaceId surfaceId) const noexcept;
	[[nodiscard]] FramePresentationOwnerTelemetry PresentationTelemetry() const noexcept;
	[[nodiscard]] FrameDeviceDomainTelemetry PresentationDeviceTelemetry() const noexcept;
	[[nodiscard]] FrameCoordinatorTelemetry Telemetry() const noexcept;
	//! Fixed-size, lock-free runtime observations for C8 performance gates.
	[[nodiscard]] FrameRuntimeTelemetrySnapshot RuntimeTelemetry() const noexcept;
	//! Selected event cadence; this is an observation, never a scheduler.
	[[nodiscard]] FrameCadenceResult Cadence() const noexcept;
	//! Bounded owner-path admission/presentation state.
	[[nodiscard]] FrameBackpressureSnapshot Backpressure() const noexcept;

private:
	// Kept visible to the translation-unit helpers that implement the owner
	// loop.  The pointed-to state is never exposed by the public API.
	public:
	struct Shared;

	private:
	static void OwnerMain(const std::shared_ptr<Shared>& shared) noexcept;

	std::shared_ptr<Shared> m_shared;
	std::thread m_ownerThread;
	mutable std::mutex m_joinMutex;
};

} // namespace workbench::rendering
