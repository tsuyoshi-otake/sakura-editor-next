/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpOwnerProjection.h"

#include <algorithm>
#include <utility>

namespace workbench {
namespace {
bool SameContext(const senp::effect::OperationContext& left,
	const senp::effect::OperationContext& right) noexcept
{
	return left == right;
}
bool SameRequest(const senp::effect::OperationContext& left,
	const senp::effect::OperationContext& right) noexcept
{
	return left.ownerGeneration == right.ownerGeneration
		&& left.requestGeneration == right.requestGeneration;
}
}

class CSenpOwnerProjection::RuntimePort final : public tree::ISenpTreeRuntime {
public:
	void Bind(CSenpOwnerProjection& projection, CSenpEffectCoordinator& coordinator) noexcept
	{
		m_projection = &projection; m_coordinator = &coordinator;
	}
	void Clear() noexcept { m_projection = nullptr; m_coordinator = nullptr; }
	[[nodiscard]] bool IsCurrent() const noexcept override { return m_coordinator && m_coordinator->IsCurrent(); }
	[[nodiscard]] bool CanSubmit() const noexcept override { return m_coordinator && m_coordinator->CanSubmit(); }
	[[nodiscard]] tree::SenpTreeAdmission Submit(senp::effect::TreeRequest request,
		senp::CSenpRuntimeSession::Time deadline) noexcept override
	{
		return m_coordinator ? m_coordinator->Submit(std::move(request), deadline) : tree::SenpTreeAdmission{};
	}
	void Cancel(const senp::effect::OperationContext& context) noexcept override
	{
		if (m_projection) m_projection->Cancel(context);
	}
	[[nodiscard]] bool Execute(senp::effect::CommandInvoked command) noexcept override
	{
		return m_coordinator && m_coordinator->Execute(std::move(command));
	}
private:
	CSenpOwnerProjection* m_projection{};
	CSenpEffectCoordinator* m_coordinator{};
};

CSenpOwnerProjection::CSenpOwnerProjection(senp::CSenpContributionOwners& owners,
	senp::ContributionOwnerIdentity owner, ISenpOwnerProjectionTarget& target)
	: m_owner(std::move(owner)), m_target(target), m_runtime(std::make_shared<RuntimePort>())
	, m_coordinator(std::make_unique<CSenpEffectCoordinator>(owners, m_owner,
		static_cast<ISenpEffectTarget&>(*this)))
{
	m_runtime->Bind(*this, *m_coordinator);
}

CSenpOwnerProjection::~CSenpOwnerProjection() { Close(); }

bool CSenpOwnerProjection::RegisterTree(std::wstring viewId, std::vector<std::wstring> commands,
	std::vector<tree::SenpTreeItemAction> itemActions)
{
	if (m_closed || m_pumping || viewId.empty() || m_trees.contains(viewId)) return false;
	tree::SenpTreeProviderOptions options{
		std::move(viewId),
		{ m_owner.generation, m_owner.workspaceRevision, m_owner.accountGeneration },
		std::move(commands), m_runtime, std::move(itemActions),
	};
	auto provider = std::make_shared<tree::SenpTreeProvider>(std::move(options));
	return m_trees.emplace(std::wstring(provider->ViewId()), std::move(provider)).second;
}

std::shared_ptr<tree::SenpTreeProvider> CSenpOwnerProjection::Tree(std::wstring_view viewId) const noexcept
{
	const auto found = m_trees.find(viewId);
	return found == m_trees.end() ? nullptr : found->second;
}

bool CSenpOwnerProjection::PublishWorkspace(senp::effect::WorkspaceChanged workspace) noexcept
{
	if (m_closed || m_closeRequested) return false;
	if (!senp::effect::ValidateWorkspace(workspace)) return false;
	try {
		m_workspace = std::move(workspace);
		return true;
	} catch (const std::exception&) {
		// Only the std::optional assignment above can throw here.
		return false;
	}
}

bool CSenpOwnerProjection::Publish(senp::InvocationResult result) noexcept
{
	return !m_closed && m_coordinator->Publish(std::move(result));
}

bool CSenpOwnerProjection::IsCurrent() const noexcept
{
	return !m_closed && m_coordinator->IsCurrent();
}

ESenpOwnerProjectionStatus CSenpOwnerProjection::Pump(const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed) return ESenpOwnerProjectionStatus::Closed;
	if (m_pumping) return ESenpOwnerProjectionStatus::Busy;
	m_pumping = true;
	const auto drained = m_coordinator->Drain(now);
	if (drained.Status() == ESenpEffectDrainStatus::Rejected || drained.Status() == ESenpEffectDrainStatus::Closed) {
		m_pumping = false;
		FinishClose();
		return drained.Status() == ESenpEffectDrainStatus::Rejected
			? ESenpOwnerProjectionStatus::Rejected : ESenpOwnerProjectionStatus::Closed;
	}
	bool applied = drained.Status() == ESenpEffectDrainStatus::Applied;
	try {
	if (m_workspace) {
		const auto admission = m_coordinator->SubmitWorkspace(*m_workspace,
			now + senp::CSenpRuntimeSession::kMaximumLifetime);
		if (admission.Status() == senp::AdmissionStatus::Accepted) {
			m_workspace.reset();
			applied = true;
		} else if (admission.Status() != senp::AdmissionStatus::Busy) {
			// The payload was validated when it was queued, so a refusal here is
			// the owner generation ending rather than a bad snapshot. Dropping it
			// keeps a retired owner from retrying the same event every turn; the
			// window republishes into the owner that replaces this one.
			m_workspace.reset();
		}
	}
	while (!m_documentQueue.empty()) {
		auto resource = std::move(m_documentQueue.front());
		m_documentQueue.erase(m_documentQueue.begin());
		const auto admission = m_coordinator->SubmitDocument(resource,
			now + senp::CSenpRuntimeSession::kMaximumLifetime);
		if (admission.Status() == senp::AdmissionStatus::Busy) {
			m_documentQueue.insert(m_documentQueue.begin(), std::move(resource));
			break;
		}
		if (admission.Status() != senp::AdmissionStatus::Accepted
			|| !m_target.BeginDocument(resource, admission.Context())) {
			if (admission.Status() == senp::AdmissionStatus::Accepted) m_coordinator->Cancel(admission.Context());
			m_pumping = false;
			FinishClose();
			return ESenpOwnerProjectionStatus::Rejected;
		}
		if (m_closeRequested) {
			m_coordinator->Cancel(admission.Context());
			m_pumping = false;
			FinishClose();
			return ESenpOwnerProjectionStatus::Closed;
		}
		if (!m_documents.emplace(admission.Context().operationId,
			std::make_pair(admission.Context(), std::move(resource))).second) {
			m_coordinator->Cancel(admission.Context());
			m_pumping = false;
			FinishClose();
			return ESenpOwnerProjectionStatus::Rejected;
		}
		applied = true;
	}
	while (m_toolCompletions.size() < senp::CSenpRuntimeSession::kMaximumPending) {
		auto terminal = m_target.TakeToolRead();
		if (!terminal) break;
		const auto found = m_toolReads.find(terminal->m_completion.readId);
		if (found == m_toolReads.end() || !SameContext(found->second, terminal->m_context)) {
			m_pumping = false;
			FinishClose();
			return ESenpOwnerProjectionStatus::Rejected;
		}
		m_toolCompletions.push_back(std::move(*terminal));
	}
	while (!m_toolCompletions.empty()) {
		auto& terminal = m_toolCompletions.front();
		const auto admission = m_coordinator->SubmitDerived(terminal.m_context,
			terminal.m_completion, now + senp::CSenpRuntimeSession::kMaximumLifetime);
		if (admission.Status() == senp::AdmissionStatus::Busy) break;
		if (admission.Status() != senp::AdmissionStatus::Accepted) {
			m_pumping = false;
			FinishClose();
			return ESenpOwnerProjectionStatus::Rejected;
		}
		m_toolReads.erase(terminal.m_completion.readId);
		m_toolCompletions.erase(m_toolCompletions.begin());
		applied = true;
	}
	for (const auto& [id, provider] : m_trees) provider->Pump(now);
	} catch (...) {
		m_pumping = false;
		FinishClose();
		return ESenpOwnerProjectionStatus::Rejected;
	}
	m_pumping = false;
	if (m_closeRequested) {
		FinishClose();
		return ESenpOwnerProjectionStatus::Closed;
	}
	return applied ? ESenpOwnerProjectionStatus::Applied : ESenpOwnerProjectionStatus::Idle;
}

