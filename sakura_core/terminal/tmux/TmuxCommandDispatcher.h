/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandTypes.h"
#include "terminal/tmux/TmuxFormatEvaluator.h"

namespace terminal::tmux {

struct TmuxDispatcherLimits final {
	std::chrono::seconds maximumWait{ 300 };
	std::size_t maximumInputBytes{ 64u * 1024u };
	std::size_t maximumOutputBytes{ 512u * 1024u };
	TmuxFormatLimits formatLimits;
};

class TmuxCommandDispatcher final {
public:
	explicit TmuxCommandDispatcher(ITmuxRuntimePort& runtime,
		TmuxCompatibilityProfile profile = {}, TmuxDispatcherLimits limits = {}) noexcept;

	[[nodiscard]] TmuxCommandResult Dispatch(const TmuxCommand& command) noexcept;

private:
	[[nodiscard]] TmuxCommandResult DispatchList(const TmuxCommand&, const TmuxRuntimeSnapshot&);
	[[nodiscard]] TmuxCommandResult DispatchSend(const TmuxCommand&, const TmuxRuntimeSnapshot&);
	[[nodiscard]] TmuxCommandResult DispatchCapture(const TmuxCommand&, const TmuxRuntimeSnapshot&);
	[[nodiscard]] TmuxCommandResult DispatchDisplay(const TmuxCommand&, const TmuxRuntimeSnapshot&);
	[[nodiscard]] TmuxCommandResult DispatchTopology(const TmuxCommand&, const TmuxRuntimeSnapshot&);
	[[nodiscard]] TmuxCommandResult DispatchWait(const TmuxCommand&, const TmuxRuntimeSnapshot&);

	ITmuxRuntimePort& m_runtime;
	TmuxCompatibilityProfile m_profile;
	TmuxDispatcherLimits m_limits;
};

} // namespace terminal::tmux
