/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "view/CEditView_RenderingState.h"

#include <limits>

namespace editor::rendering {
namespace {

[[nodiscard]] constexpr EEditViewRenderPhase ToEditPhase(
	const workbench::rendering::EFrameSurfaceAdapterPhase phase) noexcept
{
	switch( phase ) {
	case workbench::rendering::EFrameSurfaceAdapterPhase::Closed:
		return EEditViewRenderPhase::Closed;
	case workbench::rendering::EFrameSurfaceAdapterPhase::Idle:
		return EEditViewRenderPhase::Idle;
	case workbench::rendering::EFrameSurfaceAdapterPhase::Requested:
		return EEditViewRenderPhase::Requested;
	case workbench::rendering::EFrameSurfaceAdapterPhase::Committed:
		return EEditViewRenderPhase::Committed;
	}
	return EEditViewRenderPhase::Closed;
}

[[nodiscard]] constexpr EEditViewRenderStatus ToEditStatus(
	const workbench::rendering::EFrameSurfaceAdapterStatus status) noexcept
{
	switch( status ) {
	case workbench::rendering::EFrameSurfaceAdapterStatus::Succeeded:
		return EEditViewRenderStatus::Succeeded;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Replaced:
		return EEditViewRenderStatus::Replaced;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Stale:
		return EEditViewRenderStatus::Stale;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Invalid:
	case workbench::rendering::EFrameSurfaceAdapterStatus::UnknownSurface:
		return EEditViewRenderStatus::Invalid;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Busy:
		return EEditViewRenderStatus::Busy;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Closed:
		return EEditViewRenderStatus::Closed;
	case workbench::rendering::EFrameSurfaceAdapterStatus::Exhausted:
		return EEditViewRenderStatus::Exhausted;
	}
	return EEditViewRenderStatus::Invalid;
}

[[nodiscard]] constexpr std::size_t DamageIndex(
	const EEditViewDamage damage) noexcept
{
	switch( damage ) {
	case EEditViewDamage::BaseText:
		return 0;
	case EEditViewDamage::Selection:
		return 1;
	case EEditViewDamage::Caret:
		return 2;
	case EEditViewDamage::Ime:
		return 3;
	case EEditViewDamage::Scrollbar:
		return 4;
	case EEditViewDamage::Minimap:
		return 5;
	}
	return kEditViewDamageLayerCount;
}

[[nodiscard]] constexpr bool IsDamageMaskValid(
	const EditViewDamageMask mask) noexcept
{
	return (mask & static_cast<EditViewDamageMask>(~kEditViewDamageAll)) == 0;
}

[[nodiscard]] constexpr bool IsAtMaximum(const std::uint64_t value) noexcept
{
	return value == (std::numeric_limits<std::uint64_t>::max)();
}

} // namespace

bool EditViewRenderTicket::IsValid() const noexcept
{
	return surface.IsValid();
}

CEditViewRenderState::CEditViewRenderState(
	const workbench::rendering::FrameSurfaceId stableSurfaceId) noexcept
	: m_surface(stableSurfaceId)
{
	m_damageGeneration.fill(0);
	m_committedDamageGeneration.fill(0);
}

EditViewRenderResult CEditViewRenderState::Result(
	const EEditViewRenderStatus status) const noexcept
{
	return {
		.status = status,
		.phase = ToEditPhase(m_surface.Snapshot().phase),
	};
}

EditViewRenderResult CEditViewRenderState::MapSurfaceResult(
	const workbench::rendering::FrameSurfaceAdapterResult& result) const noexcept
{
	EditViewRenderResult mapped{
		.status = ToEditStatus(result.status),
		.phase = ToEditPhase(result.phase),
	};
	if( result.ticket ) {
		EditViewRenderTicket ticket{
			.surface = *result.ticket,
			.generation = m_damageGeneration,
		};
		mapped.ticket = ticket;
	}
	return mapped;
}

EditViewRenderResult CEditViewRenderState::Open(
	const std::string_view hostId, const bool visible,
	std::uint64_t surfaceLifetimeEpoch, const std::uint64_t layoutEpoch,
	const std::uint64_t deviceEpoch,
	const std::uint64_t contentGeneration) noexcept
{
	if( surfaceLifetimeEpoch == 0 ) {
		if( IsAtMaximum(m_lastLifetimeEpoch) ) {
			return Result(EEditViewRenderStatus::Exhausted);
		}
		surfaceLifetimeEpoch = m_lastLifetimeEpoch + 1;
	}
	const auto result = m_surface.Open(
		hostId, visible, surfaceLifetimeEpoch, layoutEpoch, deviceEpoch,
		contentGeneration);
	if( !result.Accepted() ) {
		return MapSurfaceResult(result);
	}

	m_lastLifetimeEpoch = surfaceLifetimeEpoch;
	m_layoutEpoch = layoutEpoch;
	m_deviceEpoch = deviceEpoch;
	m_contentGeneration = contentGeneration;
	m_requestId = 0;
	m_pending.reset();
	m_damageGeneration.fill(1);
	m_committedDamageGeneration.fill(0);
	m_pendingDamage = kEditViewDamageAll;
	m_lastCommittedPaintBoundary = 0;
	CancelPaintContinuation();
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::Close() noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	const auto result = m_surface.Close(m_lastLifetimeEpoch);
	if( result.Accepted() ) {
		WithdrawPending();
		m_pendingDamage = kEditViewDamageNone;
		m_damageGeneration.fill(0);
		m_committedDamageGeneration.fill(0);
		CancelPaintContinuation();
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::SetHost(
	const std::string_view hostId) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	const auto before = m_surface.Snapshot();
	const auto result = m_surface.SetHost(hostId);
	if( result.Accepted() && before.hostId != hostId ) {
		WithdrawPending();
		SetAllDamage();
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::SetVisible(
	const bool visible) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	const auto before = m_surface.Snapshot();
	const auto result = m_surface.SetVisible(visible);
	if( result.Accepted() && before.visible != visible ) {
		WithdrawPending();
		SetAllDamage();
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::NotifyLayout() noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( IsAtMaximum(m_layoutEpoch) ) {
		return Result(EEditViewRenderStatus::Exhausted);
	}
	return NotifyLayoutEpoch(m_layoutEpoch + 1);
}

EditViewRenderResult CEditViewRenderState::NotifyLayoutEpoch(
	const std::uint64_t epoch) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( epoch == 0 ) {
		return Result(EEditViewRenderStatus::Invalid);
	}
	if( epoch < m_layoutEpoch ) {
		return Result(EEditViewRenderStatus::Stale);
	}
	if( epoch == m_layoutEpoch ) {
		return Result(EEditViewRenderStatus::Succeeded);
	}
	const auto result = m_surface.UpdateEpochs(
		m_contentGeneration, epoch, m_deviceEpoch);
	if( result.Accepted() ) {
		m_layoutEpoch = epoch;
		WithdrawPending();
		SetAllDamage();
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::NotifyDeviceEpoch(
	const std::uint64_t epoch) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( epoch == 0 ) {
		return Result(EEditViewRenderStatus::Invalid);
	}
	if( epoch < m_deviceEpoch ) {
		return Result(EEditViewRenderStatus::Stale);
	}
	if( epoch == m_deviceEpoch ) {
		return Result(EEditViewRenderStatus::Succeeded);
	}
	const auto result = m_surface.UpdateEpochs(
		m_contentGeneration, m_layoutEpoch, epoch);
	if( result.Accepted() ) {
		m_deviceEpoch = epoch;
		WithdrawPending();
		SetAllDamage();
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::NotifyContentGeneration(
	const std::uint64_t generation) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( generation == 0 ) {
		return Result(EEditViewRenderStatus::Invalid);
	}
	if( generation < m_contentGeneration ) {
		return Result(EEditViewRenderStatus::Stale);
	}
	if( generation == m_contentGeneration ) {
		return Result(EEditViewRenderStatus::Succeeded);
	}
	if( IsAtMaximum(m_damageGeneration[DamageIndex(EEditViewDamage::BaseText)]) ) {
		return Result(EEditViewRenderStatus::Exhausted);
	}
	const auto result = m_surface.UpdateEpochs(
		generation, m_layoutEpoch, m_deviceEpoch);
	if( result.Accepted() ) {
		m_contentGeneration = generation;
		WithdrawPending();
		(void)AdvanceDamage(static_cast<EditViewDamageMask>(EEditViewDamage::BaseText));
		m_pendingDamage = static_cast<EditViewDamageMask>(
			m_pendingDamage | static_cast<EditViewDamageMask>(EEditViewDamage::BaseText));
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::MarkDamage(
	const EditViewDamageMask mask) noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( !IsDamageMaskValid(mask) ) {
		return Result(EEditViewRenderStatus::Invalid);
	}
	if( mask == kEditViewDamageNone ) {
		return Result(EEditViewRenderStatus::NoDamage);
	}
	if( IsAtMaximum(m_contentGeneration) ) {
		return Result(EEditViewRenderStatus::Exhausted);
	}
	for( std::size_t index = 0; index < kEditViewDamageLayerCount; ++index ) {
		const auto bit = static_cast<EditViewDamageMask>(1U << index);
		if( (mask & bit) != 0 && IsAtMaximum(m_damageGeneration[index]) ) {
			return Result(EEditViewRenderStatus::Exhausted);
		}
	}

	const auto result = m_surface.UpdateEpochs(
		m_contentGeneration + 1, m_layoutEpoch, m_deviceEpoch);
	if( result.Accepted() ) {
		++m_contentGeneration;
		WithdrawPending();
		(void)AdvanceDamage(mask);
		m_pendingDamage = static_cast<EditViewDamageMask>(m_pendingDamage | mask);
	}
	return MapSurfaceResult(result);
}

EditViewRenderResult CEditViewRenderState::RequestFrame() noexcept
{
	if( !m_surface.IsOpen() ) {
		return Result(EEditViewRenderStatus::Closed);
	}
	if( m_pendingDamage == kEditViewDamageNone ) {
		return Result(EEditViewRenderStatus::NoDamage);
	}
	if( IsAtMaximum(m_requestId) ) {
		return Result(EEditViewRenderStatus::Exhausted);
	}

	const auto surfaceSnapshot = m_surface.Snapshot();
	workbench::rendering::FrameSurfaceAdapterRequest request{
		.surfaceId = surfaceSnapshot.surfaceId,
		.hostId = surfaceSnapshot.hostId,
		.surfaceLifetimeEpoch = surfaceSnapshot.surfaceLifetimeEpoch,
		.requestId = m_requestId + 1,
		.contentGeneration = surfaceSnapshot.contentGeneration,
		.layoutEpoch = surfaceSnapshot.layoutEpoch,
		.deviceEpoch = surfaceSnapshot.deviceEpoch,
		.workClass = surfaceSnapshot.visible
			? workbench::rendering::EFrameWorkClass::Interactive
			: workbench::rendering::EFrameWorkClass::Background,
		.visible = surfaceSnapshot.visible,
		.hostEpoch = surfaceSnapshot.hostEpoch,
		.visibilityEpoch = surfaceSnapshot.visibilityEpoch,
	};
	const auto result = m_surface.Request(request);
	if( result.Accepted() ) {
		m_requestId = request.requestId;
		const auto mapped = MapSurfaceResult(result);
		if( mapped.ticket ) {
			m_pending = *mapped.ticket;
		}
		return mapped;
	}
	return MapSurfaceResult(result);
}

bool CEditViewRenderState::IsCurrent(
	const EditViewRenderTicket& ticket) const noexcept
{
	return ticket.IsValid() && m_pending && *m_pending == ticket
		&& m_pendingDamage != kEditViewDamageNone
		&& m_surface.IsCurrent(ticket.surface)
		&& ticket.generation == m_damageGeneration;
}

EditViewRenderResult CEditViewRenderState::CommitGdiFrame(
	const EditViewRenderTicket& ticket, const bool paintSucceeded) noexcept
{
	if( !ticket.IsValid() ) {
		return Result(EEditViewRenderStatus::Invalid);
	}
	if( !IsCurrent(ticket) ) {
		const auto phase = ToEditPhase(m_surface.Snapshot().phase);
		if( phase == EEditViewRenderPhase::Closed ) {
			return Result(EEditViewRenderStatus::Closed);
		}
		return Result(EEditViewRenderStatus::Stale);
	}
	if( !paintSucceeded ) {
		return Result(EEditViewRenderStatus::PaintFailed);
	}

	const auto result = m_surface.Commit(ticket.surface);
	if( result.Accepted() ) {
		// A cooperative turn may have painted only a prefix of a giant line.
		// Commit the GDI boundary, but keep the damage pending until the cursor
		// reaches the line end so an incomplete frame is never advertised as the
		// newest complete content.
		const bool hasContinuation = HasPaintContinuation();
		if( !hasContinuation ) {
			m_committedDamageGeneration = ticket.generation;
			m_pendingDamage = kEditViewDamageNone;
		}
		m_pending.reset();
		if( !IsAtMaximum(m_lastCommittedPaintBoundary) ) {
			++m_lastCommittedPaintBoundary;
		}
	}
	return MapSurfaceResult(result);
}

std::optional<EditViewRenderSnapshot> CEditViewRenderState::CommitGdiFrame(
	const bool paintSucceeded) noexcept
{
	if( !m_pending ) {
		return std::nullopt;
	}
	const auto ticket = *m_pending;
	const auto result = CommitGdiFrame(ticket, paintSucceeded);
	if( !result.Accepted() ) {
		return std::nullopt;
	}
	return Snapshot();
}

EditViewRenderSnapshot CEditViewRenderState::Snapshot() const noexcept
{
	const auto surface = m_surface.Snapshot();
	return {
		.surface = surface,
		.damage = {
			.pendingMask = m_pendingDamage,
			.generation = m_damageGeneration,
			.committedGeneration = m_committedDamageGeneration,
		},
		.hasLastGoodBitmap = surface.hasLastGoodContent,
		.lastCommittedPaintBoundary = m_lastCommittedPaintBoundary,
		.paintCursor = m_paintCursor,
		.paintQuantumRemaining = m_paintQuantumRemaining,
		.paintQuantumConsumed = m_paintQuantumConsumed,
	};
}

EditViewPaintQuantumSnapshot CEditViewRenderState::BeginPaintQuantum(
	const EditViewPaintViewport& viewport) noexcept
{
	if( !m_paintViewport || *m_paintViewport != viewport ) {
		m_paintViewport = viewport;
		m_paintCursor.reset();
	}
	m_paintQuantumRemaining = kPaintQuantum;
	m_paintQuantumConsumed = 0;
	return {
		.limit = kPaintQuantum,
		.consumed = m_paintQuantumConsumed,
		.remaining = m_paintQuantumRemaining,
	};
}

std::optional<EditViewPaintCursor> CEditViewRenderState::PaintCursor() const noexcept
{
	return m_paintCursor;
}

bool CEditViewRenderState::IsPaintCursorFor(
	const std::int64_t layoutLine) const noexcept
{
	return m_paintCursor && m_paintCursor->layoutLine == layoutLine;
}

bool CEditViewRenderState::ConsumePaintWork(const std::size_t units) noexcept
{
	if( units == 0 ) {
		return m_paintQuantumRemaining != 0;
	}
	if( units > m_paintQuantumRemaining ) {
		m_paintQuantumConsumed += m_paintQuantumRemaining;
		m_paintQuantumRemaining = 0;
		return false;
	}
	m_paintQuantumRemaining -= units;
	m_paintQuantumConsumed += units;
	return true;
}

void CEditViewRenderState::SavePaintCursor(
	const std::int64_t layoutLine,
	const std::int64_t logicOffset,
	const std::int64_t drawColumn) noexcept
{
	if( !m_paintViewport ) {
		return;
	}
	m_paintCursor = EditViewPaintCursor{
		.viewport = *m_paintViewport,
		.layoutLine = layoutLine,
		.logicOffset = logicOffset,
		.drawColumn = drawColumn,
	};
}

void CEditViewRenderState::CompletePaintCursor() noexcept
{
	m_paintCursor.reset();
}

void CEditViewRenderState::CancelPaintContinuation() noexcept
{
	m_paintViewport.reset();
	m_paintCursor.reset();
	m_paintQuantumRemaining = 0;
	m_paintQuantumConsumed = 0;
}

bool CEditViewRenderState::AdvanceDamage(const EditViewDamageMask mask) noexcept
{
	if( !IsDamageMaskValid(mask) ) {
		return false;
	}
	for( std::size_t index = 0; index < kEditViewDamageLayerCount; ++index ) {
		const auto bit = static_cast<EditViewDamageMask>(1U << index);
		if( (mask & bit) != 0 && IsAtMaximum(m_damageGeneration[index]) ) {
			return false;
		}
	}
	for( std::size_t index = 0; index < kEditViewDamageLayerCount; ++index ) {
		const auto bit = static_cast<EditViewDamageMask>(1U << index);
		if( (mask & bit) != 0 ) {
			++m_damageGeneration[index];
		}
	}
	return true;
}

void CEditViewRenderState::SetAllDamage() noexcept
{
	if( AdvanceDamage(kEditViewDamageAll) ) {
		m_pendingDamage = kEditViewDamageAll;
		return;
	}
	// Saturation is still bounded and keeps the projection dirty.  The caller
	// will observe Exhausted from the next mutation instead of wrapping a
	// generation and accepting an old ticket.
	m_pendingDamage = kEditViewDamageAll;
}

void CEditViewRenderState::WithdrawPending() noexcept
{
	m_pending.reset();
	CancelPaintContinuation();
}

} // namespace editor::rendering
