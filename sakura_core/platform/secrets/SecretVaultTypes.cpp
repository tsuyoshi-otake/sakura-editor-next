/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/SecretVaultTypes.h"

#include <span>
#include <tuple>

namespace platform::secrets {
namespace {

[[nodiscard]] bool ContainsControl(std::string_view value) noexcept
{
	for (const unsigned char byte : value) {
		if (byte < 0x20 || byte == 0x7f) {
			return true;
		}
	}
	return false;
}

} // namespace

bool IsValidSecretVaultUtf8(std::string_view value, bool allowEmpty) noexcept
{
	if (!allowEmpty && value.empty()) {
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

bool IsValidSecretVaultIdentifier(std::string_view value, std::size_t maximumBytes) noexcept
{
	return value.size() <= maximumBytes && IsValidSecretVaultUtf8(value, false)
		&& !ContainsControl(value);
}

bool CanonicalizeSecretVaultExtensionId(
	std::string_view extensionId, std::string& canonicalExtensionId)
{
	canonicalExtensionId.clear();
	if (extensionId.empty() || extensionId.size() > kMaximumSecretVaultExtensionIdBytes) {
		return false;
	}
	bool hasPublisherSeparator = false;
	bool previousHyphen = false;
	canonicalExtensionId.reserve(extensionId.size());
	for (const unsigned char byte : extensionId) {
		char canonical = static_cast<char>(byte);
		if (canonical >= 'A' && canonical <= 'Z') {
			canonical = static_cast<char>(canonical - 'A' + 'a');
		}
		const bool alphaNumeric = (canonical >= 'a' && canonical <= 'z')
			|| (canonical >= '0' && canonical <= '9');
		const bool isPublisherSeparator = canonical == '.';
		const bool isHyphen = canonical == '-';
		if (!alphaNumeric && !isPublisherSeparator && !isHyphen) {
			canonicalExtensionId.clear();
			return false;
		}
		if (isHyphen && (canonicalExtensionId.empty() || canonicalExtensionId.back() == '.')) {
			canonicalExtensionId.clear();
			return false;
		}
		if (isPublisherSeparator && (hasPublisherSeparator || canonicalExtensionId.empty()
			|| canonicalExtensionId.back() == '-')) {
			canonicalExtensionId.clear();
			return false;
		}
		if (isPublisherSeparator) {
			hasPublisherSeparator = true;
		}
		previousHyphen = isHyphen;
		canonicalExtensionId.push_back(canonical);
	}
	if (previousHyphen || canonicalExtensionId.back() == '.' || !hasPublisherSeparator) {
		canonicalExtensionId.clear();
		return false;
	}
	return true;
}

bool SecretAddress::IsValid() const
{
	std::string canonicalExtensionId;
	return CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
		&& canonicalExtensionId == extensionId
		&& IsValidSecretVaultIdentifier(key, kMaximumSecretVaultKeyBytes);
}

bool SecretAddress::operator<(const SecretAddress& other) const noexcept
{
	return std::tie(extensionId, key) < std::tie(other.extensionId, other.key);
}

} // namespace platform::secrets
