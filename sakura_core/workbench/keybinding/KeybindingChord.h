/*! @file
	@brief VS Code のキー式を、このホストが比較できる打鍵列へ変換する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace workbench::keybinding {

/*!
	@brief 1 打鍵

	Win32 の仮想キーコードで持つが、この型自体は Windows.h に依存しない。
	キー式の解釈は HWND もメッセージループも無しに検証できる必要があるため。
*/
struct KeyStroke {
	std::uint32_t	virtualKey = 0;
	bool			control = false;
	bool			shift = false;
	bool			alt = false;
	bool			win = false;

	friend bool operator==(const KeyStroke&, const KeyStroke&) = default;
};

//! VS Code の chord は最大 2 打鍵。3 打鍵以上は VS Code 自身が受け付けない。
inline constexpr std::size_t kMaxChordStrokes = 2;

/*!
	@brief `ctrl+shift+p` や `ctrl+k ctrl+i` を打鍵列へ変換する

	解釈できない式では**空の列**を返す。部分的に解釈した列を返さないのは、
	`ctrl+k ctrl+???` を「ctrl+k だけの割り当て」として登録してしまうと、
	拡張が意図していないキーを 1 打鍵で奪うことになるため。落とす方が安全。

	`mac` / `linux` の選択は拡張ホスト側（extension-loader.cjs）で済んでいるので、
	ここに来るのは Windows 用の式 1 本だけ。
*/
[[nodiscard]] std::vector<KeyStroke> ParseKeybinding(std::wstring_view expression);

} // namespace workbench::keybinding
