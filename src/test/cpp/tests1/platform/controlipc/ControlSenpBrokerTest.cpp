/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "platform/controlipc/ControlSenpBroker.h"

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

class Authority final : public senp::ISenpToolGrantAuthority {
public:
	explicit Authority(senp::SenpApprovedToolOwner value) : m_value(std::move(value)) {}
	std::optional<senp::SenpApprovedToolOwner> Resolve(std::wstring_view, std::wstring_view) const override
	{
		return m_value;
	}
	void Set(senp::SenpApprovedToolOwner value) { m_value = std::move(value); }
private:
	senp::SenpApprovedToolOwner m_value;
};

//! Records what the broker admitted. It never blocks, mirroring the contract
//! that frame processing must not wait on an external process.
class Executor final : public ISenpToolExecutor {
public:
	EControlSenpRpcStatus StartRead(const SenpToolExecutionScope& scope,
		const SenpToolReadCommand& command) noexcept override
	{
		started.push_back({ scope, command });
		return next;
	}
	std::optional<senp::effect::ToolCompleted> TakeCompleted(
		const SenpToolExecutionScope& scope) noexcept override
	{
		polled.push_back(scope);
		if (!pending) return {};
		auto value = *pending;
		pending.reset();
		return value;
	}
	void CancelRead(const SenpToolExecutionScope& scope, std::wstring_view readId) noexcept override
	{
		cancelledReads.emplace_back(scope, std::wstring(readId));
	}
	void CancelScope(const SenpToolExecutionScope& scope) noexcept override
	{
		cancelledScopes.push_back(scope);
	}
	EControlSenpRpcStatus ReadResource(const SenpToolExecutionScope&, std::wstring_view handle,
		std::uint64_t offset, std::uint32_t length, ControlSenpRpcResponse& response) noexcept override
	{
		// A refused read still writes, so the broker must discard a partial answer.
		response.resourceHandle = std::wstring(handle);
		response.resourceOffset = offset;
		response.resourceBytes.assign(length, 'x');
		response.resourceFinal = true;
		return resourceStatus;
	}
	void ReleaseResource(const SenpToolExecutionScope&, std::wstring_view handle) noexcept override
	{
		released.push_back(std::wstring(handle));
	}
	EControlSenpRpcStatus QueryAccount(std::wstring_view profileId,
		ControlSenpRpcResponse& response) noexcept override
	{
		accountQueries.push_back(std::wstring(profileId));
		// A refused query still writes, so the broker must discard a partial answer.
		response.accountGeneration = accountGeneration;
		response.accountState = accountState;
		return accountStatus;
	}

	EControlSenpRpcStatus next = EControlSenpRpcStatus::Succeeded;
	EControlSenpRpcStatus resourceStatus = EControlSenpRpcStatus::Succeeded;
	std::optional<senp::effect::ToolCompleted> pending;
	std::vector<std::pair<SenpToolExecutionScope, SenpToolReadCommand>> started;
	std::vector<SenpToolExecutionScope> polled;
	std::vector<std::pair<SenpToolExecutionScope, std::wstring>> cancelledReads;
	std::vector<SenpToolExecutionScope> cancelledScopes;
	std::vector<std::wstring> released;
	EControlSenpRpcStatus accountStatus = EControlSenpRpcStatus::Succeeded;
	std::int64_t accountGeneration = 0;
	EControlSenpAccountState accountState = EControlSenpAccountState::Unknown;
	std::vector<std::wstring> accountQueries;
};

ControlSenpRpcOwner Owner()
{
	return { L"sample.github", kDigest, 7, 11, 13 };
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

ControlSenpRpcRequest IssueGrant()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::IssueGrant;
	request.profileId = L"profile-1";
	request.owner = Owner();
	request.capabilities = static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead);
	return request;
}

ControlSenpRpcRequest QueryAccount()
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::QueryAccount;
	request.profileId = L"profile-1";
	return request;
}

ControlSenpRpcRequest StartRead(std::string grantId)
{
	ControlSenpRpcRequest request;
	request.operation = EControlSenpRpcOperation::StartRead;
	request.profileId = L"profile-1";
	request.owner = Owner();
	request.grantId = std::move(grantId);
	request.readId = L"issues:open:1";
	request.toolId = L"github";
	request.toolOperation = L"repositoryRead";
	request.arguments = { { L"path", L"issues" } };
	return request;
}

struct Fixture {
	std::shared_ptr<Authority> authority = std::make_shared<Authority>(Approved());
	std::shared_ptr<senp::CSenpToolGrants> grants = std::make_shared<senp::CSenpToolGrants>(authority);
	std::shared_ptr<Executor> executor = std::make_shared<Executor>();
	CControlSenpBroker broker{ grants, executor };
	ControlIpcSessionContext connection{ 31, 1701 };

	std::unique_ptr<IControlIpcSessionHandler> Open() { return broker.CreateSession(connection); }

	std::string Grant(IControlIpcSessionHandler& session)
	{
		const auto reply = ReadResponse(session.HandleFrame(connection, RequestFrame(IssueGrant())));
		return reply && reply->status == EControlSenpRpcStatus::Succeeded ? reply->grantId : std::string{};
	}
};