ESenpEffectTargetStatus CSenpOwnerProjection::Apply(const senp::effect::OperationContext& context,
	senp::effect::Effect effect, const senp::CSenpRuntimeSession::Time now) noexcept
{
	try {
	return std::visit([&](auto value) -> ESenpEffectTargetStatus {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, senp::effect::PublishTreePage>) {
			const auto found = m_trees.find(value.viewId);
			if (found == m_trees.end()) return ESenpEffectTargetStatus::Rejected;
			const auto result = found->second->Apply(context, std::move(value), now);
			return PreserveLineage(context,
				result == tree::TreeResult::Applied || result == tree::TreeResult::Unchanged
					? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected);
		} else if constexpr (std::is_same_v<T, senp::effect::InvalidateTree>) {
			const auto found = m_trees.find(value.viewId);
			if (found == m_trees.end()) return ESenpEffectTargetStatus::Rejected;
			found->second->Refresh(now);
			return PreserveLineage(context, ESenpEffectTargetStatus::Applied);
		} else if constexpr (std::is_same_v<T, senp::effect::OpenDocument>) {
			if (value.resourceId.empty() || m_documentQueue.size() + m_documents.size() >= 16)
				return ESenpEffectTargetStatus::Rejected;
			const bool known = std::ranges::any_of(m_documentQueue, [&](const auto& item) { return item == value.resourceId; })
				|| std::ranges::any_of(m_documents, [&](const auto& item) { return item.second.second == value.resourceId; });
			if (!known) m_documentQueue.push_back(std::move(value.resourceId));
			return PreserveLineage(context, ESenpEffectTargetStatus::Applied);
		} else if constexpr (std::is_same_v<T, senp::effect::PublishDocument>) {
			const auto found = std::ranges::find_if(m_documents, [&](const auto& document) {
				return SameRequest(document.second.first, context)
					&& document.second.second == value.resourceId;
			});
			if (found == m_documents.end())
				return ESenpEffectTargetStatus::Rejected;
			const auto original = found->second.first;
			const bool accepted = m_target.PublishDocument(original, std::move(value));
			m_documents.erase(found);
			return PreserveLineage(original, accepted
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected);
		} else if constexpr (std::is_same_v<T, senp::effect::CompleteCommand>) {
			return PreserveLineage(context, m_target.CompleteCommand(context, std::move(value))
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected);
		} else if constexpr (std::is_same_v<T, senp::effect::ReleaseResource>) {
			return PreserveLineage(context, m_target.ReleaseResource(value.handle)
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected);
		} else if constexpr (std::is_same_v<T, senp::effect::StartToolRead>) {
			if (m_toolReads.size() >= senp::CSenpRuntimeSession::kMaximumPending
				|| m_toolReads.contains(value.readId)) return ESenpEffectTargetStatus::Rejected;
			const auto readId = value.readId;
			if (!m_toolReads.emplace(readId, context).second) return ESenpEffectTargetStatus::Rejected;
			if (!m_target.StartToolRead(context, std::move(value))) {
				m_toolReads.erase(readId);
				return ESenpEffectTargetStatus::Rejected;
			}
			return ESenpEffectTargetStatus::Retained;
		} else {
			return ESenpEffectTargetStatus::Rejected;
		}
	}, std::move(effect));
	} catch (const std::exception&) {
		// ISenpOwnerProjectionTarget's virtuals are all declared noexcept, so only
		// the std::visit/container bookkeeping above can throw here.
		return ESenpEffectTargetStatus::Rejected;
	}
}

