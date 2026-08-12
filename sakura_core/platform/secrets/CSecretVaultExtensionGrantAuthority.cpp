/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"

#include <sakura/controlipc/ProfileAuthorityIdentity.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace platform::secrets {
namespace {

[[nodiscard]] bool IsExactCanonicalExtensionId(std::string_view extensionId) noexcept
{
	if (extensionId.empty() || extensionId.size() > kMaximumSecretVaultExtensionIdBytes) return false;
	bool hasPublisherSeparator = false;
	char previous = '\0';
	for (const unsigned char character : extensionId) {
		const bool alphaNumeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
		const bool publisherSeparator = character == '.';
		const bool hyphen = character == '-';
		if (!alphaNumeric && !publisherSeparator && !hyphen) return false;
		if (hyphen && (previous == '\0' || previous == '.')) return false;
		if (publisherSeparator && (hasPublisherSeparator || previous == '\0' || previous == '-')) return false;
		hasPublisherSeparator = hasPublisherSeparator || publisherSeparator;
		previous = static_cast<char>(character);
	}
	return hasPublisherSeparator && previous != '-' && previous != '.';
}

[[nodiscard]] bool HasNoDuplicates(const std::vector<std::string>& values) noexcept
{
	for (std::size_t left = 0; left < values.size(); ++left) {
		if (!IsExactCanonicalExtensionId(values[left])) return false;
		for (std::size_t right = left + 1; right < values.size(); ++right) {
			if (values[left] == values[right]) return false;
		}
	}
	return true;
}

[[nodiscard]] bool HasNoDuplicates(const std::vector<std::uint32_t>& values) noexcept
{
	for (std::size_t left = 0; left < values.size(); ++left) {
		if (values[left] == 0) return false;
		for (std::size_t right = left + 1; right < values.size(); ++right) {
			if (values[left] == values[right]) return false;
		}
	}
	return true;
}

template<typename T>
[[nodiscard]] bool Contains(const std::vector<T>& values, const T& value) noexcept
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

bool SecretVaultExtensionGrantAuthorityConfig::IsValid() const noexcept
{
	return maximumEditorLeases != 0 && maximumEditorLeases <= kMaximumSecretVaultExtensionGrantEditorLeases
		&& maximumApprovedExtensions != 0 && maximumApprovedExtensions <= kMaximumSecretVaultExtensionGrantApprovedExtensions;
}

bool SecretVaultExtensionGrantAuthoritySessionMutation::IsWellFormed() const noexcept
{
	// Revision zero is a syntactically valid optimistic fence; once a session is
	// active it is observed as RevisionMismatch rather than conflated with a
	// malformed identity.
	return hostGeneration != 0
		&& IsValidSecretVaultIdentifier(hostSessionId, kMaximumSecretVaultCapabilitySessionIdBytes);
}

bool SecretVaultExtensionGrantAuthorityActivateRequest::IsWellFormed() const noexcept
{
	return hostGeneration != 0 && IsValidSecretVaultIdentifier(hostSessionId, kMaximumSecretVaultCapabilitySessionIdBytes)
		&& HasNoDuplicates(approvedExtensionIds) && HasNoDuplicates(editorProcessIds);
}

CSecretVaultExtensionGrantAuthority::CSecretVaultExtensionGrantAuthority(std::string canonicalProfileId,
	std::uint64_t controlConnectionGeneration, SecretVaultExtensionGrantAuthorityConfig config)
	: m_profileId(std::move(canonicalProfileId))
	, m_controlConnectionGeneration(controlConnectionGeneration)
	, m_config(config)
	, m_configurationValid(profiles::IsCanonicalProfileAuthorityId(m_profileId)
		&& m_controlConnectionGeneration != 0 && m_config.IsValid())
{
	if (!m_configurationValid) return;
	try {
		m_active.approvedExtensionIds.reserve(m_config.maximumApprovedExtensions);
		m_active.editorProcessIds.reserve(m_config.maximumEditorLeases);
	} catch (...) {
		m_configurationValid = false;
		ClearActiveLocked();
	}
}

std::string_view CSecretVaultExtensionGrantAuthority::GetProfileId() const noexcept
{
	return m_profileId;
}

std::uint64_t CSecretVaultExtensionGrantAuthority::GetControlConnectionGeneration() const noexcept
{
	return m_controlConnectionGeneration;
}

bool CSecretVaultExtensionGrantAuthority::IsConfigured() const noexcept
{
	return m_configurationValid;
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::CurrentResultLocked(
	ESecretVaultExtensionGrantAuthorityStatus status) const noexcept
{
	return { status, m_revision };
}

void CSecretVaultExtensionGrantAuthority::ClearActiveLocked() noexcept
{
	m_active.hostSessionId.clear();
	m_active.hostGeneration = 0;
	m_active.enabled = false;
	m_active.approvedExtensionIds.clear();
	m_active.editorProcessIds.clear();
}

bool CSecretVaultExtensionGrantAuthority::MatchesActiveLocked(
	const SecretVaultExtensionGrantAuthoritySessionMutation& request) const noexcept
{
	return request.IsWellFormed() && request.expectedRevision == m_revision
		&& request.hostGeneration == m_active.hostGeneration && request.hostSessionId == m_active.hostSessionId;
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::ActivateOrReplace(
	const SecretVaultExtensionGrantAuthorityActivateRequest& request)
{
	try {
		std::scoped_lock lock(m_mutex);
		if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
		if (!request.IsWellFormed() || request.approvedExtensionIds.size() > m_config.maximumApprovedExtensions
			|| request.editorProcessIds.size() > m_config.maximumEditorLeases) {
			return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
		}
		if (request.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
		if (request.hostGeneration <= m_lastHostGeneration) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);

		ActiveSession replacement;
		replacement.hostSessionId = request.hostSessionId;
		replacement.hostGeneration = request.hostGeneration;
		replacement.enabled = true;
		replacement.approvedExtensionIds = request.approvedExtensionIds;
		replacement.editorProcessIds = request.editorProcessIds;
		m_active = std::move(replacement);
		m_lastHostGeneration = request.hostGeneration;
		++m_revision;
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
	} catch (...) {
		std::scoped_lock lock(m_mutex);
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	}
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::RegisterEditorProcess(
	const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request)
{
	try {
		std::scoped_lock lock(m_mutex);
		if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
		if (!request.session.IsWellFormed() || request.editorProcessId == 0) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
		if (request.session.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
		if (!MatchesActiveLocked(request.session)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
		if (Contains(m_active.editorProcessIds, request.editorProcessId)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
		if (m_active.editorProcessIds.size() == m_config.maximumEditorLeases) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::CapacityReached);
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		m_active.editorProcessIds.push_back(request.editorProcessId);
		++m_revision;
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
	} catch (...) {
		std::scoped_lock lock(m_mutex);
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	}
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::UnregisterEditorProcess(
	const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request)
{
	try {
		std::scoped_lock lock(m_mutex);
		if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
		if (!request.session.IsWellFormed() || request.editorProcessId == 0) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
		if (request.session.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
		if (!MatchesActiveLocked(request.session)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
		const auto found = std::find(m_active.editorProcessIds.begin(), m_active.editorProcessIds.end(), request.editorProcessId);
		if (found == m_active.editorProcessIds.end()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::NotFound);
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		m_active.editorProcessIds.erase(found);
		++m_revision;
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
	} catch (...) {
		std::scoped_lock lock(m_mutex);
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	}
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::ReplaceApprovedExtensions(
	const SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation& request)
{
	try {
		std::scoped_lock lock(m_mutex);
		if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
		if (!request.session.IsWellFormed() || !HasNoDuplicates(request.approvedExtensionIds)
			|| request.approvedExtensionIds.size() > m_config.maximumApprovedExtensions) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
		if (request.session.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
		if (!MatchesActiveLocked(request.session)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		m_active.approvedExtensionIds = request.approvedExtensionIds;
		++m_revision;
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
	} catch (...) {
		std::scoped_lock lock(m_mutex);
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	}
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::DisableExtension(
	const SecretVaultExtensionGrantAuthorityDisableExtensionMutation& request)
{
	try {
		std::scoped_lock lock(m_mutex);
		if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
		if (!request.session.IsWellFormed() || !IsExactCanonicalExtensionId(request.extensionId)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
		if (request.session.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
		if (!MatchesActiveLocked(request.session)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
		const auto found = std::find(m_active.approvedExtensionIds.begin(), m_active.approvedExtensionIds.end(), request.extensionId);
		if (found == m_active.approvedExtensionIds.end()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::NotFound);
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
		m_active.approvedExtensionIds.erase(found);
		++m_revision;
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
	} catch (...) {
		std::scoped_lock lock(m_mutex);
		return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	}
}

SecretVaultExtensionGrantAuthorityResult CSecretVaultExtensionGrantAuthority::Deactivate(
	const SecretVaultExtensionGrantAuthoritySessionMutation& request)
{
	std::scoped_lock lock(m_mutex);
	if (!IsConfigured()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	if (m_stopped) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Stopped);
	if (!request.IsWellFormed()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidRequest);
	if (request.expectedRevision != m_revision) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::RevisionMismatch);
	if (!MatchesActiveLocked(request)) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::GenerationMismatch);
	if (m_revision == std::numeric_limits<std::uint64_t>::max()) return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration);
	ClearActiveLocked();
	++m_revision;
	return CurrentResultLocked(ESecretVaultExtensionGrantAuthorityStatus::Applied);
}

SecretVaultExtensionGrantAuthorizationResult CSecretVaultExtensionGrantAuthority::AuthorizeIssue(
	const SecretVaultExtensionGrantAuthorityIssueRequest& request) const
{
	std::scoped_lock lock(m_mutex);
	if (!IsConfigured()) return { ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration, {}, m_revision };
	if (m_stopped) return { ESecretVaultExtensionGrantAuthorizationStatus::Stopped, {}, m_revision };
	if (request.profileId != m_profileId || request.controlConnectionGeneration != m_controlConnectionGeneration
		|| !m_active.enabled || request.hostSessionId != m_active.hostSessionId
		|| request.hostGeneration != m_active.hostGeneration || request.clientProcessId == 0
		|| !Contains(m_active.editorProcessIds, request.clientProcessId)
		|| !IsExactCanonicalExtensionId(request.extensionId)
		|| !Contains(m_active.approvedExtensionIds, request.extensionId)) {
		return { ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, {}, m_revision };
	}
	return { ESecretVaultExtensionGrantAuthorizationStatus::Authorized,
		{ { m_profileId, m_active.hostSessionId, request.clientProcessId, m_controlConnectionGeneration }, request.extensionId }, m_revision };
}

SecretVaultExtensionGrantRevokeAuthorizationResult CSecretVaultExtensionGrantAuthority::AuthorizeRevokeSession(
	const SecretVaultExtensionGrantRevokeAuthorizationRequest& request) const
{
	std::scoped_lock lock(m_mutex);
	if (!IsConfigured()) return { ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration, {}, m_revision, 0 };
	if (m_stopped) return { ESecretVaultExtensionGrantAuthorizationStatus::Stopped, {}, m_revision, 0 };
	if (request.profileId != m_profileId || request.controlConnectionGeneration != m_controlConnectionGeneration
		|| m_active.hostGeneration == 0 || request.clientProcessId == 0
		|| !Contains(m_active.editorProcessIds, request.clientProcessId)) {
		return { ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, {}, m_revision, 0 };
	}
	return { ESecretVaultExtensionGrantAuthorizationStatus::Authorized,
		{ m_profileId, m_active.hostSessionId, request.clientProcessId, m_controlConnectionGeneration }, m_revision, m_active.hostGeneration };
}

ESecretVaultExtensionGrantAuthorityStatus CSecretVaultExtensionGrantAuthority::Stop() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) return ESecretVaultExtensionGrantAuthorityStatus::Stopped;
	m_stopped = true;
	ClearActiveLocked();
	if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
	return ESecretVaultExtensionGrantAuthorityStatus::Stopped;
}

} // namespace platform::secrets
