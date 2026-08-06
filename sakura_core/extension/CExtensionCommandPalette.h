/*! @file
	@brief 組み込みコマンドと拡張コマンドを統合する Command Palette モデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionContextKeys.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SExtensionCommandDescriptor {
	std::wstring id;
	std::wstring title;
	std::wstring category;
	std::wstring whenClause;
	std::wstring enablementClause;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	bool builtIn = false;
};

struct SExtensionCommandPaletteItem {
	std::wstring id;
	std::wstring label;
	std::wstring detail;
	std::wstring extensionId;
	bool enabled = true;
	bool builtIn = false;
};

class CExtensionCommandPalette final {
public:
	//! Duplicate IDs are rejected so command ownership cannot be silently replaced.
	bool Register(SExtensionCommandDescriptor command);
	bool Unregister(std::wstring_view commandId, std::wstring_view ownerExtensionId = {});
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();

	[[nodiscard]] bool Contains(std::wstring_view commandId) const;
	/*!
		@brief コマンド 1 件の宣言をコピーで返す。未登録なら nullopt。

		メニュー投影はコマンドの `title` と `enablement` を必要とするが、
		パレット検索の経路（Search）を通すと関係のない絞り込みと並べ替えが挟まる。
		参照ではなくコピーを返すのは、返した後にロックが外れるため。
	*/
	[[nodiscard]] std::optional<SExtensionCommandDescriptor> Find(std::wstring_view commandId) const;
	[[nodiscard]] std::vector<std::wstring> CommandIds(bool filterInternal = false) const;
	[[nodiscard]] std::vector<SExtensionCommandPaletteItem> Search(
		std::wstring_view query,
		const CExtensionContextKeys& contextKeys,
		std::size_t maximumResults = 200) const;

private:
	mutable std::shared_mutex m_mutex;
	std::unordered_map<std::wstring, SExtensionCommandDescriptor> m_commands;
};
