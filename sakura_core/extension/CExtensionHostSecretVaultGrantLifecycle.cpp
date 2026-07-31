/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostSecretVaultGrantLifecycle.h"

#include "platform/secrets/SecretVaultTypes.h"

#include <algorithm>
#include <limits>
#include <utility>

CExtensionHostSecretVaultGrantCoordinator::CExtensionHostSecretVaultGrantCoordinator(
	std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle> lifecycle)
	: m_lifecycle(std::move(lifecycle))
{
}

bool CExtensionHostSecretVaultGrantCoordinator::IsSuccessfulAdvance(
	const SExtensionHostSecretVaultGrantResult& result,
	std::uint64_t expectedRevision) noexcept
{
	return result.status == EExtensionHostSecretVaultGrantStatus::Succeeded &&
		result.revision > expectedRevision;
}

bool CExtensionHostSecretVaultGrantCoordinator::GetActive(std::shared_ptr<ActiveSession>& active) const noexcept
{
	std::scoped_lock lock(m_mutex);
	if (!m_active) return false;
	active = m_active;
	return true;
}

bool CExtensionHostSecretVaultGrantCoordinator::UpdateActiveRevision(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t expectedRevision,
	std::uint64_t nextRevision) noexcept
{
	std::scoped_lock lock(m_mutex);
	if (!m_active || m_active->session != session ||
		m_active->revision.load(std::memory_order_acquire) != expectedRevision) return false;
	m_active->revision.store(nextRevision, std::memory_order_release);
	return true;
}

void CExtensionHostSecretVaultGrantCoordinator::ClearActiveIfMatches(
	const SExtensionHostSecretVaultGrantSession& session) noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_active && m_active->session == session) m_active.reset();
}

bool CExtensionHostSecretVaultGrantCoordinator::EndSession(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t revision) noexcept
{
	if (!m_lifecycle) return true;

	// The callbacks deliberately run with no coordinator mutex held. Deactivation is
	// attempted even when revoke reports a conflict/failure, because a remote side may
	// have partially applied the first request and must still be fenced.
	std::uint64_t deactivateRevision = revision;
	bool revoked = false;
	try {
		const auto revoke = m_lifecycle->RevokeIssuedCapabilities(session, revision);
		revoked = IsSuccessfulAdvance(revoke, revision);
		// A production adapter fences issuance before revoking stored bearers.
		// Even if bearer revocation then fails, Deactivate must use the advanced
		// authority revision rather than accidentally turning the fence into an
		// un-deactivatable intermediate terminal state.
		if (revoke.revision > deactivateRevision) deactivateRevision = revoke.revision;
	} catch (...) {
		revoked = false;
	}

	bool deactivated = false;
	try {
		const auto deactivate = m_lifecycle->Deactivate(session, deactivateRevision);
		deactivated = IsSuccessfulAdvance(deactivate, deactivateRevision);
	} catch (...) {
		deactivated = false;
	}
	return revoked && deactivated;
}

void CExtensionHostSecretVaultGrantCoordinator::FailClosed(
	const SExtensionHostSecretVaultGrantSession& session,
	std::uint64_t revision) noexcept
{
	if (!m_lifecycle || !session.IsValid()) {
		ClearActiveIfMatches(session);
		return;
	}

	EndSession(session, revision);
	ClearActiveIfMatches(session);
}

bool CExtensionHostSecretVaultGrantCoordinator::RegisterActiveEditorProcess(
	std::uint32_t editorProcessId) noexcept
{
	std::shared_ptr<ActiveSession> active;
	if (!GetActive(active) || editorProcessId == 0) return false;
	const auto expectedRevision = active->revision.load(std::memory_order_acquire);

	SExtensionHostSecretVaultGrantResult result;
	try {
		// No coordinator mutex is held while invoking the injected runtime adapter.
		result = m_lifecycle->RegisterEditorProcess(active->session, editorProcessId, expectedRevision);
	} catch (...) {
		FailClosed(active->session, expectedRevision);
		return false;
	}
	if (!IsSuccessfulAdvance(result, expectedRevision) ||
		!UpdateActiveRevision(active->session, expectedRevision, result.revision)) {
		FailClosed(active->session, result.revision > expectedRevision ? result.revision : expectedRevision);
		return false;
	}
	return true;
}

