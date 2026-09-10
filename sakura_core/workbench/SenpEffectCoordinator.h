/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpOwnerRequests.h"
#include "workbench/tree/SenpTreeProvider.h"

namespace workbench {

enum class ESenpEffectTargetStatus : std::uint8_t {
	Applied,
	Retained,
	Rejected,
};

//! Native effect destination. Apply is invoked only after the contribution
//! owner's bounded Poll has returned. Retained keeps the request lineage alive
//! for a later derived ToolCompleted event; every other successful terminal is
//! released by the coordinator before Drain returns.
class ISenpEffectTarget {
public:
	virtual ~ISenpEffectTarget() = default;
	[[nodiscard]] virtual ESenpEffectTargetStatus Apply(
		const senp::effect::OperationContext& context,
		senp::effect::Effect effect,
		senp::CSenpRuntimeSession::Time now) noexcept = 0;
	[[nodiscard]] virtual bool Failed(
		const senp::effect::OperationContext& context,
		senp::InvocationStatus status,
		senp::CSenpRuntimeSession::Time now) noexcept = 0;
};

enum class ESenpEffectDrainStatus : std::uint8_t {
	Idle,
	Applied,
	Busy,
	Rejected,
	Closed,
};

class SenpEffectDrainResult final {
public:
	SenpEffectDrainResult(ESenpEffectDrainStatus status, std::size_t terminals) noexcept
		: m_status(status), m_terminals(terminals) {}
	[[nodiscard]] ESenpEffectDrainStatus Status() const noexcept { return m_status; }
	[[nodiscard]] std::size_t Terminals() const noexcept { return m_terminals; }
private:
	ESenpEffectDrainStatus m_status{ ESenpEffectDrainStatus::Idle };
	std::size_t m_terminals{};
};

//! UI-thread adapter shared by native Tree Views, readonly documents and owner
//! publication. Publication callbacks only call Publish. Composition calls
//! Drain after CSenpContributionOwners::Poll and owns revocation when Drain
//! reports Rejected. The coordinator performs no I/O and pumps no messages.
class CSenpEffectCoordinator final : public tree::ISenpTreeRuntime {
public:
	CSenpEffectCoordinator(senp::CSenpContributionOwners& owners,
		senp::ContributionOwnerIdentity owner, ISenpEffectTarget& target);
	~CSenpEffectCoordinator() override;
	CSenpEffectCoordinator(const CSenpEffectCoordinator&) = delete;
	CSenpEffectCoordinator& operator=(const CSenpEffectCoordinator&) = delete;

	[[nodiscard]] bool IsCurrent() const noexcept override;
	//! Drain refuses every submission it would reenter; this names that window.
	[[nodiscard]] bool CanSubmit() const noexcept override;
	[[nodiscard]] tree::SenpTreeAdmission Submit(senp::effect::TreeRequest request,
		senp::CSenpRuntimeSession::Time deadline) noexcept override;
	void Cancel(const senp::effect::OperationContext& context) noexcept override;
	[[nodiscard]] bool Execute(senp::effect::CommandInvoked command) noexcept override;

	[[nodiscard]] senp::OwnerRequestAdmission SubmitDocument(std::wstring resourceId,
		senp::CSenpRuntimeSession::Time deadline) noexcept;
	[[nodiscard]] senp::OwnerRequestAdmission SubmitVisibility(std::wstring viewId,
		bool visible, senp::CSenpRuntimeSession::Time deadline) noexcept;
	//! One complete workspace snapshot. The payload replaces whatever the
	//! extension last heard rather than amending it, so a repository that is gone
	//! is expressed by its absence and no retraction event is needed.
	[[nodiscard]] senp::OwnerRequestAdmission SubmitWorkspace(
		senp::effect::WorkspaceChanged workspace,
		senp::CSenpRuntimeSession::Time deadline) noexcept;
	[[nodiscard]] senp::OwnerRequestAdmission SubmitDerived(
		const senp::effect::OperationContext& request, senp::effect::ToolCompleted completed,
		senp::CSenpRuntimeSession::Time deadline) noexcept;
	//! Called synchronously by ISenpOwnerPublication::Apply while owner Poll is
	//! active. It validates and queues only; target callbacks are deferred.
	[[nodiscard]] bool Publish(senp::InvocationResult result) noexcept;
	[[nodiscard]] SenpEffectDrainResult Drain(senp::CSenpRuntimeSession::Time now) noexcept;
	//! Releases a target-retained lineage that will emit no derived invocation.
	[[nodiscard]] bool Finish(const senp::effect::OperationContext& request) noexcept;
	void Close() noexcept;
	[[nodiscard]] senp::OwnerRequestsSnapshot Snapshot() const noexcept;
	[[nodiscard]] const senp::ContributionOwnerIdentity& Owner() const noexcept { return m_owner; }

private:
	[[nodiscard]] senp::OwnerRequestAdmission SubmitEvent(senp::effect::Event event,
		senp::CSenpRuntimeSession::Time deadline) noexcept;

	senp::CSenpContributionOwners& m_owners;
	senp::ContributionOwnerIdentity m_owner;
	ISenpEffectTarget& m_target;
	senp::CSenpOwnerRequests m_requests;
	bool m_pumping{};
	bool m_closeRequested{};
	bool m_closed{};
	bool m_failed{};
};

} // namespace workbench
