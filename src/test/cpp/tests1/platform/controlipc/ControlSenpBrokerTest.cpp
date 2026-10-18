/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlSenpBroker.h"

#include "senp/SenpTextResource.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr std::uint64_t kGeneration = 42;
const std::wstring kDigest(64, L'a');

senp::SenpApprovedToolOwner Approved(bool enabled = true,
	senp::SenpToolCapability capabilities = senp::SenpToolCapability::GitHubRepositoryRead)
{
	return { L"profile-1", L"sample.github", kDigest, 5, capabilities, enabled };
}

senp::SenpApprovedToolOwner ApprovedFor(std::wstring extensionId)
{
	return { L"profile-1", std::move(extensionId), kDigest, 5,
		senp::SenpToolCapability::GitHubRepositoryRead, true };
}

class Authority final : public senp::ISenpToolGrantAuthority {
public:
	explicit Authority(senp::SenpApprovedToolOwner value) { Set(std::move(value)); }
	std::optional<senp::SenpApprovedToolOwner> Resolve(std::wstring_view profileId,
		std::wstring_view extensionId) const override
	{
		const auto found = m_values.find(std::wstring(extensionId));
		return found != m_values.end() && found->second.ProfileId() == profileId
			? std::optional<senp::SenpApprovedToolOwner>(found->second) : std::nullopt;
	}
	void Set(senp::SenpApprovedToolOwner value)
	{
		m_values.insert_or_assign(std::wstring(value.ExtensionId()), std::move(value));
	}
private:
	std::map<std::wstring, senp::SenpApprovedToolOwner> m_values;
};

//! Records what the broker admitted. It never blocks, mirroring the contract
//! that frame processing must not wait on an external process. State is
//! private with test-facing accessors/setters so this fake follows the same
//! encapsulation rule as the production DTOs it records.
class Executor final : public ISenpToolExecutor {
public:
	EControlSenpRpcStatus StartRead(const SenpToolExecutionScope& scope,
		const SenpToolReadCommand& command) noexcept override
	{
		m_started.push_back({ scope, command });
		return m_next;
	}
	std::optional<senp::effect::ToolCompleted> TakeCompleted(
		const SenpToolExecutionScope& scope) noexcept override
	{
		m_polled.push_back(scope);
		if (!m_pending) return {};
		auto value = *m_pending;
		m_pending.reset();
		return value;
	}
	void CancelRead(const SenpToolExecutionScope& scope, std::wstring_view readId) noexcept override
	{
		m_cancelledReads.emplace_back(scope, std::wstring(readId));
	}
	void CancelConnection(std::uint64_t sessionId, std::uint32_t clientProcessId) noexcept override
	{
		m_cancelledConnections.emplace_back(sessionId, clientProcessId);
	}
	EControlSenpRpcStatus ReadResource(const SenpToolExecutionScope&, std::wstring_view handle,
		std::uint64_t offset, std::uint32_t length, ControlSenpRpcResponse& response) noexcept override
	{
		// A refused read still writes, so the broker must discard a partial answer.
		response.SetResourceHandle(std::wstring(handle));
		response.SetResourceOffset(offset);
		response.SetResourceBytes(std::string(length, 'x'));
		response.SetResourceState(static_cast<std::uint8_t>(senp::TextResourceState::Complete));
		response.SetResourceEnd(static_cast<std::uint8_t>(senp::TextResourceEnd::Complete));
		response.SetResourceLength(offset + length);
		response.SetResourceRevision(11);
		return m_resourceStatus;
	}
	void ReleaseResource(const SenpToolExecutionScope&, std::wstring_view handle) noexcept override
	{
		m_released.push_back(std::wstring(handle));
	}
	EControlSenpRpcStatus QueryAccount(std::wstring_view profileId,
		ControlSenpRpcResponse& response) noexcept override
	{
		m_accountQueries.push_back(std::wstring(profileId));
		// A refused query still writes, so the broker must discard a partial answer.
		response.SetAccountGeneration(m_accountGeneration);
		response.SetAccountState(m_accountState);
		return m_accountStatus;
	}
	EControlSenpRpcStatus AdoptWorkspace(const SenpWorkspaceAdoption& adoption) noexcept override
	{
		m_adopted.push_back(adoption);
		return m_adoptStatus;
	}
	void WithdrawWorkspace(const SenpConnectionIdentity& connection) noexcept override
	{
		m_withdrawn.push_back(connection);
	}

