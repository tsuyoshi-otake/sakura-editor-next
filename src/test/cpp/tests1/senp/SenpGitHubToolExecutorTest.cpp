/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include <memory>
#include <string>

namespace {
using namespace senp;
using namespace senp::github;
using platform::controlipc::ControlSenpRpcResponse;
using platform::controlipc::EControlSenpAccountState;
using platform::controlipc::EControlSenpRpcStatus;
using platform::controlipc::SenpToolExecutionScope;
using platform::controlipc::SenpToolReadCommand;
using platform::process::EBoundedProcessStatus;
using platform::process::EBoundedProcessStream;

constexpr std::uint32_t kIdleTimeoutMilliseconds = 5000;

std::vector<std::uint8_t> Bytes(const std::string_view value)
{
	return { value.begin(), value.end() };
}

std::wstring Digest()
{
	return std::wstring(64, L'a');
}

class Authority final : public ISenpToolGrantAuthority {
public:
	std::optional<SenpApprovedToolOwner> Resolve(std::wstring_view profileId,
		std::wstring_view extensionId) const override
	{
		return SenpApprovedToolOwner(std::wstring(profileId), std::wstring(extensionId),
			Digest(), 1, SenpToolCapability::GitHubRepositoryRead, true);
	}
};

class Credential final : public IGhAccountCredential {
public:
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>& arguments,
		std::uint32_t, std::size_t, std::size_t, HANDLE) override
	{
		m_arguments = arguments;
		return m_outcome;
	}
	//! A streamed run hands its bytes to the observer and keeps none: that is
	//! what makes a log a resource rather than a page carried in an outcome.
	GhProcessOutcome RunAuthenticatedStreaming(const std::vector<std::wstring>& arguments,
		std::uint32_t, std::size_t, std::size_t,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> observer, HANDLE) override
	{
		m_arguments = arguments;
		for (const auto& chunk : m_streamed) {
			if (observer && !observer->OnOutput(EBoundedProcessStream::StandardOutput, chunk)) {
				return { EBoundedProcessStatus::ObserverRejected, -1, {}, {} };
			}
		}
		return { m_streamStatus, m_streamStatus == EBoundedProcessStatus::Succeeded ? 0 : 1, {}, {} };
	}
	void Revoke() noexcept override {}
	void Set(std::string response)
	{
		m_outcome = { EBoundedProcessStatus::Succeeded, 0, Bytes(response), {} };
	}
	void SetStream(const EBoundedProcessStatus status, const std::vector<std::string>& chunks)
	{
		m_streamStatus = status;
		m_streamed.clear();
		for (const auto& chunk : chunks) m_streamed.push_back(Bytes(chunk));
	}
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
private:
	GhProcessOutcome m_outcome{ EBoundedProcessStatus::Failed, 1, {}, {} };
	std::vector<std::wstring> m_arguments;
	std::vector<std::vector<std::uint8_t>> m_streamed;
	EBoundedProcessStatus m_streamStatus{ EBoundedProcessStatus::Succeeded };
};

class ConnectionPlatform final : public IGhConnectionPlatform {
public:
	explicit ConnectionPlatform(std::shared_ptr<Credential> credential) : m_credential(std::move(credential)) {}
	GhConnectionCheckResult Check(const GhToolProbe&, std::wstring_view configuration,
		std::wstring_view hostname, std::optional<std::wstring_view>, HANDLE) override
	{
		return { GhConnectionTerminal::Succeeded,
			GhAccountIdentity(std::wstring(hostname), L"octocat", L"keyring", std::wstring(configuration)),
			m_credential };
	}
private:
	std::shared_ptr<Credential> m_credential;
};

