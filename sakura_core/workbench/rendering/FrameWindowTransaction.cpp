/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameWindowTransaction.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::rendering {

FrameWindowTransaction::FrameWindowTransaction(
	const std::size_t maximumSurfaceCount) noexcept
	: m_maximumSurfaceCount(std::max<std::size_t>(maximumSurfaceCount, 1))
{
}

FrameWindowTransactionResult FrameWindowTransaction::OpenSurface(
	const FrameWindowSurfaceSpec& spec,
	const std::uint64_t surfaceLifetimeEpoch)
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	if (spec.surfaceId == 0 || spec.hostId.empty()
		|| surfaceLifetimeEpoch == 0) {
		return {EFrameWindowTransactionStatus::Invalid};
	}
	if (auto* existing = Find(spec.surfaceId); existing != nullptr) {
		if (existing->state.IsOpen()) {
			return {EFrameWindowTransactionStatus::Invalid};
		}
		const auto reopened = existing->state.Open(spec.hostId, spec.visible,
			surfaceLifetimeEpoch, m_layoutEpoch, m_deviceEpoch, 1);
		return SingleResult(reopened);
	}
	if (m_surfaces.size() >= m_maximumSurfaceCount) {
		return {EFrameWindowTransactionStatus::Full};
	}

	auto entry = std::make_unique<Entry>(spec.surfaceId);
	const auto opened = entry->state.Open(spec.hostId, spec.visible,
		surfaceLifetimeEpoch, m_layoutEpoch, m_deviceEpoch, 1);
	if (!opened.Accepted()) return SingleResult(opened);
	m_surfaces.push_back(std::move(entry));
	return {EFrameWindowTransactionStatus::Succeeded, 1, 0};
}

FrameWindowTransactionResult FrameWindowTransaction::CloseSurface(
	const FrameSurfaceId surfaceId) noexcept
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	auto* entry = Find(surfaceId);
	if (entry == nullptr) return {EFrameWindowTransactionStatus::UnknownSurface};
	return SingleResult(entry->state.Close());
}

FrameWindowTransactionResult FrameWindowTransaction::Close() noexcept
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	FrameWindowTransactionResult result{
		EFrameWindowTransactionStatus::Succeeded, 0, 0};
	for (auto& entry : m_surfaces) {
		if (!entry->state.IsOpen()) continue;
		if (entry->state.Close().Accepted()) {
			++result.acceptedSurfaceCount;
		} else {
			++result.rejectedSurfaceCount;
			++m_telemetry.rejectedSurfaceOperations;
		}
	}
	m_closed = true;
	if (result.rejectedSurfaceCount != 0) {
		result.status = EFrameWindowTransactionStatus::Partial;
		++m_telemetry.partialTransactions;
	}
	return result;
}

FrameWindowTransactionResult FrameWindowTransaction::SetProjection(
	const FrameSurfaceId surfaceId, const std::string& hostId,
	const bool visible)
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	auto* entry = Find(surfaceId);
	if (entry == nullptr) return {EFrameWindowTransactionStatus::UnknownSurface};
	const auto before = entry->state.Snapshot();
	const auto hostResult = entry->state.SetHost(hostId);
	if (!hostResult.Accepted()) return SingleResult(hostResult);
	const auto visibilityResult = entry->state.SetVisible(visible);
	if (!visibilityResult.Accepted()) return SingleResult(visibilityResult);
	const auto after = entry->state.Snapshot();
	if (after.hostEpoch == before.hostEpoch
		&& after.visibilityEpoch == before.visibilityEpoch) {
		return {EFrameWindowTransactionStatus::Succeeded, 1, 0};
	}
	return SingleResult(entry->state.RequestCurrent());
}

FrameWindowTransactionResult FrameWindowTransaction::BeginLayout() noexcept
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	if (m_layoutEpoch == (std::numeric_limits<std::uint64_t>::max)()) {
		return {EFrameWindowTransactionStatus::Exhausted};
	}
	++m_layoutEpoch;
	++m_telemetry.layoutTransactions;
	FrameWindowTransactionResult result{
		EFrameWindowTransactionStatus::Succeeded, 0, 0};
	for (auto& entry : m_surfaces) {
		if (!entry->state.IsOpen()) continue;
		const auto requested = entry->state.NotifyLayoutEpoch(m_layoutEpoch);
		if (requested.Accepted()) {
			++result.acceptedSurfaceCount;
		} else {
			++result.rejectedSurfaceCount;
			++m_telemetry.rejectedSurfaceOperations;
		}
	}
	if (result.rejectedSurfaceCount != 0) {
		result.status = EFrameWindowTransactionStatus::Partial;
		++m_telemetry.partialTransactions;
	}
	return result;
}

