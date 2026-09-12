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
#include "platform/foundation/NativeWorkerThread.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::editor {

//! Construction options for CSenpControlToolReads. State is private: every
//! caller in this repository builds one by default-constructing and then
//! assigning each field it needs, so that pattern is preserved through plain
//! setters rather than requiring callers to adopt designated-initializer or
//! constructor-argument syntax.
class SenpControlToolReadsOptions final {
public:
	[[nodiscard]] const std::string& AuthorityProfileId() const noexcept { return m_authorityProfileId; }
	void SetAuthorityProfileId(std::string value) { m_authorityProfileId = std::move(value); }

	[[nodiscard]] const std::wstring& AuthorityProfileHash() const noexcept { return m_authorityProfileHash; }
	void SetAuthorityProfileHash(std::wstring value) { m_authorityProfileHash = std::move(value); }

	//! Anti-rollback floor handed to the connection.
	[[nodiscard]] std::uint64_t MinimumGeneration() const noexcept { return m_minimumGeneration; }
	void SetMinimumGeneration(std::uint64_t value) noexcept { m_minimumGeneration = value; }

	//! The user-data profile the control side scopes every grant to.
	[[nodiscard]] const std::wstring& SenpProfileId() const noexcept { return m_senpProfileId; }
	void SetSenpProfileId(std::wstring value) { m_senpProfileId = std::move(value); }

	[[nodiscard]] std::chrono::milliseconds PollInterval() const noexcept { return m_pollInterval; }
	void SetPollInterval(std::chrono::milliseconds value) noexcept { m_pollInterval = value; }

	[[nodiscard]] std::chrono::milliseconds ExchangeDeadline() const noexcept { return m_exchangeDeadline; }
	void SetExchangeDeadline(std::chrono::milliseconds value) noexcept { m_exchangeDeadline = value; }

	//! Floor between two account queries. The window asks on every frame turn,
	//! so the seam - not its callers - owns how often that reaches the wire.
	[[nodiscard]] std::chrono::milliseconds AccountRefreshInterval() const noexcept { return m_accountRefreshInterval; }
	void SetAccountRefreshInterval(std::chrono::milliseconds value) noexcept { m_accountRefreshInterval = value; }

	[[nodiscard]] const std::function<std::unique_ptr<platform::controlipc::IControlPlatformClientChannel>()>&
	ChannelFactory() const noexcept { return m_channelFactory; }
	void SetChannelFactory(
		std::function<std::unique_ptr<platform::controlipc::IControlPlatformClientChannel>()> value)
	{
		m_channelFactory = std::move(value);
	}

private:
	std::string m_authorityProfileId;
	std::wstring m_authorityProfileHash;
	std::uint64_t m_minimumGeneration = 0;
	std::wstring m_senpProfileId;
	std::chrono::milliseconds m_pollInterval = std::chrono::milliseconds(50);
	std::chrono::milliseconds m_exchangeDeadline = std::chrono::seconds(5);
	std::chrono::milliseconds m_accountRefreshInterval = std::chrono::seconds(2);
	std::function<std::unique_ptr<platform::controlipc::IControlPlatformClientChannel>()> m_channelFactory;
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
	static constexpr std::size_t MaximumOwners() noexcept { return 8; }
	static constexpr std::size_t MaximumQueuedCommands() noexcept { return 128; }
	//! Attempts tolerated for one read before it is failed. The broker answers
	//! Busy while its executor is saturated and Expired once a grant outlives its
	//! lifetime; both are transient and worth another turn.
	static constexpr std::uint32_t MaximumAttempts() noexcept { return 3; }

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
	[[nodiscard]] SenpToolAccount Account() const noexcept override;
	void RefreshAccount() noexcept override;

	/*!
		@brief Publishes the workspace this window is open on.

		The control side derives the repository a read answers for from the
		folders declared here, and from nothing else: a window that declares none
		is answered Unavailable rather than guessed for. Folder identities only -
		what they resolve to is read from the remotes found there, so this claims
		nothing about their contents.

		Callable on every window turn. Only a declaration that differs from the
		one this seam last published reaches the wire, so the cadence belongs
		here rather than in each caller, and a declaration is re-sent on a new
		connection because the broker binds it to the connection that made it.
	*/
	void DeclareWorkspace(std::uint64_t generation, std::uint64_t revision,
		std::vector<std::wstring> folders) noexcept;

