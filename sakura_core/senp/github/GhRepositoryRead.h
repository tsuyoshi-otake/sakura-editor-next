/*! @file
 * @brief Typed GitHub repository HTTP-envelope read boundary.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/github/GhConnectionLifecycle.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace senp::github {

enum class GhRepositoryResponseStatus : std::uint8_t {
	Succeeded,
	NotModified,
	RateLimited,
	Forbidden,
	NotFound,
	Unauthorized,
	InvalidRequest,
	ToolUnavailable,
	UnsupportedVersion,
	Failed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
	InvalidEnvelope,
	InvalidJson,
};

class GhRepositoryResponse final {
public:
	GhRepositoryResponse(GhRepositoryResponseStatus status, int httpStatus,
		std::vector<std::uint8_t> body, std::optional<std::string> etag,
		std::uint32_t currentPage, std::optional<std::uint32_t> nextPage,
		std::optional<std::uint32_t> retryAfterSeconds = std::nullopt,
		std::optional<std::uint64_t> rateLimitResetUnixSeconds
			= std::nullopt);
	[[nodiscard]] GhRepositoryResponseStatus Status() const noexcept { return m_status; }
	[[nodiscard]] int HttpStatus() const noexcept { return m_httpStatus; }
	[[nodiscard]] const std::vector<std::uint8_t>& Body() const noexcept { return m_body; }
	[[nodiscard]] const std::optional<std::string>& ETag() const noexcept { return m_etag; }
	[[nodiscard]] std::uint32_t CurrentPage() const noexcept { return m_currentPage; }
	[[nodiscard]] const std::optional<std::uint32_t>& NextPage() const noexcept { return m_nextPage; }
	[[nodiscard]] bool HasNextPage() const noexcept { return m_nextPage.has_value(); }
	[[nodiscard]] const std::optional<std::uint32_t>& RetryAfterSeconds() const noexcept { return m_retryAfterSeconds; }
	[[nodiscard]] const std::optional<std::uint64_t>& RateLimitResetUnixSeconds() const noexcept { return m_rateLimitResetUnixSeconds; }
private:
	GhRepositoryResponseStatus m_status{ GhRepositoryResponseStatus::InvalidRequest };
	int m_httpStatus{};
	std::vector<std::uint8_t> m_body;
	std::optional<std::string> m_etag;
	std::uint32_t m_currentPage{ 1 };
	std::optional<std::uint32_t> m_nextPage;
	std::optional<std::uint32_t> m_retryAfterSeconds;
	std::optional<std::uint64_t> m_rateLimitResetUnixSeconds;
};

//! Executes a policy-built request through a verified account lease, then
//! validates one HTTP envelope and one JSON page. It never follows Link URLs.
class CGhRepositoryReader final {
public:
	explicit CGhRepositoryReader(const CGhToolPolicy& policy) noexcept : m_policy(policy) {}
	[[nodiscard]] GhRepositoryResponse Read(GhAuthenticatedAccount& account,
		const GhToolProbe& probe, const GhRepositoryReadRequest& request, HANDLE stop) const;
private:
	const CGhToolPolicy& m_policy;
};

} // namespace senp::github
