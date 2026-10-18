/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlSenpBroker.h"

#include "platform/profiles/UserDataProfileIdentity.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr std::size_t kMaximumPayload = kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;

//! The session remembers only the most recent minted grant ids, generously
//! against the live-record cap: an id it evicts answers Unauthorized like one
//! never minted, which the editor recovers from the same way as Expired.
constexpr std::size_t kIssuedGrantIdHistory = 4 * CControlSenpBroker::MaximumTrackedGrants();

ControlIpcFrame Frame(const ControlIpcFrame& request, EControlIpcKind kind,
	std::vector<std::uint8_t> payload) noexcept
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal,
		request.header.requestId, request.header.generation }, std::move(payload) };
}

ControlIpcFrame ErrorFrame(const ControlIpcFrame& request, EControlIpcTerminalStatus status) noexcept
{
	return Frame(request, EControlIpcKind::Error,
		EncodeControlIpcError({ status, {} }).value_or(std::vector<std::uint8_t>{}));
}

//! Maps a grant outcome onto the broker's own status vocabulary. A refused
//! grant is a normal terminal answer, so it never becomes a transport error.
EControlSenpRpcStatus ToStatus(const senp::SenpToolGrantIssueStatus issued) noexcept
{
	switch (issued) {
	case senp::SenpToolGrantIssueStatus::Granted: return EControlSenpRpcStatus::Succeeded;
	case senp::SenpToolGrantIssueStatus::Unauthorized: return EControlSenpRpcStatus::Unauthorized;
	case senp::SenpToolGrantIssueStatus::ResourceExhausted: return EControlSenpRpcStatus::ResourceExhausted;
	case senp::SenpToolGrantIssueStatus::Unavailable: return EControlSenpRpcStatus::Unavailable;
	case senp::SenpToolGrantIssueStatus::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::InvalidRequest;
	}
}

EControlSenpRpcStatus ToStatus(const senp::SenpToolGrantCheck checked) noexcept
{
	switch (checked) {
	case senp::SenpToolGrantCheck::Granted: return EControlSenpRpcStatus::Succeeded;
	case senp::SenpToolGrantCheck::Expired: return EControlSenpRpcStatus::Expired;
	case senp::SenpToolGrantCheck::WrongConnection:
	case senp::SenpToolGrantCheck::WrongScope:
	case senp::SenpToolGrantCheck::StaleAuthority: return EControlSenpRpcStatus::Unauthorized;
	case senp::SenpToolGrantCheck::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::InvalidRequest;
	}
}

//! The capability a tool operation needs is decided here, never taken from the
//! request. StartRead is the only operation that names a tool, so the closed
//! set is enforced at admission rather than inside the executor.
bool IsAdmittedToolOperation(const ControlSenpRpcRequest& request,
	const senp::SenpToolCapability capability) noexcept
{
	if (capability != senp::SenpToolCapability::GitHubRepositoryRead) return false;
	if (request.ToolId() != kSenpGitHubToolId) return false;
	// Both operations read the repository this capability names, so both are
	// admitted by it. The set stays written out here rather than derived from a
	// prefix: an operation is admitted because it was listed, never because it
	// resembled one that was.
	return request.ToolOperation() == kSenpGitHubRepositoryReadOperation
		|| request.ToolOperation() == kSenpGitHubJobLogOperation;
}

} // namespace

/*!
	@brief One authenticated connection's SENP broker state.

	Frame handling stays bounded: it decodes, rechecks the control-owned grant,
	then performs one admission or one state retrieval. It never waits on a
	child process. Destruction cancels every executor scope of this connection
	in one call before the grant session is closed, so no worker can deliver
	into a released connection.
*/
class CControlSenpBroker::Session final : public IControlIpcSessionHandler {
public:
	Session(std::unique_ptr<senp::CSenpToolGrantSession> grants,
		std::shared_ptr<ISenpToolExecutor> executor, ControlIpcSessionContext connection,
		std::function<std::chrono::steady_clock::time_point()> clock) noexcept :
		m_grants(std::move(grants)), m_executor(std::move(executor)),
		m_connection(connection), m_clock(std::move(clock))
	{
		if (!m_clock) m_clock = &std::chrono::steady_clock::now;
	}

