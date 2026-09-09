/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/editor/SenpControlToolReads.h"

#include "senp/SenpRuntimeSession.h"
#include "senp/SenpToolGrants.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace workbench::editor {
namespace {

using platform::controlipc::ControlSenpRpcRequest;
using platform::controlipc::EControlSenpClientState;
using platform::controlipc::EControlSenpRpcOperation;
using platform::controlipc::EControlSenpRpcStatus;

//! Per-owner bound for both outstanding reads and their terminals. One admitted
//! read produces exactly one terminal, so the same bound covers both queues.
constexpr std::size_t kPerOwner = senp::CSenpRuntimeSession::kMaximumPending;

//! Diagnostics are generic on purpose. They travel into a document the extension
//! can read, so they never carry a grant id, a profile or a tool argument.
constexpr wchar_t kUnavailable[] = L"the control endpoint is unavailable";
constexpr wchar_t kReplaced[] = L"the control connection was replaced";
constexpr wchar_t kLost[] = L"the control connection was lost";
constexpr wchar_t kRefused[] = L"the control side refused the tool read";
constexpr wchar_t kSaturated[] = L"the control side is saturated";
constexpr wchar_t kStopped[] = L"the tool broker was stopped";

//! A broker refusal is a host outage only when the control side says the tool
//! host itself is gone. Everything else is this request's own failure.
senp::effect::CompletionStatus ToCompletion(const EControlSenpRpcStatus status) noexcept
{
	switch (status) {
	case EControlSenpRpcStatus::Unavailable:
	case EControlSenpRpcStatus::NotConnected:
	case EControlSenpRpcStatus::Closed:
		return senp::effect::CompletionStatus::HostUnavailable;
	default:
		return senp::effect::CompletionStatus::Failed;
	}
}

platform::controlipc::ControlSenpClientOptions ClientOptions(const SenpControlToolReadsOptions& options)
{
	platform::controlipc::ControlSenpClientOptions client;
	client.profileId = options.authorityProfileId;
	client.profileHash = options.authorityProfileHash;
	client.minimumGeneration = options.minimumGeneration;
	client.exchangeDeadline = options.exchangeDeadline;
	client.channelFactory = options.channelFactory;
	return client;
}

//! The reference the adopting constructor hands the client is bound before the
//! client member exists, so the null check has to happen inside the member
//! initializer rather than in the body.
platform::controlipc::IControlPlatformEndpointReader& Adopted(
	const std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader>& reader)
{
	if (!reader) throw std::invalid_argument("SenpControlToolReads requires an endpoint reader");
	return *reader;
}

} // namespace

CSenpControlToolReads::CSenpControlToolReads(SenpControlToolReadsOptions options,
	platform::controlipc::IControlPlatformEndpointReader& endpointReader) :
	m_options(std::move(options)), m_client(ClientOptions(m_options), endpointReader)
{
	// A zero interval would turn the idle worker into a spin, so the poll cadence
	// has a floor rather than a caller-chosen one.
	if (m_options.pollInterval < std::chrono::milliseconds(1)) {
		m_options.pollInterval = std::chrono::milliseconds(1);
	}
	m_worker = std::thread([this] { Worker(); });
}

CSenpControlToolReads::CSenpControlToolReads(SenpControlToolReadsOptions options,
	std::unique_ptr<platform::controlipc::IControlPlatformEndpointReader> endpointReader) :
	m_options(std::move(options)), m_ownedReader(std::move(endpointReader)),
	m_client(ClientOptions(m_options), Adopted(m_ownedReader))
{
	if (m_options.pollInterval < std::chrono::milliseconds(1)) {
		m_options.pollInterval = std::chrono::milliseconds(1);
	}
	m_worker = std::thread([this] { Worker(); });
}

CSenpControlToolReads::~CSenpControlToolReads()
{
	Stop();
}

// --- UI thread -------------------------------------------------------------

