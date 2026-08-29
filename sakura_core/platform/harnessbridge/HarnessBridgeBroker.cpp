/*! @file */
#include <sakura/harnessbridge/HarnessBridgeBroker.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>

namespace platform::harnessbridge {

bool HarnessOpaqueId::IsValid() const noexcept
{
	for (const auto byte : value) if (byte != 0) return true;
	return false;
}

CHarnessBridgeBroker::CHarnessBridgeBroker(const HarnessBrokerLimits limits)
	: m_limits(limits)
{
	m_limits.maximumEndpoints = (std::max)(std::size_t{ 1 }, m_limits.maximumEndpoints);
	m_limits.maximumQueuedMessagesPerEndpoint = (std::max)(std::size_t{ 1 }, m_limits.maximumQueuedMessagesPerEndpoint);
	m_limits.maximumInflightMessagesPerEndpoint = (std::max)(std::size_t{ 1 }, m_limits.maximumInflightMessagesPerEndpoint);
	m_limits.maximumMessagePayloadBytes = (std::max)(std::size_t{ 1 }, m_limits.maximumMessagePayloadBytes);
	m_limits.maximumQueuedMessageBytesPerEndpoint = (std::max)(std::size_t{ 1 }, m_limits.maximumQueuedMessageBytesPerEndpoint);
	m_limits.maximumDedupeRecords = (std::max)(std::size_t{ 1 }, m_limits.maximumDedupeRecords);
	m_limits.maximumHopCount = (std::min)(m_limits.maximumHopCount, std::size_t{ 255 });
	m_limits.maximumRuns = (std::max)(std::size_t{ 1 }, m_limits.maximumRuns);
}

CHarnessBridgeBroker::~CHarnessBridgeBroker()
{
	Stop();
}

EHarnessBrokerStatus CHarnessBridgeBroker::Start() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_state == EHarnessBrokerState::Running) return EHarnessBrokerStatus::Succeeded;
	if (m_state == EHarnessBrokerState::Stopping) return EHarnessBrokerStatus::BrokerStopping;
	m_state = EHarnessBrokerState::Running;
	m_condition.notify_all();
	return EHarnessBrokerStatus::Accepted;
}

void CHarnessBridgeBroker::Stop() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		if (m_state == EHarnessBrokerState::Stopped) return;
		m_state = EHarnessBrokerState::Stopping;
		for (auto& [id, result] : m_runs) {
			(void)id;
			if (!result) result = HarnessRunResult{ id, EHarnessRunTerminalStatus::Cancelled, 0, 0 };
		}
		m_endpoints.clear();
		m_state = EHarnessBrokerState::Stopped;
	}
	m_condition.notify_all();
}

EHarnessBrokerState CHarnessBridgeBroker::State() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_state;
}

bool CHarnessBridgeBroker::IsRunningLocked() const noexcept
{
	return m_state == EHarnessBrokerState::Running;
}

