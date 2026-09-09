/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include "workbench/editor/SenpControlToolReads.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include "senp/SenpRuntimeSession.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::editor {
namespace {

using platform::controlipc::ControlIpcFrame;
using platform::controlipc::ControlSenpRpcRequest;
using platform::controlipc::ControlSenpRpcResponse;
using platform::controlipc::EControlSenpRpcOperation;
using platform::controlipc::EControlSenpRpcStatus;

constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kAuthorityId[] = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kGeneration = 7;
constexpr wchar_t kSenpProfile[] = L"github-profile";
constexpr wchar_t kExtension[] = L"sakura.github-pull-requests";

senp::ContributionOwnerIdentity Owner()
{
	return { kExtension, std::wstring(64, L'a'), 7, 11, 13 };
}

senp::ContributionOwnerIdentity OtherOwner()
{
	return { L"sakura.github-actions", std::wstring(64, L'b'), 9, 11, 13 };
}

senp::effect::OperationContext Context(std::wstring operationId, std::int64_t requestGeneration)
{
	return { std::move(operationId), 7, 11, 13, requestGeneration };
}

senp::effect::StartToolRead Read(std::wstring readId)
{
	return { std::move(readId), L"github", L"repositoryRead",
		{ senp::effect::Field{ L"repository", L"owner/project" } } };
}

platform::controlipc::ControlPlatformEndpointSnapshot Endpoint()
{
	return { ::GetCurrentProcessId(), kGeneration,
		platform::controlipc::ControlPlatformEndpointLifecycle::Accepting, kProfileHash,
		platform::controlipc::BuildControlPipeName(kProfileHash), kAuthorityId };
}

/*!
	@brief In-memory stand-in for the control-owned broker.

	It answers by decoding the request rather than by replaying a fixed script,
	because the seam decides for itself how often it polls. Every counter is read
	from the test thread while the seam's worker writes it, so all of it lives
	under one mutex.
*/
struct Broker {
	mutable std::mutex mutex;
	int hello = 0;
	int issued = 0;
	int started = 0;
	int polled = 0;
	int cancelled = 0;
	std::vector<ControlSenpRpcRequest> requests;
	std::vector<std::wstring> startedReads;
	std::vector<std::wstring> cancelledReads;
	//! The scope each read was admitted under. The real broker routes a terminal
	//! by scope, so this stand-in must not hand one scope another scope's read.
	std::map<std::wstring, platform::controlipc::ControlSenpRpcOwner> readOwner;
	//! Terminals the broker hands back, one per poll of the owning scope.
	std::deque<senp::effect::ToolCompleted> ready;
	EControlSenpRpcStatus issueStatus = EControlSenpRpcStatus::Succeeded;
	EControlSenpRpcStatus startStatus = EControlSenpRpcStatus::Succeeded;
	//! Transport failure rather than a broker answer.
	bool severed = false;

	[[nodiscard]] int Started() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return started;
	}
	[[nodiscard]] int Cancelled() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return cancelled;
	}
	[[nodiscard]] int Issued() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return issued;
	}
	[[nodiscard]] int Hello() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return hello;
	}
	void Sever()
	{
		std::lock_guard<std::mutex> lock(mutex);
		severed = true;
	}
	void Publish(senp::effect::ToolCompleted completion)
	{
		std::lock_guard<std::mutex> lock(mutex);
		ready.push_back(std::move(completion));
	}
	[[nodiscard]] std::vector<ControlSenpRpcRequest> Requests() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return requests;
	}
	[[nodiscard]] std::vector<std::wstring> Cancellations() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return cancelledReads;
	}
};

ControlIpcFrame Answer(const ControlIpcFrame& request, platform::controlipc::EControlIpcKind kind,
	std::vector<std::uint8_t> payload)
{
	return { { platform::controlipc::kControlIpcMajorVersion,
		platform::controlipc::kControlIpcMinorVersion, kind,
		platform::controlipc::EControlIpcFlags::Response | platform::controlipc::EControlIpcFlags::Terminal,
		request.header.requestId, kGeneration }, std::move(payload) };
}