bool CSenpControlToolReads::Start(const senp::ContributionOwnerIdentity& owner,
	const senp::effect::OperationContext& context,
	const senp::effect::StartToolRead& read) noexcept
{
	if (read.readId.empty() || read.toolId.empty() || read.operation.empty()) return false;
	if (read.arguments.size() > platform::controlipc::kControlSenpRpcMaximumArguments) return false;
	// The broker's own coherence rules would refuse a request shaped like this,
	// so the boundary fails here instead of dispatching something unencodable.
	if (owner.extensionId.empty() || owner.generation <= 0 || owner.workspaceRevision < 0
		|| owner.accountGeneration < 0 || m_options.senpProfileId.empty()) {
		return false;
	}
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopped) return false;
		auto* entry = FindLocked(owner);
		if (!entry) {
			if (m_owners.size() >= kMaximumOwners) return false;
			m_owners.push_back(Owner{ owner, {}, {}, false });
			entry = &m_owners.back();
		}
		if (entry->retired || entry->outstanding.size() >= kPerOwner) return false;
		if (std::any_of(entry->outstanding.begin(), entry->outstanding.end(),
				[&](const Read& held) { return held.readId == read.readId; })) {
			return false;
		}
		Command command;
		command.kind = Command::Kind::Start;
		command.owner = owner;
		command.context = context;
		command.read = read;
		if (!EnqueueLocked(std::move(command))) return false;
		try {
			entry->outstanding.push_back(Read{ read.readId, context });
		} catch (...) {
			m_commands.pop_back();
			return false;
		}
	} catch (...) {
		return false;
	}
	m_work.notify_all();
	return true;
}

std::optional<senp::effect::ToolCompleted> CSenpControlToolReads::Take(
	const senp::ContributionOwnerIdentity& owner) noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto* entry = FindLocked(owner);
	if (!entry || entry->completions.empty()) return {};
	auto completion = std::move(entry->completions.front());
	entry->completions.pop_front();
	return completion;
}

void CSenpControlToolReads::Cancel(const senp::ContributionOwnerIdentity& owner,
	const senp::effect::OperationContext& context) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry || entry->retired) return;
		std::vector<std::wstring> cancelled;
		for (auto current = entry->outstanding.begin(); current != entry->outstanding.end();) {
			if (current->context.ownerGeneration == context.ownerGeneration
				&& current->context.requestGeneration == context.requestGeneration) {
				cancelled.push_back(current->readId);
				current = entry->outstanding.erase(current);
			} else {
				++current;
			}
		}
		if (cancelled.empty()) return;
		const auto isCancelled = [&](const std::wstring& readId) {
			return std::find(cancelled.begin(), cancelled.end(), readId) != cancelled.end();
		};
		// A terminal already drained for a cancelled read must not surface later.
		std::erase_if(entry->completions,
			[&](const senp::effect::ToolCompleted& held) { return isCancelled(held.readId); });
		// A read still queued here never reached the broker, so it needs no
		// cancellation on the wire - only the ones already dispatched do.
		std::vector<std::wstring> undispatched;
		std::erase_if(m_commands, [&](const Command& queued) {
			if (queued.kind != Command::Kind::Start || !(queued.owner == owner)
				|| !isCancelled(queued.read.readId)) {
				return false;
			}
			undispatched.push_back(queued.read.readId);
			return true;
		});
		for (auto& readId : cancelled) {
			if (std::find(undispatched.begin(), undispatched.end(), readId) != undispatched.end()) {
				continue;
			}
			Command command;
			command.kind = Command::Kind::Cancel;
			command.owner = owner;
			command.read.readId = readId;
			if (!EnqueueLocked(std::move(command))) break;
		}
	} catch (...) {
		// The reads are already forgotten here; a queue that could not take the
		// cancellation leaves the broker to time its own read out.
	}
	m_work.notify_all();
}

