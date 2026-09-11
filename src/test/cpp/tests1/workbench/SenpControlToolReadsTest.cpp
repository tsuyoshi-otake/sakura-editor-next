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
#include "senp/SenpTextResource.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::editor {
namespace {

using platform::controlipc::ControlIpcFrame;
using platform::controlipc::ControlSenpRpcRequest;
using platform::controlipc::ControlSenpRpcResponse;
using platform::controlipc::EControlSenpAccountState;
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
	from the test thread while the seam's worker writes it, so the dispatch logic
	that mutates them lives here too, behind one lock this class owns end to end -
	`CBrokerChannel` never reaches into this state directly, it only decodes a
	wire frame and asks this broker to dispatch it.

	The guarded state sits behind a `unique_ptr` rather than a `mutable` member so
	the query methods below can lock it from a `const` method without needing the
	`mutable` keyword: constness on `m_state` only pins the pointer, not what it
	points to, exactly like a pimpl.
*/
class Broker {
public:
	[[nodiscard]] int Started() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->started;
	}
	[[nodiscard]] int Cancelled() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->cancelled;
	}
	[[nodiscard]] int Issued() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->issued;
	}
	[[nodiscard]] int Hello() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->hello;
	}
	[[nodiscard]] int Accounts() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->accounts;
	}
	[[nodiscard]] int Adopted() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->adopted;
	}
	[[nodiscard]] int ResourceReads() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->resourceReads;
	}
	[[nodiscard]] int Releases() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->releases;
	}
	[[nodiscard]] std::vector<std::wstring> Released() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->releasedResources;
	}
	[[nodiscard]] std::vector<ControlSenpRpcRequest> Requests() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->requests;
	}
	[[nodiscard]] std::vector<std::wstring> Cancellations() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->cancelledReads;
	}

	void Sever()
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->severed = true;
	}
	void Restore()
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->severed = false;
	}
	void Publish(senp::effect::ToolCompleted completion)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->ready.push_back(std::move(completion));
	}

	//! Set before the worker exists in every test that uses these: the script is
	//! only safe to write from the test thread while nothing is reading it yet,
	//! but each setter still locks so a broker touched after Start stays coherent.
	void SetStartStatus(EControlSenpRpcStatus status)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->startStatus = status;
	}
	void SetAccountGeneration(std::int64_t generation)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->accountGeneration = generation;
	}
	void SetAccountState(EControlSenpAccountState state)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->accountState = state;
	}
	void SetAccountStatus(EControlSenpRpcStatus status)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->accountStatus = status;
	}
	void SetAdoptStatus(EControlSenpRpcStatus status)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->adoptStatus = status;
	}
	void SetResourceStatus(EControlSenpRpcStatus status)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		m_state->resourceStatus = status;
	}

	//! The wire-facing entry points `CBrokerChannel` calls. Both hold the state
	//! lock for the whole exchange, exactly as the single mutex used to, so a
	//! `Sever`/`Restore`/`Publish` from the test thread cannot interleave with one
	//! dispatch. Neither records anything when severed.
	[[nodiscard]] bool BeginHello()
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->severed) return false;
		++m_state->hello;
		return true;
	}
	[[nodiscard]] std::optional<ControlSenpRpcResponse> Dispatch(const ControlSenpRpcRequest& request)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (m_state->severed) return std::nullopt;
		m_state->requests.push_back(request);
		return HandleLocked(request);
	}