class CBrokerChannel final : public platform::controlipc::IControlPlatformClientChannel {
public:
	explicit CBrokerChannel(std::shared_ptr<Broker> broker) : m_broker(std::move(broker)) {}

	platform::controlipc::ControlIpcTransportResult Connect(
		const platform::controlipc::ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		return { true, platform::controlipc::EControlIpcTransportDisconnectReason::None, 0, L"" };
	}

	platform::controlipc::ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds) override
	{
		std::lock_guard<std::mutex> lock(m_broker->mutex);
		if (m_broker->severed) {
			return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0,
				L"severed" };
		}
		if (request.header.kind == platform::controlipc::EControlIpcKind::Hello) {
			++m_broker->hello;
			auto hello = platform::controlipc::EncodeControlStorageHello(kAuthorityId);
			if (!hello) return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0, L"" };
			responses.push_back(Answer(request, platform::controlipc::EControlIpcKind::HelloAck,
				std::move(*hello)));
			return { true, platform::controlipc::EControlIpcTransportDisconnectReason::None, 0, L"" };
		}
		auto fields = platform::controlipc::DecodeControlIpcFields(request.payload);
		if (fields.outcome != platform::controlipc::EControlIpcFieldDecodeOutcome::Decoded
			|| fields.fields.size() != 1) {
			return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0, L"" };
		}
		auto decoded = platform::controlipc::DecodeControlSenpRpcRequest(fields.fields.front().value);
		if (!decoded) {
			return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0, L"" };
		}
		m_broker->requests.push_back(*decoded);
		const auto response = Handle(*decoded);
		auto payload = platform::controlipc::EncodeControlSenpRpcResponse(response);
		if (!payload) return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0, L"" };
		auto outer = platform::controlipc::EncodeControlIpcFields(
			{ { static_cast<std::uint16_t>(platform::controlipc::EControlIpcFieldTag::SenpPayload),
				std::move(*payload) } });
		if (!outer) return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0, L"" };
		responses.push_back(Answer(request, platform::controlipc::EControlIpcKind::SenpResponse,
			std::move(*outer)));
		return { true, platform::controlipc::EControlIpcTransportDisconnectReason::None, 0, L"" };
	}

	void Close() noexcept override {}

private:
	//! Requires the broker mutex, which Exchange already holds.
	ControlSenpRpcResponse Handle(const ControlSenpRpcRequest& request) const
	{
		ControlSenpRpcResponse response;
		switch (request.operation) {
		case EControlSenpRpcOperation::IssueGrant:
			++m_broker->issued;
			response.status = m_broker->issueStatus;
			if (response.status == EControlSenpRpcStatus::Succeeded) {
				response.grantId = "grant-" + std::to_string(m_broker->issued);
				response.expiresAtMilliseconds = 10000;
			}
			return response;
		case EControlSenpRpcOperation::StartRead:
			++m_broker->started;
			response.status = m_broker->startStatus;
			if (response.status == EControlSenpRpcStatus::Succeeded) {
				m_broker->startedReads.push_back(request.readId);
				m_broker->readOwner[request.readId] = request.owner;
			}
			return response;
		case EControlSenpRpcOperation::PollRead:
			++m_broker->polled;
			response.status = EControlSenpRpcStatus::Succeeded;
			for (auto current = m_broker->ready.begin(); current != m_broker->ready.end(); ++current) {
				const auto scope = m_broker->readOwner.find(current->readId);
				if (scope == m_broker->readOwner.end() || !(scope->second == request.owner)) continue;
				response.hasCompletion = true;
				response.completion = *current;
				m_broker->ready.erase(current);
				break;
			}
			return response;
		case EControlSenpRpcOperation::CancelRead:
			++m_broker->cancelled;
			m_broker->cancelledReads.push_back(request.readId);
			response.status = EControlSenpRpcStatus::Succeeded;
			return response;
		default:
			response.status = EControlSenpRpcStatus::InvalidRequest;
			return response;
		}
	}

	std::shared_ptr<Broker> m_broker;
};

