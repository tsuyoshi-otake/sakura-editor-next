/*! @file
	@brief `extensions.supportUntrustedWorkspaces` のユーザー上書きを解決する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/ExtensionUntrustedWorkspaceOverride.h"

#include <cwctype>
#include <utility>
#include <variant>

namespace {

//! JSON メンバー名。picojson ではなく config::ConfigurationValue 経路なので wstring。
constexpr wchar_t kSupportedMemberName[] = L"supported";
constexpr wchar_t kVersionMemberName[] = L"version";
//! `capabilities.untrustedWorkspaces.supported` の "limited" と同じ大文字小文字の
//! 区別を踏襲する。CExtensionManager::ParseUntrustedWorkspaceSupport
//! (CExtensionManager.cpp) が `"limited"` を厳密一致でしか受け付けないのと同じ理由で、
//! "Limited" や "LIMITED" は次の分岐で drop される。
constexpr wchar_t kLimitedValue[] = L"limited";

/*!
	@brief 拡張識別子として大文字小文字を無視して等しいか

	CExtensionManager::FindInstalled (CExtensionManager.cpp) が既に sUniqueId を
	wmemicmp で大文字小文字を無視して比較しており、ここもそれに合わせる。
	wmemicmp は NUL 終端を前提とするが、この関数は std::wstring_view を受け取り
	NUL 終端を保証できないため、長さ照合込みの自前比較にしてある。
*/
bool AreExtensionIdsEqual(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::towupper(static_cast<std::wint_t>(left[index])) != std::towupper(static_cast<std::wint_t>(right[index]))) {
			return false;
		}
	}
	return true;
}

} // namespace

std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>>
ParseExtensionUntrustedWorkspaceOverrides(const config::ConfigurationValue& value)
{
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> result;

	const auto* pRoot = std::get_if<config::ConfigurationValue::Object>(&value.Value());
	if (!pRoot) {
		return result;
	}

	for (const auto& [sUniqueId, entryValue] : *pRoot) {
		if (result.size() >= kMaxExtensionUntrustedWorkspaceOverrideEntries) {
			// これ以上は取り込まない。パースは純粋関数で、切り詰めを呼び出し側へ
			// 伝える経路を持たないため、固定の上限だけを決めて余剰を捨てる。
			break;
		}

		const auto* pEntryObject = std::get_if<config::ConfigurationValue::Object>(&entryValue.Value());
		if (!pEntryObject) {
			// オブジェクトでない値は「その拡張の上書き」として読めないので、
			// その項目だけを落とす。他の拡張の上書きには影響しない。
			continue;
		}

		const auto itSupported = pEntryObject->find(kSupportedMemberName);
		if (itSupported == pEntryObject->end()) {
			continue;
		}

		EExtensionUntrustedWorkspaceSupport supported = EExtensionUntrustedWorkspaceSupport::NotSupported;
		if (const auto* pBoolValue = std::get_if<bool>(&itSupported->second.Value())) {
			supported = *pBoolValue
				? EExtensionUntrustedWorkspaceSupport::Supported
				: EExtensionUntrustedWorkspaceSupport::NotSupported;
		}
		else if (const auto* pStringValue = std::get_if<std::wstring>(&itSupported->second.Value());
			pStringValue && *pStringValue == kLimitedValue) {
			supported = EExtensionUntrustedWorkspaceSupport::Limited;
		}
		else {
			// bool でも "limited" 文字列でもない値（数値・配列・オブジェクト・null・
			// 大文字小文字違いの文字列）は読み取れない申告なので、この項目を落とす。
			continue;
		}

		ExtensionUntrustedWorkspaceOverride parsedOverride;
		parsedOverride.supported = supported;

		const auto itVersion = pEntryObject->find(kVersionMemberName);
		if (itVersion != pEntryObject->end()) {
			const auto* pVersionString = std::get_if<std::wstring>(&itVersion->second.Value());
			if (!pVersionString) {
				// ユーザーは明らかにバージョンを絞るつもりだったのに、その絞り込みが
				// 読めない。読めないまま全バージョンへ適用すると、書いていない免除を
				// 勝手に広げてしまうので、絞り込みごとこの項目を落とす。
				continue;
			}
			parsedOverride.version = *pVersionString;
		}

		result.insert_or_assign(sUniqueId, std::move(parsedOverride));
	}

	return result;
}

EExtensionUntrustedWorkspaceSupport ResolveUntrustedWorkspaceSupport(
	EExtensionUntrustedWorkspaceSupport manifestValue,
	std::wstring_view uniqueId,
	std::wstring_view installedVersion,
	const std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>>& overrides)
{
	const ExtensionUntrustedWorkspaceOverride* pVersionAgnosticMatch = nullptr;

	for (const auto& [sOverrideId, overrideEntry] : overrides) {
		if (!AreExtensionIdsEqual(sOverrideId, uniqueId)) {
			continue;
		}
		if (!overrideEntry.version.empty()) {
			if (overrideEntry.version == installedVersion) {
				// バージョンを明示した一致は最も具体的な申告なので、確定させてよい。
				return overrideEntry.supported;
			}
			// このバージョンには適用されない申告。一致しない版指定は無視する。
			continue;
		}
		// バージョン指定なしは「全バージョンへ適用」の申告。より具体的な
		// バージョン一致が後から見つかるかもしれないので、確定はせず保持する。
		if (!pVersionAgnosticMatch) {
			pVersionAgnosticMatch = &overrideEntry;
		}
	}

	if (pVersionAgnosticMatch) {
		return pVersionAgnosticMatch->supported;
	}
	return manifestValue;
}
