/*! @file @brief Bounded ConPTY child environment construction. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/session/TerminalSession.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace terminal {

enum class TerminalEnvironmentBuildStatus : std::uint8_t {
	Succeeded,
	InvalidOverride,
	InvalidPathDirectory,
	TooLarge,
	ReadFailed,
};

struct TerminalEnvironmentBuildResult {
	TerminalEnvironmentBuildStatus status{ TerminalEnvironmentBuildStatus::ReadFailed };
	std::vector<wchar_t> block;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == TerminalEnvironmentBuildStatus::Succeeded;
	}
};

//! Builds a sorted, double-NUL-terminated Windows environment block without
//! changing the current process. The supplied inherited entries exclude their
//! final double NUL and exist as a test seam around GetEnvironmentStringsW.
[[nodiscard]] TerminalEnvironmentBuildResult BuildTerminalEnvironmentBlock(
	const TerminalLaunchOptions& options,
	std::span<const std::wstring> inheritedEntries );

//! Production overload which snapshots the current process environment.
[[nodiscard]] TerminalEnvironmentBuildResult BuildTerminalEnvironmentBlock(
	const TerminalLaunchOptions& options ) noexcept;

} // namespace terminal