class CFixedEndpointReader final : public platform::controlipc::IControlPlatformEndpointReader {
public:
	std::optional<platform::controlipc::ControlPlatformEndpointSnapshot> Read(
		const platform::controlipc::ControlPlatformEndpointReadRequirements&) override
	{
		return endpoint;
	}

	std::optional<platform::controlipc::ControlPlatformEndpointSnapshot> endpoint = Endpoint();
};

/*!
	@brief Endpoint reader that holds the worker inside discovery until released.

	A bound is only observable while nothing drains it, so these tests park the
	worker on its first connect instead of racing it. Release must run before the
	seam is destroyed, because Stop joins that worker.
*/
class CGatedEndpointReader final : public platform::controlipc::IControlPlatformEndpointReader {
public:
	std::optional<platform::controlipc::ControlPlatformEndpointSnapshot> Read(
		const platform::controlipc::ControlPlatformEndpointReadRequirements&) override
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_gate.wait(lock, [this] { return m_released; });
		return std::nullopt;
	}

	void Release()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_released = true;
		}
		m_gate.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_gate;
	bool m_released = false;
};

SenpControlToolReadsOptions Options(std::shared_ptr<Broker> broker)
{
	SenpControlToolReadsOptions options;
	options.authorityProfileId = kAuthorityId;
	options.authorityProfileHash = kProfileHash;
	options.senpProfileId = kSenpProfile;
	options.pollInterval = std::chrono::milliseconds(1);
	options.exchangeDeadline = std::chrono::milliseconds(200);
	options.channelFactory = [broker = std::move(broker)] {
		return std::make_unique<CBrokerChannel>(broker);
	};
	return options;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return predicate();
}

constexpr std::chrono::milliseconds kSettle{ 5000 };

} // namespace

TEST(SenpControlToolReads, CarriesOneReadFromTheOwnerScopeToTheBrokerAndBack)
{
	auto broker = std::make_shared<Broker>();
	broker->Publish({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]", L"" });
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	EXPECT_EQ(ESenpControlToolReadsState::Idle, reads.State());
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto completion = reads.Take(Owner());
	ASSERT_TRUE(completion);
	EXPECT_EQ(L"issues:open:1", completion->readId);
	EXPECT_EQ(senp::effect::CompletionStatus::Succeeded, completion->status);
	EXPECT_EQ(L"[1]", completion->data);
	EXPECT_FALSE(reads.Take(Owner()));
	EXPECT_EQ(0U, reads.OutstandingReads());
	EXPECT_EQ(ESenpControlToolReadsState::Connected, reads.State());
	EXPECT_EQ(1u, reads.ConnectionEpoch());

	// One connection, one grant, and a StartRead that names only what the closed
	// operation set admits.
	EXPECT_EQ(1, broker->Hello());
	EXPECT_EQ(1, broker->Issued());
	EXPECT_EQ(1, broker->Started());
	const auto requests = broker->Requests();
	ASSERT_GE(requests.size(), 2u);
	EXPECT_EQ(EControlSenpRpcOperation::IssueGrant, requests[0].operation);
	EXPECT_EQ(kSenpProfile, requests[0].profileId);
	EXPECT_EQ(platform::controlipc::FromContributionOwner(Owner()), requests[0].owner);
	EXPECT_EQ(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead),
		requests[0].capabilities);
	EXPECT_EQ(EControlSenpRpcOperation::StartRead, requests[1].operation);
	EXPECT_EQ("grant-1", requests[1].grantId);
	EXPECT_EQ(L"issues:open:1", requests[1].readId);
	EXPECT_EQ(L"github", requests[1].toolId);
	EXPECT_EQ(L"repositoryRead", requests[1].toolOperation);
	ASSERT_EQ(1u, requests[1].arguments.size());
	EXPECT_EQ(L"repository", requests[1].arguments[0].name);
	EXPECT_EQ(L"owner/project", requests[1].arguments[0].value);
}

