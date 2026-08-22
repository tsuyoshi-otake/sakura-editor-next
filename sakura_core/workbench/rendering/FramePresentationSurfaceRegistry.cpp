#include "StdAfx.h"
#include "workbench/rendering/FramePresentationSurfaceRegistry.h"

#include <limits>

namespace workbench::rendering {

FramePresentationSurfaceRegistry::FramePresentationSurfaceRegistry(
	const std::size_t maximumSurfaceCount) noexcept
	: m_maximumSurfaceCount(maximumSurfaceCount == 0 ? 1 : maximumSurfaceCount)
{
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::Register(
	const FramePresentationSurfaceSpec& spec)
{
	if (!IsValidSpec(spec)) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	const auto iterator = m_surfaces.find(spec.surfaceId);
	if (iterator == m_surfaces.end()) {
		if (m_surfaces.size() >= m_maximumSurfaceCount) return Result(
			EFramePresentationSurfaceStatus::Full,
			EFramePresentationSurfaceState::Closed);
		m_surfaces.emplace(spec.surfaceId, FramePresentationSurfaceSnapshot{
			.surfaceId = spec.surfaceId,
			.surfaceLifetimeEpoch = spec.surfaceLifetimeEpoch,
			.deviceEpoch = spec.deviceEpoch,
			.layoutEpoch = spec.layoutEpoch,
			.width = spec.width,
			.height = spec.height,
			.state = EFramePresentationSurfaceState::Ready,
			.lastPresentedRequestId = 0,
			.visible = spec.visible,
			.hasLastGoodContent = false,
		});
		return Result(EFramePresentationSurfaceStatus::Succeeded,
			EFramePresentationSurfaceState::Ready);
	}

	auto& snapshot = iterator->second;
	if (snapshot.surfaceLifetimeEpoch == spec.surfaceLifetimeEpoch) {
		if (snapshot.state != EFramePresentationSurfaceState::Closed) return Result(
			EFramePresentationSurfaceStatus::Busy, snapshot.state);
		return Result(EFramePresentationSurfaceStatus::Stale, snapshot.state);
	}
	if (spec.surfaceLifetimeEpoch < snapshot.surfaceLifetimeEpoch) return Result(
		EFramePresentationSurfaceStatus::Stale, snapshot.state);
	snapshot.surfaceLifetimeEpoch = spec.surfaceLifetimeEpoch;
	snapshot.deviceEpoch = spec.deviceEpoch;
	snapshot.layoutEpoch = spec.layoutEpoch;
	snapshot.width = spec.width;
	snapshot.height = spec.height;
	snapshot.state = EFramePresentationSurfaceState::Ready;
	snapshot.lastPresentedRequestId = 0;
	snapshot.visible = spec.visible;
	snapshot.hasLastGoodContent = false;
	return Result(EFramePresentationSurfaceStatus::Succeeded,
		EFramePresentationSurfaceState::Ready);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::Close(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	snapshot->state = EFramePresentationSurfaceState::Closed;
	snapshot->visible = false;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::Resize(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint32_t width, const std::uint32_t height) noexcept
{
	if (width == 0 || height == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	snapshot->width = width;
	snapshot->height = height;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkPresented(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch, const std::uint64_t requestId) noexcept
{
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto current = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!current.Accepted()) return current;
	return MarkPresented(surfaceId, surfaceLifetimeEpoch, deviceEpoch,
		snapshot->layoutEpoch, requestId);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkPresented(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch, const std::uint64_t layoutEpoch,
	const std::uint64_t requestId) noexcept
{
	if (deviceEpoch == 0 || layoutEpoch == 0 || requestId == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	if (deviceEpoch != snapshot->deviceEpoch
		|| layoutEpoch < snapshot->layoutEpoch
		|| requestId <= snapshot->lastPresentedRequestId) {
		return Result(EFramePresentationSurfaceStatus::Stale, snapshot->state);
	}
	snapshot->deviceEpoch = deviceEpoch;
	snapshot->layoutEpoch = layoutEpoch;
	snapshot->lastPresentedRequestId = requestId;
	snapshot->state = EFramePresentationSurfaceState::Ready;
	snapshot->hasLastGoodContent = true;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkBackpressure(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	snapshot->state = EFramePresentationSurfaceState::Backpressured;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkDeviceLost(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch) noexcept
{
	if (deviceEpoch == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	if (deviceEpoch < snapshot->deviceEpoch) return Result(
		EFramePresentationSurfaceStatus::Stale, snapshot->state);
	snapshot->deviceEpoch = deviceEpoch;
	snapshot->state = EFramePresentationSurfaceState::DeviceLost;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkSoftwareOnly(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch) noexcept
{
	if (deviceEpoch == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	if (deviceEpoch < snapshot->deviceEpoch) return Result(
		EFramePresentationSurfaceStatus::Stale, snapshot->state);
	snapshot->deviceEpoch = deviceEpoch;
	snapshot->state = EFramePresentationSurfaceState::SoftwareOnly;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::ReprojectDevice(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch, const bool softwareOnly) noexcept
{
	if (deviceEpoch == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	if (deviceEpoch <= snapshot->deviceEpoch) return Result(
		EFramePresentationSurfaceStatus::Stale, snapshot->state);
	snapshot->deviceEpoch = deviceEpoch;
	if (snapshot->state != EFramePresentationSurfaceState::GdiFallback) {
		snapshot->state = softwareOnly
			? EFramePresentationSurfaceState::SoftwareOnly
			: EFramePresentationSurfaceState::Ready;
	}
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkGdiFallback(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	const std::uint64_t deviceEpoch, const std::uint64_t layoutEpoch,
	const std::uint64_t requestId, const bool visible) noexcept
{
	if (deviceEpoch == 0 || layoutEpoch == 0 || requestId == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	if (deviceEpoch != snapshot->deviceEpoch
		|| layoutEpoch < snapshot->layoutEpoch
		|| requestId <= snapshot->lastPresentedRequestId) {
		return Result(EFramePresentationSurfaceStatus::Stale, snapshot->state);
	}
	snapshot->layoutEpoch = layoutEpoch;
	snapshot->lastPresentedRequestId = requestId;
	snapshot->visible = visible;
	snapshot->state = EFramePresentationSurfaceState::GdiFallback;
	snapshot->hasLastGoodContent = true;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::MarkFailed(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	FramePresentationSurfaceSnapshot* snapshot = nullptr;
	const auto result = FindCurrent(surfaceId, surfaceLifetimeEpoch, snapshot);
	if (!result.Accepted()) return result;
	snapshot->state = EFramePresentationSurfaceState::Failed;
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

std::optional<FramePresentationSurfaceSnapshot> FramePresentationSurfaceRegistry::Snapshot(
	const FrameSurfaceId surfaceId) const
{
	const auto iterator = m_surfaces.find(surfaceId);
	if (iterator == m_surfaces.end()) return std::nullopt;
	return iterator->second;
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::Result(
	const EFramePresentationSurfaceStatus status,
	const EFramePresentationSurfaceState state) const noexcept
{
	return { .status = status, .state = state };
}

FramePresentationSurfaceResult FramePresentationSurfaceRegistry::FindCurrent(
	const FrameSurfaceId surfaceId, const std::uint64_t surfaceLifetimeEpoch,
	FramePresentationSurfaceSnapshot*& snapshot) noexcept
{
	if (surfaceId == 0 || surfaceLifetimeEpoch == 0) return Result(
		EFramePresentationSurfaceStatus::Invalid,
		EFramePresentationSurfaceState::Closed);
	const auto iterator = m_surfaces.find(surfaceId);
	if (iterator == m_surfaces.end()) return Result(
		EFramePresentationSurfaceStatus::UnknownSurface,
		EFramePresentationSurfaceState::Closed);
	snapshot = &iterator->second;
	if (snapshot->surfaceLifetimeEpoch != surfaceLifetimeEpoch) return Result(
		EFramePresentationSurfaceStatus::Stale, snapshot->state);
	if (snapshot->state == EFramePresentationSurfaceState::Closed) return Result(
		EFramePresentationSurfaceStatus::Closed, snapshot->state);
	return Result(EFramePresentationSurfaceStatus::Succeeded, snapshot->state);
}

bool FramePresentationSurfaceRegistry::IsValidSpec(
	const FramePresentationSurfaceSpec& spec) noexcept
{
	return spec.surfaceId != 0 && spec.surfaceLifetimeEpoch != 0
		&& spec.deviceEpoch != 0 && spec.layoutEpoch != 0
		&& spec.width != 0 && spec.height != 0;
}

} // namespace workbench::rendering