class ToolPlatform final : public IGhToolPlatform {
public:
	std::optional<std::wstring> ResolveExecutable() const override { return L"C:/Tools/gh.exe"; }
	GhProcessOutcome Run(const GhProcessInvocation&, HANDLE) const override
	{
		return { EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.93.0 (2026-05-27)\n"), {} };
	}
};

class ProfileSource final : public ISenpGitHubProfileSource {
public:
	std::shared_ptr<CGhConnectionLifecycle> Connection(std::wstring_view profileId) override
	{
		return profileId == m_profileId ? m_lifecycle : nullptr;
	}
	std::optional<GhSelectedRepository> Repository(std::wstring_view profileId) override
	{
		if (profileId != m_profileId || !m_repository) return std::nullopt;
		return m_repository;
	}
	platform::controlipc::EControlSenpRpcStatus AdoptWorkspace(
		const platform::controlipc::SenpWorkspaceAdoption& adoption) override
	{
		adopted.push_back(adoption);
		return adoptStatus;
	}
	void WithdrawWorkspace(const platform::controlipc::SenpConnectionIdentity& connection) override
	{
		withdrawn.push_back(connection);
	}
	void SetLifecycle(std::shared_ptr<CGhConnectionLifecycle> lifecycle) { m_lifecycle = std::move(lifecycle); }
	void ClearRepository() noexcept { m_repository.reset(); }

	EControlSenpRpcStatus adoptStatus = EControlSenpRpcStatus::Succeeded;
	std::vector<platform::controlipc::SenpWorkspaceAdoption> adopted;
	std::vector<platform::controlipc::SenpConnectionIdentity> withdrawn;
private:
	std::wstring m_profileId{ L"profile-1" };
	std::shared_ptr<CGhConnectionLifecycle> m_lifecycle;
	std::optional<GhSelectedRepository> m_repository{
		GhSelectedRepository(L"repo-identity-1", L"origin", L"github.com", L"owner", L"repo") };
};

ContributionOwnerIdentity Owner()
{
	return { L"sakura.github-pull-requests", Digest(), 3, 2, 1 };
}

SenpToolExecutionScope Scope()
{
	return { 7, 1234, L"profile-1", Owner() };
}

SenpToolReadCommand IssueList()
{
	return { L"issues:open", L"github", L"repositoryRead", { { L"shape", L"issues" }, { L"state", L"open" } } };
}

SenpToolReadCommand JobLog()
{
	return { L"job:77:log", L"github", L"jobLog", { { L"id", L"77" } } };
}

class Fixture final {
public:
	Fixture()
	{
		auto lifecycle = std::make_shared<CGhConnectionLifecycle>(m_connection, m_grants,
			L"profile-1", L"C:/Profiles/gh");
		CGhToolPolicy policy(m_tool, L"C:/Sakura");
		const auto probe = policy.Probe(nullptr);
		auto attempt = lifecycle->Begin(L"github.com", std::nullopt);
		if (attempt) (void)lifecycle->Complete(std::move(*attempt), probe, nullptr);
		m_profiles->SetLifecycle(std::move(lifecycle));
		m_credential->Set("HTTP/2.0 200 OK\r\nContent-Type: application/json\r\nETag: W/\"abc\"\r\n\r\n"
			"[{\"id\":1,\"title\":\"first\"}]");
		m_executor = std::make_unique<CSenpGitHubToolExecutor>(m_tool, m_profiles, L"C:/Sakura");
	}
	[[nodiscard]] CSenpGitHubToolExecutor& Executor() noexcept { return *m_executor; }
	[[nodiscard]] Credential& CredentialValue() noexcept { return *m_credential; }
	[[nodiscard]] ProfileSource& Profiles() noexcept { return *m_profiles; }
	//! Starts one read and waits for its worker terminal without polling `gh`.
	[[nodiscard]] std::optional<effect::ToolCompleted> Fetch(const SenpToolExecutionScope& scope,
		const SenpToolReadCommand& command)
	{
		if (m_executor->StartRead(scope, command) != EControlSenpRpcStatus::Succeeded) return std::nullopt;
		if (!m_executor->WaitForIdle(kIdleTimeoutMilliseconds)) return std::nullopt;
		return m_executor->TakeCompleted(scope);
	}
private:
	std::shared_ptr<Credential> m_credential{ std::make_shared<Credential>() };
	std::shared_ptr<ConnectionPlatform> m_connection{ std::make_shared<ConnectionPlatform>(m_credential) };
	std::shared_ptr<ToolPlatform> m_tool{ std::make_shared<ToolPlatform>() };
	std::shared_ptr<Authority> m_authority{ std::make_shared<Authority>() };
	CSenpToolGrants m_grants{ m_authority };
	std::shared_ptr<ProfileSource> m_profiles{ std::make_shared<ProfileSource>() };
	std::unique_ptr<CSenpGitHubToolExecutor> m_executor;
};

std::wstring ResourceHandle(const effect::ToolCompleted& completed)
{
	const auto begin = completed.data.find(L"\"resource\":\"");
	if (begin == std::wstring::npos) return {};
	const auto start = begin + 12;
	const auto end = completed.data.find(L'"', start);
	return end == std::wstring::npos ? std::wstring() : completed.data.substr(start, end - start);
}

} // namespace

