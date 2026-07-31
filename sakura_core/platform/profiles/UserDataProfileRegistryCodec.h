/*! @file @brief Bounded portable codec for durable user-data profile metadata. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/profiles/UserDataProfileRegistry.h"

#include <string>
#include <string_view>

namespace platform::profiles {

enum class UserDataProfileRegistryCodecStatus : unsigned char {
	Decoded,
	Invalid,
	UnsupportedVersion,
	TooLarge,
};

struct UserDataProfileRegistryDecodeResult {
	UserDataProfileRegistryCodecStatus status = UserDataProfileRegistryCodecStatus::Invalid;
	UserDataProfileRegistrySnapshot snapshot;
	[[nodiscard]] bool Decoded() const noexcept { return status == UserDataProfileRegistryCodecStatus::Decoded; }
};

//! This UTF-8 Base64 format contains only profile metadata, inheritance flags and
//! URI/window associations. It intentionally has no secret, installed-extension,
//! or profile resource payload fields, so the same value is safe as a portable document.
[[nodiscard]] std::string EncodeUserDataProfileRegistryDocument(const UserDataProfileRegistrySnapshot& snapshot);
[[nodiscard]] UserDataProfileRegistryDecodeResult DecodeUserDataProfileRegistryDocument(std::string_view bytes);

} // namespace platform::profiles
