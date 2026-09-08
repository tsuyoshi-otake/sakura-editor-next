/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhReadScheduler.h"

namespace {
using namespace senp::github;

GhReadResourceKey Key(const std::wstring& resource = L"issues",
	const std::wstring& profile = L"profile-1", const std::uint64_t account = 1,
	const std::wstring& repositoryIdentity = L"repo-id-1",
	std::vector<std::pair<std::wstring, std::wstring>> query = {},
	std::optional<std::wstring> etag = std::nullopt)
{
	return { profile, account, repositoryIdentity,
		GhRepositoryReadRequest(L"github.com", L"owner", L"repo", { resource },
			std::move(query), std::move(etag)) };
}

GhRepositoryResponse Response(const GhRepositoryResponseStatus status,
	const int httpStatus = 200, std::optional<std::uint32_t> retryAfter = std::nullopt,
	std::optional<std::uint64_t> reset = std::nullopt)
{
	return { status, httpStatus, {}, std::nullopt, 1, std::nullopt,
		std::move(retryAfter), std::move(reset) };
}

std::uint64_t Subscribe(CGhReadScheduler& scheduler, const GhReadResourceKey& key,
	const GhReadPollCadence cadence = GhReadPollCadence::Manual, const bool visible = true,
	const std::uint64_t now = 0)
{
	const auto result = scheduler.Subscribe(key, cadence, visible, now);
	EXPECT_EQ(GhReadSubscribeStatus::Accepted, result.Status());
	return result.SubscriptionId();
}

} // namespace

TEST(GhReadScheduler, SharesOneResourceAcrossFiveWindowsAndTwoExtensions)
{
	CGhReadScheduler scheduler;
	std::vector<std::uint64_t> subscriptions;
	for (int window = 0; window < 5; ++window) {
		for (int extension = 0; extension < 2; ++extension) {
			(void)window;
			(void)extension;
			subscriptions.push_back(Subscribe(scheduler, Key()));
		}
	}
	EXPECT_EQ(1U, scheduler.QueuedCount(0));
	const auto dispatch = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch);
	EXPECT_FALSE(scheduler.TryDispatch(0));
	const auto observation = scheduler.Poll(subscriptions.front(), 0);
	ASSERT_TRUE(observation);
	EXPECT_EQ(GhReadPhase::Running, observation->Phase());
	EXPECT_EQ(10U, observation->SubscriberCount());
	EXPECT_EQ(10U, observation->VisibleSubscriberCount());
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch->Ticket(), Response(GhRepositoryResponseStatus::Succeeded), 10, 100));
	for (const auto subscription : subscriptions) {
		const auto completed = scheduler.Poll(subscription, 10);
		ASSERT_TRUE(completed);
		EXPECT_EQ(GhRepositoryResponseStatus::Succeeded, completed->Terminal());
	}
}

TEST(GhReadScheduler, AllowsOneReadPerAccountAndSelectsEligibleResourcesInFifoOrder)
{
	CGhReadScheduler scheduler;
	const auto first = Subscribe(scheduler, Key(L"issues"));
	const auto second = Subscribe(scheduler, Key(L"pulls"));
	const auto otherAccount = Subscribe(scheduler, Key(L"actions", L"profile-1", 2, L"repo-id-1"));
	const auto dispatch1 = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch1);
	EXPECT_EQ(L"issues", dispatch1->Key().Request().ResourceSegments().front());
	const auto dispatch2 = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch2);
	EXPECT_EQ(2U, dispatch2->Key().AccountGeneration());
	EXPECT_FALSE(scheduler.TryDispatch(0));
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch1->Ticket(), Response(GhRepositoryResponseStatus::Succeeded), 1, 100));
	const auto dispatch3 = scheduler.TryDispatch(1);
	ASSERT_TRUE(dispatch3);
	EXPECT_EQ(L"pulls", dispatch3->Key().Request().ResourceSegments().front());
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch2->Ticket(), Response(GhRepositoryResponseStatus::Succeeded), 1, 100));
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch3->Ticket(), Response(GhRepositoryResponseStatus::Succeeded), 2, 100));
	EXPECT_TRUE(scheduler.Poll(first, 2));
	EXPECT_TRUE(scheduler.Poll(second, 2));
	EXPECT_TRUE(scheduler.Poll(otherAccount, 2));
}

