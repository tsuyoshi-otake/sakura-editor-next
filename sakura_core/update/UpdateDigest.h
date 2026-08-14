/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/IUpdateService.h"

namespace update {

//! BCrypt SHA-256 over the downloaded bytes, rendered as lowercase hex — the
//! lowercase hexadecimal shape required by the update manifest
//! payloads, so a digest from either path compares the same way.
class UpdateDigest final : public IUpdateDigest {
public:
	[[nodiscard]] std::optional<std::wstring> ComputeSha256Hex(const std::vector<std::uint8_t>& bytes) override;
};

//! Case-insensitive comparison of two hex digests that additionally requires
//! both to be well-formed 64-character SHA-256 values.
//!
//! Deliberately not `==` on the strings: a truncated, empty, or non-hex
//! "digest" must never compare equal to anything, because the caller's next
//! step is to run the file it describes.
[[nodiscard]] bool DigestsMatch(std::wstring_view left, std::wstring_view right) noexcept;

} // namespace update