	~Session() override
	{
		// Cancellation precedes grant revocation: a worker must lose its scope
		// before the record that named it disappears. Teardown is by connection,
		// not by remembered scope - an owner identity is reissued on every
		// workspace-revision or account change, so remembering the distinct
		// scopes this session ever minted would grow without bound.
		if (m_executor) {
			m_executor->CancelConnection(m_connection.sessionId, m_connection.clientProcessId);
			// Unconditional: a connection that declared nothing withdraws
			// nothing, and tracking whether it did would only add a way for the
			// declaration to survive the connection that made it.
			m_executor->WithdrawWorkspace({ m_connection.sessionId, m_connection.clientProcessId });
		}
		if (m_grants) m_grants->Close();
	}

	ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext& session,
		const ControlIpcFrame& frame) override
	{
		// The adapter already matched the observed peer, but a session that is
		// reused for a different connection must fail rather than answer.
		if (session.sessionId != m_connection.sessionId
			|| session.clientProcessId != m_connection.clientProcessId) {
			return { { ErrorFrame(frame, EControlIpcTerminalStatus::AccessDenied) },
				EControlIpcSessionDecision::Close };
		}
		auto fields = DecodeControlIpcFields(frame.payload);
		if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || fields.fields.size() != 1
			|| fields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload)) {
			return Invalid(frame);
		}
		auto request = DecodeControlSenpRpcRequest(fields.fields.front().value);
		if (!request) return Invalid(frame);
		return Reply(frame, Dispatch(*request));
	}

