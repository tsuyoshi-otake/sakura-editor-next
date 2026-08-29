/*! @file
    @brief Scoped, revocable Harness Bridge capability grants.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeBroker.h>

#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace platform::harnessbridge {

struct HarnessCapabilityContext final {
	std::uint64_t bridgeEpoch = 0;
	std::uint64_t runtimeGeneration = 0;
	std::uint64_t sessionId = 0;
	std::uint64_t paneId = 0;
	//! A zero/zero process pair creates a terminal-job-scoped credential. Such a
	//! credential is accepted only by the authenticated-session path after its
	//! OS callback has proven the connecting PID and creation time belong to the
	//! target instance's job. A nonzero pair pins one exact process identity.
	std::uint32_t processId = 0;
	std::uint64_t processCreationTime = 0;
	// Optional target identity fields. They are populated for full Harness
	// sessions while the original six-field context remains source-compatible.
	std::string profileId;
	std::uint64_t profileGeneration = 0;
	std::array<std::uint8_t, 16> editorId{};
	std::uint64_t instanceGeneration = 0;
	std::uint64_t windowId = 0;
	std::uint64_t instanceId = 0;
};

struct HarnessCapabilityCredential final {
	HarnessOpaqueId id;
	std::array<std::uint8_t, 32> secret{};
	EHarnessGrant grants = EHarnessGrant::None;
};

enum class EHarnessCapabilityCheck : std::uint8_t {
	Granted,
	Invalid,
	Expired,
	Revoked,
	WrongEpoch,
	WrongTarget,
	WrongProcess,
	MissingGrant,
};

//! Stores only bounded capability records. Secrets are never serialized by this class.
class CHarnessBridgeCapabilityStore final {
public:
	CHarnessBridgeCapabilityStore() = default;
	CHarnessBridgeCapabilityStore(const CHarnessBridgeCapabilityStore&) = delete;
	CHarnessBridgeCapabilityStore& operator=(const CHarnessBridgeCapabilityStore&) = delete;

	[[nodiscard]] std::optional<HarnessCapabilityCredential> Issue(
		const HarnessCapabilityContext& context, EHarnessGrant grants,
		std::chrono::steady_clock::time_point expiresAt = {});
	void Revoke(const HarnessOpaqueId& id) noexcept;
	void RevokeAll() noexcept;
	[[nodiscard]] EHarnessCapabilityCheck Check(const HarnessCapabilityCredential& credential,
		const HarnessCapabilityContext& context, EHarnessGrant required,
		std::chrono::steady_clock::time_point now) const noexcept;
	//! Computes a challenge response without exposing the stored secret.
	[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> ComputeAuthenticationDigest(
		const HarnessOpaqueId& id, std::span<const std::uint8_t> transcript,
		std::chrono::steady_clock::time_point now) const noexcept;
	//! Checks identity, grants, expiry, and the transcript HMAC in one operation.
	[[nodiscard]] EHarnessCapabilityCheck CheckAuthentication(
		const HarnessOpaqueId& id, EHarnessGrant presentedGrants,
		const HarnessCapabilityContext& context, EHarnessGrant required,
		std::span<const std::uint8_t> transcript, std::span<const std::uint8_t> digest,
		std::chrono::steady_clock::time_point now) const noexcept;
	[[nodiscard]] std::size_t Size() const noexcept;

private:
	struct Record final {
		HarnessCapabilityCredential credential;
		HarnessCapabilityContext context;
		std::chrono::steady_clock::time_point expiresAt{};
		bool revoked = false;
	};

	mutable std::mutex m_mutex;
	std::map<HarnessOpaqueId, Record> m_records;
};

} // namespace platform::harnessbridge
