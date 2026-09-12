/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlSenpComposition.h"

#include "platform/storage/CInMemoryStorageService.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace platform::controlipc {
namespace {
namespace gh = senp::github;

constexpr char kAuthorityId[] = "0123456789abcdef0123456789abcdef";
constexpr wchar_t kProfile[] = L"github-profile";
constexpr wchar_t kControlRoot[] = L"C:\\Control";
constexpr wchar_t kProfileHome[] = L"C:\\Control\\user-data-profiles\\github-profile";
constexpr wchar_t kWorkspaceRoot[] = L"C:\\Work\\Fork";
constexpr wchar_t kOtherWorkspaceRoot[] = L"C:\\Work\\Notes";
constexpr wchar_t kGhConfiguration[] = L"C:\\Control\\gh";
constexpr wchar_t kExtension[] = L"sakura.github-pull-requests";
constexpr std::uint64_t kManagementRevision = 9;
constexpr std::uint32_t kIdleTimeoutMilliseconds = 5000;
constexpr ControlIpcSessionContext kConnection{ 31, 1701 };

const std::wstring& Digest()
{
	static const std::wstring value(64, L'a');
	return value;
}

std::vector<std::uint8_t> Bytes(std::string_view value)
{
	return { value.begin(), value.end() };
}

senp::ManagementSnapshot Packages(std::uint64_t revision = kManagementRevision,
	senp::EManagementState state = senp::EManagementState::Ready)
{
	senp::ExtensionDescriptor extension;
	extension.id = kExtension;
	extension.archiveSha256 = Digest();
	extension.installed = true;
	extension.enabled = true;
	extension.runtime.schemaVersion = 2;
	extension.runtime.abi = L"sakura:senp/extension@3.0.0";
	extension.runtime.capabilities = { L"workbench.views.tree", L"tools.github.repository.read" };
	senp::ManagementSnapshot snapshot;
	snapshot.state = state;
	snapshot.revision = revision;
	snapshot.extensions = { std::move(extension) };
	return snapshot;
}

//! Records which profile home the worker asked for. It never blocks, so a test
//! observes admission rather than the package tool's wall clock.
class FakePackages final : public IControlSenpPackageSource {
public:
	std::optional<senp::ManagementSnapshot> Refresh(const std::wstring& profileHome) override
	{
		std::unique_lock lock(*m_mutex);
		m_released.wait(lock, [this]() { return !m_held; });
		m_homes.push_back(profileHome);
		if (m_closed) return std::nullopt;
		return m_snapshot;
	}
	//! Keeps the refresh worker inside Refresh until Release. A test that means
	//! "nothing is published yet" then observes exactly that, instead of racing
	//! the worker that the same declaration just admitted.
	void Hold()
	{
		std::lock_guard lock(*m_mutex);
		m_held = true;
	}
	void Release()
	{
		{
			std::lock_guard lock(*m_mutex);
			m_held = false;
		}
		m_released.notify_all();
	}
	void Close() noexcept override
	{
		std::lock_guard lock(*m_mutex);
		m_closed = true;
	}
	void Set(std::optional<senp::ManagementSnapshot> value)
	{
		std::lock_guard lock(*m_mutex);
		m_snapshot = std::move(value);
	}
	[[nodiscard]] std::vector<std::wstring> Homes() const
	{
		std::lock_guard lock(*m_mutex);
		return m_homes;
	}
	[[nodiscard]] bool Closed() const
	{
		std::lock_guard lock(*m_mutex);
		return m_closed;
	}
private:
	// Owned separately rather than a class-level mutable mutex, so the const
	// accessors above do not need to defeat their own constness to serialize
	// with Refresh()/Close()/Set().
	std::unique_ptr<std::mutex> m_mutex = std::make_unique<std::mutex>();
	std::optional<senp::ManagementSnapshot> m_snapshot{ Packages() };
	std::condition_variable m_released;
	std::vector<std::wstring> m_homes;
	bool m_closed = false;
	bool m_held = false;
};

class FakeManagementService final : public senp::ISenpManagementService {
public:
	explicit FakeManagementService(std::shared_ptr<std::vector<std::wstring>> log, std::wstring profileHome) :
		m_log(std::move(log)), m_profileHome(std::move(profileHome))
	{
	}
	senp::ManagementOperationResult Start() override
	{
		m_log->push_back(L"start:" + m_profileHome);
		return { senp::EManagementOperationStatus::Succeeded, Packages() };
	}
	senp::ManagementOperationResult InstallDeveloperPackage(std::wstring_view, bool) override { return {}; }
	senp::ManagementOperationResult InstallBuiltInPackage(std::wstring_view) override { return {}; }
	senp::ManagementOperationResult UninstallBuiltInPackage(std::wstring_view) override { return {}; }
	senp::ManagementOperationResult Refresh() override
	{
		m_log->push_back(L"refresh:" + m_profileHome);
		return { senp::EManagementOperationStatus::Succeeded, Packages(kManagementRevision + 1) };
	}
	void Stop() noexcept override
	try {
		m_log->push_back(L"stop:" + m_profileHome);
	} catch (const std::exception&) {
	}
	senp::ManagementSnapshot Snapshot() const override { return Packages(); }
private:
	std::shared_ptr<std::vector<std::wstring>> m_log;
	std::wstring m_profileHome;
};

class FakeToolPlatform final : public gh::IGhToolPlatform {
public:
	std::optional<std::wstring> ResolveExecutable() const override { return std::wstring(L"C:\\Tools\\gh.exe"); }
	gh::GhProcessOutcome Run(const gh::GhProcessInvocation&, HANDLE) const override
	{
		++(*m_runs);
		return { ::platform::process::EBoundedProcessStatus::Succeeded, 0,
			Bytes("gh version 2.93.0 (2026-05-27)\n"), {} };
	}
	[[nodiscard]] std::size_t Runs() const noexcept { return m_runs->load(); }
private:
	// Owned separately rather than a mutable counter: Run() is const to match
	// the interface, and the pointee is mutable through the non-mutable pointer.
	std::unique_ptr<std::atomic<std::size_t>> m_runs = std::make_unique<std::atomic<std::size_t>>(0);
};

class FakeCredential final : public gh::IGhAccountCredential {
public:
	gh::GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>&, std::uint32_t,
		std::size_t, std::size_t, HANDLE) override
	{
		return { ::platform::process::EBoundedProcessStatus::Succeeded, 0, Bytes("{}"), {} };
	}
	gh::GhProcessOutcome RunAuthenticatedStreaming(const std::vector<std::wstring>& arguments,
		std::uint32_t timeout, std::size_t output, std::size_t error,
		std::shared_ptr<::platform::process::IBoundedProcessOutputObserver>, HANDLE stop) override
	{
		return RunAuthenticated(arguments, timeout, output, error, stop);
	}
	void Revoke() noexcept override { m_revoked = true; }
	[[nodiscard]] bool Revoked() const noexcept { return m_revoked; }
private:
	std::atomic<bool> m_revoked{ false };
};

