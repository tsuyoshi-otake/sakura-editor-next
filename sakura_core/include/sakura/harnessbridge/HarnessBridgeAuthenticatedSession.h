/*! @file
    @brief Capability-authenticated Harness Bridge protocol session.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeCapability.h>
#include <sakura/harnessbridge/HarnessBridgeEnvironment.h>
#include <sakura/harnessbridge/HarnessBridgeTransport.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace platform::harnessbridge {

enum class EHarnessBridgeAuthenticationState : std::uint8_t {
	AwaitingHello,
	ChallengeSent,
	Ready,
	Closed,
};

struct HarnessBridgePeerProcessCallbacks final {
	std::function<std::optional<std::uint64_t>(std::uint32_t)> queryCreationTime;
	//! Proves the peer belongs to the ConPTY job owned by the exact advertised
	//! target. The target argument prevents one terminal's descendant from
	//! authenticating with another terminal's job-scoped credential.
	std::function<bool(const HarnessBridgeTargetDescriptor&, std::uint32_t, std::uint64_t)> isJobMember;
};

struct HarnessBridgeSessionFences final {
	//! Process-wide immutable coordinates are always exact. A zero instance
	//! coordinate is a wildcard populated from Hello and then pinned for the
	//! lifetime of that authenticated connection; nonzero fields remain useful
	//! for focused tests and single-target hosts.
	HarnessBridgeTargetDescriptor target;
	std::array<std::uint8_t, 16> bridgeId{};
};

struct HarnessBridgeAuthenticatedSessionOptions final {
	std::chrono::milliseconds handshakeTimeout = std::chrono::seconds(5);
	std::size_t maximumCompletedOperations = 4096;
	std::size_t maximumCompletedOperationBytes = 1024u * 1024u;
};

//! Protocol-only DTO. Runtime/editor objects must not cross this interface.
struct HarnessBridgeOperationRequestDto final {
	HarnessOpaqueId operationId;
	EHarnessOperationKind operation = EHarnessOperationKind::QueryOperation;
	HarnessBridgeTargetDescriptor target;
	std::vector<std::uint8_t> payload;
	std::chrono::steady_clock::time_point deadline{};
};

struct HarnessBridgeOperationResponseDto final {
	EHarnessTerminalStatus status = EHarnessTerminalStatus::InternalError;
	std::vector<std::uint8_t> payload;
};

//! Canonical transcript shared by protocol clients and the authenticated session.
[[nodiscard]] std::vector<std::uint8_t> BuildHarnessBridgeAuthenticationTranscript(
	std::uint64_t requestId, std::uint32_t clientProcessId,
	const std::array<std::uint8_t, 16>& bridgeId,
	const HarnessBridgeTargetDescriptor& target,
	const std::array<std::uint8_t, 16>& clientNonce,
	const std::array<std::uint8_t, 16>& serverNonce,
	EHarnessGrant requestedGrants);

class IHarnessBridgeOperationDispatcher {
public:
	virtual ~IHarnessBridgeOperationDispatcher() = default;
	[[nodiscard]] virtual HarnessBridgeOperationResponseDto Dispatch(
		const HarnessBridgeSessionContext& session,
		const HarnessBridgeOperationRequestDto& request) = 0;
	virtual void Cancel(std::uint64_t requestId) noexcept = 0;
};

using HarnessBridgeOperationDispatcherFactory = std::function<std::shared_ptr<IHarnessBridgeOperationDispatcher>(
	const HarnessBridgeSessionContext&, const HarnessBridgeFrame&)>;

//! Performs the wire handshake and fences every operation to one capability target.
class CHarnessBridgeAuthenticatedSession final : public IHarnessBridgeSessionHandler {
public:
	CHarnessBridgeAuthenticatedSession(
		HarnessBridgeSessionFences fences,
		CHarnessBridgeCapabilityStore& capabilities,
		HarnessBridgePeerProcessCallbacks processCallbacks,
		std::shared_ptr<IHarnessBridgeOperationDispatcher> dispatcher,
		HarnessBridgeAuthenticatedSessionOptions options = {});
	~CHarnessBridgeAuthenticatedSession() override = default;
	CHarnessBridgeAuthenticatedSession(const CHarnessBridgeAuthenticatedSession&) = delete;
	CHarnessBridgeAuthenticatedSession& operator=(const CHarnessBridgeAuthenticatedSession&) = delete;

	[[nodiscard]] EHarnessBridgeAuthenticationState State() const noexcept { return m_state; }
	[[nodiscard]] bool IsReady() const noexcept { return m_state == EHarnessBridgeAuthenticationState::Ready; }

	HarnessBridgeTransportResult HandleFrame(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
		std::vector<HarnessBridgeFrame>& responses) override;

private:
	struct CompletedOperation final {
		HarnessOpaqueId operationId;
		HarnessBridgeOperationResponseDto response;
	};

	[[nodiscard]] HarnessBridgeTransportResult HandleHello(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
		std::vector<HarnessBridgeFrame>& responses);
	[[nodiscard]] HarnessBridgeTransportResult HandleAuthenticate(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
		std::vector<HarnessBridgeFrame>& responses);
	[[nodiscard]] HarnessBridgeTransportResult HandleOperation(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
		std::vector<HarnessBridgeFrame>& responses);
	[[nodiscard]] HarnessBridgeTransportResult Reject(
		std::uint64_t requestId, EHarnessTerminalStatus status,
		std::vector<HarnessBridgeFrame>& responses) noexcept;
	[[nodiscard]] bool CheckFrameBasics(const HarnessBridgeFrame& frame,
		EHarnessBridgeFrameKind kind, EHarnessBridgeFrameFlags requiredFlags) const noexcept;
	[[nodiscard]] std::optional<HarnessBridgeFields> DecodeFields(
		const HarnessBridgeFrame& frame) const noexcept;
	[[nodiscard]] std::optional<HarnessBridgeTargetDescriptor> DecodeTarget(
		const HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag) const;
	[[nodiscard]] std::optional<std::uint32_t> GetU32(
		const HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag) const noexcept;
	[[nodiscard]] std::optional<std::uint16_t> GetU16(
		const HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag) const noexcept;
	[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> GetNonce(
		const HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag) const noexcept;
	[[nodiscard]] static EHarnessGrant RequiredGrant(EHarnessOperationKind operation) noexcept;
	[[nodiscard]] std::vector<std::uint8_t> BuildTranscript(
		const HarnessBridgeSessionContext& session) const;
	[[nodiscard]] HarnessCapabilityContext BuildCapabilityContext(
		const HarnessBridgeSessionContext& session) const;
	[[nodiscard]] HarnessBridgeFrame MakeFrame(
		EHarnessBridgeFrameKind kind, EHarnessBridgeFrameFlags flags,
		std::uint64_t requestId, const HarnessBridgeFields& fields) const;

	HarnessBridgeSessionFences m_fences;
	CHarnessBridgeCapabilityStore& m_capabilities;
	HarnessBridgePeerProcessCallbacks m_processCallbacks;
	std::shared_ptr<IHarnessBridgeOperationDispatcher> m_dispatcher;
	HarnessBridgeAuthenticatedSessionOptions m_options;
	EHarnessBridgeAuthenticationState m_state = EHarnessBridgeAuthenticationState::AwaitingHello;
	std::chrono::steady_clock::time_point m_handshakeDeadline;
	std::uint64_t m_helloRequestId = 0;
	HarnessOpaqueId m_capabilityId;
	EHarnessGrant m_requestedGrants = EHarnessGrant::None;
	std::array<std::uint8_t, 16> m_clientNonce{};
	std::array<std::uint8_t, 16> m_serverNonce{};
	HarnessBridgeTargetDescriptor m_clientTarget;
	std::uint64_t m_peerCreationTime = 0;
	std::vector<std::uint8_t> m_transcript;
	std::array<std::uint8_t, 32> m_authenticationDigest{};
	std::deque<CompletedOperation> m_completedOperations;
	std::size_t m_completedOperationBytes = 0;
};

//! Transport-facing factory. It creates a fresh protocol session per pipe peer.
class CHarnessBridgeAuthenticatedSessionFactory final : public IHarnessBridgeSessionFactory {
public:
	CHarnessBridgeAuthenticatedSessionFactory(
		HarnessBridgeSessionFences fences,
		CHarnessBridgeCapabilityStore& capabilities,
		HarnessBridgePeerProcessCallbacks processCallbacks,
		HarnessBridgeOperationDispatcherFactory dispatcherFactory,
		HarnessBridgeAuthenticatedSessionOptions options = {});

	std::unique_ptr<IHarnessBridgeSessionHandler> CreateSession(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& firstFrame) override;

private:
	HarnessBridgeSessionFences m_fences;
	CHarnessBridgeCapabilityStore& m_capabilities;
	HarnessBridgePeerProcessCallbacks m_processCallbacks;
	HarnessBridgeOperationDispatcherFactory m_dispatcherFactory;
	HarnessBridgeAuthenticatedSessionOptions m_options;
};

} // namespace platform::harnessbridge
