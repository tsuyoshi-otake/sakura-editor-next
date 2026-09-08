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

//! Result of one publish. approved counts the owners that hold at least one
//! tool capability; it is not the extension count of the snapshot.
struct SenpPackageAuthorityPublishResult final {
	ESenpPackageAuthorityPublishStatus status = ESenpPackageAuthorityPublishStatus::Unverified;
	std::uint64_t revision = 0;
	std::size_t approved = 0;
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ESenpPackageAuthorityPublishStatus::Published;
	}
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
	struct Owner final {
		std::wstring packageDigest;
		SenpToolCapability capabilities = SenpToolCapability::None;
	};
	struct Profile final {
		std::uint64_t revision = 0;
		std::map<std::wstring, Owner, std::less<>> owners;
	};

	mutable std::mutex m_mutex;
	std::map<std::wstring, Profile, std::less<>> m_profiles;
	bool m_closed = false;
};

} // namespace senp