void CSenpControlToolReads::CancelAll(const senp::ContributionOwnerIdentity& owner) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry) return;
		entry->retired = true;
		std::erase_if(m_commands, [&](const Command& queued) {
			return queued.kind == Command::Kind::Start && queued.owner == owner;
		});
		for (const auto& read : entry->outstanding) {
			Command command;
			command.kind = Command::Kind::Cancel;
			command.owner = owner;
			command.read.readId = read.readId;
			if (!EnqueueLocked(std::move(command))) break;
		}
		entry->outstanding.clear();
		entry->completions.clear();
		Command retire;
		retire.kind = Command::Kind::Retire;
		retire.owner = owner;
		// Retire follows the cancellations in queue order, so the grant they need
		// is still cached when they run.
		static_cast<void>(EnqueueLocked(std::move(retire)));
	} catch (...) {
		// The owner is retired under the lock before anything here can throw, so
		// no completion of it can be routed even if the queue refused the work.
	}
	m_work.notify_all();
}

void CSenpControlToolReads::Stop() noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopped = true;
		m_commands.clear();
		for (auto& owner : m_owners) {
			owner.retired = true;
			owner.outstanding.clear();
			owner.completions.clear();
		}
	}
	m_work.notify_all();
	m_quiet.notify_all();
	// Stop closes the active channel, so a worker blocked in an exchange returns
	// instead of being waited on.
	m_client.Stop();
	if (m_worker.joinable()) m_worker.join();
}

ESenpControlToolReadsState CSenpControlToolReads::State() const noexcept
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopped) return ESenpControlToolReadsState::Stopped;
	}
	switch (m_client.State()) {
	case EControlSenpClientState::Connected:
		return ESenpControlToolReadsState::Connected;
	case EControlSenpClientState::Stopped:
		return ESenpControlToolReadsState::Stopped;
	default:
		return m_client.ConnectionEpoch() == 0
			? ESenpControlToolReadsState::Idle : ESenpControlToolReadsState::Disconnected;
	}
}

std::size_t CSenpControlToolReads::OutstandingReads() const noexcept
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return PendingLocked();
}

std::uint64_t CSenpControlToolReads::ConnectionEpoch() const noexcept
{
	return m_client.ConnectionEpoch();
}

bool CSenpControlToolReads::WaitForSettled(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	return m_quiet.wait_for(lock, timeout, [this] {
		return m_stopped || (m_commands.empty() && !m_busy && PendingLocked() == 0);
	});
}

// --- shared state ----------------------------------------------------------

CSenpControlToolReads::Owner* CSenpControlToolReads::FindLocked(
	const senp::ContributionOwnerIdentity& owner) noexcept
{
	const auto found = std::find_if(m_owners.begin(), m_owners.end(),
		[&](const Owner& held) { return held.identity == owner; });
	return found == m_owners.end() ? nullptr : &*found;
}

std::size_t CSenpControlToolReads::PendingLocked() const noexcept
{
	std::size_t pending = 0;
	for (const auto& owner : m_owners) pending += owner.outstanding.size();
	return pending;
}

void CSenpControlToolReads::PublishLocked(Owner& owner, std::wstring readId,
	senp::effect::CompletionStatus status, std::wstring message)
{
	// One admitted read yields one terminal and the outstanding list is bounded,
	// so this queue cannot outgrow the same bound.
	if (owner.completions.size() >= kPerOwner) return;
	owner.completions.push_back(senp::effect::ToolCompleted{ std::move(readId), status, {},
		std::move(message) });
}

bool CSenpControlToolReads::EnqueueLocked(Command command) noexcept
{
	if (m_stopped || m_commands.size() >= kMaximumQueuedCommands) return false;
	try {
		m_commands.push_back(std::move(command));
	} catch (...) {
		return false;
	}
	return true;
}

