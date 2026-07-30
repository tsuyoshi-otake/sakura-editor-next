/*! @file
	@brief プロファイル単位の Node.js 拡張ホスト所有ブローカー
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONHOSTBROKER_C6606C45_B526_442B_87AF_0C195E77DAE0_H_
#define SAKURA_CEXTENSIONHOSTBROKER_C6606C45_B526_442B_87AF_0C195E77DAE0_H_
#pragma once

#include "extension/CExtensionHostProcess.h"
#include "extension/CExtensionHostStateMachine.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

struct SExtensionHostBrokerConfig {
	std::filesystem::path nodeExecutable;
	std::filesystem::path hostBundle;
	std::filesystem::path securityShim;
	std::filesystem::path workingDirectory;
	std::filesystem::path profileDirectory;
	std::wstring bootIdOverride;
	std::uint32_t brokerProcessId = 0;
	bool developerInspect = false;
	SExtensionHostLifecycleConfig lifecycle;
};

struct SExtensionHostBrokerSnapshot {
	EExtensionHostState state = EExtensionHostState::Absent;
	std::uint64_t generation = 0;
	std::uint32_t hostProcessId = 0;
	std::uint32_t retryCount = 0;
	std::size_t leaseOwnerCount = 0;
	std::size_t leaseCount = 0;
	std::wstring profileHash;
	std::wstring bootId;
	std::wstring pipeName;
	std::string lastDiagnostic;
};

class IExtensionHostBrokerObserver {
public:
	virtual ~IExtensionHostBrokerObserver() = default;
	virtual void OnExtensionHostLifecycleAction(
		const SExtensionHostLifecycleAction& action,
		const SExtensionHostBrokerSnapshot& snapshot) noexcept = 0;
};

class CExtensionHostBroker final {
public:
	using Clock = CExtensionHostStateMachine::Clock;
	using TimePoint = CExtensionHostStateMachine::TimePoint;

	explicit CExtensionHostBroker(
		SExtensionHostBrokerConfig config,
		std::unique_ptr<IExtensionHostProcess> process = std::make_unique<CExtensionHostProcess>(),
		IExtensionHostBrokerObserver* observer = nullptr);
	~CExtensionHostBroker();
	CExtensionHostBroker(const CExtensionHostBroker&) = delete;
	CExtensionHostBroker& operator=(const CExtensionHostBroker&) = delete;

	void AcquireLease(std::uint32_t editorProcessId, TimePoint now = Clock::now(), double jitterUnit = 0.5);
	void ReleaseLease(std::uint32_t editorProcessId, TimePoint now = Clock::now(), double jitterUnit = 0.5);
	bool AcceptHandshake(
		std::uint64_t generation,
		std::uint32_t serverProcessId,
		std::wstring_view bootId,
		TimePoint now = Clock::now(),
		double jitterUnit = 0.5);
	void NotifyHostLost(
		std::uint64_t generation,
		EExtensionHostLossKind kind,
		std::string diagnostic,
		TimePoint now = Clock::now(),
		double jitterUnit = 0.5);
	void NotifyQuiesceCompleted(
		std::uint64_t generation,
		TimePoint now = Clock::now(),
		double jitterUnit = 0.5);
	void Tick(TimePoint now = Clock::now(), double jitterUnit = 0.5);
	void Shutdown(TimePoint now = Clock::now(), double jitterUnit = 0.5);

	SExtensionHostBrokerSnapshot GetSnapshot() const;
	static std::wstring ComputeProfileHash(const std::filesystem::path& profileDirectory);
	static std::wstring GenerateBootId();

private:
	void Dispatch(
		CExtensionHostStateMachine::Actions actions,
		TimePoint now,
		double jitterUnit);
	void ExecuteStart(
		const SExtensionHostLifecycleAction& action,
		TimePoint now,
		double jitterUnit);
	void NotifyObserver(const SExtensionHostLifecycleAction& action) noexcept;
	static std::string NarrowDiagnostic(std::wstring_view value);

	SExtensionHostBrokerConfig m_config;
	CExtensionHostStateMachine m_stateMachine;
	std::unique_ptr<IExtensionHostProcess> m_process;
	IExtensionHostBrokerObserver* m_observer = nullptr;
	std::wstring m_profileHash;
	std::wstring m_bootId;
	std::wstring m_pipeName;
	std::uint64_t m_identityGeneration = 0;
	std::string m_lastDiagnostic;
	std::deque<SExtensionHostLifecycleAction> m_pendingActions;
	bool m_dispatching = false;
};

#endif /* SAKURA_CEXTENSIONHOSTBROKER_C6606C45_B526_442B_87AF_0C195E77DAE0_H_ */
