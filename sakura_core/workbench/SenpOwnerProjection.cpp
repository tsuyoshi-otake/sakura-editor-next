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
}

class CSenpOwnerProjection::RuntimePort final : public tree::ISenpTreeRuntime {
public:
	void Bind(CSenpEffectCoordinator& coordinator) noexcept { m_coordinator = &coordinator; }
	void Clear() noexcept { m_coordinator = nullptr; }
	[[nodiscard]] bool IsCurrent() const noexcept override { return m_coordinator && m_coordinator->IsCurrent(); }
	[[nodiscard]] tree::SenpTreeAdmission Submit(senp::effect::TreeRequest request,
		senp::CSenpRuntimeSession::Time deadline) noexcept override
	{
		return m_coordinator ? m_coordinator->Submit(std::move(request), deadline) : tree::SenpTreeAdmission{};
	}
	void Cancel(const senp::effect::OperationContext& context) noexcept override
	{
		if (m_coordinator) m_coordinator->Cancel(context);
	}
	[[nodiscard]] bool Execute(senp::effect::CommandInvoked command) noexcept override
	{
		return m_coordinator && m_coordinator->Execute(std::move(command));
	}
private:
	CSenpEffectCoordinator* m_coordinator{};
};

CSenpOwnerProjection::CSenpOwnerProjection(senp::CSenpContributionOwners& owners,
	senp::ContributionOwnerIdentity owner, ISenpOwnerProjectionTarget& target)
	: m_owner(std::move(owner)), m_target(target), m_runtime(std::make_shared<RuntimePort>())
	, m_coordinator(std::make_unique<CSenpEffectCoordinator>(owners, m_owner,
		static_cast<ISenpEffectTarget&>(*this)))
{
	m_runtime->Bind(*m_coordinator);
}

CSenpOwnerProjection::~CSenpOwnerProjection() { Close(); }

bool CSenpOwnerProjection::RegisterTree(std::wstring viewId, std::vector<std::wstring> commands)
{
	if (m_closed || m_pumping || viewId.empty() || m_trees.contains(viewId)) return false;
	tree::SenpTreeProviderOptions options{
		std::move(viewId),
		{ m_owner.generation, m_owner.workspaceRevision, m_owner.accountGeneration },
		std::move(commands), m_runtime,
	};
	auto provider = std::make_shared<tree::SenpTreeProvider>(std::move(options));
	return m_trees.emplace(std::wstring(provider->ViewId()), std::move(provider)).second;
}

std::shared_ptr<tree::SenpTreeProvider> CSenpOwnerProjection::Tree(std::wstring_view viewId) const noexcept
{
	const auto found = m_trees.find(viewId);
	return found == m_trees.end() ? nullptr : found->second;
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
			return result == tree::TreeResult::Applied || result == tree::TreeResult::Unchanged
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected;
		} else if constexpr (std::is_same_v<T, senp::effect::InvalidateTree>) {
			const auto found = m_trees.find(value.viewId);
			if (found == m_trees.end()) return ESenpEffectTargetStatus::Rejected;
			found->second->Refresh(now);
			return ESenpEffectTargetStatus::Applied;
		} else if constexpr (std::is_same_v<T, senp::effect::OpenDocument>) {
			if (value.resourceId.empty() || m_documentQueue.size() + m_documents.size() >= 16)
				return ESenpEffectTargetStatus::Rejected;
			const bool known = std::ranges::any_of(m_documentQueue, [&](const auto& item) { return item == value.resourceId; })
				|| std::ranges::any_of(m_documents, [&](const auto& item) { return item.second.second == value.resourceId; });
			if (!known) m_documentQueue.push_back(std::move(value.resourceId));
			return ESenpEffectTargetStatus::Applied;
		} else if constexpr (std::is_same_v<T, senp::effect::PublishDocument>) {
			const auto found = m_documents.find(context.operationId);
			if (found == m_documents.end() || !SameContext(found->second.first, context)
				|| found->second.second != value.resourceId)
				return ESenpEffectTargetStatus::Rejected;
			const bool accepted = m_target.PublishDocument(context, std::move(value));
			m_documents.erase(found);
			return accepted ? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected;
		} else if constexpr (std::is_same_v<T, senp::effect::CompleteCommand>) {
			return m_target.CompleteCommand(context, std::move(value))
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected;
		} else if constexpr (std::is_same_v<T, senp::effect::ReleaseResource>) {
			return m_target.ReleaseResource(value.handle)
				? ESenpEffectTargetStatus::Applied : ESenpEffectTargetStatus::Rejected;
		} else {
			return ESenpEffectTargetStatus::Rejected;
		}
	}, std::move(effect));
	} catch (...) {
		return ESenpEffectTargetStatus::Rejected;
	}
}

bool CSenpOwnerProjection::Failed(const senp::effect::OperationContext& context,
	const senp::InvocationStatus status, const senp::CSenpRuntimeSession::Time now) noexcept
{
	const auto document = m_documents.find(context.operationId);
	if (document != m_documents.end()) {
		const bool accepted = m_target.FailDocument(context, status);
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
	m_documents.clear();
	m_documentQueue.clear();
	m_target.Revoke();
}

} // namespace workbench
