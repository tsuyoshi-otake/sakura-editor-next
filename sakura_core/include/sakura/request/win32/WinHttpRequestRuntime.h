/*! @file
 * @brief Win32 adapters for the request contracts.
 */
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/

#pragma once

#include <sakura/request/RequestService.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace platform::request::win32 {

class WinHttpRequestTransport final : public IRequestTransport {
public:
	TransportResult Send(const TransportRequest& request, const IRequestCancellation* cancellation) override;
};

class Win32RequestClock final : public IRequestClock {
public:
	std::chrono::system_clock::time_point Now() const override;
	std::chrono::steady_clock::time_point SteadyNow() const override;
};

class Win32RequestScheduler final : public IRequestScheduler {
public:
	explicit Win32RequestScheduler(std::chrono::milliseconds maximumPollInterval = std::chrono::milliseconds(50));

	bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation* cancellation) override;

private:
	std::chrono::milliseconds m_maximumPollInterval;
};

class ThreadSafeRetryJitterSource final : public IRetryJitterSource {
public:
	ThreadSafeRetryJitterSource();
	explicit ThreadSafeRetryJitterSource(std::uint64_t seed);

	double NextUnitInterval() override;

private:
	std::mutex m_mutex;
	std::mt19937_64 m_generator;
};

} // namespace platform::request::win32
