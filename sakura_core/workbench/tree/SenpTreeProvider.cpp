/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/tree/SenpTreeProvider.h"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace workbench::tree {
namespace {
bool Id(std::wstring_view id)
{
	TreeItem value; value.id = id; value.label = L"id";
	return TreeViewModel::ValidItem(value);
}
TreeItem Item(senp::effect::TreeItem value)
{
	TreeItem item;
	item.id = std::move(value.id); item.label = std::move(value.label); item.description = std::move(value.description);
	item.tooltip = std::move(value.tooltip); item.icon = std::move(value.icon); item.commandId = std::move(value.commandId);
	item.arguments = std::move(value.arguments);
	switch (value.collapsibleState) {
	case senp::effect::CollapsibleState::Leaf: item.collapsibleState = TreeItemCollapsibleState::None; break;
	case senp::effect::CollapsibleState::Collapsed: item.collapsibleState = TreeItemCollapsibleState::Collapsed; break;
	case senp::effect::CollapsibleState::Expanded: item.collapsibleState = TreeItemCollapsibleState::Expanded; break;
	default: item.collapsibleState = static_cast<TreeItemCollapsibleState>(255); break;
	}
	return item;
}
}

struct SenpTreeProvider::Impl {
	struct Pending final { TreeLoadRequest request; senp::effect::OperationContext context; Time deadline; };
	SenpTreeProviderOptions options;
	TreeViewModel model;
	std::map<std::uint64_t, Pending> pending;
	std::set<std::wstring, std::less<>> commands;
	ISenpTreeObserver* observer{};
	std::int64_t lastRequestGeneration{};
	bool visible{}, pumping{};
	explicit Impl(SenpTreeProviderOptions value) : options(std::move(value))
	{
		if (!options.runtime || !Id(options.viewId) || options.scope.ownerGeneration <= 0
			|| options.scope.workspaceRevision < 0 || options.scope.accountGeneration < 0 || options.commands.size() > 256)
			throw std::invalid_argument("Invalid SENP tree provider options.");
		for (const auto& command : options.commands) if (!Id(command) || !commands.insert(command).second)
			throw std::invalid_argument("Invalid SENP tree command declaration.");
	}
	bool Current() const noexcept { return !model.IsClosed() && options.runtime->IsCurrent(); }
	bool Scope(const senp::effect::OperationContext& context) const noexcept
	{
		return context.ownerGeneration == options.scope.ownerGeneration && context.workspaceRevision == options.scope.workspaceRevision
			&& context.accountGeneration == options.scope.accountGeneration && context.requestGeneration > 0;
	}
	void Changed(std::wstring_view parentId) noexcept { if (observer) observer->TreeChanged(parentId); }
	void CancelTickets(const std::vector<std::uint64_t>& tickets) noexcept
	{
		for (const auto ticket : tickets) if (const auto it = pending.find(ticket); it != pending.end()) {
			options.runtime->Cancel(it->second.context); pending.erase(it);
		}
	}
	void Close() noexcept
	{
		visible = false;
		// Cancel transport subscribers before freeing their context; the runtime
		// owns physical cleanup even after this projection's authority is revoked.
		for (const auto& [ticket, request] : pending) options.runtime->Cancel(request.context);
		pending.clear();
		(void)model.Close();
		Changed(L"");
	}
	TreeResult Load(std::wstring_view parentId, TreeLoadKind kind, Time now)
	{
		if (!Current() || !visible) return TreeResult::Unavailable;
		auto admission = model.Begin(parentId, kind);
		if (!admission.request) return admission.result;
		const auto ticket = admission.request->ticket;
		// Reserve ownership before Submit can accept any work.
		auto [it, inserted] = pending.emplace(ticket, Pending{ std::move(*admission.request), {}, now + kLoadLifetime });
		auto& request = it->second;
		auto submitted = options.runtime->Submit({ options.viewId, request.request.parentId, request.request.cursor }, now + senp::CSenpRuntimeSession::kMaximumLifetime);
		if (submitted.status != senp::AdmissionStatus::Accepted) {
			(void)model.Fail(request.request, submitted.status == senp::AdmissionStatus::Busy
				? L"The provider is busy. Retry when it is available." : L"The provider is unavailable.");
			pending.erase(it); Changed(parentId);
			return submitted.status == senp::AdmissionStatus::Busy ? TreeResult::Busy : TreeResult::Unavailable;
		}
		if (!Scope(submitted.context) || !Id(submitted.context.operationId) || submitted.context.operationId.size() > 96
			|| submitted.context.requestGeneration <= lastRequestGeneration) {
			options.runtime->Cancel(submitted.context);
			(void)model.Fail(request.request, L"The provider returned an invalid request identity.");
			pending.erase(it); Changed(parentId); return TreeResult::Invalid;
		}
		lastRequestGeneration = submitted.context.requestGeneration;
		request.context = std::move(submitted.context);
		Changed(parentId); return TreeResult::Accepted;
	}
	std::map<std::uint64_t, Pending>::iterator Find(const senp::effect::OperationContext& context)
	{
		if (!Current() || !Scope(context)) return pending.end();
		return std::find_if(pending.begin(), pending.end(), [&](const auto& entry) { return entry.second.context.requestGeneration == context.requestGeneration; });
	}
};

