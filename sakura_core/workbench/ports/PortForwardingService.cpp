/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/ports/PortForwardingService.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace workbench::ports {
namespace {

constexpr std::size_t kMaximumStableIdBytes = 160;
constexpr std::size_t kMaximumHostBytes = 255;
constexpr std::size_t kMaximumLabelBytes = 512;
constexpr std::size_t kMaximumSourceDescriptionBytes = 512;
constexpr std::size_t kMaximumProcessDescriptionBytes = 1'024;
constexpr std::size_t kMaximumLocalAddressBytes = 2'048;
constexpr std::size_t kMaximumErrorBytes = 1'024;

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if ((!permitControls && (first < 0x20 || first == 0x7f)) || first == 0) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) { continuationCount = 1; codePoint = first & 0x1f; }
		else if (first >= 0xe0 && first <= 0xef) { continuationCount = 2; codePoint = first & 0x0f; }
		else if (first >= 0xf0 && first <= 0xf4) { continuationCount = 3; codePoint = first & 0x07; }
		else return false;
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		if (!permitControls && (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidBoundedText(const std::string_view value, const std::size_t maximumBytes, const bool permitControls = false) noexcept
{
	return value.size() <= maximumBytes && IsValidUtf8(value, permitControls);
}

bool IsValidPrivacy(const EPortPrivacy value) noexcept
{
	return value == EPortPrivacy::Private || value == EPortPrivacy::Public || value == EPortPrivacy::ConstantPrivate;
}

bool IsValidProtocol(const EPortProtocol value) noexcept
{
	return value == EPortProtocol::Auto || value == EPortProtocol::Http || value == EPortProtocol::Https || value == EPortProtocol::Tcp;
}

bool IsValidSource(const EPortSource value) noexcept
{
	return value == EPortSource::User || value == EPortSource::Extension || value == EPortSource::AutoForward || value == EPortSource::Environment;
}

void AppendToken(std::string& target, const std::string_view value)
{
	target.append(std::to_string(value.size()));
	target.push_back(':');
	target.append(value);
	target.push_back(';');
}

void AppendNumber(std::string& target, const std::uint64_t value)
{
	AppendToken(target, std::to_string(value));
}

void AppendOperation(std::string& target, const PortOperation& operation)
{
	AppendToken(target, operation.operationId);
	AppendToken(target, operation.expectedRevision ? std::to_string(*operation.expectedRevision) : "-");
}

void AppendOwner(std::string& target, const PortOwner& owner)
{
	AppendToken(target, owner.id);
	AppendNumber(target, owner.generation);
}

void AppendEndpoint(std::string& target, const PortEndpoint& endpoint)
{
	AppendToken(target, endpoint.host);
	AppendNumber(target, endpoint.port);
}

void AppendMutation(std::string& target, const PortMutationRequest& request)
{
	AppendOperation(target, request.operation);
	AppendOwner(target, request.owner);
	AppendToken(target, request.portId);
}

std::string DiscoverFingerprint(const DiscoverPortRequest& request)
{
	std::string result("discover;");
	AppendOperation(result, request.operation);
	AppendOwner(result, request.owner);
	AppendToken(result, request.portId);
	AppendEndpoint(result, request.remoteEndpoint);
	AppendNumber(result, static_cast<std::uint64_t>(request.privacy));
	AppendNumber(result, static_cast<std::uint64_t>(request.protocol));
	AppendNumber(result, static_cast<std::uint64_t>(request.source));
	AppendToken(result, request.sourceDescription);
	AppendToken(result, request.label.value_or(""));
	AppendToken(result, request.processDescription.value_or(""));
	AppendNumber(result, request.closeable ? 1U : 0U);
	return result;
}

std::string MutationFingerprint(const std::string_view verb, const PortMutationRequest& request)
{
	std::string result(verb);
	result.push_back(';');
	AppendMutation(result, request);
	return result;
}

} // namespace

struct PortForwardingService::Impl final {
	struct OwnerGeneration final {
		std::uint64_t generation{};
		bool disposed{};
	};
	struct CompletedOperation final {
		std::string fingerprint;
		PortOperationResult result;
	};
	struct PendingNotification final {
		PortForwardingServiceChange change;
		std::vector<PortForwardingSubscriptionId> subscriberIds;
	};

	mutable std::mutex mutex;
	PortForwardingServiceLimits limits;
	std::map<std::string, PortSnapshot, std::less<>> ports;
	//! Tombstones retain the newest generation even after disposal, fencing late producer messages.
	std::map<std::string, OwnerGeneration, std::less<>> owners;
	std::map<std::string, CompletedOperation, std::less<>> completedOperations;
	std::deque<std::string> completedOperationOrder;
	std::map<PortForwardingSubscriptionId, PortForwardingListener> subscriptions;
	std::deque<PendingNotification> pendingNotifications;
	std::uint64_t revision{ 1 };
	std::uint64_t droppedNotificationCount{};
	PortForwardingSubscriptionId nextSubscriptionId{ 1 };
	bool drainingNotifications{};
	std::thread::id notificationDispatchThreadId;
	std::condition_variable notificationDrained;
	bool stopped{};

	explicit Impl(PortForwardingServiceLimits initialLimits)
		: limits(std::move(initialLimits))
	{
		if (limits.maximumOwners == 0) limits.maximumOwners = 1;
		if (limits.maximumPorts == 0) limits.maximumPorts = 1;
		if (limits.maximumPayloadBytes == 0) limits.maximumPayloadBytes = 1;
		if (limits.maximumSubscriptions == 0) limits.maximumSubscriptions = 1;
		if (limits.maximumRememberedOperations == 0) limits.maximumRememberedOperations = 1;
		if (limits.maximumPendingNotifications == 0) limits.maximumPendingNotifications = 1;
		if (limits.maximumRevision == 0) limits.maximumRevision = 1;
	}

	[[nodiscard]] PortOperationResult Current(const EPortOperationStatus status, const EPortOperationReason reason) const noexcept
	{
		return { status, reason, revision };
	}

	[[nodiscard]] bool CanAdvanceLocked() const noexcept { return revision < limits.maximumRevision; }

	void RememberLocked(std::string operationId, std::string fingerprint, const PortOperationResult& result)
	{
		if (completedOperations.size() == limits.maximumRememberedOperations) {
			completedOperations.erase(completedOperationOrder.front());
			completedOperationOrder.pop_front();
		}
		completedOperationOrder.push_back(operationId);
		completedOperations.emplace(std::move(operationId), CompletedOperation{ std::move(fingerprint), result });
	}

	[[nodiscard]] std::optional<PortOperationResult> ReplayOrConflictLocked(const PortOperation& operation,
		const std::string& fingerprint) const
	{
		const auto found = completedOperations.find(operation.operationId);
		if (found == completedOperations.end()) return std::nullopt;
		if (found->second.fingerprint != fingerprint) return Current(EPortOperationStatus::Conflict, EPortOperationReason::OperationIdConflict);
		auto result = found->second.result;
		result.status = EPortOperationStatus::Replayed;
		return result;
	}

	static void SaturatingIncrement(std::uint64_t& value) noexcept
	{
		if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
	}

	[[nodiscard]] bool QueueNotificationLocked(const EPortChangeKind kind, const std::optional<std::string>& portId,
		const EPortForwardingState state) noexcept
	{
		try {
			if (pendingNotifications.size() >= limits.maximumPendingNotifications) {
				SaturatingIncrement(droppedNotificationCount);
				return false;
			}
			PendingNotification pending{ .change = { .revision = revision, .kind = kind, .portId = portId, .state = state } };
			pending.subscriberIds.reserve(subscriptions.size());
			for (const auto& [id, ignored] : subscriptions) {
				(void)ignored;
				pending.subscriberIds.push_back(id);
			}
			pendingNotifications.push_back(std::move(pending));
			if (drainingNotifications) return false;
			drainingNotifications = true;
			return true;
		} catch (...) {
			SaturatingIncrement(droppedNotificationCount);
			return false;
		}
	}

	[[nodiscard]] bool WaitForNotificationDrain() noexcept
	{
		std::unique_lock lock(mutex);
		if (drainingNotifications && notificationDispatchThreadId == std::this_thread::get_id()) return true;
		try { notificationDrained.wait(lock, [this] { return !drainingNotifications; }); }
		catch (...) { return drainingNotifications; }
		return false;
	}

	void DrainNotifications() noexcept
	{
		{
			std::lock_guard lock(mutex);
			if (!drainingNotifications || notificationDispatchThreadId != std::thread::id{}) return;
			notificationDispatchThreadId = std::this_thread::get_id();
		}
		for (;;) {
			PendingNotification pending;
			{
				std::lock_guard lock(mutex);
				if (pendingNotifications.empty()) {
					drainingNotifications = false;
					notificationDispatchThreadId = {};
					notificationDrained.notify_all();
					return;
				}
				pending = std::move(pendingNotifications.front());
				pendingNotifications.pop_front();
			}
			for (const auto id : pending.subscriberIds) {
				PortForwardingListener listener;
				{
					std::lock_guard lock(mutex);
					const auto found = subscriptions.find(id);
					if (found != subscriptions.end()) listener = found->second;
				}
				if (!listener) continue;
				try {
					listener(pending.change);
				} catch (...) {
				}
			}
		}
	}

	[[nodiscard]] PortOperationResult CommitLocked(const PortOperation& operation, std::string fingerprint,
		const EPortChangeKind kind, const std::optional<std::string>& portId, const EPortForwardingState state, bool& shouldDrain)
	{
		if (!CanAdvanceLocked()) return Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		++revision;
		const auto result = Current(EPortOperationStatus::Succeeded, EPortOperationReason::None);
		RememberLocked(operation.operationId, std::move(fingerprint), result);
		shouldDrain = QueueNotificationLocked(kind, portId, state);
		return result;
	}

	[[nodiscard]] PortOperationResult CheckOperationLocked(const PortOperation& operation, const std::string& fingerprint) const
	{
		if (!PortForwardingService::IsValidOperationId(operation.operationId)) return Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOperationId);
		if (const auto replay = ReplayOrConflictLocked(operation, fingerprint)) return *replay;
		if (operation.expectedRevision && *operation.expectedRevision != revision) return Current(EPortOperationStatus::StaleRevision, EPortOperationReason::ExpectedRevisionMismatch);
		return {};
	}

	[[nodiscard]] static bool IsFinished(const PortOperationResult& result) noexcept
	{
		return result.status != EPortOperationStatus::Rejected || result.reason != EPortOperationReason::None;
	}

	[[nodiscard]] PortOperationResult CheckOwnerForDiscoverLocked(const PortOwner& owner) const
	{
		if (!owner.IsValid()) return Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		const auto found = owners.find(owner.id);
		if (found == owners.end()) return {};
		if (owner.generation < found->second.generation) return Current(EPortOperationStatus::Conflict, EPortOperationReason::OwnerGenerationConflict);
		if (owner.generation == found->second.generation && found->second.disposed) return Current(EPortOperationStatus::Conflict, EPortOperationReason::OwnerDisposed);
		return {};
	}

	[[nodiscard]] PortOperationResult CheckPortOwnerLocked(const PortSnapshot& port, const PortOwner& owner) const
	{
		if (!owner.IsValid()) return Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (port.owner == owner) return {};
		if (port.owner.id == owner.id) return Current(EPortOperationStatus::Conflict, EPortOperationReason::OwnerGenerationConflict);
		return Current(EPortOperationStatus::Conflict, EPortOperationReason::PortOwnerConflict);
	}

	void AdoptOwnerGenerationLocked(const PortOwner& owner)
	{
		const auto found = owners.find(owner.id);
		if (found != owners.end() && owner.generation > found->second.generation) {
			for (auto port = ports.begin(); port != ports.end();) {
				if (port->second.owner.id == owner.id) port = ports.erase(port);
				else ++port;
			}
		}
		owners[owner.id] = { owner.generation, false };
	}
};

bool PortOwner::IsValid() const noexcept
{
	return generation != 0 && PortForwardingService::IsValidStableId(id);
}

bool PortEndpoint::IsValid() const noexcept
{
	return port != 0 && !host.empty() && IsValidBoundedText(host, kMaximumHostBytes);
}

PortForwardingService::PortForwardingService(PortForwardingServiceLimits limits)
	: m_impl(new Impl(std::move(limits)))
{
}

PortForwardingService::~PortForwardingService()
{
	(void)Stop();
	delete m_impl;
}

PortOperationResult PortForwardingService::Discover(const DiscoverPortRequest& request)
{
	const auto fingerprint = DiscoverFingerprint(request);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (const auto owner = m_impl->CheckOwnerForDiscoverLocked(request.owner); Impl::IsFinished(owner)) return owner;
		if (!IsValidStableId(request.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		if (!request.remoteEndpoint.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidEndpoint);
		if (!IsValidPrivacy(request.privacy)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPrivacy);
		if (!IsValidProtocol(request.protocol)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidProtocol);
		if (!IsValidSource(request.source)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidSource);
		if (request.sourceDescription.empty()
			|| !IsValidBoundedText(request.sourceDescription, kMaximumSourceDescriptionBytes)) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidSourceDescription);
		}
		if (request.label && (!IsValidBoundedText(*request.label, kMaximumLabelBytes) || request.label->empty())) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidLabel);
		if (request.processDescription && (request.processDescription->empty()
			|| !IsValidBoundedText(*request.processDescription, kMaximumProcessDescriptionBytes))) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidProcessDescription);
		}
		const auto payloadBytes = request.portId.size() + request.owner.id.size() + request.remoteEndpoint.host.size()
			+ request.sourceDescription.size() + request.label.value_or("").size()
			+ request.processDescription.value_or("").size();
		if (payloadBytes > m_impl->limits.maximumPayloadBytes) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::PayloadLimitExceeded);
		if (const auto existing = m_impl->ports.find(request.portId); existing != m_impl->ports.end()
			&& (existing->second.owner.id != request.owner.id || existing->second.owner.generation >= request.owner.generation)) {
			return m_impl->Current(EPortOperationStatus::Conflict, EPortOperationReason::PortAlreadyExists);
		}
		const auto ownerRecord = m_impl->owners.find(request.owner.id);
		if (ownerRecord == m_impl->owners.end() && m_impl->owners.size() >= m_impl->limits.maximumOwners) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::OwnerLimitExceeded);
		}
		const bool replacesOlderOwner = ownerRecord != m_impl->owners.end() && request.owner.generation > ownerRecord->second.generation;
		const auto replacedPortCount = static_cast<std::size_t>(std::count_if(m_impl->ports.begin(), m_impl->ports.end(), [&request](const auto& entry) {
			return entry.second.owner.id == request.owner.id;
		}));
		const auto retainedPortCount = m_impl->ports.size() - (replacesOlderOwner ? replacedPortCount : 0);
		if (retainedPortCount >= m_impl->limits.maximumPorts) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::PortLimitExceeded);
		}
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		m_impl->AdoptOwnerGenerationLocked(request.owner);
		m_impl->ports.emplace(request.portId, PortSnapshot{
			.portId = request.portId, .owner = request.owner, .remoteEndpoint = request.remoteEndpoint, .privacy = request.privacy,
			.protocol = request.protocol, .source = request.source, .sourceDescription = request.sourceDescription,
			.label = request.label, .processDescription = request.processDescription, .closeable = request.closeable,
		});
		result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::Discovered, request.portId,
			EPortForwardingState::Discovered, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::StartForwarding(const PortMutationRequest& request)
{
	const auto fingerprint = MutationFingerprint("start", request);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (!IsValidStableId(request.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		const auto found = m_impl->ports.find(request.portId);
		if (found == m_impl->ports.end()) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::PortNotFound);
		if (const auto owner = m_impl->CheckPortOwnerLocked(found->second, request.owner); Impl::IsFinished(owner)) return owner;
		if (found->second.state != EPortForwardingState::Discovered && found->second.state != EPortForwardingState::Stopped && found->second.state != EPortForwardingState::Failed) {
			return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::InvalidTransition);
		}
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		found->second.state = EPortForwardingState::Forwarding;
		found->second.error.reset();
		found->second.localAddress.reset();
		found->second.localPort.reset();
		found->second.tunnelId.reset();
		result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::ForwardingStarted, request.portId,
			EPortForwardingState::Forwarding, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::CompleteForwarding(const CompletePortForwardingRequest& request)
{
	std::string fingerprint("complete-forward;");
	AppendMutation(fingerprint, request.mutation);
	AppendToken(fingerprint, request.tunnelId);
	AppendToken(fingerprint, request.localAddress);
	AppendToken(fingerprint, request.localPort ? std::to_string(*request.localPort) : "-");
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.mutation.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.mutation.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (!IsValidStableId(request.mutation.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		if (!IsValidStableId(request.tunnelId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidTunnelId);
		if (request.localAddress.empty() || !IsValidBoundedText(request.localAddress, kMaximumLocalAddressBytes)
			|| (request.localPort && *request.localPort == 0)) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidLocalAddress);
		}
		if (request.tunnelId.size() + request.localAddress.size() > m_impl->limits.maximumPayloadBytes) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::PayloadLimitExceeded);
		const auto found = m_impl->ports.find(request.mutation.portId);
		if (found == m_impl->ports.end()) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::PortNotFound);
		if (const auto owner = m_impl->CheckPortOwnerLocked(found->second, request.mutation.owner); Impl::IsFinished(owner)) return owner;
		if (found->second.state != EPortForwardingState::Forwarding) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::InvalidTransition);
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		found->second.state = EPortForwardingState::Forwarded;
		found->second.tunnelId = request.tunnelId;
		found->second.localAddress = request.localAddress;
		found->second.localPort = request.localPort;
		result = m_impl->CommitLocked(request.mutation.operation, fingerprint, EPortChangeKind::Forwarded, request.mutation.portId,
			EPortForwardingState::Forwarded, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::FailForwarding(const FailPortForwardingRequest& request)
{
	std::string fingerprint("fail-forward;");
	AppendMutation(fingerprint, request.mutation);
	AppendNumber(fingerprint, request.error.code);
	AppendToken(fingerprint, request.error.message);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.mutation.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.mutation.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (!IsValidStableId(request.mutation.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		if (request.error.code == 0 || request.error.message.empty() || !IsValidBoundedText(request.error.message, kMaximumErrorBytes)) {
			return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidError);
		}
		const auto found = m_impl->ports.find(request.mutation.portId);
		if (found == m_impl->ports.end()) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::PortNotFound);
		if (const auto owner = m_impl->CheckPortOwnerLocked(found->second, request.mutation.owner); Impl::IsFinished(owner)) return owner;
		if (found->second.state != EPortForwardingState::Forwarding) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::InvalidTransition);
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		found->second.state = EPortForwardingState::Failed;
		found->second.error = request.error;
		result = m_impl->CommitLocked(request.mutation.operation, fingerprint, EPortChangeKind::ForwardingFailed, request.mutation.portId,
			EPortForwardingState::Failed, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::RequestStop(const PortMutationRequest& request)
{
	const auto fingerprint = MutationFingerprint("request-stop", request);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (!IsValidStableId(request.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		const auto found = m_impl->ports.find(request.portId);
		if (found == m_impl->ports.end()) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::PortNotFound);
		if (const auto owner = m_impl->CheckPortOwnerLocked(found->second, request.owner); Impl::IsFinished(owner)) return owner;
		if (found->second.state == EPortForwardingState::Discovered || found->second.state == EPortForwardingState::Failed) {
			if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
			found->second.state = EPortForwardingState::Stopped;
			result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::Stopped, request.portId, EPortForwardingState::Stopped, shouldDrain);
		} else if (found->second.state == EPortForwardingState::Forwarding || found->second.state == EPortForwardingState::Forwarded) {
			if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
			found->second.state = EPortForwardingState::Stopping;
			result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::Stopping, request.portId, EPortForwardingState::Stopping, shouldDrain);
		} else {
			return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::InvalidTransition);
		}
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::CompleteStop(const PortMutationRequest& request)
{
	const auto fingerprint = MutationFingerprint("complete-stop", request);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		if (!IsValidStableId(request.portId)) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidPortId);
		const auto found = m_impl->ports.find(request.portId);
		if (found == m_impl->ports.end()) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::PortNotFound);
		if (const auto owner = m_impl->CheckPortOwnerLocked(found->second, request.owner); Impl::IsFinished(owner)) return owner;
		if (found->second.state != EPortForwardingState::Stopping) return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::InvalidTransition);
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		found->second.state = EPortForwardingState::Stopped;
		result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::Stopped, request.portId, EPortForwardingState::Stopped, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::DisposeOwner(const DisposePortOwnerRequest& request)
{
	std::string fingerprint("dispose-owner;");
	AppendOperation(fingerprint, request.operation);
	AppendOwner(fingerprint, request.owner);
	bool shouldDrain{};
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(EPortOperationStatus::Stopped, EPortOperationReason::None);
		result = m_impl->CheckOperationLocked(request.operation, fingerprint);
		if (Impl::IsFinished(result)) return result;
		if (!request.owner.IsValid()) return m_impl->Current(EPortOperationStatus::Rejected, EPortOperationReason::InvalidOwner);
		const auto owner = m_impl->owners.find(request.owner.id);
		if (owner == m_impl->owners.end() || owner->second.generation != request.owner.generation || owner->second.disposed) {
			return m_impl->Current(EPortOperationStatus::NotApplicable, EPortOperationReason::OwnerDisposed);
		}
		if (!m_impl->CanAdvanceLocked()) return m_impl->Current(EPortOperationStatus::RevisionExhausted, EPortOperationReason::None);
		for (auto port = m_impl->ports.begin(); port != m_impl->ports.end();) {
			if (port->second.owner == request.owner) port = m_impl->ports.erase(port);
			else ++port;
		}
		owner->second.disposed = true;
		result = m_impl->CommitLocked(request.operation, fingerprint, EPortChangeKind::OwnerDisposed, std::nullopt,
			EPortForwardingState::Stopped, shouldDrain);
	}
	if (shouldDrain) m_impl->DrainNotifications();
	return result;
}

