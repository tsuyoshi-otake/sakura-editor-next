/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpToolGrants.h"
#include "platform/profiles/UserDataProfileIdentity.h"

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <utility>

namespace senp {
namespace {

constexpr auto kKnownCapabilities = SenpToolCapability::GitHubRepositoryRead
	| SenpToolCapability::OpenConnectionUi;

bool HasCapability(const SenpToolCapability available, const SenpToolCapability required) noexcept
{
	const auto availableBits = static_cast<std::uint32_t>(available);
	const auto requiredBits = static_cast<std::uint32_t>(required);
	return requiredBits != 0 && (requiredBits & ~static_cast<std::uint32_t>(kKnownCapabilities)) == 0
		&& (availableBits & requiredBits) == requiredBits;
}

bool IsExtensionId(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 128 || value.front() == L'.' || value.back() == L'.') return false;
	return std::ranges::all_of(value, [](const wchar_t character) {
		return (character >= L'a' && character <= L'z') || (character >= L'0' && character <= L'9')
			|| character == L'-' || character == L'.';
	});
}

bool IsDigest(const std::wstring_view value) noexcept
{
	return value.size() == 64 && std::ranges::all_of(value, [](const wchar_t character) {
		return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
	});
}

bool IsConnection(const platform::controlipc::ControlIpcSessionContext& value) noexcept
{
	return value.sessionId != 0 && value.clientProcessId != 0;
}

bool IsRequest(const SenpToolGrantRequest& value) noexcept
{
	const auto& owner = value.Owner();
	return platform::profiles::IsOpaqueUserDataProfileId(value.ProfileId())
		&& IsExtensionId(owner.extensionId) && IsDigest(owner.packageDigest)
		&& owner.generation > 0 && owner.workspaceRevision >= 0 && owner.accountGeneration >= 0
		&& HasCapability(kKnownCapabilities, value.Capability());
}

bool SameConnection(const platform::controlipc::ControlIpcSessionContext& left,
	const platform::controlipc::ControlIpcSessionContext& right) noexcept
{
	return left.sessionId == right.sessionId && left.clientProcessId == right.clientProcessId;
}

bool SameRequest(const SenpToolGrantRequest& left, const SenpToolGrantRequest& right) noexcept
{
	return left.ProfileId() == right.ProfileId() && left.Owner() == right.Owner()
		&& left.Capability() == right.Capability();
}

bool MatchesAuthority(const SenpToolGrantRequest& request,
	const SenpApprovedToolOwner& authority) noexcept
{
	return authority.Enabled() && authority.ManagementRevision() != 0
		&& authority.ProfileId() == request.ProfileId()
		&& authority.ExtensionId() == request.Owner().extensionId
		&& authority.PackageDigest() == request.Owner().packageDigest
		&& HasCapability(authority.Capabilities(), request.Capability());
}

std::optional<std::string> RandomGrantId() noexcept
{
	std::array<std::uint8_t, 16> bytes{};
	if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return std::nullopt;
	constexpr char digits[] = "0123456789abcdef";
	std::string value(bytes.size() * 2, '0');
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		value[index * 2] = digits[bytes[index] >> 4];
		value[index * 2 + 1] = digits[bytes[index] & 0x0f];
	}
	return value;
}

} // namespace

SenpApprovedToolOwner::SenpApprovedToolOwner(std::wstring profileId, std::wstring extensionId,
	std::wstring packageDigest, const std::uint64_t managementRevision,
	const SenpToolCapability capabilities, const bool enabled) :
	m_profileId(std::move(profileId)), m_extensionId(std::move(extensionId)),
	m_packageDigest(std::move(packageDigest)), m_managementRevision(managementRevision),
	m_capabilities(capabilities), m_enabled(enabled) {}
