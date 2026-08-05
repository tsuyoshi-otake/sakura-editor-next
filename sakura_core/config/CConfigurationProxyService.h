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

// Compatibility aliases keep configuration consumers source-stable while the
// actual resolver contract belongs to the lower-level request component.
using ESystemProxyResolutionOutcome = platform::request::ESystemProxyResolutionOutcome;
using SystemProxyResolution = platform::request::SystemProxyResolution;
using ISystemProxyResolver = platform::request::ISystemProxyResolver;

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