	void SetNext(EControlSenpRpcStatus value) noexcept { m_next = value; }
	void SetResourceStatus(EControlSenpRpcStatus value) noexcept { m_resourceStatus = value; }
	void SetPending(senp::effect::ToolCompleted value) { m_pending = std::move(value); }
	void SetAccountStatus(EControlSenpRpcStatus value) noexcept { m_accountStatus = value; }
	void SetAccountGeneration(std::int64_t value) noexcept { m_accountGeneration = value; }
	void SetAccountState(EControlSenpAccountState value) noexcept { m_accountState = value; }
	void SetAdoptStatus(EControlSenpRpcStatus value) noexcept { m_adoptStatus = value; }

	[[nodiscard]] const std::vector<std::pair<SenpToolExecutionScope, SenpToolReadCommand>>&
		Started() const noexcept { return m_started; }
	[[nodiscard]] const std::vector<SenpToolExecutionScope>& Polled() const noexcept { return m_polled; }
	[[nodiscard]] const std::vector<std::pair<SenpToolExecutionScope, std::wstring>>&
		CancelledReads() const noexcept { return m_cancelledReads; }
	[[nodiscard]] const std::vector<SenpConnectionIdentity>& CancelledConnections() const noexcept
	{
		return m_cancelledConnections;
	}
	[[nodiscard]] const std::vector<std::wstring>& Released() const noexcept { return m_released; }
	[[nodiscard]] const std::vector<std::wstring>& AccountQueries() const noexcept { return m_accountQueries; }
	[[nodiscard]] const std::vector<SenpWorkspaceAdoption>& Adopted() const noexcept { return m_adopted; }
	[[nodiscard]] const std::vector<SenpConnectionIdentity>& Withdrawn() const noexcept { return m_withdrawn; }

private:
	EControlSenpRpcStatus m_next = EControlSenpRpcStatus::Succeeded;
	EControlSenpRpcStatus m_resourceStatus = EControlSenpRpcStatus::Succeeded;
	std::optional<senp::effect::ToolCompleted> m_pending;
	std::vector<std::pair<SenpToolExecutionScope, SenpToolReadCommand>> m_started;
	std::vector<SenpToolExecutionScope> m_polled;
	std::vector<std::pair<SenpToolExecutionScope, std::wstring>> m_cancelledReads;
	std::vector<SenpConnectionIdentity> m_cancelledConnections;
	std::vector<std::wstring> m_released;
	EControlSenpRpcStatus m_accountStatus = EControlSenpRpcStatus::Succeeded;
	std::int64_t m_accountGeneration = 0;
	EControlSenpAccountState m_accountState = EControlSenpAccountState::Unknown;
	std::vector<std::wstring> m_accountQueries;
	EControlSenpRpcStatus m_adoptStatus = EControlSenpRpcStatus::Succeeded;
	std::vector<SenpWorkspaceAdoption> m_adopted;
	std::vector<SenpConnectionIdentity> m_withdrawn;
};

ControlSenpRpcOwner Owner()
{
	return ControlSenpRpcOwner(L"sample.github", kDigest, 7, 11, 13);
}

ControlSenpRpcOwner OwnerB()
{
	return ControlSenpRpcOwner(L"other.github", kDigest, 7, 11, 13);
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
		EControlIpcFlags::Request, requestId, kGeneration },
		fields.value_or(std::vector<std::uint8_t>{}) };
}

std::optional<ControlSenpRpcResponse> ReadResponse(const ControlIpcFrameDispatchResult& result)
{
	if (result.responseFrames.size() != 1) return {};
	const auto& frame = result.responseFrames.front();
	if (frame.header.kind != EControlIpcKind::SenpResponse) return {};
	const auto fields = DecodeControlIpcFields(frame.payload);
	if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || fields.fields.size() != 1) return {};
	return DecodeControlSenpRpcResponse(fields.fields.front().value);
}

ControlSenpRpcRequest IssueGrant(ControlSenpRpcOwner owner = Owner())
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::IssueGrant);
	request.SetProfileId(L"profile-1");
	request.SetOwner(std::move(owner));
	request.SetCapabilities(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead));
	return request;
}

ControlSenpRpcRequest QueryAccount()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::QueryAccount);
	request.SetProfileId(L"profile-1");
	return request;
}