class FakeConnectionPlatform final : public gh::IGhConnectionPlatform {
public:
	gh::GhConnectionCheckResult Check(const gh::GhToolProbe& probe, std::wstring_view configurationDirectory,
		std::wstring_view hostname, std::optional<std::wstring_view>, HANDLE) override
	{
		std::lock_guard lock(*m_mutex);
		m_directories.emplace_back(configurationDirectory);
		m_hosts.emplace_back(hostname);
		m_probes.push_back(probe.Status());
		if (m_terminal != gh::GhConnectionTerminal::Succeeded) return { m_terminal, std::nullopt, {} };
		return { m_terminal,
			gh::GhAccountIdentity(std::wstring(hostname), L"octocat", L"keyring",
				std::wstring(configurationDirectory)),
			std::make_shared<FakeCredential>() };
	}
	void Set(gh::GhConnectionTerminal terminal)
	{
		std::lock_guard lock(*m_mutex);
		m_terminal = terminal;
	}
	[[nodiscard]] std::vector<std::wstring> Directories() const
	{
		std::lock_guard lock(*m_mutex);
		return m_directories;
	}
	[[nodiscard]] std::vector<std::wstring> Hosts() const
	{
		std::lock_guard lock(*m_mutex);
		return m_hosts;
	}
	[[nodiscard]] std::vector<gh::GhToolAvailability> Probes() const
	{
		std::lock_guard lock(*m_mutex);
		return m_probes;
	}
private:
	// Owned separately: Directories()/Hosts()/Probes() are const and must not
	// defeat their own constness with a class-level mutable mutex.
	std::unique_ptr<std::mutex> m_mutex = std::make_unique<std::mutex>();
	gh::GhConnectionTerminal m_terminal{ gh::GhConnectionTerminal::Succeeded };
	std::vector<std::wstring> m_directories, m_hosts;
	std::vector<gh::GhToolAvailability> m_probes;
};

