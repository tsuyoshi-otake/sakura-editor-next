/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/rendering/FrameSurfaceAdapter.h"

#include <limits>
#include <utility>

namespace workbench::rendering {
namespace {

[[nodiscard]] bool IsValidEpochs(
	std::uint64_t surfaceLifetimeEpoch, std::uint64_t contentGeneration,
	std::uint64_t layoutEpoch, std::uint64_t deviceEpoch ) noexcept
{
	return surfaceLifetimeEpoch != 0 && contentGeneration != 0
		&& layoutEpoch != 0 && deviceEpoch != 0;
}

[[nodiscard]] FrameSurfaceAdapterResult Result(
	EFrameSurfaceAdapterStatus status, EFrameSurfaceAdapterPhase phase ) noexcept
{
	return { .status = status, .phase = phase };
}

} // namespace

bool FrameSurfaceAdapterRequest::IsValid() const noexcept
{
	return surfaceId != 0 && !hostId.empty()
		&& surfaceLifetimeEpoch != 0 && requestId != 0
		&& contentGeneration != 0 && layoutEpoch != 0 && deviceEpoch != 0
		&& hostEpoch != 0 && visibilityEpoch != 0;
}

bool FrameSurfaceAdapterTicket::IsValid() const noexcept
{
	return surfaceId != 0 && !hostId.empty()
		&& surfaceLifetimeEpoch != 0 && requestId != 0
		&& contentGeneration != 0 && layoutEpoch != 0 && deviceEpoch != 0
		&& hostEpoch != 0 && visibilityEpoch != 0;
}

FrameSurfaceAdapter::FrameSurfaceAdapter( FrameSurfaceId stableSurfaceId ) noexcept
	: m_surfaceId(stableSurfaceId)
{
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::Open(
	std::string_view hostId, bool visible,
	std::uint64_t surfaceLifetimeEpoch, std::uint64_t layoutEpoch,
	std::uint64_t deviceEpoch, std::uint64_t contentGeneration )
{
	if( m_surfaceId == 0 || hostId.empty()
		|| !IsValidEpochs(surfaceLifetimeEpoch, contentGeneration, layoutEpoch, deviceEpoch) ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( m_phase != EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Busy, m_phase);
	}
	if( surfaceLifetimeEpoch <= m_lastLifetimeEpoch ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}

	m_hostId.assign(hostId);
	m_visible = visible;
	m_surfaceLifetimeEpoch = surfaceLifetimeEpoch;
	m_lastLifetimeEpoch = surfaceLifetimeEpoch;
	m_layoutEpoch = layoutEpoch;
	m_deviceEpoch = deviceEpoch;
	m_contentGeneration = contentGeneration;
	m_newestRequestId = 0;
	m_committedRequestId = 0;
	m_hostEpoch = 1;
	m_visibilityEpoch = 1;
	m_latestRequest.reset();
	m_hasLastGoodContent = false;
	m_phase = EFrameSurfaceAdapterPhase::Idle;
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::Close(
	std::uint64_t surfaceLifetimeEpoch ) noexcept
{
	if( surfaceLifetimeEpoch == 0 ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( surfaceLifetimeEpoch != m_surfaceLifetimeEpoch ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}

	m_latestRequest.reset();
	m_hostId.clear();
	m_visible = false;
	m_hasLastGoodContent = false;
	m_phase = EFrameSurfaceAdapterPhase::Closed;
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::SetHost( std::string_view hostId )
{
	if( hostId.empty() ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}
	if( m_hostId == hostId ) {
		return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
	}
	if( m_hostEpoch == (std::numeric_limits<std::uint64_t>::max)() ) {
		return Result(EFrameSurfaceAdapterStatus::Exhausted, m_phase);
	}

	m_hostId.assign(hostId);
	++m_hostEpoch;
	WithdrawRequest();
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::SetVisible( bool visible ) noexcept
{
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}
	if( m_visible == visible ) {
		return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
	}
	if( m_visibilityEpoch == (std::numeric_limits<std::uint64_t>::max)() ) {
		return Result(EFrameSurfaceAdapterStatus::Exhausted, m_phase);
	}

	m_visible = visible;
	++m_visibilityEpoch;
	WithdrawRequest();
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::UpdateEpochs(
	std::uint64_t contentGeneration, std::uint64_t layoutEpoch,
	std::uint64_t deviceEpoch ) noexcept
{
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}
	if( !IsValidEpochs(m_surfaceLifetimeEpoch, contentGeneration, layoutEpoch, deviceEpoch) ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( contentGeneration < m_contentGeneration || layoutEpoch < m_layoutEpoch
		|| deviceEpoch < m_deviceEpoch ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}
	if( contentGeneration == m_contentGeneration && layoutEpoch == m_layoutEpoch
		&& deviceEpoch == m_deviceEpoch ) {
		return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
	}

	m_contentGeneration = contentGeneration;
	m_layoutEpoch = layoutEpoch;
	m_deviceEpoch = deviceEpoch;
	WithdrawRequest();
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::Request(
	const FrameSurfaceAdapterRequest& request )
{
	if( request.surfaceId != m_surfaceId ) {
		return Result(EFrameSurfaceAdapterStatus::UnknownSurface, m_phase);
	}
	if( !request.IsValid() ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}
	if( m_newestRequestId == (std::numeric_limits<std::uint64_t>::max)() ) {
		return Result(EFrameSurfaceAdapterStatus::Exhausted, m_phase);
	}
	if( !MatchesCurrentProjection(request) ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}
	if( request.requestId <= m_newestRequestId ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}

	const bool replaced = m_phase == EFrameSurfaceAdapterPhase::Requested
		&& m_latestRequest.has_value();
	m_latestRequest = MakeTicket(request);
	m_newestRequestId = request.requestId;
	m_phase = EFrameSurfaceAdapterPhase::Requested;
	return {
		.status = replaced ? EFrameSurfaceAdapterStatus::Replaced : EFrameSurfaceAdapterStatus::Succeeded,
		.phase = m_phase,
		.ticket = m_latestRequest,
	};
}

FrameSurfaceAdapterResult FrameSurfaceAdapter::Commit(
	const FrameSurfaceAdapterTicket& ticket ) noexcept
{
	if( !ticket.IsValid() ) {
		return Result(EFrameSurfaceAdapterStatus::Invalid, m_phase);
	}
	if( ticket.surfaceId != m_surfaceId ) {
		return Result(EFrameSurfaceAdapterStatus::UnknownSurface, m_phase);
	}
	if( m_phase == EFrameSurfaceAdapterPhase::Closed ) {
		return Result(EFrameSurfaceAdapterStatus::Closed, m_phase);
	}
	if( !MatchesCurrentProjection(ticket) || !m_latestRequest
		|| *m_latestRequest != ticket
		|| m_phase != EFrameSurfaceAdapterPhase::Requested ) {
		return Result(EFrameSurfaceAdapterStatus::Stale, m_phase);
	}

	m_committedRequestId = ticket.requestId;
	m_hasLastGoodContent = true;
	m_phase = EFrameSurfaceAdapterPhase::Committed;
	return Result(EFrameSurfaceAdapterStatus::Succeeded, m_phase);
}

bool FrameSurfaceAdapter::IsCurrent(
	const FrameSurfaceAdapterTicket& ticket ) const noexcept
{
	return ticket.IsValid() && m_phase != EFrameSurfaceAdapterPhase::Closed
		&& m_phase == EFrameSurfaceAdapterPhase::Requested
		&& m_latestRequest && *m_latestRequest == ticket
		&& MatchesCurrentProjection(ticket);
}

FrameSurfaceAdapterSnapshot FrameSurfaceAdapter::Snapshot() const
{
	return {
		.surfaceId = m_surfaceId,
		.hostId = m_hostId,
		.phase = m_phase,
		.surfaceLifetimeEpoch = m_surfaceLifetimeEpoch,
		.layoutEpoch = m_layoutEpoch,
		.deviceEpoch = m_deviceEpoch,
		.contentGeneration = m_contentGeneration,
		.newestRequestId = m_newestRequestId,
		.committedRequestId = m_committedRequestId,
		.visible = m_visible,
		.hasLastGoodContent = m_hasLastGoodContent,
		.hostEpoch = m_hostEpoch,
		.visibilityEpoch = m_visibilityEpoch,
	};
}

FrameSurfaceAdapterTicket FrameSurfaceAdapter::MakeTicket(
	const FrameSurfaceAdapterRequest& request )
{
	return {
		.surfaceId = request.surfaceId,
		.hostId = request.hostId,
		.surfaceLifetimeEpoch = request.surfaceLifetimeEpoch,
		.requestId = request.requestId,
		.contentGeneration = request.contentGeneration,
		.layoutEpoch = request.layoutEpoch,
		.deviceEpoch = request.deviceEpoch,
		.workClass = request.workClass,
		.visible = request.visible,
		.hostEpoch = request.hostEpoch,
		.visibilityEpoch = request.visibilityEpoch,
	};
}

bool FrameSurfaceAdapter::MatchesCurrentProjection(
	const FrameSurfaceAdapterRequest& request ) const noexcept
{
	return request.surfaceLifetimeEpoch == m_surfaceLifetimeEpoch
		&& request.contentGeneration == m_contentGeneration
		&& request.layoutEpoch == m_layoutEpoch
		&& request.deviceEpoch == m_deviceEpoch
		&& request.hostId == m_hostId
		&& request.hostEpoch == m_hostEpoch
		&& request.visible == m_visible
		&& request.visibilityEpoch == m_visibilityEpoch;
}

bool FrameSurfaceAdapter::MatchesCurrentProjection(
	const FrameSurfaceAdapterTicket& ticket ) const noexcept
{
	return ticket.surfaceLifetimeEpoch == m_surfaceLifetimeEpoch
		&& ticket.contentGeneration == m_contentGeneration
		&& ticket.layoutEpoch == m_layoutEpoch
		&& ticket.deviceEpoch == m_deviceEpoch
		&& ticket.hostId == m_hostId
		&& ticket.hostEpoch == m_hostEpoch
		&& ticket.visible == m_visible
		&& ticket.visibilityEpoch == m_visibilityEpoch;
}

void FrameSurfaceAdapter::WithdrawRequest() noexcept
{
	m_latestRequest.reset();
	m_phase = m_hasLastGoodContent
		? EFrameSurfaceAdapterPhase::Committed
		: EFrameSurfaceAdapterPhase::Idle;
}

} // namespace workbench::rendering