ControlSenpRpcRequest AdoptWorkspace()
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::AdoptWorkspace);
	request.SetProfileId(L"profile-1");
	request.SetWorkspace(ControlSenpRpcWorkspace(5, 11, { L"file:///c:/work/repo" }));
	return request;
}

ControlSenpRpcRequest StartRead(std::string grantId, ControlSenpRpcOwner owner = Owner())
{
	ControlSenpRpcRequest request;
	request.SetOperation(EControlSenpRpcOperation::StartRead);
	request.SetProfileId(L"profile-1");
	request.SetOwner(std::move(owner));
	request.SetGrantId(std::move(grantId));
	request.SetReadId(L"issues:open:1");
	request.SetToolId(L"github");
	request.SetToolOperation(L"repositoryRead");
	request.SetArguments({ { L"path", L"issues" } });
	return request;
}

//! Test-owned fixture. Its shared setup state is private with named accessors,
//! matching the encapsulation rule applied to the production DTOs it drives.
class Fixture {
public:
	Fixture() :
		m_authority(std::make_shared<Authority>(Approved())),
		m_grants(std::make_shared<senp::CSenpToolGrants>(m_authority)),
		m_executor(std::make_shared<Executor>()),
		m_now(std::chrono::steady_clock::now()),
		m_broker(m_grants, m_executor, [this] { return m_now; }), m_connection{ 31, 1701 }
	{
	}

	[[nodiscard]] const std::shared_ptr<Authority>& ToolAuthority() const noexcept { return m_authority; }
	[[nodiscard]] const std::shared_ptr<senp::CSenpToolGrants>& Grants() const noexcept { return m_grants; }
	[[nodiscard]] const std::shared_ptr<Executor>& ToolExecutor() const noexcept { return m_executor; }
	[[nodiscard]] CControlSenpBroker& Broker() noexcept { return m_broker; }
	[[nodiscard]] const ControlIpcSessionContext& Connection() const noexcept { return m_connection; }
	void SetConnection(ControlIpcSessionContext value) noexcept { m_connection = value; }

	std::unique_ptr<IControlIpcSessionHandler> Open() { return m_broker.CreateSession(m_connection); }

	void Advance(std::chrono::steady_clock::duration span) noexcept { m_now += span; }
	void AdvancePastGrantLifetime() noexcept { Advance(senp::CSenpToolGrants::GrantLifetime() + std::chrono::minutes(1)); }

	std::string Grant(IControlIpcSessionHandler& session, ControlSenpRpcOwner owner = Owner())
	{
		const auto reply = ReadResponse(session.HandleFrame(m_connection, RequestFrame(IssueGrant(std::move(owner)))));
		return reply && reply->Status() == EControlSenpRpcStatus::Succeeded ? reply->GrantId() : std::string{};
	}

private:
	std::shared_ptr<Authority> m_authority;
	std::shared_ptr<senp::CSenpToolGrants> m_grants;
	std::shared_ptr<Executor> m_executor;
	std::chrono::steady_clock::time_point m_now;
	CControlSenpBroker m_broker;
	ControlIpcSessionContext m_connection;
};

TEST(ControlSenpBroker, IssuesConnectionBoundGrantAndAdmitsOneRead)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto issued = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(IssueGrant())));
	ASSERT_TRUE(issued);
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, issued->Status());
	EXPECT_EQ(32U, issued->GrantId().size());
	// A steady_clock origin is meaningless across processes, so the wire must
	// carry a remaining lifetime rather than an absolute instant.
	EXPECT_GT(issued->ExpiresAtMilliseconds(), 0U);
	EXPECT_LE(issued->ExpiresAtMilliseconds(),
		static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			senp::CSenpToolGrants::GrantLifetime()).count()));

	const auto started = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(issued->GrantId()), 2)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started->Status());
	ASSERT_EQ(1U, fixture.ToolExecutor()->Started().size());
	const auto& scope = fixture.ToolExecutor()->Started().front().first;
	EXPECT_EQ(fixture.Connection().sessionId, scope.SessionId());
	EXPECT_EQ(fixture.Connection().clientProcessId, scope.ClientProcessId());
	EXPECT_EQ(L"profile-1", scope.ProfileId());
	EXPECT_EQ(L"sample.github", scope.Owner().extensionId);
	EXPECT_EQ(7, scope.Owner().generation);
	EXPECT_EQ(L"issues:open:1", fixture.ToolExecutor()->Started().front().second.ReadId());
}

