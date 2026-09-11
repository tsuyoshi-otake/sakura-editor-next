/*! @file
	@brief Control-owned SENP tool broker session over one authenticated pipe.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "platform/controlipc/ControlSenpRpc.h"
#include "senp/SenpToolGrants.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platform::controlipc {

//! Fixed tool identity the broker admits. The editor selects an operation from
//! this closed set; it never names an executable, argument or environment.
inline constexpr std::wstring_view kSenpGitHubToolId = L"github";
inline constexpr std::wstring_view kSenpGitHubRepositoryReadOperation = L"repositoryRead";
//! One workflow job's log. It reads the same repository the other operation
//! does and needs no capability of its own; what separates it is the shape of
//! what comes back, which is a log stream rather than a JSON page.
inline constexpr std::wstring_view kSenpGitHubJobLogOperation = L"jobLog";

//! One connection, named without an owner.
//!
//! A workspace is declared by the connection that opened on it, before any owner
//! carrying a workspace revision can exist, so a declaration cannot be scoped by
//! SenpToolExecutionScope the way a read is. It is scoped by this instead, and
//! that is also what makes the declaration disappear with the connection.
class SenpConnectionIdentity {
public:
	SenpConnectionIdentity() = default;
	SenpConnectionIdentity(std::uint64_t sessionId, std::uint32_t clientProcessId) noexcept :
		m_sessionId(sessionId), m_clientProcessId(clientProcessId)
	{
	}

	[[nodiscard]] std::uint64_t SessionId() const noexcept { return m_sessionId; }
	[[nodiscard]] std::uint32_t ClientProcessId() const noexcept { return m_clientProcessId; }

private:
	std::uint64_t m_sessionId = 0;
	std::uint32_t m_clientProcessId = 0;
};

//! Free function rather than a member: it reads every field through the same
//! public accessors any other caller uses, so the comparison stays exactly
//! member-wise without a member declaration of its own.
[[nodiscard]] inline bool operator==(const SenpConnectionIdentity& lhs, const SenpConnectionIdentity& rhs) noexcept
{
	return lhs.SessionId() == rhs.SessionId() && lhs.ClientProcessId() == rhs.ClientProcessId();
}

//! One bounded execution scope. It pairs the OS-observed connection with the
//! profile and owner the control side resolved for itself, so a completion can
//! never be handed to a different connection, profile, owner or generation.
class SenpToolExecutionScope {
public:
	SenpToolExecutionScope() = default;
	SenpToolExecutionScope(std::uint64_t sessionId, std::uint32_t clientProcessId, std::wstring profileId,
		senp::ContributionOwnerIdentity owner) :
		m_sessionId(sessionId), m_clientProcessId(clientProcessId), m_profileId(std::move(profileId)),
		m_owner(std::move(owner))
	{
	}

	[[nodiscard]] std::uint64_t SessionId() const noexcept { return m_sessionId; }
	[[nodiscard]] std::uint32_t ClientProcessId() const noexcept { return m_clientProcessId; }
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] const senp::ContributionOwnerIdentity& Owner() const noexcept { return m_owner; }

private:
	std::uint64_t m_sessionId = 0;
	std::uint32_t m_clientProcessId = 0;
	std::wstring m_profileId;
	senp::ContributionOwnerIdentity m_owner;
};

//! Free function rather than a member: it reads every field through the same
//! public accessors any other caller uses, so the comparison stays exactly
//! member-wise without a member declaration of its own.
[[nodiscard]] inline bool operator==(const SenpToolExecutionScope& lhs, const SenpToolExecutionScope& rhs) noexcept
{
	return lhs.SessionId() == rhs.SessionId() && lhs.ClientProcessId() == rhs.ClientProcessId()
		&& lhs.ProfileId() == rhs.ProfileId() && lhs.Owner() == rhs.Owner();
}

//! One connection's declared workspace. It names folders and never a repository:
//! what those folders resolve to is decided by the control side from the remotes
//! it finds there, so the editor's claim is verified rather than trusted.
class SenpWorkspaceAdoption {
public:
	SenpWorkspaceAdoption() = default;
	SenpWorkspaceAdoption(SenpConnectionIdentity connection, std::wstring profileId,
		ControlSenpRpcWorkspace workspace) :
		m_connection(connection), m_profileId(std::move(profileId)), m_workspace(std::move(workspace))
	{
	}

	[[nodiscard]] const SenpConnectionIdentity& Connection() const noexcept { return m_connection; }
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] const ControlSenpRpcWorkspace& Workspace() const noexcept { return m_workspace; }

private:
	SenpConnectionIdentity m_connection;
	std::wstring m_profileId;
	ControlSenpRpcWorkspace m_workspace;
};

//! One admitted read. The broker forwards only values it has already bounded.
class SenpToolReadCommand {
public:
	SenpToolReadCommand() = default;
	SenpToolReadCommand(std::wstring readId, std::wstring toolId, std::wstring operation,
		std::vector<senp::effect::Field> arguments) :
		m_readId(std::move(readId)), m_toolId(std::move(toolId)), m_operation(std::move(operation)),
		m_arguments(std::move(arguments))
	{
	}

	[[nodiscard]] const std::wstring& ReadId() const noexcept { return m_readId; }
	[[nodiscard]] const std::wstring& ToolId() const noexcept { return m_toolId; }
	[[nodiscard]] const std::wstring& Operation() const noexcept { return m_operation; }
	[[nodiscard]] const std::vector<senp::effect::Field>& Arguments() const noexcept { return m_arguments; }

private:
	std::wstring m_readId;
	std::wstring m_toolId;
	std::wstring m_operation;
	std::vector<senp::effect::Field> m_arguments;
};

/*!
	@brief Bounded, nonblocking tool execution seam owned by the control process.

	Every method must return without waiting on `gh`, on the package tool or on
	any other external process: frame processing is admission and state retrieval
	only. An implementation owns its own workers, its own cancellation and its own
	resource store, and must treat the scope as the sole authority for routing a
	completion back. CancelScope is called from the session destructor and must
	leave no worker able to touch that scope afterwards.
*/
class ISenpToolExecutor {
public:
	virtual ~ISenpToolExecutor() = default;
	//! Admits one read without dispatching synchronously. Succeeded means only
	//! that the read was queued; the terminal arrives through TakeCompleted.
	[[nodiscard]] virtual EControlSenpRpcStatus StartRead(const SenpToolExecutionScope& scope,
		const SenpToolReadCommand& command) noexcept = 0;
	//! Drains at most one finished terminal for this scope. An empty result means
	//! nothing has finished yet; it is a successful answer, not a failure.
	[[nodiscard]] virtual std::optional<senp::effect::ToolCompleted> TakeCompleted(
		const SenpToolExecutionScope& scope) noexcept = 0;
	virtual void CancelRead(const SenpToolExecutionScope& scope, std::wstring_view readId) noexcept = 0;
	//! Terminates every read and resource of this scope and completes physical
	//! cleanup before returning. It is the destructor's cancellation obligation.
	virtual void CancelScope(const SenpToolExecutionScope& scope) noexcept = 0;
	[[nodiscard]] virtual EControlSenpRpcStatus ReadResource(const SenpToolExecutionScope& scope,
		std::wstring_view handle, std::uint64_t offset, std::uint32_t length,
		ControlSenpRpcResponse& response) noexcept = 0;
	virtual void ReleaseResource(const SenpToolExecutionScope& scope,
		std::wstring_view handle) noexcept = 0;
	/*!
		@brief Answers one profile's adopted account state.

		This is the only method with no execution scope, because it is what the
		editor asks before it can know the account generation an owner would carry.
		An implementation fills the account members of `response` and nothing else,
		and stays as bounded as every other method here.
	*/
	[[nodiscard]] virtual EControlSenpRpcStatus QueryAccount(std::wstring_view profileId,
		ControlSenpRpcResponse& response) noexcept = 0;
	/*!
		@brief Records the workspace one connection declares it is opened on.

		Like QueryAccount it carries no execution scope, because it precedes every
		owner. It must only record the declaration: inspecting a folder for its
		remotes is exactly the kind of blocking work frame processing may not do,
		so it belongs on an implementation's own worker.
	*/
	[[nodiscard]] virtual EControlSenpRpcStatus AdoptWorkspace(
		const SenpWorkspaceAdoption& adoption) noexcept = 0;
	/*!
		@brief Withdraws whatever that connection declared.

		The session destructor calls it for every connection, including ones that
		declared nothing, so withdrawing an unknown connection must be a no-op.
		A declaration outliving its connection would let a closed window keep
		deciding which repository a profile answers for.
	*/
	virtual void WithdrawWorkspace(const SenpConnectionIdentity& connection) noexcept = 0;
};

