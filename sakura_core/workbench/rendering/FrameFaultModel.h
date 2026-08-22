/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>

namespace workbench::rendering {

enum class EFrameFaultBoundary : std::uint8_t {
	Present,
	Resize,
	CompositionCommit,
};

enum class EFrameFaultState : std::uint8_t {
	Ready,
	DeviceLost,
	Recovering,
	WarpReady,
	SoftwareOnly,
	Closed,
	Failed,
};

enum class EFrameFaultStatus : std::uint8_t {
	Accepted,
	AlreadyApplied,
	Invalid,
	Closed,
	Failed,
};

struct FrameFaultResult final {
	EFrameFaultStatus status = EFrameFaultStatus::Invalid;
	EFrameFaultState state = EFrameFaultState::Failed;
	std::uint64_t deviceEpoch = 1;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameFaultStatus::Accepted
			|| status == EFrameFaultStatus::AlreadyApplied;
	}
};

struct FrameFaultSnapshot final {
	EFrameFaultState state = EFrameFaultState::Ready;
	std::uint64_t deviceEpoch = 1;
	std::uint64_t injectedFailures = 0;
	std::uint64_t recoveries = 0;
	std::uint64_t softwareFallbacks = 0;
	EFrameFaultBoundary lastBoundary = EFrameFaultBoundary::Present;
	long lastFailureCode = 0;
};

//! Deterministic owner-thread fault transition model for C8 harnesses.
//!
//! It models the terminal outcome of a device-loss injection without touching
//! a native device. The production FrameDeviceDomainModel remains the source
//! of native recovery; this type makes fault gates repeatable in pure tests.
class FrameFaultModel final {
public:
	explicit FrameFaultModel(std::uint64_t initialDeviceEpoch = 1) noexcept;

	[[nodiscard]] FrameFaultResult Inject(
		EFrameFaultBoundary boundary, long failureCode) noexcept;
	[[nodiscard]] FrameFaultResult Recover(
		bool hardwareAvailable, bool warpAvailable) noexcept;
	[[nodiscard]] FrameFaultResult Close() noexcept;
	[[nodiscard]] FrameFaultSnapshot Snapshot() const noexcept { return m_snapshot; }

private:
	FrameFaultSnapshot m_snapshot;
};

} // namespace workbench::rendering