private:
	//! Requires `m_state->mutex`, which every caller above already holds.
	ControlSenpRpcResponse HandleLocked(const ControlSenpRpcRequest& request)
	{
		ControlSenpRpcResponse response;
		switch (request.Operation()) {
		case EControlSenpRpcOperation::IssueGrant:
			++m_state->issued;
			response.SetStatus(m_state->issueStatus);
			if (response.Status() == EControlSenpRpcStatus::Succeeded) {
				response.SetGrantId("grant-" + std::to_string(m_state->issued));
				response.SetExpiresAtMilliseconds(10000);
			}
			return response;
		case EControlSenpRpcOperation::StartRead:
			++m_state->started;
			response.SetStatus(m_state->startStatus);
			if (response.Status() == EControlSenpRpcStatus::Succeeded) {
				m_state->startedReads.push_back(request.ReadId());
				m_state->readOwner[request.ReadId()] = request.Owner();
			}
			return response;
		case EControlSenpRpcOperation::PollRead:
			++m_state->polled;
			response.SetStatus(EControlSenpRpcStatus::Succeeded);
			for (auto current = m_state->ready.begin(); current != m_state->ready.end(); ++current) {
				const auto scope = m_state->readOwner.find(current->readId);
				if (scope == m_state->readOwner.end() || !(scope->second == request.Owner())) continue;
				response.SetHasCompletion(true);
				response.SetCompletion(*current);
				m_state->ready.erase(current);
				break;
			}
			return response;
		case EControlSenpRpcOperation::QueryAccount:
			++m_state->accounts;
			response.SetStatus(m_state->accountStatus);
			// The account members are filled even for a refusal, so a seam that
			// read them past one would be caught rather than merely unlucky.
			response.SetAccountGeneration(m_state->accountGeneration);
			response.SetAccountState(m_state->accountState);
			return response;
		case EControlSenpRpcOperation::AdoptWorkspace:
			++m_state->adopted;
			response.SetStatus(m_state->adoptStatus);
			return response;
		case EControlSenpRpcOperation::CancelRead:
			++m_state->cancelled;
			m_state->cancelledReads.push_back(request.ReadId());
			response.SetStatus(EControlSenpRpcStatus::Succeeded);
			return response;
		case EControlSenpRpcOperation::ReadResource: {
			++m_state->resourceReads;
			response.SetStatus(m_state->resourceStatus);
			if (response.Status() != EControlSenpRpcStatus::Succeeded) return response;
			if (request.ResourceHandle() != m_state->resourceHandle
				|| request.Offset() > m_state->resourceBody.size()) {
				response.SetStatus(EControlSenpRpcStatus::NotFound);
				return response;
			}
			const auto offset = static_cast<std::size_t>(request.Offset());
			const auto served = (std::min)(static_cast<std::size_t>(request.Length()),
				m_state->resourceBody.size() - offset);
			response.SetResourceHandle(request.ResourceHandle());
			response.SetResourceOffset(request.Offset());
			response.SetResourceBytes(m_state->resourceBody.substr(offset, served));
			response.SetResourceLength(m_state->resourceBody.size());
			response.SetResourceRevision(m_state->resourceRevision);
			// The store calls a resource Complete only once a chunk reaches the
			// end of the body; before that it is still loading, and the editor
			// is what decides which of the two it has from these members.
			const auto whole = offset + served == m_state->resourceBody.size();
			response.SetResourceState(static_cast<std::uint8_t>(
				whole ? senp::TextResourceState::Complete : senp::TextResourceState::Loading));
			response.SetResourceEnd(static_cast<std::uint8_t>(
				whole ? senp::TextResourceEnd::Complete : senp::TextResourceEnd::None));
			return response;
		}
		case EControlSenpRpcOperation::ReleaseResource:
			++m_state->releases;
			m_state->releasedResources.push_back(request.ResourceHandle());
			response.SetStatus(EControlSenpRpcStatus::Succeeded);
			return response;
		default:
			response.SetStatus(EControlSenpRpcStatus::InvalidRequest);
			return response;
		}
	}

	class State {
		friend class Broker;
		std::mutex mutex;
		int hello = 0;
		int issued = 0;
		int started = 0;
		int polled = 0;
		int cancelled = 0;
		std::vector<ControlSenpRpcRequest> requests;
		std::vector<std::wstring> startedReads;
		std::vector<std::wstring> cancelledReads;
		//! The scope each read was admitted under. The real broker routes a
		//! terminal by scope, so this stand-in must not hand one scope another
		//! scope's read.
		std::map<std::wstring, platform::controlipc::ControlSenpRpcOwner> readOwner;
		//! Terminals the broker hands back, one per poll of the owning scope.
		std::deque<senp::effect::ToolCompleted> ready;
		EControlSenpRpcStatus issueStatus = EControlSenpRpcStatus::Succeeded;
		EControlSenpRpcStatus startStatus = EControlSenpRpcStatus::Succeeded;
		//! The account this profile has adopted, as the control side would
		//! answer it.
		int accounts = 0;
		std::int64_t accountGeneration = 0;
		EControlSenpAccountState accountState = EControlSenpAccountState::Unknown;
		EControlSenpRpcStatus accountStatus = EControlSenpRpcStatus::Succeeded;
		//! Workspace declarations, which the real broker binds to the connection
		//! that made them. Counted rather than replayed: what matters here is
		//! how often one reaches the wire, and the payload is read from
		//! `requests`.
		int adopted = 0;
		EControlSenpRpcStatus adoptStatus = EControlSenpRpcStatus::Succeeded;
		//! Transport failure rather than a broker answer.
		bool severed = false;
		//! The one text resource this stand-in holds. A read is answered out of
		//! the body rather than replayed, because the seam chooses its own
		//! windows.
		int resourceReads = 0;
		int releases = 0;
		std::vector<std::wstring> releasedResources;
		std::wstring resourceHandle = L"log-1";
		std::string resourceBody = "run step output";
		std::int64_t resourceRevision = 11;
		EControlSenpRpcStatus resourceStatus = EControlSenpRpcStatus::Succeeded;
	};

	std::unique_ptr<State> m_state = std::make_unique<State>();
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
		if (request.header.kind == platform::controlipc::EControlIpcKind::Hello) {
			if (!m_broker->BeginHello()) {
				return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0,
					L"severed" };
			}
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
		const auto response = m_broker->Dispatch(*decoded);
		if (!response) {
			return { false, platform::controlipc::EControlIpcTransportDisconnectReason::IoError, 0,
				L"severed" };
		}
		auto payload = platform::controlipc::EncodeControlSenpRpcResponse(*response);
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
	std::shared_ptr<Broker> m_broker;
};

