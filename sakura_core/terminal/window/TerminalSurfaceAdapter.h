/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameSurfaceCommitState.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace terminal {

//! Logical host used by a terminal pane until the workbench supplies a more
//! specific Part/View host identity.
inline constexpr std::string_view kTerminalDefaultFrameHostId =
	"workbench.parts.panel";

//! Stable identity for the default terminal pane. Production multiplexer
//! panes may supply a tab-derived id to CTerminalWnd's constructor; the id is
//! intentionally independent from an HWND so recreating native children does
//! not let late frame work address a recycled window.
inline constexpr workbench::rendering::FrameSurfaceId kTerminalSurfaceId =
	0x5445524D494E0001ULL; // "TERMIN01"

//! Derives a pane-stable identity from the manager-owned tab id. The tab id is
//! semantic state, so destroying/recreating the native child does not recycle
//! the logical surface identity. The zero id is reserved for the unbound
//! viewport and intentionally uses the default terminal surface id.
[[nodiscard]] constexpr workbench::rendering::FrameSurfaceId
TerminalSurfaceIdForTab(const std::uint64_t tabId) noexcept
{
	if( tabId == 0 ) return kTerminalSurfaceId;
	const auto surfaceId = kTerminalSurfaceId ^ tabId;
	return surfaceId == 0 ? 1 : surfaceId;
}

//! Terminal-facing frame publication boundary.
//!
//! PTY bytes and the TerminalModel remain owned by the terminal/session layer.
//! This adapter records only the logical surface projection and its epochs.
//! Content notifications are latest-only: a newer drain replaces a pending
//! publication, while the previous last-good GDI frame remains observable
//! until the owner reaches CommitGdiFrame() after its real GDI boundary.
//! No method waits, touches an HWND, or creates a timer.
class TerminalSurfaceAdapter final {
public:
	using Result = workbench::rendering::FrameSurfaceAdapterResult;
	using SnapshotType = workbench::rendering::FrameSurfaceAdapterSnapshot;
	using Status = workbench::rendering::EFrameSurfaceAdapterStatus;
	using SurfaceId = workbench::rendering::FrameSurfaceId;

	explicit TerminalSurfaceAdapter(
		const SurfaceId stableSurfaceId = kTerminalSurfaceId) noexcept
		: m_surface(stableSurfaceId)
	{
	}

	TerminalSurfaceAdapter(const TerminalSurfaceAdapter&) = delete;
	TerminalSurfaceAdapter& operator=(const TerminalSurfaceAdapter&) = delete;

	//! Opens one logical lifetime. A zero lifetime selects the next epoch.
	[[nodiscard]] Result Open(
		const std::string_view hostId = kTerminalDefaultFrameHostId,
		const bool visible = true,
		const std::uint64_t surfaceLifetimeEpoch = 0,
		const std::uint64_t layoutEpoch = 1,
		const std::uint64_t deviceEpoch = 1,
		const std::uint64_t contentGeneration = 1) noexcept
	{
		if (m_surface.IsOpen()) return Busy();
		std::uint64_t lifetime = surfaceLifetimeEpoch;
		if (lifetime == 0) {
			if (m_lastLifetimeEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
				return Exhausted();
			}
			lifetime = m_lastLifetimeEpoch + 1;
		}
		const auto result = m_surface.Open(hostId, visible, lifetime, layoutEpoch,
			deviceEpoch, contentGeneration);
		if (result.Accepted()) m_lastLifetimeEpoch = lifetime;
		return result;
	}

	//! Closes the current lifetime and withdraws all pending publication work.
	[[nodiscard]] Result Close() noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		return m_surface.Close();
	}

	//! Changes the logical host projection and requests the replacement frame.
	[[nodiscard]] Result SetHost(const std::string_view hostId) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		const auto before = m_surface.Snapshot();
		const auto result = m_surface.SetHost(hostId);
		if (!result.Accepted() || before.hostId == hostId) return result;
		return m_surface.RequestCurrent();
	}

	//! Changes visibility without destroying semantic/session state.
	[[nodiscard]] Result SetVisible(const bool visible) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		const auto before = m_surface.Snapshot();
		const auto result = m_surface.SetVisible(visible);
		if (!result.Accepted() || before.visible == visible) return result;
		return m_surface.RequestCurrent();
	}

	//! Publishes the newest model generation after one bounded model drain.
	[[nodiscard]] Result NotifyContent() noexcept
	{
		return m_surface.NotifyContent();
	}

	//! Fences geometry, DPI, theme, and child placement changes.
	[[nodiscard]] Result NotifyLayout() noexcept
	{
		return m_surface.NotifyLayout();
	}

	[[nodiscard]] Result NotifyLayoutEpoch(const std::uint64_t epoch) noexcept
	{
		return m_surface.NotifyLayoutEpoch(epoch);
	}

	//! Fences work from a lost/recreated device domain.
	[[nodiscard]] Result NotifyDeviceEpoch(const std::uint64_t epoch) noexcept
	{
		return m_surface.NotifyDeviceEpoch(epoch);
	}

	//! Requests a frame without changing content/layout/device epochs.
	[[nodiscard]] Result RequestCurrent() noexcept
	{
		return m_surface.RequestCurrent();
	}

	//! Commits only the latest request at the actual post-paint GDI boundary.
	//! This does not flush GDI; the enclosing owner performs that operation.
	[[nodiscard]] std::optional<SnapshotType> CommitGdiFrame() noexcept
	{
		return m_surface.CommitGdiFrame();
	}

	[[nodiscard]] SnapshotType Snapshot() const noexcept
	{
		return m_surface.Snapshot();
	}
	[[nodiscard]] SnapshotType FrameSurfaceSnapshot() const noexcept
	{
		return m_surface.Snapshot();
	}
	[[nodiscard]] SurfaceId StableSurfaceId() const noexcept
	{
		return m_surface.SurfaceId();
	}
	[[nodiscard]] bool IsOpen() const noexcept { return m_surface.IsOpen(); }
	[[nodiscard]] bool HasPendingFrame() const noexcept
	{
		return m_surface.HasPendingFrame();
	}

private:
	[[nodiscard]] Result Closed() const noexcept
	{
		return {Status::Closed, m_surface.Snapshot().phase, std::nullopt};
	}
	[[nodiscard]] Result Busy() const noexcept
	{
		return {Status::Busy, m_surface.Snapshot().phase, std::nullopt};
	}
	[[nodiscard]] Result Exhausted() const noexcept
	{
		return {Status::Exhausted, m_surface.Snapshot().phase, std::nullopt};
	}

	workbench::rendering::FrameSurfaceCommitState m_surface;
	std::uint64_t m_lastLifetimeEpoch = 0;
};

//! Name used by the frame migration notes; keep it as an alias so callers do
//! not create a second terminal surface state machine.
using TerminalFrameSurfaceAdapter = TerminalSurfaceAdapter;

} // namespace terminal