/*!
	@brief Frame handler factory for the additive SenpRequest/SenpResponse kinds.

	It is composed by the control runtime only, after storage Hello has already
	authenticated the connection. Each accepted pipe gets one session that owns a
	connection-bound grant session; destroying that session revokes its grants and
	cancels its executor scope before the peer handle is released.
*/
class CControlSenpBroker final : public IControlIpcFrameHandler {
public:
	//! Sessions minted per connection stay bounded by the grant registry's own
	//! maximum, so one connection cannot hold more capability records than grants.
	[[nodiscard]] static constexpr std::size_t MaximumTrackedGrants() noexcept
	{
		return senp::CSenpToolGrants::MaximumGrants();
	}
	CControlSenpBroker(std::shared_ptr<senp::CSenpToolGrants> grants,
		std::shared_ptr<ISenpToolExecutor> executor);
	~CControlSenpBroker() override;
	CControlSenpBroker(const CControlSenpBroker&) = delete;
	CControlSenpBroker& operator=(const CControlSenpBroker&) = delete;

	[[nodiscard]] std::unique_ptr<IControlIpcSessionHandler> CreateSession(
		const ControlIpcSessionContext& session) override;

private:
	class Session;

	std::shared_ptr<senp::CSenpToolGrants> m_grants;
	std::shared_ptr<ISenpToolExecutor> m_executor;
};

} // namespace platform::controlipc
