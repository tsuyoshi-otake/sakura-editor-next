/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/outline/OutlineViewLifecycle.h"

#include <cstdint>
#include <limits>

namespace workbench::outline {

enum class OutlineRefreshScheduleResult : std::uint8_t {
	Ignored,
	Armed,
	ImmediateFallback,
	Closed,
};

//! UI-owned debounce state machine used by the workbench Outline dialog.
//!
//! It owns only versioned timer tokens.  Capture and submit remain callbacks so
//! the production dialog and deterministic tests exercise the same transitions.
class OutlineRefreshScheduler final {
public:
	[[nodiscard]] bool CanReuse(
		bool forceRefresh,
		int requestedOutlineType,
		int currentOutlineType,
		OutlineDocumentVersion requestedVersion,
		std::uint64_t requestedGeneration,
		OutlineDocumentVersion currentVersion ) const noexcept
	{
		if( forceRefresh || requestedOutlineType != currentOutlineType ) return false;
		const bool submittedCurrentVersion = requestedGeneration != 0
			&& requestedVersion.IsValid() && requestedVersion == currentVersion;
		const bool debounceOwnsCurrentVersion = m_timerArmed
			&& m_timerVersion.IsValid() && m_timerVersion == currentVersion;
		return submittedCurrentVersion || debounceOwnsCurrentVersion;
	}

	template <class ArmTimer, class KillTimer, class ImmediateRefresh, class ClosedTerminal>
	[[nodiscard]] OutlineRefreshScheduleResult NotifyChange(
		bool enabled,
		bool hasWindow,
		OutlineDocumentVersion timerVersion,
		ArmTimer&& armTimer,
		KillTimer&& killTimer,
		ImmediateRefresh&& immediateRefresh,
		ClosedTerminal&& closedTerminal )
	{
		if( m_timerArmed ) {
			// Replacing a quiet-period timer never creates a second callback.
			killTimer(m_timerToken);
		}
		m_timerArmed = false;
		m_timerToken = 0;
		m_timerVersion = {};
		if( !enabled ) {
			return OutlineRefreshScheduleResult::Ignored;
		}
		if( !hasWindow ) {
			closedTerminal();
			return OutlineRefreshScheduleResult::Closed;
		}

		if( !timerVersion.IsValid()
			|| m_nextTimerToken == (std::numeric_limits<std::uint64_t>::max)() ) {
			immediateRefresh();
			return OutlineRefreshScheduleResult::ImmediateFallback;
		}
		m_timerVersion = timerVersion;
		m_timerToken = ++m_nextTimerToken;
		bool armed = false;
		try {
			armed = armTimer(m_timerToken);
		}catch( ... ) {
			armed = false;
		}
		if( armed ) {
			m_timerArmed = true;
			return OutlineRefreshScheduleResult::Armed;
		}

		m_timerToken = 0;
		m_timerVersion = {};
		immediateRefresh();
		return OutlineRefreshScheduleResult::ImmediateFallback;
	}

	template <class KillTimer, class Refresh>
	[[nodiscard]] bool ConsumeTimer(
		std::uint64_t firedTimerToken,
		bool enabled,
		bool hasWindow,
		OutlineDocumentVersion currentVersion,
		KillTimer&& killTimer,
		Refresh&& refresh )
	{
		if( !m_timerArmed ) return false;
		// A WM_TIMER already queued before KillTimer may arrive after a document
		// switch or re-arm.  Its unique token must not consume the new timer.
		if( firedTimerToken == 0 || firedTimerToken != m_timerToken ) return false;
		killTimer(m_timerToken);
		const auto timerVersion = m_timerVersion;
		m_timerArmed = false;
		m_timerToken = 0;
		m_timerVersion = {};
		if( !enabled || !hasWindow || !timerVersion.IsValid()
			|| timerVersion != currentVersion ) return false;
		refresh();
		return true;
	}

	template <class KillTimer>
	void Stop( KillTimer&& killTimer )
	{
		if( m_timerArmed ) killTimer(m_timerToken);
		m_timerArmed = false;
		m_timerToken = 0;
		m_timerVersion = {};
	}

	template <class KillTimer>
	void Disarm( KillTimer&& killTimer )
	{
		if( m_timerArmed ) killTimer(m_timerToken);
		m_timerArmed = false;
		m_timerToken = 0;
		m_timerVersion = {};
	}

	[[nodiscard]] bool IsTimerArmed() const noexcept { return m_timerArmed; }
	[[nodiscard]] std::uint64_t TimerToken() const noexcept { return m_timerToken; }
	[[nodiscard]] OutlineDocumentVersion TimerVersion() const noexcept { return m_timerVersion; }

private:
	std::uint64_t m_nextTimerToken = 0;
	std::uint64_t m_timerToken = 0;
	OutlineDocumentVersion m_timerVersion{};
	bool m_timerArmed = false;
};

} // namespace workbench::outline
