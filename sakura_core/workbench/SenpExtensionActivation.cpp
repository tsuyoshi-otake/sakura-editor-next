/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpExtensionActivation.h"

#include <algorithm>
#include <set>
#include <utility>

namespace workbench {
namespace {
using Status = senp::OwnerChangeStatus;
using ActivationState = SenpExtensionActivationState;

class ActivationCall final {
public:
	explicit ActivationCall(bool& entered) noexcept : m_entered(entered) { m_entered = true; }
	~ActivationCall() { m_entered = false; }
private:
	bool& m_entered;
};

ActivationState StateFor(Status status) noexcept
{
	switch (status) {
	case Status::Accepted: return ActivationState::Preparing;
	case Status::Activated: return ActivationState::Active;
	case Status::Busy: return ActivationState::Busy;
	case Status::Unsupported: return ActivationState::Unsupported;
	case Status::Stopped: return ActivationState::Stopped;
	default: return ActivationState::Failed;
	}
}

bool Eligible(const senp::ExtensionDescriptor& extension) noexcept
{
	return extension.installed && extension.enabled && extension.runtime.schemaVersion == 2
		&& extension.runtime.compatible;
}
}

class CSenpExtensionActivation::Entry final {
public:
	explicit Entry(senp::ExtensionDescriptor descriptor) : m_descriptor(std::move(descriptor))
	{
		if (!m_descriptor.installed || !m_descriptor.enabled) m_state = ActivationState::Disabled;
		else if (!Eligible(m_descriptor)) m_state = ActivationState::Unsupported;
	}
private:
	friend class CSenpExtensionActivation;
	senp::ExtensionDescriptor m_descriptor;
	SenpExtensionActivationState m_state{ ActivationState::Dormant };
	senp::OwnerChangeResult m_result{ Status::Unsupported };
	std::optional<senp::ContributionOwnerIdentity> m_active;
	std::set<std::wstring, std::less<>> m_requestedViews;
};

CSenpExtensionActivation::CSenpExtensionActivation(CSenpOwnerComposition& composition,
	std::wstring hostExecutable, SenpExtensionPublicationFactory publication)
	: m_composition(composition), m_hostExecutable(std::move(hostExecutable)), m_publication(std::move(publication)) {}

CSenpExtensionActivation::~CSenpExtensionActivation() { (void)Close(); }

void CSenpExtensionActivation::RevokeAll(const senp::effect::StopReason reason) noexcept
{
	for (auto& [id, entry] : m_entries) {
		(void)m_composition.Revoke(id, reason);
		entry->m_active.reset();
		entry->m_result = { Status::Stopped };
		entry->m_state = ActivationState::Stopped;
	}
}

bool CSenpExtensionActivation::Synchronize(const senp::ManagementSnapshot& snapshot,
	const std::int64_t workspaceRevision, const std::int64_t accountGeneration,
	const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_entered) return false;
	ActivationCall call(m_entered);
	if (workspaceRevision < 0 || accountGeneration < 0
		|| (m_synchronized && snapshot.revision < m_revision)) return false;
	if (snapshot.state != senp::EManagementState::Ready
		&& snapshot.state != senp::EManagementState::ReadyWithDiagnostics) {
		RevokeAll(senp::effect::StopReason::HostUnavailable);
		m_entries.clear();
		m_synchronized = false;
		return false;
	}
	if (m_synchronized && snapshot.revision == m_revision
		&& workspaceRevision == m_workspaceRevision && accountGeneration == m_accountGeneration) return true;
	try {
		// Validate and allocate the complete desired catalog before revoking any
		// current owner. A malformed authority snapshot is never partially applied.
		std::map<std::wstring, std::unique_ptr<Entry>, std::less<>> desired;
		std::set<std::wstring, std::less<>> views;
		for (const auto& extension : snapshot.extensions) {
			if (extension.runtime.schemaVersion != 2) continue;
			if (desired.size() >= 64 || extension.id.empty() || extension.views.size() > 64
				|| !desired.emplace(extension.id, std::make_unique<Entry>(extension)).second) return false;
			for (const auto& view : extension.views) if (view.id.empty() || !views.insert(view.id).second) return false;
		}
		const bool scopeChanged = workspaceRevision != m_workspaceRevision || accountGeneration != m_accountGeneration;
		for (auto& [id, entry] : m_entries) {
			const auto replacement = desired.find(id);
			if (replacement == desired.end()) {
				(void)m_composition.Revoke(id, senp::effect::StopReason::Disabled);
				continue;
			}
			auto& next = replacement->second;
			if (!scopeChanged && next->m_descriptor == entry->m_descriptor) {
				next->m_state = entry->m_state;
				next->m_result = entry->m_result;
				next->m_active = entry->m_active;
				next->m_requestedViews = entry->m_requestedViews;
				continue;
			}
			next->m_requestedViews = entry->m_requestedViews;
			std::erase_if(next->m_requestedViews, [&](const auto& viewId) {
				return std::ranges::none_of(next->m_descriptor.views,
					[&](const auto& view) { return view.id == viewId; });
			});
			if (scopeChanged || !Eligible(next->m_descriptor) || entry->m_state == ActivationState::Preparing)
				(void)m_composition.Revoke(id, Eligible(next->m_descriptor)
					? senp::effect::StopReason::Updated : senp::effect::StopReason::Disabled);
			else next->m_active = entry->m_active; // Candidate failure preserves the previous package.
		}
		m_entries = std::move(desired);
		m_revision = snapshot.revision;
		m_workspaceRevision = workspaceRevision;
		m_accountGeneration = accountGeneration;
		m_synchronized = true;
		for (auto& [id, entry] : m_entries) {
			if (entry->m_requestedViews.empty() || entry->m_state != ActivationState::Dormant) continue;
			entry->m_state = ActivationState::Queued;
			entry->m_result = { Status::Busy };
		}
		AdmitNext(now);
		return true;
	} catch (...) {
		RevokeAll(senp::effect::StopReason::HostUnavailable);
		m_entries.clear();
		m_synchronized = false;
		return false;
	}
}

