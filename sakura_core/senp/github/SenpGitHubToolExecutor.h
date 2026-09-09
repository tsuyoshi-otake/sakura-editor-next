/*! @file
 * @brief Control-owned GitHub execution behind the SENP tool broker seam.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "platform/controlipc/ControlSenpBroker.h"
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
#include <thread>
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
};

/*!
	@brief Translates one admitted repositoryRead argument list into a policy request.

	The editor names a shape from a closed set and, for the item shapes, a decimal
	id; it never names a path segment, host, owner or repository. Anything outside
	the closed set is refused here instead of being forwarded to the tool policy.
*/
[[nodiscard]] std::optional<GhRepositoryReadRequest> BuildRepositoryReadRequest(
	const GhSelectedRepository& repository, const std::vector<effect::Field>& arguments);

/*!
	@brief Bounded GitHub implementation of the control-side tool execution seam.

	Frame processing only admits work and drains already-finished state: every
	`gh` invocation runs on this object's single worker thread. The worker never
	holds an execution scope - it writes finished pages into a small keyed cache -
	so removing a scope under the state mutex is by itself enough to guarantee that
	no worker can touch it afterwards. Bodies never travel inside a completion;
	each finished page is published as a text resource whose handle the editor
	reads back in bounded chunks.
*/
class CSenpGitHubToolExecutor final : public platform::controlipc::ISenpToolExecutor {
public:
	//! One connection cannot hold more concurrent reads or scopes than these.
	[[nodiscard]] static constexpr std::size_t MaximumScopes() noexcept { return 32; }
	[[nodiscard]] static constexpr std::size_t MaximumReadsPerScope() noexcept { return 32; }
	[[nodiscard]] static constexpr std::size_t MaximumResourcesPerScope() noexcept { return 8; }
	//! A page larger than this is refused instead of being cached or published.
	[[nodiscard]] static constexpr std::size_t MaximumPageBytes() noexcept { return 2u * 1024u * 1024u; }
	[[nodiscard]] static constexpr std::size_t MaximumCachedPages() noexcept { return 8; }

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

	//! Blocks until the worker has no dispatch left to run. Test-only observation
	//! point; production code drives the executor through the broker alone.
	[[nodiscard]] bool WaitForIdle(std::uint32_t timeoutMilliseconds) noexcept;

private:
	struct Read final {
		std::wstring readId;
		std::wstring cacheKey;
		std::uint64_t subscriptionId{};
		std::uint64_t deliveredCycle{};
	};
	struct Page final {
		std::wstring cacheKey;
		std::uint64_t cycle{};
		GhRepositoryResponseStatus status{ GhRepositoryResponseStatus::Failed };
		int httpStatus{};
		std::uint32_t currentPage{ 1 };
		std::optional<std::uint32_t> nextPage;
		std::optional<std::string> etag;
		std::string body;
	};
	struct ScopeState final {
		platform::controlipc::SenpToolExecutionScope scope;
		TextResourceScope resourceScope;
		std::vector<Read> reads;
		std::deque<effect::ToolCompleted> pending;
		std::vector<std::wstring> resources;
	};

	[[nodiscard]] ScopeState* Find(const platform::controlipc::SenpToolExecutionScope& scope) noexcept;
	void Drain(ScopeState& state);
	[[nodiscard]] std::optional<effect::ToolCompleted> Publish(ScopeState& state,
		const Read& read, const Page& page);
	void Release(ScopeState& state) noexcept;
	void Run() noexcept;
	void Execute(const GhReadDispatch& dispatch);
	[[nodiscard]] const GhToolProbe& Probe();

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
	bool m_dispatching{};
	bool m_stopping{};

	//! Worker-thread-only lazy probe; frame processing never runs `gh --version`.
	std::optional<GhToolProbe> m_probe;
	std::thread m_worker;
};

} // namespace senp::github