class FakeRepositoryPlatform final : public gh::IGhLocalRepositoryPlatform {
public:
	FakeRepositoryPlatform()
	{
		m_entries.emplace(kWorkspaceRoot, std::tuple{ kWorkspaceRoot, std::wstring(L"c:\\work\\fork"),
			std::string("origin\thttps://github.com/me/project.git (fetch)\n"
				"origin\thttps://github.com/me/project.git (push)\n") });
	}
	gh::GhLocalRepositoryRead Inspect(std::wstring_view root, HANDLE) const override
	{
		m_roots->Record(root);
		const auto found = m_entries.find(std::wstring(root));
		if (found == m_entries.end()) return { gh::GhLocalRepositoryStatus::NotRepository, {}, {}, {} };
		return { gh::GhLocalRepositoryStatus::Succeeded, std::get<0>(found->second),
			std::get<1>(found->second), Bytes(std::get<2>(found->second)) };
	}
	[[nodiscard]] std::vector<std::wstring> Roots() const
	{
		return m_roots->Snapshot();
	}
private:
	//! Owned separately from the object rather than as mutable members: Inspect()
	//! and Roots() are const, and this is the only state either one mutates. A
	//! unique_ptr member keeps the pointee non-const in those const methods, so
	//! this helper's own lock/push methods need no `mutable` of their own.
	class RootLog {
	public:
		void Record(std::wstring_view root)
		{
			std::lock_guard lock(m_mutex);
			m_seen.emplace_back(root);
		}
		[[nodiscard]] std::vector<std::wstring> Snapshot()
		{
			std::lock_guard lock(m_mutex);
			return m_seen;
		}
	private:
		std::mutex m_mutex;
		std::vector<std::wstring> m_seen;
	};
	std::unique_ptr<RootLog> m_roots = std::make_unique<RootLog>();
	std::map<std::wstring, std::tuple<std::wstring, std::wstring, std::string>> m_entries;
};

std::shared_ptr<profiles::ControlUserDataProfileRegistry> Registry()
{
	auto storage = std::make_shared<::platform::storage::CInMemoryStorageService>();
	auto registry = std::make_shared<profiles::ControlUserDataProfileRegistry>(storage);
	EXPECT_TRUE(registry->Start().Succeeded());
	EXPECT_TRUE(registry->CreateNamed(
		{ kProfile, L"GitHub", profiles::UserDataProfileKind::Normal, {}, {} },
		{ "create-github-profile", std::nullopt }).Succeeded());
	// Kept even though no repository is derived from it any more: it is what
	// proves the association stopped deciding which repository a profile
	// answers for, rather than that the test simply stopped writing one.
	auto workspace = ::platform::uri::Uri::FromWindowsPath(kWorkspaceRoot);
	EXPECT_TRUE(workspace);
	EXPECT_TRUE(registry->AssociateWorkspace(kProfile, std::move(*workspace.value),
		{ "associate-workspace", std::nullopt }).Succeeded());
	return registry;
}

ControlSenpCompositionOptions Options()
{
	ControlSenpCompositionOptions options;
	options.SetControlProfileRoot(kControlRoot);
	options.SetControlAuthorityId(kAuthorityId);
	options.SetControlAuthorityGeneration(7);
	options.SetGhConfigurationDirectory(kGhConfiguration);
	options.SetWorkingDirectory(kControlRoot);
	return options;
}

//! Every seam is a fake, so no test starts `gh`, git or the package tool.
//! Private state with accessors for the same reason as every other DTO in
//! this subsystem: fields are constructed once and never reassigned by a test.
class Fakes {
public:
	[[nodiscard]] const std::shared_ptr<FakePackages>& Packages() const noexcept { return m_packages; }
	[[nodiscard]] const std::shared_ptr<FakeToolPlatform>& Tool() const noexcept { return m_tool; }
	[[nodiscard]] const std::shared_ptr<FakeConnectionPlatform>& Connection() const noexcept { return m_connection; }
	[[nodiscard]] const std::shared_ptr<FakeRepositoryPlatform>& Repositories() const noexcept
	{
		return m_repositories;
	}

	[[nodiscard]] ControlSenpCompositionDependencies Dependencies() const
	{
		ControlSenpCompositionDependencies dependencies;
		dependencies.SetPackages(m_packages);
		dependencies.SetToolPlatform(m_tool);
		dependencies.SetConnectionPlatform(m_connection);
		dependencies.SetRepositoryPlatform(m_repositories);
		return dependencies;
	}
private:
	std::shared_ptr<FakePackages> m_packages = std::make_shared<FakePackages>();
	std::shared_ptr<FakeToolPlatform> m_tool = std::make_shared<FakeToolPlatform>();
	std::shared_ptr<FakeConnectionPlatform> m_connection = std::make_shared<FakeConnectionPlatform>();
	std::shared_ptr<FakeRepositoryPlatform> m_repositories = std::make_shared<FakeRepositoryPlatform>();
};

ControlSenpRpcRequest IssueGrant(std::wstring profileId = kProfile)
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::IssueGrant);
	request.SetProfileId(std::move(profileId));
	request.SetOwner({ kExtension, Digest(), 7, 11, 13 });
	request.SetCapabilities(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead));
	return request;
}

//! The folder identity a window would declare for a root it is open on.
std::wstring WorkspaceFolder(const wchar_t* root = kWorkspaceRoot)
{
	auto uri = ::platform::uri::Uri::FromWindowsPath(root);
	EXPECT_TRUE(uri);
	return uri ? uri.value->ToString() : std::wstring();
}

ControlSenpRpcRequest AdoptWorkspace(std::vector<std::wstring> folders, std::int64_t revision = 11,
	std::wstring profileId = kProfile)
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::AdoptWorkspace);
	request.SetProfileId(std::move(profileId));
	ControlSenpRpcWorkspace workspace;
	workspace.SetGeneration(5);
	workspace.SetRevision(revision);
	workspace.SetFolders(std::move(folders));
	request.SetWorkspace(std::move(workspace));
	return request;
}

