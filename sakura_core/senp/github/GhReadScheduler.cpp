/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhReadScheduler.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace senp::github {
namespace {

constexpr std::uint64_t kListPollMilliseconds = 60000;
constexpr std::uint64_t kActivePollMilliseconds = 15000;
constexpr std::uint64_t kUnknownRateLimitMilliseconds = 60000;
constexpr std::uint64_t kMaximumCooldownMilliseconds = 24ULL * 60ULL * 60ULL * 1000ULL;

std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept
{
	return right > std::numeric_limits<std::uint64_t>::max() - left
		? std::numeric_limits<std::uint64_t>::max() : left + right;
}

bool SameQuery(const GhRepositoryReadRequest& left, const GhRepositoryReadRequest& right) noexcept
{
	return left.Query() == right.Query();
}

} // namespace

GhReadResourceKey::GhReadResourceKey(std::wstring profileId, const std::uint64_t accountGeneration,
	std::wstring repositoryIdentity, GhRepositoryReadRequest request) :
	m_profileId(std::move(profileId)), m_accountGeneration(accountGeneration),
	m_repositoryIdentity(std::move(repositoryIdentity)), m_request(std::move(request))
{
	auto query = m_request.Query();
	std::ranges::sort(query);
	m_request = GhRepositoryReadRequest(m_request.Hostname(), m_request.Owner(), m_request.Repository(),
		m_request.ResourceSegments(), std::move(query), m_request.IfNoneMatch());
}

bool GhReadResourceKey::Valid() const noexcept
{
	if (m_profileId.empty() || m_profileId.size() > 256 || m_accountGeneration == 0
		|| m_repositoryIdentity.empty() || m_repositoryIdentity.size() > 256) return false;
	const auto& request = m_request;
	if (request.Hostname().empty() || request.Hostname().size() > 253
		|| request.Owner().empty() || request.Owner().size() > 100
		|| request.Repository().empty() || request.Repository().size() > 100
		|| request.ResourceSegments().empty() || request.ResourceSegments().size() > 16
		|| request.Query().size() > 16) return false;
	std::size_t characters{};
	for (const auto& segment : request.ResourceSegments()) characters += segment.size();
	for (const auto& [name, value] : request.Query()) characters += name.size() + value.size();
	return characters <= 4096;
}

bool GhReadResourceKey::SameResource(const GhReadResourceKey& other) const noexcept
{
	return SameAccount(other) && m_repositoryIdentity == other.m_repositoryIdentity
		&& m_request.Owner() == other.m_request.Owner()
		&& m_request.Repository() == other.m_request.Repository()
		&& m_request.ResourceSegments() == other.m_request.ResourceSegments()
		&& SameQuery(m_request, other.m_request);
}

bool GhReadResourceKey::SameAccount(const GhReadResourceKey& other) const noexcept
{
	return m_profileId == other.m_profileId && m_accountGeneration == other.m_accountGeneration
		&& m_request.Hostname() == other.m_request.Hostname();
}

GhReadObservation::GhReadObservation(const GhReadPhase phase, const std::uint64_t cycle,
	const std::size_t subscriberCount, const std::size_t visibleSubscriberCount,
	std::optional<GhRepositoryResponseStatus> terminal,
	std::optional<std::uint64_t> nextPollAtMilliseconds,
	std::optional<std::uint64_t> nextAllowedAtMilliseconds) noexcept :
	m_phase(phase), m_cycle(cycle), m_subscriberCount(subscriberCount),
	m_visibleSubscriberCount(visibleSubscriberCount), m_terminal(std::move(terminal)),
	m_nextPollAtMilliseconds(std::move(nextPollAtMilliseconds)),
	m_nextAllowedAtMilliseconds(std::move(nextAllowedAtMilliseconds)) {}

class GhReadDispatch::StopSignal final {
public:
	StopSignal() noexcept : m_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
	~StopSignal() { if (m_event) ::CloseHandle(m_event); }
	StopSignal(const StopSignal&) = delete;
	StopSignal& operator=(const StopSignal&) = delete;
	[[nodiscard]] bool Valid() const noexcept { return m_event != nullptr; }
	[[nodiscard]] HANDLE Handle() const noexcept { return m_event; }
	void Signal() noexcept { if (m_event) ::SetEvent(m_event); }
	[[nodiscard]] bool Signaled() const noexcept
	{
		return m_event && ::WaitForSingleObject(m_event, 0) == WAIT_OBJECT_0;
	}
private:
	HANDLE m_event{};
};

GhReadDispatch::GhReadDispatch(const std::uint64_t ticket, const std::uint64_t cycle,
	GhReadResourceKey key, std::shared_ptr<StopSignal> stopSignal) :
	m_ticket(ticket), m_cycle(cycle), m_key(std::move(key)), m_stopSignal(std::move(stopSignal)) {}

HANDLE GhReadDispatch::StopHandle() const noexcept
{
	return m_stopSignal ? m_stopSignal->Handle() : nullptr;
}

bool GhReadDispatch::CancellationRequested() const noexcept
{
	return m_stopSignal && m_stopSignal->Signaled();
}

class CGhReadScheduler::Impl final {
public:
	class Subscription final {
	public:
		Subscription(const std::uint64_t subscriptionId, const GhReadPollCadence pollCadence,
			const bool isVisible) noexcept : id(subscriptionId), cadence(pollCadence), visible(isVisible) {}
private:
		std::uint64_t id{};
		GhReadPollCadence cadence{ GhReadPollCadence::Manual };
		bool visible{};
		friend class Impl;
		friend class CGhReadScheduler;
	};
	class Entry final {
public:
		explicit Entry(GhReadResourceKey resourceKey) : key(std::move(resourceKey)) {}
	private:
		GhReadResourceKey key;
		std::vector<Subscription> subscriptions;
		GhReadPhase phase{ GhReadPhase::Hidden };
		std::uint64_t cycle{}, queueSequence{}, ticket{};
		std::shared_ptr<GhReadDispatch::StopSignal> stopSignal;
		std::optional<GhRepositoryResponseStatus> terminal;
		std::optional<std::uint64_t> nextPollAt;
		friend class Impl;
		friend class CGhReadScheduler;
	};
	class Lane final {
	public:
		explicit Lane(const GhReadResourceKey& key) : account(key) {}
	private:
		GhReadResourceKey account;
		std::uint64_t cooldownUntil{};
		friend class Impl;
		friend class CGhReadScheduler;
	};

	private:
	std::mutex mutex;
	std::vector<std::unique_ptr<Entry>> entries;
	std::vector<Lane> lanes;
	std::uint64_t nextSubscription{}, nextQueueSequence{}, nextTicket{};
	bool closed{};
	friend class CGhReadScheduler;

public:
	Entry* FindEntry(const GhReadResourceKey& key) noexcept
	{
		const auto found = std::ranges::find_if(entries, [&](const auto& entry) { return entry->key.SameResource(key); });
		return found == entries.end() ? nullptr : found->get();
	}

	std::pair<Entry*, Subscription*> FindSubscription(const std::uint64_t id) noexcept
	{
		for (const auto& entry : entries) {
			const auto found = std::ranges::find_if(entry->subscriptions,
				[&](const Subscription& subscription) { return subscription.id == id; });
			if (found != entry->subscriptions.end()) return { entry.get(), &*found };
		}
		return {};
	}

	Lane* FindLane(const GhReadResourceKey& key) noexcept
	{
		const auto found = std::ranges::find_if(lanes, [&](const Lane& lane) { return lane.account.SameAccount(key); });
		return found == lanes.end() ? nullptr : &*found;
	}

	std::size_t SubscriptionCount() const noexcept
	{
		std::size_t count{};
		for (const auto& entry : entries) count += entry->subscriptions.size();
		return count;
	}

	static std::size_t VisibleCount(const Entry& entry) noexcept
	{
		return static_cast<std::size_t>(std::ranges::count_if(entry.subscriptions,
			[](const Subscription& subscription) { return subscription.visible; }));
	}

	static std::optional<std::uint64_t> PollInterval(const Entry& entry) noexcept
	{
		bool list{};
		for (const auto& subscription : entry.subscriptions) {
			if (!subscription.visible) continue;
			if (subscription.cadence == GhReadPollCadence::Active) return kActivePollMilliseconds;
			if (subscription.cadence == GhReadPollCadence::List) list = true;
		}
		return list ? std::optional<std::uint64_t>(kListPollMilliseconds) : std::nullopt;
	}

	void Queue(Entry& entry) noexcept
	{
		entry.phase = GhReadPhase::Queued;
		entry.cycle++;
		entry.queueSequence = ++nextQueueSequence;
		entry.terminal.reset();
		entry.nextPollAt.reset();
	}

	void RefreshDue(const std::uint64_t now) noexcept
	{
		for (const auto& entry : entries) {
			if ((entry->phase == GhReadPhase::Completed || entry->phase == GhReadPhase::RateLimited)
				&& entry->nextPollAt && *entry->nextPollAt <= now && VisibleCount(*entry) != 0) Queue(*entry);
		}
	}

	bool AccountRunning(const GhReadResourceKey& key) const noexcept
	{
		return std::ranges::any_of(entries, [&](const auto& entry) {
			return entry->key.SameAccount(key)
				&& (entry->phase == GhReadPhase::Running || entry->phase == GhReadPhase::Cancelling);
		});
	}

	std::optional<std::uint64_t> Cooldown(const GhReadResourceKey& key, const std::uint64_t now) noexcept
	{
		const auto lane = FindLane(key);
		return lane && lane->cooldownUntil > now ? std::optional<std::uint64_t>(lane->cooldownUntil) : std::nullopt;
	}

	void CancelIfUnobserved(Entry& entry) noexcept
	{
		if (VisibleCount(entry) != 0) return;
		if (entry.phase == GhReadPhase::Running) {
			entry.phase = GhReadPhase::Cancelling;
			if (entry.stopSignal) entry.stopSignal->Signal();
		} else if (entry.phase != GhReadPhase::Cancelling) {
			entry.phase = GhReadPhase::Hidden;
			entry.terminal.reset();
			entry.nextPollAt.reset();
		}
	}

	void EraseUnused() noexcept
	{
		std::erase_if(entries, [](const auto& entry) {
			return entry->subscriptions.empty()
				&& entry->phase != GhReadPhase::Running && entry->phase != GhReadPhase::Cancelling;
		});
	}

	void EraseExpiredLanes(const std::uint64_t now) noexcept
	{
		std::erase_if(lanes, [&](const Lane& lane) {
			return lane.cooldownUntil <= now && std::ranges::none_of(entries,
				[&](const auto& entry) { return lane.account.SameAccount(entry->key); });
		});
	}
};

CGhReadScheduler::CGhReadScheduler() : m_impl(std::make_shared<Impl>()) {}
CGhReadScheduler::~CGhReadScheduler() { Close(); }

GhReadSubscriptionResult CGhReadScheduler::Subscribe(const GhReadResourceKey& key,
	const GhReadPollCadence cadence, const bool visible, const std::uint64_t nowMilliseconds)
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return { GhReadSubscribeStatus::Closed, 0 };
	if (!key.Valid()) return { GhReadSubscribeStatus::InvalidScope, 0 };
	m_impl->EraseUnused();
	if (m_impl->SubscriptionCount() >= MaximumSubscriptions()) return { GhReadSubscribeStatus::SubscriptionLimit, 0 };
	m_impl->RefreshDue(nowMilliseconds);
	m_impl->EraseExpiredLanes(nowMilliseconds);
	auto entry = m_impl->FindEntry(key);
	if (!entry) {
		if (m_impl->entries.size() >= MaximumResources()) return { GhReadSubscribeStatus::QueueFull, 0 };
		if (!m_impl->FindLane(key) && m_impl->lanes.size() >= MaximumAccountLanes()) {
			return { GhReadSubscribeStatus::QueueFull, 0 };
		}
		auto added = std::make_unique<Impl::Entry>(key);
		entry = added.get();
		if (!m_impl->FindLane(key)) m_impl->lanes.emplace_back(key);
		m_impl->entries.push_back(std::move(added));
	}
	const auto id = ++m_impl->nextSubscription;
	entry->subscriptions.push_back({ id, cadence, visible });
	if (visible && entry->phase == GhReadPhase::Hidden) m_impl->Queue(*entry);
	return { GhReadSubscribeStatus::Accepted, id };
}