TEST(ControlSenpBroker, IssuesConnectionBoundGrantAndAdmitsOneRead)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto issued = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(IssueGrant())));
	ASSERT_TRUE(issued);
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, issued->status);
	EXPECT_EQ(32U, issued->grantId.size());
	// A steady_clock origin is meaningless across processes, so the wire must
	// carry a remaining lifetime rather than an absolute instant.
	EXPECT_GT(issued->expiresAtMilliseconds, 0U);
	EXPECT_LE(issued->expiresAtMilliseconds,
		static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			senp::CSenpToolGrants::GrantLifetime()).count()));

	const auto started = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(StartRead(issued->grantId), 2)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, started->status);
	ASSERT_EQ(1U, fixture.executor->started.size());
	const auto& scope = fixture.executor->started.front().first;
	EXPECT_EQ(fixture.connection.sessionId, scope.sessionId);
	EXPECT_EQ(fixture.connection.clientProcessId, scope.clientProcessId);
	EXPECT_EQ(L"profile-1", scope.profileId);
	EXPECT_EQ(L"sample.github", scope.owner.extensionId);
	EXPECT_EQ(7, scope.owner.generation);
	EXPECT_EQ(L"issues:open:1", fixture.executor->started.front().second.readId);
}

TEST(ControlSenpBroker, AnswersTheAccountQueryWithoutAGrantOrAnOwner)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	fixture.executor->accountGeneration = 5;
	fixture.executor->accountState = EControlSenpAccountState::Connected;
	// No IssueGrant precedes it on purpose: the editor cannot build the owner a
	// grant is scoped by until it has been told which account generation to use.
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(QueryAccount())));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, reply->status);
	EXPECT_EQ(5, reply->accountGeneration);
	EXPECT_EQ(EControlSenpAccountState::Connected, reply->accountState);
	ASSERT_EQ(1U, fixture.executor->accountQueries.size());
	EXPECT_EQ(L"profile-1", fixture.executor->accountQueries.front());
	EXPECT_TRUE(fixture.executor->started.empty());
}

TEST(ControlSenpBroker, RefusesAnAccountQueryOutsideTheProfileIdentitySpace)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	auto request = QueryAccount();
	request.profileId = L"../other";
	// Nothing owner-scoped is rechecked for this operation, so admission is the
	// whole check: the id must belong to the space every grant is scoped by.
	const auto reply = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(request)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::InvalidRequest, reply->status);
	EXPECT_TRUE(fixture.executor->accountQueries.empty());
}

TEST(ControlSenpBroker, DiscardsAPartialAccountAnswerWhenTheQueryIsRefused)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	fixture.executor->accountGeneration = 5;
	fixture.executor->accountState = EControlSenpAccountState::Connected;
	fixture.executor->accountStatus = EControlSenpRpcStatus::Unavailable;
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(QueryAccount())));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, reply->status);
	EXPECT_EQ(0, reply->accountGeneration);
	EXPECT_EQ(EControlSenpAccountState::Unknown, reply->accountState);
}

TEST(ControlSenpBroker, RefusesEveryOperationThatNamesAnUnknownGrant)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(StartRead("not-a-grant"))));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->status);
	EXPECT_TRUE(fixture.executor->started.empty());
}

TEST(ControlSenpBroker, RefusesAGrantMintedOnAnotherConnection)
{
	Fixture fixture;
	auto first = fixture.Open();
	ASSERT_NE(nullptr, first);
	const auto grantId = fixture.Grant(*first);
	ASSERT_FALSE(grantId.empty());

	fixture.connection = { 32, 1702 };
	auto second = fixture.Open();
	ASSERT_NE(nullptr, second);
	// The record exists in the registry but belongs to a different pipe, so the
	// second connection must not be able to name it.
	const auto reply = ReadResponse(
		second->HandleFrame(fixture.connection, RequestFrame(StartRead(grantId))));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->status);
	EXPECT_TRUE(fixture.executor->started.empty());
}

TEST(ControlSenpBroker, RefusesAReadWhoseOwnerDoesNotMatchTheGrantScope)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	auto request = StartRead(grantId);
	request.owner.generation = 8;
	const auto reply = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(request, 3)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->status);
	EXPECT_TRUE(fixture.executor->started.empty());
}

TEST(ControlSenpBroker, RefusesAToolOperationOutsideTheClosedSet)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	auto foreignTool = StartRead(grantId);
	foreignTool.toolId = L"shell";
	const auto tool = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(foreignTool, 3)));
	ASSERT_TRUE(tool);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, tool->status);

	auto foreignOperation = StartRead(grantId);
	foreignOperation.toolOperation = L"repositoryWrite";
	const auto operation = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(foreignOperation, 4)));
	ASSERT_TRUE(operation);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, operation->status);
	EXPECT_TRUE(fixture.executor->started.empty());
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
	fixture.authority->Set(Approved(false));
	const auto reply = ReadResponse(
		session->HandleFrame(fixture.connection, RequestFrame(StartRead(grantId), 3)));
	ASSERT_TRUE(reply);
	EXPECT_EQ(EControlSenpRpcStatus::Unauthorized, reply->status);
	EXPECT_TRUE(fixture.executor->started.empty());
}

