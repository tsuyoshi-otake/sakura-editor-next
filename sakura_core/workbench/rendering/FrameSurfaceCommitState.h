#pragma once

#include "FrameSurfaceAdapter.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace workbench::rendering {

// Pure boundary between a native layout notification and the enclosing GDI
// flush. A requested ticket is never presented until CommitGdiFrame succeeds.
class FrameSurfaceCommitState final {
public:
	explicit FrameSurfaceCommitState(const FrameSurfaceId surfaceId) noexcept
		: m_surface(surfaceId)
	{
	}

	[[nodiscard]] FrameSurfaceAdapterResult Open(
		const std::string_view hostId, const bool visible,
		const std::uint64_t surfaceLifetimeEpoch = 1,
		const std::uint64_t layoutEpoch = 1,
		const std::uint64_t deviceEpoch = 1,
		const std::uint64_t contentGeneration = 1) noexcept
	{
		auto result = m_surface.Open(hostId, visible, surfaceLifetimeEpoch,
			layoutEpoch, deviceEpoch, contentGeneration);
		if (result.Accepted()) {
			m_surfaceLifetimeEpoch = surfaceLifetimeEpoch;
			m_layoutEpoch = layoutEpoch;
			m_deviceEpoch = deviceEpoch;
			m_contentGeneration = contentGeneration;
			m_requestId = 0;
			m_pending.reset();
		}
		return result;
	}

	[[nodiscard]] FrameSurfaceAdapterResult Close() noexcept
	{
		m_pending.reset();
		return m_surface.Close(m_surfaceLifetimeEpoch);
	}

	[[nodiscard]] bool IsOpen() const noexcept { return m_surface.IsOpen(); }
	[[nodiscard]] FrameSurfaceId SurfaceId() const noexcept
	{
		return m_surface.StableSurfaceId();
	}

	[[nodiscard]] FrameSurfaceAdapterResult SetHost(const std::string_view hostId) noexcept
	{
		const auto previousEpoch = m_surface.Snapshot().hostEpoch;
		auto result = m_surface.SetHost(hostId);
		if (result.Accepted() && m_surface.Snapshot().hostEpoch != previousEpoch) {
			m_pending.reset();
		}
		return result;
	}

	[[nodiscard]] FrameSurfaceAdapterResult SetVisible(const bool visible) noexcept
	{
		const auto previousEpoch = m_surface.Snapshot().visibilityEpoch;
		auto result = m_surface.SetVisible(visible);
		if (result.Accepted()
			&& m_surface.Snapshot().visibilityEpoch != previousEpoch) {
			m_pending.reset();
		}
		return result;
	}

	[[nodiscard]] FrameSurfaceAdapterResult NotifyLayout() noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (m_layoutEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
			return Exhausted();
		}
		++m_layoutEpoch;
		return UpdateEpochsAndRequest();
	}

	[[nodiscard]] FrameSurfaceAdapterResult NotifyLayoutEpoch(
		const std::uint64_t layoutEpoch) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (layoutEpoch < m_layoutEpoch) return Stale();
		if (layoutEpoch == m_layoutEpoch) return RequestCurrent();
		m_layoutEpoch = layoutEpoch;
		return UpdateEpochsAndRequest();
	}

	[[nodiscard]] FrameSurfaceAdapterResult NotifyContent() noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (m_contentGeneration == (std::numeric_limits<std::uint64_t>::max)()) {
			return Exhausted();
		}
		++m_contentGeneration;
		return UpdateEpochsAndRequest();
	}

	[[nodiscard]] FrameSurfaceAdapterResult NotifyContentGeneration(
		const std::uint64_t contentGeneration) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (contentGeneration < m_contentGeneration) return Stale();
		if (contentGeneration == m_contentGeneration) return RequestCurrent();
		m_contentGeneration = contentGeneration;
		return UpdateEpochsAndRequest();
	}

	[[nodiscard]] FrameSurfaceAdapterResult NotifyDeviceEpoch(
		const std::uint64_t deviceEpoch) noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (deviceEpoch < m_deviceEpoch) return Stale();
		if (deviceEpoch == m_deviceEpoch) return RequestCurrent();
		m_deviceEpoch = deviceEpoch;
		return UpdateEpochsAndRequest();
	}

	[[nodiscard]] FrameSurfaceAdapterResult RequestCurrent() noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		if (m_requestId == (std::numeric_limits<std::uint64_t>::max)()) {
			return Exhausted();
		}
		const auto snapshot = m_surface.Snapshot();
		auto requested = m_surface.Request(FrameSurfaceAdapterRequest{
			.surfaceId = snapshot.surfaceId,
			.hostId = snapshot.hostId,
			.surfaceLifetimeEpoch = snapshot.surfaceLifetimeEpoch,
			.requestId = ++m_requestId,
			.contentGeneration = snapshot.contentGeneration,
			.layoutEpoch = snapshot.layoutEpoch,
			.deviceEpoch = snapshot.deviceEpoch,
			.workClass = snapshot.visible ? EFrameWorkClass::Interactive
				: EFrameWorkClass::Background,
			.visible = snapshot.visible,
			.hostEpoch = snapshot.hostEpoch,
			.visibilityEpoch = snapshot.visibilityEpoch,
		});
		if (requested.ticket) m_pending = requested.ticket;
		return requested;
	}

	[[nodiscard]] std::optional<FrameSurfaceAdapterSnapshot> CommitGdiFrame() noexcept
	{
		if (!m_pending.has_value()) return std::nullopt;
		const auto ticket = *m_pending;
		m_pending.reset();
		if (!m_surface.Commit(ticket).Accepted()) return std::nullopt;
		return m_surface.Snapshot();
	}

	[[nodiscard]] FrameSurfaceAdapterSnapshot Snapshot() const noexcept
	{
		return m_surface.Snapshot();
	}

	[[nodiscard]] bool HasPendingFrame() const noexcept { return m_pending.has_value(); }

private:
	[[nodiscard]] FrameSurfaceAdapterResult UpdateEpochsAndRequest() noexcept
	{
		if (!m_surface.IsOpen()) return Closed();
		const auto epochs = m_surface.UpdateEpochs(
			m_contentGeneration, m_layoutEpoch, m_deviceEpoch);
		if (!epochs.Accepted()) return epochs;
		return RequestCurrent();
	}

	[[nodiscard]] FrameSurfaceAdapterResult Closed() const noexcept
	{
		return {EFrameSurfaceAdapterStatus::Closed,
			m_surface.Snapshot().phase, std::nullopt};
	}

	[[nodiscard]] FrameSurfaceAdapterResult Stale() const noexcept
	{
		return {EFrameSurfaceAdapterStatus::Stale,
			m_surface.Snapshot().phase, std::nullopt};
	}

	[[nodiscard]] FrameSurfaceAdapterResult Exhausted() const noexcept
	{
		return {EFrameSurfaceAdapterStatus::Exhausted,
			m_surface.Snapshot().phase, std::nullopt};
	}

	FrameSurfaceAdapter m_surface;
	std::uint64_t m_surfaceLifetimeEpoch = 0;
	std::uint64_t m_layoutEpoch = 1;
	std::uint64_t m_deviceEpoch = 1;
	std::uint64_t m_contentGeneration = 1;
	std::uint64_t m_requestId = 0;
	std::optional<FrameSurfaceAdapterTicket> m_pending;
};

} // namespace workbench::rendering
