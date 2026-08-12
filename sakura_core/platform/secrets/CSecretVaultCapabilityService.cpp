/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/CSecretVaultCapabilityService.h"

#include <sakura/controlipc/ProfileAuthorityIdentity.h>

#include <algorithm>
#include <bcrypt.h>
#include <limits>
#include <utility>

namespace platform::secrets {
namespace {

// Token and digest are intentionally the same fixed-width byte-array type.
// One erasure routine therefore covers both without an invalid overload pair.
void SecureClear(SecretVaultCapabilityToken& bytes) noexcept
{
	volatile std::uint8_t* cursor = bytes.data();
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		cursor[index] = 0;
	}
}

[[nodiscard]] bool IsExactCanonicalExtensionId(std::string_view extensionId) noexcept
{
	if (extensionId.empty() || extensionId.size() > kMaximumSecretVaultExtensionIdBytes) {
		return false;
	}
	bool hasPublisherSeparator = false;
	char previous = '\0';
	for (const unsigned char character : extensionId) {
		const bool alphaNumeric = (character >= 'a' && character <= 'z')
			|| (character >= '0' && character <= '9');
		const bool publisherSeparator = character == '.';
		const bool hyphen = character == '-';
		if (!alphaNumeric && !publisherSeparator && !hyphen) {
			return false;
		}
		if (hyphen && (previous == '\0' || previous == '.')) {
			return false;
		}
		if (publisherSeparator && (hasPublisherSeparator || previous == '\0' || previous == '-')) {
			return false;
		}
		if (publisherSeparator) {
			hasPublisherSeparator = true;
		}
		previous = static_cast<char>(character);
	}
	return hasPublisherSeparator && previous != '-' && previous != '.';
}

[[nodiscard]] bool IsValidCapabilityAddress(const SecretAddress& address) noexcept
{
	return IsExactCanonicalExtensionId(address.extensionId)
		&& IsValidSecretVaultIdentifier(address.key, kMaximumSecretVaultKeyBytes);
}

} // namespace

bool SecretVaultCapabilitySessionIdentity::IsValid() const noexcept
{
	return profiles::IsCanonicalProfileAuthorityId(profileId)
		&& IsValidSecretVaultIdentifier(extensionHostSessionId,
			kMaximumSecretVaultCapabilitySessionIdBytes)
		&& clientProcessId != 0
		&& connectionGeneration != 0;
}

bool SecretVaultCapabilityHostSessionIdentity::IsValid() const noexcept
{
	return profiles::IsCanonicalProfileAuthorityId(profileId)
		&& IsValidSecretVaultIdentifier(extensionHostSessionId,
			kMaximumSecretVaultCapabilitySessionIdBytes)
		&& connectionGeneration != 0;
}

bool SecretVaultCapabilityBinding::IsValid() const noexcept
{
	return session.IsValid()
		&& IsExactCanonicalExtensionId(extensionId);
}

