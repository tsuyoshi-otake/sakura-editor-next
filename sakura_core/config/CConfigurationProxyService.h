/*! @file
 * @brief Deterministic configuration-backed proxy selection.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/CConfigurationNetworkPolicy.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace config {

//! The system resolver is deliberately a narrow boundary: PAC, WinHTTP, WPAD,
//! and environment discovery belong behind this contract, not in Settings.
//! Each value is a terminal result; adapters must not return a partial choice.
enum class ESystemProxyResolutionOutcome : std::uint8_t {
	Selected,
	//! The system could not be asked at all -- unreadable configuration, a
	//! WinHTTP query failure, or another inability to produce an answer. This
	//! is strictly "unable to resolve"; it must never stand in for "the
	//! system authoritatively says no proxy applies", which is
	//! NoProxyRequired below.
	Unavailable,
	Cancelled,
	DeadlineExceeded,
	InvalidResult,
	//! The system answered authoritatively that this target needs no proxy.
	//! This is a selection ("connect directly"), not a failure to resolve.
	NoProxyRequired,
};

struct SystemProxyResolution final {
	ESystemProxyResolutionOutcome outcome = ESystemProxyResolutionOutcome::Unavailable;
	platform::request::ProxySelection selection;
};

class ISystemProxyResolver {
public:
	virtual ~ISystemProxyResolver() = default;

	//! The resolver may block, but must use only the supplied shared deadline and
	//! cancellation token. It must return exactly one ESystemProxyResolutionOutcome.
	virtual SystemProxyResolution Resolve(
		const platform::request::ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const platform::request::IRequestCancellation* cancellation
	) = 0;
};

//! Profile-scoped proxy policy adapter. It reads one coherent settings snapshot
//! per selection; it never reads PAC data, credential material, or raw settings.
class CConfigurationProxyService final : public platform::request::IProxyService {
public:
	CConfigurationProxyService(
		const CConfigurationNetworkPolicy& networkPolicy,
		ISystemProxyResolver& systemResolver
	);

	//! Retains a validated policy snapshot for a detached/shared request context.
	//! Callers obtain this value from CConfigurationNetworkPolicy::Snapshot() before
	//! releasing the configuration service and policy lifetime.
	CConfigurationProxyService(
		ConfigurationNetworkPolicySnapshot networkPolicySnapshot,
		ISystemProxyResolver& systemResolver
	);

	platform::request::ProxySelection SelectProxy(
		const platform::request::ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const platform::request::IRequestCancellation* cancellation
	) override;

private:
	const CConfigurationNetworkPolicy* m_networkPolicy = nullptr;
	const std::optional<ConfigurationNetworkPolicySnapshot> m_networkPolicySnapshot;
	ISystemProxyResolver& m_systemResolver;
};

} // namespace config
