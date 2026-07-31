/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/request/RequestService.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

//! Win32 固有の request 実行時 adapter。
namespace platform::request::win32 {

//! WinHTTP を使う同期 transport。redirect と credential lookup は RequestService が所有する。
class WinHttpRequestTransport final : public IRequestTransport {
public:
	TransportResult Send(const TransportRequest& request, const IRequestCancellation* cancellation) override;
};

//! system/steady clock を request contract に接続する。
class Win32RequestClock final : public IRequestClock {
public:
	std::chrono::system_clock::time_point Now() const override;
	std::chrono::steady_clock::time_point SteadyNow() const override;
};

//! cancellation を短い poll 間隔で観測する、bounded retry scheduler。
class Win32RequestScheduler final : public IRequestScheduler {
public:
	explicit Win32RequestScheduler(std::chrono::milliseconds maximumPollInterval = std::chrono::milliseconds(50));

	bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation* cancellation) override;

private:
	std::chrono::milliseconds m_maximumPollInterval;
};

//! mutex で generator を保護する retry jitter source。seed constructor は決定的な unit test 用。
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
