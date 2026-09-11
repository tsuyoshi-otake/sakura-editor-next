/*! @file
	@brief Control-process composition of the SENP tool broker and its GitHub path.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlSenpBroker.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "senp/SenpControlPackageAuthority.h"
#include "senp/SenpManagementService.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace platform::controlipc {

/*!
	@brief Control-owned package state for one profile home.

	Refresh runs the external package tool and blocks, so the composition calls
	it only from its refresh worker: never from frame processing, never from
	grant validation and never from a poll.
*/
class IControlSenpPackageSource {
public:
	virtual ~IControlSenpPackageSource() = default;
	//! An empty result means the profile's package state could not be read at
	//! all. A returned snapshot still has to prove its own enablement to the
	//! authority; this seam never decides who may hold a capability.
	[[nodiscard]] virtual std::optional<senp::ManagementSnapshot> Refresh(
		const std::wstring& profileHome) = 0;
	virtual void Close() noexcept = 0;
};

/*!
	@brief Production package source over one management service per profile home.

	The service is started once and later refreshed. Restarting it would reload
	the built-in catalog and install default packages again, which is not what a
	permission refresh may cost.
*/
class CControlSenpPackageSource final : public IControlSenpPackageSource {
public:
	//! Bounded number of profile homes this source keeps a service for.
	[[nodiscard]] static constexpr std::size_t MaximumProfileHomes() noexcept { return 8; }

	using Factory =
		std::function<std::shared_ptr<senp::ISenpManagementService>(const std::wstring&)>;
	//! A disengaged factory selects the production Win32 management service.
	explicit CControlSenpPackageSource(Factory factory = {});
	~CControlSenpPackageSource() override;
	CControlSenpPackageSource(const CControlSenpPackageSource&) = delete;
	CControlSenpPackageSource& operator=(const CControlSenpPackageSource&) = delete;

	[[nodiscard]] std::optional<senp::ManagementSnapshot> Refresh(
		const std::wstring& profileHome) override;
	void Close() noexcept override;

private:
	Factory m_factory;
	std::mutex m_mutex;
	std::map<std::wstring, std::shared_ptr<senp::ISenpManagementService>, std::less<>> m_services;
	bool m_closed = false;
};

/*!
	@brief Bounded, deduplicating and rate limited refresh admission.

	Request() runs inside grant issue and grant validation, so it touches only
	this object's own state: it never reaches the package tool, the filesystem or
	an external process. One profile cannot schedule work faster than its own
	interval, so a five-hundred-millisecond poll cannot turn into a package-tool
	reload loop.
*/
class CControlSenpRefreshQueue final {
public:
	CControlSenpRefreshQueue(std::chrono::milliseconds minimumInterval, std::size_t maximumPending);
	CControlSenpRefreshQueue(const CControlSenpRefreshQueue&) = delete;
	CControlSenpRefreshQueue& operator=(const CControlSenpRefreshQueue&) = delete;

	//! True only when this call admitted new work for the profile.
	bool Request(std::wstring_view profileId);
	/*!
		@brief Admits work for a profile whose control-owned state actually changed.

		The interval exists so a five-hundred-millisecond poll cannot become a
		package-tool reload loop. An edge - a connection declaring a workspace, or
		dropping the one it declared - is not a poll, and making it wait out an
		interval would leave the editor looking at a workspace the control side
		has already been told about. Deduplication and the pending bound still
		apply, so an editor cannot turn repeated declarations into repeated work.
	*/
	bool RequestChanged(std::wstring_view profileId);
	//! Blocks until work is admitted or the queue closes. An empty result means closed.
	[[nodiscard]] std::optional<std::wstring> WaitAndTake();
	//! Records the attempt so the interval starts at its end, not at its start.
	void Complete(const std::wstring& profileId);
	[[nodiscard]] bool WaitForIdle(std::uint32_t timeoutMilliseconds);
	void Close() noexcept;

private:
	//! The one admission path. Both entry points differ only in whether the
	//! poll interval applies, so the bound and the deduplication cannot drift.
	bool Admit(std::wstring_view profileId, bool respectInterval);