bool CExtensionHostSecretVaultGrantCoordinator::ReplaceActiveInventory(
	const std::vector<std::string>& canonicalExtensionIds) noexcept
{
	std::shared_ptr<ActiveSession> active;
	if (!GetActive(active)) return false;
	const auto expectedRevision = active->revision.load(std::memory_order_acquire);

	SExtensionHostSecretVaultGrantResult result;
	try {
		// No coordinator mutex is held while invoking the injected runtime adapter.
		result = m_lifecycle->ReplaceInstalledExtensionInventory(
			active->session, canonicalExtensionIds, expectedRevision);
	} catch (...) {
		FailClosed(active->session, expectedRevision);
		return false;
	}
	if (!IsSuccessfulAdvance(result, expectedRevision) ||
		!UpdateActiveRevision(active->session, expectedRevision, result.revision)) {
		FailClosed(active->session, result.revision > expectedRevision ? result.revision : expectedRevision);
		return false;
	}
	return true;
}

bool CExtensionHostSecretVaultGrantCoordinator::ReissueInventoryAndLeases()
{
	std::vector<std::string> inventory;
	std::vector<std::uint32_t> processIds;
	bool inventoryKnown = false;
	{
		std::scoped_lock lock(m_mutex);
		inventoryKnown = m_inventoryKnown;
		if (inventoryKnown) inventory = m_canonicalExtensionInventory;
		processIds.reserve(m_editorLeases.size());
		for (const auto& [processId, count] : m_editorLeases) {
			if (count != 0) processIds.push_back(processId);
		}
	}

	if (inventoryKnown && !ReplaceActiveInventory(inventory)) return false;
	for (const auto processId : processIds) {
		if (!RegisterActiveEditorProcess(processId)) return false;
	}
	return true;
}

bool CExtensionHostSecretVaultGrantCoordinator::Activate(
	const SExtensionHostSecretVaultGrantSession& session) noexcept
{
	if (!m_lifecycle) return true;
	if (!session.IsValid()) return false;

	try {
		std::shared_ptr<ActiveSession> previous;
		if (GetActive(previous)) {
			if (previous->session == session) return true;
			EndSession(previous->session, previous->revision.load(std::memory_order_acquire));
			ClearActiveIfMatches(previous->session);
		}

		SExtensionHostSecretVaultGrantResult result;
		try {
			// No coordinator mutex is held while invoking the injected runtime adapter.
			result = m_lifecycle->Activate(session, 0);
		} catch (...) {
			FailClosed(session, 0);
			return false;
		}
		if (!IsSuccessfulAdvance(result, 0)) {
			FailClosed(session, result.revision);
			return false;
		}

		{
			std::scoped_lock lock(m_mutex);
			if (m_active) {
				// A reentrant/concurrent callback won the race. Fence the newly created
				// remote session rather than letting two grants remain active.
				// The lock is released before FailClosed below.
			} else {
				auto next = std::make_shared<ActiveSession>();
				next->session = session;
				next->revision.store(result.revision, std::memory_order_release);
				m_active = std::move(next);
			}
		}
		if (!IsActiveForGeneration(session.generation)) {
			FailClosed(session, result.revision);
			return false;
		}
		return ReissueInventoryAndLeases();
	} catch (...) {
		FailClosed(session, 0);
		return false;
	}
}