TEST(ControlSenpBroker, PollReturnsAnEmptySuccessUntilATerminalIsDrained)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	ControlSenpRpcRequest poll;
	poll.operation = EControlSenpRpcOperation::PollRead;
	poll.profileId = L"profile-1";
	poll.owner = Owner();
	poll.grantId = grantId;

	const auto empty = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(poll, 3)));
	ASSERT_TRUE(empty);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, empty->status);
	EXPECT_FALSE(empty->hasCompletion);

	fixture.executor->pending = senp::effect::ToolCompleted{ L"issues:open:1",
		senp::effect::CompletionStatus::Succeeded, LR"({"items":[]})", L"" };
	const auto drained = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(poll, 4)));
	ASSERT_TRUE(drained);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, drained->status);
	ASSERT_TRUE(drained->hasCompletion);
	EXPECT_EQ(L"issues:open:1", drained->completion.readId);
	EXPECT_EQ(LR"({"items":[]})", drained->completion.data);
}

TEST(ControlSenpBroker, RefusedResourceReadDiscardsThePartialChunk)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);
	const auto grantId = fixture.Grant(*session);
	ASSERT_FALSE(grantId.empty());

	ControlSenpRpcRequest read;
	read.operation = EControlSenpRpcOperation::ReadResource;
	read.profileId = L"profile-1";
	read.owner = Owner();
	read.grantId = grantId;
	read.resourceHandle = L"log-1";
	read.offset = 64;
	read.length = 16;

	fixture.executor->resourceStatus = EControlSenpRpcStatus::NotFound;
	const auto refused = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(read, 3)));
	ASSERT_TRUE(refused);
	EXPECT_EQ(EControlSenpRpcStatus::NotFound, refused->status);
	EXPECT_TRUE(refused->resourceBytes.empty());
	EXPECT_TRUE(refused->resourceHandle.empty());

	fixture.executor->resourceStatus = EControlSenpRpcStatus::Succeeded;
	const auto served = ReadResponse(session->HandleFrame(fixture.connection, RequestFrame(read, 4)));
	ASSERT_TRUE(served);
	EXPECT_EQ(EControlSenpRpcStatus::Succeeded, served->status);
	EXPECT_EQ(L"log-1", served->resourceHandle);
	EXPECT_EQ(64U, served->resourceOffset);
	EXPECT_EQ(16U, served->resourceBytes.size());
}

TEST(ControlSenpBroker, DestroyingTheSessionCancelsItsExecutorScope)
{
	Fixture fixture;
	{
		auto session = fixture.Open();
		ASSERT_NE(nullptr, session);
		ASSERT_FALSE(fixture.Grant(*session).empty());
		EXPECT_TRUE(fixture.executor->cancelledScopes.empty());
	}
	ASSERT_EQ(1U, fixture.executor->cancelledScopes.size());
	EXPECT_EQ(fixture.connection.sessionId, fixture.executor->cancelledScopes.front().sessionId);
	EXPECT_EQ(L"profile-1", fixture.executor->cancelledScopes.front().profileId);
	// Closing the session must also give the record back to the registry.
	EXPECT_EQ(0U, fixture.grants->Size());
}

TEST(ControlSenpBroker, RejectsAFrameThatIsNotAWellFormedSenpPayload)
{
	Fixture fixture;
	auto session = fixture.Open();
	ASSERT_NE(nullptr, session);

	auto frame = RequestFrame(IssueGrant());
	frame.payload.pop_back();
	const auto result = session->HandleFrame(fixture.connection, frame);
	ASSERT_EQ(1U, result.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, result.responseFrames.front().header.kind);
	const auto error = DecodeControlIpcError(result.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, error->status);

	ControlIpcFrame wrongTag = RequestFrame(IssueGrant(), 2);
	wrongTag.payload[0] = static_cast<std::uint8_t>(EControlIpcFieldTag::ProfilePayload);
	const auto tagged = session->HandleFrame(fixture.connection, wrongTag);
	ASSERT_EQ(1U, tagged.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::Error, tagged.responseFrames.front().header.kind);
}

TEST(ControlSenpBroker, RefusesToOpenASessionForAnUnobservedPeer)
{
	Fixture fixture;
	EXPECT_EQ(nullptr, fixture.broker.CreateSession({ 0, 1701 }));
	EXPECT_EQ(nullptr, fixture.broker.CreateSession({ 31, 0 }));
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
	ASSERT_EQ(EControlSenpRpcStatus::Succeeded, issued->status);
	// An unconfigured tool boundary must report Unavailable rather than pretend
	// a read was admitted.
	const auto started = ReadResponse(
		session->HandleFrame(connection, RequestFrame(StartRead(issued->grantId), 2)));
	ASSERT_TRUE(started);
	EXPECT_EQ(EControlSenpRpcStatus::Unavailable, started->status);
}

} // namespace
} // namespace platform::controlipc