	const std::chrono::milliseconds m_minimumInterval;
	const std::size_t m_maximumPending;
	std::mutex m_mutex;
	std::condition_variable m_admitted;
	std::condition_variable m_idle;
	std::deque<std::wstring> m_pending;
	std::map<std::wstring, std::chrono::steady_clock::time_point, std::less<>> m_attempted;
	std::wstring m_running;
	//! A change reported for the profile currently being refreshed. The pass
	//! in flight may already have read the state that change replaced, so it
	//! is re-admitted when the attempt ends instead of being dropped.
	bool m_changedWhileRunning = false;
	bool m_busy = false;
	bool m_closed = false;
};

/*!
	@brief Grant authority the registry resolves against, with refresh admission.

	Resolution stays a bounded lookup over the already published table. A profile
	that has no published table has never had its packages read, or lost them, so
	the miss also admits one rate limited refresh. The miss itself still refuses
	the grant: nothing is issued on the strength of a request for a refresh.
*/
class CControlSenpAuthorityGate final : public senp::ISenpToolGrantAuthority {
public:
	CControlSenpAuthorityGate(std::shared_ptr<const senp::CSenpControlPackageAuthority> authority,
		std::shared_ptr<CControlSenpRefreshQueue> refresh);
	[[nodiscard]] std::optional<senp::SenpApprovedToolOwner> Resolve(
		std::wstring_view profileId, std::wstring_view extensionId) const override;

private:
	std::shared_ptr<const senp::CSenpControlPackageAuthority> m_authority;
	std::shared_ptr<CControlSenpRefreshQueue> m_refresh;
};

/*!
	@brief Production per-profile GitHub state for the tool executor.

	Both lookups are bounded and never block, so they answer only what the
	composition's refresh worker has already published. A profile whose account
	has not been adopted answers a null connection and one whose workspace
	resolves to no single GitHub repository answers an empty selection, which the
	executor reports as NotConnected and Unavailable instead of inventing a host,
	owner or repository.
*/
class CControlSenpProfileSource final : public senp::github::ISenpGitHubProfileSource {
public:
	[[nodiscard]] static constexpr std::size_t MaximumProfiles() noexcept { return 8; }

	//! Connections whose declarations are held at once. It bounds the store the
	//! same way the grant registry bounds what one connection can mint.
	[[nodiscard]] static constexpr std::size_t MaximumAdoptions() noexcept { return 32; }

	CControlSenpProfileSource(std::shared_ptr<senp::CSenpToolGrants> grants,
		std::shared_ptr<senp::github::IGhConnectionPlatform> platform,
		std::wstring configurationDirectory,
		std::shared_ptr<CControlSenpRefreshQueue> refresh
			= nullptr);
	~CControlSenpProfileSource() override;
	CControlSenpProfileSource(const CControlSenpProfileSource&) = delete;
	CControlSenpProfileSource& operator=(const CControlSenpProfileSource&) = delete;

	[[nodiscard]] std::shared_ptr<senp::github::CGhConnectionLifecycle> Connection(
		std::wstring_view profileId) override;
	[[nodiscard]] std::optional<senp::github::GhSelectedRepository> Repository(
		std::wstring_view profileId) override;
	[[nodiscard]] EControlSenpRpcStatus AdoptWorkspace(
		const SenpWorkspaceAdoption& adoption) override;
	void WithdrawWorkspace(const SenpConnectionIdentity& connection) override;

	/*!
		@brief The one workspace every connection open on this profile agrees on.

		Empty when none has declared one, and empty when two disagree: a profile
		is shared by every window that selected it, so two windows on different
		folders are ambiguity, and ambiguity publishes nothing. That is the same
		rule the remote selection applies to a workspace naming two repositories.
	*/
	[[nodiscard]] std::optional<ControlSenpRpcWorkspace> DeclaredWorkspace(
		std::wstring_view profileId) const;

