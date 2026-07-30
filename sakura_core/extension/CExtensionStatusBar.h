/*! @file
	@brief VS Code StatusBarItem 互換のウィンドウ状態モデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class EExtensionStatusBarAlignment : std::uint8_t {
	Left,
	Right,
};
struct SExtensionStatusBarItem {
	std::wstring handle;
	std::wstring itemId;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	EExtensionStatusBarAlignment alignment = EExtensionStatusBarAlignment::Left;
	double priority = 0.0;
	std::wstring text;
	std::wstring tooltip;
	std::wstring command;
	std::wstring accessibilityLabel;
	bool visible = false;
};

class CExtensionStatusBar final {
public:
	//! Existing handles may only be updated by their original extension generation.
	bool Upsert(SExtensionStatusBarItem item);
	bool Remove(std::wstring_view handle, std::wstring_view ownerExtensionId, std::uint64_t generation);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();

	[[nodiscard]] std::vector<SExtensionStatusBarItem> Snapshot() const;
	[[nodiscard]] std::optional<std::wstring> CommandFor(std::wstring_view handle) const;

private:
	mutable std::shared_mutex m_mutex;
	std::unordered_map<std::wstring, SExtensionStatusBarItem> m_items;
};