void CSenpControlToolReads::Complete(const senp::ContributionOwnerIdentity& owner,
	const std::wstring& readId, senp::effect::CompletionStatus status, std::wstring message) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry || entry->retired) return;
		const auto found = std::find_if(entry->outstanding.begin(), entry->outstanding.end(),
			[&](const Read& held) { return held.readId == readId; });
		if (found == entry->outstanding.end()) return;
		entry->outstanding.erase(found);
		PublishLocked(*entry, readId, status, std::move(message));
	} catch (...) {
	}
}

void CSenpControlToolReads::Route(const senp::ContributionOwnerIdentity& owner,
	senp::effect::ToolCompleted completion) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry || entry->retired) return;
		const auto found = std::find_if(entry->outstanding.begin(), entry->outstanding.end(),
			[&](const Read& held) { return held.readId == completion.readId; });
		// A terminal for a read this seam no longer holds was cancelled while in
		// flight. Dropping it here keeps the target from ever seeing it.
		if (found == entry->outstanding.end()) return;
		entry->outstanding.erase(found);
		if (entry->completions.size() >= kPerOwner) return;
		entry->completions.push_back(std::move(completion));
	} catch (...) {
	}
}

void CSenpControlToolReads::FailOwner(const senp::ContributionOwnerIdentity& owner,
	senp::effect::CompletionStatus status, std::wstring message) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry) return;
		auto outstanding = std::move(entry->outstanding);
		entry->outstanding.clear();
		if (entry->retired) return;
		for (auto& read : outstanding) PublishLocked(*entry, read.readId, status, message);
	} catch (...) {
	}
}

void CSenpControlToolReads::FailDispatched(senp::effect::CompletionStatus status,
	std::wstring message) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto& owner : m_owners) {
			for (auto current = owner.outstanding.begin(); current != owner.outstanding.end();) {
				if (current->epoch == 0) {
					++current;
					continue;
				}
				if (!owner.retired) PublishLocked(owner, current->readId, status, message);
				current = owner.outstanding.erase(current);
			}
		}
	} catch (...) {
	}
}

void CSenpControlToolReads::Dispatched(const senp::ContributionOwnerIdentity& owner,
	const std::wstring& readId, std::uint64_t epoch) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		auto* entry = FindLocked(owner);
		if (!entry) return;
		for (auto& read : entry->outstanding) {
			if (read.readId == readId) {
				read.epoch = epoch;
				return;
			}
		}
	} catch (...) {
	}
}

// --- worker thread ---------------------------------------------------------

void CSenpControlToolReads::Worker() noexcept
{
	for (;;) {
		Command command;
		bool run = false;
		bool poll = false;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			const auto ready = [this] { return !m_commands.empty() || m_stopped; };
			if (!ready()) {
				// A deadline only matters while something is outstanding; with an
				// empty scope the worker sleeps until it is given work.
				if (PendingLocked() != 0) m_work.wait_for(lock, m_options.pollInterval, ready);
				else m_work.wait(lock, ready);
			}
			if (m_stopped) break;
			if (!m_commands.empty()) {
				command = std::move(m_commands.front());
				m_commands.pop_front();
				run = true;
			} else {
				poll = PendingLocked() != 0;
			}
			m_busy = run || poll;
		}
		if (run) Run(std::move(command));
		else if (poll) Poll();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_busy = false;
		}
		m_quiet.notify_all();
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_busy = false;
	}
	m_quiet.notify_all();
}

