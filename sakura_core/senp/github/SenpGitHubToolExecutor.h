/*! @file
 * @brief Control-owned GitHub execution behind the SENP tool broker seam.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "platform/controlipc/ControlSenpBroker.h"
#include "platform/foundation/NativeWorkerThread.h"
#include "senp/SenpTextResource.h"
#include "senp/github/GhConnectionLifecycle.h"
#include "senp/github/GhReadScheduler.h"
#include "senp/github/GhRepositoryRead.h"
#include "senp/github/GhRepositorySelection.h"
#include "senp/github/GhToolPolicy.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace senp::github {

/*!
	@brief Per-profile GitHub state the control runtime owns.

	The executor adopts no account and resolves no workspace for itself. A profile
	that has neither must fail its reads explicitly rather than let the executor
	invent a host, owner or repository. Both methods are called from the executor's
	worker thread as well as from frame processing, so an implementation must be
	safe for concurrent use and must not block on an external process.
*/
class ISenpGitHubProfileSource {
public:
	virtual ~ISenpGitHubProfileSource() = default;
	//! Null while the profile has adopted no account.
	[[nodiscard]] virtual std::shared_ptr<CGhConnectionLifecycle> Connection(std::wstring_view profileId) = 0;
	//! Empty while the profile's workspace resolves to no single GitHub repository.
	[[nodiscard]] virtual std::optional<GhSelectedRepository> Repository(std::wstring_view profileId) = 0;
	/*!
		@brief Records one connection's declared workspace, or withdraws it.

		The executor holds no workspace of its own, because resolving folders to a
		repository means reading git remotes, and that belongs on the refresh
		worker that already owns every blocking lookup here. These two only carry
		the declaration to whoever does own it.
	*/
	[[nodiscard]] virtual platform::controlipc::EControlSenpRpcStatus AdoptWorkspace(
		const platform::controlipc::SenpWorkspaceAdoption& adoption) = 0;
	virtual void WithdrawWorkspace(
		const platform::controlipc::SenpConnectionIdentity& connection) = 0;
};

/*!
	@brief Translates one admitted repositoryRead argument list into a policy request.

	The editor names a shape from a closed set and, for the item shapes, a decimal
	id - plus a second decimal number for the two run-attempt shapes, which no
	single id identifies. It never names a path segment, host, owner or
	repository. Anything outside the closed set is refused here instead of being
	forwarded to the tool policy.
*/
[[nodiscard]] std::optional<GhRepositoryReadRequest> BuildRepositoryReadRequest(
	const GhSelectedRepository& repository, const std::vector<effect::Field>& arguments);

/*!
	@brief Translates one admitted jobLog argument list into a policy request.

	A job log names exactly one job of the repository the control side already
	resolved, so the only argument is that job's decimal id. The editor never
	names the host, owner, repository or the log's own short-lived download URL,
	which the GitHub CLI follows internally and never exposes.
*/
[[nodiscard]] std::optional<GhJobLogRequest> BuildJobLogRequest(
	const GhSelectedRepository& repository, const std::vector<effect::Field>& arguments);

/*!
	@brief Bounded GitHub implementation of the control-side tool execution seam.

	Frame processing only admits work and drains already-finished state: every
	`gh` invocation runs on this object's single worker thread. The worker never
	holds an execution scope - it writes finished pages into a small keyed cache -
	so removing a scope under the state mutex is by itself enough to guarantee that
	no worker can touch it afterwards. A finished page carries its body inside the
	completion, because parsing it is the extension's own work and the protocol
	gives an extension no way to read a resource; the same bytes are also kept as
	a text resource while a slot is free, so a document may name the raw response.
	A log is the other way round: it is only ever read back in bounded chunks.
*/
class CSenpGitHubToolExecutor final : public platform::controlipc::ISenpToolExecutor {
public:
	//! One connection cannot hold more concurrent reads or scopes than these.
	[[nodiscard]] static constexpr std::size_t MaximumScopes() noexcept { return 32; }
	[[nodiscard]] static constexpr std::size_t MaximumReadsPerScope() noexcept { return 32; }
	[[nodiscard]] static constexpr std::size_t MaximumResourcesPerScope() noexcept { return 8; }
	//! A page larger than this is refused instead of being cached or published.
	[[nodiscard]] static constexpr std::size_t MaximumPageBytes() noexcept { return 2u * 1024u * 1024u; }
	/*!
		@brief Largest body one completion may carry.

		The wire and the effect protocol both bound a completion's data at
		256 KiB, and the rest of the envelope has to fit beside the body. A page
		over this is refused by name rather than delivered short: a truncated JSON
		body is not a smaller page, it is one nothing can parse.
	*/
	[[nodiscard]] static constexpr std::size_t MaximumInlineBodyBytes() noexcept { return 248u * 1024u; }
	[[nodiscard]] static constexpr std::size_t MaximumCachedPages() noexcept { return 8; }
	//! A log downloads on the same single worker every page fetch runs on, so
	//! this is what bounds how much work one connection can queue ahead of
	//! another's. A log that does not fit is refused, never silently dropped.
	[[nodiscard]] static constexpr std::size_t MaximumQueuedLogs() noexcept { return 4; }

