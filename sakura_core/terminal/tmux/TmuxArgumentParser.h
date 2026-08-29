/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandTypes.h"

namespace terminal::tmux {

class TmuxArgumentParser final {
public:
	[[nodiscard]] static TmuxParseResult Parse(
		const std::vector<std::string>& argv,
		TmuxCommand& command) noexcept
	{
		return ParseTmuxArguments(argv, command);
	}
};

} // namespace terminal::tmux
