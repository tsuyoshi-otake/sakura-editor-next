/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameSurfaceCommitState.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace workbench::rendering {

//! Stable singleton roles in one editor window. Dynamic editor and terminal
//! panes use caller-assigned ids outside this reserved range.
enum class EFrameWindowSurfaceRole : std::uint8_t {
	ActivityBar,
	PrimarySideBar,
	SecondarySideBar,
	Panel,
	TitleAndMenu,
	Tabs,
	StatusBar,
	Editor,
	MarkdownPreview,
	Terminal,
};

[[nodiscard]] constexpr FrameSurfaceId FrameWindowSurfaceId(
	const EFrameWindowSurfaceRole role) noexcept
{
	return 0x100u + static_cast<FrameSurfaceId>(role);
}

struct FrameWindowSurfaceSpec final {
	FrameSurfaceId surfaceId = 0;
	std::string hostId;
	bool visible = false;
};

enum class EFrameWindowTransactionStatus : std::uint8_t {
	Succeeded,
	Partial,
	Invalid,
	UnknownSurface,
	Full,
	Closed,
	Exhausted,
};

struct FrameWindowTransactionResult final {
	EFrameWindowTransactionStatus status = EFrameWindowTransactionStatus::Invalid;
	std::size_t acceptedSurfaceCount = 0;
	std::size_t rejectedSurfaceCount = 0;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EFrameWindowTransactionStatus::Succeeded;
	}
};

struct FrameWindowTransactionTelemetry final {
	std::uint64_t layoutTransactions = 0;
	std::uint64_t contentRequests = 0;
	std::uint64_t deviceTransactions = 0;
	std::uint64_t committedSurfaces = 0;
	std::uint64_t partialTransactions = 0;
	std::uint64_t rejectedSurfaceOperations = 0;
};

//! Pure, bounded per-window transaction boundary.
//!
//! Every surface receives the same monotonically increasing LayoutEpoch, but
//! commits remain per-surface. A missing or late surface therefore cannot hold
//! ready siblings behind a window-wide readiness barrier. Native code calls
//! CommitGdiBoundary only after child placement, synchronous invalidation, and
//! GdiFlush have completed.
class FrameWindowTransaction final {
public:
	explicit FrameWindowTransaction(std::size_t maximumSurfaceCount = 64) noexcept;

	FrameWindowTransaction(const FrameWindowTransaction&) = delete;
	FrameWindowTransaction& operator=(const FrameWindowTransaction&) = delete;
	FrameWindowTransaction(FrameWindowTransaction&&) = delete;
	FrameWindowTransaction& operator=(FrameWindowTransaction&&) = delete;

	[[nodiscard]] FrameWindowTransactionResult OpenSurface(
		const FrameWindowSurfaceSpec& spec,
		std::uint64_t surfaceLifetimeEpoch = 1);
	[[nodiscard]] FrameWindowTransactionResult CloseSurface(
		FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] FrameWindowTransactionResult Close() noexcept;

	[[nodiscard]] FrameWindowTransactionResult SetProjection(
		FrameSurfaceId surfaceId, const std::string& hostId,
		bool visible);
	[[nodiscard]] FrameWindowTransactionResult BeginLayout() noexcept;
	[[nodiscard]] FrameWindowTransactionResult NotifyContent(
		FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] FrameWindowTransactionResult SetDeviceEpoch(
		std::uint64_t deviceEpoch) noexcept;

	[[nodiscard]] std::optional<FrameSurfaceAdapterSnapshot> CommitSurfaceGdi(
		FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] std::vector<FrameSurfaceAdapterSnapshot>
		CommitGdiBoundary();

	[[nodiscard]] std::optional<FrameSurfaceAdapterSnapshot> SurfaceSnapshot(
		FrameSurfaceId surfaceId) const;
	[[nodiscard]] std::vector<FrameSurfaceAdapterSnapshot> Snapshots() const;
	[[nodiscard]] std::uint64_t LayoutEpoch() const noexcept { return m_layoutEpoch; }
	[[nodiscard]] std::uint64_t DeviceEpoch() const noexcept { return m_deviceEpoch; }
	[[nodiscard]] std::size_t Size() const noexcept { return m_surfaces.size(); }
	[[nodiscard]] const FrameWindowTransactionTelemetry& Telemetry() const noexcept
	{
		return m_telemetry;
	}

private:
	struct Entry final {
		explicit Entry(const FrameSurfaceId surfaceId) noexcept
			: state(surfaceId)
		{
		}

		FrameSurfaceCommitState state;
	};

	[[nodiscard]] Entry* Find(FrameSurfaceId surfaceId) noexcept;
	[[nodiscard]] const Entry* Find(FrameSurfaceId surfaceId) const noexcept;
	[[nodiscard]] FrameWindowTransactionResult SingleResult(
		const FrameSurfaceAdapterResult& result) noexcept;

	std::size_t m_maximumSurfaceCount = 1;
	std::uint64_t m_layoutEpoch = 1;
	std::uint64_t m_deviceEpoch = 1;
	bool m_closed = false;
	std::vector<std::unique_ptr<Entry>> m_surfaces;
	FrameWindowTransactionTelemetry m_telemetry;
};

} // namespace workbench::rendering