TEST(ControlSenpBroker, AnswersTheAccountQueryWithoutAGrantOrAnOwner)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	fixture.ToolExecutor()->SetAccountGeneration(5);
	fixture.ToolExecutor()->SetAccountState(EControlSenpAccountState::Connected);
	// No IssueGrant precedes it on purpose: the editor cannot build the owner a
	// grant is scoped by until it has been told which account generation to use.
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(QueryAccount())));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, reply->Status());
	EXPECT_EQ(5, reply->AccountGeneration());
	EXPECT_EQ(EControlSenpAccountState::Connected, reply->AccountState());
	ASSERT_EQ(1U, fixture.ToolExecutor()->AccountQueries().size());
	EXPECT_EQ(L"profile-1", fixture.ToolExecutor()->AccountQueries().front());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, RefusesAnAccountQueryOutsideTheProfileIdentitySpace)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	auto request = QueryAccount();
	request.SetProfileId(L"../other");
	// Nothing owner-scoped is rechecked for this operation, so admission is the
	// whole check: the id must belong to the space every grant is scoped by.
	const auto reply = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(request)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->AccountQueries().empty());
}

TEST(ControlSenpBroker, DiscardsAPartialAccountAnswerWhenTheQueryIsRefused)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	fixture.ToolExecutor()->SetAccountGeneration(5);
	fixture.ToolExecutor()->SetAccountState(EControlSenpAccountState::Connected);
	fixture.ToolExecutor()->SetAccountStatus(EControlSenpRpcStatus::Unavailable);
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(QueryAccount())));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, reply->Status());
	EXPECT_EQ(0, reply->AccountGeneration());
	EXPECT_EQ(EControlSenpAccountState::Unknown, reply->AccountState());
}

TEST(ControlSenpBroker, AttributesADeclaredWorkspaceToTheConnectionTheOsObserved)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	// No IssueGrant precedes it, for the same reason the account query needs
	// none: the declaration is what a window says before any owner exists.
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(AdoptWorkspace())));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, reply->Status());
	ASSERT_EQ(1U, fixture.ToolExecutor()->Adopted().size());
	const auto& adoption = fixture.ToolExecutor()->Adopted().front();
	// Taken from the pipe rather than from the request, so a declaration can
	// only ever be attributed to the connection that actually made it.
	EXPECT_EQ(fixture.Connection().sessionId, adoption.Connection().SessionId());
	EXPECT_EQ(fixture.Connection().clientProcessId, adoption.Connection().ClientProcessId());
	EXPECT_EQ(L"profile-1", adoption.ProfileId());
	EXPECT_EQ(11, adoption.Workspace().Revision());
	ASSERT_EQ(1U, adoption.Workspace().Folders().size());
	EXPECT_EQ(L"file:///c:/work/repo", adoption.Workspace().Folders().front());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, RefusesAWorkspaceDeclaredOutsideTheProfileIdentitySpace)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	auto request = AdoptWorkspace();
	request.SetProfileId(L"../other");
	// Nothing owner-scoped is rechecked for this operation either, so admission
	// is the whole check and it must not reach the executor.
	const auto reply = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(request)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Adopted().empty());
}

TEST(ControlSenpBroker, WithdrawsTheDeclaredWorkspaceWhenTheConnectionEnds)
{
	Fixture fixture;
	{
		auto session = fixture.Open();
		ASSERT_NE(nullptr, session);
		ASSERT_TRUE(ReadResponse(
			session->HandleFrame(fixture.Connection(), RequestFrame(AdoptWorkspace()))));
		EXPECT_TRUE(fixture.ToolExecutor()->Withdrawn().empty());
	}
	// A declaration outliving its connection would let a closed window keep
	// deciding which repository the profile answers for.
	ASSERT_EQ(1U, fixture.ToolExecutor()->Withdrawn().size());
	EXPECT_EQ(fixture.Connection().sessionId, fixture.ToolExecutor()->Withdrawn().front().SessionId());
	EXPECT_EQ(fixture.Connection().clientProcessId, fixture.ToolExecutor()->Withdrawn().front().ClientProcessId());

	{
		// A connection that declared nothing withdraws unconditionally too:
		// remembering which ones declared would only add a way to skip one.
		auto session = fixture.Open();
		ASSERT_NE(nullptr, session);
	}
	EXPECT_EQ(2U, fixture.ToolExecutor()->Withdrawn().size());
}

