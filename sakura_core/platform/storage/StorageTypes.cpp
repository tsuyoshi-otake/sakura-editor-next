/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/storage/StorageTypes.h"

#include <span>
#include <tuple>

namespace platform::storage {
namespace {

[[nodiscard]] bool ContainsNull(std::string_view value) noexcept
{
	return value.find('\0') != std::string::npos;
}

} // namespace

bool IsValidStorageUtf8(std::string_view value, bool allowEmpty) noexcept
{
	if ((!allowEmpty && value.empty()) || value.size() > kMaximumStorageStringBytes || ContainsNull(value)) {
		return false;
	}
	const auto bytes = std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
	for (std::size_t i = 0; i < bytes.size();) {
		const auto first = bytes[i];
		if (first <= 0x7f) {
			++i;
			continue;
		}
		std::size_t tails = 0;
		std::uint32_t point = 0;
		if ((first & 0xe0) == 0xc0) {
			tails = 1;
			point = first & 0x1f;
		} else if ((first & 0xf0) == 0xe0) {
			tails = 2;
			point = first & 0x0f;
		} else if ((first & 0xf8) == 0xf0) {
			tails = 3;
			point = first & 0x07;
		} else {
			return false;
		}
		if (i + tails >= bytes.size()) {
			return false;
		}
		for (std::size_t tail = 1; tail <= tails; ++tail) {
			const auto byte = bytes[i + tail];
			if ((byte & 0xc0) != 0x80) {
				return false;
			}
			point = (point << 6) | (byte & 0x3f);
		}
		const auto minimum = tails == 1 ? 0x80u : tails == 2 ? 0x800u : 0x10000u;
		if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
			return false;
		}
		i += tails + 1;
	}
	return true;
}

bool StorageAddress::IsValid() const noexcept
{
	if (!IsValidStorageScope(scope)
		|| scopeId.size() > kMaximumStorageAddressPartBytes
		|| owner.size() > kMaximumStorageAddressPartBytes
		|| key.size() > kMaximumStorageAddressPartBytes
		|| !IsValidStorageUtf8(scopeId)
		|| !IsValidStorageUtf8(owner, false)
		|| !IsValidStorageUtf8(key, false)) {
		return false;
	}

	const bool needsScopeId = scope == EStorageScope::Profile || scope == EStorageScope::Workspace;
	return needsScopeId ? !scopeId.empty() : scopeId.empty();
}

bool StorageAddress::operator<(const StorageAddress& other) const noexcept
{
	return std::tie(scope, scopeId, owner, key) < std::tie(other.scope, other.scopeId, other.owner, other.key);
}

} // namespace platform::storage