	//! Worker-only. Creates this profile's connection lifecycle while the source
	//! still admits one. It adopts no account by itself.
	[[nodiscard]] std::shared_ptr<senp::github::CGhConnectionLifecycle> Adopt(const std::wstring& profileId);
	//! Worker-only. An empty selection withdraws the profile's repository.
	void PublishRepository(const std::wstring& profileId,
		std::optional<senp::github::GhSelectedRepository> repository);
	void Withdraw(std::wstring_view profileId) noexcept;
	void Close() noexcept;

private:
	//! Private state with accessors/setters for the same encapsulation reason as
	//! every other DTO in this subsystem, even though this one never leaves the
	//! owning source.
	class Profile final {
	public:
		Profile() = default;
		Profile(std::shared_ptr<senp::github::CGhConnectionLifecycle> connection,
			std::optional<senp::github::GhSelectedRepository> repository) :
			m_connection(std::move(connection)), m_repository(std::move(repository))
		{
		}

		[[nodiscard]] const std::shared_ptr<senp::github::CGhConnectionLifecycle>& Connection() const noexcept
		{
			return m_connection;
		}
		[[nodiscard]] const std::optional<senp::github::GhSelectedRepository>& Repository() const noexcept
		{
			return m_repository;
		}

		void SetConnection(std::shared_ptr<senp::github::CGhConnectionLifecycle> value)
		{
			m_connection = std::move(value);
		}
		void SetRepository(std::optional<senp::github::GhSelectedRepository> value)
		{
			m_repository = std::move(value);
		}

	private:
		std::shared_ptr<senp::github::CGhConnectionLifecycle> m_connection;
		std::optional<senp::github::GhSelectedRepository> m_repository;
	};
	//! Held per connection, never per profile: a declaration has to disappear
	//! with the window that made it, and only the connection identifies that.
	//! Private state; `connection` is set once at construction and never
	//! reassigned, so it needs only a getter.
	class Adoption final {
	public:
		Adoption(SenpConnectionIdentity connection, std::wstring profileId,
			ControlSenpRpcWorkspace workspace) :
			m_connection(std::move(connection)), m_profileId(std::move(profileId)),
			m_workspace(std::move(workspace))
		{
		}

		[[nodiscard]] const SenpConnectionIdentity& Connection() const noexcept { return m_connection; }
		[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
		[[nodiscard]] const ControlSenpRpcWorkspace& Workspace() const noexcept { return m_workspace; }

		void SetProfileId(std::wstring value) { m_profileId = std::move(value); }
		void SetWorkspace(ControlSenpRpcWorkspace value) { m_workspace = std::move(value); }

	private:
		SenpConnectionIdentity m_connection;
		std::wstring m_profileId;
		ControlSenpRpcWorkspace m_workspace;
	};

	//! Requests a refresh for each named profile. Called with no lock held,
	//! because the queue wakes the worker that calls back into this source.
	void Resolve(const std::vector<std::wstring>& profileIds) noexcept;

	std::shared_ptr<senp::CSenpToolGrants> m_grants;
	std::shared_ptr<senp::github::IGhConnectionPlatform> m_platform;
	const std::wstring m_configurationDirectory;
	std::shared_ptr<CControlSenpRefreshQueue> m_refresh;
	//! Owned separately from the class so DeclaredWorkspace() can lock it from a
	//! const method without a class-level mutable member, matching
	//! ControlSenpClient.h.
	std::unique_ptr<std::mutex> m_mutex = std::make_unique<std::mutex>();
	std::map<std::wstring, Profile, std::less<>> m_profiles;
	std::vector<Adoption> m_adoptions;
	bool m_closed = false;
};

//! Immutable inputs the control runtime has already resolved for itself. The
//! authority identity and the control profile root are the canonical pair the
//! profile bootstrap resolves a profile home from; they are never editor claims.
//! Private state with accessors/setters, matching every other DTO in this
//! subsystem: callers build one with the default constructor and setters, then
//! pass it by value into the composition, which never mutates it afterwards.
class ControlSenpCompositionOptions {
public:
	ControlSenpCompositionOptions() = default;

