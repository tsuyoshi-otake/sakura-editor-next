/*! @file
 * @brief Profile-scoped, typed network policy resolved from configuration.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/IConfigurationService.h"
#include <sakura/request/RequestService.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace config {

//! A policy read has a terminal result.  Unsupported means the supplied
//! configuration service does not provide the required descriptor contract;
//! InvalidConfiguration means the resolved values do not form a safe policy.
enum class EConfigurationNetworkPolicyOutcome : std::uint8_t {
	Ready,
	InvalidConfiguration,
	Unsupported,
};

//! Transport-neutral settings snapshot.  This class deliberately does not
//! resolve PAC/system proxy state and never contains proxy credentials.
struct ConfigurationNetworkPolicySnapshot final {
	platform::request::EProxySupport proxySupport = platform::request::EProxySupport::Fallback;
	std::optional<std::wstring> proxyUrl;
	std::vector<std::wstring> noProxy;
	bool proxyStrictSSL = true;
	bool systemCertificates = true;
	platform::request::RequestLimits requestLimits;
};

struct ConfigurationNetworkPolicyResult final {
	EConfigurationNetworkPolicyOutcome outcome = EConfigurationNetworkPolicyOutcome::Unsupported;
	std::optional<ConfigurationNetworkPolicySnapshot> snapshot;
	//! Deliberately path- and value-free.  It is safe to surface this in the
	//! workbench diagnostics view without exposing proxy endpoints.
	std::string diagnostic;

	explicit operator bool() const noexcept
	{
		return outcome == EConfigurationNetworkPolicyOutcome::Ready && snapshot.has_value();
	}
};

//! Read-only adapter for one immutable profile target.  Workspace/folder
//! sources are intentionally absent from the target, so repository settings
//! cannot influence network policy.
class CConfigurationNetworkPolicy final {
public:
	CConfigurationNetworkPolicy(IConfigurationService& configurationService, std::wstring profileId);

	CConfigurationNetworkPolicy(const CConfigurationNetworkPolicy&) = delete;
	CConfigurationNetworkPolicy& operator=(const CConfigurationNetworkPolicy&) = delete;

	[[nodiscard]] ConfigurationNetworkPolicyResult Snapshot() const;
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_target.profileId; }

private:
	IConfigurationService& m_configurationService;
	ConfigurationTarget m_target;
};

} // namespace config