TEST(SenpGitHubToolExecutor, TranslatesOnlyTheClosedShapeSet)
{
	const GhSelectedRepository repository(L"repo-identity-1", L"origin", L"github.com", L"owner", L"repo");
	const auto issues = BuildRepositoryReadRequest(repository,
		{ { L"shape", L"issues" }, { L"state", L"open" }, { L"page", L"2" } });
	ASSERT_TRUE(issues);
	EXPECT_EQ((std::vector<std::wstring>{ L"issues" }), issues->ResourceSegments());
	EXPECT_EQ(L"owner", issues->Owner());
	ASSERT_EQ(2U, issues->Query().size());
	EXPECT_EQ(L"page", issues->Query()[0].first);
	EXPECT_EQ(L"state", issues->Query()[1].first);

	const auto jobs = BuildRepositoryReadRequest(repository, { { L"shape", L"runJobs" }, { L"id", L"42" } });
	ASSERT_TRUE(jobs);
	EXPECT_EQ((std::vector<std::wstring>{ L"actions", L"runs", L"42", L"jobs" }), jobs->ResourceSegments());

	// An unknown shape, a missing or surplus id, a free path and an unlisted query
	// name are all refused before anything reaches the tool policy.
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"secrets" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issue" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issues" }, { L"id", L"1" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issue" }, { L"id", L"01" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issue" }, { L"id", L"1/../secrets" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issues" }, { L"path", L"secrets" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, { { L"shape", L"issues" }, { L"state", L"" } }));
	EXPECT_FALSE(BuildRepositoryReadRequest(repository, {}));
}

TEST(SenpGitHubToolExecutor, CarriesAWorkspaceDeclarationWithoutReadingAFolder)
{
	Fixture fixture;
	platform::controlipc::SenpWorkspaceAdoption adoption;
	adoption.connection = { 7, 1234 };
	adoption.profileId = L"profile-1";
	adoption.workspace.generation = 5;
	adoption.workspace.revision = 11;
	adoption.workspace.folders = { L"file:///c:/work/repo" };
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().AdoptWorkspace(adoption));
	// Forwarded, not stored: resolving folders to a repository means reading git
	// remotes, which belongs on the worker that owns every blocking lookup here.
	ASSERT_EQ(1U, fixture.Profiles().adopted.size());
	EXPECT_EQ(L"profile-1", fixture.Profiles().adopted.front().profileId);
	EXPECT_EQ(11, fixture.Profiles().adopted.front().workspace.revision);

	// A declaration that names no connection could never be withdrawn, and one
	// that names no profile answers for nothing. Neither reaches the source.
	auto nameless = adoption;
	nameless.connection = {};
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().AdoptWorkspace(nameless));
	auto profileless = adoption;
	profileless.profileId.clear();
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().AdoptWorkspace(profileless));
	EXPECT_EQ(1U, fixture.Profiles().adopted.size());

	fixture.Executor().WithdrawWorkspace(adoption.connection);
	ASSERT_EQ(1U, fixture.Profiles().withdrawn.size());
	EXPECT_EQ(7U, fixture.Profiles().withdrawn.front().sessionId);
	EXPECT_EQ(1234U, fixture.Profiles().withdrawn.front().clientProcessId);
}