bool CSystemSecretVaultCapabilityTokenSource::Fill(SecretVaultCapabilityToken& token) noexcept
{
	return ::BCryptGenRandom(nullptr, token.data(), static_cast<ULONG>(token.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

std::chrono::steady_clock::time_point CSteadySecretVaultCapabilityClock::Now() const noexcept
{
	return std::chrono::steady_clock::now();
}

bool SecretVaultCapabilityServiceConfig::IsValid() const noexcept
{
	return maximumGrants != 0 && maximumGrants <= kMaximumSecretVaultCapabilityGrants
		&& maximumLifetime.count() > 0 && maximumLifetime <= kMaximumSecretVaultCapabilityLifetime;
}

CSecretVaultCapabilityService::CSecretVaultCapabilityService(
	std::string canonicalProfileId,
	SecretVaultCapabilityServiceConfig config)
	: CSecretVaultCapabilityService(std::move(canonicalProfileId),
		std::make_shared<CSystemSecretVaultCapabilityTokenSource>(),
		std::make_shared<CSteadySecretVaultCapabilityClock>(), config)
{
}

CSecretVaultCapabilityService::CSecretVaultCapabilityService(
	std::string canonicalProfileId,
	std::shared_ptr<ISecretVaultCapabilityTokenSource> tokenSource,
	std::shared_ptr<ISecretVaultCapabilityClock> clock,
	SecretVaultCapabilityServiceConfig config)
	: m_profileId(std::move(canonicalProfileId))
	, m_tokenSource(std::move(tokenSource))
	, m_clock(std::move(clock))
	, m_config(config)
	, m_configurationValid(profiles::IsCanonicalProfileAuthorityId(m_profileId)
		&& m_tokenSource != nullptr && m_clock != nullptr && m_config.IsValid())
{
	if (m_configurationValid) {
		try {
			m_grants.reserve(m_config.maximumGrants);
		} catch (...) {
			m_configurationValid = false;
		}
	}
}

CSecretVaultCapabilityService::~CSecretVaultCapabilityService()
{
	(void)Stop();
}

std::string_view CSecretVaultCapabilityService::GetProfileId() const noexcept
{
	return m_profileId;
}

void CSecretVaultCapabilityService::PruneExpiredLocked(std::chrono::steady_clock::time_point now) noexcept
{
	std::size_t destination = 0;
	for (std::size_t source = 0; source < m_grants.size(); ++source) {
		if (now >= m_grants[source].expiresAt) {
			SecureClear(m_grants[source].digest);
			continue;
		}
		if (destination != source) {
			m_grants[destination] = std::move(m_grants[source]);
			// std::array move-assignment copies bytes. Wipe the source slot so
			// vector compaction never leaves a second usable digest behind.
			SecureClear(m_grants[source].digest);
		}
		++destination;
	}
	for (std::size_t index = destination; index < m_grants.size(); ++index) {
		SecureClear(m_grants[index].digest);
	}
	m_grants.resize(destination);
}

bool CSecretVaultCapabilityService::ConstantTimeDigestEqual(
	const SecretVaultCapabilityDigest& left,
	const SecretVaultCapabilityDigest& right) noexcept
{
	volatile std::uint8_t difference = 0;
	for (std::size_t index = 0; index < left.size(); ++index) {
		difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
	}
	return difference == 0;
}

bool CSecretVaultCapabilityService::ContainsDigestLocked(const SecretVaultCapabilityDigest& digest) const noexcept
{
	bool found = false;
	// Keep scanning after a match: a supplied token must never gain an early-exit
	// timing distinction from a digest held by this process-local authority.
	for (const Grant& grant : m_grants) {
		found = ConstantTimeDigestEqual(grant.digest, digest) || found;
	}
	return found;
}

bool CSecretVaultCapabilityService::ComputeDigest(
	const SecretVaultCapabilityToken& token,
	SecretVaultCapabilityDigest& digest) noexcept
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	const NTSTATUS openStatus = ::BCryptOpenAlgorithmProvider(
		&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (openStatus < 0 || algorithm == nullptr) {
		return false;
	}
	const NTSTATUS hashStatus = ::BCryptHash(algorithm, nullptr, 0,
		reinterpret_cast<PUCHAR>(const_cast<std::uint8_t*>(token.data())),
		static_cast<ULONG>(token.size()), digest.data(), static_cast<ULONG>(digest.size()));
	::BCryptCloseAlgorithmProvider(algorithm, 0);
	if (hashStatus < 0) {
		SecureClear(digest);
		return false;
	}
	return true;
}

SecretVaultCapabilityIssueResult CSecretVaultCapabilityService::Issue(
	const SecretVaultCapabilityIssueRequest& request)
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretVaultCapabilityIssueStatus::Stopped };
	}
	if (!m_configurationValid || !m_clock || !m_tokenSource) {
		return { .status = ESecretVaultCapabilityIssueStatus::InvalidConfiguration };
	}
	if (!request.binding.IsValid() || request.binding.session.profileId != m_profileId) {
		return { .status = ESecretVaultCapabilityIssueStatus::InvalidBinding };
	}
	if (request.lifetime.count() <= 0 || request.lifetime > m_config.maximumLifetime) {
		return { .status = ESecretVaultCapabilityIssueStatus::InvalidLifetime };
	}

	const auto now = m_clock->Now();
	const auto lifetime = std::chrono::duration_cast<std::chrono::steady_clock::duration>(request.lifetime);
	if (lifetime <= std::chrono::steady_clock::duration::zero()
		|| now > std::chrono::steady_clock::time_point::max() - lifetime) {
		return { .status = ESecretVaultCapabilityIssueStatus::InvalidLifetime };
	}
	PruneExpiredLocked(now);
	if (m_grants.size() >= m_config.maximumGrants) {
		return { .status = ESecretVaultCapabilityIssueStatus::CapacityReached };
	}

	SecretVaultCapabilityToken token{};
	if (!m_tokenSource->Fill(token)) {
		SecureClear(token);
		return { .status = ESecretVaultCapabilityIssueStatus::EntropyFailure };
	}
	SecretVaultCapabilityDigest digest{};
	if (!ComputeDigest(token, digest)) {
		SecureClear(token);
		return { .status = ESecretVaultCapabilityIssueStatus::DigestFailure };
	}
	if (ContainsDigestLocked(digest)) {
		SecureClear(digest);
		SecureClear(token);
		return { .status = ESecretVaultCapabilityIssueStatus::Collision };
	}

	const auto expiresAt = now + lifetime;
	try {
		m_grants.push_back(Grant{ .digest = digest, .binding = request.binding, .expiresAt = expiresAt });
	} catch (...) {
		SecureClear(digest);
		SecureClear(token);
		return { .status = ESecretVaultCapabilityIssueStatus::StorageFailure };
	}
	SecureClear(digest);
	SecretVaultCapabilityIssueResult result{
		.status = ESecretVaultCapabilityIssueStatus::Issued,
		.capability = token,
		.expiresAt = expiresAt,
	};
	SecureClear(token);
	return result;
}

