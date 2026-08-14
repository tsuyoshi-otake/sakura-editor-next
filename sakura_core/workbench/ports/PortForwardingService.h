/*! @file
 * @brief Pure, bounded PORTS/port-forwarding state authority.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::ports {

//! Reloaded producers must use a strictly newer generation; late producers cannot mutate a replacement.
struct PortOwner final {
	std::string id;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const PortOwner&) const noexcept = default;
};

//! A remote endpoint is descriptive state only. The service never opens, binds, or probes it.
struct PortEndpoint final {
	std::string host;
	std::uint16_t port{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const PortEndpoint&) const noexcept = default;
};

enum class EPortPrivacy : std::uint8_t {
	Private,
	Public,
	ConstantPrivate,
};

enum class EPortProtocol : std::uint8_t {
	Auto,
	Http,
	Https,
	Tcp,
};

enum class EPortSource : std::uint8_t {
	User,
	Provider,
	AutoForward,
	Environment,
};

//! Every forwarding transition is explicitly represented; no backend activity is inferred from a snapshot.
enum class EPortForwardingState : std::uint8_t {
	Discovered,
	Forwarding,
	Forwarded,
	Stopping,
	Stopped,
	Failed,
};

[[nodiscard]] constexpr bool IsTerminalPortForwardingState(const EPortForwardingState state) noexcept
{
	return state == EPortForwardingState::Stopped || state == EPortForwardingState::Failed;
}

struct PortForwardingError final {
	std::uint32_t code{};
	std::string message;

	[[nodiscard]] bool operator==(const PortForwardingError&) const noexcept = default;
};

struct PortOperation final {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

//! Stable `portId` is an internal operation identity. VS Code-compatible UI and
//! tunnel adapters project `remoteEndpoint.host` and `.port` as the visible key.
struct DiscoverPortRequest final {
	PortOperation operation;
	PortOwner owner;
	std::string portId;
	PortEndpoint remoteEndpoint;
	EPortPrivacy privacy{ EPortPrivacy::Private };
	EPortProtocol protocol{ EPortProtocol::Auto };
	EPortSource source{ EPortSource::Provider };
	std::string sourceDescription;
	std::optional<std::string> label;
	std::optional<std::string> processDescription;
	bool closeable{ true };
};

struct PortMutationRequest final {
	PortOperation operation;
	PortOwner owner;
	std::string portId;
};

//! Completion is produced by a bounded host adapter after its real tunnel backend reaches a terminal result.
struct CompletePortForwardingRequest final {
	PortMutationRequest mutation;
	std::string tunnelId;
	//! Display/open target returned by the tunnel backend (for example
	//! `127.0.0.1:51234` or an HTTPS URL).
	std::string localAddress;
	std::optional<std::uint16_t> localPort;
};

struct FailPortForwardingRequest final {
	PortMutationRequest mutation;
	PortForwardingError error;
};

struct DisposePortOwnerRequest final {
	PortOperation operation;
	PortOwner owner;
};

enum class EPortOperationStatus : std::uint8_t {
	Succeeded,
	Replayed,
	NotApplicable,
	Rejected,
	Conflict,
	StaleRevision,
	RevisionExhausted,
	Stopped,
};

enum class EPortOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	InvalidOwner,
	InvalidPortId,
	InvalidEndpoint,
	InvalidPrivacy,
	InvalidProtocol,
	InvalidSource,
	InvalidLocalAddress,
	InvalidLabel,
	InvalidSourceDescription,
	InvalidProcessDescription,
	InvalidTunnelId,
	InvalidError,
	PayloadLimitExceeded,
	//! Disposed owner-generation tombstones are lifetime-bounded and never evicted before Stop().
	OwnerLimitExceeded,
	PortLimitExceeded,
	PortAlreadyExists,
	PortNotFound,
	OwnerGenerationConflict,
	OwnerDisposed,
	PortOwnerConflict,
	OperationIdConflict,
	ExpectedRevisionMismatch,
	InvalidTransition,
};

struct PortOperationResult final {
	EPortOperationStatus status{ EPortOperationStatus::Rejected };
	EPortOperationReason reason{ EPortOperationReason::None };
	std::uint64_t revision{};
	bool callbackDrainDeferred{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EPortOperationStatus::Succeeded || status == EPortOperationStatus::Replayed;
	}
};

struct PortSnapshot final {
	std::string portId;
	PortOwner owner;
	PortEndpoint remoteEndpoint;
	std::optional<std::string> localAddress;
	std::optional<std::uint16_t> localPort;
	//! Set only after a successful forwarding completion; this is the backend tunnel identity, not an endpoint alias.
	std::optional<std::string> tunnelId;
	EPortPrivacy privacy{ EPortPrivacy::Private };
	EPortProtocol protocol{ EPortProtocol::Auto };
	EPortSource source{ EPortSource::Provider };
	std::string sourceDescription;
	std::optional<std::string> label;
	std::optional<std::string> processDescription;
	bool closeable{ true };
	EPortForwardingState state{ EPortForwardingState::Discovered };
	std::optional<PortForwardingError> error;
};

struct PortForwardingServiceSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	//! Advisory notifications are bounded; callers resnapshot when this is nonzero.
	std::uint64_t droppedNotificationCount{};
	//! Ordered by stable port ID, independent of insertion order.
	std::vector<PortSnapshot> ports;
};

enum class EPortChangeKind : std::uint8_t {
	Discovered,
	ForwardingStarted,
	Forwarded,
	ForwardingFailed,
	Stopping,
	Stopped,
	OwnerDisposed,
};

struct PortForwardingServiceChange final {
	std::uint64_t revision{};
	EPortChangeKind kind{ EPortChangeKind::Discovered };
	std::optional<std::string> portId;
	EPortForwardingState state{ EPortForwardingState::Discovered };
};

using PortForwardingSubscriptionId = std::uint64_t;
using PortForwardingListener = std::function<void(const PortForwardingServiceChange&)>;

struct PortForwardingServiceLimits final {
	std::size_t maximumOwners{ 128 };
	std::size_t maximumPorts{ 256 };
	std::size_t maximumPayloadBytes{ 64U << 10 };
	std::size_t maximumSubscriptions{ 256 };
	std::size_t maximumRememberedOperations{ 512 };
	std::size_t maximumPendingNotifications{ 512 };
	//! Testable monotonic fence. The production default never wraps.
	std::uint64_t maximumRevision{ (std::numeric_limits<std::uint64_t>::max)() };
};

/*! 
	@brief Thread-safe pure PORTS model with explicit backend-completion transitions.

	No networking, process launch, retry, or detached work occurs here. A host adapter may call StartForwarding,
	then one completion method after its backend has reached a result. Exact successful operation replay is fenced
	by a bounded operation journal. Listener invocation happens after releasing the model lock and listener faults
	are contained.
*/
class PortForwardingService final {
public:
	explicit PortForwardingService(PortForwardingServiceLimits limits = {});
	~PortForwardingService();

