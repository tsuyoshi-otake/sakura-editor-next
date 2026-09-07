/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpRuntimeSession.h"
#include <algorithm>
#include <deque>
#include <map>

namespace senp {
using namespace effect;

struct CSenpRuntimeSession::Impl final {
	struct Pending final {
		OperationContext context{};
		Time deadline{};
		bool activation{};
		std::int64_t sentSequence{};
	};
	struct Queued final {
		Message body{};
		std::wstring operationId{};
		std::size_t reservedBytes{};
	};
	std::int64_t generation{};
	std::int64_t inputSequence{};
	std::int64_t outputSequence{};
	std::int64_t ticket{};
	std::int64_t owner{};
	RuntimePhase phase{ RuntimePhase::Idle };
	std::map<std::wstring, Pending, std::less<>> pending{};
	std::deque<InvocationResult> completed{};
	std::deque<Queued> outgoing{};
	std::map<std::int64_t, std::string> receipts{};
	std::size_t queuedBytes{};
	std::optional<Activate> activation{};

	void Finish(std::map<std::wstring, Pending, std::less<>>::iterator found,
		InvocationStatus status, std::vector<Effect> effects = {}, RejectionCode rejection = {})
	{
		const auto operationId = found->first;
		completed.push_back({ std::move(found->second.context), found->second.activation,
			status, rejection, std::move(effects) });
		pending.erase(found);
		// Sequence numbers are assigned only when a frame is taken by the pipe
		// owner, so a cancelled unsent request leaves no gap in the stream.
		std::erase_if(outgoing, [&](const auto& item) {
			if (item.operationId != operationId) return false;
			queuedBytes -= item.reservedBytes;
			return true;
		});
	}

	void InvalidateCompleted(InvocationStatus status)
	{
		for (auto& result : completed) {
			if (result.status != InvocationStatus::EffectsReady) continue;
			result.status = status;
			result.effects.clear();
		}
	}

	void Fail(InvocationStatus status)
	{
		while (!pending.empty()) Finish(pending.begin(), status);
		InvalidateCompleted(status);
		phase = RuntimePhase::Stopped;
		outgoing.clear();
		queuedBytes = 0;
		receipts.clear();
		activation.reset();
	}

	AdmissionStatus Queue(Message body, std::wstring operationId = {})
	{
		// Reserve using the largest legal sequence representation; all actual
		// encodings are no larger. Control frames have their own bounded reserve.
		Envelope candidate{ 2, kMaximumCounter, generation, std::move(body) };
		auto bytes = Encode(candidate);
		if (!bytes) return AdmissionStatus::InvalidRequest;
		const auto limit = kMaximumQueuedBytes + (operationId.empty() ? 65536U : 0U);
		if (outgoing.size() >= 64 || bytes->size() > limit - (std::min)(limit, queuedBytes)) return AdmissionStatus::Busy;
		queuedBytes += bytes->size();
		outgoing.push_back({ std::move(candidate.body), std::move(operationId), bytes->size() });
		return AdmissionStatus::Accepted;
	}

	void AckResponse(std::int64_t sequence)
	{
		if (Queue(Ack{ sequence }) != AdmissionStatus::Accepted) Fail(InvocationStatus::ProtocolError);
	}

	std::wstring MintOperation()
	{
		if (ticket == kMaximumCounter) return {};
		return L"s" + std::to_wstring(generation) + L":o" + std::to_wstring(++ticket);
	}