void CSenpControlToolReads::Run(Command command) noexcept
{
	if (command.kind == Command::Kind::Retire) {
		std::erase_if(m_grants, [&](const Grant& grant) { return grant.identity == command.owner; });
		try {
			std::lock_guard<std::mutex> lock(m_mutex);
			std::erase_if(m_owners, [&](const Owner& owner) {
				return owner.retired && owner.identity == command.owner;
			});
		} catch (...) {
		}
		return;
	}
	if (command.kind == Command::Kind::Cancel) {
		// A cancellation never revives a connection: without one the broker lost
		// this read together with the grant that named it.
		if (m_client.State() != EControlSenpClientState::Connected) return;
		auto failure = senp::effect::CompletionStatus::Failed;
		const auto grant = Authorize(command.owner, failure);
		if (!grant) return;
		auto request = Compose(EControlSenpRpcOperation::CancelRead, command.owner, *grant);
		request.readId = command.read.readId;
		const auto answer = m_client.Execute(request);
		if (!answer.Answered()) m_client.Disconnect();
		return;
	}

	if (!Ensure()) {
		Complete(command.owner, command.read.readId,
			senp::effect::CompletionStatus::HostUnavailable, kUnavailable);
		return;
	}
	auto failure = senp::effect::CompletionStatus::Failed;
	const auto grant = Authorize(command.owner, failure);
	if (!grant) {
		if (failure == senp::effect::CompletionStatus::HostUnavailable) {
			Complete(command.owner, command.read.readId, failure, kLost);
			return;
		}
		Retry(std::move(command), kRefused);
		return;
	}
	auto request = Compose(EControlSenpRpcOperation::StartRead, command.owner, *grant);
	request.readId = command.read.readId;
	request.toolId = command.read.toolId;
	request.toolOperation = command.read.operation;
	request.arguments = command.read.arguments;
	const auto answer = m_client.Execute(request);
	if (!answer.Answered()) {
		m_client.Disconnect();
		Complete(command.owner, command.read.readId,
			senp::effect::CompletionStatus::HostUnavailable, kLost);
		return;
	}
	switch (answer.response.status) {
	case EControlSenpRpcStatus::Succeeded:
		// Admitted only. The terminal arrives through Poll, and the read now
		// belongs to this connection: losing it loses the read.
		Dispatched(command.owner, command.read.readId, m_client.ConnectionEpoch());
		return;
	case EControlSenpRpcStatus::Busy:
	case EControlSenpRpcStatus::ResourceExhausted:
		Retry(std::move(command), kSaturated);
		return;
	case EControlSenpRpcStatus::Expired:
	case EControlSenpRpcStatus::Unauthorized:
		// The record may simply have outlived its lifetime; mint a new one and
		// let the bounded retry decide whether the refusal was real.
		Forget(command.owner);
		Retry(std::move(command), kRefused);
		return;
	default:
		Complete(command.owner, command.read.readId,
			ToCompletion(answer.response.status), kRefused);
		return;
	}
}

void CSenpControlToolReads::Retry(Command command, std::wstring message) noexcept
{
	if (++command.attempts >= kMaximumAttempts) {
		Complete(command.owner, command.read.readId,
			senp::effect::CompletionStatus::Failed, std::move(message));
		return;
	}
	const auto owner = command.owner;
	const auto readId = command.read.readId;
	bool queued = false;
	bool stopped = false;
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		stopped = m_stopped;
		queued = EnqueueLocked(std::move(command));
	} catch (...) {
	}
	if (!queued) {
		Complete(owner, readId, stopped
			? senp::effect::CompletionStatus::Cancelled : senp::effect::CompletionStatus::Failed,
			stopped ? kStopped : std::move(message));
		return;
	}
	m_work.notify_all();
}

