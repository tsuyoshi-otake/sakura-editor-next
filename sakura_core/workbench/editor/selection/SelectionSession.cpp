/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include <sakura/editor/SelectionSession.h>

namespace editor::selection {

ESelectionTransition SelectionSession::Begin(ESelectionMode mode) noexcept
{
	if (!m_active) {
		m_active = true;
		m_mode = mode;
		return ESelectionTransition::Started;
	}
	if (m_mode == mode) {
		return ESelectionTransition::Noop;
	}
	m_mode = mode;
	return ESelectionTransition::Restarted;
}

ESelectionTransition SelectionSession::End() noexcept
{
	if (!m_active) {
		return ESelectionTransition::Noop;
	}
	Reset();
	return ESelectionTransition::Ended;
}

ESelectionTransition SelectionSession::SetModeEnabled(ESelectionMode mode, bool enabled) noexcept
{
	if (enabled) {
		return Begin(mode);
	}
	if (!m_active || m_mode != mode) {
		return ESelectionTransition::Noop;
	}
	m_mode = ESelectionMode::Linear;
	return ESelectionTransition::ModeChanged;
}

} // namespace editor::selection