GhReadMutationStatus CGhReadScheduler::SetVisible(const std::uint64_t subscriptionId,
	const bool visible, const std::uint64_t nowMilliseconds) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return GhReadMutationStatus::Closed;
	const auto [entry, subscription] = m_impl->FindSubscription(subscriptionId);
	if (!entry) return GhReadMutationStatus::NotFound;
	if (subscription->visible == visible) return GhReadMutationStatus::Applied;
	subscription->visible = visible;
	if (visible && entry->phase == GhReadPhase::Hidden) m_impl->Queue(*entry);
	else if (!visible) m_impl->CancelIfUnobserved(*entry);
	m_impl->RefreshDue(nowMilliseconds);
	return GhReadMutationStatus::Applied;
}

GhReadMutationStatus CGhReadScheduler::RequestRefresh(const std::uint64_t subscriptionId,
	const std::uint64_t nowMilliseconds) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return GhReadMutationStatus::Closed;
	const auto [entry, subscription] = m_impl->FindSubscription(subscriptionId);
	if (!entry) return GhReadMutationStatus::NotFound;
	if (!subscription->visible) return GhReadMutationStatus::NotVisible;
	if (entry->phase == GhReadPhase::Completed || entry->phase == GhReadPhase::RateLimited
		|| entry->phase == GhReadPhase::Hidden) m_impl->Queue(*entry);
	m_impl->RefreshDue(nowMilliseconds);
	return GhReadMutationStatus::Applied;
}

