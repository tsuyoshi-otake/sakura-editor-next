/*! @file @brief Shared VS Code workspace-folder limits. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>

namespace workbench::workspace {

//! A single bound is used for parsing, editing, and transition planning.
inline constexpr std::size_t kMaximumWorkspaceFolders = 64U;

} // namespace workbench::workspace
