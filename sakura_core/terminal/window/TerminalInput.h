/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace terminal {

struct TerminalKeyEvent {
	std::uint32_t virtualKey{};
	bool shift{};
	bool control{};
	bool alt{};
	std::uint16_t scanCode{};
	std::uint16_t repeatCount{ 1 };
	wchar_t character{};
	bool keyDown{ true };
	bool enhanced{};
	bool capsLock{};
	bool numLock{};
	bool rightAlt{};
	bool rightControl{};
};

enum class TerminalMouseAction : std::uint8_t {
	Press,
	Release,
	Move,
	WheelUp,
	WheelDown,
};

struct TerminalMouseEvent {
	TerminalMouseAction action{ TerminalMouseAction::Press };
	unsigned int button{};
	std::size_t column{};
	std::size_t row{};
	bool shift{};
	bool alt{};
	bool control{};
};

enum class TerminalShortcutAction : std::uint8_t {
	None,
	Copy,
	Paste,
	SendInterrupt,
};

enum class TerminalRightClickAction : std::uint8_t {
	SendToApplication,
	CopySelection,
	PasteClipboard,
};

[[nodiscard]] std::string EncodeTerminalKey( const TerminalKeyEvent& event );
[[nodiscard]] std::string EncodeTerminalText( std::wstring_view text );
[[nodiscard]] std::string EncodeTerminalPaste( std::wstring_view text, bool bracketedPaste );
[[nodiscard]] std::string EncodeTerminalMouse( const TerminalMouseEvent& event, const TerminalModes& modes );
[[nodiscard]] TerminalShortcutAction ResolveTerminalShortcut( const TerminalKeyEvent& event, bool hasSelection ) noexcept;
[[nodiscard]] TerminalRightClickAction ResolveTerminalRightClick( bool hasSelection, bool mouseReporting, bool shift ) noexcept;

} // namespace terminal
