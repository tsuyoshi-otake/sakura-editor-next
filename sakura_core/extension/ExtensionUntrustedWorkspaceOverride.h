/*! @file
	@brief `extensions.supportUntrustedWorkspaces` のユーザー上書きを解決する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "config/ConfigurationTypes.h"
#include "extension/CExtensionManager.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

//! `extensions.supportUntrustedWorkspaces` に宣言できる上書きの数の上限。
//! 設定値は信用できない入力なので、パース時点で有界にしておく。テストから
//! 直接参照できるよう公開する。
inline constexpr std::size_t kMaxExtensionUntrustedWorkspaceOverrideEntries = 4096;

//! 1 拡張に対するユーザー上書き 1 件。
struct ExtensionUntrustedWorkspaceOverride final {
	EExtensionUntrustedWorkspaceSupport supported = EExtensionUntrustedWorkspaceSupport::NotSupported;
	//! 空はすべての導入済みバージョンへ適用されることを意味する。
	std::wstring version;
};

/*!
	@brief `extensions.supportUntrustedWorkspaces` 設定値をパースする

	VS Code の同名設定は `{ "<publisher>.<name>": { "supported": true|false|"limited", "version"?: "x.y.z" } }`
	という形をとる。この関数は config::ConfigurationValue 経路（JSONC から既に
	デコードされた値）を受け取る純粋関数で、ファイル I/O・HWND・サービス参照を
	一切持たない。

	読めない形はすべて「その項目だけを落とす」で fail closed する。オブジェクトで
	ない設定値全体、オブジェクトでないメンバー値、`supported` が無い／型が違う
	メンバー、文字列でない `version` を持つメンバーは、それぞれその 1 項目だけを
	結果から除く。他の項目には影響しない。
*/
[[nodiscard]] std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>>
ParseExtensionUntrustedWorkspaceOverrides(const config::ConfigurationValue& value);

/*!
	@brief 1 個の導入済み拡張について、実際に効く untrustedWorkspaces 対応を決める

	上書きが無ければ @p manifestValue をそのまま返す。上書きがあれば、それが
	@p manifestValue より緩い方向でも厳しい方向でも、上書きの値をそのまま採用する
	（VS Code 自身がそう動くため、片方向だけ効かせる作りにしてはならない）。

	@p uniqueId の一致は大文字小文字を無視する。CExtensionManager::FindInstalled
	（CExtensionManager.cpp）が既に sUniqueId をそう比較しており、VS Code 自身も
	拡張識別子を大文字小文字を無視して比較するため、これは divergence ではなく
	両者の一致。

	@p overrides に @p uniqueId が一致し、かつバージョン指定が一致する項目
	（バージョン指定なしは常に一致）が複数あるときは、バージョンを明示した
	項目を優先する。バージョン指定が無いのに一致する項目は @p installedVersion
	を問わず全バージョンへ適用されるので、より具体的な指定を優先させないと
	ユーザーが書いたつもりの絞り込みが無視されてしまう。
*/
[[nodiscard]] EExtensionUntrustedWorkspaceSupport ResolveUntrustedWorkspaceSupport(
	EExtensionUntrustedWorkspaceSupport manifestValue,
	std::wstring_view uniqueId,
	std::wstring_view installedVersion,
	const std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>>& overrides);