const std::wstring& SenpApprovedToolOwner::ProfileId() const noexcept { return m_profileId; }
const std::wstring& SenpApprovedToolOwner::ExtensionId() const noexcept { return m_extensionId; }
const std::wstring& SenpApprovedToolOwner::PackageDigest() const noexcept { return m_packageDigest; }
std::uint64_t SenpApprovedToolOwner::ManagementRevision() const noexcept { return m_managementRevision; }
SenpToolCapability SenpApprovedToolOwner::Capabilities() const noexcept { return m_capabilities; }
bool SenpApprovedToolOwner::Enabled() const noexcept { return m_enabled; }

SenpToolGrantRequest::SenpToolGrantRequest(std::wstring profileId, ContributionOwnerIdentity owner,
	const SenpToolCapability capability) : m_profileId(std::move(profileId)),
	m_owner(std::move(owner)), m_capability(capability) {}
const std::wstring& SenpToolGrantRequest::ProfileId() const noexcept { return m_profileId; }
const ContributionOwnerIdentity& SenpToolGrantRequest::Owner() const noexcept { return m_owner; }
SenpToolCapability SenpToolGrantRequest::Capability() const noexcept { return m_capability; }

SenpToolGrantIssue::SenpToolGrantIssue(const SenpToolGrantIssueStatus status, std::string grantId,
	const std::chrono::steady_clock::time_point expiresAt) : m_status(status),
	m_grantId(std::move(grantId)), m_expiresAt(expiresAt) {}
SenpToolGrantIssueStatus SenpToolGrantIssue::Status() const noexcept { return m_status; }
const std::string& SenpToolGrantIssue::GrantId() const noexcept { return m_grantId; }
std::chrono::steady_clock::time_point SenpToolGrantIssue::ExpiresAt() const noexcept { return m_expiresAt; }

class CSenpToolGrants::Impl final {
public:
	explicit Impl(std::shared_ptr<const ISenpToolGrantAuthority> value) : authority(std::move(value)) {}
	[[nodiscard]] bool CanOpen() noexcept;
	[[nodiscard]] SenpToolGrantIssue Issue(
		const platform::controlipc::ControlIpcSessionContext& connection,
		const SenpToolGrantRequest& request, std::chrono::steady_clock::time_point now);
	[[nodiscard]] SenpToolGrantCheck Validate(std::string_view grantId,
		const platform::controlipc::ControlIpcSessionContext& connection,
		const SenpToolGrantRequest& request, std::chrono::steady_clock::time_point now);
	void RevokeConnection(const platform::controlipc::ControlIpcSessionContext& connection) noexcept;
	void RevokeOwner(std::wstring_view profileId, const ContributionOwnerIdentity& owner) noexcept;
	void RevokeProfile(std::wstring_view profileId) noexcept;
	void Close() noexcept;
	[[nodiscard]] std::size_t Size() noexcept;
	class Record final {
	public:
		Record(platform::controlipc::ControlIpcSessionContext connection,
			SenpToolGrantRequest request, const std::uint64_t managementRevision,
			const std::chrono::steady_clock::time_point expiresAt) :
			m_connection(connection), m_request(std::move(request)),
			m_managementRevision(managementRevision), m_expiresAt(expiresAt) {}
		[[nodiscard]] const platform::controlipc::ControlIpcSessionContext& Connection() const noexcept { return m_connection; }
		[[nodiscard]] const SenpToolGrantRequest& Request() const noexcept { return m_request; }
		[[nodiscard]] std::uint64_t ManagementRevision() const noexcept { return m_managementRevision; }
		[[nodiscard]] std::chrono::steady_clock::time_point ExpiresAt() const noexcept { return m_expiresAt; }
	private:
		platform::controlipc::ControlIpcSessionContext m_connection;
		SenpToolGrantRequest m_request;
		std::uint64_t m_managementRevision{};
		std::chrono::steady_clock::time_point m_expiresAt;
	};
private:
	std::shared_ptr<const ISenpToolGrantAuthority> authority;
	std::mutex mutex;
	std::map<std::string, Record, std::less<>> records;
	bool closed{};
};

