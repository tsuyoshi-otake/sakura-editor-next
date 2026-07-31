/*! @file
	@brief Editor-owned discovery reader for the control platform endpoint.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CONTROLPLATFORMENDPOINTDISCOVERYREADER_26B5B543_E36A_4D55_A7C7_03D809011A42_H_
#define SAKURA_CONTROLPLATFORMENDPOINTDISCOVERYREADER_26B5B543_E36A_4D55_A7C7_03D809011A42_H_
#pragma once

#include "platform/controlipc/ControlPlatformClient.h"

#include <filesystem>
#include <mutex>
#include <string>

namespace platform::controlipc {

/*!
	@brief Lazily discovers and owns the protected control endpoint mapping.

	Process composition supplies one already-resolved profile directory and its
	canonical hash.  The descriptor is validated before any mapping is opened.
	Read performs no waiting or polling: it makes at most one reopen attempt and
	leaves retry timing to CControlPlatformClient.
*/
class CControlPlatformEndpointDiscoveryReader final : public IControlPlatformEndpointReader {
public:
	CControlPlatformEndpointDiscoveryReader(
		std::filesystem::path profileDirectory, std::wstring expectedProfileHash);
	~CControlPlatformEndpointDiscoveryReader();
	CControlPlatformEndpointDiscoveryReader(const CControlPlatformEndpointDiscoveryReader&) = delete;
	CControlPlatformEndpointDiscoveryReader& operator=(const CControlPlatformEndpointDiscoveryReader&) = delete;

	[[nodiscard]] std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements) override;
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult ReadDetailed(
		const ControlPlatformEndpointReadRequirements& requirements) override;
	//! Terminal, idempotent shutdown boundary. Reads after Close never reopen a mapping.
	void Close() noexcept override;

private:
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult OpenLocked();
	void ReleaseLocked() noexcept;

	const std::filesystem::path m_profileDirectory;
	const std::wstring m_expectedProfileHash;
	const bool m_descriptorValid;
	std::mutex m_mutex;
	CControlPlatformEndpoint m_endpoint;
	bool m_open = false;
	bool m_closed = false;
};

} // namespace platform::controlipc

#endif /* SAKURA_CONTROLPLATFORMENDPOINTDISCOVERYREADER_26B5B543_E36A_4D55_A7C7_03D809011A42_H_ */
