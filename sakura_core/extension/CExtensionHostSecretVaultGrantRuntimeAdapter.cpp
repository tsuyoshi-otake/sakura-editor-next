/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostSecretVaultGrantRuntimeAdapter.h"

#include "platform/profiles/ProfileAuthorityIdentity.h"
#include "platform/secrets/SecretVaultTypes.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {

bool ToUtf8(std::wstring_view value, std::string& result) noexcept
{
	result.clear();
	if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return false;
	}
	const int bytes = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
		nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return false;
	try {
		result.resize(static_cast<std::size_t>(bytes));
	} catch (...) {
		return false;
	}
	if (::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
			result.data(), bytes, nullptr, nullptr) != bytes) {
		result.clear();
		return false;
	}
	return platform::secrets::IsValidSecretVaultIdentifier(
		result, platform::secrets::kMaximumSecretVaultCapabilitySessionIdBytes);
}

bool IsCanonicalInventory(const std::vector<std::string>& extensionIds) noexcept
{
	if (extensionIds.size() > platform::secrets::kMaximumSecretVaultExtensionGrantApprovedExtensions) {
		return false;
	}
	for (std::size_t index = 0; index < extensionIds.size(); ++index) {
		std::string canonical;
		if (!platform::secrets::CanonicalizeSecretVaultExtensionId(extensionIds[index], canonical)
			|| canonical != extensionIds[index]) {
			return false;
		}
		if (std::find(extensionIds.begin(), extensionIds.begin() + static_cast<std::ptrdiff_t>(index),
				extensionIds[index]) != extensionIds.begin() + static_cast<std::ptrdiff_t>(index)) {
			return false;
		}
	}
	return true;
}

} // namespace

CExtensionHostSecretVaultGrantRuntimeAdapter::CExtensionHostSecretVaultGrantRuntimeAdapter(
	std::shared_ptr<platform::secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<platform::secrets::ISecretVaultCapabilityService> capabilities)
	: m_grantAuthority(std::move(grantAuthority))
	, m_capabilities(std::move(capabilities))
{
	if (!m_grantAuthority || !m_capabilities) return;
	try {
		m_profileId.assign(m_grantAuthority->GetProfileId());
		m_controlConnectionGeneration = m_grantAuthority->GetControlConnectionGeneration();
		m_valid = platform::profiles::IsCanonicalProfileAuthorityId(m_profileId)
			&& m_controlConnectionGeneration != 0
			&& m_capabilities->GetProfileId() == m_profileId;
	} catch (...) {
		m_valid = false;
	}
}

bool CExtensionHostSecretVaultGrantRuntimeAdapter::IsValid() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_valid;
}

bool CExtensionHostSecretVaultGrantRuntimeAdapter::MatchesActiveLocked(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t expectedRevision) const noexcept
{
	return m_valid && m_activeSession.IsValid() && session == m_activeSession
		&& expectedRevision == m_authorityRevision;
}

platform::secrets::SecretVaultExtensionGrantAuthoritySessionMutation
CExtensionHostSecretVaultGrantRuntimeAdapter::SessionMutationLocked(
	std::uint64_t expectedRevision) const
{
	return {
		.expectedRevision = expectedRevision,
		.hostSessionId = m_activeHostSessionId,
		.hostGeneration = m_activeSession.generation,
	};
}

SExtensionHostSecretVaultGrantResult
CExtensionHostSecretVaultGrantRuntimeAdapter::ApplyAuthorityResultLocked(
	const platform::secrets::SecretVaultExtensionGrantAuthorityResult& result) noexcept
{
	if (result.revision >= m_authorityRevision) m_authorityRevision = result.revision;
	using Status = platform::secrets::ESecretVaultExtensionGrantAuthorityStatus;
	switch (result.status) {
	case Status::Applied:
		return { EExtensionHostSecretVaultGrantStatus::Succeeded, result.revision };
	case Status::RevisionMismatch:
		return { EExtensionHostSecretVaultGrantStatus::Conflict, result.revision };
	case Status::NotActive:
	case Status::NotFound:
		return { EExtensionHostSecretVaultGrantStatus::Inactive, result.revision };
	case Status::InvalidRequest:
	case Status::GenerationMismatch:
		return { EExtensionHostSecretVaultGrantStatus::Invalid, result.revision };
	case Status::Stopped:
	case Status::InvalidConfiguration:
	case Status::CapacityReached:
	default:
		return { EExtensionHostSecretVaultGrantStatus::Failed, result.revision };
	}
}

void CExtensionHostSecretVaultGrantRuntimeAdapter::ClearActiveLocked() noexcept
{
	m_activeSession = {};
	m_activeHostSessionId.clear();
	m_canonicalExtensionInventory.clear();
	m_editorProcessIds.clear();
}

