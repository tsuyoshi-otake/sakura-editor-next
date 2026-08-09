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

//! Owns the presentation-neutral selection phase, mode, and selection lock.
//!
//! This type deliberately has no native window handle, view, document, layout, or shared-state dependency.
//! Native adapters may retain their rendering range separately until that range is migrated to a
//! typed coordinate contract.
class SelectionSession final {
public:
	SelectionSession() noexcept = default;

	[[nodiscard]] ESelectionTransition Begin(ESelectionMode mode) noexcept;
	//! Ends the active pointer/input phase but deliberately retains a keyboard selection lock.
	[[nodiscard]] ESelectionTransition End() noexcept;
	[[nodiscard]] ESelectionTransition SetModeEnabled(ESelectionMode mode, bool enabled) noexcept;

	[[nodiscard]] bool IsLocked() const noexcept { return m_locked; }
	void SetLocked(bool locked) noexcept { m_locked = locked; }

	//! Explicit terminal for a cleared selection: phase, mode, and lock all return to their initial state.
	void Clear() noexcept
	{
		Reset();
		m_locked = false;
	}

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
};

} // namespace editor::selection