ControlIpcFrame RequestFrame(const ControlSenpRpcRequest& request, std::uint64_t requestId = 1)
{
	auto payload = EncodeControlSenpRpcRequest(request);
	EXPECT_TRUE(payload.has_value());
	auto fields = EncodeControlIpcFields(
		{ { static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload),
			payload.value_or(std::vector<std::uint8_t>{}) } });
	EXPECT_TRUE(fields.has_value());
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::SenpRequest,
		EControlIpcFlags::Request, requestId, 42 },
		fields.value_or(std::vector<std::uint8_t>{}) };
}

std::optional<ControlSenpRpcResponse> ReadResponse(const ControlIpcFrameDispatchResult& result)
{
	if (result.responseFrames.size() != 1) return std::nullopt;
	const auto& frame = result.responseFrames.front();
	if (frame.header.kind != EControlIpcKind::SenpResponse) return std::nullopt;
	const auto fields = DecodeControlIpcFields(frame.payload);
	if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || fields.fields.size() != 1) {
		return std::nullopt;
	}
	return DecodeControlSenpRpcResponse(fields.fields.front().value);
}

EControlSenpRpcStatus Issue(IControlIpcSessionHandler& session, std::uint64_t requestId,
	std::wstring profileId = kProfile)
{
	const auto reply = ReadResponse(
		session.HandleFrame(kConnection, RequestFrame(IssueGrant(std::move(profileId)), requestId)));
	return reply ? reply->Status() : EControlSenpRpcStatus::InvalidRequest;
}

EControlSenpRpcStatus Adopt(IControlIpcSessionHandler& session, std::uint64_t requestId,
	std::vector<std::wstring> folders, const ControlIpcSessionContext& connection = kConnection)
{
	const auto reply = ReadResponse(session.HandleFrame(connection,
		RequestFrame(AdoptWorkspace(std::move(folders)), requestId)));
	return reply ? reply->Status() : EControlSenpRpcStatus::InvalidRequest;
}

SenpWorkspaceAdoption Adoption(SenpConnectionIdentity connection, std::vector<std::wstring> folders,
	std::int64_t revision = 11, std::wstring profileId = kProfile)
{
	ControlSenpRpcWorkspace workspace;
	workspace.SetGeneration(5);
	workspace.SetRevision(revision);
	workspace.SetFolders(std::move(folders));
	return SenpWorkspaceAdoption(connection, std::move(profileId), std::move(workspace));
}

} // namespace

TEST(ControlSenpRefreshQueue, DeduplicatesAndStartsThePerProfileIntervalWhenTheAttemptEnds)
{
	CControlSenpRefreshQueue queue(std::chrono::milliseconds{ 60000 }, 4);
	EXPECT_FALSE(queue.Request(L""));
	EXPECT_TRUE(queue.Request(kProfile));
	// One pending entry is enough: a five-hundred-millisecond poll must not be
	// able to stack requests for the same profile.
	EXPECT_FALSE(queue.Request(kProfile));
	EXPECT_FALSE(queue.WaitForIdle(0));

	const auto taken = queue.WaitAndTake();
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(kProfile, *taken);
	// The attempt is running, so re-entering it is refused even though nothing
	// is pending any more.
	EXPECT_FALSE(queue.Request(kProfile));
	EXPECT_FALSE(queue.WaitForIdle(0));
	EXPECT_TRUE(queue.Request(L"other-profile"));

	queue.Complete(*taken);
	// The interval is measured from the end of the attempt, so the profile that
	// just finished is refused while a different one is still admitted.
	EXPECT_FALSE(queue.Request(kProfile));
	queue.Close();
}

TEST(ControlSenpRefreshQueue, ReAdmitsAChangeThatArrivedWhileThatProfileWasRefreshing)
{
	CControlSenpRefreshQueue queue(std::chrono::milliseconds{ 60000 }, 4);
	EXPECT_TRUE(queue.Request(kProfile));
	const auto taken = queue.WaitAndTake();
	ASSERT_TRUE(taken.has_value());
	// A poll landing on the running attempt is the duplicate the queue drops.
	EXPECT_FALSE(queue.Request(kProfile));
	// A declared workspace is not a poll. The attempt in flight read the
	// workspace before this existed, so losing it would leave the profile
	// answering for no repository until an unrelated request arrived.
	EXPECT_FALSE(queue.RequestChanged(kProfile));
	queue.Complete(*taken);
	const auto again = queue.WaitAndTake();
	ASSERT_TRUE(again.has_value());
	EXPECT_EQ(kProfile, *again);
	// Exactly one re-admission: the flag is spent, not sticky.
	queue.Complete(*again);
	EXPECT_TRUE(queue.WaitForIdle(0));
	queue.Close();
}