TEST(ControlSenpBroker, RefusesEveryOperationThatNamesAnUnknownGrant)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead("not-a-grant"))));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, RefusesAGrantMintedOnAnotherConnection)
{
	Fixture fixture;
	auto first = fixture.Open();
	ASSERT_NE(nullptr, first);
	const auto grantId = fixture.Grant(*first);
	ASSERT_FALSE(grantId.empty());

	fixture.SetConnection({ 32, 1702 });
	auto second = fixture.Open();
	ASSERT_NE(nullptr, second);
	// The record exists in the registry but belongs to a different pipe, so the
	// second connection must not be able to name it.
	const auto reply = ReadResponse(
		second->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantId))));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, RefusesAReadWhoseOwnerDoesNotMatchTheGrantScope)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	auto request = StartRead(grantId);
	request.Owner().SetGeneration(8);
	const auto reply = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(request, 3)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, RefusesAToolOperationOutsideTheClosedSet)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	auto foreignTool = StartRead(grantId);
	foreignTool.SetToolId(L"shell");
	const auto tool = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(foreignTool, 3)));
	ASSERT_TRUE(tool);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, tool->Status());

	auto foreignOperation = StartRead(grantId);
	foreignOperation.SetToolOperation(L"repositoryWrite");
	const auto operation = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(foreignOperation, 4)));
	ASSERT_TRUE(operation);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, operation->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());

	// The set holds exactly two, and the second was admitted by the capability
	// the first already needed: a job log reads the repository a page reads.
	auto log = StartRead(grantId);
	log.SetToolOperation(L"jobLog");
	log.SetReadId(L"job:77:log");
	log.SetArguments({ { L"id", L"77" } });
	const auto admitted = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(log, 5)));
	ASSERT_TRUE(admitted);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, admitted->Status());
	ASSERT_EQ(1U, fixture.ToolExecutor()->Started().size());
	EXPECT_EQ(L"jobLog", fixture.ToolExecutor()->Started().front().second.Operation());
}

TEST(ControlSenpBroker, RevokedAuthorityStopsAnAlreadyIssuedGrant)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	// Disabling the package is a control-owned decision; the live grant must
	// stop working on its very next use rather than at its expiry.
	fixture.ToolAuthority()->Set(Approved(false));
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantId), 3)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());
}

TEST(ControlSenpBroker, PollReturnsAnEmptySuccessUntilATerminalIsDrained)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	ControlSenpRpcRequest poll;
	poll.SetOperation(EControlSenpRpcOperation::PollRead);
	poll.SetProfileId(L"profile-1");
	poll.SetOwner(Owner());
	poll.SetGrantId(grantId);

	const auto empty = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(poll, 3)));
	ASSERT_TRUE(empty);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, empty->Status());
	EXPECT_FALSE(empty->HasCompletion());

	fixture.ToolExecutor()->SetPending(senp::effect::ToolCompleted{ L"issues:open:1",
		senp::effect::CompletionStatus::Succeeded, LR"({"items":[]})", L"" });
	const auto drained = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(poll, 4)));
	ASSERT_TRUE(drained);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, drained->Status());
	ASSERT_TRUE(drained->HasCompletion());
	EXPECT_EQ(L"issues:open:1", drained->Completion().readId);
	EXPECT_EQ(LR"({"items":[]})", drained->Completion().data);
}

