/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/filesystem/IFileService.h>

namespace platform::filesystem {

//! Create an unconfigured service for composition and contract tests.
//! Providers are registered through the construction-time composition seam.
[[nodiscard]] FileResult<std::unique_ptr<IFileService>> CreateFileService();

//! Create the default local Win32 file service with its `file:` provider.
//! The returned service owns all provider state and worker lifetimes.
[[nodiscard]] FileResult<std::unique_ptr<IFileService>> CreateWin32FileService();

} // namespace platform::filesystem
