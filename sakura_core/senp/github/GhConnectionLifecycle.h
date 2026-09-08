/*! @file
 * @brief Generation-fenced GitHub CLI account connection lifecycle.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpToolGrants.h"
#include "senp/github/GhToolPolicy.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace senp::github {

enum class GhConnectionState : std::uint8_t {
	Unknown,
	Checking,
	Disconnected,
	Connected,
	ReauthenticationRequired,
	Unavailable,
};

enum class GhConnectionTerminal : std::uint8_t {
	Succeeded,
	AuthenticationRequired,
	IdentityMismatch,
	InvalidRequest,
	Busy,
	ToolUnavailable,
	UnsupportedVersion,
	Failed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
	Stale,
	Closed,
};

class GhAccountIdentity final {
public:
	GhAccountIdentity(std::wstring hostname, std::wstring login,
		std::wstring tokenSource, std::wstring configurationDirectory);
	[[nodiscard]] const std::wstring& Hostname() const noexcept { return m_hostname; }
	[[nodiscard]] const std::wstring& Login() const noexcept { return m_login; }
	[[nodiscard]] const std::wstring& TokenSource() const noexcept { return m_tokenSource; }
	[[nodiscard]] const std::wstring& ConfigurationDirectory() const noexcept { return m_configurationDirectory; }
private:
	std::wstring m_hostname, m_login, m_tokenSource, m_configurationDirectory;
};

class IGhAccountCredential {
public:
	virtual ~IGhAccountCredential() = default;
	//! Runs a native-host-owned fixed invocation with the adopted token. Callers
	//! must build argv through a closed policy; extensions never receive this object.
	[[nodiscard]] virtual GhProcessOutcome RunAuthenticated(
		const std::vector<std::wstring>& arguments, std::uint32_t timeoutMilliseconds,
		std::size_t maximumOutputBytes, std::size_t maximumErrorBytes, HANDLE stop) = 0;
	//! Streams a fixed invocation while the same private token lease remains valid.
	//! The observer receives process bytes only; it never receives the token.
	[[nodiscard]] virtual GhProcessOutcome RunAuthenticatedStreaming(
		const std::vector<std::wstring>& arguments, std::uint32_t timeoutMilliseconds,
		std::size_t maximumOutputBytes, std::size_t maximumErrorBytes,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> outputObserver,
		HANDLE stop) = 0;
	//! Stops future use of retained leases. In-flight work still reaches its own
	//! terminal and is rejected by the account/grant generation fence.
	virtual void Revoke() noexcept = 0;
};

class GhConnectionCheckResult final {
public:
	GhConnectionCheckResult(GhConnectionTerminal terminal,
		std::optional<GhAccountIdentity> identity,
		std::shared_ptr<IGhAccountCredential> credential);
	[[nodiscard]] GhConnectionTerminal Terminal() const noexcept { return m_terminal; }
	[[nodiscard]] const std::optional<GhAccountIdentity>& Identity() const noexcept { return m_identity; }
	[[nodiscard]] const std::shared_ptr<IGhAccountCredential>& Credential() const noexcept { return m_credential; }
private:
	GhConnectionTerminal m_terminal{ GhConnectionTerminal::InvalidRequest };
	std::optional<GhAccountIdentity> m_identity;
	std::shared_ptr<IGhAccountCredential> m_credential;
};

class IGhConnectionPlatform {
public:
	virtual ~IGhConnectionPlatform() = default;
	//! Enumerates accounts, selects one account, obtains its token through a
	//! private pipe, and verifies the token against the fixed `user` endpoint.
	[[nodiscard]] virtual GhConnectionCheckResult Check(
		const GhToolProbe& probe, std::wstring_view configurationDirectory,
		std::wstring_view hostname, std::optional<std::wstring_view> requestedLogin,
		HANDLE stop) = 0;
};

class CWindowsGhConnectionPlatform final : public IGhConnectionPlatform {
public:
	CWindowsGhConnectionPlatform(std::shared_ptr<const IGhToolPlatform> platform,
		std::wstring workingDirectory);
	[[nodiscard]] GhConnectionCheckResult Check(const GhToolProbe& probe,
		std::wstring_view configurationDirectory, std::wstring_view hostname,
		std::optional<std::wstring_view> requestedLogin, HANDLE stop) override;
private:
	std::shared_ptr<const IGhToolPlatform> m_platform;
	std::wstring m_workingDirectory;
};

class GhConnectionSnapshot final {
public:
	GhConnectionSnapshot(GhConnectionState state, std::wstring profileId,
		std::optional<GhAccountIdentity> account, std::int64_t accountGeneration);
	[[nodiscard]] GhConnectionState State() const noexcept { return m_state; }
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] const std::optional<GhAccountIdentity>& Account() const noexcept { return m_account; }
	[[nodiscard]] std::int64_t AccountGeneration() const noexcept { return m_accountGeneration; }
private:
	GhConnectionState m_state{ GhConnectionState::Unknown };
	std::wstring m_profileId;
	std::optional<GhAccountIdentity> m_account;
	std::int64_t m_accountGeneration{};
};

class CGhConnectionLifecycle;

class GhConnectionAttempt final {
public:
	GhConnectionAttempt(GhConnectionAttempt&&) noexcept = default;
	GhConnectionAttempt& operator=(GhConnectionAttempt&&) noexcept = default;
	GhConnectionAttempt(const GhConnectionAttempt&) = delete;
	GhConnectionAttempt& operator=(const GhConnectionAttempt&) = delete;
private:
	GhConnectionAttempt(const CGhConnectionLifecycle& owner, std::uint64_t operation,
		std::uint64_t epoch, std::wstring hostname, std::optional<std::wstring> requestedLogin);
	std::reference_wrapper<const CGhConnectionLifecycle> m_owner;
	std::uint64_t m_operation{}, m_epoch{};
	std::wstring m_hostname;
	std::optional<std::wstring> m_requestedLogin;
	friend class CGhConnectionLifecycle;
};

class GhConnectionOperationResult final {
public:
	GhConnectionOperationResult(GhConnectionTerminal terminal, GhConnectionSnapshot snapshot);
	[[nodiscard]] GhConnectionTerminal Terminal() const noexcept { return m_terminal; }
	[[nodiscard]] const GhConnectionSnapshot& Snapshot() const noexcept { return m_snapshot; }
private:
	GhConnectionTerminal m_terminal{ GhConnectionTerminal::InvalidRequest };
	GhConnectionSnapshot m_snapshot;
};

class GhAuthenticatedAccount final {
public:
	[[nodiscard]] const GhAccountIdentity& Identity() const noexcept { return m_identity; }
	[[nodiscard]] std::int64_t AccountGeneration() const noexcept { return m_accountGeneration; }
	[[nodiscard]] IGhAccountCredential& Credential() noexcept { return *m_credential; }
private:
	GhAuthenticatedAccount(GhAccountIdentity identity, std::int64_t accountGeneration,
		std::shared_ptr<IGhAccountCredential> credential);
	GhAccountIdentity m_identity;
	std::int64_t m_accountGeneration{};
	std::shared_ptr<IGhAccountCredential> m_credential;
	friend class CGhConnectionLifecycle;
};

//! One Control-process profile connection. Begin publishes Checking but retains
//! the usable account. Complete publishes a new account only after identity
//! verification and only while the attempt epoch is current.
class CGhConnectionLifecycle final {
public:
	CGhConnectionLifecycle(std::shared_ptr<IGhConnectionPlatform> platform,
		CSenpToolGrants& grants, std::wstring profileId, std::wstring configurationDirectory);
	~CGhConnectionLifecycle();
	CGhConnectionLifecycle(const CGhConnectionLifecycle&) = delete;
	CGhConnectionLifecycle& operator=(const CGhConnectionLifecycle&) = delete;
	[[nodiscard]] std::optional<GhConnectionAttempt> Begin(
		std::wstring hostname, std::optional<std::wstring> requestedLogin);
	[[nodiscard]] GhConnectionOperationResult Complete(
		GhConnectionAttempt attempt, const GhToolProbe& probe, HANDLE stop);
	[[nodiscard]] GhConnectionSnapshot Snapshot();
	[[nodiscard]] std::optional<GhAuthenticatedAccount> Acquire(
		std::int64_t expectedAccountGeneration);
	void Disconnect() noexcept;
	void Close() noexcept;
private:
	[[nodiscard]] GhConnectionSnapshot SnapshotLocked() const;
	std::shared_ptr<IGhConnectionPlatform> m_platform;
	CSenpToolGrants& m_grants;
	std::wstring m_profileId, m_configurationDirectory;
	std::mutex m_mutex;
	GhConnectionState m_state{ GhConnectionState::Unknown };
	std::optional<GhAccountIdentity> m_account;
	std::shared_ptr<IGhAccountCredential> m_credential;
	std::int64_t m_accountGeneration{};
	std::int64_t m_nextAccountGeneration{ 1 };
	std::uint64_t m_epoch{ 1 }, m_nextOperation{ 1 }, m_activeOperation{};
	bool m_closed{};
};

} // namespace senp::github