TEST(ControlSenpBroker, RefusedResourceReadDiscardsThePartialChunk)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	ControlSenpRpcRequest read;
	read.SetOperation(EControlSenpRpcOperation::ReadResource);
	read.SetProfileId(L"profile-1");
	read.SetOwner(Owner());
	read.SetGrantId(grantId);
	read.SetResourceHandle(L"log-1");
	read.SetOffset(64);
	read.SetLength(16);

	fixture.ToolExecutor()->SetResourceStatus(EControlSenpRpcStatus::NotFound);
	const auto refused = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(read, 3)));
	ASSERT_TRUE(refused);
	EXPECT_EQ(EControlSenpRpcStatus::NotFound, refused->Status());
	EXPECT_TRUE(refused->ResourceBytes().empty());
	EXPECT_TRUE(refused->ResourceHandle().empty());
	// The rest of the chunk is part of that partial answer and must not survive
	// the refusal either: a length and a revision with no handle still describe
	// a resource, and the editor has one open to attribute them to.
	EXPECT_EQ(0U, refused->ResourceLength());
	EXPECT_EQ(0, refused->ResourceRevision());
	EXPECT_EQ(0U, refused->ResourceEnd());

	fixture.ToolExecutor()->SetResourceStatus(EControlSenpRpcStatus::Succeeded);
	const auto served = ReadResponse(session->HandleFrame(fixture.Connection(), RequestFrame(read, 4)));
	ASSERT_TRUE(served);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, served->Status());
	EXPECT_EQ(L"log-1", served->ResourceHandle());
	EXPECT_EQ(64U, served->ResourceOffset());
	EXPECT_EQ(16U, served->ResourceBytes().size());
	EXPECT_EQ(80U, served->ResourceLength());
	EXPECT_EQ(11, served->ResourceRevision());
	EXPECT_EQ(static_cast<std::uint8_t>(senp::TextResourceEnd::Complete), served->ResourceEnd());
}

TEST(ControlSenpBroker, DestroyingTheSessionCancelsItsExecutorScope)
{
	Fixture fixture;
	{
		auto session = fixture.Open();
		ASSERT_NE(nullptr, session);
		ASSERT_FALSE(fixture.Grant(*session).empty());
		EXPECT_TRUE(fixture.ToolExecutor()->CancelledConnections().empty());
	}
	// Teardown is by connection, exactly once, never once per minted scope.
	const auto& cancelled = fixture.ToolExecutor()->CancelledConnections();
	ASSERT_EQ(1U, cancelled.size());
	EXPECT_EQ(fixture.Connection().sessionId, cancelled.front().SessionId());
	EXPECT_EQ(fixture.Connection().clientProcessId, cancelled.front().ClientProcessId());
	// Closing the session must also give the record back to the registry.
	EXPECT_EQ(0U, fixture.Grants()->Size());
}

TEST(ControlSenpBroker, RejectsAFrameThatIsNotAWellFormedSenpPayload)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);

	auto frame = RequestFrame(IssueGrant());
	frame.payload.pop_back();
	const auto result = session->HandleFrame(fixture.Connection(), frame);
	ASSERT_EQ(1U, result.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, result.responseFrames.front().header.kind);
	const auto error = DecodeControlIpcError(result.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, error->status);

	ControlIpcFrame wrongTag = RequestFrame(IssueGrant(), 2);
	wrongTag.payload[0] = static_cast<std::uint8_t>(EControlIpcFieldTag::ProfilePayload);
	const auto tagged = session->HandleFrame(fixture.Connection(), wrongTag);
	ASSERT_EQ(1U, tagged.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, tagged.responseFrames.front().header.kind);
}

TEST(ControlSenpBroker, RefusesToOpenASessionForAnUnobservedPeer)
{
	Fixture fixture;
	EXPECT_EQ(nullptr, fixture.Broker().CreateSession({ 0, 1701 }));
	EXPECT_EQ(nullptr, fixture.Broker().CreateSession({ 31, 0 }));
}

TEST(ControlSenpBroker, WithoutAnExecutorTheToolBoundaryFailsExplicitly)
{
	auto authority = std::make_shared<Authority>(Approved());
	auto grants = std::make_shared<senp::CSenpToolGrants>(authority);
	CControlSenpBroker broker(grants, nullptr);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = broker.CreateSession(connection);
	ASSERT_NE(nullptr, session);

	const auto issued = ReadResponse(session->HandleFrame(connection, RequestFrame(IssueGrant())));
	ASSERT_TRUE(issued);
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, issued->Status());
	// An unconfigured tool boundary must report Unavailable rather than pretend
	// a read was admitted.
	const auto started = ReadResponse(
		session->HandleFrame(connection, RequestFrame(StartRead(issued->GrantId()), 2)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, started->Status());
}