TEST(SenpGitHubToolExecutor, PublishesOneFetchedPageAsAReadableResource)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	EXPECT_EQ(effect::CompletionStatus::Succeeded, completed->status);
	EXPECT_EQ(L"issues:open", completed->readId);
	const auto handle = ResourceHandle(*completed);
	ASSERT_FALSE(handle.empty());
	// The extension parses this body itself and the protocol gives it no way to
	// read a resource, so the body travels inside the completion. Everything the
	// page was answered with travels beside it, unchanged.
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"(,"body":[{"id":1,"title":"first"}]})"));
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"("httpStatus":200)"));
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"("page":1)"));

	ControlSenpRpcResponse response;
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, handle, 0, 64 * 1024, response));
	EXPECT_EQ("[{\"id\":1,\"title\":\"first\"}]", response.resourceBytes);
	EXPECT_EQ(handle, response.resourceHandle);
	// The whole chunk, as the store answered it. Without the length and the end
	// the editor could not tell a finished body from a truncated one, and would
	// have to decide that from the byte count it happened to receive.
	EXPECT_EQ(static_cast<std::uint8_t>(TextResourceState::Complete), response.resourceState);
	EXPECT_EQ(static_cast<std::uint8_t>(TextResourceEnd::Complete), response.resourceEnd);
	EXPECT_EQ(response.resourceBytes.size(), response.resourceLength);

	// The argv reaching the credential is the one the closed policy built.
	const auto& arguments = fixture.CredentialValue().Arguments();
	ASSERT_FALSE(arguments.empty());
	EXPECT_EQ(L"api", arguments.front());
	EXPECT_EQ(L"repos/owner/repo/issues?state=open", arguments.back());

	// One terminal is drained exactly once.
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
}

TEST(SenpGitHubToolExecutor, AnswersTheAdoptedAccountGenerationWithoutAnExecutionScope)
{
	Fixture fixture;
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().QueryAccount(L"profile-1", response));
	EXPECT_EQ(EControlSenpAccountState::Connected, response.accountState);
	// This is exactly the number a read's owner has to carry, which is why the
	// editor has to be able to ask for it before it builds one.
	EXPECT_EQ(Owner().accountGeneration, response.accountGeneration);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(Scope(), IssueList()));
}

TEST(SenpGitHubToolExecutor, SeparatesAnUnadoptedProfileFromASignedOutOne)
{
	Fixture fixture;
	ControlSenpRpcResponse unadopted;
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().QueryAccount(L"profile-2", unadopted));
	// Both answers carry generation zero. Only the state distinguishes "nothing
	// has been adopted or checked" from "the account is signed out", so reporting
	// Disconnected here would announce a sign-out that never happened.
	EXPECT_EQ(0, unadopted.accountGeneration);
	EXPECT_EQ(EControlSenpAccountState::Unknown, unadopted.accountState);

	const auto connection = fixture.Profiles().Connection(L"profile-1");
	ASSERT_NE(nullptr, connection);
	connection->Disconnect();
	ControlSenpRpcResponse signedOut;
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().QueryAccount(L"profile-1", signedOut));
	EXPECT_EQ(0, signedOut.accountGeneration);
	EXPECT_EQ(EControlSenpAccountState::Disconnected, signedOut.accountState);
}