TEST(ControlSenpRefreshQueue, BoundsPendingWorkAndCloseReleasesEveryWaiter)
{
	CControlSenpRefreshQueue queue(std::chrono::milliseconds{ 0 }, 2);
	EXPECT_TRUE(queue.Request(L"first"));
	EXPECT_TRUE(queue.Request(L"second"));
	EXPECT_FALSE(queue.Request(L"third"));

	ASSERT_TRUE(queue.WaitAndTake().has_value());
	queue.Complete(L"first");
	// A zero interval still admits the same profile again once its attempt ended.
	EXPECT_TRUE(queue.Request(L"first"));

	std::atomic<bool> released{ false };
	std::thread waiter([&queue, &released]() {
		while (queue.WaitAndTake().has_value()) queue.Complete(L"drained");
		released = true;
	});
	queue.Close();
	waiter.join();
	EXPECT_TRUE(released.load());
	EXPECT_FALSE(queue.Request(L"first"));
	EXPECT_FALSE(queue.WaitAndTake().has_value());
	queue.Close();
}

TEST(ControlSenpAuthorityGate, AdmitsOneRefreshOnlyOnAMissAndStillRefusesTheGrant)
{
	auto authority = std::make_shared<senp::CSenpControlPackageAuthority>();
	ASSERT_TRUE(authority->Publish(kProfile, Packages()).Succeeded());
	auto refresh = std::make_shared<CControlSenpRefreshQueue>(std::chrono::milliseconds{ 60000 }, 4);
	const CControlSenpAuthorityGate gate(authority, refresh);

	// A hit is a bounded lookup: it must not schedule package-tool work at all.
	const auto resolved = gate.Resolve(kProfile, kExtension);
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(senp::SenpToolCapability::GitHubRepositoryRead, resolved->Capabilities());
	EXPECT_TRUE(refresh->WaitForIdle(0));

	// A miss refuses and admits exactly one refresh, however often it is retried.
	EXPECT_FALSE(gate.Resolve(L"absent-profile", kExtension).has_value());
	EXPECT_FALSE(gate.Resolve(L"absent-profile", kExtension).has_value());
	EXPECT_FALSE(refresh->WaitForIdle(0));
	const auto taken = refresh->WaitAndTake();
	ASSERT_TRUE(taken.has_value());
	EXPECT_EQ(L"absent-profile", *taken);
	refresh->Complete(*taken);
	EXPECT_TRUE(refresh->WaitForIdle(0));
	refresh->Close();
}

TEST(ControlSenpPackageSource, StartsOneServicePerProfileHomeAndRefusesUnusablePaths)
{
	auto log = std::make_shared<std::vector<std::wstring>>();
	CControlSenpPackageSource source([log](const std::wstring& profileHome) {
		return std::make_shared<FakeManagementService>(log, profileHome);
	});

	const auto started = source.Refresh(kProfileHome);
	ASSERT_TRUE(started.has_value());
	EXPECT_EQ(kManagementRevision, started->revision);
	// Restarting would reload the built-in catalog and install default packages
	// again, which a permission refresh may not cost.
	const auto refreshed = source.Refresh(kProfileHome);
	ASSERT_TRUE(refreshed.has_value());
	EXPECT_EQ(kManagementRevision + 1, refreshed->revision);
	ASSERT_EQ(2U, log->size());
	EXPECT_EQ(std::wstring(L"start:") + kProfileHome, (*log)[0]);
	EXPECT_EQ(std::wstring(L"refresh:") + kProfileHome, (*log)[1]);

	EXPECT_FALSE(source.Refresh(L"user-data-profiles\\relative").has_value());
	EXPECT_FALSE(source.Refresh(L"").has_value());
	EXPECT_EQ(2U, log->size());

	for (std::size_t index = 1; index < CControlSenpPackageSource::MaximumProfileHomes(); ++index) {
		EXPECT_TRUE(source.Refresh(L"C:\\Control\\home" + std::to_wstring(index)).has_value());
	}
	EXPECT_FALSE(source.Refresh(L"C:\\Control\\overflow").has_value());
	EXPECT_EQ(CControlSenpPackageSource::MaximumProfileHomes() + 1, log->size());

	source.Close();
	EXPECT_EQ(CControlSenpPackageSource::MaximumProfileHomes() * 2 + 1, log->size());
	EXPECT_FALSE(source.Refresh(kProfileHome).has_value());
	source.Close();
}