void CSenpExtensionActivation::AdmitNext(const senp::CSenpRuntimeSession::Time now)
{
	// One prepared native/catalog transaction at a time. Queuing is admission,
	// not a retry: runtime Busy/Failed results remain terminal until requested.
	if (std::ranges::any_of(m_entries, [](const auto& item) {
		return item.second->m_state == ActivationState::Preparing;
	})) return;
	for (auto& [id, entry] : m_entries) {
		if (entry->m_state != ActivationState::Queued) continue;
		(void)Start(*entry, now);
		break;
	}
}

senp::OwnerChangeResult CSenpExtensionActivation::Start(Entry& entry,
	const senp::CSenpRuntimeSession::Time now)
{
	const auto& descriptor = entry.m_descriptor;
	const bool declaredDemand = std::ranges::any_of(descriptor.runtime.activationEvents,
		[&](const auto& event) {
			const std::wstring_view value(event);
			return value.starts_with(L"onView:") && entry.m_requestedViews.contains(value.substr(7));
		});
	if (!Eligible(descriptor) || !m_publication || !declaredDemand) {
		entry.m_state = ActivationState::Unsupported;
		return entry.m_result = { Status::Unsupported };
	}
	entry.m_result = m_composition.Activate({
		.hostExecutable = m_hostExecutable, .modulePath = descriptor.modulePath,
		.moduleSha256 = descriptor.moduleSha256, .extensionId = descriptor.id,
		.context = { .workspaceRevision = m_workspaceRevision, .accountGeneration = m_accountGeneration },
	}, descriptor.archiveSha256, [&](const senp::ContributionOwnerIdentity& owner) {
		return m_publication(descriptor, owner);
	}, now);
	entry.m_state = StateFor(entry.m_result.status);
	return entry.m_result;
}

senp::OwnerChangeResult CSenpExtensionActivation::RequestView(const std::wstring_view viewId,
	const senp::CSenpRuntimeSession::Time now, const bool explicitRetry) noexcept
{
	if (m_closed || m_entered) return { m_closed ? Status::Stopped : Status::Busy };
	ActivationCall call(m_entered);
	try {
		if (viewId.empty() || viewId.size() > 128) return { Status::Unsupported };
		const auto activationEvent = L"onView:" + std::wstring(viewId);
		for (auto& [id, entry] : m_entries) {
			const auto& extension = entry->m_descriptor;
			if (!Eligible(extension) || std::ranges::none_of(extension.views,
				[&](const auto& view) { return view.id == viewId; })) continue;
			entry->m_requestedViews.emplace(viewId);
			if (std::ranges::none_of(extension.runtime.activationEvents,
				[&](const auto& event) { return event == activationEvent; })) {
				entry->m_state = ActivationState::Unsupported;
				return entry->m_result = { Status::Unsupported };
			}
			if (entry->m_state == ActivationState::Dormant || (explicitRetry
				&& entry->m_state != ActivationState::Queued
				&& entry->m_state != ActivationState::Preparing && entry->m_state != ActivationState::Active)) {
				entry->m_state = ActivationState::Queued;
				entry->m_result = { Status::Busy };
				AdmitNext(now);
			}
			return entry->m_result;
		}
		return { Status::Unsupported };
	} catch (...) {
		RevokeAll(senp::effect::StopReason::HostUnavailable);
		return { Status::Failed };
	}
}

bool CSenpExtensionActivation::Poll(const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_entered) return false;
	ActivationCall call(m_entered);
	try {
		const bool projected = m_composition.Poll(now);
		for (std::size_t count = 0; count < 16; ++count) {
			auto result = m_composition.TakeTransition();
			if (!result) break;
			const auto found = m_entries.find(result->owner.extensionId);
			if (found == m_entries.end()) continue;
			auto& entry = *found->second;
			if (entry.m_state != ActivationState::Preparing || entry.m_result.owner != result->owner) continue;
			entry.m_result = std::move(*result);
			entry.m_state = StateFor(entry.m_result.status);
			if (entry.m_state == ActivationState::Active) entry.m_active = entry.m_result.owner;
		}
		for (auto& [id, entry] : m_entries) {
			if (!entry->m_active || m_composition.IsCurrent(*entry->m_active)) continue;
			entry->m_active.reset();
			if (entry->m_state == ActivationState::Active) {
				entry->m_result.status = Status::Failed;
				entry->m_state = ActivationState::Failed;
			}
		}
		AdmitNext(now);
		return projected;
	} catch (...) {
		RevokeAll(senp::effect::StopReason::HostUnavailable);
		return false;
	}
}

std::optional<SenpExtensionActivationState> CSenpExtensionActivation::State(std::wstring_view extensionId) const noexcept
{
	const auto found = m_entries.find(extensionId);
	return found == m_entries.end() ? std::nullopt : std::optional(found->second->m_state);
}

std::optional<senp::ContributionOwnerIdentity> CSenpExtensionActivation::ActiveOwner(std::wstring_view extensionId) const
{
	const auto found = m_entries.find(extensionId);
	return found == m_entries.end() ? std::nullopt : found->second->m_active;
}

bool CSenpExtensionActivation::Close() noexcept
{
	if (m_entered) return false;
	ActivationCall call(m_entered);
	m_closed = true;
	RevokeAll(senp::effect::StopReason::Shutdown);
	return m_composition.Close();
}

} // namespace workbench
