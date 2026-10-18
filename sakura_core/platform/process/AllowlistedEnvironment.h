/*! @file
 * @brief UI-independent explicit-allowlist child environment blocks.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <span>
#include <string_view>
#include <vector>

namespace platform::process {

//! Builds a Unicode environment block that copies only the entries named by
//! `names` out of the calling process environment. Name matching is
//! case-insensitive ordinal; an allowed name the parent does not define is
//! simply absent from the block. Entries are sorted case-insensitively and
//! the block is double-NUL terminated, as CreateProcessW requires when the
//! caller passes CREATE_UNICODE_ENVIRONMENT. Callers that launch untrusted or
//! credential-adjacent children use this instead of `lpEnvironment = nullptr`
//! so secrets like GH_TOKEN never reach the child.
[[nodiscard]] std::vector<wchar_t> BuildAllowlistedEnvironmentBlock(
	std::span<const std::wstring_view> names);

} // namespace platform::process