SenpTreeProvider::SenpTreeProvider(SenpTreeProviderOptions options) : m_impl(std::make_unique<Impl>(std::move(options))) {}
SenpTreeProvider::~SenpTreeProvider() { m_impl->observer = nullptr; m_impl->Close(); }
bool SenpTreeProvider::Observe(ISenpTreeObserver& observer) noexcept
{
	if (m_impl->observer && m_impl->observer != &observer) return false;
	m_impl->observer = &observer; return true;
}
void SenpTreeProvider::Unobserve(ISenpTreeObserver& observer) noexcept { if (m_impl->observer == &observer) m_impl->observer = nullptr; }
void SenpTreeProvider::SetVisible(bool visible, Time now)
{
	if (m_impl->model.IsClosed()) return;
	if (!m_impl->Current()) { Close(); return; }
	m_impl->visible = visible;
	if (!visible) { m_impl->CancelTickets(m_impl->model.CancelAll()); m_impl->Changed(L""); }
	else Pump(now);
}
void SenpTreeProvider::Pump(Time now)
{
	auto& state = *m_impl;
	if (state.pumping || state.model.IsClosed()) return;
	if (!state.Current()) { Close(); return; }
	for (auto it = state.pending.begin(); it != state.pending.end();) {
		if (now < it->second.deadline) { ++it; continue; }
		const auto parent = it->second.request.parentId;
		(void)state.model.Fail(it->second.request, L"The tree request timed out. Retry to try again.");
		state.options.runtime->Cancel(it->second.context); it = state.pending.erase(it);
		state.Changed(parent);
	}
	// While the runtime delivers results, demand stays queued in the model; the
	// owner pumps every subscriber again once delivery returns.
	if (!state.visible || !state.options.runtime->CanSubmit()) return;
	// One pass admits at most eight visible demands. Failed admissions are terminal,
	// never put back into an immediate retry loop by paint, timer or other results.
	struct DispatchGuard final { bool& flag; explicit DispatchGuard(bool& value) : flag(value) { flag = true; } ~DispatchGuard() { flag = false; } } guard(state.pumping);
	for (const auto& parent : state.model.Demand()) {
		if (state.pending.size() == kMaximumTreeLoads) break;
		(void)state.Load(parent, TreeLoadKind::Initial, now);
	}
}
TreeResult SenpTreeProvider::SetExpanded(std::wstring_view id, bool expanded, Time now)
{
	if (!m_impl->Current()) { Close(); return TreeResult::Unavailable; }
	const auto changed = m_impl->model.SetExpanded(id, expanded);
	m_impl->CancelTickets(changed.cancelled); m_impl->Changed(id); Pump(now);
	return changed.result;
}
TreeResult SenpTreeProvider::LoadNext(std::wstring_view parentId, Time now) { return m_impl->Load(parentId, TreeLoadKind::NextPage, now); }
TreeResult SenpTreeProvider::Retry(std::wstring_view parentId, Time now)
{
	const auto* node = m_impl->model.Inspect(parentId);
	if (!node || node->state != TreeChildrenState::Failed) return TreeResult::Invalid;
	return m_impl->Load(parentId, node->retryKind, now);
}
void SenpTreeProvider::Refresh(Time now)
{
	if (!m_impl->Current()) { Close(); return; }
	m_impl->CancelTickets(m_impl->model.Invalidate()); m_impl->Changed(L""); Pump(now);
}
bool SenpTreeProvider::Select(std::wstring_view id)
{
	if (!m_impl->Current()) { Close(); return false; }
	return m_impl->model.Select(id);
}
bool SenpTreeProvider::Execute(std::wstring_view id)
{
	auto& state = *m_impl;
	if (!state.Current() || !state.visible) return false;
	const auto* node = state.model.Inspect(id);
	if (!node || node->item.commandId.empty() || !state.commands.contains(node->item.commandId) || !state.model.Select(id)) return false;
	return state.options.runtime->Execute({ node->item.commandId, node->item.arguments });
}
bool SenpTreeProvider::ExecuteViewCommand(std::wstring_view commandId)
{
	auto& state = *m_impl;
	if (!state.Current() || !state.commands.contains(commandId)) return false;
	return state.options.runtime->Execute({ std::wstring(commandId), {} });
}
TreeResult SenpTreeProvider::Apply(const senp::effect::OperationContext& context, senp::effect::PublishTreePage page, Time now)
{
	auto& state = *m_impl;
	Pump(now);
	const auto it = state.Find(context); if (it == state.pending.end()) return TreeResult::Stale;
	const auto parent = it->second.request.parentId;
	const auto reject = [&]() {
		(void)state.model.Fail(it->second.request, L"The provider returned an invalid tree page.");
		state.options.runtime->Cancel(it->second.context); state.pending.erase(it); state.Changed(parent); return TreeResult::Invalid;
	};
	if (page.viewId != state.options.viewId || page.revision < 0 || page.items.size() > kMaximumTreePageItems) return reject();
	TreeChildrenPage native;
	native.parentId = std::move(page.parentId); native.nextCursor = std::move(page.nextCursor);
	native.message = std::move(page.message); native.revision = static_cast<std::uint64_t>(page.revision);
	switch (page.status) {
	case senp::effect::PageStatus::Complete: native.state = TreePageState::Complete; break;
	case senp::effect::PageStatus::Partial: native.state = TreePageState::Partial; break;
	case senp::effect::PageStatus::Empty: native.state = TreePageState::Empty; break;
	case senp::effect::PageStatus::Failed: native.state = TreePageState::Failed; break;
	default: return reject();
	}
	native.items.reserve(page.items.size());
	for (auto& item : page.items) {
		if (!item.commandId.empty() && !state.commands.contains(item.commandId)) return reject();
		native.items.push_back(Item(std::move(item)));
	}
	const auto changed = state.model.Apply(it->second.request, std::move(native));
	if (changed.result != TreeResult::Applied) state.options.runtime->Cancel(it->second.context);
	state.pending.erase(it); state.CancelTickets(changed.cancelled); state.Changed(parent); Pump(now);
	return changed.result;
}
TreeResult SenpTreeProvider::Failed(const senp::effect::OperationContext& context, senp::InvocationStatus status, Time now)
{
	Pump(now);
	const auto it = m_impl->Find(context); if (it == m_impl->pending.end()) return TreeResult::Stale;
	if (status == senp::InvocationStatus::EffectsReady) return TreeResult::Invalid;
	const auto parent = it->second.request.parentId;
	(void)m_impl->model.Fail(it->second.request, status == senp::InvocationStatus::TimedOut
		? L"The provider timed out." : status == senp::InvocationStatus::Cancelled ? L"The request was cancelled." : L"The provider could not load the tree.");
	m_impl->options.runtime->Cancel(it->second.context); m_impl->pending.erase(it); m_impl->Changed(parent); Pump(now);
	return TreeResult::Applied;
}
void SenpTreeProvider::Close() noexcept { m_impl->Close(); }
const TreeViewModel& SenpTreeProvider::Model() const noexcept { return m_impl->model; }
std::optional<SenpTreeProvider::Time> SenpTreeProvider::NextDeadline() const noexcept
{
	std::optional<Time> result;
	for (const auto& [ticket, pending] : m_impl->pending) if (!result || pending.deadline < *result) result = pending.deadline;
	return result;
}
bool SenpTreeProvider::IsVisible() const noexcept { return m_impl->visible; }
std::wstring_view SenpTreeProvider::ViewId() const noexcept { return m_impl->options.viewId; }

} // namespace workbench::tree