bool CHarnessBridgeBroker::IsValidText(const std::string_view value, const std::size_t limit) noexcept
{
	if (value.empty() || value.size() > limit) return false;
	for (const auto character : value) if (character == '\0') return false;
	return IsValidHarnessBridgeUtf8(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool CHarnessBridgeBroker::IsValidAuthority(const HarnessBridgeTargetDescriptor& authority) noexcept
{
	const auto editorIsValid = std::any_of(authority.editorId.begin(), authority.editorId.end(),
		[](const std::uint8_t byte) { return byte != 0; });
	return IsValidText(authority.profileId, 256) && authority.profileGeneration != 0
		&& editorIsValid && authority.bridgeEpoch != 0 && authority.runtimeGeneration != 0
		&& authority.instanceGeneration != 0 && authority.sessionId != 0
		&& authority.windowId != 0 && authority.paneId != 0 && authority.instanceId != 0;
}

std::optional<std::array<std::uint8_t, 32>> CHarnessBridgeBroker::ComputePayloadDigest(
	const std::vector<std::uint8_t>& payload) noexcept
{
	BCRYPT_ALG_HANDLE algorithm{};
	BCRYPT_HASH_HANDLE hash{};
	std::vector<std::uint8_t> object;
	std::array<std::uint8_t, 32> digest{};
	DWORD objectBytes{};
	DWORD copied{};
	bool succeeded = BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
		&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
	if (succeeded) {
		succeeded = BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0));
	}
	try {
		if (succeeded) object.resize(objectBytes);
	} catch (...) {
		succeeded = false;
	}
	if (succeeded) {
		succeeded = BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash,
			object.data(), objectBytes, nullptr, 0, 0));
	}
	if (succeeded && !payload.empty()) {
		if (payload.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
			succeeded = false;
		} else {
			succeeded = BCRYPT_SUCCESS(::BCryptHashData(hash,
				const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(payload.data())),
				static_cast<ULONG>(payload.size()), 0));
		}
	}
	if (succeeded) {
		succeeded = BCRYPT_SUCCESS(::BCryptFinishHash(
			hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
	}
	if (hash) (void)::BCryptDestroyHash(hash);
	if (algorithm) (void)::BCryptCloseAlgorithmProvider(algorithm, 0);
	return succeeded ? std::optional{ digest } : std::nullopt;
}

bool CHarnessBridgeBroker::MatchesDedupeRecord(
	const MessageDedupeRecord& record, const HarnessMessage& message,
	const std::array<std::uint8_t, 32>& payloadDigest) noexcept
{
	return record.sender == message.sender && record.recipient == message.recipient
		&& record.runId == message.runId && record.replyTo == message.replyTo
		&& record.hopCount == message.hopCount && record.type == message.type
		&& record.deadline == message.deadline && record.payloadBytes == message.payload.size()
		&& record.payloadDigest == payloadDigest;
}

bool CHarnessBridgeBroker::EnsureDedupeCapacityLocked() noexcept
{
	if (m_messageRecords.size() < m_limits.maximumDedupeRecords) return true;
	for (auto order = m_dedupeOrder.begin(); order != m_dedupeOrder.end(); ++order) {
		const auto record = m_messageRecords.find(*order);
		if (record == m_messageRecords.end()) {
			m_dedupeOrder.erase(order);
			return true;
		}
		if (record->second.state == MessageDedupeState::Acknowledged
			|| record->second.state == MessageDedupeState::Rejected) {
			m_messageRecords.erase(record);
			m_dedupeOrder.erase(order);
			return true;
		}
	}
	return false;
}

void CHarnessBridgeBroker::RejectEndpointMessagesLocked(EndpointState& endpoint) noexcept
{
	for (const auto& delivery : endpoint.queued) {
		const auto record = m_messageRecords.find(delivery.message.messageId);
		if (record != m_messageRecords.end()) record->second.state = MessageDedupeState::Rejected;
	}
	for (const auto& [messageId, delivery] : endpoint.inflight) {
		(void)delivery;
		const auto record = m_messageRecords.find(messageId);
		if (record != m_messageRecords.end()) record->second.state = MessageDedupeState::Rejected;
	}
}

HarnessBrokerResult CHarnessBridgeBroker::RegisterEndpoint(const HarnessEndpointRegistration& registration,
	const HarnessBridgeTargetDescriptor& authority)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	if (!IsValidId(registration.endpointId) || !IsValidText(registration.displayName, 128)
		|| !IsValidText(registration.scope, 256) || registration.grants == EHarnessGrant::None
		|| registration.maximumQueue == 0 || !IsValidAuthority(authority)) {
		return { EHarnessBrokerStatus::InvalidRequest };
	}
	if (m_endpoints.find(registration.endpointId) != m_endpoints.end()) return { EHarnessBrokerStatus::Duplicate };
	if (m_endpoints.size() >= m_limits.maximumEndpoints) return { EHarnessBrokerStatus::ResourceExhausted };
	EndpointState state;
	state.registration = registration;
	state.authority = authority;
	state.registration.maximumQueue = (std::min)(registration.maximumQueue, m_limits.maximumQueuedMessagesPerEndpoint);
	m_endpoints.emplace(registration.endpointId, std::move(state));
	return { EHarnessBrokerStatus::Accepted };
}