class CFixedEndpointReader final : public platform::controlipc::IControlPlatformEndpointReader {
public:
	std::optional<platform::controlipc::ControlPlatformEndpointSnapshot> Read(
		const platform::controlipc::ControlPlatformEndpointReadRequirements&) override
	{
		return m_endpoint;
	}

	//! Withdraws the published endpoint, as if control discovery found none.
	void ClearEndpoint()
	{
		m_endpoint.reset();
	}

private:
	std::optional<platform::controlipc::ControlPlatformEndpointSnapshot> m_endpoint = Endpoint();
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
	options.SetAuthorityProfileId(kAuthorityId);
	options.SetAuthorityProfileHash(kProfileHash);
	options.SetSenpProfileId(kSenpProfile);
	options.SetPollInterval(std::chrono::milliseconds(1));
	options.SetExchangeDeadline(std::chrono::milliseconds(200));
	options.SetChannelFactory([broker = std::move(broker)] {
		return std::make_unique<CBrokerChannel>(broker);
	});
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

//! The declarations the seam put on the wire, in order.
std::vector<ControlSenpRpcRequest> Declarations(const Broker& broker)
{
	std::vector<ControlSenpRpcRequest> declarations;
	for (const auto& request : broker.Requests()) {
		if (request.Operation() == EControlSenpRpcOperation::AdoptWorkspace) {
			declarations.push_back(request);
		}
	}
	return declarations;
}

std::vector<std::wstring> Folders(std::size_t count)
{
	std::vector<std::wstring> folders;
	for (std::size_t index = 0; index < count; ++index) {
		folders.push_back(L"file:///C:/Work/folder" + std::to_wstring(index));
	}
	return folders;
}

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
	EXPECT_EQ(EControlSenpRpcOperation::IssueGrant, requests[0].Operation());
	EXPECT_EQ(kSenpProfile, requests[0].ProfileId());
	EXPECT_EQ(platform::controlipc::FromContributionOwner(Owner()), requests[0].Owner());
	EXPECT_EQ(static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead),
		requests[0].Capabilities());
	EXPECT_EQ(EControlSenpRpcOperation::StartRead, requests[1].Operation());
	EXPECT_EQ("grant-1", requests[1].GrantId());
	EXPECT_EQ(L"issues:open:1", requests[1].ReadId());
	EXPECT_EQ(L"github", requests[1].ToolId());
	EXPECT_EQ(L"repositoryRead", requests[1].ToolOperation());
	ASSERT_EQ(1u, requests[1].Arguments().size());
	EXPECT_EQ(L"repository", requests[1].Arguments()[0].name);
	EXPECT_EQ(L"owner/project", requests[1].Arguments()[0].value);
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
	broker->SetStartStatus(EControlSenpRpcStatus::Unauthorized);
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
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::MaximumAttempts()), broker->Started());
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::MaximumAttempts()), broker->Issued());
	EXPECT_EQ(0U, reads.OutstandingReads());
}

