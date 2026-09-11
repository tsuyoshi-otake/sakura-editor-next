/*! @file
	@brief Control-owned trusted package permission table for SENP tool grants.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "senp/SenpManagementService.h"
#include "senp/SenpToolGrants.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace senp {

//! One terminal outcome of publishing a management snapshot as permissions.
enum class ESenpPackageAuthorityPublishStatus : std::uint8_t {
	//! A Ready snapshot with a committed revision replaced the profile's table.
	Published,
	//! The snapshot does not prove the current enablement state. The profile's
	//! table is withdrawn, so every later Resolve for it refuses.
	Unverified,
	//! The snapshot is older than the one already published for this profile.
	//! The newer table is kept; nothing is withdrawn.
	Stale,
	//! profileId is not an opaque user-data profile identifier.
	InvalidRequest,
	//! More profiles or more approved owners than this authority admits. The
	//! profile's table is withdrawn rather than truncated.
	ResourceExhausted,
	Closed,
};

//! Result of one publish. Approved() counts the owners that hold at least one
//! tool capability; it is not the extension count of the snapshot.
struct SenpPackageAuthorityPublishResult final {
	constexpr SenpPackageAuthorityPublishResult(ESenpPackageAuthorityPublishStatus status,
		std::uint64_t revision = 0, std::size_t approved = 0) noexcept
		: m_status(status), m_revision(revision), m_approved(approved)
	{
	}

	[[nodiscard]] constexpr ESenpPackageAuthorityPublishStatus Status() const noexcept { return m_status; }
	[[nodiscard]] constexpr std::uint64_t Revision() const noexcept { return m_revision; }
	[[nodiscard]] constexpr std::size_t Approved() const noexcept { return m_approved; }
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return m_status == ESenpPackageAuthorityPublishStatus::Published;
	}

private:
	ESenpPackageAuthorityPublishStatus m_status;
	std::uint64_t m_revision;
	std::size_t m_approved;
};

/*!
	@brief Resolves tool grants from Control-owned installed package state only.

	Resolve() is called on every grant issue and on every grant validation, so it
	is a bounded lookup over an already-published table: it never invokes the
	package tool, touches the filesystem or waits on any external process.

	Publishing is the only route that admits permissions, and it is deliberately
	driven by whoever owns the management service in the control process. A
	snapshot that is not Ready withdraws the profile's table instead of extending
	it: ReadyWithDiagnostics retains the previously discovered extensions after a
	failed reload and therefore proves nothing about the current enablement state.
	An editor's own claim about its packages is never an input here.
*/
class CSenpControlPackageAuthority final : public ISenpToolGrantAuthority {
public:
	//! One control process serves a bounded number of user-data profiles, and
	//! one profile a bounded number of tool-capable owners.
	[[nodiscard]] static constexpr std::size_t MaximumProfiles() noexcept { return 8; }
	[[nodiscard]] static constexpr std::size_t MaximumOwners() noexcept { return 64; }

	CSenpControlPackageAuthority() = default;
	~CSenpControlPackageAuthority() override = default;
	CSenpControlPackageAuthority(const CSenpControlPackageAuthority&) = delete;
	CSenpControlPackageAuthority& operator=(const CSenpControlPackageAuthority&) = delete;

	//! Replaces the profile's permission table with the owners this snapshot
	//! proves. It performs no I/O of its own.
	[[nodiscard]] SenpPackageAuthorityPublishResult Publish(std::wstring_view profileId,
		const ManagementSnapshot& snapshot);
	//! Removes a profile's table without waiting for a snapshot, for a profile
	//! that was deselected or whose management service stopped.
	void Withdraw(std::wstring_view profileId) noexcept;
	void Close() noexcept;

	[[nodiscard]] std::optional<SenpApprovedToolOwner> Resolve(std::wstring_view profileId,
		std::wstring_view extensionId) const override;
	//! Published revision of a profile's table, for callers that revoke grants
	//! when the trusted state moves. Disengaged when nothing is published.
	[[nodiscard]] std::optional<std::uint64_t> Revision(std::wstring_view profileId) const;
	[[nodiscard]] std::size_t Size() const noexcept;

private:
	//! Holds one approved owner's identity proof. Immutable once constructed:
	//! an owner's digest/capabilities never change without a whole new Publish.
	class Owner final {
	public:
		Owner(std::wstring packageDigest, SenpToolCapability capabilities) noexcept
			: m_packageDigest(std::move(packageDigest)), m_capabilities(capabilities)
		{
		}

		[[nodiscard]] const std::wstring& PackageDigest() const noexcept { return m_packageDigest; }
		[[nodiscard]] SenpToolCapability Capabilities() const noexcept { return m_capabilities; }

	private:
		std::wstring m_packageDigest;
		SenpToolCapability m_capabilities;
	};

	//! Holds one profile's published table. AddOwner()/erase mutate it only
	//! while a Publish is still building or replacing it under m_mutex.
	class Profile final {
	public:
		explicit Profile(std::uint64_t revision) noexcept : m_revision(revision) {}

		[[nodiscard]] std::uint64_t Revision() const noexcept { return m_revision; }
		[[nodiscard]] std::size_t OwnerCount() const noexcept { return m_owners.size(); }
		[[nodiscard]] const Owner* FindOwner(std::wstring_view extensionId) const
		{
			const auto found = m_owners.find(extensionId);
			return found == m_owners.end() ? nullptr : &found->second;
		}
		//! Returns false when extensionId already names an owner in this table.
		[[nodiscard]] bool AddOwner(std::wstring_view extensionId, Owner owner)
		{
			return m_owners.emplace(std::wstring(extensionId), std::move(owner)).second;
		}

	private:
		std::uint64_t m_revision;
		std::map<std::wstring, Owner, std::less<>> m_owners;
	};

	//! Guards m_profiles/m_closed. Held through an owned indirection rather
	//! than a `mutable` member so const readers (Resolve/Revision/Size) can
	//! lock it without granting themselves mutable access to anything else.
	std::unique_ptr<std::mutex> m_mutex = std::make_unique<std::mutex>();
	std::map<std::wstring, Profile, std::less<>> m_profiles;
	bool m_closed = false;
};

} // namespace senp