	[[nodiscard]] bool ReadResource(const senp::ContributionOwnerIdentity& owner,
		std::wstring_view handle, std::uint64_t offset, std::uint32_t length) noexcept override;
	[[nodiscard]] std::optional<SenpToolResourceAnswer> TakeResource(
		const senp::ContributionOwnerIdentity& owner) noexcept override;
	void ReleaseResource(const senp::ContributionOwnerIdentity& owner,
		std::wstring_view handle) noexcept override;

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
	//! Every nested record below is a private implementation detail: it is
	//! declared as `class` (private members by default) with this owning class
	//! as the sole friend, rather than as an aggregate `struct` with public
	//! fields. That keeps the representation genuinely encapsulated (nothing
	//! outside CSenpControlToolReads can read or mutate a field) while letting
	//! every existing internal call site keep using plain field access.
	class Command final {
		friend class CSenpControlToolReads;
		//! Cancel carries only `read.readId`; Retire, Account and Workspace carry
		//! neither. A Workspace command carries no declaration either: it only
		//! wakes the worker, which reads the published one under the lock.
		enum class Kind : std::uint8_t {
			Start, Cancel, Retire, Account, Workspace, Resource, Release
		} kind{ Kind::Start };
		senp::ContributionOwnerIdentity owner;
		senp::effect::OperationContext context;
		senp::effect::StartToolRead read;
		//! Named by Resource and Release only. A Release names no window: it
		//! withdraws the whole resource rather than a range of it.
		std::wstring resourceHandle;
		std::uint64_t resourceOffset{};
		std::uint32_t resourceLength{};
		std::uint32_t attempts{};
	};
	//! One read admitted here and not yet terminated. The context travels with it
	//! so lineage cancellation matches the predicate the target and the owner
	//! projection use.
	class Read final {
		friend class CSenpControlToolReads;
	public:
		Read() = default;
		Read(std::wstring readId, senp::effect::OperationContext context) noexcept
			: readId(std::move(readId)), context(std::move(context)) {}
	private:
		std::wstring readId;
		senp::effect::OperationContext context;
		//! The connection epoch the broker admitted this read on, zero while it
		//! is still queued here. Only a dispatched read dies with its connection;
		//! one that never left is dispatched again on the next one.
		std::uint64_t epoch{};
	};
	class Owner final {
		friend class CSenpControlToolReads;
	public:
		explicit Owner(senp::ContributionOwnerIdentity identity) noexcept : identity(std::move(identity)) {}
	private:
		senp::ContributionOwnerIdentity identity;
		std::vector<Read> outstanding;
		std::deque<senp::effect::ToolCompleted> completions;
		bool retired{};
		//! The resource read in flight for this owner, empty when none is, plus
		//! the settled answer waiting to be drained. One at a time: two answers
		//! could only be told apart by the surface remembering which it asked
		//! for, and remembering that is what this pair is doing on its behalf.
		std::wstring resourceHandle;
		std::uint64_t resourceOffset{};
		std::optional<SenpToolResourceAnswer> resource;
	};
	//! Worker-private grant record. The epoch is part of a grant's identity: the
	//! same id on a later connection is not the same capability.
	class Grant final {
		friend class CSenpControlToolReads;
	public:
		Grant(senp::ContributionOwnerIdentity identity, std::string grantId, std::uint64_t epoch) noexcept
			: identity(std::move(identity)), grantId(std::move(grantId)), epoch(epoch) {}
	private:
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
	//! Publishes one settled resource answer. An answer for a read this owner is
	//! no longer waiting on was cancelled with it and is dropped.
	void Answer(const senp::ContributionOwnerIdentity& owner, SenpToolResourceAnswer answer) noexcept;
	//! Answers a resource read the control side did not serve. An engaged result
	//! is the refusal it gave; a disengaged one means no answer arrived at all.
	void Refuse(const Command& command, std::optional<senp::TextResourceResult> result) noexcept;