TEST(SenpControlToolReads, FailsClosedWhenNoControlEndpointIsPublished)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	reader.ClearEndpoint();
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

	for (std::size_t index = 1; index < CSenpControlToolReads::MaximumOwners(); ++index) {
		senp::ContributionOwnerIdentity owner = Owner();
		owner.generation = static_cast<std::int64_t>(index) + 100;
		ASSERT_TRUE(reads.Start(owner, Context(L"tool.1", 1), Read(L"issues:open:1")));
	}
	senp::ContributionOwnerIdentity overflow = Owner();
	overflow.generation = 999;
	EXPECT_FALSE(reads.Start(overflow, Context(L"tool.1", 1), Read(L"issues:open:1")));
	EXPECT_EQ(senp::CSenpRuntimeSession::kMaximumPending + CSenpControlToolReads::MaximumOwners() - 1,
		reads.OutstandingReads());
	reader.Release();
}

TEST(SenpControlToolReads, AdoptsTheEndpointReaderTheCompositionHandsIt)
{
	auto broker = std::make_shared<Broker>();
	broker->Publish({ L"issues:open:1", senp::effect::CompletionStatus::Succeeded, L"[1]", L"" });
	// Production has nowhere to keep a reader for exactly the seam lifetime, so
	// the seam owns it and must still reach the broker through it.
	CSenpControlToolReads reads(Options(broker), std::make_unique<CFixedEndpointReader>());

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto completion = reads.Take(Owner());
	ASSERT_TRUE(completion);
	EXPECT_EQ(senp::effect::CompletionStatus::Succeeded, completion->status);
	EXPECT_EQ(1, broker->Hello());
	EXPECT_EQ(1, broker->Started());
	EXPECT_NE(0u, reads.ConnectionEpoch());
}

TEST(SenpControlToolReads, AnswersTheAccountFenceWithoutNamingAnOwnerOrAGrant)
{
	auto broker = std::make_shared<Broker>();
	broker->SetAccountGeneration(21);
	broker->SetAccountState(EControlSenpAccountState::Connected);
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// Nothing has been asked yet. That is not a signed-out account, and the
	// window must not be able to mistake it for one.
	EXPECT_EQ(SenpToolAccountState::Unknown, reads.Account().State());
	EXPECT_EQ(0, reads.Account().Generation());

	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	EXPECT_EQ(SenpToolAccountState::Connected, reads.Account().State());
	EXPECT_EQ(21, reads.Account().Generation());
	// This is what the window asks before an owner carrying a generation can
	// exist, so the request names no owner and holds no grant to name one with.
	const auto requests = broker->Requests();
	ASSERT_EQ(1u, requests.size());
	EXPECT_EQ(EControlSenpRpcOperation::QueryAccount, requests[0].Operation());
	EXPECT_EQ(kSenpProfile, requests[0].ProfileId());
	EXPECT_TRUE(requests[0].Owner() == platform::controlipc::ControlSenpRpcOwner{});
	EXPECT_TRUE(requests[0].GrantId().empty());
	EXPECT_EQ(0u, requests[0].Capabilities());
	EXPECT_EQ(0, broker->Issued());
}

TEST(SenpControlToolReads, ReportsTheFenceTheControlSideNamesWithoutCollapsingIt)
{
	auto broker = std::make_shared<Broker>();
	// A generation the control side still remembers, under a state that carries
	// no authority. Deciding what that means is the window's rule, not this
	// seam's, so the answer is reported exactly as it arrived.
	broker->SetAccountGeneration(4);
	broker->SetAccountState(EControlSenpAccountState::ReauthenticationRequired);
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	EXPECT_EQ(SenpToolAccountState::ReauthenticationRequired, reads.Account().State());
	EXPECT_EQ(4, reads.Account().Generation());
}

TEST(SenpControlToolReads, ReportsNoFenceWhenTheControlSideIsUnreachableOrRefuses)
{
	auto broker = std::make_shared<Broker>();
	broker->SetAccountGeneration(21);
	broker->SetAccountState(EControlSenpAccountState::Connected);
	// Set before the worker exists: the stand-in's script is only safe to write
	// from this thread while nothing is reading it.
	broker->SetAccountStatus(EControlSenpRpcStatus::NotFound);
	broker->Sever();
	CFixedEndpointReader reader;
	auto options = Options(broker);
	// Both halves of this test ask, and the second follows the first by less
	// than any floor worth naming, so there is none.
	options.SetAccountRefreshInterval(std::chrono::milliseconds(0));
	CSenpControlToolReads reads(std::move(options), reader);

	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	// Unavailable, not Unknown: the question was asked and could not be answered.
	EXPECT_EQ(SenpToolAccountState::Unavailable, reads.Account().State());
	EXPECT_EQ(0, reads.Account().Generation());

	broker->Restore();
	reads.RefreshAccount();
	ASSERT_TRUE(WaitUntil([&] { return broker->Accounts() == 1; }));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	// A refusal names no account, so the members it still carried are ignored.
	EXPECT_EQ(SenpToolAccountState::Unavailable, reads.Account().State());
	EXPECT_EQ(0, reads.Account().Generation());
}