TEST(GhReadScheduler, AppliesTypedRateLimitCooldownWithoutTreatingOrdinaryForbiddenAsRateLimited)
{
	CGhReadScheduler scheduler;
	const auto limited = Subscribe(scheduler, Key(L"issues"));
	const auto waiting = Subscribe(scheduler, Key(L"pulls"));
	const auto first = scheduler.TryDispatch(1000);
	ASSERT_TRUE(first);
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.Complete(first->Ticket(),
		Response(GhRepositoryResponseStatus::RateLimited, 429, 120, 150), 1000, 100));
	const auto observation = scheduler.Poll(limited, 1000);
	ASSERT_TRUE(observation);
	EXPECT_EQ(GhReadPhase::RateLimited, observation->Phase());
	ASSERT_TRUE(observation->NextAllowedAtMilliseconds());
	EXPECT_EQ(121000U, *observation->NextAllowedAtMilliseconds());
	EXPECT_FALSE(scheduler.TryDispatch(120999));
	const auto second = scheduler.TryDispatch(121000);
	ASSERT_TRUE(second);
	EXPECT_EQ(L"pulls", second->Key().Request().ResourceSegments().front());
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.Complete(second->Ticket(),
		Response(GhRepositoryResponseStatus::Forbidden, 403), 121001, 220));
	const auto thirdId = Subscribe(scheduler, Key(L"actions"), GhReadPollCadence::Manual, true, 121001);
	const auto third = scheduler.TryDispatch(121001);
	ASSERT_TRUE(third);
	EXPECT_EQ(L"actions", third->Key().Request().ResourceSegments().front());
	EXPECT_TRUE(scheduler.Poll(waiting, 121001));
	EXPECT_TRUE(scheduler.Poll(thirdId, 121001));
}

TEST(GhReadScheduler, PollsOnlyVisibleSubscribersAtTheFastestVisibleCadence)
{
	CGhReadScheduler scheduler;
	const auto hidden = Subscribe(scheduler, Key(), GhReadPollCadence::List, false);
	EXPECT_EQ(0U, scheduler.QueuedCount(0));
	EXPECT_FALSE(scheduler.TryDispatch(0));
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.SetVisible(hidden, true, 0));
	const auto active = Subscribe(scheduler, Key(), GhReadPollCadence::Active, true, 0);
	const auto dispatch = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch);
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch->Ticket(), Response(GhRepositoryResponseStatus::NotModified, 304), 1000, 100));
	const auto completed = scheduler.Poll(hidden, 15999);
	ASSERT_TRUE(completed);
	EXPECT_EQ(GhReadPhase::Completed, completed->Phase());
	EXPECT_EQ(16000U, completed->NextPollAtMilliseconds());
	EXPECT_FALSE(scheduler.TryDispatch(15999));
	const auto next = scheduler.TryDispatch(16000);
	ASSERT_TRUE(next);
	EXPECT_EQ(2U, next->Cycle());
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.SetVisible(active, false, 16000));
	EXPECT_FALSE(next->CancellationRequested());
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.SetVisible(hidden, false, 16000));
	EXPECT_TRUE(next->CancellationRequested());
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(next->Ticket(), Response(GhRepositoryResponseStatus::Cancelled), 16001, 116));
	EXPECT_EQ(GhReadPhase::Hidden, scheduler.Poll(hidden, 16001)->Phase());
}

