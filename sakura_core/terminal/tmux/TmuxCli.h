/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandDispatcher.h"
#include "terminal/tmux/TmuxArgumentParser.h"

namespace terminal::tmux {

struct TmuxCliResponse final {
	int exitCode{ 1 };
	std::string stdoutText;
	std::string stderrText;

	[[nodiscard]] bool Succeeded() const noexcept { return exitCode == 0; }
};

//! Canonical process boundary for both sakura-tmux.exe and tmux.exe shim.
//! The caller owns stdout/stderr handles and supplies the runtime transport.
class TmuxCli final {
public:
	[[nodiscard]] static TmuxCliResponse Run(
		const std::vector<std::string>& argv,
		ITmuxRuntimePort& runtime,
		TmuxCompatibilityProfile profile = {},
		TmuxDispatcherLimits limits = {}) noexcept;

	[[nodiscard]] static TmuxCliResponse FromParse(const TmuxParseResult&) noexcept;
	[[nodiscard]] static TmuxCliResponse FromDispatch(const TmuxCommandResult&) noexcept;
};

} // namespace terminal::tmux
