/*! @file
	@brief 拡張ホストのライフサイクル状態機械
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONHOSTSTATEMACHINE_7E08A7B4_8D67_4B06_B0A9_51A9D2D637D8_H_
#define SAKURA_CEXTENSIONHOSTSTATEMACHINE_7E08A7B4_8D67_4B06_B0A9_51A9D2D637D8_H_
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class EExtensionHostState {
	Absent,
	Starting,
	Ready,
	KeepAlive,
	Quiescing,
	Failed,
	Stopped,
};

enum class EExtensionHostLossKind {
	HostCrash,
	ProtocolError,
	BrokerLost,
};

enum class EExtensionHostLifecycleActionKind {
	StartHost,
	ScheduleRetry,
	BeginQuiesce,
	ForceTerminate,
	RejectPendingHostLost,
	Stopped,
	LeaseRejected,
};

struct SExtensionHostLifecycleConfig {
	std::chrono::milliseconds keepAlive{ 60'000 };
	std::chrono::milliseconds startTimeout{ 15'000 };
	std::chrono::milliseconds quiesceTimeout{ 5'000 };
	std::chrono::milliseconds initialBackoff{ 100 };
	std::chrono::milliseconds maximumBackoff{ 5'000 };
	std::uint32_t maximumRetryCount = 4;
	double jitterRatio = 0.20;
};

struct SExtensionHostLifecycleAction {
	EExtensionHostLifecycleActionKind kind = EExtensionHostLifecycleActionKind::Stopped;
	std::uint64_t generation = 0;
	std::chrono::milliseconds delay{};
	std::string diagnostic;
};

class CExtensionHostStateMachine {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	using Actions = std::vector<SExtensionHostLifecycleAction>;

	explicit CExtensionHostStateMachine(SExtensionHostLifecycleConfig config = {});

	Actions AcquireLease(std::uint32_t editorProcessId, TimePoint now);
	Actions ReleaseLease(std::uint32_t editorProcessId, TimePoint now);
	Actions OnHostReady(std::uint64_t generation, TimePoint now);
	Actions OnHostStartFailed(
		std::uint64_t generation,
		TimePoint now,
		std::string diagnostic,
		double jitterUnit = 0.5);
	Actions OnHostLost(
		std::uint64_t generation,
		TimePoint now,
		EExtensionHostLossKind lossKind,
		std::string diagnostic,
		double jitterUnit = 0.5);
	Actions OnQuiesceCompleted(std::uint64_t generation, TimePoint now);
	Actions Tick(TimePoint now, double jitterUnit = 0.5);
	Actions Shutdown(TimePoint now);

	EExtensionHostState GetState() const noexcept { return m_state; }
	std::uint64_t GetGeneration() const noexcept { return m_generation; }
	std::size_t GetLeaseOwnerCount() const noexcept { return m_leases.size(); }
	std::size_t GetLeaseCount() const noexcept;
	std::uint32_t GetRetryCount() const noexcept { return m_retryCount; }
	bool IsShutdownRequested() const noexcept { return m_shutdownRequested; }
	TimePoint GetDeadline() const noexcept { return m_deadline; }

private:
	void BeginStart(TimePoint now, Actions& actions);
	void EnterStopped(Actions& actions, std::string diagnostic = {});
	void EnterFailure(
		TimePoint now,
		std::string diagnostic,
		double jitterUnit,
		Actions& actions);
	std::chrono::milliseconds ComputeBackoff(double jitterUnit) const noexcept;
	bool HasLeases() const noexcept { return !m_leases.empty(); }
	bool IsCurrentGeneration(std::uint64_t generation) const noexcept;

	SExtensionHostLifecycleConfig m_config;
	EExtensionHostState m_state = EExtensionHostState::Absent;
	std::unordered_map<std::uint32_t, std::uint32_t> m_leases;
	std::uint64_t m_generation = 0;
	std::uint32_t m_retryCount = 0;
	TimePoint m_deadline{};
	bool m_shutdownRequested = false;
};

#endif /* SAKURA_CEXTENSIONHOSTSTATEMACHINE_7E08A7B4_8D67_4B06_B0A9_51A9D2D637D8_H_ */
