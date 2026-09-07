/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpEffectRuntime.h"
#include <functional>

namespace senp {

struct ContributionOwnerIdentity final {
	std::wstring extensionId;
	std::wstring packageDigest;
	std::int64_t generation{};
	std::int64_t workspaceRevision{};
	std::int64_t accountGeneration{};
	bool operator==(const ContributionOwnerIdentity&) const = default;
};

//! Native projection transaction. Prepare owns all provisional resources and
//! destruction aborts an unpublished transaction. None of these methods may
//! pump messages or reenter the owner service. Commit(false) changes nothing;
//! Commit(true) performs only prepared, non-throwing publication. Revoke clears
//! only this exact generation's grants, commands, visible data and callbacks,
//! and terminates its outstanding tool/UI requests before releasing subscribers.
class ISenpOwnerPublication {
public:
	virtual ~ISenpOwnerPublication() = default;
	[[nodiscard]] virtual bool Validate(const InvocationResult& result) const noexcept = 0;
	[[nodiscard]] virtual bool Commit() noexcept = 0;
	[[nodiscard]] virtual bool Apply(InvocationResult result) noexcept = 0;
	virtual void Revoke(effect::StopReason reason) noexcept = 0;
};

using PrepareOwnerPublication = std::function<std::unique_ptr<ISenpOwnerPublication>(
	const ContributionOwnerIdentity& candidate, const ContributionOwnerIdentity* previous)>;
using EffectRuntimeFactory = std::function<std::unique_ptr<ISenpEffectRuntime>(EffectRuntimeLaunch)>;

enum class OwnerChangeStatus : std::uint8_t {
	Accepted, Activated, Busy, InvalidRequest, Unsupported, Failed, Cancelled, TimedOut, Stopped,
};
struct OwnerChangeResult final {
	OwnerChangeStatus status{ OwnerChangeStatus::Failed };
	ContributionOwnerIdentity owner;
};
struct ContributionOwnersSnapshot final {
	std::size_t active{};
	std::size_t preparing{};
	std::size_t retiring{};
	std::size_t cleanupFailed{};
	std::size_t transitions{};
	bool closed{};
};

//! Serialized native composition owner. Poll is a bounded drain, never a wait
//! or a network operation. Stop is nonblocking; Close owns teardown-time joins.
//! The four-slot limit includes active, preparing and retiring processes. A
//! failed join is retained without retrying on each Poll; explicit Close owns
//! one further attempt. Scheduling Poll is the native composition's obligation.
//! Every accepted preparation reserves one of 16 terminal transition receipts.
class CSenpContributionOwners final {
public:
	explicit CSenpContributionOwners(EffectRuntimeFactory factory = {});
	~CSenpContributionOwners();
	CSenpContributionOwners(const CSenpContributionOwners&) = delete;
	CSenpContributionOwners& operator=(const CSenpContributionOwners&) = delete;
	[[nodiscard]] OwnerChangeResult Prepare(EffectRuntimeLaunch launch, std::wstring packageDigest,
		PrepareOwnerPublication publication, CSenpRuntimeSession::Time now);
	void Poll(CSenpRuntimeSession::Time now);
	[[nodiscard]] bool Revoke(std::wstring_view extensionId, effect::StopReason reason);
	[[nodiscard]] InvocationAdmission Submit(const ContributionOwnerIdentity& owner, effect::Event event,
		std::int64_t requestGeneration, CSenpRuntimeSession::Time deadline);
	[[nodiscard]] bool Cancel(const ContributionOwnerIdentity& owner, std::wstring_view operationId);
	//! Consumers retaining a previously taken result must check this immediately
	//! before applying it; generation equality alone does not prove a live owner.
	[[nodiscard]] bool IsCurrent(const ContributionOwnerIdentity& owner) const noexcept;
	[[nodiscard]] std::optional<OwnerChangeResult> TakeTransition();
	[[nodiscard]] ContributionOwnersSnapshot Snapshot() const noexcept;
	//! Returns false and retains a bounded failed-cleanup owner if physical exit
	//! cannot be confirmed. Never detaches a worker or silently admits more hosts.
	[[nodiscard]] bool Close() noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace senp
