/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace update {

//! The identity of one build, in the two spellings this product publishes:
//! the four-field product version `3.1.0.7221` that `version.h` carries into
//! the binary, and the release tag `v3.1.0-build.7221` that names the GitHub
//! Release the installer was uploaded to.
//!
//! `revision` is `BUILD_VERSION` from the generated `githash.h`. It is derived
//! from the commit count, so it increases monotonically across the whole
//! history regardless of what the marketing fields do, which is why it — not
//! `major` — leads the ordering below. The remaining three fields are a
//! tiebreaker for the case where two builds share a revision; they never decide
//! the comparison on their own.
struct UpdateVersion final {
	std::uint32_t major = 0;
	std::uint32_t minor = 0;
	std::uint32_t patch = 0;
	std::uint32_t revision = 0;

	[[nodiscard]] std::strong_ordering operator<=>(const UpdateVersion& other) const noexcept;
	[[nodiscard]] bool operator==(const UpdateVersion&) const noexcept = default;

	//! `3.1.0.7221`.
	[[nodiscard]] std::wstring ToProductVersion() const;
	//! `v3.1.0-build.7221`.
	[[nodiscard]] std::wstring ToReleaseTag() const;
};

//! Parses `3.1.0.7221`. Exactly four decimal fields are required; a partial
//! version is refused rather than zero-filled, because a zero-filled revision
//! would compare as older than every release and offer an endless update.
[[nodiscard]] std::optional<UpdateVersion> ParseProductVersion(std::wstring_view text);

//! Parses `v3.1.0-build.7221`. The leading `v` is optional, matching the way
//! GitHub renders a tag with and without it.
[[nodiscard]] std::optional<UpdateVersion> ParseReleaseTag(std::wstring_view text);

//! Whether `candidate` is a build this installation should offer to move to.
//! Strictly greater; an equal build is not an update.
[[nodiscard]] bool IsNewerBuild(const UpdateVersion& candidate, const UpdateVersion& current) noexcept;

} // namespace update
