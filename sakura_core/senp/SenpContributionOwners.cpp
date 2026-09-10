/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpContributionOwners.h"
#include <algorithm>

namespace senp {
namespace {
using Clock = std::chrono::steady_clock;
using Time = CSenpRuntimeSession::Time;
using namespace std::chrono_literals;
constexpr std::size_t kMaximumHosts = 4;
constexpr std::size_t kMaximumTransitions = 16;
enum class Phase { Preparing, Active, Retiring, CleanupFailed };
bool IsDigest(const std::wstring_view digest) noexcept
{
	return digest.size() == 64 && std::ranges::all_of(digest, [](wchar_t c) {
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
	});
}
struct CallGuard {
	bool& entered;
	explicit CallGuard(bool& value) : entered(value) { entered = true; }
	~CallGuard() { entered = false; }
};
} // namespace

struct CSenpContributionOwners::Impl {
	struct Slot {
		ContributionOwnerIdentity owner;
		std::unique_ptr<ISenpEffectRuntime> runtime;
		std::unique_ptr<ISenpOwnerPublication> publication;
		std::wstring activationId;
		Phase phase{ Phase::Preparing };
		Time deadline;
		bool joinAttempted{};
		bool joined{};
	};
	EffectRuntimeFactory factory;
	std::vector<std::unique_ptr<Slot>> slots;
	std::vector<OwnerChangeResult> transitions;
	std::int64_t generation{};
	bool entered{};
	bool closed{};
	explicit Impl(EffectRuntimeFactory create) : factory(std::move(create))
	{
		slots.reserve(kMaximumHosts);
		transitions.reserve(kMaximumTransitions);
		if (!factory) factory = [](EffectRuntimeLaunch launch) {
			return std::make_unique<CSenpEffectRuntime>(std::move(launch));
		};
	}
	Slot* Current(const std::wstring_view id) const noexcept
	{
		for (const auto& slot : slots) if (slot->phase == Phase::Active && slot->owner.extensionId == id) return slot.get();
		return nullptr;
	}
	void Finish(const Slot& slot, const OwnerChangeStatus status) noexcept
	{
		for (auto& result : transitions) {
			if (result.owner.generation == slot.owner.generation && result.status == OwnerChangeStatus::Accepted) {
				result.status = status;
				break;
			}
		}
	}
	void Retire(Slot& slot, const effect::StopReason reason, const OwnerChangeStatus status, const Time now) noexcept
	{
		if (slot.phase == Phase::Retiring || slot.phase == Phase::CleanupFailed) return;
		// Remove result authority before any callback or process-stop operation.
		slot.phase = Phase::Retiring;
		slot.deadline = now + 3s;
		Finish(slot, status);
		slot.publication->Revoke(reason);
		try { slot.runtime->Stop(reason); }
		catch (...) { slot.phase = Phase::CleanupFailed; }
	}
	bool Collect(Slot& slot, const Time now) noexcept
	{
		try {
			const auto state = slot.runtime->Snapshot();
			if (!state.workerExited) {
				if (now >= slot.deadline) slot.phase = Phase::CleanupFailed;
				return false;
			}
			if (!slot.joined) {
				if (slot.joinAttempted) { slot.phase = Phase::CleanupFailed; return false; }
				slot.joinAttempted = true;
				slot.runtime->Join();
				slot.joined = true;
			}
			if (slot.runtime->Snapshot().processExitConfirmed) return true;
		} catch (...) {}
		slot.phase = Phase::CleanupFailed;
		return false;
	}
	bool Matches(const Slot& slot, const InvocationResult& result) const noexcept
	{
		return result.context.ownerGeneration == slot.owner.generation
			&& result.context.workspaceRevision == slot.owner.workspaceRevision
			&& result.context.accountGeneration == slot.owner.accountGeneration;
	}
	void Drain(Slot& slot, const Time now)
	{
		if (slot.phase == Phase::Preparing && now >= slot.deadline) {
			Retire(slot, effect::StopReason::HostUnavailable, OwnerChangeStatus::TimedOut, now);
			return;
		}
		const auto state = slot.runtime->Snapshot();
		if (state.phase == RuntimePhase::Stopping || state.phase == RuntimePhase::Stopped) {
			Retire(slot, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, now);
			return;
		}
		for (std::size_t count = 0; count < CSenpRuntimeSession::kMaximumPending; ++count) {
			auto result = slot.runtime->TakeCompleted();
			if (!result) return;
			const bool preparing = slot.phase == Phase::Preparing;
			if (!Matches(slot, *result) || result->activation != preparing
				|| (preparing && (result->context.operationId != slot.activationId
					|| result->status != InvocationStatus::EffectsReady || state.phase != RuntimePhase::Active))
				|| !slot.publication->Validate(*result)) {
				Retire(slot, effect::StopReason::ProtocolError, OwnerChangeStatus::Failed, now);
				return;
			}
			if (preparing) {
				if (!slot.publication->Commit()) {
					Retire(slot, effect::StopReason::Updated, OwnerChangeStatus::Failed, now);
					return;
				}
				if (auto* previous = Current(slot.owner.extensionId))
					Retire(*previous, effect::StopReason::Updated, OwnerChangeStatus::Cancelled, now);
				slot.phase = Phase::Active;
			}
			if (!slot.publication->Apply(std::move(*result))) {
				Retire(slot, effect::StopReason::ProtocolError, OwnerChangeStatus::Failed, now);
				return;
			}
			if (preparing) Finish(slot, OwnerChangeStatus::Activated);
		}
	}
};

CSenpContributionOwners::CSenpContributionOwners(EffectRuntimeFactory factory)
	: m_impl(std::make_unique<Impl>(std::move(factory))) {}
CSenpContributionOwners::~CSenpContributionOwners() { (void)Close(); }

OwnerChangeResult CSenpContributionOwners::Prepare(EffectRuntimeLaunch launch, std::wstring packageDigest,
	PrepareOwnerPublication publication, const Time now)
{
	auto& s = *m_impl;
	if (s.closed) return { OwnerChangeStatus::Stopped };
	if (s.entered || s.slots.size() >= kMaximumHosts || s.transitions.size() >= kMaximumTransitions)
		return { OwnerChangeStatus::Busy };
	if (!publication) return { OwnerChangeStatus::Unsupported };
	if (!IsDigest(packageDigest) || !IsDigest(launch.moduleSha256) || launch.hostExecutable.empty()
		|| launch.modulePath.empty() || launch.hostExecutable.size() > 32767 || launch.modulePath.size() > 32767
		|| s.generation == effect::kMaximumCounter) return { OwnerChangeStatus::InvalidRequest };
	for (const auto& slot : s.slots) {
		if (slot->owner.extensionId == launch.extensionId && slot->phase == Phase::Preparing)
			return { OwnerChangeStatus::Busy };
	}
	CallGuard guard(s.entered);
	Impl::Slot* admitted{};
	try {
		launch.generation = ++s.generation;
		launch.context.ownerGeneration = launch.generation;
		launch.context.operationId = L"prepare";
		if (!effect::Validate({ 2, 1, launch.generation, effect::Activate{ launch.context, launch.extensionId } }))
			return { OwnerChangeStatus::InvalidRequest };
		auto slot = std::make_unique<Impl::Slot>();
		slot->owner = { launch.extensionId, std::move(packageDigest), launch.generation,
			launch.context.workspaceRevision, launch.context.accountGeneration };
		// A slot that has not become callable yet is waiting on a cold start, not
		// on one invocation, so it is held to the budget written for one.
		slot->deadline = now + CSenpRuntimeSession::kMaximumColdStart;
		const auto* previous = s.Current(launch.extensionId);
		slot->publication = publication(slot->owner, previous ? &previous->owner : nullptr);
		if (!slot->publication) return { OwnerChangeStatus::Unsupported };
		slot->runtime = s.factory(std::move(launch));
		if (!slot->runtime) return { OwnerChangeStatus::Failed };
		OwnerChangeResult result{ OwnerChangeStatus::Accepted, slot->owner };
		s.transitions.push_back(result);
		admitted = slot.get();
		s.slots.push_back(std::move(slot));
		const auto started = admitted->runtime->Start();
		if (started.status != AdmissionStatus::Accepted)
			s.Retire(*admitted, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, now);
		else admitted->activationId = started.operationId;
		return result;
	} catch (...) {
		if (admitted) {
			s.Retire(*admitted, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, now);
			// Admission reserved its single terminal receipt before Start.
			return { OwnerChangeStatus::Accepted, admitted->owner };
		}
		return { OwnerChangeStatus::Failed };
	}
}

void CSenpContributionOwners::Poll(const Time now)
{
	auto& s = *m_impl;
	if (s.entered) return; // The outer bounded drain retains finalization ownership.
	CallGuard guard(s.entered);
	for (auto it = s.slots.begin(); it != s.slots.end();) {
		auto& slot = **it;
		if (slot.phase == Phase::Preparing || slot.phase == Phase::Active) {
			try { s.Drain(slot, now); }
			catch (...) { s.Retire(slot, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, now); }
		}
		if ((slot.phase == Phase::Retiring || slot.phase == Phase::CleanupFailed) && s.Collect(slot, now))
			it = s.slots.erase(it);
		else ++it;
	}
}

bool CSenpContributionOwners::Revoke(const std::wstring_view extensionId, const effect::StopReason reason)
{
	auto& s = *m_impl;
	if (s.entered) return false;
	CallGuard guard(s.entered);
	bool found{};
	for (auto& slot : s.slots) if (slot->owner.extensionId == extensionId) {
		s.Retire(*slot, reason, OwnerChangeStatus::Cancelled, Clock::now());
		found = true;
	}
	return found;
}

InvocationAdmission CSenpContributionOwners::Submit(const ContributionOwnerIdentity& owner, effect::Event event,
	const std::int64_t requestGeneration, const Time deadline)
{
	auto& s = *m_impl;
	if (s.entered) return { AdmissionStatus::Busy };
	auto* slot = s.Current(owner.extensionId);
	if (s.closed || !slot || slot->owner != owner) return { AdmissionStatus::Unavailable };
	if (requestGeneration < 0) return { AdmissionStatus::InvalidRequest };
	CallGuard guard(s.entered);
	try {
		return slot->runtime->Submit({ {}, owner.generation, owner.workspaceRevision, owner.accountGeneration, requestGeneration },
			std::move(event), deadline);
	} catch (...) {
		s.Retire(*slot, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, Clock::now());
		return { AdmissionStatus::Unavailable };
	}
}

bool CSenpContributionOwners::Cancel(const ContributionOwnerIdentity& owner, const std::wstring_view operationId)
{
	auto& s = *m_impl;
	if (s.entered || !IsCurrent(owner)) return false;
	CallGuard guard(s.entered);
	auto& slot = *s.Current(owner.extensionId);
	try { return slot.runtime->Cancel(operationId); }
	catch (...) { s.Retire(slot, effect::StopReason::HostUnavailable, OwnerChangeStatus::Failed, Clock::now()); return false; }
}

bool CSenpContributionOwners::IsCurrent(const ContributionOwnerIdentity& owner) const noexcept
{
	const auto* current = m_impl->Current(owner.extensionId);
	return !m_impl->closed && current && current->owner == owner;
}

std::optional<OwnerChangeResult> CSenpContributionOwners::TakeTransition()
{
	if (m_impl->entered) return std::nullopt;
	auto& values = m_impl->transitions;
	const auto found = std::ranges::find_if(values, [](const auto& result) { return result.status != OwnerChangeStatus::Accepted; });
	if (found == values.end()) return std::nullopt;
	OwnerChangeResult result = std::move(*found);
	values.erase(found);
	return result;
}

ContributionOwnersSnapshot CSenpContributionOwners::Snapshot() const noexcept
{
	ContributionOwnersSnapshot result{ .transitions = m_impl->transitions.size(), .closed = m_impl->closed };
	for (const auto& slot : m_impl->slots) switch (slot->phase) {
	case Phase::Preparing: ++result.preparing; break;
	case Phase::Active: ++result.active; break;
	case Phase::Retiring: ++result.retiring; break;
	case Phase::CleanupFailed: ++result.cleanupFailed; break;
	}
	return result;
}

bool CSenpContributionOwners::Close() noexcept
{
	auto& s = *m_impl;
	if (s.entered) return false;
	CallGuard guard(s.entered);
	s.closed = true;
	for (auto& slot : s.slots) s.Retire(*slot, effect::StopReason::Shutdown, OwnerChangeStatus::Cancelled, Clock::now());
	// Signal every process before joining any worker. This avoids serializing
	// four full deadlines while other owners remain callable.
	for (auto it = s.slots.begin(); it != s.slots.end();) {
		auto& slot = **it;
		try {
			if (!slot.joined) {
				slot.joinAttempted = true; // Explicit Close may retry a failed join once.
				slot.runtime->Join();
				slot.joined = true;
			}
			const auto state = slot.runtime->Snapshot();
			if (state.workerExited && state.processExitConfirmed) { it = s.slots.erase(it); continue; }
		} catch (...) {}
		slot.phase = Phase::CleanupFailed;
		++it;
	}
	return s.slots.empty();
}

} // namespace senp
