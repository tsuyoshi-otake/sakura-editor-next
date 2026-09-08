/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpContributionOwners.h"

namespace senp {

class OwnerRequestAdmission final {
public:
	OwnerRequestAdmission() = default;
	OwnerRequestAdmission(AdmissionStatus status, effect::OperationContext context = {})
		: m_status(status), m_context(std::move(context)) {}
	[[nodiscard]] AdmissionStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const effect::OperationContext& Context() const noexcept { return m_context; }
private:
	AdmissionStatus m_status{ AdmissionStatus::Unavailable };
	effect::OperationContext m_context;
};

class OwnerRequestsSnapshot final {
public:
	OwnerRequestsSnapshot(std::size_t requests, std::size_t invocations, std::size_t completed,
		std::int64_t lastRequestGeneration, bool closed) noexcept
		: m_requests(requests), m_invocations(invocations), m_completed(completed),
		m_lastRequestGeneration(lastRequestGeneration), m_closed(closed) {}
	[[nodiscard]] std::size_t Requests() const noexcept { return m_requests; }
	[[nodiscard]] std::size_t Invocations() const noexcept { return m_invocations; }
	[[nodiscard]] std::size_t Completed() const noexcept { return m_completed; }
	[[nodiscard]] std::int64_t LastRequestGeneration() const noexcept { return m_lastRequestGeneration; }
	[[nodiscard]] bool Closed() const noexcept { return m_closed; }
private:
	std::size_t m_requests{};
	std::size_t m_invocations{};
	std::size_t m_completed{};
	std::int64_t m_lastRequestGeneration{};
	bool m_closed{};
};

//! UI-thread request broker for one current contribution owner. Root requests
//! receive owner-wide monotonic request generations. Derived events retain that
//! generation while receiving a distinct transport operation ID. Publication
//! callbacks enqueue completions only; callers drain them after owner Poll has
//! returned, so applying an effect cannot reenter the owner service.
class CSenpOwnerRequests final {
public:
	CSenpOwnerRequests(CSenpContributionOwners& owners, ContributionOwnerIdentity owner);
	~CSenpOwnerRequests();
	CSenpOwnerRequests(const CSenpOwnerRequests&) = delete;
	CSenpOwnerRequests& operator=(const CSenpOwnerRequests&) = delete;
	[[nodiscard]] OwnerRequestAdmission Submit(effect::Event event, CSenpRuntimeSession::Time deadline) noexcept;
	[[nodiscard]] OwnerRequestAdmission SubmitDerived(const effect::OperationContext& request,
		effect::Event event, CSenpRuntimeSession::Time deadline) noexcept;
	//! Validate and enqueue one runtime terminal from ISenpOwnerPublication::Apply.
	//! This method never calls the owner service or consumer code.
	[[nodiscard]] bool Publish(InvocationResult result) noexcept;
	[[nodiscard]] std::optional<InvocationResult> TakeCompleted() noexcept;
	//! Releases a lineage after its terminal has been drained and all emitted
	//! work has either completed or been explicitly cancelled.
	[[nodiscard]] bool Finish(const effect::OperationContext& request) noexcept;
	//! Suppresses queued delivery and cancels every in-flight invocation derived
	//! from the same request generation.
	[[nodiscard]] bool Cancel(const effect::OperationContext& request) noexcept;
	[[nodiscard]] bool IsCurrent() const noexcept;
	void Close() noexcept;
	[[nodiscard]] OwnerRequestsSnapshot Snapshot() const noexcept;
private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace senp
