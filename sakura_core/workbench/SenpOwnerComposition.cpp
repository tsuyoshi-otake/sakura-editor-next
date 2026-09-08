/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpOwnerComposition.h"

#include <utility>

namespace workbench {

class CSenpOwnerComposition::Call final {
public:
	explicit Call(bool& entered) noexcept : m_entered(entered) { m_entered = true; }
	~Call() { m_entered = false; }
private:
	bool& m_entered;
};

CSenpOwnerComposition::CSenpOwnerComposition(
	layout::WorkbenchContributionRegistry& contributions,
	viewcontainer::CViewContainerPages& pages)
	: CSenpOwnerComposition(contributions, pages, {}) {}

CSenpOwnerComposition::CSenpOwnerComposition(
	layout::WorkbenchContributionRegistry& contributions,
	viewcontainer::CViewContainerPages& pages,
	senp::EffectRuntimeFactory runtimeFactory)
	: m_owners(std::move(runtimeFactory)), m_publications(m_owners, contributions, pages) {}

CSenpOwnerComposition::~CSenpOwnerComposition() { (void)Close(); }

senp::OwnerChangeResult CSenpOwnerComposition::Activate(senp::EffectRuntimeLaunch launch,
	std::wstring packageDigest, SenpOwnerPublicationOptions publication,
	const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_entered) return { m_closed ? senp::OwnerChangeStatus::Stopped : senp::OwnerChangeStatus::Busy };
	Call call(m_entered);
	try {
		auto pending = std::make_shared<std::optional<SenpOwnerPublicationOptions>>(std::move(publication));
		return m_owners.Prepare(std::move(launch), std::move(packageDigest),
			[this, pending](const senp::ContributionOwnerIdentity& candidate,
				const senp::ContributionOwnerIdentity* previous) {
				if (!*pending) return std::unique_ptr<senp::ISenpOwnerPublication>{};
				auto options = std::move(**pending);
				pending->reset();
				return m_publications.Prepare(candidate, previous, std::move(options));
			}, now);
	} catch (...) {
		return { senp::OwnerChangeStatus::Failed };
	}
}

bool CSenpOwnerComposition::Poll(const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_entered) return false;
	Call call(m_entered);
	try {
		m_owners.Poll(now);
		return m_publications.Pump(now);
	} catch (...) {
		return false;
	}
}

bool CSenpOwnerComposition::Revoke(const std::wstring_view extensionId,
	const senp::effect::StopReason reason) noexcept
{
	if (m_closed || m_entered) return false;
	Call call(m_entered);
	try { return m_owners.Revoke(extensionId, reason); }
	catch (...) { return false; }
}

std::optional<senp::OwnerChangeResult> CSenpOwnerComposition::TakeTransition() noexcept
{
	if (m_entered) return {};
	try { return m_owners.TakeTransition(); }
	catch (...) { return {}; }
}

senp::ContributionOwnersSnapshot CSenpOwnerComposition::Snapshot() const noexcept
{
	return m_owners.Snapshot();
}

bool CSenpOwnerComposition::Close() noexcept
{
	if (m_closed) return true;
	if (m_entered) return false;
	Call call(m_entered);
	m_closed = true;
	const bool stopped = m_owners.Close();
	m_publications.Close();
	return stopped;
}

} // namespace workbench
