/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameCoordinatorModel.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace workbench::rendering {
namespace {

constexpr std::uint32_t kAgingIncrement = 64;
constexpr std::uint32_t kMaximumAging = 2048;

[[nodiscard]] std::uint32_t BasePriority( EFrameWorkClass workClass ) noexcept
{
	switch( workClass ) {
	case EFrameWorkClass::Interactive:
		return 1024;
	case EFrameWorkClass::Visible:
		return 512;
	case EFrameWorkClass::Background:
		return 0;
	}
	return 0;
}

[[nodiscard]] bool Matches( const FrameSurfaceRequest& request, const FrameWorkTicket& ticket ) noexcept
{
	return request.surfaceId == ticket.surfaceId
		&& request.surfaceLifetimeEpoch == ticket.surfaceLifetimeEpoch
		&& request.requestId == ticket.requestId
		&& request.contentGeneration == ticket.contentGeneration
		&& request.layoutEpoch == ticket.layoutEpoch
		&& request.deviceEpoch == ticket.deviceEpoch;
}

} // namespace

bool FrameSurfaceRequest::IsValid() const noexcept
{
	return surfaceId != 0 && surfaceLifetimeEpoch != 0 && requestId != 0
		&& contentGeneration != 0 && layoutEpoch != 0 && deviceEpoch != 0;
}

bool FrameWorkTicket::IsValid() const noexcept
{
	return surfaceId != 0 && surfaceLifetimeEpoch != 0 && requestId != 0
		&& contentGeneration != 0 && layoutEpoch != 0 && deviceEpoch != 0;
}

FrameCoordinatorModel::FrameCoordinatorModel( std::uint64_t deviceEpoch ) noexcept
	: m_deviceEpoch(deviceEpoch == 0 ? 1 : deviceEpoch)
{
}

FrameOperationResult FrameCoordinatorModel::RegisterSurface(
	FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch )
{
	if( surfaceId == 0 || surfaceLifetimeEpoch == 0 ) {
		return { EFrameOperationStatus::Invalid, EFrameSurfacePhase::Closed };
	}
	auto [it, inserted] = m_surfaces.try_emplace(surfaceId);
	auto& slot = it->second;
	if( !inserted && slot.phase != EFrameSurfacePhase::Closed ) {
		return { EFrameOperationStatus::Busy, slot.phase };
	}
	if( !inserted && surfaceLifetimeEpoch <= slot.lifetimeEpoch ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	slot = {};
	slot.lifetimeEpoch = surfaceLifetimeEpoch;
	slot.phase = EFrameSurfacePhase::Idle;
	return { EFrameOperationStatus::Succeeded, slot.phase };
}

FrameOperationResult FrameCoordinatorModel::CloseSurface(
	FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch )
{
	const auto it = m_surfaces.find(surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( surfaceLifetimeEpoch != slot.lifetimeEpoch ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	slot.pending.reset();
	if( slot.phase == EFrameSurfacePhase::GpuUpdating ) {
		slot.closeRequested = true;
		slot.phase = EFrameSurfacePhase::Closing;
		return { EFrameOperationStatus::Busy, slot.phase };
	}
	if( slot.phase == EFrameSurfacePhase::Closing ) {
		return { EFrameOperationStatus::Busy, slot.phase };
	}
	if( slot.phase == EFrameSurfacePhase::Publishable
		|| slot.phase == EFrameSurfacePhase::Withdrawn ) {
		slot.closeRequested = true;
		slot.phase = EFrameSurfacePhase::Withdrawn;
		return { EFrameOperationStatus::Busy, slot.phase };
	}
	slot.active.reset();
	slot.phase = EFrameSurfacePhase::Closed;
	slot.aging = 0;
	slot.hasLastGoodContent = false;
	slot.closeRequested = false;
	return { EFrameOperationStatus::Succeeded, slot.phase };
}

FrameOperationResult FrameCoordinatorModel::FinalizeCloseSurface(
	FrameSurfaceId surfaceId, std::uint64_t surfaceLifetimeEpoch ) noexcept
{
	const auto it = m_surfaces.find(surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( surfaceLifetimeEpoch != slot.lifetimeEpoch ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	slot.pending.reset();
	slot.active.reset();
	slot.phase = EFrameSurfacePhase::Closed;
	slot.aging = 0;
	slot.hasLastGoodContent = false;
	slot.closeRequested = false;
	return { EFrameOperationStatus::Succeeded, slot.phase };
}

FrameOperationResult FrameCoordinatorModel::Request( const FrameSurfaceRequest& request )
{
	if( !request.IsValid() || request.deviceEpoch != m_deviceEpoch ) {
		return { request.IsValid() ? EFrameOperationStatus::Stale : EFrameOperationStatus::Invalid,
			EFrameSurfacePhase::Closed };
	}
	const auto it = m_surfaces.find(request.surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( slot.phase == EFrameSurfacePhase::Closed || slot.closeRequested
		|| slot.phase == EFrameSurfacePhase::Closing ) {
		return { EFrameOperationStatus::Closed, slot.phase };
	}
	if( slot.newestRequestId == (std::numeric_limits<std::uint64_t>::max)() ) {
		return { EFrameOperationStatus::Exhausted, slot.phase };
	}
	if( request.surfaceLifetimeEpoch != slot.lifetimeEpoch
		|| request.requestId <= slot.newestRequestId ) {
		++m_telemetry.staleRequests;
		return { EFrameOperationStatus::Stale, slot.phase };
	}

	const bool replaced = slot.pending.has_value();
	slot.pending = request;
	slot.newestRequestId = request.requestId;
	if( !slot.active ) slot.phase = EFrameSurfacePhase::Requested;
	++m_telemetry.acceptedRequests;
	if( replaced ) ++m_telemetry.replacedRequests;
	return { replaced ? EFrameOperationStatus::Replaced : EFrameOperationStatus::Succeeded,
		slot.phase };
}

std::optional<FrameWorkTicket> FrameCoordinatorModel::TakeNextCpuWork()
{
	SurfaceSlot* selected = nullptr;
	FrameSurfaceId selectedId = 0;
	std::uint32_t selectedScore = 0;
	for( auto& [surfaceId, slot] : m_surfaces ) {
		if( slot.phase != EFrameSurfacePhase::Requested || !slot.pending || slot.active ) continue;
		const auto score = BasePriority(slot.pending->workClass) + slot.aging;
		if( !selected || score > selectedScore || (score == selectedScore && surfaceId < selectedId) ) {
			selected = &slot;
			selectedId = surfaceId;
			selectedScore = score;
		}
	}
	if( !selected ) return std::nullopt;

	for( auto& [surfaceId, slot] : m_surfaces ) {
		if( surfaceId == selectedId || slot.phase != EFrameSurfacePhase::Requested || !slot.pending ) continue;
		slot.aging = (std::min)(kMaximumAging, slot.aging + kAgingIncrement);
	}
	selected->active = MakeTicket(*selected->pending);
	selected->pending.reset();
	selected->phase = EFrameSurfacePhase::CpuRunning;
	selected->aging = 0;
	++m_telemetry.scheduledCpuWork;
	return selected->active;
}

FrameOperationResult FrameCoordinatorModel::CancelCpu( const FrameWorkTicket& ticket )
{
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( slot.phase == EFrameSurfacePhase::Closed ) {
		return { EFrameOperationStatus::Closed, slot.phase };
	}
	if( ticket.deviceEpoch != m_deviceEpoch || ticket.surfaceLifetimeEpoch != slot.lifetimeEpoch
		|| slot.phase != EFrameSurfacePhase::CpuRunning || !slot.active
		|| *slot.active != ticket ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	slot.phase = EFrameSurfacePhase::Withdrawn;
	++m_telemetry.supersededWork;
	return { EFrameOperationStatus::Superseded, slot.phase };
}

FrameOperationResult FrameCoordinatorModel::CompleteCpu( const FrameWorkTicket& ticket )
{
	auto result = Transition(ticket, EFrameSurfacePhase::CpuRunning, EFrameSurfacePhase::CpuReady);
	if( result.status != EFrameOperationStatus::Succeeded ) return result;
	auto& slot = m_surfaces.at(ticket.surfaceId);
	if( slot.pending ) {
		slot.phase = EFrameSurfacePhase::Withdrawn;
		++m_telemetry.supersededWork;
		return { EFrameOperationStatus::Superseded, slot.phase };
	}
	return result;
}

FrameOperationResult FrameCoordinatorModel::QueueGpu( const FrameWorkTicket& ticket )
{
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it != m_surfaces.end() && it->second.pending ) {
		auto result = Transition(ticket, EFrameSurfacePhase::CpuReady, EFrameSurfacePhase::Withdrawn);
		if( result.status == EFrameOperationStatus::Succeeded ) {
			++m_telemetry.supersededWork;
			result.status = EFrameOperationStatus::Superseded;
		}
		return result;
	}
	return Transition(ticket, EFrameSurfacePhase::CpuReady, EFrameSurfacePhase::GpuQueued);
}

FrameOperationResult FrameCoordinatorModel::BeginGpu( const FrameWorkTicket& ticket )
{
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it != m_surfaces.end() && it->second.pending ) {
		auto result = Transition(ticket, EFrameSurfacePhase::GpuQueued, EFrameSurfacePhase::Withdrawn);
		if( result.status == EFrameOperationStatus::Succeeded ) {
			++m_telemetry.supersededWork;
			result.status = EFrameOperationStatus::Superseded;
		}
		return result;
	}
	return Transition(ticket, EFrameSurfacePhase::GpuQueued, EFrameSurfacePhase::GpuUpdating);
}

FrameOperationResult FrameCoordinatorModel::CompleteGpu( const FrameWorkTicket& ticket )
{
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it != m_surfaces.end() && it->second.phase == EFrameSurfacePhase::Closing
		&& it->second.active && *it->second.active == ticket ) {
		it->second.phase = EFrameSurfacePhase::Withdrawn;
		++m_telemetry.supersededWork;
		return { EFrameOperationStatus::Superseded, it->second.phase };
	}
	if( it != m_surfaces.end() && it->second.pending ) {
		auto result = Transition(ticket, EFrameSurfacePhase::GpuUpdating, EFrameSurfacePhase::Withdrawn);
		if( result.status == EFrameOperationStatus::Succeeded ) {
			++m_telemetry.supersededWork;
			result.status = EFrameOperationStatus::Superseded;
		}
		return result;
	}
	return Transition(ticket, EFrameSurfacePhase::GpuUpdating, EFrameSurfacePhase::Publishable);
}

FrameOperationResult FrameCoordinatorModel::RetireWithdrawn( const FrameWorkTicket& ticket )
{
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( slot.phase != EFrameSurfacePhase::Withdrawn || !slot.active || *slot.active != ticket ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	FinishActive(slot);
	return { EFrameOperationStatus::Succeeded, slot.phase };
}

FrameCommitCohort FrameCoordinatorModel::AssembleCommit(
	std::uint64_t windowFrameId, std::uint64_t layoutEpoch,
	std::size_t maximumPublications )
{
	FrameCommitCohort cohort{ .windowFrameId = windowFrameId, .layoutEpoch = layoutEpoch,
		.deviceEpoch = m_deviceEpoch };
	if( windowFrameId == 0 || layoutEpoch == 0 || windowFrameId <= m_lastCompletedFrameId ) {
		return cohort;
	}

	std::vector<FrameSurfaceId> orderedIds;
	orderedIds.reserve(m_surfaces.size());
	for( const auto& [surfaceId, slot] : m_surfaces ) {
		if( slot.phase != EFrameSurfacePhase::Closed ) orderedIds.push_back(surfaceId);
	}
	std::sort(orderedIds.begin(), orderedIds.end());
	for( const auto surfaceId : orderedIds ) {
		auto& slot = m_surfaces.at(surfaceId);
		const bool publishable = slot.phase == EFrameSurfacePhase::Publishable
			&& slot.active && !slot.pending
			&& slot.active->layoutEpoch == layoutEpoch
			&& slot.active->deviceEpoch == m_deviceEpoch;
		if( publishable && cohort.publications.size() < maximumPublications ) {
			cohort.publications.push_back({ *slot.active });
			continue;
		}
		if( IsWorkOutstanding(slot) ) {
			const bool visible = (slot.pending && slot.pending->visible)
				|| (slot.active && slot.active->visible);
			if( visible ) cohort.lateSurfaces.push_back({ surfaceId, slot.hasLastGoodContent });
		}
	}
	++m_telemetry.assembledFrames;
	m_telemetry.lateSurfaceObservations += cohort.lateSurfaces.size();
	return cohort;
}

FrameOperationResult FrameCoordinatorModel::CompleteCommit( const FrameCommitCohort& cohort )
{
	if( cohort.windowFrameId == 0 || cohort.layoutEpoch == 0
		|| cohort.deviceEpoch != m_deviceEpoch
		|| cohort.windowFrameId <= m_lastCompletedFrameId ) {
		return { EFrameOperationStatus::Stale, EFrameSurfacePhase::Closed };
	}
	std::unordered_set<FrameSurfaceId> seen;
	seen.reserve(cohort.publications.size());
	for( const auto& publication : cohort.publications ) {
		if( publication.ticket.layoutEpoch != cohort.layoutEpoch
			|| publication.ticket.deviceEpoch != cohort.deviceEpoch
			|| !seen.insert(publication.ticket.surfaceId).second ) {
			return { EFrameOperationStatus::Invalid, EFrameSurfacePhase::Closed };
		}
		const auto it = m_surfaces.find(publication.ticket.surfaceId);
		if( it == m_surfaces.end() ) {
			return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
		}
		const auto& slot = it->second;
		if( slot.phase != EFrameSurfacePhase::Publishable || !slot.active
			|| *slot.active != publication.ticket || slot.pending ) {
			return { EFrameOperationStatus::Stale, slot.phase };
		}
	}

	for( const auto& publication : cohort.publications ) {
		auto& slot = m_surfaces.at(publication.ticket.surfaceId);
		slot.publishedRequestId = publication.ticket.requestId;
		slot.hasLastGoodContent = true;
		FinishActive(slot);
		++m_telemetry.publishedSurfaces;
	}
	m_lastCompletedFrameId = cohort.windowFrameId;
	return { EFrameOperationStatus::Succeeded, EFrameSurfacePhase::Idle };
}

FrameOperationResult FrameCoordinatorModel::ResetDevice( std::uint64_t newDeviceEpoch )
{
	if( newDeviceEpoch == 0 ) {
		return { EFrameOperationStatus::Invalid, EFrameSurfacePhase::Closed };
	}
	if( newDeviceEpoch <= m_deviceEpoch ) {
		return { EFrameOperationStatus::Stale, EFrameSurfacePhase::Closed };
	}
	m_deviceEpoch = newDeviceEpoch;
	m_lastCompletedFrameId = 0;
	for( auto& [surfaceId, slot] : m_surfaces ) {
		(void)surfaceId;
		if( slot.phase == EFrameSurfacePhase::Closed ) continue;
		slot.pending.reset();
		slot.active.reset();
		slot.phase = slot.closeRequested ? EFrameSurfacePhase::Closed : EFrameSurfacePhase::Idle;
		slot.aging = 0;
		slot.hasLastGoodContent = false;
		slot.closeRequested = false;
	}
	++m_telemetry.deviceResets;
	return { EFrameOperationStatus::Succeeded, EFrameSurfacePhase::Idle };
}

std::optional<FrameSurfaceSnapshot> FrameCoordinatorModel::SurfaceSnapshot(
	FrameSurfaceId surfaceId ) const
{
	const auto it = m_surfaces.find(surfaceId);
	if( it == m_surfaces.end() ) return std::nullopt;
	const auto& slot = it->second;
	return FrameSurfaceSnapshot{
		.surfaceId = surfaceId,
		.surfaceLifetimeEpoch = slot.lifetimeEpoch,
		.phase = slot.phase,
		.pendingDepth = slot.pending ? 1u : 0u,
		.activeRequestId = slot.active ? slot.active->requestId : 0,
		.newestRequestId = slot.newestRequestId,
		.publishedRequestId = slot.publishedRequestId,
		.aging = slot.aging,
		.hasLastGoodContent = slot.hasLastGoodContent,
		.closeRequested = slot.closeRequested,
	};
}

FrameOperationResult FrameCoordinatorModel::Transition(
	const FrameWorkTicket& ticket, EFrameSurfacePhase expected,
	EFrameSurfacePhase next )
{
	if( !ticket.IsValid() || ticket.deviceEpoch != m_deviceEpoch ) {
		return { EFrameOperationStatus::Stale, EFrameSurfacePhase::Closed };
	}
	const auto it = m_surfaces.find(ticket.surfaceId);
	if( it == m_surfaces.end() ) {
		return { EFrameOperationStatus::UnknownSurface, EFrameSurfacePhase::Closed };
	}
	auto& slot = it->second;
	if( slot.phase == EFrameSurfacePhase::Closed ) {
		return { EFrameOperationStatus::Closed, slot.phase };
	}
	if( ticket.surfaceLifetimeEpoch != slot.lifetimeEpoch || slot.phase != expected
		|| !slot.active || *slot.active != ticket ) {
		return { EFrameOperationStatus::Stale, slot.phase };
	}
	slot.phase = next;
	return { EFrameOperationStatus::Succeeded, slot.phase };
}

FrameWorkTicket FrameCoordinatorModel::MakeTicket( const FrameSurfaceRequest& request ) noexcept
{
	return {
		.surfaceId = request.surfaceId,
		.surfaceLifetimeEpoch = request.surfaceLifetimeEpoch,
		.requestId = request.requestId,
		.contentGeneration = request.contentGeneration,
		.layoutEpoch = request.layoutEpoch,
		.deviceEpoch = request.deviceEpoch,
		.workClass = request.workClass,
		.visible = request.visible,
	};
}

bool FrameCoordinatorModel::IsWorkOutstanding( const SurfaceSlot& slot ) noexcept
{
	return slot.pending.has_value() || slot.active.has_value();
}

void FrameCoordinatorModel::FinishActive( SurfaceSlot& slot )
{
	slot.active.reset();
	if( slot.closeRequested ) {
		slot.pending.reset();
		slot.closeRequested = false;
		slot.hasLastGoodContent = false;
		slot.phase = EFrameSurfacePhase::Closed;
		return;
	}
	slot.phase = slot.pending ? EFrameSurfacePhase::Requested : EFrameSurfacePhase::Idle;
}

} // namespace workbench::rendering