TEST(ControlSenpProfileSource, AnswersNothingUntilTheWorkerPublishesAndWithdrawClosesTheConnection)
{
	auto authority = std::make_shared<senp::CSenpControlPackageAuthority>();
	auto grants = std::make_shared<senp::CSenpToolGrants>(authority);
	auto platform = std::make_shared<FakeConnectionPlatform>();
	CControlSenpProfileSource source(grants, platform, kGhConfiguration);

	// Before adoption the executor must learn NotConnected and Unavailable rather
	// than a guessed host, owner or repository.
	EXPECT_EQ(nullptr, source.Connection(kProfile));
	EXPECT_FALSE(source.Repository(kProfile).has_value());

	auto connection = source.Adopt(kProfile);
	ASSERT_NE(nullptr, connection);
	EXPECT_EQ(connection, source.Adopt(kProfile));
	EXPECT_EQ(connection, source.Connection(kProfile));
	EXPECT_FALSE(source.Repository(kProfile).has_value());

	source.PublishRepository(kProfile, gh::GhSelectedRepository(L"c:\\work\\fork", L"origin",
		L"github.com", L"me", L"project"));
	const auto published = source.Repository(kProfile);
	ASSERT_TRUE(published.has_value());
	EXPECT_EQ(L"me", published->Owner());
	EXPECT_EQ(L"project", published->Repository());
	source.PublishRepository(kProfile, std::nullopt);
	EXPECT_FALSE(source.Repository(kProfile).has_value());
	// An empty publish for an unknown profile records nothing at all.
	source.PublishRepository(L"other-profile", std::nullopt);
	EXPECT_EQ(nullptr, source.Connection(L"other-profile"));

	for (std::size_t index = 1; index < CControlSenpProfileSource::MaximumProfiles(); ++index) {
		EXPECT_NE(nullptr, source.Adopt(L"profile" + std::to_wstring(index)));
	}
	EXPECT_EQ(nullptr, source.Adopt(L"profile-overflow"));

	source.Withdraw(kProfile);
	EXPECT_EQ(nullptr, source.Connection(kProfile));
	EXPECT_EQ(gh::GhConnectionState::Unavailable, connection->Snapshot().State());
	EXPECT_FALSE(connection->Begin(L"github.com", std::nullopt).has_value());

	source.Close();
	EXPECT_EQ(nullptr, source.Adopt(L"profile1"));
	EXPECT_EQ(nullptr, source.Connection(L"profile1"));
	source.Close();
}

TEST(ControlSenpProfileSource, AgreesOnADeclaredWorkspaceOnlyWhileEveryConnectionDeclaresTheSameOne)
{
	auto authority = std::make_shared<senp::CSenpControlPackageAuthority>();
	auto grants = std::make_shared<senp::CSenpToolGrants>(authority);
	auto refresh = std::make_shared<CControlSenpRefreshQueue>(std::chrono::milliseconds{ 60000 }, 8);
	CControlSenpProfileSource source(grants, std::make_shared<FakeConnectionPlatform>(),
		kGhConfiguration, refresh);
	EXPECT_FALSE(source.DeclaredWorkspace(kProfile).has_value());

	const SenpConnectionIdentity first{ 31, 1701 };
	const SenpConnectionIdentity second{ 32, 1702 };
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		source.AdoptWorkspace(Adoption(first, { L"file:///c:/work/fork" })));
	// A declaration is edge-triggered, so it admits work without waiting out the
	// interval that protects the worker from polling.
	EXPECT_TRUE(refresh->WaitAndTake().has_value());
	refresh->Complete(kProfile);
	{
		const auto declared = source.DeclaredWorkspace(kProfile);
		ASSERT_TRUE(declared.has_value());
		EXPECT_EQ(11, declared->Revision());
	}

	// Re-declaring what this connection already declared is not a change, so it
	// admits nothing: a reconnecting editor must not be able to drive the worker
	// by repeating itself.
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		source.AdoptWorkspace(Adoption(first, { L"file:///c:/work/fork" })));
	EXPECT_TRUE(refresh->WaitForIdle(0));

	// The same workspace seen later by another window: the newer observation is
	// what a staleness check has to be made against.
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		source.AdoptWorkspace(Adoption(second, { L"file:///c:/work/fork" }, 12)));
	{
		const auto declared = source.DeclaredWorkspace(kProfile);
		ASSERT_TRUE(declared.has_value());
		EXPECT_EQ(12, declared->Revision());
	}

	// Two windows of one profile on different workspaces cannot both be answered
	// for by a profile-scoped repository, so nothing is agreed rather than one
	// of them being picked.
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		source.AdoptWorkspace(Adoption(second, { L"file:///c:/work/notes" }, 13)));
	EXPECT_FALSE(source.DeclaredWorkspace(kProfile).has_value());

	// Withdrawing the disagreeing declaration leaves one, and it agrees again.
	source.WithdrawWorkspace(second);
	EXPECT_TRUE(source.DeclaredWorkspace(kProfile).has_value());
	// Withdrawing a connection that declared nothing is a no-op: the session
	// destructor calls it for every connection, including those.
	source.WithdrawWorkspace({ 33, 1703 });
	EXPECT_TRUE(source.DeclaredWorkspace(kProfile).has_value());

	for (std::size_t index = 1; index < CControlSenpProfileSource::MaximumAdoptions(); ++index) {
		EXPECT_EQ(EControlSenpRpcStatus::Succeeded, source.AdoptWorkspace(Adoption(
			{ 100 + index, 2000 }, { L"file:///c:/work/fork" }, 11, L"profile" + std::to_wstring(index))));
	}
	// The control side inspects every declared folder, so declarations are
	// bounded the way every other admitted resource here is.
	EXPECT_EQ(EControlSenpRpcStatus::ResourceExhausted,
		source.AdoptWorkspace(Adoption({ 999, 2000 }, { L"file:///c:/work/fork" })));

	source.Close();
	EXPECT_FALSE(source.DeclaredWorkspace(kProfile).has_value());
	EXPECT_EQ(EControlSenpRpcStatus::Closed,
		source.AdoptWorkspace(Adoption(first, { L"file:///c:/work/fork" })));
	refresh->Close();
}

