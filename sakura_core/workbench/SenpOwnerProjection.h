/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpEffectCoordinator.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace workbench {

class SenpToolReadTerminal final {
public:
	SenpToolReadTerminal(senp::effect::OperationContext context,
		senp::effect::ToolCompleted completion)
		: m_context(std::move(context)), m_completion(std::move(completion)) {}
	[[nodiscard]] const senp::effect::OperationContext& Context() const noexcept { return m_context; }
	[[nodiscard]] const senp::effect::ToolCompleted& Completion() const noexcept { return m_completion; }
private:
	friend class CSenpOwnerProjection;
	senp::effect::OperationContext m_context;
	senp::effect::ToolCompleted m_completion;
};

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
	//! Starts one broker-authorized read without waiting. The target owns its
	//! cancellation and returns completions only from TakeToolRead on the UI thread.
	[[nodiscard]] virtual bool StartToolRead(const senp::effect::OperationContext&,
		senp::effect::StartToolRead) noexcept { return false; }
	[[nodiscard]] virtual std::optional<SenpToolReadTerminal> TakeToolRead() noexcept { return {}; }
	virtual void CancelToolReads(const senp::effect::OperationContext&) noexcept {}
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

	[[nodiscard]] bool RegisterTree(std::wstring viewId, std::vector<std::wstring> commands,
		std::vector<tree::SenpTreeItemAction> itemActions = {});
	[[nodiscard]] std::shared_ptr<tree::SenpTreeProvider> Tree(std::wstring_view viewId) const noexcept;
	//! Call after contribution-owner Poll. It drains terminals, then admits queued
	//! document requests. No method waits, performs I/O, or pumps window messages.
	[[nodiscard]] ESenpOwnerProjectionStatus Pump(senp::CSenpRuntimeSession::Time now) noexcept;
	//! Queues one workspace snapshot for this owner. A newer snapshot replaces an
	//! unsent one: the event carries the whole workspace, so delivering an older
	//! list after a newer one would be wrong rather than merely late. Submission
	//! happens in Pump, where the request broker's back-pressure already applies
	//! and where reentering the owner service is impossible. False means the
	//! payload is not one the wire accepts, or that this projection is closing.
	[[nodiscard]] bool PublishWorkspace(senp::effect::WorkspaceChanged workspace) noexcept;
	[[nodiscard]] bool Publish(senp::InvocationResult result) noexcept;
	[[nodiscard]] bool IsCurrent() const noexcept;
	void Close() noexcept;

private:
	class RuntimePort;
	[[nodiscard]] ESenpEffectTargetStatus Apply(const senp::effect::OperationContext& context,
		senp::effect::Effect effect, senp::CSenpRuntimeSession::Time now) noexcept override;
	[[nodiscard]] bool Failed(const senp::effect::OperationContext& context,
		senp::InvocationStatus status, senp::CSenpRuntimeSession::Time now) noexcept override;
	[[nodiscard]] ESenpEffectTargetStatus PreserveLineage(
		const senp::effect::OperationContext& context, ESenpEffectTargetStatus status) const noexcept;
	void Cancel(const senp::effect::OperationContext& context) noexcept;
	void FinishClose() noexcept;

	senp::ContributionOwnerIdentity m_owner;
	ISenpOwnerProjectionTarget& m_target;
	std::shared_ptr<RuntimePort> m_runtime;
	std::unique_ptr<CSenpEffectCoordinator> m_coordinator;
	std::map<std::wstring, std::shared_ptr<tree::SenpTreeProvider>, std::less<>> m_trees;
	std::optional<senp::effect::WorkspaceChanged> m_workspace;
	std::vector<std::wstring> m_documentQueue;
	std::map<std::wstring, std::pair<senp::effect::OperationContext, std::wstring>, std::less<>> m_documents;
	std::map<std::wstring, senp::effect::OperationContext, std::less<>> m_toolReads;
	std::vector<SenpToolReadTerminal> m_toolCompletions;
	bool m_pumping{};
	bool m_closeRequested{};
	bool m_closed{};
};

} // namespace workbench