TEST(SenpControlToolReads, CoalescesAccountRefreshesAndReasksAfterTheConnectionIsReplaced)
{
	auto broker = std::make_shared<Broker>();
	broker->SetAccountGeneration(21);
	broker->SetAccountState(EControlSenpAccountState::Connected);
	CFixedEndpointReader reader;
	auto options = Options(broker);
	// A floor no part of this test can wait out, so a second query proves the
	// cadence was reset rather than merely expired.
	options.SetAccountRefreshInterval(std::chrono::seconds(60));
	CSenpControlToolReads reads(std::move(options), reader);

	// The window asks on every frame turn; only the seam decides how often that
	// reaches the wire.
	for (int turn = 0; turn < 6; ++turn) reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(1, broker->Accounts());
	EXPECT_EQ(SenpToolAccountState::Connected, reads.Account().State());

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(WaitUntil([&] { return broker->Started() == 1; }));
	broker->Sever();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_TRUE(reads.Take(Owner()));

	broker->Restore();
	broker->Publish({ L"issues:open:2", senp::effect::CompletionStatus::Succeeded, L"[2]", L"" });
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"issues:open:2")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_TRUE(reads.Take(Owner()));
	ASSERT_EQ(2u, reads.ConnectionEpoch());
	// The connection that answered the fence is gone, so the answer is gone with
	// it - reporting the old generation would name authority nothing holds.
	EXPECT_EQ(SenpToolAccountState::Unknown, reads.Account().State());
	EXPECT_EQ(0, reads.Account().Generation());

	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(2, broker->Accounts());
	EXPECT_EQ(SenpToolAccountState::Connected, reads.Account().State());
	EXPECT_EQ(21, reads.Account().Generation());
}