	[[nodiscard]] const std::wstring& ControlProfileRoot() const noexcept { return m_controlProfileRoot; }
	[[nodiscard]] const std::string& ControlAuthorityId() const noexcept { return m_controlAuthorityId; }
	[[nodiscard]] std::uint64_t ControlAuthorityGeneration() const noexcept
	{
		return m_controlAuthorityGeneration;
	}
	//! Absolute GH_CONFIG_DIR of the control process's own user. An empty value
	//! selects the location the GitHub CLI itself would use for that user.
	[[nodiscard]] const std::wstring& GhConfigurationDirectory() const noexcept
	{
		return m_ghConfigurationDirectory;
	}
	//! Absolute working directory for `gh` invocations. Empty selects the
	//! control profile root.
	[[nodiscard]] const std::wstring& WorkingDirectory() const noexcept { return m_workingDirectory; }
	//! The hostname the connection check adopts an account for.
	[[nodiscard]] const std::wstring& Hostname() const noexcept { return m_hostname; }

	void SetControlProfileRoot(std::wstring value) { m_controlProfileRoot = std::move(value); }
	void SetControlAuthorityId(std::string value) { m_controlAuthorityId = std::move(value); }
	void SetControlAuthorityGeneration(std::uint64_t value) noexcept { m_controlAuthorityGeneration = value; }
	void SetGhConfigurationDirectory(std::wstring value) { m_ghConfigurationDirectory = std::move(value); }
	void SetWorkingDirectory(std::wstring value) { m_workingDirectory = std::move(value); }
	void SetHostname(std::wstring value) { m_hostname = std::move(value); }

private:
	std::wstring m_controlProfileRoot;
	std::string m_controlAuthorityId;
	std::uint64_t m_controlAuthorityGeneration = 0;
	std::wstring m_ghConfigurationDirectory;
	std::wstring m_workingDirectory;
	std::wstring m_hostname = L"github.com";
};

//! Narrow seams for deterministic tests. A disengaged member selects its
//! production implementation; production composition passes none of them.
//! Private state with accessors/setters for the same reason as every other DTO
//! in this subsystem.
class ControlSenpCompositionDependencies {
public:
	ControlSenpCompositionDependencies() = default;

	[[nodiscard]] const std::shared_ptr<IControlSenpPackageSource>& Packages() const noexcept
	{
		return m_packages;
	}
	[[nodiscard]] const std::shared_ptr<const senp::github::IGhToolPlatform>& ToolPlatform() const noexcept
	{
		return m_toolPlatform;
	}
	[[nodiscard]] const std::shared_ptr<senp::github::IGhConnectionPlatform>& ConnectionPlatform() const noexcept
	{
		return m_connectionPlatform;
	}
	[[nodiscard]] const std::shared_ptr<const senp::github::IGhLocalRepositoryPlatform>&
		RepositoryPlatform() const noexcept
	{
		return m_repositoryPlatform;
	}

