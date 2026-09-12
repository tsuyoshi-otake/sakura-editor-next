/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlSenpComposition.h"

#include "config/WorkspaceContextTypes.h"
#include "platform/process/WindowsExecutableResolver.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include "platform/profiles/UserDataProfileIdentity.h"

#include <algorithm>
#include <utility>

namespace platform::controlipc {

namespace {

//! The GitHub CLI reads its account state from GH_CONFIG_DIR, and from the
//! user's roaming application data when that variable is unset. The control
//! process runs as that same user, so this resolves the one directory whose
//! accounts it may adopt. It invents nothing: an unresolvable location leaves
//! the value empty and every connection attempt is refused instead.
[[nodiscard]] std::wstring ResolveGhConfigurationDirectory()
{
	std::wstring buffer(1024, L'\0');
	const auto read = [&buffer](const wchar_t* name) -> std::optional<std::wstring> {
		const auto length = ::GetEnvironmentVariableW(name, buffer.data(),
			static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size()) return std::nullopt;
		return std::wstring(buffer.data(), length);
	};
	if (const auto explicitDirectory = read(L"GH_CONFIG_DIR");
		explicitDirectory && ::platform::IsAbsoluteWindowsPath(*explicitDirectory)) {
		return *explicitDirectory;
	}
	const auto roaming = read(L"APPDATA");
	if (!roaming || !::platform::IsAbsoluteWindowsPath(*roaming)) return {};
	auto resolved = *roaming;
	if (!resolved.empty() && resolved.back() != L'\\') resolved.push_back(L'\\');
	resolved += L"GitHub CLI";
	return resolved;
}

[[nodiscard]] ControlSenpCompositionOptions Normalize(ControlSenpCompositionOptions options)
{
	if (options.WorkingDirectory().empty()) options.SetWorkingDirectory(options.ControlProfileRoot());
	if (options.GhConfigurationDirectory().empty()) {
		options.SetGhConfigurationDirectory(ResolveGhConfigurationDirectory());
	}
	if (!::platform::IsAbsoluteWindowsPath(options.GhConfigurationDirectory())) {
		options.SetGhConfigurationDirectory({});
	}
	return options;
}

} // namespace

CControlSenpPackageSource::CControlSenpPackageSource(Factory factory) :
	m_factory(factory ? std::move(factory)
		: [](const std::wstring& profileHome) -> std::shared_ptr<senp::ISenpManagementService> {
			return std::make_shared<senp::CWin32SenpManagementService>(profileHome);
		})
{
}

CControlSenpPackageSource::~CControlSenpPackageSource()
{
	Close();
}

std::optional<senp::ManagementSnapshot> CControlSenpPackageSource::Refresh(const std::wstring& profileHome)
try {
	if (!::platform::IsAbsoluteWindowsPath(profileHome)) return std::nullopt;
	std::shared_ptr<senp::ISenpManagementService> service;
	bool created = false;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed) return std::nullopt;
		if (const auto found = m_services.find(profileHome); found != m_services.end()) {
			service = found->second;
		} else {
			if (m_services.size() >= MaximumProfileHomes()) return std::nullopt;
			service = m_factory ? m_factory(profileHome) : nullptr;
			if (!service) return std::nullopt;
			m_services.emplace(profileHome, service);
			created = true;
		}
	}
	// Start loads the built-in catalog and may install default packages, so it
	// runs once per profile home. Every later refresh only reloads installed
	// state. The snapshot travels back whatever its terminal was: judging what
	// it proves belongs to the authority, not to this source.
	const auto result = created ? service->Start() : service->Refresh();
	return result.snapshot;
} catch (const std::exception&) {
	return std::nullopt;
}

void CControlSenpPackageSource::Close() noexcept
try {
	std::map<std::wstring, std::shared_ptr<senp::ISenpManagementService>, std::less<>> services;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed) return;
		m_closed = true;
		services.swap(m_services);
	}
	for (auto& [profileHome, service] : services) {
		if (service) service->Stop();
	}
} catch (const std::exception&) {
}

CControlSenpRefreshQueue::CControlSenpRefreshQueue(std::chrono::milliseconds minimumInterval,
	std::size_t maximumPending) :
	m_minimumInterval(minimumInterval), m_maximumPending(maximumPending == 0 ? 1 : maximumPending)
{
}