HarnessBrokerResult CHarnessBridgeBroker::UnregisterEndpoint(const HarnessEndpointId& endpoint,
	const HarnessBridgeTargetDescriptor& authority)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	const auto it = m_endpoints.find(endpoint);
	if (it == m_endpoints.end()) return { EHarnessBrokerStatus::UnknownEndpoint };
	if (!IsValidAuthority(authority) || it->second.authority != authority) {
		return { EHarnessBrokerStatus::AccessDenied };
	}
	RejectEndpointMessagesLocked(it->second);
	m_endpoints.erase(it);
	return { EHarnessBrokerStatus::Succeeded };
}

std::vector<HarnessEndpointInfo> CHarnessBridgeBroker::ListEndpoints() const
{
	std::lock_guard lock(m_mutex);
	std::vector<HarnessEndpointInfo> result;
	result.reserve(m_endpoints.size());
	for (const auto& [id, state] : m_endpoints) {
		(void)id;
		result.push_back({ state.registration.endpointId, state.registration.displayName,
			state.registration.scope, state.registration.grants });
	}
	return result;
}

HarnessBrokerResult CHarnessBridgeBroker::SendEndpointMessage(const HarnessMessage& message,
	const HarnessBridgeTargetDescriptor& authority)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	if (!IsValidId(message.messageId) || !IsValidId(message.sender) || !IsValidId(message.recipient)
		|| !IsValidText(message.type, 128) || message.payload.size() > m_limits.maximumMessagePayloadBytes
		|| message.hopCount > m_limits.maximumHopCount) return { EHarnessBrokerStatus::InvalidRequest };
	if (message.deadline != std::chrono::steady_clock::time_point{} && message.deadline <= std::chrono::steady_clock::now()) {
		return { EHarnessBrokerStatus::DeadlineExceeded };
	}
	const auto payloadDigest = ComputePayloadDigest(message.payload);
	if (!payloadDigest) return { EHarnessBrokerStatus::ResourceExhausted };
	const auto sender = m_endpoints.find(message.sender);
	const auto recipient = m_endpoints.find(message.recipient);
	if (sender == m_endpoints.end() || recipient == m_endpoints.end()) return { EHarnessBrokerStatus::UnknownEndpoint };
	if (!IsValidAuthority(authority) || sender->second.authority != authority) {
		return { EHarnessBrokerStatus::AccessDenied };
	}
	if (!HasGrant(sender->second.registration.grants, EHarnessGrant::Message)
		|| !HasGrant(recipient->second.registration.grants, EHarnessGrant::Message)) return { EHarnessBrokerStatus::AccessDenied };
	const auto existing = m_messageRecords.find(message.messageId);
	if (existing != m_messageRecords.end()) {
		return { MatchesDedupeRecord(existing->second, message, *payloadDigest)
			? EHarnessBrokerStatus::Duplicate : EHarnessBrokerStatus::Conflict };
	}
	if (recipient->second.queued.size() >= recipient->second.registration.maximumQueue
		|| message.payload.size() > m_limits.maximumQueuedMessageBytesPerEndpoint
		|| recipient->second.queuedBytes > m_limits.maximumQueuedMessageBytesPerEndpoint - message.payload.size()) return { EHarnessBrokerStatus::ResourceExhausted };
	if (!EnsureDedupeCapacityLocked()) return { EHarnessBrokerStatus::ResourceExhausted };
	recipient->second.queued.push_back({ message, 1 });
	recipient->second.queuedBytes += message.payload.size();
	m_messageRecords.emplace(message.messageId, MessageDedupeRecord{
		message.sender, message.recipient, message.runId, message.replyTo,
		message.hopCount, message.type, message.deadline, message.payload.size(),
		*payloadDigest, MessageDedupeState::Pending });
	m_dedupeOrder.push_back(message.messageId);
	m_condition.notify_all();
	return { EHarnessBrokerStatus::Accepted };
}

