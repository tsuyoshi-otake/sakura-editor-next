/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpEffectProtocol.h"
#include <chrono>
#include <memory>

namespace senp {

enum class RuntimePhase : std::uint8_t { Idle, Handshaking, Activating, Active, Stopping, Stopped };
enum class InvocationStatus : std::uint8_t { EffectsReady, Cancelled, TimedOut, Rejected, HostUnavailable, ProtocolError };
enum class AdmissionStatus : std::uint8_t { Accepted, Busy, InvalidRequest, Unavailable };

struct InvocationResult final {
	effect::OperationContext context{};
	bool activation{};
	InvocationStatus status{};
	effect::RejectionCode rejection{};
	std::vector<effect::Effect> effects{};
};

struct InvocationAdmission final {
	AdmissionStatus status{};
	std::wstring operationId{};
};

struct RuntimeFrame final {
	std::string bytes{};
	bool expectsReply{};
};

//! Serialized by its runtime owner. No OS handles, threads or caller callbacks.
//! Completion of a Wasm invocation does not finalize its emitted tool reads.
class CSenpRuntimeSession final {
public:
	using Time = std::chrono::steady_clock::time_point;
	static constexpr std::size_t kMaximumPending = 16;
	static constexpr std::size_t kMaximumQueuedBytes = 4U * 1024U * 1024U;
	static constexpr auto kMaximumLifetime = std::chrono::seconds(10);

	explicit CSenpRuntimeSession(std::int64_t generation);
	~CSenpRuntimeSession();
	CSenpRuntimeSession(const CSenpRuntimeSession&) = delete;
	CSenpRuntimeSession& operator=(const CSenpRuntimeSession&) = delete;

	[[nodiscard]] InvocationAdmission Begin(std::wstring extensionId,
		effect::OperationContext context, Time deadline, Time now);
	[[nodiscard]] InvocationAdmission Submit(effect::OperationContext context,
		effect::Event event, Time deadline, Time now);
	bool Cancel(std::wstring_view operationId);
	void Expire(Time now);
	void Stop(effect::StopReason reason);
	void TransportFailed(InvocationStatus status = InvocationStatus::HostUnavailable);
	void Receive(std::string_view bytes);
	[[nodiscard]] std::optional<RuntimeFrame> TakeOutgoing();
	[[nodiscard]] std::optional<InvocationResult> TakeCompleted();
	[[nodiscard]] RuntimePhase Phase() const;
	[[nodiscard]] std::size_t PendingCount() const;
	[[nodiscard]] std::size_t CompletionCount() const;
	[[nodiscard]] std::size_t QueuedBytes() const;
	[[nodiscard]] std::optional<Time> NextDeadline() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace senp