	void SetPackages(std::shared_ptr<IControlSenpPackageSource> value) { m_packages = std::move(value); }
	void SetToolPlatform(std::shared_ptr<const senp::github::IGhToolPlatform> value)
	{
		m_toolPlatform = std::move(value);
	}
	void SetConnectionPlatform(std::shared_ptr<senp::github::IGhConnectionPlatform> value)
	{
		m_connectionPlatform = std::move(value);
	}
	void SetRepositoryPlatform(std::shared_ptr<const senp::github::IGhLocalRepositoryPlatform> value)
	{
		m_repositoryPlatform = std::move(value);
	}

private:
	std::shared_ptr<IControlSenpPackageSource> m_packages;
	std::shared_ptr<const senp::github::IGhToolPlatform> m_toolPlatform;
	std::shared_ptr<senp::github::IGhConnectionPlatform> m_connectionPlatform;
	std::shared_ptr<const senp::github::IGhLocalRepositoryPlatform> m_repositoryPlatform;
};

/*!
	@brief Owns every control-side SENP object and the one worker that feeds them.

	The broker, the grant registry, the trusted package table, the per-profile
	GitHub state and the tool executor have one lifetime here, so the control
	runtime composes a single object and injects one frame handler. Everything
	that blocks - reading a profile's installed packages, checking a GitHub
	account, inspecting a workspace for its remotes - runs on this object's own
	refresh worker. Frame processing keeps its bounded admission-only contract.

	The worker is never started by Start(): a refresh is admitted when a grant
	resolution finds no published table for a profile, so an editor that never
	asks for a capability costs nothing, and one that does gets its table read
	once per interval rather than once per poll.
*/
class CControlSenpComposition final {
public:
	[[nodiscard]] static constexpr std::chrono::milliseconds MinimumRefreshInterval() noexcept
	{
		return std::chrono::milliseconds{ 30000 };
	}
	[[nodiscard]] static constexpr std::size_t MaximumPendingRefreshes() noexcept { return 8; }

	CControlSenpComposition(ControlSenpCompositionOptions options,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
		ControlSenpCompositionDependencies dependencies
			= {});
	~CControlSenpComposition();
	CControlSenpComposition(const CControlSenpComposition&) = delete;
	CControlSenpComposition& operator=(const CControlSenpComposition&) = delete;

	//! The additive SenpRequest/SenpResponse handler the service host accepts.
	[[nodiscard]] std::shared_ptr<IControlIpcFrameHandler> Handler() const noexcept { return m_handler; }
	//! Bounded, deduplicated and rate limited; safe from any thread.
	bool RequestRefresh(std::wstring_view profileId);
	//! Revokes every grant, closes every connection and joins the worker.
	void Close() noexcept;

	//! Test-only observation points; production drives this through the handler.
	[[nodiscard]] bool WaitForIdle(std::uint32_t timeoutMilliseconds);
	[[nodiscard]] std::optional<std::uint64_t> PublishedRevision(std::wstring_view profileId) const;
	[[nodiscard]] std::optional<senp::github::GhSelectedRepository> PublishedRepository(
		std::wstring_view profileId) const;
	[[nodiscard]] senp::github::GhConnectionState ConnectionState(std::wstring_view profileId) const;

private:
	void Run() noexcept;
	void Refresh(const std::wstring& profileId);
	[[nodiscard]] std::optional<std::wstring> ResolveProfileHome(const std::wstring& profileId,
		const profiles::UserDataProfileRegistrySnapshot& registry) const;
	void RefreshRepository(const std::wstring& profileId);
	void RefreshConnection(const std::wstring& profileId);
	void Discard(const std::wstring& profileId) noexcept;

	const ControlSenpCompositionOptions m_options;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profileRegistry;
	std::shared_ptr<const senp::github::IGhToolPlatform> m_toolPlatform;
	std::shared_ptr<IControlSenpPackageSource> m_packages;
	std::shared_ptr<senp::CSenpControlPackageAuthority> m_authority;
	std::shared_ptr<CControlSenpRefreshQueue> m_refresh;
	std::shared_ptr<senp::CSenpToolGrants> m_grants;
	std::shared_ptr<CControlSenpProfileSource> m_profiles;
	std::shared_ptr<senp::github::CSenpGitHubToolExecutor> m_executor;
	std::shared_ptr<IControlIpcFrameHandler> m_handler;
	senp::github::CGhToolPolicy m_policy;
	senp::github::CGhRepositorySelection m_repositories;
	std::mutex m_workerMutex;
	std::optional<senp::github::GhToolProbe> m_probe;
	bool m_closed = false;
	std::thread m_worker;
};

} // namespace platform::controlipc