SExtensionHostSecretVaultGrantResult CExtensionHostSecretVaultGrantRuntimeAdapter::Activate(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	if (!m_valid || !session.IsValid() || expectedRevision != 0 || m_activeSession.IsValid()) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	std::string hostSessionId;
	if (!ToUtf8(session.extensionHostSessionId, hostSessionId)) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	try {
		const auto result = m_grantAuthority->ActivateOrReplace({
			.expectedRevision = m_authorityRevision,
			.hostSessionId = hostSessionId,
			.hostGeneration = session.generation,
			.approvedExtensionIds = {},
			.editorProcessIds = {},
		});
		const auto mapped = ApplyAuthorityResultLocked(result);
		if (mapped.status == EExtensionHostSecretVaultGrantStatus::Succeeded) {
			m_activeSession = session;
			m_activeHostSessionId = std::move(hostSessionId);
			m_canonicalExtensionInventory.clear();
			m_editorProcessIds.clear();
		}
		return mapped;
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

SExtensionHostSecretVaultGrantResult CExtensionHostSecretVaultGrantRuntimeAdapter::RegisterEditorProcess(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint32_t editorProcessId,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	if (!MatchesActiveLocked(session, expectedRevision) || editorProcessId == 0
		|| std::find(m_editorProcessIds.begin(), m_editorProcessIds.end(), editorProcessId) != m_editorProcessIds.end()) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	try {
		const auto result = m_grantAuthority->RegisterEditorProcess({
			.session = SessionMutationLocked(expectedRevision),
			.editorProcessId = editorProcessId,
		});
		const auto mapped = ApplyAuthorityResultLocked(result);
		if (mapped.status == EExtensionHostSecretVaultGrantStatus::Succeeded) {
			m_editorProcessIds.push_back(editorProcessId);
		}
		return mapped;
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

SExtensionHostSecretVaultGrantResult CExtensionHostSecretVaultGrantRuntimeAdapter::UnregisterEditorProcess(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint32_t editorProcessId,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	const auto found = std::find(m_editorProcessIds.begin(), m_editorProcessIds.end(), editorProcessId);
	if (!MatchesActiveLocked(session, expectedRevision) || found == m_editorProcessIds.end()) {
		return { EExtensionHostSecretVaultGrantStatus::Inactive, m_authorityRevision };
	}
	try {
		const auto result = m_grantAuthority->UnregisterEditorProcess({
			.session = SessionMutationLocked(expectedRevision),
			.editorProcessId = editorProcessId,
		});
		const auto mapped = ApplyAuthorityResultLocked(result);
		if (mapped.status == EExtensionHostSecretVaultGrantStatus::Succeeded) {
			m_editorProcessIds.erase(found);
		}
		return mapped;
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

SExtensionHostSecretVaultGrantResult
CExtensionHostSecretVaultGrantRuntimeAdapter::ReplaceInstalledExtensionInventory(
	const SExtensionHostSecretVaultGrantSession& session,
	const std::vector<std::string>& canonicalExtensionIds,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	if (!MatchesActiveLocked(session, expectedRevision) || !IsCanonicalInventory(canonicalExtensionIds)) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	try {
		const auto result = m_grantAuthority->ReplaceApprovedExtensions({
			.session = SessionMutationLocked(expectedRevision),
			.approvedExtensionIds = canonicalExtensionIds,
		});
		const auto mapped = ApplyAuthorityResultLocked(result);
		if (mapped.status == EExtensionHostSecretVaultGrantStatus::Succeeded) {
			m_canonicalExtensionInventory = canonicalExtensionIds;
		}
		return mapped;
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

SExtensionHostSecretVaultGrantResult CExtensionHostSecretVaultGrantRuntimeAdapter::RevokeIssuedCapabilities(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	if (!MatchesActiveLocked(session, expectedRevision)) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	try {
		// First fence every future issue authorization. The capability service is
		// independently concurrent, so revoking bearers before this CAS would
		// allow one newly authorized token to survive the teardown.
		const auto fenced = m_grantAuthority->ReplaceApprovedExtensions({
			.session = SessionMutationLocked(expectedRevision),
			.approvedExtensionIds = {},
		});
		const auto fenceResult = ApplyAuthorityResultLocked(fenced);
		if (fenceResult.status != EExtensionHostSecretVaultGrantStatus::Succeeded) {
			return fenceResult;
		}
		const auto revoked = m_capabilities->RevokeHostSession({
			.profileId = m_profileId,
			.extensionHostSessionId = m_activeHostSessionId,
			.connectionGeneration = m_controlConnectionGeneration,
		});
		if (revoked.status != platform::secrets::ESecretVaultCapabilityRevokeStatus::Revoked
			&& revoked.status != platform::secrets::ESecretVaultCapabilityRevokeStatus::NotFound) {
			return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
		}
		return { EExtensionHostSecretVaultGrantStatus::Succeeded, m_authorityRevision };
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

SExtensionHostSecretVaultGrantResult CExtensionHostSecretVaultGrantRuntimeAdapter::Deactivate(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t expectedRevision)
{
	std::scoped_lock lock(m_mutex);
	if (!MatchesActiveLocked(session, expectedRevision)) {
		return { EExtensionHostSecretVaultGrantStatus::Invalid, m_authorityRevision };
	}
	try {
		const auto result = m_grantAuthority->Deactivate(SessionMutationLocked(expectedRevision));
		const auto mapped = ApplyAuthorityResultLocked(result);
		if (mapped.status == EExtensionHostSecretVaultGrantStatus::Succeeded) ClearActiveLocked();
		return mapped;
	} catch (...) {
		return { EExtensionHostSecretVaultGrantStatus::Failed, m_authorityRevision };
	}
}

std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle>
CreateExtensionHostSecretVaultGrantRuntimeAdapter(
	std::shared_ptr<platform::secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<platform::secrets::ISecretVaultCapabilityService> capabilities)
{
	try {
		auto adapter = std::make_shared<CExtensionHostSecretVaultGrantRuntimeAdapter>(
			std::move(grantAuthority), std::move(capabilities));
		return adapter->IsValid() ? std::move(adapter) : nullptr;
	} catch (...) {
		return nullptr;
	}
}
