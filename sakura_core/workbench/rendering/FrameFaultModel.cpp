#include "StdAfx.h"
#include "workbench/rendering/FrameFaultModel.h"

#include <limits>

namespace workbench::rendering {

FrameFaultModel::FrameFaultModel(const std::uint64_t initialDeviceEpoch) noexcept
{
	m_snapshot.deviceEpoch = initialDeviceEpoch == 0 ? 1 : initialDeviceEpoch;
}

FrameFaultResult FrameFaultModel::Inject(
	const EFrameFaultBoundary boundary, const long failureCode) noexcept
{
	if (m_snapshot.state == EFrameFaultState::Closed) {
		return { EFrameFaultStatus::Closed, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	if (m_snapshot.state == EFrameFaultState::DeviceLost
		|| m_snapshot.state == EFrameFaultState::Recovering) {
		return { EFrameFaultStatus::AlreadyApplied, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	if (m_snapshot.state != EFrameFaultState::Ready
		&& m_snapshot.state != EFrameFaultState::WarpReady
		&& m_snapshot.state != EFrameFaultState::SoftwareOnly) {
		return { EFrameFaultStatus::Failed, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	++m_snapshot.injectedFailures;
	m_snapshot.lastBoundary = boundary;
	m_snapshot.lastFailureCode = failureCode;
	m_snapshot.state = EFrameFaultState::DeviceLost;
	return { EFrameFaultStatus::Accepted, m_snapshot.state, m_snapshot.deviceEpoch };
}

FrameFaultResult FrameFaultModel::Recover(
	const bool hardwareAvailable, const bool warpAvailable) noexcept
{
	if (m_snapshot.state == EFrameFaultState::Closed) {
		return { EFrameFaultStatus::Closed, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	if (m_snapshot.state != EFrameFaultState::DeviceLost) {
		return { EFrameFaultStatus::AlreadyApplied, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	m_snapshot.state = EFrameFaultState::Recovering;
	if (m_snapshot.deviceEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
		m_snapshot.state = EFrameFaultState::Failed;
		return { EFrameFaultStatus::Failed, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	++m_snapshot.deviceEpoch;
	++m_snapshot.recoveries;
	if (hardwareAvailable) {
		m_snapshot.state = EFrameFaultState::Ready;
	} else if (warpAvailable) {
		m_snapshot.state = EFrameFaultState::WarpReady;
	} else {
		m_snapshot.state = EFrameFaultState::SoftwareOnly;
		++m_snapshot.softwareFallbacks;
	}
	return { EFrameFaultStatus::Accepted, m_snapshot.state, m_snapshot.deviceEpoch };
}

FrameFaultResult FrameFaultModel::Close() noexcept
{
	if (m_snapshot.state == EFrameFaultState::Closed) {
		return { EFrameFaultStatus::AlreadyApplied, m_snapshot.state, m_snapshot.deviceEpoch };
	}
	m_snapshot.state = EFrameFaultState::Closed;
	return { EFrameFaultStatus::Accepted, m_snapshot.state, m_snapshot.deviceEpoch };
}

} // namespace workbench::rendering
