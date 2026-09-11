/*! @file
	@brief Production authorization projection for SENP text resources.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/SenpReadonlyDocumentHost.h"
#include "senp/SenpContributionOwners.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::editor {

/*!
	@brief The owners a window will still show text resources for.

	A published document names a text resource by handle alone. Everything else
	the surface needs to attribute that handle - which extension, which
	activation, which account the bytes were fetched under - lives in the owner
	the document arrived from, and this is the window's record of which owners
	are still live.

	It answers about authority only. Whether a handle names anything is the
	control-side store's business, and asking it is what a read is for, so a
	handle this object has never heard of is not refused here: refusing it would
	be this side claiming the resource does not exist.

	UI thread only, like the interface it implements. It performs no I/O and
	holds nothing the control side owns.
*/
class CSenpOwnerTextResources final : public ISenpReadonlyTextResources {
public:
	//! One window shows one workbench, and its owners are bounded by the
	//! contribution service long before this cap is reached. It exists so a
	//! runaway admission cannot grow the record without limit.
	static constexpr std::size_t MaximumOwners() noexcept { return 64; }
	//! Handles are opaque to this side, so only their length is checked. The
	//! bound matches the resource ids a document may name.
	static constexpr std::size_t MaximumHandleCharacters() noexcept { return 512; }

	explicit CSenpOwnerTextResources(std::wstring_view profileId);

	//! False when the profile id this was built from is not an opaque user-data
	//! profile id. Such an object admits nothing and resolves nothing: a scope
	//! naming no profile would be a resource cohort belonging to no one.
	[[nodiscard]] bool Usable() const noexcept { return !m_profileId.empty(); }

	/*!
		@brief Records one owner as live.

		False means the owner cannot be projected onto a resource scope at all -
		an unusable profile, an extension id or package digest outside the shape
		a scope may carry, a generation the store would reject, or the record
		already holding a different package under this exact identity. The last
		is a contradiction rather than a duplicate, and admitting it would let
		one of the two packages read the other's resources.

		Admitting an owner already recorded is not an error and changes nothing.
	*/
	[[nodiscard]] bool Admit(const senp::ContributionOwnerIdentity& owner);
	//! Drops one owner. A scope resolved before this keeps its values, but
	//! IsCurrent stops agreeing, which is what closes the surfaces built on it.
	void Retire(const senp::ContributionOwnerIdentity& owner) noexcept;
	[[nodiscard]] std::size_t OwnerCount() const noexcept { return m_owners.size(); }

	[[nodiscard]] std::optional<senp::TextResourceScope> Resolve(const SenpReadonlyScope& document,
		const senp::effect::TextResourceSection& section) const override;
	[[nodiscard]] bool IsCurrent(const senp::TextResourceScope& scope, std::wstring_view handle) const override;

private:
	class Owner final {
	public:
		Owner(SenpReadonlyScope document, std::string packageDigest) noexcept
			: m_document(std::move(document)), m_packageDigest(std::move(packageDigest)) {}

		[[nodiscard]] const SenpReadonlyScope& Document() const noexcept { return m_document; }
		[[nodiscard]] const std::string& PackageDigest() const noexcept { return m_packageDigest; }

	private:
		SenpReadonlyScope m_document;
		std::string m_packageDigest;
	};
	[[nodiscard]] const Owner* Find(const SenpReadonlyScope& document) const noexcept;
	[[nodiscard]] senp::TextResourceScope Project(const Owner& owner) const;

	std::string m_profileId;
	std::vector<Owner> m_owners;
};

} // namespace workbench::editor
