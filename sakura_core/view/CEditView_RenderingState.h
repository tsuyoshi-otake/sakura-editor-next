/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/rendering/FrameSurfaceAdapter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace editor::rendering {

//! Editor panes are registered in a window-local presentation owner.  The id
//! is semantic (pane role + minimap role), never an HWND value; the lifetime
//! epoch fences a late completion after the native window is recreated.
inline constexpr workbench::rendering::FrameSurfaceId kEditorViewSurfaceIdBase =
	0x454449544F525000ULL; // "EDITORP"

[[nodiscard]] constexpr workbench::rendering::FrameSurfaceId EditorViewSurfaceId(
	const std::uint32_t paneIndex, const bool minimap) noexcept
{
	return kEditorViewSurfaceIdBase +
		(static_cast<workbench::rendering::FrameSurfaceId>(paneIndex) + 1ULL) * 2ULL +
		(minimap ? 1ULL : 0ULL);
}

//! Damage is intentionally a fixed bitset.  It does not grow with document
//! size, scrollback, or the number of invalidations received by the HWND.
enum class EEditViewDamage : std::uint8_t {
	BaseText = 1U << 0,
	Selection = 1U << 1,
	Caret = 1U << 2,
	Ime = 1U << 3,
	Scrollbar = 1U << 4,
	Minimap = 1U << 5,
};

using EditViewDamageMask = std::uint8_t;
inline constexpr EditViewDamageMask kEditViewDamageNone = 0;
inline constexpr EditViewDamageMask kEditViewDamageAll =
	static_cast<EditViewDamageMask>(EEditViewDamage::BaseText)
	| static_cast<EditViewDamageMask>(EEditViewDamage::Selection)
	| static_cast<EditViewDamageMask>(EEditViewDamage::Caret)
	| static_cast<EditViewDamageMask>(EEditViewDamage::Ime)
	| static_cast<EditViewDamageMask>(EEditViewDamage::Scrollbar)
	| static_cast<EditViewDamageMask>(EEditViewDamage::Minimap);

[[nodiscard]] constexpr EditViewDamageMask operator|(
	const EEditViewDamage left, const EEditViewDamage right) noexcept
{
	return static_cast<EditViewDamageMask>(left)
		| static_cast<EditViewDamageMask>(right);
}

[[nodiscard]] constexpr EditViewDamageMask operator|(
	const EditViewDamageMask left, const EEditViewDamage right) noexcept
{
	return left | static_cast<EditViewDamageMask>(right);
}

inline constexpr std::size_t kEditViewDamageLayerCount = 6;

using EditViewDamageGeneration =
	std::array<std::uint64_t, kEditViewDamageLayerCount>;

struct EditViewDamageSnapshot final {
	EditViewDamageMask pendingMask = kEditViewDamageNone;
	EditViewDamageGeneration generation{};
	EditViewDamageGeneration committedGeneration{};

	[[nodiscard]] bool HasPending() const noexcept { return pendingMask != 0; }
	[[nodiscard]] bool Has(EEditViewDamage damage) const noexcept
	{
		return (pendingMask & static_cast<EditViewDamageMask>(damage)) != 0;
	}
	[[nodiscard]] bool operator==(const EditViewDamageSnapshot&) const noexcept = default;
};

//! Immutable work identity carried from request to the post-GDI boundary.
struct EditViewRenderTicket final {
	workbench::rendering::FrameSurfaceAdapterTicket surface;
	EditViewDamageGeneration generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const EditViewRenderTicket&) const noexcept = default;
};

enum class EEditViewRenderPhase : std::uint8_t {
	Closed,
	Idle,
	Requested,
	Committed,
};

enum class EEditViewRenderStatus : std::uint8_t {
	Succeeded,
	Replaced,
	NoDamage,
	Stale,
	Invalid,
	Closed,
	Busy,
	Exhausted,
	PaintFailed,
};

struct EditViewRenderResult final {
	EEditViewRenderStatus status = EEditViewRenderStatus::Invalid;
	EEditViewRenderPhase phase = EEditViewRenderPhase::Closed;
	std::optional<EditViewRenderTicket> ticket;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == EEditViewRenderStatus::Succeeded
			|| status == EEditViewRenderStatus::Replaced;
	}
};

//! The viewport identity used to fence a cooperative paint continuation.
//!
//! A cursor is only reusable while the document/layout generation and the
//! visible range are unchanged.  This prevents a cursor into a previous
//! layout from silently skipping text after a scroll, resize, or edit.
struct EditViewPaintViewport final {
	std::uint64_t contentGeneration{};
	std::uint64_t layoutEpoch{};
	std::int64_t layoutTop{};
	std::int64_t layoutBottom{};
	std::int64_t viewLeftColumn{};
	std::int64_t viewRightColumn{};