	void Worker() noexcept;
	void Run(Command command) noexcept;
	//! Re-queues one refused read, or fails it once its attempts are spent.
	void Retry(Command command, std::wstring message) noexcept;
	//! Worker-side. Serves one resource read and always answers it.
	void Fetch(Command command) noexcept;
	//! Re-queues one transiently refused resource read. `exhausted` is the
	//! refusal it carried, which is what the answer says once the attempts are
	//! spent: a surface told the resource expired re-resolves it, where a
	//! generic failure would only be reported.
	void Refetch(Command command, senp::TextResourceResult exhausted) noexcept;
	void Poll() noexcept;
	//! Worker-side. Asks the control side which account the profile has adopted.
	[[nodiscard]] SenpToolAccount Query() noexcept;
	//! Publishes one settled answer and clears the pending query.
	void Settle(SenpToolAccount account) noexcept;
	//! Drops an answer that a replaced connection made meaningless, and lets the
	//! next refresh reach the wire without waiting out the cadence.
	void Stale() noexcept;
	[[nodiscard]] bool Ensure() noexcept;
	//! Worker-side. Sends the published declaration when the connection has not
	//! heard it. A connection that has is left alone: re-declaring the same
	//! workspace would let a window drive the control side's refresh worker.
	[[nodiscard]] bool Declare() noexcept;
	//! Returns the live grant for this owner, minting one when the cached record
	//! belongs to an earlier connection. Empty means the control side refused,
	//! and `failure` then says whether that refusal was a host outage.
	[[nodiscard]] std::optional<std::string> Authorize(const senp::ContributionOwnerIdentity& owner,
		senp::effect::CompletionStatus& failure) noexcept;
	void Forget(const senp::ContributionOwnerIdentity& owner) noexcept;
	[[nodiscard]] platform::controlipc::ControlSenpRpcRequest Compose(
		platform::controlipc::EControlSenpRpcOperation operation,
		const senp::ContributionOwnerIdentity& owner, const std::string& grantId) const;
	//! Starts the worker thread. Both constructors funnel through this one
	//! site so the worker is acquired the same way whichever one runs.
	void StartWorker();

	//! The mutex-guarded state, held behind a pointer rather than as plain data
	//! members. A pointer does not propagate constness to its pointee, so a
	//! const method can still lock `m_state->m_mutex` without the mutex itself
	//! ever being declared `mutable`. `Shared` is a private nested class with
	//! this class as its sole friend, so its fields stay genuinely encapsulated.
	//! Named `Shared` rather than `State` because the public `State()` accessor
	//! above already occupies that name in this class's scope: a member function
	//! hides a same-named nested type for unqualified lookup, which left
	//! `std::unique_ptr<State>` and `std::make_unique<State>()` resolving to the
	//! accessor instead of the type.
	class Shared final {
		friend class CSenpControlToolReads;
		std::mutex m_mutex;
		std::condition_variable m_work;
		std::condition_variable m_quiet;
		std::deque<Command> m_commands;
		std::vector<Owner> m_owners;
		//! Last settled answer plus the cadence state guarding the next query. The
		//! pending flag is what keeps a per-frame caller from queueing a second one.
		SenpToolAccount m_account;
		bool m_accountPending{};
		bool m_accountAsked{};
		std::chrono::steady_clock::time_point m_accountAskedAt{};
		bool m_busy{};
		bool m_stopped{};
		//! The declaration the window has published, empty until it declares one.
		platform::controlipc::ControlSenpRpcWorkspace m_workspace;
	};

	SenpControlToolReadsOptions m_options;
	//! Engaged only for the adopting constructor, and declared ahead of the
	//! client because the client keeps a reference into it.
	std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader> m_ownedReader;
	platform::controlipc::CControlSenpClient m_client;
	std::unique_ptr<Shared> m_state;
	//! Worker-private: only the worker thread reads or writes it, and Stop joins
	//! the worker before the client is destroyed.
	std::vector<Grant> m_grants;
	//! Worker-private, like the grants and for the same reason: a declaration
	//! belongs to the connection it was made on, so the epoch is part of it.
	platform::controlipc::ControlSenpRpcWorkspace m_declared;
	std::uint64_t m_declaredEpoch{};
	platform::foundation::CNativeWorkerThread m_worker;
};

} // namespace workbench::editor
