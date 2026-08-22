/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameCoordinatorModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::rendering {

//! Stable host identity supplied by the presentation owner.
//!
//! This is deliberately a logical id rather than an HWND or another native
//! object identity.  A ViewContainer can therefore move between hosts without
//! allowing a late completion to address a recycled native window.
using FrameSurfaceHostId = std::string;

enum class EFrameSurfaceAdapterPhase : std::uint8_t {
	Closed,
	Idle,
	Requested,
	Committed,
};

enum class EFrameSurfaceAdapterStatus : std::uint8_t {
	Succeeded,
	Replaced,
	Stale,
	Invalid,
	UnknownSurface,
	Busy,
	Closed,
	Exhausted,
};

//! The complete identity of one request issued for one logical surface.
//!
//! Every field participates in completion fencing.  Content generation is the
//! semantic document/version identity; the other epochs fence a surface
//! lifetime, layout/device domain, and host projection respectively.
struct FrameSurfaceAdapterRequest final {
	FrameSurfaceId surfaceId = 0;
	FrameSurfaceHostId hostId;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint64_t contentGeneration = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	EFrameWorkClass workClass = EFrameWorkClass::Background;
	bool visible = false;
	// These fields are appended to preserve existing designated aggregate
	// initializers.  A newly opened projection starts at epoch one.
	std::uint64_t hostEpoch = 1;
	std::uint64_t visibilityEpoch = 1;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! Immutable identity carried from request submission to completion.
struct FrameSurfaceAdapterTicket final {
	FrameSurfaceId surfaceId = 0;
	FrameSurfaceHostId hostId;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t requestId = 0;
	std::uint64_t contentGeneration = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	EFrameWorkClass workClass = EFrameWorkClass::Background;
	bool visible = false;
	std::uint64_t hostEpoch = 1;
	std::uint64_t visibilityEpoch = 1;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==( const FrameSurfaceAdapterTicket& rhs ) const noexcept = default;
};

struct FrameSurfaceAdapterResult final {
	EFrameSurfaceAdapterStatus status = EFrameSurfaceAdapterStatus::Invalid;
	EFrameSurfaceAdapterPhase phase = EFrameSurfaceAdapterPhase::Closed;
	std::optional<FrameSurfaceAdapterTicket> ticket;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameSurfaceAdapterStatus::Succeeded
			|| status == EFrameSurfaceAdapterStatus::Replaced;
	}
};

struct FrameSurfaceAdapterSnapshot final {
	FrameSurfaceId surfaceId = 0;
	FrameSurfaceHostId hostId;
	EFrameSurfaceAdapterPhase phase = EFrameSurfaceAdapterPhase::Closed;
	std::uint64_t surfaceLifetimeEpoch = 0;
	std::uint64_t layoutEpoch = 0;
	std::uint64_t deviceEpoch = 0;
	std::uint64_t contentGeneration = 0;
	std::uint64_t newestRequestId = 0;
	std::uint64_t committedRequestId = 0;
	bool visible = false;
	bool hasLastGoodContent = false;
	std::uint64_t hostEpoch = 1;
	std::uint64_t visibilityEpoch = 1;
};

//! Pure, one-surface boundary between a presentation owner and frame work.
//!
//! The adapter owns no HWND, worker, lock, callback, or wait.  It records the
//! logical host and visibility projection and rejects any request/completion
//! that does not match the current lifetime, content, layout, and device
//! epochs.  Close is terminal for the current lifetime; reopening requires a
//! strictly newer lifetime epoch.
class FrameSurfaceAdapter final {
public:
	explicit FrameSurfaceAdapter( FrameSurfaceId stableSurfaceId ) noexcept;
	~FrameSurfaceAdapter() = default;
	FrameSurfaceAdapter( const FrameSurfaceAdapter& ) = delete;
	FrameSurfaceAdapter& operator=( const FrameSurfaceAdapter& ) = delete;
	FrameSurfaceAdapter( FrameSurfaceAdapter&& ) = delete;
	FrameSurfaceAdapter& operator=( FrameSurfaceAdapter&& ) = delete;

	//! Opens the logical surface for one host/lifetime projection.
	[[nodiscard]] FrameSurfaceAdapterResult Open(
		std::string_view hostId, bool visible,
		std::uint64_t surfaceLifetimeEpoch, std::uint64_t layoutEpoch,
		std::uint64_t deviceEpoch, std::uint64_t contentGeneration );

	//! Closes the current lifetime and invalidates every request from it.
	[[nodiscard]] FrameSurfaceAdapterResult Close(
		std::uint64_t surfaceLifetimeEpoch ) noexcept;

	//! A host move invalidates work captured for the previous host projection.
	[[nodiscard]] FrameSurfaceAdapterResult SetHost( std::string_view hostId );

	//! A visibility transition invalidates work captured for the old projection.
	[[nodiscard]] FrameSurfaceAdapterResult SetVisible( bool visible ) noexcept;

	//! Advances one or more monotonic content/layout/device epochs.
	//!
	//! A change keeps last-good content available but withdraws the old request;
	//! the owner must issue a new request using the returned snapshot values.
	[[nodiscard]] FrameSurfaceAdapterResult UpdateEpochs(
		std::uint64_t contentGeneration, std::uint64_t layoutEpoch,
		std::uint64_t deviceEpoch ) noexcept;

	//! Accepts a request only when it describes the current projection exactly.
	[[nodiscard]] FrameSurfaceAdapterResult Request(
		const FrameSurfaceAdapterRequest& request );

	//! Commits only the newest request for the current projection.
	[[nodiscard]] FrameSurfaceAdapterResult Commit(
		const FrameSurfaceAdapterTicket& ticket ) noexcept;

	//! Allows an owner to fence payload application before calling Commit.
	[[nodiscard]] bool IsCurrent(
		const FrameSurfaceAdapterTicket& ticket ) const noexcept;

	[[nodiscard]] FrameSurfaceAdapterSnapshot Snapshot() const;
	[[nodiscard]] FrameSurfaceId StableSurfaceId() const noexcept { return m_surfaceId; }
	[[nodiscard]] bool IsOpen() const noexcept
	{
		return m_phase != EFrameSurfaceAdapterPhase::Closed;
	}

private:
	[[nodiscard]] static FrameSurfaceAdapterTicket MakeTicket(
		const FrameSurfaceAdapterRequest& request );
	[[nodiscard]] bool MatchesCurrentProjection(
		const FrameSurfaceAdapterRequest& request ) const noexcept;
	[[nodiscard]] bool MatchesCurrentProjection(
		const FrameSurfaceAdapterTicket& ticket ) const noexcept;
	void WithdrawRequest() noexcept;

	FrameSurfaceId m_surfaceId = 0;
	FrameSurfaceHostId m_hostId;
	EFrameSurfaceAdapterPhase m_phase = EFrameSurfaceAdapterPhase::Closed;
	std::uint64_t m_surfaceLifetimeEpoch = 0;
	std::uint64_t m_lastLifetimeEpoch = 0;
	std::uint64_t m_layoutEpoch = 0;
	std::uint64_t m_deviceEpoch = 0;
	std::uint64_t m_contentGeneration = 0;
	std::uint64_t m_newestRequestId = 0;
	std::uint64_t m_committedRequestId = 0;
	std::uint64_t m_hostEpoch = 1;
	std::uint64_t m_visibilityEpoch = 1;
	std::optional<FrameSurfaceAdapterTicket> m_latestRequest;
	bool m_visible = false;
	bool m_hasLastGoodContent = false;
};

} // namespace workbench::rendering
