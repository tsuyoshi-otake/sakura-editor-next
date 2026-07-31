/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/controlipc/ControlPlatformEndpointDiscoveryReader.h"

#include "platform/controlipc/ControlIpcSecurity.h"

#include <utility>

namespace platform::controlipc {
namespace {

bool IsValidDescriptor(const std::filesystem::path& profileDirectory, std::wstring_view expectedProfileHash)
{
	if (profileDirectory.empty() || !profileDirectory.is_absolute() || expectedProfileHash.empty() ||
		BuildControlEndpointMappingName(expectedProfileHash).empty()) {
		return false;
	}
	return ComputeCanonicalProfileHash(profileDirectory) == expectedProfileHash;
}

} // namespace

CControlPlatformEndpointDiscoveryReader::CControlPlatformEndpointDiscoveryReader(
	std::filesystem::path profileDirectory, std::wstring expectedProfileHash)
	: m_profileDirectory(std::move(profileDirectory))
	, m_expectedProfileHash(std::move(expectedProfileHash))
	, m_descriptorValid(IsValidDescriptor(m_profileDirectory, m_expectedProfileHash))
{
}

CControlPlatformEndpointDiscoveryReader::~CControlPlatformEndpointDiscoveryReader()
{
	Close();
}

std::optional<ControlPlatformEndpointSnapshot> CControlPlatformEndpointDiscoveryReader::Read(
	const ControlPlatformEndpointReadRequirements& requirements)
{
	auto result = ReadDetailed(requirements);
	return std::move(result.snapshot);
}

ControlPlatformEndpointDiscoveryResult CControlPlatformEndpointDiscoveryReader::ReadDetailed(
	const ControlPlatformEndpointReadRequirements& requirements)
{
	std::lock_guard lock(m_mutex);
	if (m_closed) {
		return { EControlPlatformEndpointDiscoveryDisposition::Closed, std::nullopt, ERROR_INVALID_HANDLE,
			L"Control IPC endpoint discovery reader is closed" };
	}
	if (!m_descriptorValid) {
		return { EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor, std::nullopt, ERROR_INVALID_PARAMETER,
			L"Control IPC endpoint descriptor is invalid" };
	}

	if (!m_open) {
		auto opened = OpenLocked();
		if (opened.disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) return opened;
	}
	auto result = m_endpoint.ReadDetailed(requirements);
	if (result.disposition == EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		return result;
	}

	// The mapped object may belong to a control process that is starting,
	// stopping, stale, or already replaced.  Release it before one bounded
	// reopen so this reader never pins an unusable mapping across retries.
	ReleaseLocked();
	auto reopened = OpenLocked();
	if (reopened.disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		return reopened;
	}
	result = m_endpoint.ReadDetailed(requirements);
	if (result.disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		ReleaseLocked();
	}
	return result;
}

void CControlPlatformEndpointDiscoveryReader::Close() noexcept
{
	std::lock_guard lock(m_mutex);
	m_closed = true;
	ReleaseLocked();
}

ControlPlatformEndpointDiscoveryResult CControlPlatformEndpointDiscoveryReader::OpenLocked()
{
	// Revalidate before every kernel-object open.  The immutable absolute path
	// prevents current-directory changes from redirecting discovery.
	if (ComputeCanonicalProfileHash(m_profileDirectory) != m_expectedProfileHash) {
		m_open = false;
		return { EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor, std::nullopt, ERROR_INVALID_PARAMETER,
			L"Control IPC endpoint descriptor changed before open" };
	}
	auto result = m_endpoint.OpenForEditorDetailed(m_profileDirectory);
	if (result.disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		m_open = false;
		return result;
	}
	if (m_endpoint.ProfileHash() != m_expectedProfileHash ||
		!IsSafeControlEndpointMappingName(m_endpoint.MappingName())) {
		m_endpoint.Close();
		m_open = false;
		return { EControlPlatformEndpointDiscoveryDisposition::SecurityRejected, std::nullopt, ERROR_INVALID_DATA,
			L"Control IPC endpoint mapping identity was rejected" };
	}
	m_open = true;
	return result;
}

void CControlPlatformEndpointDiscoveryReader::ReleaseLocked() noexcept
{
	m_endpoint.Close();
	m_open = false;
}

} // namespace platform::controlipc