TEST(SenpGitHubToolExecutor, RefusesAReadBeforeAnAccountOrWorkspaceExists)
{
	Fixture fixture;
	auto unadopted = Scope();
	unadopted.owner.accountGeneration = 0;
	EXPECT_EQ(EControlSenpRpcStatus::NotConnected,
		fixture.Executor().StartRead(unadopted, IssueList()));

	auto foreign = Scope();
	foreign.profileId = L"profile-2";
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, fixture.Executor().StartRead(foreign, IssueList()));

	fixture.Profiles().ClearRepository();
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, fixture.Executor().StartRead(Scope(), IssueList()));
}

TEST(SenpGitHubToolExecutor, RefusesAnOwnerWithoutAnArchiveDigest)
{
	Fixture fixture;
	auto scope = Scope();
	scope.owner.packageDigest = L"sha256:ab";
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().StartRead(scope, IssueList()));
}

TEST(SenpGitHubToolExecutor, ReportsAToolTerminalWithoutPublishingAResource)
{
	Fixture fixture;
	fixture.CredentialValue().Set("HTTP/2.0 404 Not Found\r\nContent-Type: application/json\r\n\r\n{}");
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	EXPECT_EQ(effect::CompletionStatus::Failed, completed->status);
	EXPECT_EQ(L"not-found", completed->message);
	EXPECT_TRUE(completed->data.empty());

	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::NotFound,
		fixture.Executor().ReadResource(scope, L"text:1:1", 0, 1024, response));
}

TEST(SenpGitHubToolExecutor, ReleasingAResourceEndsFurtherReads)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	const auto handle = ResourceHandle(*completed);
	ASSERT_FALSE(handle.empty());
	fixture.Executor().ReleaseResource(scope, handle);
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::NotFound,
		fixture.Executor().ReadResource(scope, handle, 0, 1024, response));
	EXPECT_TRUE(response.resourceBytes.empty());
}

TEST(SenpGitHubToolExecutor, AResourceBelongsToExactlyOneScope)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	const auto handle = ResourceHandle(*completed);
	ASSERT_FALSE(handle.empty());

	auto other = Scope();
	other.sessionId = 8;
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized,
		fixture.Executor().ReadResource(other, handle, 0, 1024, response));
	EXPECT_FALSE(fixture.Executor().TakeCompleted(other));
}

TEST(SenpGitHubToolExecutor, CancellingTheScopeDropsItsReadsAndResources)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	const auto handle = ResourceHandle(*completed);
	ASSERT_FALSE(handle.empty());

	fixture.Executor().CancelScope(scope);
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized,
		fixture.Executor().ReadResource(scope, handle, 0, 1024, response));
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
	// Cancelling twice is not an error and must not resurrect the scope.
	fixture.Executor().CancelScope(scope);
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
}

TEST(SenpGitHubToolExecutor, RepeatingAReadIdentityRefreshesInsteadOfSubscribingTwice)
{
	Fixture fixture;
	const auto scope = Scope();
	ASSERT_TRUE(fixture.Fetch(scope, IssueList()));
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(scope, IssueList()));
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	const auto refreshed = fixture.Executor().TakeCompleted(scope);
	ASSERT_TRUE(refreshed);
	EXPECT_EQ(effect::CompletionStatus::Succeeded, refreshed->status);
	EXPECT_NE(ResourceHandle(*refreshed), std::wstring());

	// The same identity may not be reused for a different resource.
	auto different = IssueList();
	different.arguments = { { L"shape", L"pulls" } };
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().StartRead(scope, different));
}

TEST(SenpGitHubToolExecutor, CancellingAReadStopsItsDelivery)
{
	Fixture fixture;
	const auto scope = Scope();
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(scope, IssueList()));
	fixture.Executor().CancelRead(scope, L"issues:open");
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
}

TEST(SenpGitHubToolExecutor, RefusesAnUnusableResourceRead)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	const auto handle = ResourceHandle(*completed);
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest,
		fixture.Executor().ReadResource(scope, handle, 0, 0, response));
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest,
		fixture.Executor().ReadResource(scope, handle, 0, 64 * 1024 + 1, response));
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest,
		fixture.Executor().ReadResource(scope, L"", 0, 1024, response));
}

