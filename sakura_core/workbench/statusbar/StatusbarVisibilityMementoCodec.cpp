/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/statusbar/StatusbarVisibilityMementoCodec.h"

#include <sakura/storage/StorageTypes.h>
#include "workbench/statusbar/StatusbarViewModel.h"

#include <picojson/picojson.h>

#include <set>

namespace workbench::statusbar {

std::optional<std::string> StatusbarVisibilityMementoCodec::Encode(
	const std::vector<std::string>& hiddenIds) noexcept
{
	try {
		if (hiddenIds.size() > kMaximumStatusbarEntries) return std::nullopt;
		picojson::array values;
		std::set<std::string, std::less<>> unique;
		for (const auto& id : hiddenIds) {
			if (!StatusbarViewModel::IsValidId(id) || !unique.emplace(id).second) return std::nullopt;
			values.emplace_back(id);
		}
		const std::string payload = picojson::value(std::move(values)).serialize();
		if (payload.size() > platform::storage::kMaximumStorageStringBytes) return std::nullopt;
		return payload;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<std::vector<std::string>> StatusbarVisibilityMementoCodec::Decode(
	std::string_view payload) noexcept
{
	try {
		if (payload.empty() || payload.size() > platform::storage::kMaximumStorageStringBytes) return std::nullopt;
		picojson::value root;
		auto begin = payload.begin();
		const auto end = payload.end();
		const std::string error = picojson::parse(root, begin, end);
		if (!error.empty() || !root.is<picojson::array>()) return std::nullopt;
		const auto& array = root.get<picojson::array>();
		if (array.size() > kMaximumStatusbarEntries) return std::nullopt;
		std::vector<std::string> result;
		std::set<std::string, std::less<>> unique;
		for (const auto& value : array) {
			if (!value.is<std::string>()) return std::nullopt;
			auto id = value.get<std::string>();
			if (!StatusbarViewModel::IsValidId(id) || !unique.emplace(id).second) return std::nullopt;
			result.emplace_back(std::move(id));
		}
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

} // namespace workbench::statusbar