bool CControlSenpRefreshQueue::Request(std::wstring_view profileId)
{
	return Admit(profileId, true);
}

bool CControlSenpRefreshQueue::RequestChanged(std::wstring_view profileId)
{
	return Admit(profileId, false);
}

bool CControlSenpRefreshQueue::Admit(std::wstring_view profileId, const bool respectInterval)
try {
	if (profileId.empty()) return false;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed || m_pending.size() >= m_maximumPending) return false;
		if (m_busy && m_running == profileId) {
			// A poll that lands on the running attempt is the duplicate this
			// refuses. A change is not: the attempt may have read the workspace
			// before this declaration existed, and nothing else would ask again,
			// so the profile would answer for a repository it no longer has
			// until an unrelated request happened to arrive.
			if (!respectInterval) m_changedWhileRunning = true;
			return false;
		}
		if (std::ranges::find(m_pending, profileId) != m_pending.end()) return false;
		if (respectInterval) {
			if (const auto attempted = m_attempted.find(profileId); attempted != m_attempted.end()
				&& std::chrono::steady_clock::now() - attempted->second < m_minimumInterval) {
				return false;
			}
		}
		m_pending.emplace_back(profileId);
	}
	m_admitted.notify_one();
	return true;
} catch (const std::exception&) {
	return false;
}

std::optional<std::wstring> CControlSenpRefreshQueue::WaitAndTake()
{
	std::unique_lock lock(m_mutex);
	m_admitted.wait(lock, [this]() { return m_closed || !m_pending.empty(); });
	if (m_closed) return std::nullopt;
	auto profileId = std::move(m_pending.front());
	m_pending.pop_front();
	m_running = profileId;
	m_busy = true;
	return profileId;
}

void CControlSenpRefreshQueue::Complete(const std::wstring& profileId)
try {
	bool readmitted = false;
	{
		std::lock_guard lock(m_mutex);
		// The interval starts when the attempt ends, so a slow package tool can
		// never be re-entered by the very poll that was waiting for it.
		m_attempted.insert_or_assign(profileId, std::chrono::steady_clock::now());
		m_running.clear();
		m_busy = false;
		// The change that arrived mid-attempt is admitted now, without the
		// interval it never had to wait out, and still under the pending bound.
		if (m_changedWhileRunning) {
			m_changedWhileRunning = false;
			if (!m_closed && m_pending.size() < m_maximumPending
				&& std::ranges::find(m_pending, profileId) == m_pending.end()) {
				m_pending.emplace_back(profileId);
				readmitted = true;
			}
		}
	}
	if (readmitted) m_admitted.notify_one();
	m_idle.notify_all();
} catch (const std::exception&) {
}

bool CControlSenpRefreshQueue::WaitForIdle(std::uint32_t timeoutMilliseconds)
{
	std::unique_lock lock(m_mutex);
	return m_idle.wait_for(lock, std::chrono::milliseconds{ timeoutMilliseconds },
		[this]() { return m_closed || (m_pending.empty() && !m_busy); });
}

void CControlSenpRefreshQueue::Close() noexcept
try {
	{
		std::lock_guard lock(m_mutex);
		if (m_closed) return;
		m_closed = true;
		m_changedWhileRunning = false;
		m_pending.clear();
	}
	m_admitted.notify_all();
	m_idle.notify_all();
} catch (const std::exception&) {
}

CControlSenpAuthorityGate::CControlSenpAuthorityGate(
	std::shared_ptr<const senp::CSenpControlPackageAuthority> authority,
	std::shared_ptr<CControlSenpRefreshQueue> refresh) :
	m_authority(std::move(authority)), m_refresh(std::move(refresh))
{
}

std::optional<senp::SenpApprovedToolOwner> CControlSenpAuthorityGate::Resolve(
	std::wstring_view profileId, std::wstring_view extensionId) const
{
	auto owner = m_authority ? m_authority->Resolve(profileId, extensionId) : std::nullopt;
	// A miss admits one bounded refresh and still refuses. Nothing is ever issued
	// on the strength of a refresh this call only asked for.
	if (!owner && m_refresh) (void)m_refresh->Request(profileId);
	return owner;
}