TEST(SenpGitHubToolExecutor, RefusesAPageTooLargeForOneCompletion)
{
	Fixture fixture;
	const auto scope = Scope();
	// Under the fetch budget and over the completion budget. Delivering it short
	// would hand the extension a JSON body that ends mid-value, which parses as
	// a broken page rather than as the too-large one it is.
	const std::string body = "[\"" + std::string(70 * 1024, 'x') + "\"]";
	fixture.CredentialValue().Set("HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n" + body);
	const auto completed = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(completed);
	EXPECT_EQ(effect::CompletionStatus::Failed, completed->status);
	EXPECT_EQ(L"page-too-large", completed->message);
	EXPECT_TRUE(completed->data.empty());
	// Nothing was published under it either, so no slot is spent on a page the
	// editor was never given a way to reach.
	EXPECT_TRUE(ResourceHandle(*completed).empty());
}

TEST(SenpGitHubToolExecutor, ARefreshReplacesThePageResourceItPublished)
{
	Fixture fixture;
	const auto scope = Scope();
	const auto first = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(first);
	const auto firstHandle = ResourceHandle(*first);
	ASSERT_FALSE(firstHandle.empty());

	// A subscription refreshes for as long as the view is open. One read owning
	// more than one resource at a time would spend the scope's whole allowance
	// on a single view that simply stayed open.
	std::wstring lastHandle = firstHandle;
	for (int refresh = 0; refresh < static_cast<int>(
		CSenpGitHubToolExecutor::MaximumResourcesPerScope()) + 4; ++refresh) {
		const auto refreshed = fixture.Fetch(scope, IssueList());
		ASSERT_TRUE(refreshed) << refresh;
		EXPECT_EQ(effect::CompletionStatus::Succeeded, refreshed->status) << refresh;
		lastHandle = ResourceHandle(*refreshed);
		ASSERT_FALSE(lastHandle.empty()) << refresh;
	}
	ControlSenpRpcResponse response;
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, lastHandle, 0, 64 * 1024, response));
	// The replaced one is gone rather than merely unreferenced: what a refresh
	// published before is not what the view is showing now.
	EXPECT_EQ(EControlSenpRpcStatus::NotFound,
		fixture.Executor().ReadResource(scope, firstHandle, 0, 64 * 1024, response));
}

TEST(SenpGitHubToolExecutor, PublishesAPageWithoutAResourceWhenNoSlotIsLeft)
{
	Fixture fixture;
	const auto scope = Scope();
	// One view of one extension opens more concurrent reads than a scope has
	// resource slots. The resource is an extra, so running out of them cannot be
	// what stops the page from arriving.
	for (std::size_t index = 0; index < CSenpGitHubToolExecutor::MaximumResourcesPerScope(); ++index) {
		auto command = IssueList();
		command.readId = L"issues:" + std::to_wstring(index);
		command.arguments.push_back({ L"page", std::to_wstring(index + 1) });
		const auto completed = fixture.Fetch(scope, command);
		ASSERT_TRUE(completed) << index;
		ASSERT_EQ(effect::CompletionStatus::Succeeded, completed->status) << index;
		EXPECT_FALSE(ResourceHandle(*completed).empty()) << index;
	}
	auto beyond = IssueList();
	beyond.readId = L"issues:beyond";
	beyond.arguments.push_back({ L"page", L"9" });
	const auto completed = fixture.Fetch(scope, beyond);
	ASSERT_TRUE(completed);
	EXPECT_EQ(effect::CompletionStatus::Succeeded, completed->status);
	EXPECT_TRUE(ResourceHandle(*completed).empty());
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"(,"body":[{"id":1,"title":"first"}]})"));
}

