/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpContributionOwners.h"
#include <sakura/controlipc/ControlIpcTransport.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace senp {

enum class SenpToolCapability : std::uint32_t {
	None = 0,
	GitHubRepositoryRead = 1U << 0,
	OpenConnectionUi = 1U << 1,
};

[[nodiscard]] constexpr SenpToolCapability operator|(SenpToolCapability left,
	SenpToolCapability right) noexcept
{
	return static_cast<SenpToolCapability>(static_cast<std::uint32_t>(left)
		| static_cast<std::uint32_t>(right));
}

//! Immutable result from the Control process's trusted package authority.
//! An Editor request can select profile/extension, but cannot construct the
//! authority used by CSenpToolGrants.
class SenpApprovedToolOwner final {
public:
	SenpApprovedToolOwner(std::wstring profileId, std::wstring extensionId,
		std::wstring packageDigest, std::uint64_t managementRevision,
		SenpToolCapability capabilities, bool enabled);
	[[nodiscard]] const std::wstring& ProfileId() const noexcept;
	[[nodiscard]] const std::wstring& ExtensionId() const noexcept;
	[[nodiscard]] const std::wstring& PackageDigest() const noexcept;
	[[nodiscard]] std::uint64_t ManagementRevision() const noexcept;
	[[nodiscard]] SenpToolCapability Capabilities() const noexcept;
	[[nodiscard]] bool Enabled() const noexcept;
private:
	std::wstring m_profileId, m_extensionId, m_packageDigest;
	std::uint64_t m_managementRevision{};
	SenpToolCapability m_capabilities{ SenpToolCapability::None };
	bool m_enabled{};
};

class ISenpToolGrantAuthority {
public:
	virtual ~ISenpToolGrantAuthority() = default;
	//! Resolves only Control-owned installed state. Request payload fields must
	//! never be copied into the returned authority without independent proof.
	[[nodiscard]] virtual std::optional<SenpApprovedToolOwner> Resolve(
		std::wstring_view profileId, std::wstring_view extensionId) const = 0;
};

class SenpToolGrantRequest final {
public:
	SenpToolGrantRequest(std::wstring profileId, ContributionOwnerIdentity owner,
		SenpToolCapability capability);
	[[nodiscard]] const std::wstring& ProfileId() const noexcept;
	[[nodiscard]] const ContributionOwnerIdentity& Owner() const noexcept;
	[[nodiscard]] SenpToolCapability Capability() const noexcept;
private:
	std::wstring m_profileId;
	ContributionOwnerIdentity m_owner;
	SenpToolCapability m_capability{ SenpToolCapability::None };
};

enum class SenpToolGrantIssueStatus : std::uint8_t {
	Granted,
	InvalidRequest,
	Unauthorized,
	ResourceExhausted,
	Unavailable,
	Closed,
};

class SenpToolGrantIssue final {
public:
	SenpToolGrantIssue(SenpToolGrantIssueStatus status, std::string grantId,
		std::chrono::steady_clock::time_point expiresAt);
	[[nodiscard]] SenpToolGrantIssueStatus Status() const noexcept;
	[[nodiscard]] const std::string& GrantId() const noexcept;
	[[nodiscard]] std::chrono::steady_clock::time_point ExpiresAt() const noexcept;
private:
	SenpToolGrantIssueStatus m_status{ SenpToolGrantIssueStatus::InvalidRequest };
	std::string m_grantId;
	std::chrono::steady_clock::time_point m_expiresAt{};
};

enum class SenpToolGrantCheck : std::uint8_t {
	Granted,
	Invalid,
	Expired,
	WrongConnection,
	WrongScope,
	StaleAuthority,
	Closed,
};

class CSenpToolGrantSession;

//! Control-owned, connection-bound grant registry. Opaque IDs are record
//! identities rather than bearer authorization: every use rechecks the full
//! connection, owner scope and current trusted package authority.
class CSenpToolGrants final {
public:
	[[nodiscard]] static constexpr std::size_t MaximumGrants() noexcept { return 64; }
	[[nodiscard]] static constexpr std::chrono::minutes GrantLifetime() noexcept {
		return std::chrono::minutes(5);
	}
	explicit CSenpToolGrants(std::shared_ptr<const ISenpToolGrantAuthority> authority);
	~CSenpToolGrants();
	CSenpToolGrants(const CSenpToolGrants&) = delete;
	CSenpToolGrants& operator=(const CSenpToolGrants&) = delete;
	//! The returned object owns connection revocation and must be retained by the
	//! named-pipe session handler for exactly that handler's lifetime.
	[[nodiscard]] std::unique_ptr<CSenpToolGrantSession> OpenSession(
		const platform::controlipc::ControlIpcSessionContext& connection);
	void RevokeOwner(std::wstring_view profileId, const ContributionOwnerIdentity& owner) noexcept;
	void RevokeProfile(std::wstring_view profileId) noexcept;
	void Close() noexcept;
	[[nodiscard]] std::size_t Size() noexcept;
private:
	friend class CSenpToolGrantSession;
	class Impl;
	std::shared_ptr<Impl> m_impl;
};

//! One OS-observed control IPC connection. Destruction always revokes all grants
//! minted through this connection before its handler releases the peer.
class CSenpToolGrantSession final {
public:
	class ConstructionKey final {
	private:
		friend class CSenpToolGrants;
		ConstructionKey() = default;
	};
	CSenpToolGrantSession(ConstructionKey, std::shared_ptr<CSenpToolGrants::Impl> grants,
		platform::controlipc::ControlIpcSessionContext connection) noexcept;
	~CSenpToolGrantSession();
	CSenpToolGrantSession(const CSenpToolGrantSession&) = delete;
	CSenpToolGrantSession& operator=(const CSenpToolGrantSession&) = delete;
	[[nodiscard]] SenpToolGrantIssue Issue(const SenpToolGrantRequest& request,
		std::chrono::steady_clock::time_point now);
	[[nodiscard]] SenpToolGrantCheck Validate(std::string_view grantId,
		const SenpToolGrantRequest& request, std::chrono::steady_clock::time_point now);
	void Close() noexcept;
private:
	std::shared_ptr<CSenpToolGrants::Impl> m_grants;
	platform::controlipc::ControlIpcSessionContext m_connection;
};

} // namespace senp