SecretVaultCapabilityValidationResult CSecretVaultCapabilityService::Validate(
	const SecretVaultCapabilityValidationRequest& request)
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretVaultCapabilityValidationStatus::Stopped };
	}
	if (!m_configurationValid || !m_clock) {
		return { .status = ESecretVaultCapabilityValidationStatus::InvalidConfiguration };
	}
	if (!request.session.IsValid()) {
		return { .status = ESecretVaultCapabilityValidationStatus::InvalidSession };
	}
	if (!IsValidCapabilityAddress(request.address)) {
		return { .status = ESecretVaultCapabilityValidationStatus::InvalidAddress };
	}

	SecretVaultCapabilityDigest digest{};
	if (!ComputeDigest(request.capability, digest)) {
		return { .status = ESecretVaultCapabilityValidationStatus::DigestFailure };
	}
	std::optional<std::size_t> matchedIndex;
	for (std::size_t index = 0; index < m_grants.size(); ++index) {
		if (ConstantTimeDigestEqual(m_grants[index].digest, digest)) {
			matchedIndex = index;
		}
	}
	SecureClear(digest);
	const auto now = m_clock->Now();
	if (!matchedIndex.has_value()) {
		PruneExpiredLocked(now);
		return { .status = ESecretVaultCapabilityValidationStatus::CapabilityMismatch };
	}

	const Grant& grant = m_grants[*matchedIndex];
	if (now >= grant.expiresAt) {
		SecureClear(m_grants[*matchedIndex].digest);
		m_grants.erase(m_grants.begin() + static_cast<std::ptrdiff_t>(*matchedIndex));
		PruneExpiredLocked(now);
		return { .status = ESecretVaultCapabilityValidationStatus::Expired };
	}
	ESecretVaultCapabilityValidationStatus status = ESecretVaultCapabilityValidationStatus::Valid;
	if (request.session.profileId != grant.binding.session.profileId) {
		status = ESecretVaultCapabilityValidationStatus::ProfileMismatch;
	} else if (request.session.extensionHostSessionId != grant.binding.session.extensionHostSessionId) {
		status = ESecretVaultCapabilityValidationStatus::SessionMismatch;
	} else if (request.session.clientProcessId != grant.binding.session.clientProcessId) {
		status = ESecretVaultCapabilityValidationStatus::ClientProcessMismatch;
	} else if (request.session.connectionGeneration != grant.binding.session.connectionGeneration) {
		status = ESecretVaultCapabilityValidationStatus::ConnectionGenerationMismatch;
	} else if (request.address.extensionId != grant.binding.extensionId) {
		status = ESecretVaultCapabilityValidationStatus::ExtensionMismatch;
	}
	PruneExpiredLocked(now);
	return { .status = status };
}