TEST(SenpGitHubToolExecutor, TranslatesOnlyAJobIdIntoALogRequest)
{
	const GhSelectedRepository repository(L"repo-identity-1", L"origin", L"github.com", L"owner", L"repo");
	const auto log = BuildJobLogRequest(repository, { { L"id", L"77" } });
	ASSERT_TRUE(log);
	// The host, owner and repository come from what the control side resolved,
	// never from the editor, exactly as they do for a page.
	EXPECT_EQ(L"github.com", log->Hostname());
	EXPECT_EQ(L"owner", log->Owner());
	EXPECT_EQ(L"repo", log->Repository());
	EXPECT_EQ(77U, log->JobId());

	// A log is one endpoint, so there is no shape to choose and nothing else to
	// name. Anything but exactly one decimal job id is refused before the policy.
	EXPECT_FALSE(BuildJobLogRequest(repository, {}));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"shape", L"job" }, { L"id", L"77" } }));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"job", L"77" } }));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"id", L"0" } }));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"id", L"077" } }));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"id", L"77/../secrets" } }));
	EXPECT_FALSE(BuildJobLogRequest(repository, { { L"id", L"" } }));
}

TEST(SenpGitHubToolExecutor, PublishesAJobLogAsAResourceOfItsOwnStore)
{
	Fixture fixture;
	const auto scope = Scope();
	fixture.CredentialValue().SetStream(EBoundedProcessStatus::Succeeded, { "line one\n", "line two\n" });
	const auto completed = fixture.Fetch(scope, JobLog());
	ASSERT_TRUE(completed);
	EXPECT_EQ(effect::CompletionStatus::Succeeded, completed->status);
	EXPECT_EQ(L"job:77:log", completed->readId);
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"("log":true)"));
	EXPECT_NE(std::wstring::npos, completed->data.find(LR"("bytes":18)"));
	// The log itself never travels inside the completion, no more than a page
	// body does: the editor reads it back through the same grant.
	EXPECT_EQ(std::wstring::npos, completed->data.find(L"line one"));

	const auto handle = ResourceHandle(*completed);
	ASSERT_FALSE(handle.empty());
	ControlSenpRpcResponse response;
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, handle, 0, 64 * 1024, response));
	EXPECT_EQ("line one\nline two\n", response.resourceBytes);
	EXPECT_EQ(static_cast<std::uint8_t>(TextResourceState::Complete), response.resourceState);
	EXPECT_EQ(static_cast<std::uint8_t>(TextResourceEnd::Complete), response.resourceEnd);
	EXPECT_EQ(18U, response.resourceLength);

	// The argv is the one the closed policy built. `gh` follows the short-lived
	// download redirect internally, so no URL is named here or anywhere else.
	const auto& arguments = fixture.CredentialValue().Arguments();
	ASSERT_FALSE(arguments.empty());
	EXPECT_EQ(L"api", arguments.front());
	EXPECT_EQ(L"repos/owner/repo/actions/jobs/77/logs", arguments.back());

	// A log owns the whole store it was written into, so releasing it is the
	// end of that store and not merely of a name inside a shared one.
	fixture.Executor().ReleaseResource(scope, handle);
	EXPECT_EQ(EControlSenpRpcStatus::NotFound,
		fixture.Executor().ReadResource(scope, handle, 0, 1024, response));
}

