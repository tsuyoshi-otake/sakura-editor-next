/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "senp/SenpControlPackageAuthority.h"
#include "platform/profiles/UserDataProfileIdentity.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace senp {
namespace {

using Status = ESenpPackageAuthorityPublishStatus;

//! Only a package that the runtime can actually activate may own a tool grant.
//! These are the same terms CSenpExtensionActivation requires, so a package can
//! never hold a capability it could not exercise.
constexpr std::uint32_t kRuntimeSchemaVersion = 2;

//! Closed manifest-name to capability map. A name outside it grants nothing:
//! an unknown capability string must not widen authority by being present.
constexpr std::array<std::pair<std::wstring_view, SenpToolCapability>, 1> kCapabilityNames{ {
	{ L"tools.github.repository.read", SenpToolCapability::GitHubRepositoryRead },
} };

bool IsExtensionId(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 128 || value.front() == L'.' || value.back() == L'.') return false;
	return std::ranges::all_of(value, [](const wchar_t character) {
		return (character >= L'a' && character <= L'z') || (character >= L'0' && character <= L'9')
			|| character == L'-' || character == L'.';
	});
}

//! The activation path publishes archiveSha256 verbatim as the owner's package
//! digest, so an uppercase or short value is refused rather than normalized:
//! normalizing here would approve a digest no request can ever carry.
bool IsDigest(const std::wstring_view value) noexcept
{
	return value.size() == 64 && std::ranges::all_of(value, [](const wchar_t character) {
		return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
	});
}

SenpToolCapability Capabilities(const ExtensionDescriptor& extension) noexcept
{
	auto granted = SenpToolCapability::None;
	for (const auto& declared : extension.runtime.capabilities) {
		for (const auto& [name, capability] : kCapabilityNames) {
			if (declared == name) granted = granted | capability;
		}
	}
	return granted;
}

bool IsActivatable(const ExtensionDescriptor& extension) noexcept
{
	return extension.installed && extension.enabled
		&& extension.runtime.schemaVersion == kRuntimeSchemaVersion
		&& extension.runtime.compatible;
}

//! A snapshot proves the current enablement state only in Ready. After a failed
//! reload the service keeps the previously discovered extensions and reports
//! ReadyWithDiagnostics, which is exactly the state that must not mint grants.
bool ProvesEnablement(const ManagementSnapshot& snapshot) noexcept
{
	return snapshot.state == EManagementState::Ready && snapshot.revision != 0;
}

} // namespace

SenpPackageAuthorityPublishResult CSenpControlPackageAuthority::Publish(
	const std::wstring_view profileId, const ManagementSnapshot& snapshot)
{
	if (!platform::profiles::IsOpaqueUserDataProfileId(profileId)) return { Status::InvalidRequest };
	std::lock_guard lock(*m_mutex);
	if (m_closed) return { Status::Closed };
	const auto published = m_profiles.find(profileId);
	if (published != m_profiles.end() && snapshot.revision < published->second.Revision()) {
		return { Status::Stale, published->second.Revision(), published->second.OwnerCount() };
	}
	if (!ProvesEnablement(snapshot)) {
		if (published != m_profiles.end()) m_profiles.erase(published);
		return { Status::Unverified };
	}

	Profile next(snapshot.revision);
	for (const auto& extension : snapshot.extensions) {
		if (!IsActivatable(extension)) continue;
		const auto capabilities = Capabilities(extension);
		if (capabilities == SenpToolCapability::None) continue;
		if (!IsExtensionId(extension.id) || !IsDigest(extension.archiveSha256)) continue;
		if (next.OwnerCount() >= MaximumOwners()) {
			if (published != m_profiles.end()) m_profiles.erase(published);
			return { Status::ResourceExhausted };
		}
		// Two records for one identity make the permission ambiguous. Resolving
		// the ambiguity by insertion order would let a second package inherit the
		// first one's authority, so the whole publish is refused instead.
		if (!next.AddOwner(extension.id, Owner(extension.archiveSha256, capabilities))) {
			if (published != m_profiles.end()) m_profiles.erase(published);
			return { Status::Unverified };
		}
	}
	if (published == m_profiles.end() && m_profiles.size() >= MaximumProfiles()) {
		return { Status::ResourceExhausted };
	}
	const auto approved = next.OwnerCount();
	if (published != m_profiles.end()) published->second = std::move(next);
	else m_profiles.emplace(std::wstring(profileId), std::move(next));
	return { Status::Published, snapshot.revision, approved };
}

void CSenpControlPackageAuthority::Withdraw(const std::wstring_view profileId) noexcept
{
	try {
		std::lock_guard lock(*m_mutex);
		const auto found = m_profiles.find(profileId);
		if (found != m_profiles.end()) m_profiles.erase(found);
	} catch (const std::system_error&) {
		// Withdrawing is a revocation path and may not expose a failure.
	}
}

void CSenpControlPackageAuthority::Close() noexcept
{
	try {
		std::lock_guard lock(*m_mutex);
		m_closed = true;
		m_profiles.clear();
	} catch (const std::system_error&) {
		// Closing is terminal; a locking failure may not be reported here.
	}
}

std::optional<SenpApprovedToolOwner> CSenpControlPackageAuthority::Resolve(
	const std::wstring_view profileId, const std::wstring_view extensionId) const
{
	std::lock_guard lock(*m_mutex);
	if (m_closed) return std::nullopt;
	const auto profile = m_profiles.find(profileId);
	if (profile == m_profiles.end()) return std::nullopt;
	const auto* owner = profile->second.FindOwner(extensionId);
	if (owner == nullptr) return std::nullopt;
	return SenpApprovedToolOwner{ std::wstring(profileId), std::wstring(extensionId),
		owner->PackageDigest(), profile->second.Revision(), owner->Capabilities(), true };
}

std::optional<std::uint64_t> CSenpControlPackageAuthority::Revision(const std::wstring_view profileId) const
{
	std::lock_guard lock(*m_mutex);
	const auto profile = m_profiles.find(profileId);
	if (profile == m_profiles.end()) return std::nullopt;
	return profile->second.Revision();
}

std::size_t CSenpControlPackageAuthority::Size() const noexcept
{
	try {
		std::lock_guard lock(*m_mutex);
		return m_profiles.size();
	} catch (const std::system_error&) {
		return 0;
	}
}

} // namespace senp