HarnessBrokerResult CHarnessBridgeBroker::ReceiveMessages(const HarnessEndpointId& recipient,
	const std::size_t maximumMessages, const std::chrono::steady_clock::time_point deadline,
	const HarnessBridgeTargetDescriptor& authority)
{
	std::unique_lock lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	if (!IsValidId(recipient) || maximumMessages == 0) return { EHarnessBrokerStatus::InvalidRequest };
	const auto end = (deadline == std::chrono::steady_clock::time_point{}) ? std::chrono::steady_clock::now() : deadline;
	const auto ready = [&] {
		const auto it = m_endpoints.find(recipient);
		return m_state != EHarnessBrokerState::Running || (it != m_endpoints.end()
			&& (!it->second.inflight.empty() || !it->second.queued.empty()));
	};
	if (!ready() && deadline != std::chrono::steady_clock::time_point{} && !m_condition.wait_until(lock, end, ready)) {
		return { EHarnessBrokerStatus::DeadlineExceeded };
	}
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	const auto it = m_endpoints.find(recipient);
	if (it == m_endpoints.end()) return { EHarnessBrokerStatus::UnknownEndpoint };
	if (!IsValidAuthority(authority) || it->second.authority != authority) {
		return { EHarnessBrokerStatus::AccessDenied };
	}
	if (!HasGrant(it->second.registration.grants, EHarnessGrant::Message)) return { EHarnessBrokerStatus::AccessDenied };
	HarnessBrokerResult result;
	for (const auto& messageId : it->second.inflightOrder) {
		if (result.messages.size() >= maximumMessages) break;
		const auto inflight = it->second.inflight.find(messageId);
		if (inflight == it->second.inflight.end()) continue;
		if (inflight->second.deliveryAttempt != (std::numeric_limits<std::uint64_t>::max)()) {
			++inflight->second.deliveryAttempt;
		}
		result.messages.push_back(inflight->second);
	}
	const auto remaining = maximumMessages - result.messages.size();
	const auto availableInflight = m_limits.maximumInflightMessagesPerEndpoint - it->second.inflight.size();
	const auto count = (std::min)({ remaining, availableInflight, it->second.queued.size() });
	std::size_t incomingBytes = 0;
	for (std::size_t index = 0; index < count; ++index) {
		const auto& delivery = it->second.queued[index];
		if (incomingBytes > m_limits.maximumQueuedMessageBytesPerEndpoint - delivery.message.payload.size()) return { EHarnessBrokerStatus::ResourceExhausted };
		incomingBytes += delivery.message.payload.size();
	}
	if (it->second.inflightBytes > m_limits.maximumQueuedMessageBytesPerEndpoint - incomingBytes) {
		return { EHarnessBrokerStatus::ResourceExhausted };
	}
	for (std::size_t index = 0; index < count; ++index) {
		auto delivery = std::move(it->second.queued.front());
		it->second.queued.pop_front();
		it->second.queuedBytes -= delivery.message.payload.size();
		it->second.inflightBytes += delivery.message.payload.size();
		it->second.inflight.emplace(delivery.message.messageId, delivery);
		it->second.inflightOrder.push_back(delivery.message.messageId);
		const auto record = m_messageRecords.find(delivery.message.messageId);
		if (record != m_messageRecords.end()) record->second.state = MessageDedupeState::Inflight;
		result.messages.push_back(std::move(delivery));
	}
	result.status = EHarnessBrokerStatus::Succeeded;
	return result;
}