PortOperationResult PortForwardingService::Stop() noexcept
{
	PortOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) {
			result = m_impl->Current(EPortOperationStatus::Succeeded, EPortOperationReason::None);
		} else {
			m_impl->ports.clear();
			m_impl->owners.clear();
			m_impl->completedOperations.clear();
			m_impl->completedOperationOrder.clear();
			m_impl->subscriptions.clear();
			m_impl->pendingNotifications.clear();
			m_impl->stopped = true;
			if (m_impl->CanAdvanceLocked()) ++m_impl->revision;
			result = m_impl->Current(EPortOperationStatus::Succeeded, EPortOperationReason::None);
		}
	}
	result.callbackDrainDeferred = m_impl->WaitForNotificationDrain();
	return result;
}

PortForwardingServiceSnapshot PortForwardingService::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	PortForwardingServiceSnapshot snapshot{ .revision = m_impl->revision, .stopped = m_impl->stopped,
		.droppedNotificationCount = m_impl->droppedNotificationCount };
	snapshot.ports.reserve(m_impl->ports.size());
	for (const auto& [id, port] : m_impl->ports) {
		(void)id;
		snapshot.ports.push_back(port);
	}
	return snapshot;
}

std::optional<PortForwardingSubscriptionId> PortForwardingService::Subscribe(PortForwardingListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->stopped || m_impl->subscriptions.size() >= m_impl->limits.maximumSubscriptions
		|| m_impl->nextSubscriptionId == 0) return std::nullopt;
	const auto id = m_impl->nextSubscriptionId++;
	m_impl->subscriptions.emplace(id, std::move(listener));
	return id;
}

void PortForwardingService::Unsubscribe(const PortForwardingSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->subscriptions.erase(subscriptionId);
}

bool PortForwardingService::IsValidStableId(const std::string_view value) noexcept
{
	return !value.empty() && IsValidBoundedText(value, kMaximumStableIdBytes);
}

bool PortForwardingService::IsValidOperationId(const std::string_view value) noexcept
{
	return IsValidStableId(value);
}

} // namespace workbench::ports