	CSenpGitHubToolExecutor(std::shared_ptr<const IGhToolPlatform> toolPlatform,
		std::shared_ptr<ISenpGitHubProfileSource> profiles, std::wstring workingDirectory);
	~CSenpGitHubToolExecutor() override;
	CSenpGitHubToolExecutor(const CSenpGitHubToolExecutor&) = delete;
	CSenpGitHubToolExecutor& operator=(const CSenpGitHubToolExecutor&) = delete;

	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus StartRead(
		const platform::controlipc::SenpToolExecutionScope& scope,
		const platform::controlipc::SenpToolReadCommand& command) noexcept override;
	[[nodiscard]] std::optional<effect::ToolCompleted> TakeCompleted(
		const platform::controlipc::SenpToolExecutionScope& scope) noexcept override;
	void CancelRead(const platform::controlipc::SenpToolExecutionScope& scope,
		std::wstring_view readId) noexcept override;
	void CancelScope(const platform::controlipc::SenpToolExecutionScope& scope) noexcept override;
	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus ReadResource(
		const platform::controlipc::SenpToolExecutionScope& scope, std::wstring_view handle,
		std::uint64_t offset, std::uint32_t length,
		platform::controlipc::ControlSenpRpcResponse& response) noexcept override;
	void ReleaseResource(const platform::controlipc::SenpToolExecutionScope& scope,
		std::wstring_view handle) noexcept override;
	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus QueryAccount(std::wstring_view profileId,
		platform::controlipc::ControlSenpRpcResponse& response) noexcept override;
	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus AdoptWorkspace(
		const platform::controlipc::SenpWorkspaceAdoption& adoption) noexcept override;
	void WithdrawWorkspace(
		const platform::controlipc::SenpConnectionIdentity& connection) noexcept override;

	//! Blocks until the worker has no dispatch left to run. Test-only observation
	//! point; production code drives the executor through the broker alone.
	[[nodiscard]] bool WaitForIdle(std::uint32_t timeoutMilliseconds) noexcept;

private:
	// The five structs below are private members of this class already - no
	// external caller can name them - but each is its own type with its own
	// fresh public/private access state, so their data fields were still
	// public-by-default from that type's own point of view. Every field moves
	// behind an explicit `private:` label with `friend class CSenpGitHubToolExecutor;`
	// so the sole owner keeps full field access while each struct's own public
	// surface narrows to what it actually needs to expose (nothing, for these
	// plain state records).
	struct Read final {
		Read(std::wstring readIdValue, std::wstring shapeValue, std::wstring cacheKeyValue,
			GhReadSubscription subscriptionValue, std::uint64_t deliveredCycleValue) noexcept
			: readId(std::move(readIdValue)), shape(std::move(shapeValue)), cacheKey(std::move(cacheKeyValue)),
			subscription(std::move(subscriptionValue)), deliveredCycle(deliveredCycleValue)
		{
		}

	private:
		friend class CSenpGitHubToolExecutor;

		std::wstring readId;
		//! The shape this read asked for. The page it answers with is reduced to
		//! that shape's stated fields, so the answer cannot be published without
		//! remembering which question it answers.
		std::wstring shape;
		std::wstring cacheKey;
		//! The scheduler admission this read owns. Erasing the read is the whole
		//! of the release, so no cancellation path has to remember a second step.
		GhReadSubscription subscription;
		std::uint64_t deliveredCycle{};
		//! The page resource this read currently owns, empty when it owns none.
		//! One read holds at most one: a refresh replaces what it published, so a
		//! subscription that refreshes forever cannot exhaust the scope's slots.
		std::wstring resource;
	};
	struct Page final {
	private:
		friend class CSenpGitHubToolExecutor;

		std::wstring cacheKey;
		std::uint64_t cycle{};
		GhRepositoryResponseStatus status{ GhRepositoryResponseStatus::Failed };
		int httpStatus{};
		std::uint32_t currentPage{ 1 };
		std::optional<std::uint32_t> nextPage;
		std::optional<std::string> etag;
		std::string body;
	};
	//! One finished log and the store that received it. The store held nothing
	//! else, so releasing the log is simply dropping this.
	struct LogResource final {
		LogResource(std::wstring handleValue, TextResourceScope resourceScopeValue,
			std::unique_ptr<SenpTextResourceStore> storeValue) noexcept
			: handle(std::move(handleValue)), resourceScope(std::move(resourceScopeValue)), store(std::move(storeValue))
		{
		}

	private:
		friend class CSenpGitHubToolExecutor;