TEST(SenpControlToolReads, DeclaresTheWorkspaceWithoutNamingAnOwnerOrAGrant)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	reads.DeclareWorkspace(7, 11, { L"file:///C:/Work/repo" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	auto declarations = Declarations(*broker);
	ASSERT_EQ(1u, declarations.size());
	EXPECT_EQ(kSenpProfile, declarations[0].ProfileId());
	// It is what the window says before an owner carrying a workspace revision
	// can exist, so like the account query it names no owner and holds no grant.
	EXPECT_TRUE(declarations[0].Owner() == platform::controlipc::ControlSenpRpcOwner{});
	EXPECT_TRUE(declarations[0].GrantId().empty());
	EXPECT_EQ(7, declarations[0].Workspace().Generation());
	EXPECT_EQ(11, declarations[0].Workspace().Revision());
	ASSERT_EQ(1u, declarations[0].Workspace().Folders().size());
	EXPECT_EQ(L"file:///C:/Work/repo", declarations[0].Workspace().Folders()[0]);
	EXPECT_EQ(0, broker->Issued());

	// The window declares on every turn; only the seam decides what reaches the
	// wire. Re-declaring what the control side already holds would let a window
	// drive the refresh worker on the other side of the pipe.
	for (int turn = 0; turn < 6; ++turn) reads.DeclareWorkspace(7, 11, { L"file:///C:/Work/repo" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(1, broker->Adopted());

	// A changed workspace is a different declaration, and the control side has
	// no way to learn about it other than being told.
	reads.DeclareWorkspace(7, 12, { L"file:///C:/Work/other" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	declarations = Declarations(*broker);
	ASSERT_EQ(2u, declarations.size());
	EXPECT_EQ(12, declarations[1].Workspace().Revision());
	ASSERT_EQ(1u, declarations[1].Workspace().Folders().size());
	EXPECT_EQ(L"file:///C:/Work/other", declarations[1].Workspace().Folders()[0]);
}

TEST(SenpControlToolReads, DeclaresAgainOnTheConnectionThatReplacedTheOneItToldFirst)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	reads.DeclareWorkspace(7, 11, { L"file:///C:/Work/repo" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_EQ(1, broker->Adopted());

	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.1", 1), Read(L"issues:open:1")));
	ASSERT_TRUE(WaitUntil([&] { return broker->Started() == 1; }));
	broker->Sever();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_TRUE(reads.Take(Owner()));

	broker->Restore();
	broker->Publish({ L"issues:open:2", senp::effect::CompletionStatus::Succeeded, L"[2]", L"" });
	ASSERT_TRUE(reads.Start(Owner(), Context(L"tool.2", 2), Read(L"issues:open:2")));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_TRUE(reads.Take(Owner()));
	ASSERT_EQ(2u, reads.ConnectionEpoch());

	// The broker binds a declaration to the connection that made it, exactly as
	// it binds a grant, so a replaced connection has been told nothing. The
	// window is not asked again: the seam re-declares what it already holds.
	const auto declarations = Declarations(*broker);
	ASSERT_EQ(2u, declarations.size());
	EXPECT_EQ(declarations[0].Workspace(), declarations[1].Workspace());
}

TEST(SenpControlToolReads, RetriesADeclarationTheControlSideRefused)
{
	auto broker = std::make_shared<Broker>();
	// Set before the worker exists: the stand-in's script is only safe to write
	// from this thread while nothing is reading it.
	broker->SetAdoptStatus(EControlSenpRpcStatus::ResourceExhausted);
	CFixedEndpointReader reader;
	auto options = Options(broker);
	options.SetAccountRefreshInterval(std::chrono::milliseconds(0));
	CSenpControlToolReads reads(std::move(options), reader);

	reads.DeclareWorkspace(7, 11, { L"file:///C:/Work/repo" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_EQ(1, broker->Adopted());

	// A refused declaration is not a held one. Recording it would leave this
	// window answering for a workspace the control side never accepted, for as
	// long as the connection lasts.
	broker->SetAdoptStatus(EControlSenpRpcStatus::Succeeded);
	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(2, broker->Adopted());
	EXPECT_EQ(1, broker->Accounts());

	// Held now, so the next connection turn says nothing.
	reads.RefreshAccount();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(2, broker->Adopted());
}

TEST(SenpControlToolReads, DeclaresNoFolderRatherThanASubsetTheWindowIsNotOpenOn)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// One past what the wire carries. The control side would refuse to inspect
	// any of them, so the window says it can select nothing - which is true -
	// rather than naming the folders that happen to fit.
	reads.DeclareWorkspace(7, 11, Folders(platform::controlipc::kControlSenpRpcMaximumWorkspaceFolders + 1));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	// A folder with no identity cannot be inspected either, and it says nothing
	// about the folders declared beside it.
	reads.DeclareWorkspace(7, 12, { L"file:///C:/Work/repo", L"" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto declarations = Declarations(*broker);
	ASSERT_EQ(2u, declarations.size());
	EXPECT_EQ(11, declarations[0].Workspace().Revision());
	EXPECT_TRUE(declarations[0].Workspace().Folders().empty());
	EXPECT_EQ(12, declarations[1].Workspace().Revision());
	EXPECT_TRUE(declarations[1].Workspace().Folders().empty());
}

TEST(SenpControlToolReads, SendsNothingForCountersThatObservedNothing)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// A counter that observed nothing would let a staleness check pass for a
	// workspace that was never captured, so neither is a declaration at all.
	reads.DeclareWorkspace(0, 11, { L"file:///C:/Work/repo" });
	reads.DeclareWorkspace(7, 0, { L"file:///C:/Work/repo" });
	reads.DeclareWorkspace(
		static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1, 11,
		{ L"file:///C:/Work/repo" });
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	EXPECT_EQ(0, broker->Adopted());
	// Nothing reached the wire at all: a seam with nothing to declare does not
	// open a connection to say so.
	EXPECT_TRUE(broker->Requests().empty());
	EXPECT_EQ(0, broker->Hello());
}

TEST(SenpControlToolReads, CarriesEachTextResourceChunkWholeFromTheControlStore)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// A window short of the body. The chunk must arrive describing the whole
	// resource it belongs to, because that is what tells the surface it has not
	// reached the end - the byte count it happened to receive does not.
	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 8));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	const auto loading = reads.TakeResource(Owner());
	ASSERT_TRUE(loading);
	EXPECT_EQ(L"log-1", loading->Handle());
	EXPECT_EQ(0U, loading->Offset());
	ASSERT_TRUE(loading->Chunk());
	EXPECT_EQ(senp::TextResourceResult::Accepted, loading->Chunk()->result);
	EXPECT_EQ(senp::TextResourceState::Loading, loading->Chunk()->state);
	EXPECT_EQ(senp::TextResourceEnd::None, loading->Chunk()->end);
	EXPECT_EQ(L"log-1", loading->Chunk()->handle);
	EXPECT_EQ(11, loading->Chunk()->revision);
	EXPECT_EQ(0U, loading->Chunk()->offset);
	EXPECT_EQ(15U, loading->Chunk()->length);
	EXPECT_EQ("run step", loading->Chunk()->bytes);
	// One answer per read: a second drain would hand the same chunk to whatever
	// asked next.
	EXPECT_FALSE(reads.TakeResource(Owner()));

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 8, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	const auto complete = reads.TakeResource(Owner());
	ASSERT_TRUE(complete);
	EXPECT_EQ(8U, complete->Offset());
	ASSERT_TRUE(complete->Chunk());
	EXPECT_EQ(senp::TextResourceState::Complete, complete->Chunk()->state);
	EXPECT_EQ(senp::TextResourceEnd::Complete, complete->Chunk()->end);
	EXPECT_EQ(8U, complete->Chunk()->offset);
	EXPECT_EQ(15U, complete->Chunk()->length);
	EXPECT_EQ(" output", complete->Chunk()->bytes);

	// The read is grant-scoped and names only what the closed operation set
	// admits for it: no read identity, no tool, no arguments.
	EXPECT_EQ(1, broker->Issued());
	EXPECT_EQ(2, broker->ResourceReads());
	const auto requests = broker->Requests();
	ASSERT_GE(requests.size(), 2u);
	EXPECT_EQ(EControlSenpRpcOperation::ReadResource, requests[1].Operation());
	EXPECT_EQ(kSenpProfile, requests[1].ProfileId());
	EXPECT_EQ(platform::controlipc::FromContributionOwner(Owner()), requests[1].Owner());
	EXPECT_EQ("grant-1", requests[1].GrantId());
	EXPECT_EQ(L"log-1", requests[1].ResourceHandle());
	EXPECT_EQ(0U, requests[1].Offset());
	EXPECT_EQ(8U, requests[1].Length());
	EXPECT_TRUE(requests[1].ReadId().empty());
	EXPECT_TRUE(requests[1].ToolId().empty());
	EXPECT_TRUE(requests[1].Arguments().empty());
}

TEST(SenpControlToolReads, HoldsOneResourceReadPerOwnerUntilItsAnswerIsDrained)
{
	auto broker = std::make_shared<Broker>();
	CGatedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// Nothing the wire could carry is admitted: a resource it cannot name, a
	// window of nothing, and one past the chunk the broker answers with.
	EXPECT_FALSE(reads.ReadResource(Owner(), L"", 0, 64));
	EXPECT_FALSE(reads.ReadResource(Owner(), L"log-1", 0, 0));
	EXPECT_FALSE(reads.ReadResource(Owner(), L"log-1", 0,
		platform::controlipc::kControlSenpRpcMaximumResourceChunkBytes + 1));
	senp::ContributionOwnerIdentity ungeneration = Owner();
	ungeneration.generation = 0;
	EXPECT_FALSE(reads.ReadResource(ungeneration, L"log-1", 0, 64));

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	// One at a time. A second read would settle into the same slot, and the
	// surface could not tell which of the two ranges it had been given.
	EXPECT_FALSE(reads.ReadResource(Owner(), L"log-1", 64, 64));
	// Another owner is another surface and is not held by this one.
	EXPECT_TRUE(reads.ReadResource(OtherOwner(), L"log-1", 0, 64));

	reader.Release();
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	const auto unreachable = reads.TakeResource(Owner());
	ASSERT_TRUE(unreachable);
	EXPECT_EQ(L"log-1", unreachable->Handle());
	// The connection is what would have carried an answer, and its absence says
	// nothing about the resource, so no chunk stands in for one.
	EXPECT_FALSE(unreachable->Chunk());
	EXPECT_TRUE(reads.TakeResource(OtherOwner()));
	// Drained, so the surface may ask again.
	EXPECT_TRUE(reads.ReadResource(Owner(), L"log-1", 64, 64));
}

TEST(SenpControlToolReads, AnswersAResourceReadTheControlSideRefusedWithoutInventingAChunk)
{
	auto broker = std::make_shared<Broker>();
	broker->SetResourceStatus(EControlSenpRpcStatus::Closed);
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto answer = reads.TakeResource(Owner());
	ASSERT_TRUE(answer);
	EXPECT_EQ(L"log-1", answer->Handle());
	ASSERT_TRUE(answer->Chunk());
	// The refusal the control side gave, in the store's own vocabulary, and
	// nothing besides: no state, length, revision or bytes were answered, so
	// none are filled in on its behalf.
	EXPECT_EQ(senp::TextResourceResult::Closed, answer->Chunk()->result);
	EXPECT_EQ(0U, answer->Chunk()->length);
	EXPECT_EQ(0, answer->Chunk()->revision);
	EXPECT_TRUE(answer->Chunk()->bytes.empty());
	// A refusal is an answer, so it is not retried.
	EXPECT_EQ(1, broker->ResourceReads());
}

TEST(SenpControlToolReads, RemintsTheGrantForAResourceReadTheControlSideCallsExpired)
{
	auto broker = std::make_shared<Broker>();
	broker->SetResourceStatus(EControlSenpRpcStatus::Expired);
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));

	const auto answer = reads.TakeResource(Owner());
	ASSERT_TRUE(answer);
	ASSERT_TRUE(answer->Chunk());
	// Bounded retries, each on a freshly minted grant, and then the refusal the
	// control side kept giving rather than a generic one: a surface told the
	// resource expired re-resolves it, where a failure is only reported.
	EXPECT_EQ(senp::TextResourceResult::Expired, answer->Chunk()->result);
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::MaximumAttempts()), broker->ResourceReads());
	EXPECT_EQ(static_cast<int>(CSenpControlToolReads::MaximumAttempts()), broker->Issued());
}

TEST(SenpControlToolReads, AnswersAResourceReadWithNoChunkWhenTheConnectionIsLost)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	ASSERT_TRUE(reads.TakeResource(Owner()));

	broker->Sever();
	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	const auto lost = reads.TakeResource(Owner());
	ASSERT_TRUE(lost);
	EXPECT_EQ(L"log-1", lost->Handle());
	EXPECT_EQ(0U, lost->Offset());
	EXPECT_FALSE(lost->Chunk());
	EXPECT_EQ(ESenpControlToolReadsState::Disconnected, reads.State());
}

