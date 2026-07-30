/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostStateMachine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace {

std::chrono::milliseconds ValidatePositive(
	std::chrono::milliseconds value,
	std::chrono::milliseconds fallback) noexcept
{
	return value.count() > 0 ? value : fallback;
}
} // namespace

CExtensionHostStateMachine::CExtensionHostStateMachine(SExtensionHostLifecycleConfig config)
	: m_config(std::move(config))
{
	m_config.keepAlive = ValidatePositive(m_config.keepAlive, std::chrono::milliseconds(60'000));
	m_config.startTimeout = ValidatePositive(m_config.startTimeout, std::chrono::milliseconds(15'000));
	m_config.quiesceTimeout = ValidatePositive(m_config.quiesceTimeout, std::chrono::milliseconds(5'000));
	m_config.initialBackoff = ValidatePositive(m_config.initialBackoff, std::chrono::milliseconds(100));
	m_config.maximumBackoff = ValidatePositive(m_config.maximumBackoff, m_config.initialBackoff);
	if (m_config.maximumBackoff < m_config.initialBackoff) {
		m_config.maximumBackoff = m_config.initialBackoff;
	}
	m_config.jitterRatio = std::clamp(m_config.jitterRatio, 0.0, 1.0);
}

std::size_t CExtensionHostStateMachine::GetLeaseCount() const noexcept
{
	return std::accumulate(m_leases.begin(), m_leases.end(), std::size_t{},
		[](std::size_t count, const auto& lease) {
			return count + lease.second;
		});
}

bool CExtensionHostStateMachine::IsCurrentGeneration(std::uint64_t generation) const noexcept
{
	return generation != 0 && generation == m_generation;
}

void CExtensionHostStateMachine::BeginStart(TimePoint now, Actions& actions)
{
	++m_generation;
	if (m_generation == 0) {
		++m_generation;
	}
	m_state = EExtensionHostState::Starting;
	m_deadline = now + m_config.startTimeout;
	actions.push_back({ EExtensionHostLifecycleActionKind::StartHost, m_generation });
}

void CExtensionHostStateMachine::EnterStopped(Actions& actions, std::string diagnostic)
{
	m_state = EExtensionHostState::Stopped;
	m_deadline = {};
	actions.push_back({
		EExtensionHostLifecycleActionKind::Stopped,
		m_generation,
		{},
		std::move(diagnostic),
	});
}

std::chrono::milliseconds CExtensionHostStateMachine::ComputeBackoff(double jitterUnit) const noexcept
{
	const auto retryExponent = m_retryCount > 0 ? m_retryCount - 1 : 0;
	std::int64_t delay = m_config.initialBackoff.count();
	for (std::uint32_t i = 0; i < retryExponent && delay < m_config.maximumBackoff.count(); ++i) {
		if (delay > m_config.maximumBackoff.count() / 2) {
			delay = m_config.maximumBackoff.count();
			break;
		}
		delay *= 2;
	}
	delay = (std::min)(delay, m_config.maximumBackoff.count());

	const double normalized = std::clamp(jitterUnit, 0.0, 1.0);
	const double signedUnit = normalized * 2.0 - 1.0;
	const auto offset = static_cast<std::int64_t>(std::llround(
		static_cast<double>(delay) * m_config.jitterRatio * signedUnit));
	return std::chrono::milliseconds((std::max)(std::int64_t{}, delay + offset));
}

void CExtensionHostStateMachine::EnterFailure(
	TimePoint now,
	std::string diagnostic,
	double jitterUnit,
	Actions& actions)
{
	if (m_shutdownRequested) {
		EnterStopped(actions, std::move(diagnostic));
		return;
	}
	if (!HasLeases()) {
		m_state = EExtensionHostState::Absent;
		m_deadline = {};
		m_retryCount = 0;
		return;
	}

	++m_retryCount;
	if (m_retryCount > m_config.maximumRetryCount) {
		EnterStopped(actions, std::move(diagnostic));
		return;
	}

	const auto delay = ComputeBackoff(jitterUnit);
	m_state = EExtensionHostState::Failed;
	m_deadline = now + delay;
	actions.push_back({
		EExtensionHostLifecycleActionKind::ScheduleRetry,
		m_generation,
		delay,
		std::move(diagnostic),
	});
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::AcquireLease(
	std::uint32_t editorProcessId,
	TimePoint now)
{
	Actions actions;
	if (editorProcessId == 0) {
		actions.push_back({
			EExtensionHostLifecycleActionKind::LeaseRejected,
			m_generation,
			{},
			"editor process id must not be zero",
		});
		return actions;
	}
	if (m_shutdownRequested) {
		actions.push_back({
			EExtensionHostLifecycleActionKind::LeaseRejected,
			m_generation,
			{},
			"extension host broker is shutting down",
		});
		return actions;
	}

	++m_leases[editorProcessId];
	switch (m_state) {
	case EExtensionHostState::Absent:
	case EExtensionHostState::Stopped:
		m_retryCount = 0;
		BeginStart(now, actions);
		break;
	case EExtensionHostState::KeepAlive:
		m_state = EExtensionHostState::Ready;
		m_deadline = {};
		break;
	case EExtensionHostState::Failed:
		if (now >= m_deadline) {
			BeginStart(now, actions);
		}
		break;
	case EExtensionHostState::Starting:
	case EExtensionHostState::Ready:
	case EExtensionHostState::Quiescing:
		break;
	}
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::ReleaseLease(
	std::uint32_t editorProcessId,
	TimePoint now)
{
	Actions actions;
	const auto found = m_leases.find(editorProcessId);
	if (found == m_leases.end()) {
		return actions;
	}
	if (--found->second == 0) {
		m_leases.erase(found);
	}
	if (HasLeases()) {
		return actions;
	}

	if (m_state == EExtensionHostState::Ready) {
		m_state = EExtensionHostState::KeepAlive;
		m_deadline = now + m_config.keepAlive;
	}
	else if (m_state == EExtensionHostState::Failed) {
		m_state = EExtensionHostState::Absent;
		m_deadline = {};
		m_retryCount = 0;
	}
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::OnHostReady(
	std::uint64_t generation,
	TimePoint now)
{
	Actions actions;
	if (m_state != EExtensionHostState::Starting || !IsCurrentGeneration(generation)) {
		return actions;
	}
	m_retryCount = 0;
	if (m_shutdownRequested) {
		m_state = EExtensionHostState::Quiescing;
		m_deadline = now + m_config.quiesceTimeout;
		actions.push_back({ EExtensionHostLifecycleActionKind::BeginQuiesce, m_generation });
	}
	else if (HasLeases()) {
		m_state = EExtensionHostState::Ready;
		m_deadline = {};
	}
	else {
		m_state = EExtensionHostState::KeepAlive;
		m_deadline = now + m_config.keepAlive;
	}
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::OnHostStartFailed(
	std::uint64_t generation,
	TimePoint now,
	std::string diagnostic,
	double jitterUnit)
{
	Actions actions;
	if (m_state != EExtensionHostState::Starting || !IsCurrentGeneration(generation)) {
		return actions;
	}
	EnterFailure(now, std::move(diagnostic), jitterUnit, actions);
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::OnHostLost(
	std::uint64_t generation,
	TimePoint now,
	EExtensionHostLossKind lossKind,
	std::string diagnostic,
	double jitterUnit)
{
	Actions actions;
	if (!IsCurrentGeneration(generation) ||
		(m_state != EExtensionHostState::Starting &&
		 m_state != EExtensionHostState::Ready &&
		 m_state != EExtensionHostState::KeepAlive &&
		 m_state != EExtensionHostState::Quiescing)) {
		return actions;
	}
	actions.push_back({
		EExtensionHostLifecycleActionKind::RejectPendingHostLost,
		m_generation,
		{},
		diagnostic,
	});

	if (lossKind == EExtensionHostLossKind::BrokerLost) {
		m_state = m_shutdownRequested ? EExtensionHostState::Stopped : EExtensionHostState::Absent;
		m_deadline = {};
		m_retryCount = 0;
		if (m_shutdownRequested) {
			actions.push_back({ EExtensionHostLifecycleActionKind::Stopped, m_generation });
		}
		return actions;
	}

	EnterFailure(now, std::move(diagnostic), jitterUnit, actions);
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::OnQuiesceCompleted(
	std::uint64_t generation,
	TimePoint now)
{
	Actions actions;
	if (m_state != EExtensionHostState::Quiescing || !IsCurrentGeneration(generation)) {
		return actions;
	}
	if (!m_shutdownRequested && HasLeases()) {
		m_retryCount = 0;
		BeginStart(now, actions);
	}
	else {
		EnterStopped(actions);
	}
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::Tick(
	TimePoint now,
	double jitterUnit)
{
	Actions actions;
	if (m_deadline == TimePoint{} || now < m_deadline) {
		return actions;
	}

	switch (m_state) {
	case EExtensionHostState::Starting:
		actions.push_back({
			EExtensionHostLifecycleActionKind::ForceTerminate,
			m_generation,
			{},
			"extension host start timed out",
		});
		EnterFailure(now, "extension host start timed out", jitterUnit, actions);
		break;
	case EExtensionHostState::KeepAlive:
		m_state = EExtensionHostState::Quiescing;
		m_deadline = now + m_config.quiesceTimeout;
		actions.push_back({ EExtensionHostLifecycleActionKind::BeginQuiesce, m_generation });
		break;
	case EExtensionHostState::Failed:
		if (m_shutdownRequested || !HasLeases()) {
			EnterStopped(actions);
		}
		else {
			BeginStart(now, actions);
		}
		break;
	case EExtensionHostState::Quiescing:
		actions.push_back({
			EExtensionHostLifecycleActionKind::ForceTerminate,
			m_generation,
			{},
			"extension host deactivate timed out",
		});
		if (!m_shutdownRequested && HasLeases()) {
			BeginStart(now, actions);
		}
		else {
			EnterStopped(actions, "extension host deactivate timed out");
		}
		break;
	case EExtensionHostState::Absent:
	case EExtensionHostState::Ready:
	case EExtensionHostState::Stopped:
		break;
	}
	return actions;
}

CExtensionHostStateMachine::Actions CExtensionHostStateMachine::Shutdown(TimePoint now)
{
	Actions actions;
	if (m_shutdownRequested) {
		return actions;
	}
	m_shutdownRequested = true;
	m_leases.clear();

	switch (m_state) {
	case EExtensionHostState::Starting:
	case EExtensionHostState::Ready:
	case EExtensionHostState::KeepAlive:
		m_state = EExtensionHostState::Quiescing;
		m_deadline = now + m_config.quiesceTimeout;
		actions.push_back({ EExtensionHostLifecycleActionKind::BeginQuiesce, m_generation });
		break;
	case EExtensionHostState::Quiescing:
		break;
	case EExtensionHostState::Absent:
	case EExtensionHostState::Failed:
	case EExtensionHostState::Stopped:
		EnterStopped(actions);
		break;
	}
	return actions;
}