	PortForwardingService(const PortForwardingService&) = delete;
	PortForwardingService& operator=(const PortForwardingService&) = delete;

	[[nodiscard]] PortOperationResult Discover(const DiscoverPortRequest& request);
	[[nodiscard]] PortOperationResult StartForwarding(const PortMutationRequest& request);
	[[nodiscard]] PortOperationResult CompleteForwarding(const CompletePortForwardingRequest& request);
	[[nodiscard]] PortOperationResult FailForwarding(const FailPortForwardingRequest& request);
	[[nodiscard]] PortOperationResult RequestStop(const PortMutationRequest& request);
	[[nodiscard]] PortOperationResult CompleteStop(const PortMutationRequest& request);
	[[nodiscard]] PortOperationResult DisposeOwner(const DisposePortOwnerRequest& request);
	//! External callers wait for active listener callbacks; a reentrant listener Stop returns deferred.
	//! A listener borrows this service and must not destroy it from inside the callback.
	[[nodiscard]] PortOperationResult Stop() noexcept;

	[[nodiscard]] PortForwardingServiceSnapshot Snapshot() const;
	[[nodiscard]] std::optional<PortForwardingSubscriptionId> Subscribe(PortForwardingListener listener);
	void Unsubscribe(PortForwardingSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;
	[[nodiscard]] static bool IsValidOperationId(std::string_view value) noexcept;

private:
	struct Impl;
	Impl* m_impl;
};

} // namespace workbench::ports
