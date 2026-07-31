/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/ConfigurationTypes.h"

#include <cstddef>
#include <utility>

namespace config {
namespace {

constexpr std::size_t kMaximumConfigurationDepth = 64;
constexpr std::size_t kMaximumConfigurationNodes = 65536;
constexpr std::size_t kMaximumConfigurationStringLength = 1024 * 1024;
constexpr std::size_t kMaximumConfigurationObjectKeyLength = 64 * 1024;

bool IsValidUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit >= 0xd800U && unit <= 0xdbffU) {
			if (index + 1 >= value.size()) return false;
			const auto low = static_cast<std::uint32_t>(value[++index]);
			if (low < 0xdc00U || low > 0xdfffU) return false;
		} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
			return false;
		}
	}
	return true;
}

bool IsValidStorage(
	const ConfigurationValue::Storage& storage,
	std::size_t depth,
	std::size_t& nodes) noexcept
{
	if (depth > kMaximumConfigurationDepth || ++nodes > kMaximumConfigurationNodes) {
		return false;
	}
	if (const auto* number = std::get_if<double>(&storage)) {
		return std::isfinite(*number);
	}
	if (const auto* string = std::get_if<std::wstring>(&storage)) {
		return string->size() <= kMaximumConfigurationStringLength && IsValidUtf16(*string);
	}
	if (const auto* array = std::get_if<ConfigurationValue::Array>(&storage)) {
		for (const auto& item : *array) {
			if (!IsValidStorage(item.Value(), depth + 1, nodes)) {
				return false;
			}
		}
	}
	if (const auto* object = std::get_if<ConfigurationValue::Object>(&storage)) {
		for (const auto& [key, item] : *object) {
			if (key.size() > kMaximumConfigurationObjectKeyLength || !IsValidUtf16(key) ||
				!IsValidStorage(item.Value(), depth + 1, nodes)) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

bool ConfigurationValue::IsValid() const noexcept
{
	std::size_t nodes = 0;
	return IsValidStorage(m_value, 0, nodes);
}

bool operator==(const ConfigurationValue& left, const ConfigurationValue& right) noexcept
{
	return left.m_value == right.m_value;
}

ConfigurationSubscription::ConfigurationSubscription(ConfigurationSubscription&& other) noexcept
	: m_unsubscribe(std::move(other.m_unsubscribe))
{
}

ConfigurationSubscription& ConfigurationSubscription::operator=(ConfigurationSubscription&& other) noexcept
{
	if (this != &other) {
		Reset();
		m_unsubscribe = std::move(other.m_unsubscribe);
	}
	return *this;
}

void ConfigurationSubscription::Reset() noexcept
{
	if (m_unsubscribe) {
		auto unsubscribe = std::move(m_unsubscribe);
		try {
			unsubscribe();
		} catch (...) {
			// Destruction and explicit reset are terminal cleanup boundaries.
		}
	}
}

} // namespace config