TEST(ControlSenpBroker, IdleOwnerRecoversWithExpiredAfterAnotherOwnerRenews)
{
	Fixture fixture;
	fixture.ToolAuthority()->Set(ApprovedFor(L"other.github"));
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantA = fixture.Grant(*session);
	const auto grantB = fixture.Grant(*session, OwnerB());
	ASSERT_FALSE(grantA.empty());
	ASSERT_FALSE(grantB.empty());

	// Owner B goes idle past the lifetime while owner A renews. A's Issue
	// reclaims every expired record in the registry, so B's grant is gone even
	// though this same connection minted it.
	fixture.AdvancePastGrantLifetime();
	const auto renewedA = fixture.Grant(*session);
	ASSERT_FALSE(renewedA.empty());

	const auto stale = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantB, OwnerB()), 7)));
	ASSERT_TRUE(stale);
	// A grant this session issued that the registry dropped must answer
	// Expired, not InvalidRequest: the client forgets and re-issues on Expired
	// but keeps and reuses the dead grant on InvalidRequest forever.
	EXPECT_EQ(EControlSenpRpcStatus::Expired, stale->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());

	const auto grantB2 = fixture.Grant(*session, OwnerB());
	ASSERT_FALSE(grantB2.empty());
	const auto started = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantB2, OwnerB()), 8)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started->Status());
	EXPECT_EQ(1U, fixture.ToolExecutor()->Started().size());
}

TEST(ControlSenpBroker, IdleOwnerRecoversWithExpiredAcrossTwoConnections)
{
	Fixture fixture;
	fixture.ToolAuthority()->Set(ApprovedFor(L"other.github"));
	auto first = fixture.Open();
	ASSERT_NE(nullptr, first);
	const auto grantA = fixture.Grant(*first);
	ASSERT_FALSE(grantA.empty());

	fixture.SetConnection({ 32, 1702 });
	auto second = fixture.Open();
	ASSERT_NE(nullptr, second);
	const auto grantB = fixture.Grant(*second, OwnerB());
	ASSERT_FALSE(grantB.empty());

	fixture.AdvancePastGrantLifetime();
	// A renewal on the first connection reclaims B's registry record even
	// though B belongs to another pipe.
	fixture.SetConnection({ 31, 1701 });
	const auto renewedA = fixture.Grant(*first);
	ASSERT_FALSE(renewedA.empty());

	fixture.SetConnection({ 32, 1702 });
	const auto stale = ReadResponse(
		second->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantB, OwnerB()), 7)));
	ASSERT_TRUE(stale);
	EXPECT_EQ(EControlSenpRpcStatus::Expired, stale->Status());
	EXPECT_TRUE(fixture.ToolExecutor()->Started().empty());

	// A grant id the second connection never minted keeps answering
	// Unauthorized even after the first connection's record was pruned too:
	// being dead does not make another pipe's grant this session's to name.
	const auto foreign = ReadResponse(
		second->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantA), 8)));
	ASSERT_TRUE(foreign);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, foreign->Status());

	const auto grantB2 = fixture.Grant(*second, OwnerB());
	ASSERT_FALSE(grantB2.empty());
	const auto started = ReadResponse(
		second->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grantB2, OwnerB()), 9)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started->Status());
	EXPECT_EQ(1U, fixture.ToolExecutor()->Started().size());
}

TEST(ControlSenpBroker, AConnectionRenewsIndefinitelyPastTheTrackedGrantCap)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	// Issue, let the grant expire, repeat: expired records leave the session's
	// live set before the cap check, so renewal never exhausts it.
	for (std::uint64_t round = 0; round < 70; ++round) {
		const auto reply = ReadResponse(session->HandleFrame(
			fixture.Connection(), RequestFrame(IssueGrant(), round + 1)));
		ASSERT_TRUE(reply) << "round " << round;
		EXPECT_EQ(EControlSenpRpcStatus::Succeeded, reply->Status()) << "round " << round;
		fixture.AdvancePastGrantLifetime();
	}
}

TEST(ControlSenpBroker, ConcurrentlyLiveGrantsFillTheTrackedCapThenRefuse)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	std::vector<std::string> grants;
	for (std::size_t index = 0; index < CControlSenpBroker::MaximumTrackedGrants(); ++index) {
		const auto reply = ReadResponse(session->HandleFrame(
			fixture.Connection(), RequestFrame(IssueGrant(), index + 1)));
		ASSERT_TRUE(reply) << "grant " << index;
		ASSERT_EQ(EControlSenpRpcStatus::Succeeded, reply->Status()) << "grant " << index;
		grants.push_back(reply->GrantId());
	}
	const auto refused = ReadResponse(session->HandleFrame(
		fixture.Connection(), RequestFrame(IssueGrant(), 100)));
	ASSERT_TRUE(refused);
	EXPECT_EQ(EControlSenpRpcStatus::ResourceExhausted, refused->Status());
}