	bool WasMinted(std::wstring_view operationId) const
	{
		const auto prefix = L"s" + std::to_wstring(generation) + L":o";
		if (!operationId.starts_with(prefix)) return false;
		operationId.remove_prefix(prefix.size());
		if (operationId.empty() || operationId.front() == L'0') return false;
		std::int64_t value{};
		for (const auto ch : operationId) {
			if (ch < L'0' || ch > L'9' || value > (kMaximumCounter - (ch - L'0')) / 10) return false;
			value = value * 10 + ch - L'0';
		}
		return value <= ticket;
	}
};

CSenpRuntimeSession::CSenpRuntimeSession(std::int64_t generation) : m_impl(std::make_unique<Impl>())
{
	m_impl->generation = generation;
	if (generation <= 0) m_impl->phase = RuntimePhase::Stopped;
}
CSenpRuntimeSession::~CSenpRuntimeSession() = default;

InvocationAdmission CSenpRuntimeSession::Begin(std::wstring extensionId,
	OperationContext context, Time deadline, Time now)
{
	auto& s = *m_impl;
	if (s.phase != RuntimePhase::Idle) return { AdmissionStatus::Unavailable };
	if (deadline <= now || deadline - now > kMaximumLifetime) return { AdmissionStatus::InvalidRequest };
	context.operationId = s.MintOperation();
	if (context.operationId.empty()) { s.Fail(InvocationStatus::ProtocolError); return { AdmissionStatus::Unavailable }; }
	Activate activation{ context, std::move(extensionId) };
	Envelope candidate{ 2, 1, s.generation, std::move(activation) };
	if (!Validate(candidate)) return { AdmissionStatus::InvalidRequest };
	activation = std::get<Activate>(std::move(candidate.body));
	if (s.Queue(Hello{ std::wstring(kAbi) }) != AdmissionStatus::Accepted) return { AdmissionStatus::InvalidRequest };
	s.owner = context.ownerGeneration;
	s.pending.emplace(context.operationId, Impl::Pending{ context, deadline, true, 0 });
	s.activation = std::move(activation);
	s.phase = RuntimePhase::Handshaking;
	return { AdmissionStatus::Accepted, context.operationId };
}

InvocationAdmission CSenpRuntimeSession::Submit(OperationContext context,
	Event event, Time deadline, Time now)
{
	auto& s = *m_impl;
	if (s.phase != RuntimePhase::Active) return { AdmissionStatus::Unavailable };
	if (s.pending.size() + s.completed.size() >= kMaximumPending) return { AdmissionStatus::Busy };
	if (deadline <= now || deadline - now > kMaximumLifetime || context.ownerGeneration != s.owner)
		return { AdmissionStatus::InvalidRequest };
	context.operationId = s.MintOperation();
	if (context.operationId.empty()) { s.Fail(InvocationStatus::ProtocolError); return { AdmissionStatus::Unavailable }; }
	EventMessage message{ context, std::move(event) };
	const auto queued = s.Queue(std::move(message), context.operationId);
	if (queued != AdmissionStatus::Accepted) return { queued };
	s.pending.emplace(context.operationId, Impl::Pending{ context, deadline, false, 0 });
	return { AdmissionStatus::Accepted, context.operationId };
}

bool CSenpRuntimeSession::Cancel(std::wstring_view operationId)
{
	auto& s = *m_impl;
	const auto found = s.pending.find(operationId);
	if (found == s.pending.end()) return false;
	const auto activation = found->second.activation;
	s.Finish(found, InvocationStatus::Cancelled);
	if (activation) Stop(StopReason::Disabled);
	return true;
}

void CSenpRuntimeSession::Expire(Time now)
{
	auto& s = *m_impl;
	bool expiredActivation{};
	for (auto it = s.pending.begin(); it != s.pending.end();) {
		if (it->second.deadline > now) { ++it; continue; }
		expiredActivation |= it->second.activation;
		s.Finish(it++, InvocationStatus::TimedOut);
	}
	if (expiredActivation) Stop(StopReason::HostUnavailable);
}

void CSenpRuntimeSession::Stop(StopReason reason)
{
	auto& s = *m_impl;
	if (s.phase == RuntimePhase::Stopped || s.phase == RuntimePhase::Stopping) return;
	s.InvalidateCompleted(InvocationStatus::Cancelled);
	if (s.phase == RuntimePhase::Idle || s.phase == RuntimePhase::Handshaking) {
		s.Fail(InvocationStatus::Cancelled);
		return;
	}
	while (!s.pending.empty()) s.Finish(s.pending.begin(), InvocationStatus::Cancelled);
	s.phase = RuntimePhase::Stopping;
	s.activation.reset();
	if (s.Queue(Deactivate{ reason }) != AdmissionStatus::Accepted) s.Fail(InvocationStatus::HostUnavailable);
}

void CSenpRuntimeSession::TransportFailed(InvocationStatus status)
{
	m_impl->Fail(status);
}

void CSenpRuntimeSession::Receive(std::string_view bytes)
{
	auto& s = *m_impl;
	if (s.phase == RuntimePhase::Stopped) return;
	auto response = Decode(bytes);
	if (!response || response->sessionGeneration != s.generation) {
		s.Fail(InvocationStatus::ProtocolError);
		return;
	}
	const auto canonical = Encode(*response);
	if (!canonical) { s.Fail(InvocationStatus::ProtocolError); return; }
	if (response->sequence <= s.inputSequence) {
		const auto receipt = s.receipts.find(response->sequence);
		if (receipt != s.receipts.end() && receipt->second != *canonical) {
			s.Fail(InvocationStatus::ProtocolError);
			return;
		}
		s.AckResponse(response->sequence);
		return;
	}
	if (response->sequence != s.inputSequence + 1) { s.Fail(InvocationStatus::ProtocolError); return; }
	s.inputSequence = response->sequence;
	s.receipts.emplace(response->sequence, *canonical);
	if (s.receipts.size() > kMaximumPending) s.receipts.erase(s.receipts.begin());
	if (const auto stopped = std::get_if<Stopped>(&response->body)) {
		s.Fail(stopped->reason == StopReason::ProtocolError ? InvocationStatus::ProtocolError : InvocationStatus::HostUnavailable);
		return;
	}
	if (std::holds_alternative<Hello>(response->body)) {
		if (s.phase != RuntimePhase::Handshaking || !s.activation || s.outputSequence == 0) {
			s.Fail(InvocationStatus::ProtocolError);
			return;
		}
		s.AckResponse(response->sequence);
		if (s.phase == RuntimePhase::Stopped) return;
		if (s.Queue(*s.activation, s.activation->context.operationId) != AdmissionStatus::Accepted) { s.Fail(InvocationStatus::ProtocolError); return; }
		s.phase = RuntimePhase::Activating;
		return;
	}
	if (auto result = std::get_if<EffectsMessage>(&response->body)) {
		const auto found = s.pending.find(result->context.operationId);
		if (found == s.pending.end()) {
			if (!s.WasMinted(result->context.operationId)) { s.Fail(InvocationStatus::ProtocolError); return; }
			// Cancellation/deadline already owns the terminal notification.
			s.AckResponse(response->sequence);
			return;
		}
		if (found->second.sentSequence == 0 || found->second.context != result->context) {
			s.Fail(InvocationStatus::ProtocolError);
			return;
		}
		if (found->second.activation) {
			if (s.phase != RuntimePhase::Activating) { s.Fail(InvocationStatus::ProtocolError); return; }
			s.phase = RuntimePhase::Active;
			s.activation.reset();
		}
		s.Finish(found, InvocationStatus::EffectsReady, std::move(result->effects));
		s.AckResponse(response->sequence);
		return;
	}
	if (const auto rejection = std::get_if<Rejected>(&response->body)) {
		if (rejection->requestSequence > s.outputSequence) { s.Fail(InvocationStatus::ProtocolError); return; }
		const auto found = std::find_if(s.pending.begin(), s.pending.end(), [&](const auto& item) {
			return item.second.sentSequence == rejection->requestSequence;
		});
		if (found != s.pending.end()) {
			const auto activation = found->second.activation;
			s.Finish(found, InvocationStatus::Rejected, {}, rejection->code);
			if (activation) { s.Fail(InvocationStatus::HostUnavailable); return; }
		}
		s.AckResponse(response->sequence);
		return;
	}
	s.Fail(InvocationStatus::ProtocolError);
}

std::optional<RuntimeFrame> CSenpRuntimeSession::TakeOutgoing()
{
	auto& s = *m_impl;
	if (s.outgoing.empty() || s.phase == RuntimePhase::Stopped) return std::nullopt;
	if (s.outputSequence == kMaximumCounter) { s.Fail(InvocationStatus::ProtocolError); return std::nullopt; }
	auto item = std::move(s.outgoing.front());
	s.outgoing.pop_front();
	s.queuedBytes -= item.reservedBytes;
	const auto sequence = ++s.outputSequence;
	const auto expectsReply = !std::holds_alternative<Ack>(item.body);
	auto bytes = Encode({ 2, sequence, s.generation, std::move(item.body) });
	if (!bytes) { s.Fail(InvocationStatus::ProtocolError); return std::nullopt; }
	if (!item.operationId.empty()) {
		const auto found = s.pending.find(item.operationId);
		if (found == s.pending.end()) { s.Fail(InvocationStatus::ProtocolError); return std::nullopt; }
		found->second.sentSequence = sequence;
	}
	return RuntimeFrame{ std::move(*bytes), expectsReply };
}

std::optional<InvocationResult> CSenpRuntimeSession::TakeCompleted()
{
	if (m_impl->completed.empty()) return std::nullopt;
	auto result = std::move(m_impl->completed.front());
	m_impl->completed.pop_front();
	return result;
}

RuntimePhase CSenpRuntimeSession::Phase() const { return m_impl->phase; }
std::size_t CSenpRuntimeSession::PendingCount() const { return m_impl->pending.size(); }
std::size_t CSenpRuntimeSession::CompletionCount() const { return m_impl->completed.size(); }
std::size_t CSenpRuntimeSession::QueuedBytes() const { return m_impl->queuedBytes; }
std::optional<CSenpRuntimeSession::Time> CSenpRuntimeSession::NextDeadline() const
{
	std::optional<Time> result;
	for (const auto& [_, pending] : m_impl->pending) {
		if (!result || pending.deadline < *result) result = pending.deadline;
	}
	return result;
}

} // namespace senp