TEST(SenpControlToolReads, KeepsOneGrantPerOwnerScopeAcrossReads)
{
	auto broker = std::make_shared<Broker>();
	broker->Publish({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]", L"" });
	broker->Publish({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]", L"" });
	broker->Publish({ L"runs:1", senp::effect::CompletionStatus::Succeeded, L"[2]", L"" });
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"comments:1")));
	ASSERT_TRUE(reads.Start(OtherOwner(), Context(L"tool.3", 1), Read(L"runs:1")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	// Every terminal is queued under the owner scope that asked for it.
	const auto first = reads.Take(Owner());
	const auto second = reads.Take(Owner());
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_NE(first->readId, second->readId);
	EXPECT_FALSE(reads.Take(Owner()));
	const auto other = reads.Take(OtherOwner());
	ASSERT_TRUE(other);
	EXPECT_EQ(L"runs:1", other->readId);

	// One connection for both scopes, one grant each - a grant is per owner, not
	// per read.
	EXPECT_EQ(1, broker->Hello());
	EXPECT_EQ(2, broker->Issued());
	EXPECT_EQ(3, broker->Started());
}

TEST(SenpControlToolReads, FailsAReadTheControlSideKeepsRefusing)
{
	auto broker = std::make_shared<Broker>();
	broker->startStatus = EControlSenpRpcStatus::Unauthorized;
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto completion = reads.Take(Owner());
	ASSERT_TRUE(completion);
	EXPECT_EQ(L"issues:open:1", completion->readId);
	EXPECT_EQ(senp::effect::CompletionStatus::Failed, completion->status);
	EXPECT_FALSE(completion->message.empty());
	// A refusal may mean an expired record, so the grant is re-minted - but the
	// retry is bounded and the read ends rather than looping.
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::kMaximumAttempts), broker->Started());
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::kMaximumAttempts), broker->Issued());
	EXPECT_EQ(0U, reads.OutstandingReads());
}

TEST(SenpControlToolReads, FailsClosedWhenNoControlEndpointIsPublished)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	reader.endpoint.reset();
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto completion = reads.Take(Owner());
	ASSERT_TRUE(completion);
	EXPECT_EQ(senp::effect::CompletionStatus::HostUnavailable, completion->status);
	EXPECT_FALSE(completion->message.empty());
	EXPECT_EQ(0, broker->Hello());
	EXPECT_EQ(0, broker->Started());
	EXPECT_EQ(0u, reads.ConnectionEpoch());
}

TEST(SenpControlToolReads, FailsEveryOutstandingReadWhenTheConnectionIsLost)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"comments:1")));
	ASSERT_TRUE(WaitUntil([&] { return broker->Started() == 2; }));
	// The grants were bound to that connection, so nothing they named survives.
	broker->Sever();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto first = reads.Take(Owner());
	const auto second = reads.Take(Owner());
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_EQ(senp::effect::CompletionStatus::HostUnavailable, first->status);
	EXPECT_EQ(senp::effect::CompletionStatus::HostUnavailable, second->status);
	EXPECT_EQ(0U, reads.OutstandingReads());
	EXPECT_EQ(ESenpControlToolReadsState::Disconnected, reads.State());
}

