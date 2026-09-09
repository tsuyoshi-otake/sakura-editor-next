/*! @file
	@brief Production tool-read seam over the control-owned SENP broker.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/SenpReadonlyOwnerTarget.h"
#include "platform/controlipc/ControlSenpClient.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace workbench::editor {

struct SenpControlToolReadsOptions {
	//! Canonical control authority identity, the same value the storage Hello
	//! pins. It is not the profile a grant is scoped to.
	std::string authorityProfileId;
	std::wstring authorityProfileHash;
	//! Anti-rollback floor handed to the connection.
	std::uint64_t minimumGeneration = 0;
	//! The user-data profile the control side scopes every grant to.
	std::wstring senpProfileId;
	std::chrono::milliseconds pollInterval = std::chrono::milliseconds(50);
	std::chrono::milliseconds exchangeDeadline = std::chrono::seconds(5);
	std::function<std::unique_ptr<platform::controlipc::IControlPlatformClientChannel>()> channelFactory;
};

enum class ESenpControlToolReadsState : std::uint8_t {
	Idle,
	Connected,
	Disconnected,
	Stopped,
};

/*!
	@brief Worker-backed ISenpOwnerToolReads over one authenticated connection.

	The seam methods run on the UI thread inside the owner projection's Pump, so
	none of them touches the pipe: they publish work for a private worker and read
	back state the worker has already settled. The worker owns the client
	exclusively, which is what lets the client keep one authenticated connection -
	a grant is bound to the connection it was minted on, so a second caller on a
	second connection would hold nothing.

	Every admitted read ends in a terminal. A refused dispatch, a replaced
	connection or a stopped seam synthesizes one rather than leaving the
	projection holding a retained effect that can never complete. A grant is
	re-issued whenever the connection epoch it was minted on is no longer the
	current one, for the same reason.

	Stop is terminal and is expected from one thread only, the same UI thread that
	drives the seam.
*/
class CSenpControlToolReads final : public ISenpOwnerToolReads {
public:
	//! One window brokers a bounded number of owners. The per-owner read bound is
	//! the runtime session's pending maximum, which also bounds the completion
	//! queue: at most one terminal exists per admitted read.
	static constexpr std::size_t kMaximumOwners = 8;
	static constexpr std::size_t kMaximumQueuedCommands = 128;
	//! Attempts tolerated for one read before it is failed. The broker answers
	//! Busy while its executor is saturated and Expired once a grant outlives its
	//! lifetime; both are transient and worth another turn.
	static constexpr std::uint32_t kMaximumAttempts = 3;

	CSenpControlToolReads(SenpControlToolReadsOptions options,
		platform::controlipc::IControlPlatformEndpointReader& endpointReader);
	//! Production composition owns nothing else that could hold the discovery
	//! reader for exactly this seam's lifetime, so this overload adopts it.
	//! A null reader throws: a seam without discovery could never connect.
	CSenpControlToolReads(SenpControlToolReadsOptions options,
		std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader> endpointReader);
	~CSenpControlToolReads() override;
	CSenpControlToolReads(const CSenpControlToolReads&) = delete;
	CSenpControlToolReads& operator=(const CSenpControlToolReads&) = delete;

	[[nodiscard]] bool Start(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context,
		const senp::effect::StartToolRead& read) noexcept override;
	[[nodiscard]] std::optional<senp::effect::ToolCompleted> Take(
		const senp::ContributionOwnerIdentity& owner) noexcept override;
	void Cancel(const senp::ContributionOwnerIdentity& owner,
		const senp::effect::OperationContext& context) noexcept override;
	void CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept override;