CControlSenpProfileSource::CControlSenpProfileSource(std::shared_ptr<senp::CSenpToolGrants> grants,
	std::shared_ptr<senp::github::IGhConnectionPlatform> platform, std::wstring configurationDirectory,
	std::shared_ptr<CControlSenpRefreshQueue> refresh) :
	m_grants(std::move(grants)), m_platform(std::move(platform)),
	m_configurationDirectory(::platform::IsAbsoluteWindowsPath(configurationDirectory)
		? std::move(configurationDirectory) : std::wstring{}),
	m_refresh(std::move(refresh))
{
}

CControlSenpProfileSource::~CControlSenpProfileSource()
{
	Close();
}

std::shared_ptr<senp::github::CGhConnectionLifecycle> CControlSenpProfileSource::Connection(
	std::wstring_view profileId)
try {
	std::lock_guard lock(*m_mutex);
	if (m_closed) return nullptr;
	const auto found = m_profiles.find(profileId);
	return found == m_profiles.end() ? nullptr : found->second.Connection();
} catch (const std::exception&) {
	return nullptr;
}

std::optional<senp::github::GhSelectedRepository> CControlSenpProfileSource::Repository(
	std::wstring_view profileId)
try {
	std::lock_guard lock(*m_mutex);
	if (m_closed) return std::nullopt;
	const auto found = m_profiles.find(profileId);
	return found == m_profiles.end() ? std::nullopt : found->second.Repository();
} catch (const std::exception&) {
	return std::nullopt;
}

std::shared_ptr<senp::github::CGhConnectionLifecycle> CControlSenpProfileSource::Adopt(
	const std::wstring& profileId)
try {
	std::lock_guard lock(*m_mutex);
	if (m_closed || !m_grants || !m_platform || m_configurationDirectory.empty()) return nullptr;
	if (const auto found = m_profiles.find(profileId); found != m_profiles.end()) {
		if (found->second.Connection()) return found->second.Connection();
		found->second.SetConnection(std::make_shared<senp::github::CGhConnectionLifecycle>(
			m_platform, *m_grants, profileId, m_configurationDirectory));
		return found->second.Connection();
	}
	if (m_profiles.size() >= MaximumProfiles()) return nullptr;
	auto connection = std::make_shared<senp::github::CGhConnectionLifecycle>(
		m_platform, *m_grants, profileId, m_configurationDirectory);
	m_profiles.emplace(profileId, Profile{ connection, std::nullopt });
	return connection;
} catch (const std::exception&) {
	return nullptr;
}

EControlSenpRpcStatus CControlSenpProfileSource::AdoptWorkspace(const SenpWorkspaceAdoption& adoption)
try {
	std::vector<std::wstring> changed;
	{
		std::lock_guard lock(*m_mutex);
		if (m_closed) return EControlSenpRpcStatus::Closed;
		const auto held = std::ranges::find_if(m_adoptions,
			[&](const Adoption& existing) { return existing.Connection() == adoption.Connection(); });
		if (held != m_adoptions.end()) {
			// Re-declaring what this connection already declared is not a change.
			// Answering it with work would let a reconnecting editor drive the
			// worker by saying the same thing again.
			if (held->ProfileId() == adoption.ProfileId() && held->Workspace() == adoption.Workspace()) {
				return EControlSenpRpcStatus::Succeeded;
			}
			// A connection that moves to another profile changes both: the one it
			// leaves loses a declaration the resolution was still counting.
			if (held->ProfileId() != adoption.ProfileId()) changed.push_back(held->ProfileId());
			held->SetProfileId(adoption.ProfileId());
			held->SetWorkspace(adoption.Workspace());
		} else {
			if (m_adoptions.size() >= MaximumAdoptions()) {
				return EControlSenpRpcStatus::ResourceExhausted;
			}
			m_adoptions.push_back({ adoption.Connection(), adoption.ProfileId(), adoption.Workspace() });
		}
		changed.push_back(adoption.ProfileId());
	}
	Resolve(changed);
	return EControlSenpRpcStatus::Succeeded;
} catch (const std::exception&) {
	return EControlSenpRpcStatus::Unavailable;
}

void CControlSenpProfileSource::WithdrawWorkspace(const SenpConnectionIdentity& connection)
try {
	std::vector<std::wstring> changed;
	{
		std::lock_guard lock(*m_mutex);
		// Not guarded by m_closed: a closed source has already dropped every
		// declaration, so there is nothing left for this to leave behind.
		const auto held = std::ranges::find_if(m_adoptions,
			[&](const Adoption& existing) { return existing.Connection() == connection; });
		if (held == m_adoptions.end()) return;
		changed.push_back(held->ProfileId());
		m_adoptions.erase(held);
	}
	// The profile keeps answering the withdrawn repository until this resolves,
	// which is why a withdrawal admits work rather than only forgetting.
	Resolve(changed);
} catch (const std::exception&) {
}