	[[nodiscard]] bool operator==(const EditViewPaintViewport& rhs) const noexcept
	{
		return contentGeneration == rhs.contentGeneration
			&& layoutEpoch == rhs.layoutEpoch
			&& layoutTop == rhs.layoutTop
			&& layoutBottom == rhs.layoutBottom
			&& viewLeftColumn == rhs.viewLeftColumn
			&& viewRightColumn == rhs.viewRightColumn;
	}
};

//! A resumable position inside one logical/layout line.
//!
//! `logicOffset` is a UTF-16 code-unit offset and `drawColumn` is the layout
//! column after the last painted unit.  Keeping both is intentional: width
//! calculation is not reversible for tabs, surrogate pairs, or custom
//! figures, so the next paint must not recompute it from the source text.
struct EditViewPaintCursor final {
	EditViewPaintViewport viewport{};
	std::int64_t layoutLine{};
	std::int64_t logicOffset{};
	std::int64_t drawColumn{};

	[[nodiscard]] bool operator==(const EditViewPaintCursor& rhs) const noexcept
	{
		return viewport == rhs.viewport
			&& layoutLine == rhs.layoutLine
			&& logicOffset == rhs.logicOffset
			&& drawColumn == rhs.drawColumn;
	}
};

//! State returned by a paint's explicit work budget.
struct EditViewPaintQuantumSnapshot final {
	std::size_t limit{};
	std::size_t consumed{};
	std::size_t remaining{};

	[[nodiscard]] bool Exhausted() const noexcept { return remaining == 0; }
};

struct EditViewRenderSnapshot final {
	workbench::rendering::FrameSurfaceAdapterSnapshot surface;
	EditViewDamageSnapshot damage;
	bool hasLastGoodBitmap = false;
	std::uint64_t lastCommittedPaintBoundary = 0;
	std::optional<EditViewPaintCursor> paintCursor;
	std::size_t paintQuantumRemaining = 0;
	std::size_t paintQuantumConsumed = 0;
};

//! HWND/GDI-free state for one retained CEditView presentation island.
//!
//! The native view owns the bitmap and calls CommitGdiFrame only after the
//! corresponding EndPaint/ReleaseDC boundary.  This class never waits, calls
//! User32, starts a worker, or retains pixel payloads.  New damage replaces
//! pending work, while the last-good bitmap projection remains authoritative
//! until a newer ticket has actually crossed that boundary.
class CEditViewRenderState final {
public:
	//! A giant logical line is processed in bounded cooperative quanta.  The
	//! caller can carry the remainder to another paint without allocating work
	//! proportional to the line's total length.
	static constexpr std::size_t kMaximumLineWorkItems = 4096;
	//! The production paint budget is explicit and independent of wall-clock
	//! time, timer cadence, or the amount of text in the document.
	static constexpr std::size_t kPaintQuantum = kMaximumLineWorkItems;

	explicit CEditViewRenderState(
		workbench::rendering::FrameSurfaceId stableSurfaceId) noexcept;
	~CEditViewRenderState() = default;
	CEditViewRenderState(const CEditViewRenderState&) = delete;
	CEditViewRenderState& operator=(const CEditViewRenderState&) = delete;

	[[nodiscard]] EditViewRenderResult Open(
		std::string_view hostId, bool visible,
		std::uint64_t surfaceLifetimeEpoch = 0,
		std::uint64_t layoutEpoch = 1,
		std::uint64_t deviceEpoch = 1,
		std::uint64_t contentGeneration = 1) noexcept;
	[[nodiscard]] EditViewRenderResult Close() noexcept;
	[[nodiscard]] EditViewRenderResult SetHost(std::string_view hostId) noexcept;
	[[nodiscard]] EditViewRenderResult SetVisible(bool visible) noexcept;

	//! Epoch changes withdraw only pending work; the committed projection stays.
	[[nodiscard]] EditViewRenderResult NotifyLayout() noexcept;
	[[nodiscard]] EditViewRenderResult NotifyLayoutEpoch(std::uint64_t epoch) noexcept;
	[[nodiscard]] EditViewRenderResult NotifyDeviceEpoch(std::uint64_t epoch) noexcept;
	[[nodiscard]] EditViewRenderResult NotifyContentGeneration(
		std::uint64_t generation) noexcept;

