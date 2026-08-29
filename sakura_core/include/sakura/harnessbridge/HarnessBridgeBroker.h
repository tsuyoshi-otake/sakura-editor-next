/*! @file
    @brief Bounded in-process broker for structured Harness messages and runs.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeProtocol.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace platform::harnessbridge {

struct HarnessOpaqueId final {
	std::array<std::uint8_t, 16> value{};
	[[nodiscard]] bool IsValid() const noexcept;
	friend bool operator==(const HarnessOpaqueId&, const HarnessOpaqueId&) noexcept = default;
	friend bool operator<(const HarnessOpaqueId& left, const HarnessOpaqueId& right) noexcept
	{
		return left.value < right.value;
	}
};

using HarnessEndpointId = HarnessOpaqueId;
using HarnessMessageId = HarnessOpaqueId;
using HarnessRunId = HarnessOpaqueId;

enum class EHarnessBrokerStatus : std::uint8_t {
	Accepted,
	Duplicate,
	Conflict,
	Succeeded,
	InvalidRequest,
	UnknownEndpoint,
	UnknownMessage,
	UnknownRun,
	AccessDenied,
	ResourceExhausted,
	AlreadyTerminal,
	DeadlineExceeded,
	Cancelled,
	BrokerStopping,
};

enum class EHarnessRunTerminalStatus : std::uint8_t {
	Succeeded,
	Failed,
	Cancelled,
	TimedOut,
	HarnessExited,
};

struct HarnessBrokerLimits final {
	std::size_t maximumEndpoints = 64;
	std::size_t maximumQueuedMessagesPerEndpoint = 128;
	std::size_t maximumInflightMessagesPerEndpoint = 128;
	std::size_t maximumMessagePayloadBytes = 256u * 1024u;
	std::size_t maximumQueuedMessageBytesPerEndpoint = 1024u * 1024u;
	std::size_t maximumDedupeRecords = 4096;
	std::size_t maximumHopCount = 8;
	std::size_t maximumRuns = 1024;
};

struct HarnessEndpointRegistration final {
	HarnessEndpointId endpointId;
	std::string displayName;
	std::string scope;
	EHarnessGrant grants = EHarnessGrant::None;
	std::size_t maximumQueue = 128;
};

struct HarnessEndpointInfo final {
	HarnessEndpointId endpointId;
	std::string displayName;
	std::string scope;
	EHarnessGrant grants = EHarnessGrant::None;
};

struct HarnessMessage final {
	HarnessMessageId messageId;
	HarnessRunId runId;
	HarnessEndpointId sender;
	HarnessEndpointId recipient;
	HarnessMessageId replyTo;
	std::uint8_t hopCount = 0;
	std::string type;
	std::vector<std::uint8_t> payload;
	std::chrono::steady_clock::time_point deadline{};
};

struct HarnessMessageDelivery final {
	HarnessMessage message;
	std::uint64_t deliveryAttempt = 1;
};

struct HarnessRunResult final {
	HarnessRunId runId;
	EHarnessRunTerminalStatus status = EHarnessRunTerminalStatus::Failed;
	std::int32_t exitCode = 0;
	std::uint64_t completedAtTick = 0;
};

struct HarnessBrokerResult final {
	EHarnessBrokerStatus status = EHarnessBrokerStatus::InvalidRequest;
	std::vector<HarnessMessageDelivery> messages;
	std::optional<HarnessRunResult> run;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EHarnessBrokerStatus::Accepted || status == EHarnessBrokerStatus::Succeeded;
	}
};

enum class EHarnessBrokerState : std::uint8_t {
	Stopped,
	Running,
	Stopping,
};

//! Thread-safe bounded broker. It never logs or stores diagnostic copies of payload data.
class CHarnessBridgeBroker final {
public:
	explicit CHarnessBridgeBroker(HarnessBrokerLimits limits = {});
	~CHarnessBridgeBroker();
	CHarnessBridgeBroker(const CHarnessBridgeBroker&) = delete;
	CHarnessBridgeBroker& operator=(const CHarnessBridgeBroker&) = delete;

	[[nodiscard]] EHarnessBrokerStatus Start() noexcept;
	void Stop() noexcept;
	[[nodiscard]] EHarnessBrokerState State() const noexcept;

	[[nodiscard]] HarnessBrokerResult RegisterEndpoint(const HarnessEndpointRegistration& registration,
		const HarnessBridgeTargetDescriptor& authority);
	[[nodiscard]] HarnessBrokerResult UnregisterEndpoint(const HarnessEndpointId& endpoint,
		const HarnessBridgeTargetDescriptor& authority);
	[[nodiscard]] std::vector<HarnessEndpointInfo> ListEndpoints() const;

	[[nodiscard]] HarnessBrokerResult SendEndpointMessage(const HarnessMessage& message,
		const HarnessBridgeTargetDescriptor& authority);
	[[nodiscard]] HarnessBrokerResult ReceiveMessages(const HarnessEndpointId& recipient,
		std::size_t maximumMessages,
		std::chrono::steady_clock::time_point deadline,
		const HarnessBridgeTargetDescriptor& authority);
	[[nodiscard]] HarnessBrokerResult AcknowledgeMessage(const HarnessEndpointId& recipient,
		const HarnessMessageId& messageId,
		const HarnessBridgeTargetDescriptor& authority);

	//! Publishes one and only one terminal result for a run ID.
	[[nodiscard]] HarnessBrokerResult BeginRun(const HarnessRunId& run);
	[[nodiscard]] HarnessBrokerResult PublishRunResult(const HarnessRunResult& result);
	[[nodiscard]] HarnessBrokerResult WaitRun(const HarnessRunId& run,
		std::chrono::steady_clock::time_point deadline) const;

private:
	struct EndpointState final {
		HarnessEndpointRegistration registration;
		HarnessBridgeTargetDescriptor authority;
		std::deque<HarnessMessageDelivery> queued;
		std::map<HarnessMessageId, HarnessMessageDelivery> inflight;
		std::deque<HarnessMessageId> inflightOrder;
		std::size_t queuedBytes = 0;
		std::size_t inflightBytes = 0;
	};
	enum class MessageDedupeState : std::uint8_t {
		Pending,
		Inflight,
		Acknowledged,
		Rejected,
	};
	struct MessageDedupeRecord final {
		HarnessEndpointId sender;
		HarnessEndpointId recipient;
		HarnessRunId runId;
		HarnessMessageId replyTo;
		std::uint8_t hopCount{};
		std::string type;
		std::chrono::steady_clock::time_point deadline{};
		std::size_t payloadBytes{};
		std::array<std::uint8_t, 32> payloadDigest{};
		MessageDedupeState state = MessageDedupeState::Pending;
	};

	[[nodiscard]] bool IsRunningLocked() const noexcept;
	[[nodiscard]] static bool IsValidText(std::string_view value, std::size_t limit) noexcept;
	[[nodiscard]] static bool IsValidAuthority(const HarnessBridgeTargetDescriptor& authority) noexcept;
	[[nodiscard]] static bool IsValidId(const HarnessOpaqueId& id) noexcept { return id.IsValid(); }
	[[nodiscard]] static std::optional<std::array<std::uint8_t, 32>> ComputePayloadDigest(
		const std::vector<std::uint8_t>& payload) noexcept;
	[[nodiscard]] static bool MatchesDedupeRecord(
		const MessageDedupeRecord& record, const HarnessMessage& message,
		const std::array<std::uint8_t, 32>& payloadDigest) noexcept;
	[[nodiscard]] bool EnsureDedupeCapacityLocked() noexcept;
	void RejectEndpointMessagesLocked(EndpointState& endpoint) noexcept;

	HarnessBrokerLimits m_limits;
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_condition;
	EHarnessBrokerState m_state = EHarnessBrokerState::Stopped;
	std::map<HarnessEndpointId, EndpointState> m_endpoints;
	std::map<HarnessMessageId, MessageDedupeRecord> m_messageRecords;
	std::deque<HarnessMessageId> m_dedupeOrder;
	std::map<HarnessRunId, std::optional<HarnessRunResult>> m_runs;
};

} // namespace platform::harnessbridge