std::optional<ControlSenpRpcWorkspace> CControlSenpProfileSource::DeclaredWorkspace(
	std::wstring_view profileId) const
try {
	std::lock_guard lock(*m_mutex);
	if (m_closed) return std::nullopt;
	std::optional<ControlSenpRpcWorkspace> agreed;
	for (const auto& adoption : m_adoptions) {
		if (adoption.ProfileId() != profileId) continue;
		if (!agreed) {
			agreed = adoption.Workspace();
			continue;
		}
		if (agreed->Folders() != adoption.Workspace().Folders()) return std::nullopt;
		// The same workspace observed by two windows at different times. The
		// newer observation is the one a staleness check has to be made against.
		if (adoption.Workspace().Revision() > agreed->Revision()) agreed = adoption.Workspace();
	}
	return agreed;
} catch (const std::exception&) {
	return std::nullopt;
}

void CControlSenpProfileSource::Resolve(const std::vector<std::wstring>& profileIds) noexcept
try {
	if (!m_refresh) return;
	for (const auto& profileId : profileIds) {
		(void)m_refresh->RequestChanged(profileId);
	}
} catch (const std::exception&) {
}

void CControlSenpProfileSource::PublishRepository(const std::wstring& profileId,
	std::optional<senp::github::GhSelectedRepository> repository)
try {
	std::lock_guard lock(*m_mutex);
	if (m_closed) return;
	if (const auto found = m_profiles.find(profileId); found != m_profiles.end()) {
		found->second.SetRepository(std::move(repository));
		return;
	}
	if (!repository || m_profiles.size() >= MaximumProfiles()) return;
	m_profiles.emplace(profileId, Profile{ nullptr, std::move(repository) });
} catch (const std::exception&) {
}

void CControlSenpProfileSource::Withdraw(std::wstring_view profileId) noexcept
try {
	std::shared_ptr<senp::github::CGhConnectionLifecycle> connection;
	{
		std::lock_guard lock(*m_mutex);
		const auto found = m_profiles.find(profileId);
		if (found == m_profiles.end()) return;
		connection = found->second.Connection();
		m_profiles.erase(found);
	}
	if (connection) connection->Close();
} catch (const std::exception&) {
}

void CControlSenpProfileSource::Close() noexcept
try {
	std::map<std::wstring, Profile, std::less<>> profiles;
	{
		std::lock_guard lock(*m_mutex);
		if (m_closed) return;
		m_closed = true;
		profiles.swap(m_profiles);
		m_adoptions.clear();
	}
	for (auto& [profileId, profile] : profiles) {
		if (profile.Connection()) profile.Connection()->Close();
	}
} catch (const std::exception&) {
}

CControlSenpComposition::CControlSenpComposition(ControlSenpCompositionOptions options,
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
	ControlSenpCompositionDependencies dependencies) :
	m_options(Normalize(std::move(options))),
	m_profileRegistry(std::move(profiles)),
	m_toolPlatform(dependencies.ToolPlatform() ? dependencies.ToolPlatform()
		: std::make_shared<const senp::github::CWindowsGhToolPlatform>()),
	m_packages(dependencies.Packages() ? dependencies.Packages()
		: std::make_shared<CControlSenpPackageSource>()),
	m_authority(std::make_shared<senp::CSenpControlPackageAuthority>()),
	m_refresh(std::make_shared<CControlSenpRefreshQueue>(
		MinimumRefreshInterval(), MaximumPendingRefreshes())),
	m_grants(std::make_shared<senp::CSenpToolGrants>(
		std::make_shared<CControlSenpAuthorityGate>(m_authority, m_refresh))),
	m_profiles(std::make_shared<CControlSenpProfileSource>(m_grants,
		dependencies.ConnectionPlatform() ? dependencies.ConnectionPlatform()
			: std::make_shared<senp::github::CWindowsGhConnectionPlatform>(
				m_toolPlatform, m_options.WorkingDirectory()),
		m_options.GhConfigurationDirectory(), m_refresh)),
	m_executor(std::make_shared<senp::github::CSenpGitHubToolExecutor>(
		m_toolPlatform, m_profiles, m_options.WorkingDirectory())),
	m_handler(std::make_shared<CControlSenpBroker>(m_grants, m_executor)),
	m_policy(m_toolPlatform, m_options.WorkingDirectory()),
	m_repositories(dependencies.RepositoryPlatform() ? dependencies.RepositoryPlatform()
		: std::make_shared<const senp::github::CWindowsGhLocalRepositoryPlatform>())
{
	m_worker = foundation::CNativeWorkerThread::Start<CControlSenpComposition,
		&CControlSenpComposition::Run>(this);
}