GhReadMutationStatus CGhReadScheduler::Unsubscribe(const std::uint64_t subscriptionId) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return GhReadMutationStatus::Closed;
	const auto [entry, subscription] = m_impl->FindSubscription(subscriptionId);
	if (!entry) return GhReadMutationStatus::NotFound;
	const auto removedId = subscription->id;
	std::erase_if(entry->subscriptions,
		[&](const Impl::Subscription& candidate) { return candidate.id == removedId; });
	m_impl->CancelIfUnobserved(*entry);
	m_impl->EraseUnused();
	return GhReadMutationStatus::Applied;
}

std::optional<GhReadObservation> CGhReadScheduler::Poll(const std::uint64_t subscriptionId,
	const std::uint64_t nowMilliseconds) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return std::nullopt;
	m_impl->RefreshDue(nowMilliseconds);
	const auto [entry, subscription] = m_impl->FindSubscription(subscriptionId);
	if (!entry) return std::nullopt;
	(void)subscription;
	return GhReadObservation(entry->phase, entry->cycle, entry->subscriptions.size(),
		Impl::VisibleCount(*entry), entry->terminal, entry->nextPollAt,
		m_impl->Cooldown(entry->key, nowMilliseconds));
}

std::optional<GhReadDispatch> CGhReadScheduler::TryDispatch(const std::uint64_t nowMilliseconds)
{
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return std::nullopt;
	m_impl->RefreshDue(nowMilliseconds);
	Impl::Entry* selected{};
	for (const auto& entry : m_impl->entries) {
		if (entry->phase != GhReadPhase::Queued || Impl::VisibleCount(*entry) == 0
			|| m_impl->AccountRunning(entry->key) || m_impl->Cooldown(entry->key, nowMilliseconds)) continue;
		if (!selected || entry->queueSequence < selected->queueSequence) selected = entry.get();
	}
	if (!selected) return std::nullopt;
	auto stopSignal = std::make_shared<GhReadDispatch::StopSignal>();
	if (!stopSignal->Valid()) {
		selected->phase = GhReadPhase::Completed;
		selected->terminal = GhRepositoryResponseStatus::ToolUnavailable;
		return std::nullopt;
	}
	const auto ticket = ++m_impl->nextTicket;
	GhReadDispatch dispatch(ticket, selected->cycle, selected->key, stopSignal);
	selected->ticket = ticket;
	selected->stopSignal = std::move(stopSignal);
	selected->phase = GhReadPhase::Running;
	return dispatch;
}

