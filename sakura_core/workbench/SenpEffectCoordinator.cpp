/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpEffectCoordinator.h"

#include <utility>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

class PumpGuard final {
public:
	explicit PumpGuard(bool& value) noexcept : m_value(value) { m_value = true; }
	~PumpGuard() { m_value = false; }
private:
	bool& m_value;
};
}

CSenpEffectCoordinator::CSenpEffectCoordinator(senp::CSenpContributionOwners& owners,
	senp::ContributionOwnerIdentity owner, ISenpEffectTarget& target)
	: m_owners(owners)
	, m_owner(std::move(owner))
	, m_target(target)
	, m_requests(owners, m_owner)
{
}

CSenpEffectCoordinator::~CSenpEffectCoordinator()
{
	Close();
}

bool CSenpEffectCoordinator::IsCurrent() const noexcept
{
	return !m_closed && !m_failed && m_requests.IsCurrent();
}

senp::OwnerRequestAdmission CSenpEffectCoordinator::SubmitEvent(senp::effect::Event event,
	const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	if (!IsCurrent() || m_pumping) return {};
	return m_requests.Submit(std::move(event), deadline);
}

tree::SenpTreeAdmission CSenpEffectCoordinator::Submit(senp::effect::TreeRequest request,
	const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	auto admission = SubmitEvent(std::move(request), deadline);
	return { admission.Status(), admission.Context() };
}

void CSenpEffectCoordinator::Cancel(const senp::effect::OperationContext& context) noexcept
{
	if (!m_closed) (void)m_requests.Cancel(context);
}

bool CSenpEffectCoordinator::Execute(senp::effect::CommandInvoked command) noexcept
{
	return SubmitEvent(std::move(command), Clock::now() + senp::CSenpRuntimeSession::kMaximumLifetime).Status()
		== senp::AdmissionStatus::Accepted;
}

senp::OwnerRequestAdmission CSenpEffectCoordinator::SubmitDocument(std::wstring resourceId,
	const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	return SubmitEvent(senp::effect::DocumentRequest{ std::move(resourceId) }, deadline);
}

senp::OwnerRequestAdmission CSenpEffectCoordinator::SubmitVisibility(std::wstring viewId,
	const bool visible, const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	return SubmitEvent(senp::effect::VisibilityChanged{ std::move(viewId), visible }, deadline);
}

senp::OwnerRequestAdmission CSenpEffectCoordinator::SubmitWorkspace(
	senp::effect::WorkspaceChanged workspace, const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	return SubmitEvent(std::move(workspace), deadline);
}

senp::OwnerRequestAdmission CSenpEffectCoordinator::SubmitDerived(
	const senp::effect::OperationContext& request, senp::effect::ToolCompleted completed,
	const senp::CSenpRuntimeSession::Time deadline) noexcept
{
	if (!IsCurrent() || m_pumping) return {};
	return m_requests.SubmitDerived(request, std::move(completed), deadline);
}

bool CSenpEffectCoordinator::Publish(senp::InvocationResult result) noexcept
{
	return !m_closed && !m_failed && m_requests.Publish(std::move(result));
}

SenpEffectDrainResult CSenpEffectCoordinator::Drain(const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_failed || !m_requests.IsCurrent()) return { ESenpEffectDrainStatus::Closed, 0 };
	if (m_pumping) return { ESenpEffectDrainStatus::Busy, 0 };
	PumpGuard guard(m_pumping);
	std::size_t terminals{};
	for (; terminals < senp::CSenpRuntimeSession::kMaximumPending; ++terminals) {
		auto result = m_requests.TakeCompleted();
		if (!result) break;
		bool retained{};
		bool accepted = true;
		if (result->status == senp::InvocationStatus::EffectsReady) {
			for (auto& effect : result->effects) {
				const auto applied = m_target.Apply(result->context, std::move(effect), now);
				if (applied == ESenpEffectTargetStatus::Rejected) {
					accepted = false;
					break;
				}
				retained = retained || applied == ESenpEffectTargetStatus::Retained;
			}
		} else {
			accepted = m_target.Failed(result->context, result->status, now);
		}
		if (!accepted) {
			(void)m_requests.Cancel(result->context);
			m_failed = true;
			return { ESenpEffectDrainStatus::Rejected, terminals + 1 };
		}
		if (m_closeRequested) {
			m_closed = true;
			m_requests.Close();
			return { ESenpEffectDrainStatus::Closed, terminals + 1 };
		}
		if (!retained) (void)m_requests.Finish(result->context);
	}
	return { terminals == 0 ? ESenpEffectDrainStatus::Idle : ESenpEffectDrainStatus::Applied,
		terminals };
}

bool CSenpEffectCoordinator::Finish(const senp::effect::OperationContext& request) noexcept
{
	return !m_closed && !m_pumping && m_requests.Finish(request);
}

void CSenpEffectCoordinator::Close() noexcept
{
	if (m_closed) return;
	if (m_pumping) {
		m_closeRequested = true;
		return;
	}
	m_closed = true;
	m_requests.Close();
}

senp::OwnerRequestsSnapshot CSenpEffectCoordinator::Snapshot() const noexcept
{
	return m_requests.Snapshot();
}

} // namespace workbench