TEST(ControlSenpProfileSource, RefusesToAdoptAnAccountWithoutAnAbsoluteConfigurationDirectory)
{
	auto authority = std::make_shared<senp::CSenpControlPackageAuthority>();
	auto grants = std::make_shared<senp::CSenpToolGrants>(authority);
	CControlSenpProfileSource source(grants, std::make_shared<FakeConnectionPlatform>(), L"gh");
	EXPECT_EQ(nullptr, source.Adopt(kProfile));
	EXPECT_EQ(nullptr, source.Connection(kProfile));
}

TEST(ControlSenpComposition, PublishesControlOwnedPackagesAndThenAdmitsAGrantThroughItsHandler)
{
	Fakes fakes;
	// The declaration below admits a refresh, and the worker would publish from
	// it. Holding that attempt is what makes "nothing is published yet" a fact
	// of this test rather than a bet on the worker losing a race.
	fakes.Packages()->Hold();
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	auto session = composition.Handler()->CreateSession(kConnection);
	ASSERT_NE(nullptr, session);
	// The window says where it is open before it asks for anything. Nothing else
	// tells the control side which folders this profile answers for.
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, Adopt(*session, 1, { WorkspaceFolder() }));

	// Nothing is published yet, so the grant is refused and the miss admits no
	// second refresh: the attempt this declaration admitted is still the one.
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, Issue(*session, 2));
	fakes.Packages()->Release();
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));

	ASSERT_EQ(1U, fakes.Packages()->Homes().size());
	EXPECT_EQ(kProfileHome, fakes.Packages()->Homes().front());
	const auto revision = composition.PublishedRevision(kProfile);
	ASSERT_TRUE(revision.has_value());
	EXPECT_EQ(kManagementRevision, *revision);

	// The declaration named folders and nothing else: what they resolve to was
	// read from the remotes found there, never carried by a request.
	ASSERT_EQ(std::vector<std::wstring>{ kWorkspaceRoot }, fakes.Repositories()->Roots());
	const auto repository = composition.PublishedRepository(kProfile);
	ASSERT_TRUE(repository.has_value());
	EXPECT_EQ(L"github.com", repository->Hostname());
	EXPECT_EQ(L"me", repository->Owner());
	EXPECT_EQ(L"project", repository->Repository());

	ASSERT_EQ(1U, fakes.Connection()->Directories().size());
	EXPECT_EQ(kGhConfiguration, fakes.Connection()->Directories().front());
	EXPECT_EQ(L"github.com", fakes.Connection()->Hosts().front());
	EXPECT_EQ(gh::GhToolAvailability::Available, fakes.Connection()->Probes().front());
	EXPECT_EQ(gh::GhConnectionState::Connected, composition.ConnectionState(kProfile));

	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, Issue(*session, 3));
	// A capability the published table does not carry stays refused.
	auto widened = IssueGrant();
	widened.SetCapabilities(static_cast<std::uint32_t>(senp::SenpToolCapability::OpenConnectionUi));
	const auto refused = ReadResponse(session->HandleFrame(kConnection, RequestFrame(widened, 4)));
	ASSERT_TRUE(refused.has_value());
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, refused->Status());

	session.reset();
	composition.Close();
}

TEST(ControlSenpComposition, ReadsNoPackagesForAProfileControlOwnedStateDoesNotResolve)
{
	Fakes fakes;
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	auto session = composition.Handler()->CreateSession(kConnection);
	ASSERT_NE(nullptr, session);

	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, Issue(*session, 1, L"absent-profile"));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_TRUE(fakes.Packages()->Homes().empty());
	EXPECT_FALSE(composition.PublishedRevision(L"absent-profile").has_value());
	EXPECT_EQ(gh::GhConnectionState::Unknown, composition.ConnectionState(L"absent-profile"));

	// An identifier that is not an opaque profile id never reaches the registry.
	EXPECT_TRUE(composition.RequestRefresh(L"not an opaque id"));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_TRUE(fakes.Packages()->Homes().empty());
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, Issue(*session, 2, L"absent-profile"));

	session.reset();
	composition.Close();
}