CSenpToolGrants::CSenpToolGrants(std::shared_ptr<const ISenpToolGrantAuthority> authority) :
	m_impl(std::make_shared<Impl>(std::move(authority))) {}
CSenpToolGrants::~CSenpToolGrants() { Close(); }

std::unique_ptr<CSenpToolGrantSession> CSenpToolGrants::OpenSession(
	const platform::controlipc::ControlIpcSessionContext& connection)
{
	if (!IsConnection(connection) || !m_impl->CanOpen()) return nullptr;
	return std::make_unique<CSenpToolGrantSession>(
		CSenpToolGrantSession::ConstructionKey{}, m_impl, connection);
}

bool CSenpToolGrants::Impl::CanOpen() noexcept
{
	std::lock_guard lock(mutex);
	return !closed;
}

SenpToolGrantIssue CSenpToolGrants::Impl::Issue(
	const platform::controlipc::ControlIpcSessionContext& connection,
	const SenpToolGrantRequest& request, const std::chrono::steady_clock::time_point now)
{
	if (!authority) return { SenpToolGrantIssueStatus::Unavailable, {}, {} };
	if (!IsConnection(connection) || !IsRequest(request)) return { SenpToolGrantIssueStatus::InvalidRequest, {}, {} };
	std::optional<SenpApprovedToolOwner> approvedOwner;
	try {
		approvedOwner = authority->Resolve(request.ProfileId(), request.Owner().extensionId);
	} catch (...) {
		return { SenpToolGrantIssueStatus::Unavailable, {}, {} };
	}
	if (!approvedOwner || !MatchesAuthority(request, *approvedOwner)) {
		return { SenpToolGrantIssueStatus::Unauthorized, {}, {} };
	}
	const auto expiresAt = now + GrantLifetime();
	std::lock_guard lock(mutex);
	if (closed) return { SenpToolGrantIssueStatus::Closed, {}, {} };
	if (records.size() >= CSenpToolGrants::MaximumGrants()) {
		return { SenpToolGrantIssueStatus::ResourceExhausted, {}, {} };
	}
	for (int attempt = 0; attempt < 4; ++attempt) {
		const auto grantId = RandomGrantId();
		if (!grantId) return { SenpToolGrantIssueStatus::Unavailable, {}, {} };
		if (records.contains(*grantId)) continue;
		records.emplace(*grantId, Record{ connection, request,
			approvedOwner->ManagementRevision(), expiresAt });
		return { SenpToolGrantIssueStatus::Granted, *grantId, expiresAt };
	}
	return { SenpToolGrantIssueStatus::Unavailable, {}, {} };
}

SenpToolGrantCheck CSenpToolGrants::Impl::Validate(const std::string_view grantId,
	const platform::controlipc::ControlIpcSessionContext& connection,
	const SenpToolGrantRequest& request, const std::chrono::steady_clock::time_point now)
{
	if (!IsConnection(connection) || !IsRequest(request) || grantId.size() != 32) return SenpToolGrantCheck::Invalid;
	std::uint64_t recordedRevision = 0;
	{
		std::lock_guard lock(mutex);
		if (closed) return SenpToolGrantCheck::Closed;
		const auto found = records.find(grantId);
		if (found == records.end()) return SenpToolGrantCheck::Invalid;
		if (now >= found->second.ExpiresAt()) {
			records.erase(found);
			return SenpToolGrantCheck::Expired;
		}
		if (!SameConnection(found->second.Connection(), connection)) return SenpToolGrantCheck::WrongConnection;
		if (!SameRequest(found->second.Request(), request)) return SenpToolGrantCheck::WrongScope;
		recordedRevision = found->second.ManagementRevision();
	}
	if (!authority) return SenpToolGrantCheck::StaleAuthority;
	std::optional<SenpApprovedToolOwner> approvedOwner;
	try {
		approvedOwner = authority->Resolve(request.ProfileId(), request.Owner().extensionId);
	} catch (...) {
		return SenpToolGrantCheck::StaleAuthority;
	}
	if (!approvedOwner || !MatchesAuthority(request, *approvedOwner)
		|| approvedOwner->ManagementRevision() != recordedRevision) return SenpToolGrantCheck::StaleAuthority;
	std::lock_guard lock(mutex);
	if (closed) return SenpToolGrantCheck::Closed;
	const auto found = records.find(grantId);
	if (found == records.end()) return SenpToolGrantCheck::Invalid;
	if (now >= found->second.ExpiresAt()) {
		records.erase(found);
		return SenpToolGrantCheck::Expired;
	}
	if (!SameConnection(found->second.Connection(), connection)) return SenpToolGrantCheck::WrongConnection;
	if (!SameRequest(found->second.Request(), request)) return SenpToolGrantCheck::WrongScope;
	return SenpToolGrantCheck::Granted;
}