void CSenpControlToolReads::Poll() noexcept
{
	if (m_client.State() != EControlSenpClientState::Connected) {
		FailDispatched(senp::effect::CompletionStatus::HostUnavailable, kUnavailable);
		return;
	}
	std::vector<senp::ContributionOwnerIdentity> scopes;
	try {
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& owner : m_owners) {
			if (!owner.retired && !owner.outstanding.empty()) scopes.push_back(owner.identity);
		}
	} catch (...) {
		return;
	}
	for (const auto& scope : scopes) {
		auto failure = senp::effect::CompletionStatus::Failed;
		const auto grant = Authorize(scope, failure);
		if (!grant) {
			if (failure == senp::effect::CompletionStatus::HostUnavailable) {
				FailDispatched(failure, kLost);
				return;
			}
			FailOwner(scope, failure, kRefused);
			continue;
		}
		// One poll drains one terminal, so a scope with several finished reads is
		// drained within this cycle rather than one terminal per interval.
		for (std::size_t drained = 0; drained < kPerOwner; ++drained) {
			const auto answer = m_client.Execute(
				Compose(EControlSenpRpcOperation::PollRead, scope, *grant));
			if (!answer.Answered()) {
				m_client.Disconnect();
				FailDispatched(senp::effect::CompletionStatus::HostUnavailable, kLost);
				return;
			}
			if (answer.response.status != EControlSenpRpcStatus::Succeeded) {
				// A grant that expired between two polls is replaced on the next
				// cycle; anything else is this scope's own failure.
				if (answer.response.status == EControlSenpRpcStatus::Expired
					|| answer.response.status == EControlSenpRpcStatus::Unauthorized) {
					Forget(scope);
				} else {
					FailOwner(scope, ToCompletion(answer.response.status), kRefused);
				}
				break;
			}
			if (!answer.response.hasCompletion) break;
			Route(scope, answer.response.completion);
		}
	}
}

bool CSenpControlToolReads::Ensure() noexcept
{
	if (m_client.State() == EControlSenpClientState::Connected) return true;
	const auto connected = m_client.Connect();
	if (!connected.IsConnected()) return false;
	// A new connection voids every grant minted on the old one, and with them
	// every read the broker had admitted under those grants. A read this seam
	// has not put on the wire yet survives: it is dispatched on this connection.
	m_grants.clear();
	FailDispatched(senp::effect::CompletionStatus::HostUnavailable, kReplaced);
	return true;
}

std::optional<std::string> CSenpControlToolReads::Authorize(
	const senp::ContributionOwnerIdentity& owner, senp::effect::CompletionStatus& failure) noexcept
{
	try {
		const auto epoch = m_client.ConnectionEpoch();
		const auto found = std::find_if(m_grants.begin(), m_grants.end(),
			[&](const Grant& grant) { return grant.identity == owner; });
		if (found != m_grants.end()) {
			if (found->epoch == epoch) return found->grantId;
			m_grants.erase(found);
		}
		if (m_grants.size() >= kMaximumOwners) {
			failure = senp::effect::CompletionStatus::Failed;
			return {};
		}
		auto request = Compose(EControlSenpRpcOperation::IssueGrant, owner, {});
		request.capabilities = static_cast<std::uint32_t>(senp::SenpToolCapability::GitHubRepositoryRead);
		const auto answer = m_client.Execute(request);
		if (!answer.Answered()) {
			m_client.Disconnect();
			failure = senp::effect::CompletionStatus::HostUnavailable;
			return {};
		}
		if (answer.response.status != EControlSenpRpcStatus::Succeeded
			|| answer.response.grantId.empty()) {
			failure = ToCompletion(answer.response.status);
			return {};
		}
		m_grants.push_back(Grant{ owner, answer.response.grantId, epoch });
		return answer.response.grantId;
	} catch (...) {
		failure = senp::effect::CompletionStatus::Failed;
		return {};
	}
}

void CSenpControlToolReads::Forget(const senp::ContributionOwnerIdentity& owner) noexcept
{
	std::erase_if(m_grants, [&](const Grant& grant) { return grant.identity == owner; });
}

platform::controlipc::ControlSenpRpcRequest CSenpControlToolReads::Compose(
	EControlSenpRpcOperation operation, const senp::ContributionOwnerIdentity& owner,
	const std::string& grantId) const
{
	ControlSenpRpcRequest request;
	request.operation = operation;
	request.profileId = m_options.senpProfileId;
	request.owner = platform::controlipc::FromContributionOwner(owner);
	request.grantId = grantId;
	return request;
}

} // namespace workbench::editor
