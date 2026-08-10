/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>

namespace editor::selection {

//! The semantic selection mode.  Window/input adapters translate native gestures into these values.
enum class ESelectionMode : std::uint8_t {
	Linear,
	Box,
	Line,
	Word,
	Nazo,
};

//! Terminal result for a mode/phase transition.
enum class ESelectionTransition : std::uint8_t {
	Started,
	Restarted,
	Ended,
	ModeChanged,
	Noop,
};

//! A presentation-neutral point in the selection coordinate space.
class SelectionPoint final {
public:
	constexpr SelectionPoint() noexcept = default;
	constexpr SelectionPoint(std::int32_t x, std::int32_t y) noexcept
		: m_x(x)
		, m_y(y)
	{
	}

	[[nodiscard]] constexpr std::int32_t X() const noexcept { return m_x; }
	[[nodiscard]] constexpr std::int32_t Y() const noexcept { return m_y; }

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return m_x >= 0 && m_y >= 0;
	}

private:
	std::int32_t m_x = -1;
	std::int32_t m_y = -1;
};

[[nodiscard]] constexpr bool operator==(const SelectionPoint& lhs, const SelectionPoint& rhs) noexcept
{
	return lhs.X() == rhs.X() && lhs.Y() == rhs.Y();
}

//! A presentation-neutral selection range.
class SelectionRange final {
public:
	constexpr SelectionRange() noexcept = default;
	constexpr SelectionRange(SelectionPoint from, SelectionPoint to) noexcept
		: m_from(from)
		, m_to(to)
	{
	}

	[[nodiscard]] constexpr SelectionPoint From() const noexcept { return m_from; }
	[[nodiscard]] constexpr SelectionPoint To() const noexcept { return m_to; }

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return m_from.IsValid() && m_to.IsValid();
	}

private:
	SelectionPoint m_from;
	SelectionPoint m_to;
};

[[nodiscard]] constexpr bool operator==(const SelectionRange& lhs, const SelectionRange& rhs) noexcept
{
	return lhs.From() == rhs.From() && lhs.To() == rhs.To();
}

//! Owns the presentation-neutral selection phase, mode, selection lock, and anchor.
//!
//! This type deliberately has no native window handle, view, document, layout, or shared-state dependency.
//! Native adapters may retain their active and rendering ranges separately until those ranges are
//! migrated to typed coordinate contracts.
class SelectionSession final {
public:
	SelectionSession() noexcept = default;

	[[nodiscard]] ESelectionTransition Begin(ESelectionMode mode) noexcept;
	//! Ends the active pointer/input phase but deliberately retains a keyboard selection lock.
	[[nodiscard]] ESelectionTransition End() noexcept;
	[[nodiscard]] ESelectionTransition SetModeEnabled(ESelectionMode mode, bool enabled) noexcept;

	[[nodiscard]] bool IsLocked() const noexcept { return m_locked; }
	void SetLocked(bool locked) noexcept { m_locked = locked; }

	//! Explicit terminal for a cleared selection: phase, mode, lock, and anchor all return to their initial state.
	void Clear() noexcept
	{
		Reset();
		m_locked = false;
		ClearAnchorRange();
	}

	[[nodiscard]] SelectionRange AnchorRange() const noexcept { return m_anchor; }
	void SetAnchorRange(const SelectionRange& range) noexcept { m_anchor = range; }
	void SetAnchorFrom(const SelectionPoint& point) noexcept { m_anchor = SelectionRange(point, m_anchor.To()); }
	void SetAnchorTo(const SelectionPoint& point) noexcept { m_anchor = SelectionRange(m_anchor.From(), point); }
	void ClearAnchorRange() noexcept { m_anchor = SelectionRange{}; }

	[[nodiscard]] bool IsActive() const noexcept { return m_active; }
	[[nodiscard]] ESelectionMode Mode() const noexcept { return m_mode; }
	[[nodiscard]] bool IsMode(ESelectionMode mode) const noexcept { return m_active && m_mode == mode; }

	[[nodiscard]] bool IsBoxSelecting() const noexcept { return IsMode(ESelectionMode::Box); }
	[[nodiscard]] bool IsLineSelecting() const noexcept { return IsMode(ESelectionMode::Line); }
	[[nodiscard]] bool IsWordSelecting() const noexcept { return IsMode(ESelectionMode::Word); }

	//! Resets only the transient interaction phase and mode; a selection lock remains explicit.
	void Reset() noexcept
	{
		m_active = false;
		m_mode = ESelectionMode::Linear;
	}

private:
	bool m_active = false;
	ESelectionMode m_mode = ESelectionMode::Linear;
	bool m_locked = false;
	SelectionRange m_anchor;
};

} // namespace editor::selection