CControlSenpComposition::~CControlSenpComposition()
{
	Close();
}

bool CControlSenpComposition::RequestRefresh(std::wstring_view profileId)
{
	return m_refresh && m_refresh->Request(profileId);
}

bool CControlSenpComposition::WaitForIdle(std::uint32_t timeoutMilliseconds)
{
	return m_refresh && m_refresh->WaitForIdle(timeoutMilliseconds);
}

std::optional<std::uint64_t> CControlSenpComposition::PublishedRevision(std::wstring_view profileId) const
{
	return m_authority ? m_authority->Revision(profileId) : std::nullopt;
}

std::optional<senp::github::GhSelectedRepository> CControlSenpComposition::PublishedRepository(
	std::wstring_view profileId) const
{
	return m_profiles ? m_profiles->Repository(profileId) : std::nullopt;
}

senp::github::GhConnectionState CControlSenpComposition::ConnectionState(std::wstring_view profileId) const
{
	const auto connection = m_profiles ? m_profiles->Connection(profileId) : nullptr;
	return connection ? connection->Snapshot().State() : senp::github::GhConnectionState::Unknown;
}

void CControlSenpComposition::Close() noexcept
try {
	{
		std::lock_guard lock(m_workerMutex);
		if (m_closed) return;
		m_closed = true;
	}
	if (m_refresh) m_refresh->Close();
	m_worker.Join();
	// The grant registry closes first: a connection that is torn down while a
	// grant still names it must not be reachable through that grant afterwards.
	if (m_grants) m_grants->Close();
	if (m_authority) m_authority->Close();
	if (m_profiles) m_profiles->Close();
	if (m_packages) m_packages->Close();
} catch (const std::exception&) {
}

void CControlSenpComposition::Run() noexcept
{
	for (;;) {
		auto profileId = m_refresh->WaitAndTake();
		if (!profileId) return;
		try {
			Refresh(*profileId);
		} catch (const std::exception&) {
			// A refresh that failed leaves the profile without a published table,
			// which refuses grants. It never leaves a stale table admitting them.
		}
		m_refresh->Complete(*profileId);
	}
}

void CControlSenpComposition::Refresh(const std::wstring& profileId)
{
	if (!profiles::IsOpaqueUserDataProfileId(profileId)) {
		Discard(profileId);
		return;
	}
	const auto registry = m_profileRegistry ? m_profileRegistry->Snapshot()
		: profiles::ControlUserDataProfileRegistryResult{};
	if (!registry.snapshot) {
		Discard(profileId);
		return;
	}
	const auto profileHome = ResolveProfileHome(profileId, *registry.snapshot);
	if (!profileHome) {
		// The profile no longer resolves against control-owned state at all, so
		// its connection is withdrawn together with its permissions.
		Discard(profileId);
		return;
	}
	const auto packages = m_packages ? m_packages->Refresh(*profileHome) : std::nullopt;
	if (!packages) {
		// A package state that could not be read proves no enablement. The table
		// goes, but the adopted account is not an authorization and stays.
		m_authority->Withdraw(profileId);
		m_grants->RevokeProfile(profileId);
		return;
	}
	const auto published = m_authority->Publish(profileId, *packages);
	if (!published.Succeeded()) {
		// Stale keeps the newer table already published, and the grants that
		// match it. Every other refusal has withdrawn the table.
		if (published.Status() != senp::ESenpPackageAuthorityPublishStatus::Stale) {
			m_grants->RevokeProfile(profileId);
		}
		return;
	}
	RefreshRepository(profileId);
	RefreshConnection(profileId);
}

