/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>

namespace workbench::rendering {

enum class EFrameDeviceState : std::uint8_t {
	HardwareReady,
	DeviceLossDetected,
	Quiescing,
	RecreatingHardware,
	CreatingWarp,
	WarpReady,
	SoftwareOnly,
	ProbingHardware,
	Closed,
};

enum class EFrameDeviceFailureBoundary : std::uint8_t {
	Present,
	ResizeBuffers,
	CompositionCommit,
};

enum class EFrameDeviceTransitionStatus : std::uint8_t {
	Succeeded,
	Stale,
	InvalidState,
	Closed,
};

struct FrameDeviceTransitionResult {
	EFrameDeviceTransitionStatus status = EFrameDeviceTransitionStatus::InvalidState;
	EFrameDeviceState state = EFrameDeviceState::Closed;
	std::uint64_t deviceEpoch = 0;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameDeviceTransitionStatus::Succeeded;
	}
};

struct FrameDeviceDomainTelemetry {
	std::uint64_t lossDetections = 0;
	std::uint64_t duplicateLossSignals = 0;
	std::uint64_t staleSignals = 0;
	std::uint64_t hardwareRecreationAttempts = 0;
	std::uint64_t hardwareRecoveries = 0;
	std::uint64_t warpCreationAttempts = 0;
	std::uint64_t warpRecoveries = 0;
	std::uint64_t softwareFallbacks = 0;
	std::uint64_t hardwareProbeAttempts = 0;
};

//! Pure single-writer device-domain recovery model.
//!
//! The presentation owner is the only caller. This type performs no COM/GPU
//! work and never waits; it only fences epochs and makes every recovery branch
//! reach HardwareReady, WarpReady, SoftwareOnly, or Closed explicitly.
class FrameDeviceDomainModel final {
public:
	explicit FrameDeviceDomainModel(std::uint64_t initialDeviceEpoch = 1) noexcept;

	[[nodiscard]] FrameDeviceTransitionResult NotifyDeviceLoss(
		std::uint64_t observedDeviceEpoch,
		EFrameDeviceFailureBoundary boundary,
		long failureCode) noexcept;
	[[nodiscard]] FrameDeviceTransitionResult BeginQuiesce() noexcept;
	[[nodiscard]] FrameDeviceTransitionResult BeginHardwareRecreation() noexcept;
	[[nodiscard]] FrameDeviceTransitionResult CompleteHardwareRecreation(bool succeeded) noexcept;
	[[nodiscard]] FrameDeviceTransitionResult CompleteWarpCreation(
		bool succeeded, std::uint64_t nowMilliseconds) noexcept;

	//! Starts a bounded hardware reprobe only after its backoff deadline.
	[[nodiscard]] FrameDeviceTransitionResult BeginHardwareProbe(std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] FrameDeviceTransitionResult CompleteHardwareProbe(
		bool succeeded, std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] FrameDeviceTransitionResult Close() noexcept;

	[[nodiscard]] EFrameDeviceState State() const noexcept { return m_state; }
	[[nodiscard]] std::uint64_t DeviceEpoch() const noexcept { return m_deviceEpoch; }
	[[nodiscard]] bool SubmissionAllowed() const noexcept;
	[[nodiscard]] std::uint64_t NextHardwareProbeDeadlineMilliseconds() const noexcept
	{
		return m_nextHardwareProbeDeadlineMilliseconds;
	}
	[[nodiscard]] const FrameDeviceDomainTelemetry& Telemetry() const noexcept { return m_telemetry; }

private:
	[[nodiscard]] FrameDeviceTransitionResult Result(EFrameDeviceTransitionStatus status) const noexcept;
	void ScheduleNextProbe(std::uint64_t nowMilliseconds) noexcept;

	EFrameDeviceState m_state = EFrameDeviceState::HardwareReady;
	EFrameDeviceState m_probeFallbackState = EFrameDeviceState::SoftwareOnly;
	std::uint64_t m_deviceEpoch = 1;
	std::uint64_t m_probeBackoffMilliseconds = 1000;
	std::uint64_t m_nextHardwareProbeDeadlineMilliseconds = 0;
	EFrameDeviceFailureBoundary m_lastFailureBoundary = EFrameDeviceFailureBoundary::Present;
	long m_lastFailureCode = 0;
	FrameDeviceDomainTelemetry m_telemetry;
};

} // namespace workbench::rendering
