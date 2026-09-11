/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/editor/SenpOwnerTextResources.h"

#include "platform/profiles/UserDataProfileIdentity.h"

#include <algorithm>
#include <exception>

namespace workbench::editor {
namespace {

//! The extension id a resource scope may carry. Narrower than the identity the
//! owner arrives with, so an owner outside it is refused rather than trimmed.
bool ExtensionId(std::wstring_view source, std::string& result)
{
	if (source.empty() || source.size() > 128) return false;
	result.clear();
	result.reserve(source.size());
	for (const wchar_t value : source) {
		if (!((value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9')
			|| value == L'.' || value == L'-')) return false;
		result.push_back(static_cast<char>(value));
	}
	return true;
}

//! Exactly the digest form the store validates: 64 lowercase hexadecimal
//! characters. An owner carrying anything else names no package this side can
//! attribute a resource to.
bool PackageDigest(std::wstring_view source, std::string& result)
{
	if (source.size() != 64) return false;
	result.clear();
	result.reserve(source.size());
	for (const wchar_t value : source) {
		if (!((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f'))) return false;
		result.push_back(static_cast<char>(value));
	}
	return true;
}

bool ValidHandle(std::wstring_view handle) noexcept
{
	return !handle.empty() && handle.size() <= CSenpOwnerTextResources::MaximumHandleCharacters();
}

} // namespace

CSenpOwnerTextResources::CSenpOwnerTextResources(const std::wstring_view profileId)
{
	if (!platform::profiles::IsOpaqueUserDataProfileId(profileId)) return;
	// Validated above as an opaque id, whose alphabet is ASCII by construction.
	m_profileId.reserve(profileId.size());
	for (const wchar_t value : profileId) m_profileId.push_back(static_cast<char>(value));
}

const CSenpOwnerTextResources::Owner* CSenpOwnerTextResources::Find(
	const SenpReadonlyScope& document) const noexcept
{
	const auto found = std::find_if(m_owners.begin(), m_owners.end(),
		[&document](const Owner& owner) noexcept { return owner.Document() == document; });
	return found == m_owners.end() ? nullptr : &*found;
}

/*!
	@brief Projects one live owner onto the cohort its resources belong to.

	The revision is the owner generation because that is the revision the
	control side creates its resources under. The editor holds no other handle
	on it: the scope the store is actually read under is composed on the control
	side from the grant and never crosses the wire. A chunk whose revision
	disagrees with this one is therefore refused by the view rather than
	displayed, which is the failure this projection is meant to have if the two
	sides ever part.

	The grant id stays empty. A grant is minted inside the seam and is never
	surfaced here, so naming one would be this side inventing an authority it
	does not hold. Nothing on the editor side reads it: the field distinguishes
	cohorts within one store, and this object records only its own window.
*/
senp::TextResourceScope CSenpOwnerTextResources::Project(const Owner& owner) const
{
	senp::TextResourceScope scope;
	scope.profileId = m_profileId;
	scope.extensionId = owner.Document().extensionId;
	scope.packageDigest = owner.PackageDigest();
	scope.ownerGeneration = owner.Document().ownerGeneration;
	scope.workspaceRevision = owner.Document().workspaceRevision;
	scope.accountGeneration = owner.Document().accountGeneration;
	scope.revision = owner.Document().ownerGeneration;
	return scope;
}

bool CSenpOwnerTextResources::Admit(const senp::ContributionOwnerIdentity& owner)
try {
	if (!Usable()) return false;
	std::string extensionId;
	std::string packageDigest;
	if (!ExtensionId(owner.extensionId, extensionId)
		|| !PackageDigest(owner.packageDigest, packageDigest)) return false;
	// The store refuses a scope outside these bounds, so an owner outside them
	// could never own a resource in the first place.
	if (owner.generation <= 0 || owner.workspaceRevision < 0 || owner.accountGeneration < 0) return false;
	SenpReadonlyScope document;
	document.extensionId = std::move(extensionId);
	document.ownerGeneration = owner.generation;
	document.workspaceRevision = owner.workspaceRevision;
	document.accountGeneration = owner.accountGeneration;
	if (const auto* existing = Find(document)) {
		return existing->PackageDigest() == packageDigest;
	}
	if (m_owners.size() >= MaximumOwners()) return false;
	m_owners.emplace_back(std::move(document), std::move(packageDigest));
	return true;
} catch (const std::exception&) {
	return false;
}

void CSenpOwnerTextResources::Retire(const senp::ContributionOwnerIdentity& owner) noexcept
try {
	SenpReadonlyScope document;
	if (!ExtensionId(owner.extensionId, document.extensionId)) return;
	document.ownerGeneration = owner.generation;
	document.workspaceRevision = owner.workspaceRevision;
	document.accountGeneration = owner.accountGeneration;
	const auto removed = std::remove_if(m_owners.begin(), m_owners.end(),
		[&document](const Owner& value) noexcept { return value.Document() == document; });
	m_owners.erase(removed, m_owners.end());
} catch (const std::exception&) {
}

std::optional<senp::TextResourceScope> CSenpOwnerTextResources::Resolve(const SenpReadonlyScope& document,
	const senp::effect::TextResourceSection& section) const
{
	// The section's own length and status are the extension's account of a
	// resource the control side owns. They say nothing about authority, and the
	// bytes that arrive carry the store's own length, so neither is read here.
	if (!Usable() || !ValidHandle(section.handle)) return {};
	const auto* owner = Find(document);
	if (!owner) return {};
	return Project(*owner);
}

bool CSenpOwnerTextResources::IsCurrent(const senp::TextResourceScope& scope,
	const std::wstring_view handle) const
{
	// A handle this window has never seen is still current: whether it names a
	// resource is answered by reading it, and refusing here would state an
	// absence this side cannot observe.
	if (!Usable() || !ValidHandle(handle)) return false;
	return std::any_of(m_owners.begin(), m_owners.end(),
		[this, &scope](const Owner& owner) { return Project(owner) == scope; });
}

} // namespace workbench::editor
