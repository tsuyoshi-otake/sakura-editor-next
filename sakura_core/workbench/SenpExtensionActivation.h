/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpManagementService.h"
#include "workbench/SenpOwnerComposition.h"

#include <map>

namespace workbench {

enum class SenpExtensionActivationState : std::uint8_t {
	Dormant, Queued, Preparing, Active, Failed, Busy, Unsupported, Disabled, Stopped,
};

using SenpExtensionPublicationFactory = std::function<std::optional<SenpOwnerPublicationOptions>(
	const senp::ExtensionDescriptor&, const senp::ContributionOwnerIdentity&)>;

//! UI-thread activation authority for one window. Synchronize consumes package
//! authority snapshots; RequestView admits declared onView activation only.
//! Repeated visibility requests and ticks never restart a terminal attempt.
//! A changed package/scope or explicit retry is required for a new attempt.
//! Native declaration shells and the window own scheduling; this class performs
//! no filesystem, network, message pumping or timer registration.
class CSenpExtensionActivation final {
public:
	CSenpExtensionActivation(CSenpOwnerComposition& composition, std::wstring hostExecutable,
		SenpExtensionPublicationFactory publication);
	~CSenpExtensionActivation();
	CSenpExtensionActivation(const CSenpExtensionActivation&) = delete;
	CSenpExtensionActivation& operator=(const CSenpExtensionActivation&) = delete;

	[[nodiscard]] bool Synchronize(const senp::ManagementSnapshot& snapshot,
		std::int64_t workspaceRevision, std::int64_t accountGeneration,
		senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] senp::OwnerChangeResult RequestView(std::wstring_view viewId,
		senp::CSenpRuntimeSession::Time now, bool explicitRetry = false) noexcept;
	[[nodiscard]] bool Poll(senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] std::optional<SenpExtensionActivationState> State(std::wstring_view extensionId) const noexcept;
	[[nodiscard]] std::optional<senp::ContributionOwnerIdentity> ActiveOwner(std::wstring_view extensionId) const;
	//! Stops admission immediately; explicit repeated Close owns failed cleanup.
	[[nodiscard]] bool Close() noexcept;

private:
	class Entry;
	[[nodiscard]] senp::OwnerChangeResult Start(Entry& entry, senp::CSenpRuntimeSession::Time now);
	void AdmitNext(senp::CSenpRuntimeSession::Time now);
	void RevokeAll(senp::effect::StopReason reason) noexcept;
	CSenpOwnerComposition& m_composition;
	std::wstring m_hostExecutable;
	SenpExtensionPublicationFactory m_publication;
	std::map<std::wstring, std::unique_ptr<Entry>, std::less<>> m_entries;
	std::uint64_t m_revision{};
	std::int64_t m_workspaceRevision{};
	std::int64_t m_accountGeneration{};
	bool m_synchronized{};
	bool m_entered{};
	bool m_closed{};
};

} // namespace workbench