ESenpEffectTargetStatus CSenpOwnerProjection::PreserveLineage(
	const senp::effect::OperationContext& context, const ESenpEffectTargetStatus status) const noexcept
{
	if (status != ESenpEffectTargetStatus::Applied) return status;
	const bool pending = std::ranges::any_of(m_toolReads, [&](const auto& read) {
		return read.second.ownerGeneration == context.ownerGeneration
			&& read.second.requestGeneration == context.requestGeneration;
	});
	return pending ? ESenpEffectTargetStatus::Retained : status;
}

void CSenpOwnerProjection::Cancel(const senp::effect::OperationContext& context) noexcept
{
	if (m_closed) return;
	m_target.CancelToolReads(context);
	for (auto it = m_toolReads.begin(); it != m_toolReads.end();) {
		if (it->second.ownerGeneration == context.ownerGeneration
			&& it->second.requestGeneration == context.requestGeneration) it = m_toolReads.erase(it);
		else ++it;
	}
	std::erase_if(m_toolCompletions, [&](const auto& terminal) {
		return terminal.m_context.ownerGeneration == context.ownerGeneration
			&& terminal.m_context.requestGeneration == context.requestGeneration;
	});
	m_coordinator->Cancel(context);
}

bool CSenpOwnerProjection::Failed(const senp::effect::OperationContext& context,
	const senp::InvocationStatus status, const senp::CSenpRuntimeSession::Time now) noexcept
{
	const auto document = std::ranges::find_if(m_documents, [&](const auto& value) {
		return SameRequest(value.second.first, context);
	});
	if (document != m_documents.end()) {
		const bool accepted = m_target.FailDocument(document->second.first, status);
		m_documents.erase(document);
		return accepted;
	}
	for (const auto& [id, provider] : m_trees) {
		if (provider->Failed(context, status, now) == tree::TreeResult::Applied) return true;
	}
	return false;
}

void CSenpOwnerProjection::Close() noexcept
{
	if (m_closed) return;
	if (m_pumping) {
		m_closeRequested = true;
		m_coordinator->Close();
		return;
	}
	FinishClose();
}

void CSenpOwnerProjection::FinishClose() noexcept
{
	if (m_closed) return;
	m_closed = true;
	m_runtime->Clear();
	for (auto& [id, provider] : m_trees) provider->Close();
	m_coordinator->Close();
	m_workspace.reset();
	m_documents.clear();
	m_documentQueue.clear();
	m_toolCompletions.clear();
	m_toolReads.clear();
	m_target.Revoke();
}

} // namespace workbench