TEST(SenpControlToolReads, WithdrawsAResourceOnTheWireWithoutWaitingForAnAnswer)
{
	auto broker = std::make_shared<Broker>();
	CFixedEndpointReader reader;
	CSenpControlToolReads reads(Options(broker), reader);

	// A release for an owner this seam holds nothing for names a resource no
	// grant of ours reaches, so nothing goes on the wire for it.
	reads.ReleaseResource(Owner(), L"log-1");
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(0, broker->Releases());

	ASSERT_TRUE(reads.ReadResource(Owner(), L"log-1", 0, 64));
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_TRUE(reads.TakeResource(Owner()));

	reads.ReleaseResource(Owner(), L"");
	reads.ReleaseResource(Owner(), L"log-1");
	ASSERT_TRUE(reads.WaitForSettled(kSettle));
	EXPECT_EQ(1, broker->Releases());
	ASSERT_EQ(1u, broker->Released().size());
	EXPECT_EQ(L"log-1", broker->Released()[0]);
	// A withdrawal names the whole resource, so it carries no window, and it
	// produces no answer for the surface to drain.
	const auto requests = broker->Requests();
	const auto release = std::find_if(requests.begin(), requests.end(),
		[](const ControlSenpRpcRequest& request) {
			return request.Operation() == EControlSenpRpcOperation::ReleaseResource;
		});
	ASSERT_NE(requests.end(), release);
	EXPECT_EQ(L"log-1", release->ResourceHandle());
	EXPECT_EQ(0U, release->Offset());
	EXPECT_EQ(0U, release->Length());
	EXPECT_EQ("grant-1", release->GrantId());
	EXPECT_FALSE(reads.TakeResource(Owner()));
}

TEST(SenpControlToolReads, RefusesToExistWithoutAnEndpointReaderToDiscoverThrough)
{
	auto broker = std::make_shared<Broker>();
	// A seam with no discovery could never connect, so it fails loudly at
	// composition instead of accepting reads it would silently never carry.
	EXPECT_THROW(CSenpControlToolReads(Options(broker),
		std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader>{}),
		std::invalid_argument);
}

} // namespace workbench::editor