	//! Marks one or more independent bounded layers dirty.  The aggregate
	//! content generation advances once per notification to fence old tickets.
	[[nodiscard]] EditViewRenderResult MarkDamage(EditViewDamageMask mask) noexcept;
	[[nodiscard]] EditViewRenderResult MarkDamage(EEditViewDamage damage) noexcept
	{
		return MarkDamage(static_cast<EditViewDamageMask>(damage));
	}

	//! Requests exactly one latest-only ticket.  No request performs painting.
	[[nodiscard]] EditViewRenderResult RequestFrame() noexcept;
	[[nodiscard]] bool IsCurrent(const EditViewRenderTicket& ticket) const noexcept;

	//! The overload taking a ticket is the explicit GDI boundary operation.  A
	//! failed paint leaves the ticket pending and preserves last-good content.
	[[nodiscard]] EditViewRenderResult CommitGdiFrame(
		const EditViewRenderTicket& ticket, bool paintSucceeded = true) noexcept;
	[[nodiscard]] std::optional<EditViewRenderSnapshot> CommitGdiFrame(
		bool paintSucceeded = true) noexcept;

	//! Starts one bounded paint turn for the supplied visible viewport.  If a
	//! cursor from the previous turn belongs to another viewport it is dropped;
	//! no source text is dropped and the next turn starts at the viewport top.
	[[nodiscard]] EditViewPaintQuantumSnapshot BeginPaintQuantum(
		const EditViewPaintViewport& viewport) noexcept;
	[[nodiscard]] std::optional<EditViewPaintCursor> PaintCursor() const noexcept;
	[[nodiscard]] bool IsPaintCursorFor(std::int64_t layoutLine) const noexcept;
	[[nodiscard]] bool HasPaintContinuation() const noexcept
	{
		return m_paintCursor.has_value();
	}
	[[nodiscard]] std::size_t PaintQuantumRemaining() const noexcept
	{
		return m_paintQuantumRemaining;
	}
	[[nodiscard]] std::size_t PaintQuantumConsumed() const noexcept
	{
		return m_paintQuantumConsumed;
	}
	//! Consumes explicit line work.  It returns false when the turn is already
	//! exhausted; callers must save their cursor and yield to the next paint.
	[[nodiscard]] bool ConsumePaintWork(std::size_t units = 1) noexcept;
	void SavePaintCursor(
		std::int64_t layoutLine,
		std::int64_t logicOffset,
		std::int64_t drawColumn) noexcept;
	void CompletePaintCursor() noexcept;
	void CancelPaintContinuation() noexcept;

	[[nodiscard]] EditViewRenderSnapshot Snapshot() const noexcept;
	[[nodiscard]] workbench::rendering::FrameSurfaceId StableSurfaceId() const noexcept
	{
		return m_surface.StableSurfaceId();
	}
	[[nodiscard]] bool IsOpen() const noexcept { return m_surface.IsOpen(); }
	[[nodiscard]] bool HasPendingFrame() const noexcept { return m_pending.has_value(); }
	[[nodiscard]] EditViewDamageGeneration DamageGenerations() const noexcept
	{
		return m_damageGeneration;
	}
	[[nodiscard]] static constexpr std::size_t CapLineWork(
		const std::size_t requested) noexcept
	{
		return requested < kMaximumLineWorkItems ? requested : kMaximumLineWorkItems;
	}

private:
	[[nodiscard]] EditViewRenderResult Result(
		EEditViewRenderStatus status) const noexcept;
	[[nodiscard]] EditViewRenderResult MapSurfaceResult(
		const workbench::rendering::FrameSurfaceAdapterResult& result) const noexcept;
	[[nodiscard]] bool AdvanceDamage(EditViewDamageMask mask) noexcept;
	void SetAllDamage() noexcept;
	void WithdrawPending() noexcept;

	workbench::rendering::FrameSurfaceAdapter m_surface;
	std::uint64_t m_lastLifetimeEpoch = 0;
	std::uint64_t m_layoutEpoch = 1;
	std::uint64_t m_deviceEpoch = 1;
	std::uint64_t m_contentGeneration = 1;
	std::uint64_t m_requestId = 0;
	std::optional<EditViewRenderTicket> m_pending;
	EditViewDamageGeneration m_damageGeneration{};
	EditViewDamageGeneration m_committedDamageGeneration{};
	EditViewDamageMask m_pendingDamage = kEditViewDamageNone;
	std::uint64_t m_lastCommittedPaintBoundary = 0;
	std::optional<EditViewPaintViewport> m_paintViewport;
	std::optional<EditViewPaintCursor> m_paintCursor;
	std::size_t m_paintQuantumRemaining = 0;
	std::size_t m_paintQuantumConsumed = 0;
};

} // namespace editor::rendering
