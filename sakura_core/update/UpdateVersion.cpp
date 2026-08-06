/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateVersion.h"

#include <array>
#include <tuple>

namespace update {
namespace {

//! `4294967295` is ten digits, so nine digits can never overflow and a longer
//! run is refused outright rather than saturated.
constexpr std::size_t kMaximumFieldDigits = 9;

//! Reads one decimal field and advances past it. Returns nothing for an empty
//! run, an over-long run, or a non-digit lead.
std::optional<std::uint32_t> ReadField(std::wstring_view text, std::size_t& index) noexcept
{
	const std::size_t begin = index;
	std::uint32_t value = 0;
	while (index < text.size() && text[index] >= L'0' && text[index] <= L'9') {
		value = value * 10U + static_cast<std::uint32_t>(text[index] - L'0');
		++index;
	}
	const std::size_t digits = index - begin;
	if (digits == 0 || digits > kMaximumFieldDigits) return std::nullopt;
	return value;
}

bool ConsumeLiteral(std::wstring_view text, std::size_t& index, std::wstring_view literal) noexcept
{
	if (text.size() - index < literal.size()) return false;
	if (text.substr(index, literal.size()) != literal) return false;
	index += literal.size();
	return true;
}

//! The three marketing fields plus the separator that follows each of the first
//! two. Shared so the product version and the release tag cannot drift apart.
bool ReadMarketingFields(std::wstring_view text, std::size_t& index, UpdateVersion& version) noexcept
{
	const auto major = ReadField(text, index);
	if (!major || !ConsumeLiteral(text, index, L".")) return false;
	const auto minor = ReadField(text, index);
	if (!minor || !ConsumeLiteral(text, index, L".")) return false;
	const auto patch = ReadField(text, index);
	if (!patch) return false;
	version.major = *major;
	version.minor = *minor;
	version.patch = *patch;
	return true;
}

} // namespace

std::strong_ordering UpdateVersion::operator<=>(const UpdateVersion& other) const noexcept
{
	return std::tie(revision, major, minor, patch)
		<=> std::tie(other.revision, other.major, other.minor, other.patch);
}

std::wstring UpdateVersion::ToProductVersion() const
{
	return std::to_wstring(major) + L"." + std::to_wstring(minor) + L"."
		+ std::to_wstring(patch) + L"." + std::to_wstring(revision);
}

std::wstring UpdateVersion::ToReleaseTag() const
{
	return L"v" + std::to_wstring(major) + L"." + std::to_wstring(minor) + L"."
		+ std::to_wstring(patch) + L"-build." + std::to_wstring(revision);
}

std::optional<UpdateVersion> ParseProductVersion(std::wstring_view text)
{
	std::size_t index = 0;
	UpdateVersion version{};
	if (!ReadMarketingFields(text, index, version)) return std::nullopt;
	if (!ConsumeLiteral(text, index, L".")) return std::nullopt;
	const auto revision = ReadField(text, index);
	if (!revision || index != text.size()) return std::nullopt;
	version.revision = *revision;
	return version;
}

std::optional<UpdateVersion> ParseReleaseTag(std::wstring_view text)
{
	std::size_t index = 0;
	if (index < text.size() && (text[index] == L'v' || text[index] == L'V')) ++index;
	UpdateVersion version{};
	if (!ReadMarketingFields(text, index, version)) return std::nullopt;
	if (!ConsumeLiteral(text, index, L"-build.")) return std::nullopt;
	const auto revision = ReadField(text, index);
	if (!revision || index != text.size()) return std::nullopt;
	version.revision = *revision;
	return version;
}

bool IsNewerBuild(const UpdateVersion& candidate, const UpdateVersion& current) noexcept
{
	return candidate > current;
}

} // namespace update
