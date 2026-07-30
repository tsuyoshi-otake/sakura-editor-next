/*! @file
	@brief VS Code when-clause 用の context key ストア
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

using ExtensionContextValue = std::variant<std::monostate, bool, std::int64_t, double, std::wstring>;

class CExtensionContextKeys final {
public:
	bool Set(std::wstring key, ExtensionContextValue value, std::wstring ownerExtensionId = {});
	bool Remove(std::wstring_view key, std::wstring_view ownerExtensionId = {});
	void RemoveOwnedBy(std::wstring_view ownerExtensionId);
	void Clear();

	[[nodiscard]] bool Contains(std::wstring_view key) const;
	[[nodiscard]] ExtensionContextValue Get(std::wstring_view key) const;
	//! Invalid clauses fail closed. An empty clause is visible/enabled.
	[[nodiscard]] bool Evaluate(std::wstring_view clause) const;

private:
	struct Entry {
		ExtensionContextValue value;
		std::wstring ownerExtensionId;
	};

	mutable std::shared_mutex m_mutex;
	std::unordered_map<std::wstring, Entry> m_values;
};