TEST(ControlSenpBroker, DestroyedSessionCancelsItsWholeConnectionOnceAfterPruning)
{
	Fixture fixture;
	fixture.ToolAuthority()->Set(ApprovedFor(L"other.github"));
	{
		auto session = fixture.Open();
		ASSERT_NE(nullptr, session);
		ASSERT_FALSE(fixture.Grant(*session).empty());
		ASSERT_FALSE(fixture.Grant(*session, OwnerB()).empty());
		fixture.AdvancePastGrantLifetime();
		// This renewal prunes both expired records; pruning is bookkeeping,
		// so nothing may be cancelled while the session still lives.
		ASSERT_FALSE(fixture.Grant(*session).empty());
		EXPECT_TRUE(fixture.ToolExecutor()->CancelledConnections().empty());
	}
	// Owner identities are reissued on every workspace-revision or account
	// change, so teardown cannot remember each scope: destruction makes
	// exactly one connection-wide cancel, and the executor owns finding the
	// scopes that belong to it.
	const auto& cancelled = fixture.ToolExecutor()->CancelledConnections();
	ASSERT_EQ(1U, cancelled.size());
	EXPECT_EQ(fixture.Connection().sessionId, cancelled.front().SessionId());
	EXPECT_EQ(fixture.Connection().clientProcessId, cancelled.front().ClientProcessId());
	EXPECT_EQ(0U, fixture.Grants()->Size());
}

TEST(ControlSenpBroker, RenewalPrunesTheOldRecordWithoutCancellingItsScope)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grant = fixture.Grant(*session);
	ASSERT_FALSE(grant.empty());
	fixture.AdvancePastGrantLifetime();
	const auto renewed = fixture.Grant(*session);
	ASSERT_FALSE(renewed.empty());
	// Pruning the old record is bookkeeping, not teardown: nothing may cancel
	// the scope while the session lives because the renewed grant shares it.
	EXPECT_TRUE(fixture.ToolExecutor()->CancelledConnections().empty());

	const auto stale = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grant), 3)));
	ASSERT_TRUE(stale);
	EXPECT_EQ(EControlSenpRpcStatus::Expired, stale->Status());
}

TEST(ControlSenpBroker, ARevokedGrantAnswersExpiredSoTheClientReissues)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grant = fixture.Grant(*session);
	ASSERT_FALSE(grant.empty());

	// Revocation also drops the record this session still tracks: same answer
	// as expiry, because the client's recovery path is the same re-issue.
	fixture.Grants()->RevokeOwner(L"profile-1", ToContributionOwner(Owner()));
	const auto stale = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grant), 3)));
	ASSERT_TRUE(stale);
	EXPECT_EQ(EControlSenpRpcStatus::Expired, stale->Status());
}

TEST(ControlSenpBroker, AnEvictedMintedGrantIdFallsBackToUnauthorized)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	// The minted-id history is bounded at four times the live-record cap: each
	// issue followed by expiry drops the record, so minting one more than the
	// bound evicts the oldest id.
	std::vector<std::string> grants;
	constexpr std::size_t kHistory = 4 * CControlSenpBroker::MaximumTrackedGrants();
	for (std::size_t index = 0; index <= kHistory; ++index) {
		const auto reply = ReadResponse(session->HandleFrame(
			fixture.Connection(), RequestFrame(IssueGrant(), index + 1)));
		ASSERT_TRUE(reply) << index;
		ASSERT_EQ(EControlSenpRpcStatus::Succeeded, reply->Status()) << index;
		grants.push_back(reply->GrantId());
		fixture.AdvancePastGrantLifetime();
	}
	// An id that fell out of the bounded history answers like a grant this
	// session never minted. That is safe: the editor treats Unauthorized
	// exactly like Expired here - forget the cached grant and re-issue.
	const auto evicted = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grants.front()), 1000)));
	ASSERT_TRUE(evicted);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, evicted->Status());

	// The most recent dead id is still remembered and answers Expired.
	const auto latest = ReadResponse(
		session->HandleFrame(fixture.Connection(), RequestFrame(StartRead(grants.back()), 1001)));
	ASSERT_TRUE(latest);
	EXPECT_EQ(EControlSenpRpcStatus::Expired, latest->Status());
}

} // namespace
} // namespace platform::controlipc
