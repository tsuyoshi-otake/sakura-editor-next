/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpOwnerRequests.h"
#include <algorithm>
#include <map>

namespace senp {
namespace {
constexpr std::size_t kMaximumRequests = CSenpRuntimeSession::kMaximumPending;
bool SameScope(const effect::OperationContext& left, const effect::OperationContext& right) noexcept
{
	return left.ownerGeneration == right.ownerGeneration
		&& left.workspaceRevision == right.workspaceRevision
		&& left.accountGeneration == right.accountGeneration
		&& left.requestGeneration == right.requestGeneration;
}
bool ValidOperation(const std::wstring_view id) noexcept
{
	if (id.empty() || id.size() > 96) return false;
	return std::ranges::all_of(id, [](const wchar_t value) {
		return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z')
			|| (value >= L'0' && value <= L'9') || value == L'.' || value == L'-'
			|| value == L'_' || value == L':' || value == L'/';
	});
}
}

class CSenpOwnerRequests::Impl {
	class Request final {
		friend class Impl;
		friend class CSenpOwnerRequests;
		effect::OperationContext scope;
		std::vector<std::wstring> invocations;
		bool suppressed{};
	public:
		explicit Request(effect::OperationContext value) : scope(std::move(value)) {}
	};
	friend class CSenpOwnerRequests;
	CSenpContributionOwners& owners;
	ContributionOwnerIdentity owner;
	std::map<std::int64_t, Request> requests;
	std::vector<InvocationResult> completed;
	std::int64_t lastRequestGeneration{};
	bool closed{};
	bool Scope(const effect::OperationContext& context) const noexcept
	{
		return context.ownerGeneration == owner.generation
			&& context.workspaceRevision == owner.workspaceRevision
			&& context.accountGeneration == owner.accountGeneration
			&& context.requestGeneration > 0;
	}
	Request* Find(const effect::OperationContext& context) noexcept
	{
		if (!Scope(context)) return nullptr;
		const auto found = requests.find(context.requestGeneration);
		return found != requests.end() && SameScope(found->second.scope, context) ? &found->second : nullptr;
	}
	std::size_t InvocationCount() const noexcept
	{
		std::size_t count{};
		for (const auto& [_, request] : requests) count += request.invocations.size();
		return count;
	}
	OwnerRequestAdmission Admit(Request& request, effect::Event event, const CSenpRuntimeSession::Time deadline) noexcept
	{
		try {
			const auto admission = owners.Submit(owner, std::move(event), request.scope.requestGeneration, deadline);
			if (admission.status != AdmissionStatus::Accepted) return { admission.status };
			if (!ValidOperation(admission.operationId)
				|| std::ranges::any_of(requests, [&](const auto& entry) {
					return std::ranges::find(entry.second.invocations, admission.operationId) != entry.second.invocations.end();
				})) {
				(void)owners.Cancel(owner, admission.operationId);
				closed = true;
				return { AdmissionStatus::Unavailable };
			}
			request.invocations.push_back(admission.operationId);
			auto context = request.scope;
			context.operationId = admission.operationId;
			return { AdmissionStatus::Accepted, std::move(context) };
		} catch (...) {
			closed = true;
			return { AdmissionStatus::Unavailable };
		}
	}
public:
	Impl(CSenpContributionOwners& source, ContributionOwnerIdentity identity)
		: owners(source), owner(std::move(identity))
	{
		completed.reserve(kMaximumRequests);
	}
};

CSenpOwnerRequests::CSenpOwnerRequests(CSenpContributionOwners& owners, ContributionOwnerIdentity owner)
	: m_impl(std::make_unique<Impl>(owners, std::move(owner))) {}
CSenpOwnerRequests::~CSenpOwnerRequests() { Close(); }

OwnerRequestAdmission CSenpOwnerRequests::Submit(effect::Event event, const CSenpRuntimeSession::Time deadline) noexcept
{
	auto& state = *m_impl;
	if (state.closed || !state.owners.IsCurrent(state.owner)) return {};
	if (state.requests.size() >= kMaximumRequests) return { AdmissionStatus::Busy };
	if (state.lastRequestGeneration == effect::kMaximumCounter) return { AdmissionStatus::InvalidRequest };
	const auto generation = state.lastRequestGeneration + 1;
	effect::OperationContext scope{ {}, state.owner.generation, state.owner.workspaceRevision,
		state.owner.accountGeneration, generation };
	auto [found, inserted] = state.requests.emplace(generation, Impl::Request{ scope });
	if (!inserted) return { AdmissionStatus::Unavailable };
	auto result = state.Admit(found->second, std::move(event), deadline);
	if (result.Status() == AdmissionStatus::Accepted) state.lastRequestGeneration = generation;
	else state.requests.erase(found);
	return result;
}

OwnerRequestAdmission CSenpOwnerRequests::SubmitDerived(const effect::OperationContext& request,
	effect::Event event, const CSenpRuntimeSession::Time deadline) noexcept
{
	auto& state = *m_impl;
	if (state.closed || !state.owners.IsCurrent(state.owner)) return {};
	auto* lineage = state.Find(request);
	if (!lineage || lineage->suppressed) return { AdmissionStatus::InvalidRequest };
	if (state.InvocationCount() + state.completed.size() >= kMaximumRequests)
		return { AdmissionStatus::Busy };
	return state.Admit(*lineage, std::move(event), deadline);
}

bool CSenpOwnerRequests::Publish(InvocationResult result) noexcept
{
	auto& state = *m_impl;
	if (state.closed || result.activation || !state.Scope(result.context)) return false;
	auto* request = state.Find(result.context);
	if (!request) return false;
	const auto invocation = std::ranges::find(request->invocations, result.context.operationId);
	if (invocation == request->invocations.end()) return false;
	request->invocations.erase(invocation);
	if (request->suppressed) {
		if (request->invocations.empty()) state.requests.erase(result.context.requestGeneration);
		return true;
	}
	if (state.completed.size() >= kMaximumRequests) return false;
	try { state.completed.push_back(std::move(result)); }
	catch (...) { return false; }
	return true;
}

std::optional<InvocationResult> CSenpOwnerRequests::TakeCompleted() noexcept
{
	auto& values = m_impl->completed;
	if (values.empty()) return std::nullopt;
	auto result = std::move(values.front());
	values.erase(values.begin());
	return result;
}

bool CSenpOwnerRequests::Finish(const effect::OperationContext& request) noexcept
{
	auto& state = *m_impl;
	const auto found = state.requests.find(request.requestGeneration);
	if (found == state.requests.end() || !SameScope(found->second.scope, request)
		|| !found->second.invocations.empty()
		|| std::ranges::any_of(state.completed, [&](const auto& value) { return SameScope(value.context, request); })) return false;
	state.requests.erase(found);
	return true;
}

bool CSenpOwnerRequests::Cancel(const effect::OperationContext& context) noexcept
{
	auto& state = *m_impl;
	auto* request = state.Find(context);
	if (!request || request->suppressed) return false;
	request->suppressed = true;
	state.completed.erase(std::remove_if(state.completed.begin(), state.completed.end(),
		[&](const auto& value) { return SameScope(value.context, context); }), state.completed.end());
	for (const auto& operationId : request->invocations) (void)state.owners.Cancel(state.owner, operationId);
	if (request->invocations.empty()) state.requests.erase(context.requestGeneration);
	return true;
}

bool CSenpOwnerRequests::IsCurrent() const noexcept
{
	return !m_impl->closed && m_impl->owners.IsCurrent(m_impl->owner);
}

void CSenpOwnerRequests::Close() noexcept
{
	auto& state = *m_impl;
	if (state.closed) return;
	state.closed = true;
	state.completed.clear();
	for (auto& [_, request] : state.requests) {
		request.suppressed = true;
		for (const auto& operationId : request.invocations) (void)state.owners.Cancel(state.owner, operationId);
	}
	state.requests.clear();
}

OwnerRequestsSnapshot CSenpOwnerRequests::Snapshot() const noexcept
{
	const auto& state = *m_impl;
	return { state.requests.size(), state.InvocationCount(), state.completed.size(),
		state.lastRequestGeneration, state.closed };
}

} // namespace senp