	//! Terminal. It stops the client, joins the worker and drops every queue, so
	//! no completion is routed afterwards.
	void Stop() noexcept;
	[[nodiscard]] ESenpControlToolReadsState State() const noexcept;
	[[nodiscard]] std::size_t OutstandingReads() const noexcept;
	[[nodiscard]] std::uint64_t ConnectionEpoch() const noexcept;
	//! Blocks until the worker has drained its queue and no read is outstanding,
	//! or the timeout expires. It exists so a caller can observe settled state
	//! without sleeping; the seam itself never waits on the UI thread.
	[[nodiscard]] bool WaitForSettled(std::chrono::milliseconds timeout);

private:
	struct Command {
		//! Cancel carries only `read.readId`; Retire carries neither.
		enum class Kind : std::uint8_t { Start, Cancel, Retire } kind{ Kind::Start };
		senp::ContributionOwnerIdentity owner;
		senp::effect::OperationContext context;
		senp::effect::StartToolRead read;
		std::uint32_t attempts{};
	};
	//! One read admitted here and not yet terminated. The context travels with it
	//! so lineage cancellation matches the predicate the target and the owner
	//! projection use.
	struct Read {
		std::wstring readId;
		senp::effect::OperationContext context;
		//! The connection epoch the broker admitted this read on, zero while it
		//! is still queued here. Only a dispatched read dies with its connection;
		//! one that never left is dispatched again on the next one.
		std::uint64_t epoch{};
	};
	struct Owner {
		senp::ContributionOwnerIdentity identity;
		std::vector<Read> outstanding;
		std::deque<senp::effect::ToolCompleted> completions;
		bool retired{};
	};
	//! Worker-private grant record. The epoch is part of a grant's identity: the
	//! same id on a later connection is not the same capability.
	struct Grant {
		senp::ContributionOwnerIdentity identity;
		std::string grantId;
		std::uint64_t epoch{};
	};

	//! All four require m_mutex to be held by the caller.
	[[nodiscard]] Owner* FindLocked(const senp::ContributionOwnerIdentity& owner) noexcept;
	[[nodiscard]] std::size_t PendingLocked() const noexcept;
	void PublishLocked(Owner& owner, std::wstring readId,
		senp::effect::CompletionStatus status, std::wstring message);
	[[nodiscard]] bool EnqueueLocked(Command command) noexcept;

	//! Publishes a synthesized or received terminal and forgets the read.
	void Complete(const senp::ContributionOwnerIdentity& owner, const std::wstring& readId,
		senp::effect::CompletionStatus status, std::wstring message) noexcept;
	//! Routes a terminal the broker produced. A readId this seam does not hold
	//! was cancelled while in flight and is dropped.
	void Route(const senp::ContributionOwnerIdentity& owner, senp::effect::ToolCompleted completion) noexcept;
	//! Terminates every outstanding read of one owner.
	void FailOwner(const senp::ContributionOwnerIdentity& owner,
		senp::effect::CompletionStatus status, std::wstring message) noexcept;
	//! Terminates every read the broker had already admitted. A read still
	//! queued here is untouched: nothing on the wire refers to it yet.
	void FailDispatched(senp::effect::CompletionStatus status, std::wstring message) noexcept;
	//! Records the connection a read was admitted on.
	void Dispatched(const senp::ContributionOwnerIdentity& owner, const std::wstring& readId,
		std::uint64_t epoch) noexcept;

	void Worker() noexcept;
	void Run(Command command) noexcept;
	//! Re-queues one refused read, or fails it once its attempts are spent.
	void Retry(Command command, std::wstring message) noexcept;
	void Poll() noexcept;
	[[nodiscard]] bool Ensure() noexcept;
	//! Returns the live grant for this owner, minting one when the cached record
	//! belongs to an earlier connection. Empty means the control side refused,
	//! and `failure` then says whether that refusal was a host outage.
	[[nodiscard]] std::optional<std::string> Authorize(const senp::ContributionOwnerIdentity& owner,
		senp::effect::CompletionStatus& failure) noexcept;
	void Forget(const senp::ContributionOwnerIdentity& owner) noexcept;
	[[nodiscard]] platform::controlipc::ControlSenpRpcRequest Compose(
		platform::controlipc::EControlSenpRpcOperation operation,
		const senp::ContributionOwnerIdentity& owner, const std::string& grantId) const;

	SenpControlToolReadsOptions m_options;
	//! Engaged only for the adopting constructor, and declared ahead of the
	//! client because the client keeps a reference into it.
	std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader> m_ownedReader;
	platform::controlipc::CControlSenpClient m_client;
	mutable std::mutex m_mutex;
	std::condition_variable m_work;
	std::condition_variable m_quiet;
	std::deque<Command> m_commands;
	std::vector<Owner> m_owners;
	bool m_busy{};
	bool m_stopped{};
	//! Worker-private: only the worker thread reads or writes it, and Stop joins
	//! the worker before the client is destroyed.
	std::vector<Grant> m_grants;
	std::thread m_worker;
};

} // namespace workbench::editor