GhReadMutationStatus CGhReadScheduler::Complete(const std::uint64_t ticket,
	const GhRepositoryResponse& response, const std::uint64_t nowMilliseconds,
	const std::uint64_t nowUnixSeconds) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	const auto found = std::ranges::find_if(m_impl->entries,
		[&](const auto& entry) { return entry->ticket == ticket && ticket != 0; });
	if (found == m_impl->entries.end()) return m_impl->closed
		? GhReadMutationStatus::Closed : GhReadMutationStatus::StaleDispatch;
	auto& entry = **found;
	if (entry.phase != GhReadPhase::Running && entry.phase != GhReadPhase::Cancelling) {
		return GhReadMutationStatus::StaleDispatch;
	}
	const bool wasCancelling = entry.phase == GhReadPhase::Cancelling;
	entry.ticket = 0;
	entry.stopSignal.reset();
	if (m_impl->closed) {
		entry.phase = GhReadPhase::Closed;
		m_impl->EraseUnused();
		return GhReadMutationStatus::Applied;
	}
	if (wasCancelling) {
		if (Impl::VisibleCount(entry) != 0) m_impl->Queue(entry);
		else entry.phase = GhReadPhase::Hidden;
		m_impl->EraseUnused();
		return GhReadMutationStatus::Applied;
	}
	entry.terminal = response.Status();
	entry.phase = response.Status() == GhRepositoryResponseStatus::RateLimited
		? GhReadPhase::RateLimited : GhReadPhase::Completed;
	std::optional<std::uint64_t> cooldown;
	if (response.Status() == GhRepositoryResponseStatus::RateLimited) {
		if (response.RetryAfterSeconds()) {
			cooldown = SaturatingAdd(nowMilliseconds,
				std::min<std::uint64_t>(*response.RetryAfterSeconds() * 1000ULL, kMaximumCooldownMilliseconds));
		}
		if (response.RateLimitResetUnixSeconds() && *response.RateLimitResetUnixSeconds() > nowUnixSeconds) {
			const auto delta = std::min<std::uint64_t>(
				*response.RateLimitResetUnixSeconds() - nowUnixSeconds, kMaximumCooldownMilliseconds / 1000ULL);
			const auto reset = SaturatingAdd(nowMilliseconds, delta * 1000ULL);
			cooldown = cooldown ? std::max(*cooldown, reset) : reset;
		}
		if (!cooldown) cooldown = SaturatingAdd(nowMilliseconds, kUnknownRateLimitMilliseconds);
		if (auto lane = m_impl->FindLane(entry.key)) lane->cooldownUntil = std::max(lane->cooldownUntil, *cooldown);
	}
	if (const auto interval = Impl::PollInterval(entry)) {
		entry.nextPollAt = SaturatingAdd(nowMilliseconds, *interval);
		if (cooldown) entry.nextPollAt = std::max(*entry.nextPollAt, *cooldown);
	} else {
		entry.nextPollAt.reset();
	}
	return GhReadMutationStatus::Applied;
}