private:
	//! One grant this connection minted. The capability is remembered by the
	//! control side because the wire never restates it after issue: an editor
	//! must not be able to widen a record by naming a different capability.
	class Record {
	public:
		Record() = default;
		Record(std::string grantId, SenpToolExecutionScope scope, senp::SenpToolCapability capability,
			std::chrono::steady_clock::time_point expiresAt) :
			m_grantId(std::move(grantId)), m_scope(std::move(scope)), m_capability(capability),
			m_expiresAt(expiresAt)
		{
		}

		[[nodiscard]] const std::string& GrantId() const noexcept { return m_grantId; }
		[[nodiscard]] const SenpToolExecutionScope& Scope() const noexcept { return m_scope; }
		[[nodiscard]] senp::SenpToolCapability Capability() const noexcept { return m_capability; }
		[[nodiscard]] std::chrono::steady_clock::time_point ExpiresAt() const noexcept { return m_expiresAt; }

	private:
		std::string m_grantId;
		SenpToolExecutionScope m_scope;
		senp::SenpToolCapability m_capability{ senp::SenpToolCapability::None };
		std::chrono::steady_clock::time_point m_expiresAt{};
	};

	static ControlIpcFrameDispatchResult Invalid(const ControlIpcFrame& frame)
	{
		return { { ErrorFrame(frame, EControlIpcTerminalStatus::InvalidRequest) },
			EControlIpcSessionDecision::KeepOpen };
	}

	ControlIpcFrameDispatchResult Reply(const ControlIpcFrame& frame,
		const ControlSenpRpcResponse& response) const
	{
		auto payload = EncodeControlSenpRpcResponse(response);
		if (!payload || payload->size() > kMaximumPayload) {
			return { { ErrorFrame(frame, EControlIpcTerminalStatus::InternalError) },
				EControlIpcSessionDecision::Close };
		}
		auto outer = EncodeControlIpcFields(
			{ { static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload), std::move(*payload) } });
		if (!outer || outer->size() > kMaximumPayload) {
			return { { ErrorFrame(frame, EControlIpcTerminalStatus::InternalError) },
				EControlIpcSessionDecision::Close };
		}
		return { { Frame(frame, EControlIpcKind::SenpResponse, std::move(*outer)) },
			EControlIpcSessionDecision::KeepOpen };
	}

	//! A status-only terminal answer. Both a refusal and a plain acknowledgement
	//! travel this way, because neither carries a completion or a chunk.
	static ControlSenpRpcResponse Terminal(EControlSenpRpcStatus status) noexcept
	{
		ControlSenpRpcResponse response;
		response.SetStatus(status);
		return response;
	}

	[[nodiscard]] const Record* Find(const std::string& grantId) const noexcept
	{
		const auto found = std::find_if(m_records.begin(), m_records.end(),
			[&](const Record& record) { return record.GrantId() == grantId; });
		return found == m_records.end() ? nullptr : &*found;
	}

	//! Rechecks the control-owned authority for the named record and returns the
	//! scope only when the grant is still live for exactly this request.
	[[nodiscard]] EControlSenpRpcStatus Authorize(const ControlSenpRpcRequest& request,
		const Record*& record)
	{
		record = Find(request.GrantId());
		if (!record) {
			// A grant id this session minted but no longer tracks died either at
			// expiry or by revocation, so the client re-issues on Expired. An id
			// it never minted - or one the bounded history already evicted -
			// stays a refusal, never an expiry hint.
			return std::ranges::find(m_issued, request.GrantId()) != m_issued.end()
				? EControlSenpRpcStatus::Expired : EControlSenpRpcStatus::Unauthorized;
		}
		const auto owner = ToContributionOwner(request.Owner());
		if (record->Scope().ProfileId() != request.ProfileId() || !(record->Scope().Owner() == owner)) {
			return EControlSenpRpcStatus::Unauthorized;
		}
		if (!m_grants) return EControlSenpRpcStatus::Closed;
		const senp::SenpToolGrantRequest scoped(request.ProfileId(), owner, record->Capability());
		const auto checked = m_grants->Validate(request.GrantId(), scoped, m_clock());
		if (checked == senp::SenpToolGrantCheck::Invalid
			|| checked == senp::SenpToolGrantCheck::Expired) {
			// The session minted the grant but the registry no longer holds it:
			// it expired or was pruned or revoked. The dead record leaves the
			// live set here - never by cancelling its scope, which a renewed
			// grant of the same owner can still share - and the client hears
			// Expired so it re-issues.
			Drop(request.GrantId());
			record = nullptr;
			return EControlSenpRpcStatus::Expired;
		}
		return ToStatus(checked);
	}

	void Drop(const std::string_view grantId) noexcept
	{
		const auto found = std::find_if(m_records.begin(), m_records.end(),
			[&](const Record& record) { return record.GrantId() == grantId; });
		if (found != m_records.end()) m_records.erase(found);
	}

	ControlSenpRpcResponse Dispatch(const ControlSenpRpcRequest& request)
	{
		if (!m_grants) return Terminal(EControlSenpRpcStatus::Closed);
		if (request.Operation() == EControlSenpRpcOperation::IssueGrant) return Issue(request);
		if (request.Operation() == EControlSenpRpcOperation::QueryAccount) return Account(request);
		if (request.Operation() == EControlSenpRpcOperation::AdoptWorkspace) return Adopt(request);
		const Record* record = nullptr;
		if (const auto status = Authorize(request, record); status != EControlSenpRpcStatus::Succeeded) {
			return Terminal(status);
		}
		if (!m_executor) return Terminal(EControlSenpRpcStatus::Unavailable);
		switch (request.Operation()) {
		case EControlSenpRpcOperation::StartRead: {
			if (!IsAdmittedToolOperation(request, record->Capability())) {
				return Terminal(EControlSenpRpcStatus::Unauthorized);
			}
			const SenpToolReadCommand command(request.ReadId(), request.ToolId(),
				request.ToolOperation(), request.Arguments());
			return Terminal(m_executor->StartRead(record->Scope(), command));
		}
		case EControlSenpRpcOperation::PollRead: {
			auto completed = m_executor->TakeCompleted(record->Scope());
			ControlSenpRpcResponse response;
			response.SetStatus(EControlSenpRpcStatus::Succeeded);
			if (completed) {
				response.SetHasCompletion(true);
				response.Completion() = std::move(*completed);
			}
			return response;
		}
		case EControlSenpRpcOperation::CancelRead:
			m_executor->CancelRead(record->Scope(), request.ReadId());
			return Terminal(EControlSenpRpcStatus::Succeeded);
		case EControlSenpRpcOperation::ReadResource: {
			ControlSenpRpcResponse response;
			const auto status = m_executor->ReadResource(record->Scope(), request.ResourceHandle(),
				request.Offset(), request.Length(), response);
			// A partially filled response must not escape a refused read.
			if (status != EControlSenpRpcStatus::Succeeded) return Terminal(status);
			response.SetStatus(status);
			return response;
		}
		case EControlSenpRpcOperation::ReleaseResource:
			m_executor->ReleaseResource(record->Scope(), request.ResourceHandle());
			return Terminal(EControlSenpRpcStatus::Succeeded);
		default:
			return Terminal(EControlSenpRpcStatus::InvalidRequest);
		}
	}

	/*!
		@brief The one operation that carries no grant and no owner.

		There is nothing owner-scoped to recheck, so admission is the whole check:
		the id has to be an opaque user-data profile id, which is the same identity
		space every grant this connection could mint is scoped by. The answer names
		no account, no login and no token; it says only which generation the profile
		has adopted and how the control side currently sees it.
	*/
	ControlSenpRpcResponse Account(const ControlSenpRpcRequest& request)
	{
		if (!platform::profiles::IsOpaqueUserDataProfileId(request.ProfileId())) {
			return Terminal(EControlSenpRpcStatus::InvalidRequest);
		}
		if (!m_executor) return Terminal(EControlSenpRpcStatus::Unavailable);
		ControlSenpRpcResponse response;
		const auto status = m_executor->QueryAccount(request.ProfileId(), response);
		// A partially filled answer must not escape a refused query.
		if (status != EControlSenpRpcStatus::Succeeded) return Terminal(status);
		response.SetStatus(status);
		return response;
	}

	/*!
		@brief The other operation that carries no grant and no owner.

		Admission is the whole check here too, and it is the same check: the id
		has to be an opaque user-data profile id. The connection is taken from
		what the OS observed for this pipe rather than from the request, so a
		declaration can only ever be attributed to the connection that made it.
	*/
	ControlSenpRpcResponse Adopt(const ControlSenpRpcRequest& request)
	{
		if (!platform::profiles::IsOpaqueUserDataProfileId(request.ProfileId())) {
			return Terminal(EControlSenpRpcStatus::InvalidRequest);
		}
		if (!m_executor) return Terminal(EControlSenpRpcStatus::Unavailable);
		const SenpWorkspaceAdoption adoption(
			SenpConnectionIdentity(m_connection.sessionId, m_connection.clientProcessId),
			request.ProfileId(), request.Workspace());
		return Terminal(m_executor->AdoptWorkspace(adoption));
	}

	ControlSenpRpcResponse Issue(const ControlSenpRpcRequest& request)
	{
		const auto now = m_clock();
		// The session tracks only live grants: expired records are dropped
		// before the cap check, so one connection can renew indefinitely.
		// Dropping never cancels a scope - a renewed grant shares it.
		std::erase_if(m_records, [now](const Record& record) { return now >= record.ExpiresAt(); });
		if (m_records.size() >= MaximumTrackedGrants()) {
			return Terminal(EControlSenpRpcStatus::ResourceExhausted);
		}
		const auto capability = static_cast<senp::SenpToolCapability>(request.Capabilities());
		const auto owner = ToContributionOwner(request.Owner());
		const senp::SenpToolGrantRequest scoped(request.ProfileId(), owner, capability);
		const auto issued = m_grants->Issue(scoped, now);
		if (issued.Status() != senp::SenpToolGrantIssueStatus::Granted) {
			return Terminal(ToStatus(issued.Status()));
		}
		SenpToolExecutionScope scope(m_connection.sessionId, m_connection.clientProcessId,
			request.ProfileId(), owner);
		m_records.emplace_back(issued.GrantId(), scope, capability, issued.ExpiresAt());
		// The most recent minted ids are remembered so a record the registry
		// dropped still answers Expired rather than Unauthorized. The history
		// is bounded at four times the live-record cap; an evicted id falls
		// back to Unauthorized, which the editor treats exactly like Expired
		// (forget the cached grant and re-issue, SenpControlToolReads.cpp).
		if (m_issued.size() >= kIssuedGrantIdHistory) m_issued.pop_front();
		m_issued.push_back(issued.GrantId());

		ControlSenpRpcResponse response;
		response.SetStatus(EControlSenpRpcStatus::Succeeded);
		response.SetGrantId(issued.GrantId());
		// The wire carries a relative lifetime: a steady_clock origin means
		// nothing across processes and must never be published as a timestamp.
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			issued.ExpiresAt() - m_clock());
		response.SetExpiresAtMilliseconds(remaining.count() > 0
			? static_cast<std::uint64_t>(remaining.count()) : 0);
		return response;
	}

	std::unique_ptr<senp::CSenpToolGrantSession> m_grants;
	std::shared_ptr<ISenpToolExecutor> m_executor;
	const ControlIpcSessionContext m_connection;
	std::vector<Record> m_records;
	std::deque<std::string> m_issued;
	std::function<std::chrono::steady_clock::time_point()> m_clock;
};

CControlSenpBroker::CControlSenpBroker(std::shared_ptr<senp::CSenpToolGrants> grants,
	std::shared_ptr<ISenpToolExecutor> executor,
	std::function<std::chrono::steady_clock::time_point()> clock) :
	m_grants(std::move(grants)), m_executor(std::move(executor)), m_clock(std::move(clock))
{
	if (!m_clock) m_clock = &std::chrono::steady_clock::now;
}

CControlSenpBroker::~CControlSenpBroker() = default;

std::unique_ptr<IControlIpcSessionHandler> CControlSenpBroker::CreateSession(
	const ControlIpcSessionContext& session)
{
	// A connection the transport could not observe cannot bind a grant, so no
	// session is created rather than one whose scope could never be rechecked.
	if (!m_grants || session.sessionId == 0 || session.clientProcessId == 0) return {};
	auto grants = m_grants->OpenSession(session);
	if (!grants) return {};
	return std::make_unique<Session>(std::move(grants), m_executor, session, m_clock);
}

} // namespace platform::controlipc
