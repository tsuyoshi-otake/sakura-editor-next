/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpEffectCoordinator.h"

#include <map>
#include <memory>

namespace workbench {

//! Native document/command boundary used by one owner projection. Begin is
//! called only after a DocumentRequest was admitted. Every accepted Begin is
//! followed by Publish, Failed, or Revoke.
class ISenpOwnerProjectionTarget {
public:
	virtual ~ISenpOwnerProjectionTarget() = default;
	[[nodiscard]] virtual bool BeginDocument(std::wstring_view resourceId,
		const senp::effect::OperationContext& context) noexcept = 0;
	[[nodiscard]] virtual bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept = 0;
	[[nodiscard]] virtual bool FailDocument(const senp::effect::OperationContext& context,
		senp::InvocationStatus status) noexcept = 0;
	[[nodiscard]] virtual bool CompleteCommand(const senp::effect::OperationContext& context,
		senp::effect::CompleteCommand completion) noexcept = 0;
	[[nodiscard]] virtual bool ReleaseResource(std::wstring_view handle) noexcept = 0;
	virtual void Revoke() noexcept = 0;
};

enum class ESenpOwnerProjectionStatus : std::uint8_t {
	Idle, Applied, Busy, Rejected, Closed,
};

//! One committed owner generation projected into native Tree providers and a
//! readonly-document target. The shared runtime port is inert until construction
//! completes and is cleared before providers/targets are revoked.
class CSenpOwnerProjection final : private ISenpEffectTarget {
public:
	CSenpOwnerProjection(senp::CSenpContributionOwners& owners,
		senp::ContributionOwnerIdentity owner, ISenpOwnerProjectionTarget& target);
	~CSenpOwnerProjection();
	CSenpOwnerProjection(const CSenpOwnerProjection&) = delete;
	CSenpOwnerProjection& operator=(const CSenpOwnerProjection&) = delete;

	[[nodiscard]] bool RegisterTree(std::wstring viewId, std::vector<std::wstring> commands);
	[[nodiscard]] std::shared_ptr<tree::SenpTreeProvider> Tree(std::wstring_view viewId) const noexcept;
	//! Call after contribution-owner Poll. It drains terminals, then admits queued
	//! document requests. No method waits, performs I/O, or pumps window messages.
	[[nodiscard]] ESenpOwnerProjectionStatus Pump(senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] bool Publish(senp::InvocationResult result) noexcept;
	[[nodiscard]] bool IsCurrent() const noexcept;
	void Close() noexcept;

private:
	class RuntimePort;
	[[nodiscard]] ESenpEffectTargetStatus Apply(const senp::effect::OperationContext& context,
		senp::effect::Effect effect, senp::CSenpRuntimeSession::Time now) noexcept override;
	[[nodiscard]] bool Failed(const senp::effect::OperationContext& context,
		senp::InvocationStatus status, senp::CSenpRuntimeSession::Time now) noexcept override;
	void FinishClose() noexcept;

	senp::ContributionOwnerIdentity m_owner;
	ISenpOwnerProjectionTarget& m_target;
	std::shared_ptr<RuntimePort> m_runtime;
	std::unique_ptr<CSenpEffectCoordinator> m_coordinator;
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> m_trees;
	std::vector<std::wstring> m_documentQueue;
	std::map<std::wstring, std::pair<senp::effect::OperationContext, std::wstring>, std::less<>> m_documents;
	bool m_pumping{};
	bool m_closeRequested{};
	bool m_closed{};
};

} // namespace workbench