void CGhReadScheduler::Close() noexcept
{
	if (!m_impl) return;
	std::scoped_lock lock(m_impl->mutex);
	if (m_impl->closed) return;
	m_impl->closed = true;
	for (const auto& entry : m_impl->entries) {
		entry->subscriptions.clear();
		entry->terminal.reset();
		entry->nextPollAt.reset();
		if (entry->phase == GhReadPhase::Running || entry->phase == GhReadPhase::Cancelling) {
			entry->phase = GhReadPhase::Cancelling;
			if (entry->stopSignal) entry->stopSignal->Signal();
		} else {
			entry->phase = GhReadPhase::Closed;
		}
	}
	m_impl->EraseUnused();
}

std::size_t CGhReadScheduler::QueuedCount(const std::uint64_t nowMilliseconds) noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	m_impl->RefreshDue(nowMilliseconds);
	return static_cast<std::size_t>(std::ranges::count_if(m_impl->entries,
		[](const auto& entry) { return entry->phase == GhReadPhase::Queued; }));
}

std::size_t CGhReadScheduler::RunningCount() const noexcept
{
	std::scoped_lock lock(m_impl->mutex);
	return static_cast<std::size_t>(std::ranges::count_if(m_impl->entries, [](const auto& entry) {
		return entry->phase == GhReadPhase::Running || entry->phase == GhReadPhase::Cancelling;
	}));
}

std::size_t CGhReadScheduler::OutstandingCleanupCount() const noexcept
{
	return RunningCount();
}

} // namespace senp::github
