/*! @file
 * @brief Bounded GitHub Actions job-log transfer into a SENP text resource.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpTextResource.h"
#include "senp/github/GhConnectionLifecycle.h"

#include <cstdint>
#include <string>

namespace senp::github {

enum class GhLogResourceStatus : std::uint8_t {
	Succeeded,
	UnavailableOrNotFound,
	InvalidRequest,
	ToolUnavailable,
	UnsupportedVersion,
	Failed,
	TimedOut,
	Cancelled,
	LimitExceeded,
	ResourceUnavailable,
};

class GhLogResourceResult final {
public:
	GhLogResourceResult(GhLogResourceStatus status, std::wstring handle,
		std::size_t acceptedBytes) noexcept;
	[[nodiscard]] GhLogResourceStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const std::wstring& Handle() const noexcept { return m_handle; }
	[[nodiscard]] std::size_t AcceptedBytes() const noexcept { return m_acceptedBytes; }
private:
	GhLogResourceStatus m_status{ GhLogResourceStatus::InvalidRequest };
	std::wstring m_handle;
	std::size_t m_acceptedBytes{};
};

//! Runs one fixed `gh api` job-log request through an adopted credential lease.
//! GitHub CLI follows the short-lived redirect internally and does not expose its
//! URL; only final stdout bytes enter the resource. Every created resource is
//! terminal when Download returns unless its owning store was revoked or closed.
class CGhLogResource final {
public:
	CGhLogResource(const CGhToolPolicy& policy, SenpTextResourceStore& store) noexcept :
		m_policy(policy), m_store(store) {}
	[[nodiscard]] GhLogResourceResult Download(GhAuthenticatedAccount& account,
		const GhToolProbe& probe, const TextResourceScope& scope,
		const GhJobLogRequest& request, HANDLE stop) const;
private:
	const CGhToolPolicy& m_policy;
	SenpTextResourceStore& m_store;
};

} // namespace senp::github