TEST(SenpControlToolReads, CancelsTheDispatchedLineageOnTheWireAndKeepsItsTerminalOut)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"comments:1")));
	ASSERT_TRUE(WaitUntil([&] { return broker->Started() == 2; }));

	reads.Cancel(Owner(), Context(L"tool.1", 1));
	ASSERT_TRUE(WaitUntil([&] { return broker->Cancelled() == 1; }));
	EXPECT_EQ(std::vector<std::wstring>{ L"issues:open:1" }, broker->Cancellations());
	EXPECT_EQ(1U, reads.OutstandingReads());

	// A terminal the executor had already produced for the cancelled read must
	// never reach the owner scope again.
	broker->Publish({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]", L"" });
	broker->Publish({ L"comments:1", senp::effect::CompletionStatus::Succeeded, L"[]", L"" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	const auto completion = reads.Take(Owner());
	ASSERT_TRUE(completion);
	EXPECT_EQ(L"comments:1", completion->readId);
	EXPECT_FALSE(reads.Take(Owner()));
}

TEST(SenpControlToolReads, RevocationCancelsEveryReadOfTheScopeAndRetiresIt)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"comments:1")));
	ASSERT_TRUE(WaitUntil([&] { return broker->Started() == 2; }));

	broker->Publish({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]", L"" });
	reads.CancelAll(Owner());
	EXPECT_EQ(0U, reads.OutstandingReads());
	EXPECT_FALSE(reads.Take(Owner()));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(2, broker->Cancelled());
	EXPECT_FALSE(reads.Take(Owner()));
}

TEST(SenpControlToolReads, RefusesEveryReadItCannotAccountFor)
{
	auto broker = std::make_shared<Broker>();
	CGatedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);
	const auto context = Context(L"tool.1", 1);

	EXPECT_FALSE(reads.Start(Owner(), context, Read(L"")));
	EXPECT_FALSE(reads.Start(Owner(), context, { L"issues:open:1", L"", L"repositoryRead", {} }));
	EXPECT_FALSE(reads.Start(Owner(), context, { L"issues:open:1", L"github", L"", {} }));
	// The broker's coherence rules reject a generation of zero, so the boundary
	// refuses here instead of putting an unencodable request on the wire.
	senp::ContributionOwnerIdentity ungeneration = Owner();
	ungeneration.generation = 0;
	EXPECT_FALSE(reads.Start(ungeneration, context, Read(L"issues:open:1")));
	EXPECT_EQ(0U, reads.OutstandingReads());

	ASSERT_TRUE(reads.Start(Owner(), context, Read(L"issues:open:1")));
	EXPECT_FALSE(reads.Start(Owner(), context, Read(L"issues:open:1")));

	reader.Release();
	reads.Stop();
	EXPECT_EQ(ESenpControlToolReadsState::Stopped, reads.State());
	EXPECT_FALSE(reads.Start(Owner(), context, Read(L"issues:open:2")));
	EXPECT_FALSE(reads.Take(Owner()));
	EXPECT_EQ(0U, reads.OutstandingReads());
}

TEST(SenpControlToolReads, BoundsTheOwnerScopesAndReadsItWillHold)
{
	auto broker = std::make_shared<Broker>();
	CGatedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);
	const auto context = Context(L"tool.1", 1);

	for (std::size_t index = 0; index < senp::CSenpRuntimeSession::kMaximumPending; ++index) {
		ASSERT_TRUE(reads.Start(Owner(), context, Read(L"issues:bulk:" + std::to_wstring(index))));
	}
	EXPECT_FALSE(reads.Start(Owner(), context, Read(L"issues:overflow")));

	for (std::size_t index = 1; index < CSenpControlToolReads::kMaximumOwners; ++index) {
		senp::ContributionOwnerIdentity owner = Owner();
		owner.generation = static_cast<std::int64_t>(index) + 100;
		ASSERT_TRUE(reads.Start(owner, Context(L"tool.1", 1), Read(L"issues:open:1")));
	}
	senp::ContributionOwnerIdentity overflow = Owner();
	overflow.generation = 999;
	EXPECT_FALSE(reads.Start(overflow, Context(L"tool.1", 1), Read(L"issues:open:1")));
	EXPECT_EQ(senp::CSenpRuntimeSession::kMaximumPending + CSenpControlToolReads::kMaximumOwners - 1,
		reads.OutstandingReads());
	reader.Release();
}

} // namespace workbench::editor