SecretVaultCapabilityRevokeResult CSecretVaultCapabilityService::RevokeExtension(
	const SecretVaultCapabilityBinding& binding)
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretVaultCapabilityRevokeStatus::Stopped };
	}
	if (!m_configurationValid || !m_clock) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidConfiguration };
	}
	if (!binding.IsValid() || binding.session.profileId != m_profileId) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidBinding };
	}
	PruneExpiredLocked(m_clock->Now());
	std::size_t revoked = 0;
	std::size_t destination = 0;
	for (std::size_t source = 0; source < m_grants.size(); ++source) {
		if (m_grants[source].binding == binding) {
			++revoked;
			SecureClear(m_grants[source].digest);
			continue;
		}
		if (destination != source) {
			m_grants[destination] = std::move(m_grants[source]);
			SecureClear(m_grants[source].digest);
		}
		++destination;
	}
	for (std::size_t index = destination; index < m_grants.size(); ++index) {
		SecureClear(m_grants[index].digest);
	}
	m_grants.resize(destination);
	return revoked == 0
		? SecretVaultCapabilityRevokeResult{ .status = ESecretVaultCapabilityRevokeStatus::NotFound }
		: SecretVaultCapabilityRevokeResult{ .status = ESecretVaultCapabilityRevokeStatus::Revoked, .revokedGrantCount = revoked };
}

SecretVaultCapabilityRevokeResult CSecretVaultCapabilityService::RevokeSession(
	const SecretVaultCapabilitySessionIdentity& session)
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretVaultCapabilityRevokeStatus::Stopped };
	}
	if (!m_configurationValid || !m_clock) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidConfiguration };
	}
	if (!session.IsValid() || session.profileId != m_profileId) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidSession };
	}
	PruneExpiredLocked(m_clock->Now());
	std::size_t revoked = 0;
	std::size_t destination = 0;
	for (std::size_t source = 0; source < m_grants.size(); ++source) {
		if (m_grants[source].binding.session == session) {
			++revoked;
			SecureClear(m_grants[source].digest);
			continue;
		}
		if (destination != source) {
			m_grants[destination] = std::move(m_grants[source]);
			SecureClear(m_grants[source].digest);
		}
		++destination;
	}
	for (std::size_t index = destination; index < m_grants.size(); ++index) {
		SecureClear(m_grants[index].digest);
	}
	m_grants.resize(destination);
	return revoked == 0
		? SecretVaultCapabilityRevokeResult{ .status = ESecretVaultCapabilityRevokeStatus::NotFound }
		: SecretVaultCapabilityRevokeResult{ .status = ESecretVaultCapabilityRevokeStatus::Revoked, .revokedGrantCount = revoked };
}

SecretVaultCapabilityRevokeResult CSecretVaultCapabilityService::RevokeHostSession(
	const SecretVaultCapabilityHostSessionIdentity& session)
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretVaultCapabilityRevokeStatus::Stopped };
	}
	if (!m_configurationValid || !m_clock) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidConfiguration };
	}
	if (!session.IsValid() || session.profileId != m_profileId) {
		return { .status = ESecretVaultCapabilityRevokeStatus::InvalidSession };
	}
	PruneExpiredLocked(m_clock->Now());
	std::size_t revoked = 0;
	std::size_t destination = 0;
	for (std::size_t source = 0; source < m_grants.size(); ++source) {
		const auto& candidate = m_grants[source].binding.session;
		if (candidate.profileId == session.profileId
			&& candidate.extensionHostSessionId == session.extensionHostSessionId
			&& candidate.connectionGeneration == session.connectionGeneration) {
			++revoked;
			SecureClear(m_grants[source].digest);
			continue;
		}
		if (destination != source) {
			m_grants[destination] = std::move(m_grants[source]);
			SecureClear(m_grants[source].digest);
		}
		++destination;
	}
	for (std::size_t index = destination; index < m_grants.size(); ++index) {
		SecureClear(m_grants[index].digest);
	}
	m_grants.resize(destination);
	return revoked == 0
		? SecretVaultCapabilityRevokeResult{ .status = ESecretVaultCapabilityRevokeStatus::NotFound }
		: SecretVaultCapabilityRevokeResult{
			.status = ESecretVaultCapabilityRevokeStatus::Revoked,
			.revokedGrantCount = revoked,
		};
}

ESecretVaultCapabilityStopStatus CSecretVaultCapabilityService::Stop() noexcept
{
	try {
		std::scoped_lock lock(m_mutex);
		if (m_stopped) {
			return ESecretVaultCapabilityStopStatus::AlreadyStopped;
		}
		m_stopped = true;
		for (Grant& grant : m_grants) {
			SecureClear(grant.digest);
		}
		m_grants.clear();
		return ESecretVaultCapabilityStopStatus::Stopped;
	} catch (...) {
		m_stopped = true;
		return ESecretVaultCapabilityStopStatus::Stopped;
	}
}

} // namespace platform::secrets