TEST(GhReadScheduler, CancelsOnlyAfterTheLastVisibleSubscriberAndRejectsDelayedCompletion)
{
	CGhReadScheduler scheduler;
	const auto first = Subscribe(scheduler, Key());
	const auto second = Subscribe(scheduler, Key());
	const auto dispatch = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch);
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.Unsubscribe(first));
	EXPECT_FALSE(dispatch->CancellationRequested());
	EXPECT_EQ(GhReadMutationStatus::Applied, scheduler.Unsubscribe(second));
	EXPECT_TRUE(dispatch->CancellationRequested());
	EXPECT_EQ(1U, scheduler.OutstandingCleanupCount());
	const auto replacement = Subscribe(scheduler, Key(), GhReadPollCadence::Manual, true, 1);
	EXPECT_FALSE(scheduler.TryDispatch(1));
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch->Ticket(), Response(GhRepositoryResponseStatus::Cancelled), 2, 100));
	const auto next = scheduler.TryDispatch(2);
	ASSERT_TRUE(next);
	EXPECT_NE(dispatch->Ticket(), next->Ticket());
	EXPECT_EQ(GhReadMutationStatus::StaleDispatch,
		scheduler.Complete(dispatch->Ticket(), Response(GhRepositoryResponseStatus::Succeeded), 3, 100));
	EXPECT_EQ(GhReadPhase::Running, scheduler.Poll(replacement, 3)->Phase());
}

TEST(GhReadScheduler, BoundsUniqueResourcesButStillJoinsAnExistingFlight)
{
	CGhReadScheduler scheduler;
	std::uint64_t first{};
	for (std::size_t i = 0; i < CGhReadScheduler::MaximumResources(); ++i) {
		const auto result = scheduler.Subscribe(Key(L"resource-" + std::to_wstring(i)),
			GhReadPollCadence::Manual, true, 0);
		ASSERT_EQ(GhReadSubscribeStatus::Accepted, result.Status());
		if (i == 0) first = result.SubscriptionId();
	}
	EXPECT_EQ(CGhReadScheduler::MaximumResources(), scheduler.QueuedCount(0));
	EXPECT_EQ(GhReadSubscribeStatus::QueueFull,
		scheduler.Subscribe(Key(L"overflow"), GhReadPollCadence::Manual, true, 0).Status());
	const auto joined = scheduler.Subscribe(Key(L"resource-0"), GhReadPollCadence::Manual, true, 0);
	EXPECT_EQ(GhReadSubscribeStatus::Accepted, joined.Status());
	EXPECT_EQ(2U, scheduler.Poll(first, 0)->SubscriberCount());
}

TEST(GhReadScheduler, CanonicalizesQueryOrderButKeepsAuthorityScopesSeparate)
{
	const auto one = Key(L"issues", L"profile-1", 7, L"repo-id",
		{ { L"state", L"open" }, { L"page", L"2" } }, L"\"old\"");
	const auto reordered = Key(L"issues", L"profile-1", 7, L"repo-id",
		{ { L"page", L"2" }, { L"state", L"open" } }, L"\"new\"");
	EXPECT_TRUE(one.SameResource(reordered));
	EXPECT_FALSE(one.SameResource(Key(L"issues", L"profile-2", 7, L"repo-id")));
	EXPECT_FALSE(one.SameResource(Key(L"issues", L"profile-1", 8, L"repo-id")));
	EXPECT_FALSE(one.SameResource(Key(L"issues", L"profile-1", 7, L"other-repo")));
}

TEST(GhReadScheduler, CloseCancelsWorkAndRetainsCleanupOwnershipUntilCompletion)
{
	CGhReadScheduler scheduler;
	const auto subscription = Subscribe(scheduler, Key());
	const auto dispatch = scheduler.TryDispatch(0);
	ASSERT_TRUE(dispatch);
	scheduler.Close();
	EXPECT_TRUE(dispatch->CancellationRequested());
	EXPECT_EQ(1U, scheduler.OutstandingCleanupCount());
	EXPECT_FALSE(scheduler.Poll(subscription, 0));
	EXPECT_EQ(GhReadSubscribeStatus::Closed,
		scheduler.Subscribe(Key(L"pulls"), GhReadPollCadence::Manual, true, 0).Status());
	EXPECT_EQ(GhReadMutationStatus::Applied,
		scheduler.Complete(dispatch->Ticket(), Response(GhRepositoryResponseStatus::Cancelled), 1, 100));
	EXPECT_EQ(0U, scheduler.OutstandingCleanupCount());
}
