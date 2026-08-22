/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace workbench::rendering {

using FrameSurfaceId = std::uint64_t;

enum class EFrameWorkClass : std::uint8_t {
	Background,
	Visible,
	Interactive,
};

enum class EFrameSurfacePhase : std::uint8_t {
	Idle,
	Requested,
	CpuRunning,
	CpuReady,
	GpuQueued,
	GpuUpdating,
	Publishable,
	Closing,
	Withdrawn,
	Closed,
};

enum class EFrameOperationStatus : std::uint8_t {
	Succeeded,
	Replaced,
	Superseded,
	Stale,
	Exhausted,
	Invalid,
	UnknownSurface,
	Busy,
	Closed,
};

struct FrameSurfaceRequest {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint64_t contentGeneration = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	EFrameWorkClass workClass = EFrameWorkClass::Background;
	bool visible = false;

	[[nodiscard]] bool IsValid() const noexcept;
};

struct FrameWorkTicket {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint64_t contentGeneration = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	EFrameWorkClass workClass = EFrameWorkClass::Background;
	bool visible = false;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==( const FrameWorkTicket& rhs ) const noexcept = default;
};

struct FrameOperationResult {
	EFrameOperationStatus status = EFrameOperationStatus::Invalid;
	EFrameSurfacePhase phase = EFrameSurfacePhase::Closed;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameOperationStatus::Succeeded
			|| status == EFrameOperationStatus::Replaced;
	}
};

struct FramePublication {
	FrameWorkTicket ticket;
};

struct FrameLateSurface {
	FrameSurfaceId surfaceId = 0;
	bool hasLastGoodContent = false;
};

struct FrameCommitCohort {
	std::uint64_t windowFrameId = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::vector<FramePublication> publications;
	std::vector<FrameLateSurface> lateSurfaces;
};

struct FrameSurfaceSnapshot {
	FrameSurfaceId surfaceId = 0;
	std::uint64_t surfaceLifetimeEpoch = 0;
	EFrameSurfacePhase phase = EFrameSurfacePhase::Closed;
	std::size_t pendingDepth = 0;
	std::uint64_t activeRequestId = 0;
	std::uint64_t newestRequestId = 0;
	std::uint64_t publishedRequestId = 0;
	std::uint32_t aging = 0;
	bool hasLastGoodContent = false;
	bool closeRequested = false;
};

struct FrameCoordinatorTelemetry {
	std::uint64_t acceptedRequests = 0;
	std::uint64_t replacedRequests = 0;
	std::uint64_t staleRequests = 0;
	std::uint64_t scheduledCpuWork = 0;
	std::uint64_t supersededWork = 0;
	std::uint64_t assembledFrames = 0;
	std::uint64_t publishedSurfaces = 0;
	std::uint64_t lateSurfaceObservations = 0;
	std::uint64_t deviceResets = 0;
};

//! Pure single-writer scheduling model for one top-level window.
//!
//! This type owns no thread, HWND, worker, or GPU object.  The presentation
//! owner calls it to enforce latest-only mailboxes, epoch fencing, bounded
//! scheduling, and commit cohorts that never wait for every dirty surface.
class FrameCoordinatorModel final {
public:
	explicit FrameCoordinatorModel( std::uint64_t deviceEpoch = 1 ) noexcept;

	[[nodiscard]] FrameOperationResult RegisterSurface(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch );
	[[nodiscard]] FrameOperationResult CloseSurface(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch );
	//! Forces a registered lifetime to its terminal state during owner shutdown.
	//! The presentation owner may call this only after fencing external work and
	//! discarding publications; normal surface removal must use CloseSurface().
	[[nodiscard]] FrameOperationResult FinalizeCloseSurface(
		FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch ) noexcept;

	[[nodiscard]] FrameOperationResult Request( const FrameSurfaceRequest& request );
	[[nodiscard]] std::optional<FrameWorkTicket> TakeNextCpuWork();
	[[nodiscard]] FrameOperationResult CancelCpu( const FrameWorkTicket& ticket );
	[[nodiscard]] FrameOperationResult CompleteCpu( const FrameWorkTicket& ticket );
	[[nodiscard]] FrameOperationResult QueueGpu( const FrameWorkTicket& ticket );
	[[nodiscard]] FrameOperationResult BeginGpu( const FrameWorkTicket& ticket );
	[[nodiscard]] FrameOperationResult CompleteGpu( const FrameWorkTicket& ticket );
	[[nodiscard]] FrameOperationResult RetireWithdrawn( const FrameWorkTicket& ticket );

	[[nodiscard]] FrameCommitCohort AssembleCommit(
		std::uint64_t windowFrameId, std::uint64_t layoutEpoch,
		std::size_t maximumPublications );
	[[nodiscard]] FrameOperationResult CompleteCommit( const FrameCommitCohort& cohort );

	[[nodiscard]] FrameOperationResult ResetDevice( std::uint64_t newDeviceEpoch );

	[[nodiscard]] std::optional<FrameSurfaceSnapshot> SurfaceSnapshot(
		FrameSurfaceId surfaceId ) const;
	[[nodiscard]] const FrameCoordinatorTelemetry& Telemetry() const noexcept
	{
		return m_telemetry;
	}
	[[nodiscard]] std::uint64_t DeviceEpoch() const noexcept { return m_deviceEpoch; }

private:
	struct SurfaceSlot {
		std::uint64_t lifetimeEpoch = 0;
		EFrameSurfacePhase phase = EFrameSurfacePhase::Closed;
		std::optional<FrameSurfaceRequest> pending;
		std::optional<FrameWorkTicket> active;
		std::uint64_t newestRequestId = 0;
		std::uint64_t publishedRequestId = 0;
		std::uint32_t aging = 0;
		bool hasLastGoodContent = false;
		bool closeRequested = false;
	};

	[[nodiscard]] FrameOperationResult Transition(
		const FrameWorkTicket& ticket, EFrameSurfacePhase expected,
		EFrameSurfacePhase next );
	[[nodiscard]] static FrameWorkTicket MakeTicket( const FrameSurfaceRequest& request ) noexcept;
	[[nodiscard]] static bool IsWorkOutstanding( const SurfaceSlot& slot ) noexcept;
	void FinishActive( SurfaceSlot& slot );

	std::uint64_t m_deviceEpoch = 1;
	std::uint64_t m_lastCompletedFrameId = 0;
	std::unordered_map<FrameSurfaceId, SurfaceSlot> m_surfaces;
	FrameCoordinatorTelemetry m_telemetry;
};

} // namespace workbench::rendering