TEST(ControlSenpComposition, PublishesNoPermissionWhenThePackageStateProvesNoEnablement)
{
	for (int variant = 0; variant < 2; ++variant) {
		Fakes fakes;
		// A read that failed and a reload that kept previously discovered
		// extensions both prove nothing about what is enabled now.
		fakes.Packages()->Set(variant == 0 ? std::optional<senp::ManagementSnapshot>{}
			: Packages(kManagementRevision, senp::EManagementState::ReadyWithDiagnostics));
		CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
		auto session = composition.Handler()->CreateSession(kConnection);
		ASSERT_NE(nullptr, session);

		EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, Issue(*session, 1));
		ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
		EXPECT_EQ(1U, fakes.Packages()->Homes().size());
		EXPECT_FALSE(composition.PublishedRevision(kProfile).has_value());
		EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, Issue(*session, 2));
		// No enablement proof means no repository and no adopted account either.
		EXPECT_FALSE(composition.PublishedRepository(kProfile).has_value());
		EXPECT_TRUE(fakes.Connection()->Directories().empty());

		session.reset();
		composition.Close();
	}
}

TEST(ControlSenpComposition, PublishesNoRepositoryWhileNoConnectionHasDeclaredAWorkspace)
{
	Fakes fakes;
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	ASSERT_TRUE(composition.RequestRefresh(kProfile));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));

	EXPECT_TRUE(composition.PublishedRevision(kProfile).has_value());
	// The registry still associates a workspace with this profile, and it is
	// deliberately not what answers: one association set has no per-window
	// granularity, so it could only mix the folders of every window sharing it.
	EXPECT_TRUE(fakes.Repositories()->Roots().empty());
	EXPECT_FALSE(composition.PublishedRepository(kProfile).has_value());
	// An account is not an authorization, so it is still adopted on its own.
	EXPECT_EQ(gh::GhConnectionState::Connected, composition.ConnectionState(kProfile));
	composition.Close();
}

TEST(ControlSenpComposition, ResolvesTheDeclaredWorkspaceAndStopsAnsweringWhenTheConnectionEnds)
{
	Fakes fakes;
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	{
		auto session = composition.Handler()->CreateSession(kConnection);
		ASSERT_NE(nullptr, session);
		// The declaration alone admits the work. It is edge-triggered, so making
		// it wait out the poll-protection interval would leave the window looking
		// at a workspace the control side already knows about.
		ASSERT_EQ(EControlSenpRpcStatus::Succeeded, Adopt(*session, 1, { WorkspaceFolder() }));
		ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
		ASSERT_EQ(std::vector<std::wstring>{ kWorkspaceRoot }, fakes.Repositories()->Roots());
		const auto repository = composition.PublishedRepository(kProfile);
		ASSERT_TRUE(repository.has_value());
		EXPECT_EQ(L"github.com", repository->Hostname());
		EXPECT_EQ(L"me", repository->Owner());
		EXPECT_EQ(L"project", repository->Repository());
	}
	// A declaration cannot outlive its connection: a closed window must not keep
	// deciding which repository the profile answers for.
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_FALSE(composition.PublishedRepository(kProfile).has_value());
	composition.Close();
}

TEST(ControlSenpComposition, PublishesNoRepositoryWhileTwoConnectionsDeclareDifferentWorkspaces)
{
	constexpr ControlIpcSessionContext kOtherConnection{ 32, 1702 };
	Fakes fakes;
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	auto first = composition.Handler()->CreateSession(kConnection);
	ASSERT_NE(nullptr, first);
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, Adopt(*first, 1, { WorkspaceFolder() }));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	ASSERT_TRUE(composition.PublishedRepository(kProfile).has_value());

	auto second = composition.Handler()->CreateSession(kOtherConnection);
	ASSERT_NE(nullptr, second);
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		Adopt(*second, 1, { WorkspaceFolder(kOtherWorkspaceRoot) }, kOtherConnection));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	// Ambiguity publishes nothing, exactly as two GitHub remotes in one
	// workspace do: the executor answers Unavailable rather than pick a window.
	EXPECT_FALSE(composition.PublishedRepository(kProfile).has_value());

	second.reset();
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	// One declaration is left, so the profile answers for it again.
	EXPECT_TRUE(composition.PublishedRepository(kProfile).has_value());
	first.reset();
	composition.Close();
}

TEST(ControlSenpComposition, CloseIsIdempotentAndLeavesNoConnectionAbleToReachAGrant)
{
	Fakes fakes;
	CControlSenpComposition composition(Options(), Registry(), fakes.Dependencies());
	ASSERT_TRUE(composition.RequestRefresh(kProfile));
	ASSERT_TRUE(composition.WaitForIdle(kIdleTimeoutMilliseconds));
	ASSERT_TRUE(composition.PublishedRevision(kProfile).has_value());

	composition.Close();
	composition.Close();
	EXPECT_TRUE(fakes.Packages()->Closed());
	EXPECT_FALSE(composition.PublishedRevision(kProfile).has_value());
	EXPECT_EQ(nullptr, composition.Handler()->CreateSession(kConnection));
	EXPECT_FALSE(composition.RequestRefresh(kProfile));
}

} // namespace platform::controlipc
