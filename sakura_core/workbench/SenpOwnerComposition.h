/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpContributionOwners.h"
#include "workbench/SenpOwnerPublication.h"

namespace workbench {

//! Called once with the generation allocated by contribution ownership, before
//! runtime Start. A rejected/throwing factory publishes no native resources.
using SenpOwnerPublicationFactory = std::function<std::optional<SenpOwnerPublicationOptions>(
	const senp::ContributionOwnerIdentity&)>;

//! Window-local owner lifecycle. This is the sole scheduler boundary between
//! runtime polling and native effect projection: Poll always leaves the owner
//! service before the publication hub may submit follow-up work.
class CSenpOwnerComposition final {
public:
	CSenpOwnerComposition(layout::WorkbenchContributionRegistry& contributions,
		viewcontainer::CViewContainerPages& pages);
	CSenpOwnerComposition(layout::WorkbenchContributionRegistry& contributions,
		viewcontainer::CViewContainerPages& pages,
		senp::EffectRuntimeFactory runtimeFactory);
	~CSenpOwnerComposition();
	CSenpOwnerComposition(const CSenpOwnerComposition&) = delete;
	CSenpOwnerComposition& operator=(const CSenpOwnerComposition&) = delete;

	[[nodiscard]] senp::OwnerChangeResult Activate(senp::EffectRuntimeLaunch launch,
		std::wstring packageDigest, SenpOwnerPublicationOptions publication,
		senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] senp::OwnerChangeResult Activate(senp::EffectRuntimeLaunch launch,
		std::wstring packageDigest, SenpOwnerPublicationFactory publication,
		senp::CSenpRuntimeSession::Time now) noexcept;
	//! Publishes one workspace snapshot to every owner this composition holds,
	//! and retains it for owners that activate later. It queues only, so it never
	//! reenters the owner service; the next Poll is what puts it on the wire.
	[[nodiscard]] bool PublishWorkspace(const senp::effect::WorkspaceChanged& workspace) noexcept;
	//! One bounded UI-thread tick. False means projection failure or a closed/busy composition.
	[[nodiscard]] bool Poll(senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] bool Revoke(std::wstring_view extensionId,
		senp::effect::StopReason reason) noexcept;
	[[nodiscard]] std::optional<senp::OwnerChangeResult> TakeTransition() noexcept;
	[[nodiscard]] senp::ContributionOwnersSnapshot Snapshot() const noexcept;
	[[nodiscard]] bool IsCurrent(const senp::ContributionOwnerIdentity& owner) const noexcept;
	//! Stops every runtime before closing native publication resources.
	[[nodiscard]] bool Close() noexcept;

private:
	class Call;
	senp::CSenpContributionOwners m_owners;
	CSenpOwnerPublicationHub m_publications;
	bool m_entered{};
	bool m_closed{};
};

} // namespace workbench
