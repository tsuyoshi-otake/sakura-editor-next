/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameDeviceDomainModel.h"

#include <algorithm>
#include <limits>

namespace workbench::rendering {
namespace {

constexpr std::uint64_t kInitialProbeBackoffMilliseconds = 1000;
constexpr std::uint64_t kMaximumProbeBackoffMilliseconds = 60000;

} // namespace

FrameDeviceDomainModel::FrameDeviceDomainModel(std::uint64_t initialDeviceEpoch) noexcept
	: m_deviceEpoch(initialDeviceEpoch == 0 ? 1 : initialDeviceEpoch)
{
}

FrameDeviceTransitionResult FrameDeviceDomainModel::NotifyDeviceLoss(
	std::uint64_t observedDeviceEpoch,
	EFrameDeviceFailureBoundary boundary,
	long failureCode) noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (observedDeviceEpoch != m_deviceEpoch) {
		++m_telemetry.staleSignals;
		return Result(EFrameDeviceTransitionStatus::Stale);
	}
	if (m_state != EFrameDeviceState::HardwareReady && m_state != EFrameDeviceState::WarpReady) {
		++m_telemetry.duplicateLossSignals;
		return Result(EFrameDeviceTransitionStatus::InvalidState);
	}
	if (m_deviceEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
		return Result(EFrameDeviceTransitionStatus::InvalidState);
	}
	++m_deviceEpoch;
	m_lastFailureBoundary = boundary;
	m_lastFailureCode = failureCode;
	m_state = EFrameDeviceState::DeviceLossDetected;
	++m_telemetry.lossDetections;
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::BeginQuiesce() noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::DeviceLossDetected) return Result(EFrameDeviceTransitionStatus::InvalidState);
	m_state = EFrameDeviceState::Quiescing;
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::BeginHardwareRecreation() noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::Quiescing) return Result(EFrameDeviceTransitionStatus::InvalidState);
	m_state = EFrameDeviceState::RecreatingHardware;
	++m_telemetry.hardwareRecreationAttempts;
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::CompleteHardwareRecreation(bool succeeded) noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::RecreatingHardware) return Result(EFrameDeviceTransitionStatus::InvalidState);
	if (succeeded) {
		m_state = EFrameDeviceState::HardwareReady;
		m_probeBackoffMilliseconds = kInitialProbeBackoffMilliseconds;
		m_nextHardwareProbeDeadlineMilliseconds = 0;
		++m_telemetry.hardwareRecoveries;
	} else {
		m_state = EFrameDeviceState::CreatingWarp;
		++m_telemetry.warpCreationAttempts;
	}
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::CompleteWarpCreation(
	bool succeeded, std::uint64_t nowMilliseconds) noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::CreatingWarp) return Result(EFrameDeviceTransitionStatus::InvalidState);
	if (succeeded) {
		m_state = EFrameDeviceState::WarpReady;
		++m_telemetry.warpRecoveries;
	} else {
		m_state = EFrameDeviceState::SoftwareOnly;
		++m_telemetry.softwareFallbacks;
	}
	ScheduleNextProbe(nowMilliseconds);
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::BeginHardwareProbe(std::uint64_t nowMilliseconds) noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::WarpReady && m_state != EFrameDeviceState::SoftwareOnly) {
		return Result(EFrameDeviceTransitionStatus::InvalidState);
	}
	if (nowMilliseconds < m_nextHardwareProbeDeadlineMilliseconds) {
		return Result(EFrameDeviceTransitionStatus::InvalidState);
	}
	if (m_deviceEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
		return Result(EFrameDeviceTransitionStatus::InvalidState);
	}
	m_probeFallbackState = m_state;
	++m_deviceEpoch;
	m_state = EFrameDeviceState::ProbingHardware;
	++m_telemetry.hardwareProbeAttempts;
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::CompleteHardwareProbe(
	bool succeeded, std::uint64_t nowMilliseconds) noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	if (m_state != EFrameDeviceState::ProbingHardware) return Result(EFrameDeviceTransitionStatus::InvalidState);
	if (succeeded) {
		m_state = EFrameDeviceState::HardwareReady;
		m_probeBackoffMilliseconds = kInitialProbeBackoffMilliseconds;
		m_nextHardwareProbeDeadlineMilliseconds = 0;
		++m_telemetry.hardwareRecoveries;
	} else {
		m_state = m_probeFallbackState;
		ScheduleNextProbe(nowMilliseconds);
	}
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

FrameDeviceTransitionResult FrameDeviceDomainModel::Close() noexcept
{
	if (m_state == EFrameDeviceState::Closed) return Result(EFrameDeviceTransitionStatus::Closed);
	m_state = EFrameDeviceState::Closed;
	return Result(EFrameDeviceTransitionStatus::Succeeded);
}

bool FrameDeviceDomainModel::SubmissionAllowed() const noexcept
{
	return m_state == EFrameDeviceState::HardwareReady || m_state == EFrameDeviceState::WarpReady;
}

FrameDeviceTransitionResult FrameDeviceDomainModel::Result(EFrameDeviceTransitionStatus status) const noexcept
{
	return { status, m_state, m_deviceEpoch };
}

void FrameDeviceDomainModel::ScheduleNextProbe(std::uint64_t nowMilliseconds) noexcept
{
	const auto room = (std::numeric_limits<std::uint64_t>::max)() - nowMilliseconds;
	m_nextHardwareProbeDeadlineMilliseconds = room < m_probeBackoffMilliseconds
		? (std::numeric_limits<std::uint64_t>::max)()
		: nowMilliseconds + m_probeBackoffMilliseconds;
	m_probeBackoffMilliseconds = (std::min)(
		kMaximumProbeBackoffMilliseconds, m_probeBackoffMilliseconds * 2);
}

} // namespace workbench::rendering