FrameWindowTransactionResult FrameWindowTransaction::NotifyContent(
	const FrameSurfaceId surfaceId) noexcept
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	auto* entry = Find(surfaceId);
	if (entry == nullptr) return {EFrameWindowTransactionStatus::UnknownSurface};
	++m_telemetry.contentRequests;
	return SingleResult(entry->state.NotifyContent());
}

FrameWindowTransactionResult FrameWindowTransaction::SetDeviceEpoch(
	const std::uint64_t deviceEpoch) noexcept
{
	if (m_closed) return {EFrameWindowTransactionStatus::Closed};
	if (deviceEpoch == 0) return {EFrameWindowTransactionStatus::Invalid};
	if (deviceEpoch < m_deviceEpoch) return {EFrameWindowTransactionStatus::Invalid};
	m_deviceEpoch = deviceEpoch;
	++m_telemetry.deviceTransactions;
	FrameWindowTransactionResult result{
		EFrameWindowTransactionStatus::Succeeded, 0, 0};
	for (auto& entry : m_surfaces) {
		if (!entry->state.IsOpen()) continue;
		const auto requested = entry->state.NotifyDeviceEpoch(deviceEpoch);
		if (requested.Accepted()) {
			++result.acceptedSurfaceCount;
		} else {
			++result.rejectedSurfaceCount;
			++m_telemetry.rejectedSurfaceOperations;
		}
	}
	if (result.rejectedSurfaceCount != 0) {
		result.status = EFrameWindowTransactionStatus::Partial;
		++m_telemetry.partialTransactions;
	}
	return result;
}

std::optional<FrameSurfaceAdapterSnapshot>
FrameWindowTransaction::CommitSurfaceGdi(
	const FrameSurfaceId surfaceId) noexcept
{
	auto* entry = Find(surfaceId);
	if (m_closed || entry == nullptr) return std::nullopt;
	auto committed = entry->state.CommitGdiFrame();
	if (committed.has_value()) ++m_telemetry.committedSurfaces;
	return committed;
}

std::vector<FrameSurfaceAdapterSnapshot>
FrameWindowTransaction::CommitGdiBoundary()
{
	std::vector<FrameSurfaceAdapterSnapshot> committed;
	if (m_closed) return committed;
	committed.reserve(m_surfaces.size());
	for (auto& entry : m_surfaces) {
		auto snapshot = entry->state.CommitGdiFrame();
		if (!snapshot.has_value()) continue;
		committed.push_back(std::move(*snapshot));
		++m_telemetry.committedSurfaces;
	}
	return committed;
}

std::optional<FrameSurfaceAdapterSnapshot>
FrameWindowTransaction::SurfaceSnapshot(const FrameSurfaceId surfaceId) const
{
	const auto* entry = Find(surfaceId);
	if (entry == nullptr) return std::nullopt;
	return entry->state.Snapshot();
}

std::vector<FrameSurfaceAdapterSnapshot>
FrameWindowTransaction::Snapshots() const
{
	std::vector<FrameSurfaceAdapterSnapshot> snapshots;
	snapshots.reserve(m_surfaces.size());
	for (const auto& entry : m_surfaces) {
		snapshots.push_back(entry->state.Snapshot());
	}
	return snapshots;
}

FrameWindowTransaction::Entry* FrameWindowTransaction::Find(
	const FrameSurfaceId surfaceId) noexcept
{
	for (const auto& entry : m_surfaces) {
		if (entry->state.SurfaceId() == surfaceId) return entry.get();
	}
	return nullptr;
}

const FrameWindowTransaction::Entry* FrameWindowTransaction::Find(
	const FrameSurfaceId surfaceId) const noexcept
{
	for (const auto& entry : m_surfaces) {
		if (entry->state.SurfaceId() == surfaceId) return entry.get();
	}
	return nullptr;
}

FrameWindowTransactionResult FrameWindowTransaction::SingleResult(
	const FrameSurfaceAdapterResult& result) noexcept
{
	if (result.Accepted()) {
		return {EFrameWindowTransactionStatus::Succeeded, 1, 0};
	}
	++m_telemetry.rejectedSurfaceOperations;
	switch (result.status) {
	case EFrameSurfaceAdapterStatus::Closed:
		return {EFrameWindowTransactionStatus::Closed, 0, 1};
	case EFrameSurfaceAdapterStatus::Exhausted:
		return {EFrameWindowTransactionStatus::Exhausted, 0, 1};
	case EFrameSurfaceAdapterStatus::UnknownSurface:
		return {EFrameWindowTransactionStatus::UnknownSurface, 0, 1};
	default:
		return {EFrameWindowTransactionStatus::Invalid, 0, 1};
	}
}

} // namespace workbench::rendering
