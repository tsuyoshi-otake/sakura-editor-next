/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/cli/SakuraTmuxCli.h"

namespace terminal::cli {

//! Reads the exact interactive-child environment contract with bounded scans.
[[nodiscard]] std::optional<SakuraTmuxEnvironment> ReadSakuraTmuxEnvironment() noexcept;

//! Shared process entry called directly by both tmux.exe and sakura-tmux.exe.
//! No executable lookup, shell reconstruction, or re-exec is performed.
int SakuraTmuxCliMain(int argc, wchar_t* const* argv) noexcept;

} // namespace terminal::cli
