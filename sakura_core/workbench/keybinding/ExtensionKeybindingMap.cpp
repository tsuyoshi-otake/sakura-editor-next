/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/keybinding/ExtensionKeybindingMap.h"

#include <algorithm>
#include <utility>

namespace workbench::keybinding {

void ExtensionKeybindingMap::SetBindings(std::vector<ExtensionKeybinding> bindings)
{
	/*
		同じ打鍵列かつ同じ `when` を 2 つ以上受け取ったら、先の 1 本だけ残す。
		残す側を先にするのは「後から入れた拡張が既存の割り当てを黙って奪わない」ため。
		`when` が違えば別の割り当てなので、両方残して評価時に選ばせる。
	*/
	std::vector<ExtensionKeybinding> deduplicated;
	deduplicated.reserve(bindings.size());
	for (auto& binding : bindings) {
		if (binding.sequence.empty() || binding.commandId.empty()) continue;
		const auto duplicate = std::ranges::find_if(deduplicated, [&binding](const auto& existing) {
			return existing.sequence == binding.sequence && existing.whenClause == binding.whenClause;
		});
		if (duplicate != deduplicated.end()) continue;
		deduplicated.push_back(std::move(binding));
	}
	m_bindings = std::move(deduplicated);

	// 表が変わった以上、待機中の chord は「今も存在する割り当ての 1 打鍵目」とは
	// 限らない。押しっぱなしの状態を新しい表へ持ち越さない。
	ClearChord();
}

void ExtensionKeybindingMap::Clear() noexcept
{
	m_bindings.clear();
	ClearChord();
}

} // namespace workbench::keybinding