void CSenpToolGrants::Impl::RevokeConnection(
	const platform::controlipc::ControlIpcSessionContext& connection) noexcept
{
	std::lock_guard lock(mutex);
	std::erase_if(records, [&](const auto& entry) {
		return SameConnection(entry.second.Connection(), connection);
	});
}

void CSenpToolGrants::Impl::RevokeOwner(const std::wstring_view profileId,
	const ContributionOwnerIdentity& owner) noexcept
{
	std::lock_guard lock(mutex);
	std::erase_if(records, [&](const auto& entry) {
		return entry.second.Request().ProfileId() == profileId && entry.second.Request().Owner() == owner;
	});
}

void CSenpToolGrants::Impl::RevokeProfile(const std::wstring_view profileId) noexcept
{
	std::lock_guard lock(mutex);
	std::erase_if(records, [&](const auto& entry) {
		return entry.second.Request().ProfileId() == profileId;
	});
}

void CSenpToolGrants::Impl::Close() noexcept
{
	std::lock_guard lock(mutex);
	closed = true;
	records.clear();
}

std::size_t CSenpToolGrants::Impl::Size() noexcept
{
	std::lock_guard lock(mutex);
	return records.size();
}

void CSenpToolGrants::RevokeOwner(const std::wstring_view profileId,
	const ContributionOwnerIdentity& owner) noexcept { m_impl->RevokeOwner(profileId, owner); }
void CSenpToolGrants::RevokeProfile(const std::wstring_view profileId) noexcept { m_impl->RevokeProfile(profileId); }
void CSenpToolGrants::Close() noexcept { m_impl->Close(); }
std::size_t CSenpToolGrants::Size() noexcept { return m_impl->Size(); }

CSenpToolGrantSession::CSenpToolGrantSession(ConstructionKey,
	std::shared_ptr<CSenpToolGrants::Impl> grants,
	const platform::controlipc::ControlIpcSessionContext connection) noexcept :
	m_grants(std::move(grants)), m_connection(connection) {}

CSenpToolGrantSession::~CSenpToolGrantSession() { Close(); }

SenpToolGrantIssue CSenpToolGrantSession::Issue(const SenpToolGrantRequest& request,
	const std::chrono::steady_clock::time_point now)
{
	return m_grants ? m_grants->Issue(m_connection, request, now)
		: SenpToolGrantIssue(SenpToolGrantIssueStatus::Closed, {}, {});
}

SenpToolGrantCheck CSenpToolGrantSession::Validate(const std::string_view grantId,
	const SenpToolGrantRequest& request, const std::chrono::steady_clock::time_point now)
{
	return m_grants ? m_grants->Validate(grantId, m_connection, request, now)
		: SenpToolGrantCheck::Closed;
}

void CSenpToolGrantSession::Close() noexcept
{
	if (!m_grants) return;
	m_grants->RevokeConnection(m_connection);
	m_grants.reset();
}

} // namespace senp
