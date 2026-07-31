/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/request/win32/WinHttpRequestRuntime.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace platform::request;
using namespace platform::request::win32;

namespace {

class Cancelled final : public IRequestCancellation {
public:
	bool IsCancellationRequested() const noexcept override { return true; }
};

TransportRequest ValidRequest()
{
	TransportRequest request;
	request.url = L"https://example.invalid/request";
	request.proxy.mode = EProxyMode::Direct;
	request.limits.timeout = std::chrono::seconds(1);
	return request;
}

TEST(WinHttpRequestTransportTest, RejectsManualProxyUserInfoWithoutOpeningNetwork)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.proxy.mode = EProxyMode::Manual;
	request.proxy.proxyUrl = L"http://username:secret@proxy.example:8080";

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::UnsupportedProxyPolicy, result.failure);
}

TEST(WinHttpRequestTransportTest, RejectsManualProxyWithPathWithoutOpeningNetwork)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.proxy.mode = EProxyMode::Manual;
	request.proxy.proxyUrl = L"http://proxy.example:8080/not-a-proxy-authority";

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::UnsupportedProxyPolicy, result.failure);
}

TEST(WinHttpRequestTransportTest, RejectsTlsToProxySchemeUntilTheTransportSupportsIt)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.proxy.mode = EProxyMode::Manual;
	request.proxy.proxyUrl = L"https://proxy.example:8443";

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::UnsupportedProxyPolicy, result.failure);
}

TEST(WinHttpRequestTransportTest, ReturnsTimeoutForAnExpiredMonotonicDeadline)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::Timeout, result.failure);
}

TEST(WinHttpRequestTransportTest, RejectsInvalidResponseLimitWithoutOpeningNetwork)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.limits.maxResponseBodyBytes = 0;

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::Protocol, result.failure);
}

TEST(WinHttpRequestTransportTest, PreservesUnsupportedProxyPolicyAsTypedFailure)
{
	WinHttpRequestTransport transport;
	auto request = ValidRequest();
	request.proxy.outcome = EProxySelectionOutcome::UnsupportedPolicy;

	const auto result = transport.Send(request, nullptr);

	EXPECT_FALSE(result.response);
	EXPECT_EQ(ETransportFailure::UnsupportedProxyPolicy, result.failure);
}

TEST(WinHttpRequestTransportTest, ChecksCancellationBeforeAnyTransportWork)
{
	WinHttpRequestTransport transport;
	Cancelled cancellation;

	const auto result = transport.Send(ValidRequest(), &cancellation);

	EXPECT_FALSE(result.response);
	// IRequestTransport has no cancellation failure variant. RequestService checks this
	// token immediately after Send and publishes its distinct Cancelled terminal result.
	EXPECT_EQ(ETransportFailure::Network, result.failure);
}

TEST(Win32RequestSchedulerTest, ReturnsFalseForAnAlreadyCancelledWait)
{
	Win32RequestScheduler scheduler(std::chrono::milliseconds(1));
	Cancelled cancellation;

	EXPECT_FALSE(scheduler.WaitFor(std::chrono::seconds(1), &cancellation));
}

TEST(Win32RequestSchedulerTest, CompletesShortWaitWithoutCancellation)
{
	Win32RequestScheduler scheduler(std::chrono::milliseconds(1));

	EXPECT_TRUE(scheduler.WaitFor(std::chrono::milliseconds(1), nullptr));
}

TEST(ThreadSafeRetryJitterSourceTest, ReturnsBoundedValuesAcrossConcurrentCalls)
{
	ThreadSafeRetryJitterSource jitter(1234);
	std::atomic<bool> valid{ true };
	std::vector<std::thread> workers;
	for (int index = 0; index < 4; ++index) {
		workers.emplace_back([&jitter, &valid] {
			for (int sample = 0; sample < 100; ++sample) {
				const auto value = jitter.NextUnitInterval();
				if (value < 0.0 || value > 1.0) valid.store(false, std::memory_order_relaxed);
			}
		});
	}
	for (auto& worker : workers) worker.join();

	EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}

TEST(Win32RequestClockTest, ProvidesBothContractClockDomains)
{
	Win32RequestClock clock;
	const auto beforeSteady = std::chrono::steady_clock::now();
	const auto steady = clock.SteadyNow();
	const auto afterSteady = std::chrono::steady_clock::now();

	EXPECT_LE(beforeSteady, steady);
	EXPECT_LE(steady, afterSteady);
	EXPECT_NE(std::chrono::system_clock::time_point{}, clock.Now());
}

} // namespace