std::optional<std::wstring> CControlSenpComposition::ResolveProfileHome(const std::wstring& profileId,
	const profiles::UserDataProfileRegistrySnapshot& registry) const
{
	profiles::UserDataProfileBootstrapRequest request{
		.controlAuthority = { m_options.ControlAuthorityId(), m_options.ControlAuthorityGeneration() },
		.controlProfileRoot = m_options.ControlProfileRoot(),
		.resourceRootMode = profiles::UserDataProfileResourceRootMode::ProfileIdNamespace,
	};
	request.selection.explicitProfileId = profileId;
	auto resolved = profiles::ResolveUserDataProfileBootstrap(request, registry);
	if (resolved.Resolved()
		&& resolved.snapshot->SelectedProfile().kind == profiles::UserDataProfileKind::Default) {
		// The editor derives the default profile's resources through the same
		// legacy bridge. Resolving it differently here would read the packages of
		// a directory no editor ever loads extensions from.
		request.resourceRootMode = profiles::UserDataProfileResourceRootMode::LegacyControlRootForDefault;
		resolved = profiles::ResolveUserDataProfileBootstrap(request, registry);
	}
	if (!resolved.Resolved() || resolved.snapshot->SelectedProfileId() != profileId) return std::nullopt;
	const auto profileHome = resolved.snapshot->Resources().ProfileHome().ToWindowsPath();
	if (!profileHome.value || !::platform::IsAbsoluteWindowsPath(*profileHome.value)) return std::nullopt;
	return *profileHome.value;
}

void CControlSenpComposition::RefreshRepository(const std::wstring& profileId)
{
	// The workspace a profile answers for is declared by the connections open on
	// it, and never carried by a read request. It is not stored on the profile
	// either: the registry has no per-window granularity, so one association set
	// could only mix the folders of every window that shares this profile.
	const auto declared = m_profiles->DeclaredWorkspace(profileId);
	if (!declared || declared->Folders().empty()) {
		m_profiles->PublishRepository(profileId, std::nullopt);
		return;
	}
	config::WorkspaceContextSnapshot workspace;
	workspace.folders.reserve(declared->Folders().size());
	for (const auto& folder : declared->Folders()) {
		// The wire carries URI text, so an unparseable folder is a declaration
		// the control side cannot act on. It takes the whole workspace with it:
		// resolving the folders that did parse would answer for a workspace no
		// window is actually open on.
		auto uri = ::platform::uri::Uri::Parse(folder);
		if (!uri) {
			m_profiles->PublishRepository(profileId, std::nullopt);
			return;
		}
		// The declaration named the folder and nothing else. Everything below
		// this line is read from the folder itself, so the editor's claim is
		// verified against the real remotes rather than trusted.
		workspace.folders.push_back(config::WorkspaceFolderDescriptor{ std::move(*uri.value), {} });
	}
	workspace.generation = declared->Generation();
	workspace.revision = declared->Revision();
	workspace.kind = workspace.folders.size() == 1 ? config::EWorkspaceKind::Folder
		: config::EWorkspaceKind::Workspace;
	const auto captured = m_repositories.Capture(workspace, nullptr);
	const auto selection = m_repositories.Select(captured, workspace.generation, workspace.revision);
	// Anything other than one unambiguous GitHub remote publishes nothing, so
	// the executor answers Unavailable instead of guessing an owner or repository.
	m_profiles->PublishRepository(profileId,
		selection.Status() == senp::github::GhRepositorySelectionStatus::Selected
			? selection.Selected() : std::nullopt);
}

void CControlSenpComposition::RefreshConnection(const std::wstring& profileId)
{
	auto connection = m_profiles->Adopt(profileId);
	if (!connection) return;
	if (connection->Snapshot().State() == senp::github::GhConnectionState::Connected) return;
	auto attempt = connection->Begin(m_options.Hostname(), std::nullopt);
	if (!attempt) return;
	// Probing runs `gh --version`; it belongs on this worker and never on a frame.
	if (!m_probe || m_probe->Status() != senp::github::GhToolAvailability::Available) {
		m_probe = m_policy.Probe(nullptr);
	}
	// Complete blocks on the GitHub CLI. Its terminal is published into the
	// lifecycle, which is the only place a read may learn the account from.
	(void)connection->Complete(std::move(*attempt), *m_probe, nullptr);
}

void CControlSenpComposition::Discard(const std::wstring& profileId) noexcept
try {
	m_authority->Withdraw(profileId);
	m_grants->RevokeProfile(profileId);
	m_profiles->Withdraw(profileId);
} catch (const std::exception&) {
}

} // namespace platform::controlipc