HarnessBrokerResult CHarnessBridgeBroker::AcknowledgeMessage(const HarnessEndpointId& recipient,
	const HarnessMessageId& messageId, const HarnessBridgeTargetDescriptor& authority)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	const auto endpoint = m_endpoints.find(recipient);
	if (endpoint == m_endpoints.end()) return { EHarnessBrokerStatus::UnknownEndpoint };
	if (!IsValidAuthority(authority) || endpoint->second.authority != authority) {
		return { EHarnessBrokerStatus::AccessDenied };
	}
	if (!HasGrant(endpoint->second.registration.grants, EHarnessGrant::Message)) return { EHarnessBrokerStatus::AccessDenied };
	const auto record = m_messageRecords.find(messageId);
	if (record != m_messageRecords.end() && record->second.recipient != recipient) {
		return { EHarnessBrokerStatus::AccessDenied };
	}
	if (record != m_messageRecords.end() && record->second.state == MessageDedupeState::Acknowledged) {
		return { EHarnessBrokerStatus::Succeeded };
	}
	const auto inflight = endpoint->second.inflight.find(messageId);
	if (inflight == endpoint->second.inflight.end()) return { EHarnessBrokerStatus::UnknownMessage };
	endpoint->second.inflightBytes -= inflight->second.message.payload.size();
	endpoint->second.inflight.erase(inflight);
	const auto order = std::find(endpoint->second.inflightOrder.begin(),
		endpoint->second.inflightOrder.end(), messageId);
	if (order != endpoint->second.inflightOrder.end()) endpoint->second.inflightOrder.erase(order);
	if (record != m_messageRecords.end()) record->second.state = MessageDedupeState::Acknowledged;
	return { EHarnessBrokerStatus::Succeeded };
}

HarnessBrokerResult CHarnessBridgeBroker::PublishRunResult(const HarnessRunResult& result)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	if (!IsValidId(result.runId)) return { EHarnessBrokerStatus::InvalidRequest };
	const auto existing = m_runs.find(result.runId);
	if (existing != m_runs.end() && existing->second) return { EHarnessBrokerStatus::AlreadyTerminal };
	if (existing == m_runs.end() && m_runs.size() >= m_limits.maximumRuns) return { EHarnessBrokerStatus::ResourceExhausted };
	if (existing == m_runs.end()) m_runs.emplace(result.runId, result);
	else existing->second = result;
	m_condition.notify_all();
	return { EHarnessBrokerStatus::Accepted, {}, result };
}

HarnessBrokerResult CHarnessBridgeBroker::BeginRun(const HarnessRunId& run)
{
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked()) return { EHarnessBrokerStatus::BrokerStopping };
	if (!IsValidId(run)) return { EHarnessBrokerStatus::InvalidRequest };
	if (m_runs.find(run) != m_runs.end()) return { EHarnessBrokerStatus::Duplicate };
	if (m_runs.size() >= m_limits.maximumRuns) return { EHarnessBrokerStatus::ResourceExhausted };
	m_runs.emplace(run, std::nullopt);
	return { EHarnessBrokerStatus::Accepted };
}

HarnessBrokerResult CHarnessBridgeBroker::WaitRun(const HarnessRunId& run,
	const std::chrono::steady_clock::time_point deadline) const
{
	std::unique_lock lock(m_mutex);
	if (!IsValidId(run)) return { EHarnessBrokerStatus::InvalidRequest };
	const auto ready = [&] {
		const auto it = m_runs.find(run);
		return (it != m_runs.end() && it->second.has_value()) || m_state != EHarnessBrokerState::Running;
	};
	if (!ready()) {
		if (deadline == std::chrono::steady_clock::time_point{} || !m_condition.wait_until(lock, deadline, ready)) {
			return { EHarnessBrokerStatus::DeadlineExceeded };
		}
	}
	const auto it = m_runs.find(run);
	if (it == m_runs.end() || !it->second) return { m_state == EHarnessBrokerState::Running ? EHarnessBrokerStatus::UnknownRun : EHarnessBrokerStatus::BrokerStopping };
	return { EHarnessBrokerStatus::Succeeded, {}, it->second };
}

} // namespace platform::harnessbridge