		std::wstring handle;
		TextResourceScope resourceScope;
		std::unique_ptr<SenpTextResourceStore> store;
	};
	struct ScopeState final {
	private:
		friend class CSenpGitHubToolExecutor;

		platform::controlipc::SenpToolExecutionScope scope;
		TextResourceScope resourceScope;
		std::vector<Read> reads;
		std::deque<effect::ToolCompleted> pending;
		std::vector<std::wstring> resources;
		//! Log reads admitted but not yet answered. A read identity that is no
		//! longer here is one nothing may still be completed for.
		std::vector<std::wstring> logReads;
		std::vector<LogResource> logs;
	};
	/*!
		@brief One queued job-log download, owned by the worker while it runs.

		A log is streamed straight into a text resource rather than arriving as a
		page, and the store that receives it is a single-thread store, so each job
		carries its own: the worker writes into it alone, and only a finished job
		hands it to the scope under the state mutex. Everything else here is a
		copy, so the worker still holds no scope of its own.
	*/
	struct LogJob final {
		LogJob(platform::controlipc::SenpToolExecutionScope scopeValue, std::wstring readIdValue,
			std::wstring profileIdValue, TextResourceScope resourceScopeValue, GhJobLogRequest requestValue,
			std::unique_ptr<SenpTextResourceStore> storeValue, std::wstring handleValue = {})
			: scope(std::move(scopeValue)), readId(std::move(readIdValue)), profileId(std::move(profileIdValue)),
			resourceScope(std::move(resourceScopeValue)), request(std::move(requestValue)), store(std::move(storeValue)),
			handle(std::move(handleValue))
		{
		}

	private:
		friend class CSenpGitHubToolExecutor;

		platform::controlipc::SenpToolExecutionScope scope;
		std::wstring readId;
		std::wstring profileId;
		TextResourceScope resourceScope;
		GhJobLogRequest request;
		std::unique_ptr<SenpTextResourceStore> store;
		std::wstring handle;
	};

	[[nodiscard]] ScopeState* Find(const platform::controlipc::SenpToolExecutionScope& scope) noexcept;
	//! Resolves the scope's state, admitting it the first time. Called with the
	//! state mutex held; a refusal here is the refusal of the read that asked.
	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus Ensure(
		const platform::controlipc::SenpToolExecutionScope& scope, ScopeState*& state);
	[[nodiscard]] platform::controlipc::EControlSenpRpcStatus StartJobLog(
		const platform::controlipc::SenpToolExecutionScope& scope,
		const platform::controlipc::SenpToolReadCommand& command);
	void Drain(ScopeState& state);
	[[nodiscard]] std::optional<effect::ToolCompleted> Publish(ScopeState& state,
		Read& read, const Page& page);
	//! Drops the page resource one read owns, if it owns one. Called before a
	//! read publishes again and when it ends, so the slot never outlives what
	//! the editor could still be shown.
	void ReleasePage(ScopeState& state, Read& read) noexcept;
	void Release(ScopeState& state) noexcept;
	void Run() noexcept;
	void Execute(const GhReadDispatch& dispatch);
	void ExecuteLog(LogJob& job) noexcept;
	//! Hands one finished log to its scope, or discards it when the scope or the
	//! read identity is gone. Never throws out to the worker loop.
	void RecordLog(LogJob& job, effect::ToolCompleted completed, bool keepResource) noexcept;
	[[nodiscard]] const GhToolProbe& Probe();
	//! The sole place this class starts its worker thread: paired unconditionally
	//! with the destructor's stop-then-join, so the acquisition this rule tracks
	//! appears exactly once, already scoped to its guaranteed release.
	void StartWorker();

	std::shared_ptr<ISenpGitHubProfileSource> m_profiles;
	CGhToolPolicy m_policy;
	CGhRepositoryReader m_reader;
	CGhReadScheduler m_scheduler;

	std::mutex m_mutex;
	std::condition_variable m_wakeWorker;
	std::condition_variable m_idle;
	std::vector<std::unique_ptr<ScopeState>> m_scopes;
	std::deque<Page> m_pages;
	SenpTextResourceStore m_store;
	std::deque<LogJob> m_logQueue;
	//! One log downloads at a time, so one manual-reset event is the whole
	//! cancellation surface: it is reset and claimed under the state mutex. Null
	//! when the event could not be created, which costs cancellation promptness
	//! and nothing else - the download still ends at its own timeout.
	HANDLE m_logStop{};
	platform::controlipc::SenpToolExecutionScope m_runningLogScope;
	std::wstring m_runningLogReadId;
	bool m_runningLog{};
	bool m_dispatching{};
	bool m_stopping{};

	//! Worker-thread-only lazy probe; frame processing never runs `gh --version`.
	std::optional<GhToolProbe> m_probe;
	platform::foundation::CNativeWorkerThread m_worker;
};

} // namespace senp::github