EExtensionHostSecretVaultLeaseAcquireResult CExtensionHostSecretVaultGrantCoordinator::AcquireEditorLease(
	std::uint32_t editorProcessId) noexcept
{
	if (!m_lifecycle) return EExtensionHostSecretVaultLeaseAcquireResult::Registered;
	if (editorProcessId == 0) return EExtensionHostSecretVaultLeaseAcquireResult::Rejected;

	try {
		bool registerProcess = false;
		{
			std::scoped_lock lock(m_mutex);
			auto& count = m_editorLeases[editorProcessId];
			if (count == (std::numeric_limits<std::uint32_t>::max)()) {
				return EExtensionHostSecretVaultLeaseAcquireResult::Rejected;
			}
			++count;
			if (!m_active) return EExtensionHostSecretVaultLeaseAcquireResult::Deferred;
			registerProcess = count == 1;
		}
		if (!registerProcess) return EExtensionHostSecretVaultLeaseAcquireResult::Registered;
		if (RegisterActiveEditorProcess(editorProcessId)) {
			return EExtensionHostSecretVaultLeaseAcquireResult::Registered;
		}
		return EExtensionHostSecretVaultLeaseAcquireResult::Rejected;
	} catch (...) {
		return EExtensionHostSecretVaultLeaseAcquireResult::Rejected;
	}
}

bool CExtensionHostSecretVaultGrantCoordinator::ReleaseEditorLease(std::uint32_t editorProcessId) noexcept
{
	if (!m_lifecycle) return true;
	if (editorProcessId == 0) return false;

	try {
		std::shared_ptr<ActiveSession> active;
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_editorLeases.find(editorProcessId);
			if (found == m_editorLeases.end()) return false;
			if (found->second > 1) {
				--found->second;
				return true;
			}
			m_editorLeases.erase(found);
			if (!m_active) return true;
			active = m_active;
		}

		SExtensionHostSecretVaultGrantResult result;
		const auto expectedRevision = active->revision.load(std::memory_order_acquire);
		try {
			// No coordinator mutex is held while invoking the injected runtime adapter.
			result = m_lifecycle->UnregisterEditorProcess(active->session, editorProcessId, expectedRevision);
		} catch (...) {
			FailClosed(active->session, expectedRevision);
			return false;
		}
		if (!IsSuccessfulAdvance(result, expectedRevision) ||
			!UpdateActiveRevision(active->session, expectedRevision, result.revision)) {
			FailClosed(active->session, result.revision > expectedRevision ? result.revision : expectedRevision);
			return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

bool CExtensionHostSecretVaultGrantCoordinator::ReplaceInstalledExtensionInventory(
	const std::vector<std::string>& extensionIds) noexcept
{
	if (!m_lifecycle) return true;

	try {
		std::vector<std::string> canonicalExtensionIds;
		canonicalExtensionIds.reserve(extensionIds.size());
		for (const auto& extensionId : extensionIds) {
			std::string canonicalExtensionId;
			if (!platform::secrets::CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)) {
				RevokeAndDeactivate();
				return false;
			}
			canonicalExtensionIds.push_back(std::move(canonicalExtensionId));
		}
		std::sort(canonicalExtensionIds.begin(), canonicalExtensionIds.end());
		canonicalExtensionIds.erase(
			std::unique(canonicalExtensionIds.begin(), canonicalExtensionIds.end()), canonicalExtensionIds.end());

		bool hasActive = false;
		{
			std::scoped_lock lock(m_mutex);
			m_canonicalExtensionInventory = canonicalExtensionIds;
			m_inventoryKnown = true;
			if (m_active) {
				hasActive = true;
			}
		}
		if (!hasActive) return true;
		if (ReplaceActiveInventory(canonicalExtensionIds)) return true;
		return false;
	} catch (...) {
		RevokeAndDeactivate();
		return false;
	}
}

void CExtensionHostSecretVaultGrantCoordinator::RevokeAndDeactivate() noexcept
{
	if (!m_lifecycle) return;
	try {
		std::shared_ptr<ActiveSession> active;
		if (!GetActive(active)) return;
		EndSession(active->session, active->revision.load(std::memory_order_acquire));
		ClearActiveIfMatches(active->session);
	} catch (...) {
		// A lifecycle callback must never leave an exception path with an active local grant.
		std::scoped_lock lock(m_mutex);
		m_active.reset();
	}
}

void CExtensionHostSecretVaultGrantCoordinator::Shutdown() noexcept
{
	RevokeAndDeactivate();
	std::scoped_lock lock(m_mutex);
	m_editorLeases.clear();
}

bool CExtensionHostSecretVaultGrantCoordinator::IsActiveForGeneration(std::uint64_t generation) const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_active && m_active->session.generation == generation;
}