TEST(SenpGitHubToolExecutor, KeepsALogAndAPageApartWithinOneScope)
{
	Fixture fixture;
	const auto scope = Scope();
	fixture.CredentialValue().SetStream(EBoundedProcessStatus::Succeeded, { "log bytes" });
	const auto page = fixture.Fetch(scope, IssueList());
	ASSERT_TRUE(page);
	const auto log = fixture.Fetch(scope, JobLog());
	ASSERT_TRUE(log);
	const auto pageHandle = ResourceHandle(*page);
	const auto logHandle = ResourceHandle(*log);
	ASSERT_FALSE(pageHandle.empty());
	ASSERT_FALSE(logHandle.empty());

	// Two stores, so the handles are free to collide as strings. What routes a
	// read is which of the two the scope recorded the handle in.
	ControlSenpRpcResponse fromPage;
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, pageHandle, 0, 64 * 1024, fromPage));
	EXPECT_EQ("[{\"id\":1,\"title\":\"first\"}]", fromPage.resourceBytes);
	ControlSenpRpcResponse fromLog;
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, logHandle, 0, 64 * 1024, fromLog));
	EXPECT_EQ("log bytes", fromLog.resourceBytes);

	// A page read is a subscription and keeps its identity for as long as it is
	// held, so a log may not take one out from under it.
	auto reused = JobLog();
	reused.readId = L"issues:open";
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().StartRead(scope, reused));
	// A log is one download rather than a subscription: once answered its
	// identity is spent and a page may take it. The log itself is keyed by its
	// handle, so it outlives the identity and stays readable.
	auto reusedPage = IssueList();
	reusedPage.readId = L"job:77:log";
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(scope, reusedPage));
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	ControlSenpRpcResponse logAgain;
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded,
		fixture.Executor().ReadResource(scope, logHandle, 0, 64 * 1024, logAgain));
	EXPECT_EQ("log bytes", logAgain.resourceBytes);
}

TEST(SenpGitHubToolExecutor, ReportsAFailedJobLogWithoutPublishingAResource)
{
	Fixture fixture;
	const auto scope = Scope();
	fixture.CredentialValue().SetStream(EBoundedProcessStatus::Failed, {});
	const auto completed = fixture.Fetch(scope, JobLog());
	ASSERT_TRUE(completed);
	// A log that did not arrive is said to have failed and names no resource:
	// an empty resource the editor could open would read as an empty log.
	EXPECT_EQ(effect::CompletionStatus::Failed, completed->status);
	EXPECT_EQ(L"unavailable-or-not-found", completed->message);
	EXPECT_TRUE(completed->data.empty());
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));

	// The same identity is free again, because nothing was answered under it.
	fixture.CredentialValue().SetStream(EBoundedProcessStatus::Succeeded, { "second try" });
	const auto retried = fixture.Fetch(scope, JobLog());
	ASSERT_TRUE(retried);
	EXPECT_EQ(effect::CompletionStatus::Succeeded, retried->status);
}

TEST(SenpGitHubToolExecutor, CancellingAJobLogDropsItsResultAndItsStore)
{
	Fixture fixture;
	const auto scope = Scope();
	fixture.CredentialValue().SetStream(EBoundedProcessStatus::Succeeded, { "line one\n" });
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(scope, JobLog()));
	fixture.Executor().CancelRead(scope, L"job:77:log");
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	// Whether the worker had already claimed the job or not, forgetting the read
	// identity is what makes the result undeliverable.
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));

	// Cancelling a whole scope leaves nothing a later download could reach.
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, fixture.Executor().StartRead(scope, JobLog()));
	fixture.Executor().CancelScope(scope);
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
}

TEST(SenpGitHubToolExecutor, StartsOnlyTheTwoOperationsThisToolNames)
{
	Fixture fixture;
	const auto scope = Scope();
	// The broker admits the closed set first. This object names it again because
	// it has to know which shape a read is; one it cannot name has no shape.
	auto foreignTool = JobLog();
	foreignTool.toolId = L"shell";
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().StartRead(scope, foreignTool));
	auto foreignOperation = IssueList();
	foreignOperation.operation = L"repositoryWrite";
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest,
		fixture.Executor().StartRead(scope, foreignOperation));
	auto nameless = IssueList();
	nameless.operation.clear();
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, fixture.Executor().StartRead(scope, nameless));
	ASSERT_TRUE(fixture.Executor().WaitForIdle(kIdleTimeoutMilliseconds));
	EXPECT_FALSE(fixture.Executor().TakeCompleted(scope));
}
