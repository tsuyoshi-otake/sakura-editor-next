/*! @file
 * @brief Bounded GitHub CLI web-login presentation and lifecycle coordinator.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/github/GhConnectionLifecycle.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace senp::github {

class LoginOutputObserver;

enum class GhLoginPhase : std::uint8_t { Idle, SigningIn, Succeeded, Failed, Cancelled, Disconnected, Closed };

enum class GhLoginTerminal : std::uint8_t {
	Succeeded,
	InvalidRequest,
	ToolUnavailable,
	UnsupportedVersion,
	Failed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
	UnsupportedInteractiveFlow,
	Stale,
	Closed,
};

class GhLoginSnapshot final {
public:
	GhLoginSnapshot(GhLoginPhase phase, std::optional<GhLoginTerminal> terminal,
		std::string outputUtf8, std::wstring storageNotice, std::wstring sharedAuthenticationNotice);
	[[nodiscard]] GhLoginPhase Phase() const noexcept { return m_phase; }
	[[nodiscard]] const std::optional<GhLoginTerminal>& Terminal() const noexcept { return m_terminal; }
	[[nodiscard]] const std::string& OutputUtf8() const noexcept { return m_outputUtf8; }
	[[nodiscard]] const std::wstring& StorageNotice() const noexcept { return m_storageNotice; }
	[[nodiscard]] const std::wstring& SharedAuthenticationNotice() const noexcept
	{
		return m_sharedAuthenticationNotice;
	}
private:
	GhLoginPhase m_phase{ GhLoginPhase::Idle };
	std::optional<GhLoginTerminal> m_terminal;
	std::string m_outputUtf8;
	std::wstring m_storageNotice, m_sharedAuthenticationNotice;
};

//! Owns one bounded interactive operation. The fixed gh invocation can display
//! device-flow output, but success is published only after lifecycle identity verification.
class CGhLoginSession final {
public:
	CGhLoginSession(std::shared_ptr<const IGhToolPlatform> platform,
		CGhConnectionLifecycle& lifecycle, std::wstring workingDirectory,
		std::wstring configurationDirectory);
	~CGhLoginSession();
	CGhLoginSession(const CGhLoginSession&) = delete;
	CGhLoginSession& operator=(const CGhLoginSession&) = delete;
	[[nodiscard]] GhLoginTerminal Run(const GhToolProbe& probe,
		std::wstring hostname, HANDLE stop);
	[[nodiscard]] GhLoginSnapshot Snapshot();
	void Disconnect() noexcept;
	void Close() noexcept;
private:
	class State;
	friend class LoginOutputObserver;
	std::shared_ptr<const IGhToolPlatform> m_platform;
	CGhConnectionLifecycle& m_lifecycle;
	std::wstring m_workingDirectory, m_configurationDirectory;
	std::shared_ptr<State> m_state;
};

} // namespace senp::github
