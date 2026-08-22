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

namespace markdown {

//! Logical host used until the workbench supplies the preview's real host id.
inline constexpr std::string_view kMarkdownPreviewDefaultHostId =
	"workbench.markdown.preview";

//! Stable id for the one Markdown viewport owned by one editor window.
//!
//! The id is deliberately semantic rather than an HWND value.  The native
//! window may be destroyed and recreated while a late worker completion still
//! exists; the lifetime epoch in the adapter fences that completion.
inline constexpr workbench::rendering::FrameSurfaceId kMarkdownPreviewSurfaceId =
	0x4D41524B444F5056ULL; // "MARKDOPV"

//! Markdown-facing, HWND-free frame publication boundary.
//!
//! Markdown owns parsing and native GDI painting.  This adapter owns only the
//! logical surface identity and the request/commit fence.  It never waits,
//! calls User32, or retains a render payload.  A caller must invoke
//! CommitGdiFrame() only after the enclosing GDI frame has actually flushed;
//! stale or failed work therefore leaves FrameSurfaceAdapter's last-good
//! projection untouched.
class MarkdownPreviewSurfaceAdapter final {
public:
	using Result = workbench::rendering::FrameSurfaceAdapterResult;
	using SnapshotType = workbench::rendering::FrameSurfaceAdapterSnapshot;
	using Status = workbench::rendering::EFrameSurfaceAdapterStatus;

	explicit MarkdownPreviewSurfaceAdapter(
		const workbench::rendering::FrameSurfaceId stableSurfaceId = kMarkdownPreviewSurfaceId) noexcept
		: m_surface(stableSurfaceId)
	{
	}

	MarkdownPreviewSurfaceAdapter(const MarkdownPreviewSurfaceAdapter&) = delete;
	MarkdownPreviewSurfaceAdapter& operator=(const MarkdownPreviewSurfaceAdapter&) = delete;

	//! Opens one logical lifetime. A zero lifetime selects the next monotonic id.
	[[nodiscard]] Result Open(
		const std::string_view hostId = kMarkdownPreviewDefaultHostId,
		const bool visible = false,
		const std::uint64_t surfaceLifetimeEpoch = 0,
		const std::uint64_t layoutEpoch = 1,
		const std::uint64_t deviceEpoch = 1,
		const std::uint64_t contentGeneration = 1) noexcept
	{
		if (m_surface.IsOpen()) {
			return {Status::Busy, m_surface.Snapshot().phase, std::nullopt};
		}
		std::uint64_t lifetime = surfaceLifetimeEpoch;
		if (lifetime == 0) {
			if (m_lastLifetimeEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
				return {Status::Exhausted, m_surface.Snapshot().phase, std::nullopt};
			}
			lifetime = m_lastLifetimeEpoch + 1;
		}
		const auto result = m_surface.Open(hostId, visible, lifetime, layoutEpoch,
			deviceEpoch, contentGeneration);
		if (result.Accepted()) m_lastLifetimeEpoch = lifetime;
		return result;
	}

	//! Closes the current lifetime and withdraws every pending request.
	[[nodiscard]] Result Close() noexcept
	{
		if (!m_surface.IsOpen()) {
			return {Status::Closed, m_surface.Snapshot().phase, std::nullopt};
		}
		return m_surface.Close();
	}

	[[nodiscard]] Result SetHost(const std::string_view hostId) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		const auto before = m_surface.Snapshot();
		const auto result = m_surface.SetHost(hostId);
		if (!result.Accepted() || before.hostId == hostId) return result;
		return m_surface.RequestCurrent();
	}

	[[nodiscard]] Result SetVisible(const bool visible) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		const auto before = m_surface.Snapshot();
		const auto result = m_surface.SetVisible(visible);
		if (!result.Accepted() || before.visible == visible) return result;
		return m_surface.RequestCurrent();
	}

	//! Advances content only after an immutable render result is accepted.
	[[nodiscard]] Result NotifyContent() noexcept
	{
		return m_surface.NotifyContent();
	}

	[[nodiscard]] Result NotifyContentGeneration(
		const std::uint64_t generation) noexcept
	{
		return m_surface.NotifyContentGeneration(generation);
	}

	[[nodiscard]] Result NotifyLayout() noexcept
	{
		return m_surface.NotifyLayout();
	}

	[[nodiscard]] Result NotifyLayoutEpoch(const std::uint64_t epoch) noexcept
	{
		return m_surface.NotifyLayoutEpoch(epoch);
	}

	//! Device recovery is explicit and fences all work from the old domain.
	[[nodiscard]] Result NotifyDeviceEpoch(const std::uint64_t epoch) noexcept
	{
		return m_surface.NotifyDeviceEpoch(epoch);
	}

	//! Requests the current immutable projection without changing an epoch.
	[[nodiscard]] Result RequestCurrent() noexcept
	{
		return m_surface.RequestCurrent();
	}

	//! Commits only the ticket captured by the latest request.
	//!
	//! This method has no implicit paint or flush. The owner calls it at the
	//! actual GDI commit boundary, after its frame has been painted and flushed.
	[[nodiscard]] std::optional<SnapshotType> CommitGdiFrame() noexcept
	{
		return m_surface.CommitGdiFrame();
	}

	[[nodiscard]] SnapshotType SnapshotState() const noexcept
	{
		return m_surface.Snapshot();
	}

	[[nodiscard]] SnapshotType Snapshot() const noexcept
	{
		return m_surface.Snapshot();
	}

	[[nodiscard]] bool IsOpen() const noexcept { return m_surface.IsOpen(); }
	[[nodiscard]] bool HasPendingFrame() const noexcept { return m_surface.HasPendingFrame(); }
	[[nodiscard]] workbench::rendering::FrameSurfaceId StableSurfaceId() const noexcept
	{
		return m_surface.Snapshot().surfaceId;
	}

private:
	[[nodiscard]] Result Closed() const noexcept
	{
		return {Status::Closed, m_surface.Snapshot().phase, std::nullopt};
	}

	workbench::rendering::FrameSurfaceCommitState m_surface;
	std::uint64_t m_lastLifetimeEpoch = 0;
};

} // namespace markdown
