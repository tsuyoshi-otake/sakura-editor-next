/*! @file
 * @brief Environment-block policy for the fixed `gh auth status` query.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <vector>

namespace workbench::account::detail {

//! The exact environment block RunGhAuthStatus hands to gh.exe: the parent
//! environment minus GH_DEBUG (its API debug output can carry request
//! metadata) plus the fixed non-interactive overrides, built by the shared
//! platform::process builder. Public as a seam so tests can pin the block.
[[nodiscard]] std::vector<wchar_t> BuildGhAuthStatusEnvironmentBlock();

} // namespace workbench::account::detail
