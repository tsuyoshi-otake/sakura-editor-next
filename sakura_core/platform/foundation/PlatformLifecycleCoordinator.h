/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/foundation/PlatformServiceRegistry.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace platform::foundation {

//! A participant returns false only after it has completed its own failed-start
//! cleanup.  The coordinator subsequently stops every participant that returned
//! true, in reverse dependency order.
using LifecycleAction = std::function<bool()>;

struct LifecycleParticipant {
	ServiceId serviceId;
	std::vector<ServiceId> dependencies;
	LifecycleAction start;
	LifecycleAction shutdown;
};

enum class LifecycleRegistrationOutcome : unsigned char {
	Registered,
	InvalidParticipant,
	DuplicateServiceId,
	OperationInProgress,
};

enum class LifecycleState : unsigned char {
	Stopped,
	Starting,
	Running,
	Stopping,
};

//! Each operation terminates in one of these values.  In particular, a failed
//! start never leaves the coordinator in an indeterminate intermediate state.
enum class LifecycleOperationOutcome : unsigned char {
	Started,
	AlreadyRunning,
	ShutdownSucceeded,
	AlreadyStopped,
	DependencyMissing,
	DependencyCycle,
	StartupFailedRolledBack,
	StartupFailedRollbackFailed,
	ShutdownCompletedWithFailures,
	OperationInProgress,
};

struct LifecycleOperationResult {
	LifecycleOperationOutcome outcome = LifecycleOperationOutcome::AlreadyStopped;
	//! The participant or dependency responsible for a non-success outcome.
	ServiceId failedServiceId;
	//! Participants whose start action returned true, in startup order.
	std::vector<ServiceId> startedServices;
	//! Participants whose shutdown action was attempted, in shutdown order.
	std::vector<ServiceId> stoppedServices;
};

//! Starts independent platform services in dependency order and always provides a
//! deterministic terminal result.  It has no OS, UI, or legacy-process dependency.
class PlatformLifecycleCoordinator final {
public:
	PlatformLifecycleCoordinator() = default;
	~PlatformLifecycleCoordinator() = default;
	PlatformLifecycleCoordinator(const PlatformLifecycleCoordinator&) = delete;
	PlatformLifecycleCoordinator& operator=(const PlatformLifecycleCoordinator&) = delete;

	[[nodiscard]] LifecycleRegistrationOutcome Register(LifecycleParticipant participant)
	{
		if (participant.serviceId.empty() || !participant.start || !participant.shutdown) {
			return LifecycleRegistrationOutcome::InvalidParticipant;
		}

		std::lock_guard lock(m_mutex);
		if (m_state != LifecycleState::Stopped) {
			return LifecycleRegistrationOutcome::OperationInProgress;
		}
		if (m_participantIndices.contains(participant.serviceId)) {
			return LifecycleRegistrationOutcome::DuplicateServiceId;
		}

		m_participantIndices.emplace(participant.serviceId, m_participants.size());
		m_participants.emplace_back(std::move(participant));
		return LifecycleRegistrationOutcome::Registered;
	}

	[[nodiscard]] LifecycleOperationResult Start()
	{
		std::vector<LifecycleParticipant> participants;
		{
			std::lock_guard lock(m_mutex);
			if (m_state == LifecycleState::Running) {
				return { LifecycleOperationOutcome::AlreadyRunning };
			}
			if (m_state != LifecycleState::Stopped) {
				return { LifecycleOperationOutcome::OperationInProgress };
			}
			m_state = LifecycleState::Starting;
			participants = m_participants;
		}

		const auto order = ResolveStartupOrder(participants);
		if (order.outcome != LifecycleOperationOutcome::Started) {
			return CompleteFailedStart({ order.outcome, order.failedServiceId });
		}

		LifecycleOperationResult result;
		std::vector<LifecycleParticipant> startedParticipants;
		for (const auto participantIndex : order.indices) {
			const auto& participant = participants[participantIndex];
			if (!Invoke(participant.start)) {
				result.outcome = Rollback(startedParticipants, result.stoppedServices)
					? LifecycleOperationOutcome::StartupFailedRolledBack
					: LifecycleOperationOutcome::StartupFailedRollbackFailed;
				result.failedServiceId = participant.serviceId;
				return CompleteFailedStart(std::move(result));
			}
			result.startedServices.emplace_back(participant.serviceId);
			startedParticipants.emplace_back(participant);
		}

		result.outcome = LifecycleOperationOutcome::Started;
		{
			std::lock_guard lock(m_mutex);
			m_startedParticipants = std::move(startedParticipants);
			m_state = LifecycleState::Running;
		}
		return result;
	}

	[[nodiscard]] LifecycleOperationResult Shutdown()
	{
		std::vector<LifecycleParticipant> startedParticipants;
		{
			std::lock_guard lock(m_mutex);
			if (m_state == LifecycleState::Stopped) {
				return { LifecycleOperationOutcome::AlreadyStopped };
			}
			if (m_state != LifecycleState::Running) {
				return { LifecycleOperationOutcome::OperationInProgress };
			}
			m_state = LifecycleState::Stopping;
			startedParticipants = m_startedParticipants;
		}

		LifecycleOperationResult result;
		const bool shutdownSucceeded = Rollback(startedParticipants, result.stoppedServices, &result.failedServiceId);
		result.outcome = shutdownSucceeded
			? LifecycleOperationOutcome::ShutdownSucceeded
			: LifecycleOperationOutcome::ShutdownCompletedWithFailures;
		{
			std::lock_guard lock(m_mutex);
			m_startedParticipants.clear();
			m_state = LifecycleState::Stopped;
		}
		return result;
	}

	[[nodiscard]] LifecycleState State() const noexcept
	{
		std::lock_guard lock(m_mutex);
		return m_state;
	}

private:
	struct StartupOrder {
		LifecycleOperationOutcome outcome = LifecycleOperationOutcome::Started;
		ServiceId failedServiceId;
		std::vector<std::size_t> indices;
	};

	[[nodiscard]] static StartupOrder ResolveStartupOrder(const std::vector<LifecycleParticipant>& participants)
	{
		std::unordered_map<ServiceId, std::size_t> indices;
		indices.reserve(participants.size());
		for (std::size_t index = 0; index < participants.size(); ++index) {
			// Duplicate IDs cannot be registered, but retain an explicit terminal
			// outcome should a future caller construct the input another way.
			if (!indices.emplace(participants[index].serviceId, index).second) {
				return { LifecycleOperationOutcome::DependencyCycle, participants[index].serviceId };
			}
		}

		std::vector<std::vector<std::size_t>> dependents(participants.size());
		std::vector<std::size_t> dependencyCounts(participants.size());
		for (std::size_t index = 0; index < participants.size(); ++index) {
			std::unordered_set<ServiceId> seenDependencies;
			for (const auto& dependency : participants[index].dependencies) {
				const auto dependencyIndex = indices.find(dependency);
				if (dependencyIndex == indices.end()) {
					return { LifecycleOperationOutcome::DependencyMissing, dependency };
				}
				if (!seenDependencies.insert(dependency).second) {
					continue;
				}
				dependents[dependencyIndex->second].emplace_back(index);
				++dependencyCounts[index];
			}
		}

		std::vector<std::size_t> ready;
		ready.reserve(participants.size());
		for (std::size_t index = 0; index < participants.size(); ++index) {
			if (dependencyCounts[index] == 0) {
				ready.emplace_back(index);
			}
		}

		StartupOrder result;
		result.indices.reserve(participants.size());
		for (std::size_t readyIndex = 0; readyIndex < ready.size(); ++readyIndex) {
			const auto participantIndex = ready[readyIndex];
			result.indices.emplace_back(participantIndex);
			for (const auto dependentIndex : dependents[participantIndex]) {
				if (--dependencyCounts[dependentIndex] == 0) {
					ready.emplace_back(dependentIndex);
				}
			}
		}

		if (result.indices.size() == participants.size()) {
			return result;
		}
		for (std::size_t index = 0; index < participants.size(); ++index) {
			if (dependencyCounts[index] != 0) {
				return { LifecycleOperationOutcome::DependencyCycle, participants[index].serviceId };
			}
		}
		return { LifecycleOperationOutcome::DependencyCycle };
	}

	[[nodiscard]] LifecycleOperationResult CompleteFailedStart(LifecycleOperationResult result)
	{
		std::lock_guard lock(m_mutex);
		m_startedParticipants.clear();
		m_state = LifecycleState::Stopped;
		return result;
	}

	[[nodiscard]] static bool Invoke(const LifecycleAction& action) noexcept
	{
		try {
			return action();
		}
		catch (...) {
			return false;
		}
	}

	[[nodiscard]] static bool Rollback(const std::vector<LifecycleParticipant>& participants,
		std::vector<ServiceId>& stoppedServices, ServiceId* firstFailure = nullptr) noexcept
	{
		bool succeeded = true;
		for (auto participant = participants.rbegin(); participant != participants.rend(); ++participant) {
			stoppedServices.emplace_back(participant->serviceId);
			if (!Invoke(participant->shutdown)) {
				succeeded = false;
				if (firstFailure != nullptr && firstFailure->empty()) {
					*firstFailure = participant->serviceId;
				}
			}
		}
		return succeeded;
	}

	mutable std::mutex m_mutex;
	LifecycleState m_state = LifecycleState::Stopped;
	std::vector<LifecycleParticipant> m_participants;
	std::unordered_map<ServiceId, std::size_t> m_participantIndices;
	std::vector<LifecycleParticipant> m_startedParticipants;
};

} // namespace platform::foundation
