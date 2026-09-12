/*! @file
 * @brief Control-owned admission, deduplication and polling for GitHub reads.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/github/GhRepositoryRead.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace senp::github {

enum class GhReadPollCadence : std::uint8_t {
	Manual,
	List,
	Active,
};

enum class GhReadSubscribeStatus : std::uint8_t {
	Accepted,
	InvalidScope,
	QueueFull,
	SubscriptionLimit,
	Closed,
};

enum class GhReadMutationStatus : std::uint8_t {
	Applied,
	NotFound,
	NotVisible,
	StaleDispatch,
	Closed,
};

enum class GhReadPhase : std::uint8_t {
	Hidden,
	Queued,
	Running,
	Cancelling,
	Completed,
	RateLimited,
	Closed,
};

class GhReadResourceKey final {
public:
	GhReadResourceKey(std::wstring profileId, std::uint64_t accountGeneration,
		std::wstring repositoryIdentity, GhRepositoryReadRequest request);
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] std::uint64_t AccountGeneration() const noexcept { return m_accountGeneration; }
	[[nodiscard]] const std::wstring& RepositoryIdentity() const noexcept { return m_repositoryIdentity; }
	[[nodiscard]] const GhRepositoryReadRequest& Request() const noexcept { return m_request; }
	[[nodiscard]] bool Valid() const noexcept;
	[[nodiscard]] bool SameResource(const GhReadResourceKey& other) const noexcept;
	[[nodiscard]] bool SameAccount(const GhReadResourceKey& other) const noexcept;
private:
	std::wstring m_profileId;
	std::uint64_t m_accountGeneration{};
	std::wstring m_repositoryIdentity;
	GhRepositoryReadRequest m_request;
};

class GhReadObservation final {
public:
	GhReadObservation(GhReadPhase phase, std::uint64_t cycle,
		std::size_t subscriberCount, std::size_t visibleSubscriberCount,
		std::optional<GhRepositoryResponseStatus> terminal,
		std::optional<std::uint64_t> nextPollAtMilliseconds,
		std::optional<std::uint64_t> nextAllowedAtMilliseconds) noexcept;
	[[nodiscard]] GhReadPhase Phase() const noexcept { return m_phase; }
	[[nodiscard]] std::uint64_t Cycle() const noexcept { return m_cycle; }
	[[nodiscard]] std::size_t SubscriberCount() const noexcept { return m_subscriberCount; }
	[[nodiscard]] std::size_t VisibleSubscriberCount() const noexcept { return m_visibleSubscriberCount; }
	[[nodiscard]] const std::optional<GhRepositoryResponseStatus>& Terminal() const noexcept { return m_terminal; }
	[[nodiscard]] const std::optional<std::uint64_t>& NextPollAtMilliseconds() const noexcept { return m_nextPollAtMilliseconds; }
	[[nodiscard]] const std::optional<std::uint64_t>& NextAllowedAtMilliseconds() const noexcept { return m_nextAllowedAtMilliseconds; }
private:
	GhReadPhase m_phase{ GhReadPhase::Hidden };
	std::uint64_t m_cycle{};
	std::size_t m_subscriberCount{}, m_visibleSubscriberCount{};
	std::optional<GhRepositoryResponseStatus> m_terminal;
	std::optional<std::uint64_t> m_nextPollAtMilliseconds;
	std::optional<std::uint64_t> m_nextAllowedAtMilliseconds;
};

class CGhReadScheduler;
class GhReadSchedulerState;

class GhReadDispatch final {
public:
	GhReadDispatch() = default;
	[[nodiscard]] std::uint64_t Ticket() const noexcept { return m_ticket; }
	[[nodiscard]] std::uint64_t Cycle() const noexcept { return m_cycle; }
	[[nodiscard]] const GhReadResourceKey& Key() const noexcept { return m_key; }
	[[nodiscard]] HANDLE StopHandle() const noexcept;
	[[nodiscard]] bool CancellationRequested() const noexcept;
private:
	class StopSignal;
	GhReadDispatch(std::uint64_t ticket, std::uint64_t cycle,
		GhReadResourceKey key, std::shared_ptr<StopSignal> stopSignal);
	std::uint64_t m_ticket{}, m_cycle{};
	GhReadResourceKey m_key{ L"", 0, L"", GhRepositoryReadRequest(L"", L"", L"", {}) };
	std::shared_ptr<StopSignal> m_stopSignal;
	friend class CGhReadScheduler;
	friend class GhReadSchedulerState;
};

/*!
	@brief One admitted subscription to a shared read.

	Admission is this handle's construction and release is its destruction, and
	the scheduler offers no second spelling for either.  Dropping whatever record
	owns the handle - a read, a scope, a local in a test - is therefore the whole
	of the release, so an admitted subscription cannot be left behind by a path
	that forgot to name it.

	Move-only.  A moved-from handle, a refused admission, and a handle whose
	scheduler has already been destroyed all own nothing, and every operation on
	them reports `NotFound` rather than reaching for state that is gone.
*/
class GhReadSubscription final {
public:
	GhReadSubscription(CGhReadScheduler& scheduler, const GhReadResourceKey& key,
		GhReadPollCadence cadence, bool visible, std::uint64_t nowMilliseconds);
	~GhReadSubscription();
	GhReadSubscription(const GhReadSubscription&) = delete;
	GhReadSubscription& operator=(const GhReadSubscription&) = delete;
	GhReadSubscription(GhReadSubscription&& other) noexcept;
	GhReadSubscription& operator=(GhReadSubscription&& other) noexcept;

	//! Why admission succeeded or was refused.
	[[nodiscard]] GhReadSubscribeStatus Status() const noexcept { return m_status; }
	//! Whether this handle owns a subscription that its destruction will release.
	[[nodiscard]] bool Admitted() const noexcept { return m_id != 0; }
	[[nodiscard]] GhReadMutationStatus SetVisible(bool visible, std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] GhReadMutationStatus RequestRefresh(std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] std::optional<GhReadObservation> Poll(std::uint64_t nowMilliseconds) noexcept;
	//! Releases now rather than at destruction.  Idempotent, and the destructor
	//! performs exactly this, so an early release is never a second one.
	void Release() noexcept;
private:
	std::weak_ptr<GhReadSchedulerState> m_state;
	std::uint64_t m_id{};
	GhReadSubscribeStatus m_status{ GhReadSubscribeStatus::InvalidScope };
};

//! Cooperative scheduler. The broker owns worker execution and must call
//! Complete after every dispatch, including after cancellation or Close.
class CGhReadScheduler final {
public:
	[[nodiscard]] static constexpr std::size_t MaximumResources() noexcept { return 64; }
	[[nodiscard]] static constexpr std::size_t MaximumAccountLanes() noexcept { return 64; }
	[[nodiscard]] static constexpr std::size_t MaximumSubscriptions() noexcept { return 256; }
	CGhReadScheduler();
	~CGhReadScheduler();
	CGhReadScheduler(const CGhReadScheduler&) = delete;
	CGhReadScheduler& operator=(const CGhReadScheduler&) = delete;
	[[nodiscard]] std::optional<GhReadDispatch> TryDispatch(std::uint64_t nowMilliseconds);
	[[nodiscard]] GhReadMutationStatus Complete(std::uint64_t ticket,
		const GhRepositoryResponse& response, std::uint64_t nowMilliseconds,
		std::uint64_t nowUnixSeconds) noexcept;
	void Close() noexcept;
	[[nodiscard]] std::size_t QueuedCount(std::uint64_t nowMilliseconds) noexcept;
	[[nodiscard]] std::size_t RunningCount() const noexcept;
	[[nodiscard]] std::size_t OutstandingCleanupCount() const noexcept;
private:
	friend class GhReadSubscription;

	std::shared_ptr<GhReadSchedulerState> m_state;
};

} // namespace senp::github
