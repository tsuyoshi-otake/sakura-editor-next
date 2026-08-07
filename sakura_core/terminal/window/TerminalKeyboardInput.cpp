/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalInput.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <windows.h>

namespace terminal {
namespace {

void AppendUtf8( std::string& output, char32_t codepoint )
{
	if( codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ) codepoint = 0xfffd;
	if( codepoint <= 0x7f ) {
		output.push_back(static_cast<char>(codepoint));
	} else if( codepoint <= 0x7ff ) {
		output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
		output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
	} else if( codepoint <= 0xffff ) {
		output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
		output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
	} else {
		output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
		output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
	}
}

int ModifierParameter( const TerminalKeyEvent& event ) noexcept
{
	return 1 + (event.shift ? 1 : 0) + (event.alt ? 2 : 0) + (event.control ? 4 : 0);
}

std::string ModifiedCsi( char final, const TerminalKeyEvent& event )
{
	if( !event.shift && !event.alt && !event.control ) return std::string("\x1b[") + final;
	char buffer[24]{};
	std::snprintf(buffer, sizeof(buffer), "\x1b[1;%d%c", ModifierParameter(event), final);
	return buffer;
}

std::string ModifiedTilde( int number, const TerminalKeyEvent& event )
{
	char buffer[24]{};
	if( !event.shift && !event.alt && !event.control ) std::snprintf(buffer, sizeof(buffer), "\x1b[%d~", number);
	else std::snprintf(buffer, sizeof(buffer), "\x1b[%d;%d~", number, ModifierParameter(event));
	return buffer;
}

bool ShouldReportMouse( const TerminalMouseEvent& event, const TerminalModes& modes ) noexcept
{
	if( event.action == TerminalMouseAction::WheelUp || event.action == TerminalMouseAction::WheelDown ) {
		return modes.mouseButtonTracking || modes.mouseDragTracking || modes.mouseAnyEventTracking;
	}
	if( event.action != TerminalMouseAction::Move ) return modes.mouseButtonTracking || modes.mouseDragTracking || modes.mouseAnyEventTracking;
	if( modes.mouseAnyEventTracking ) return true;
	return modes.mouseDragTracking && event.button < 3;
}

unsigned int MouseCode( const TerminalMouseEvent& event ) noexcept
{
	unsigned int code = 0;
	switch( event.action ) {
	case TerminalMouseAction::WheelUp: code = 64; break;
	case TerminalMouseAction::WheelDown: code = 65; break;
	case TerminalMouseAction::Release: code = 3; break;
	case TerminalMouseAction::Move: code = (event.button < 3 ? event.button : 3) | 32; break;
	case TerminalMouseAction::Press: code = std::min(event.button, 2u); break;
	}
	if( event.shift ) code |= 4;
	if( event.alt ) code |= 8;
	if( event.control ) code |= 16;
	return code;
}

} // namespace

std::string EncodeTerminalText( std::wstring_view text )
{
	std::string result;
	result.reserve(text.size());
	for( std::size_t index = 0; index < text.size(); ++index ) {
		char32_t codepoint = static_cast<char32_t>(text[index]);
		if( codepoint >= 0xd800 && codepoint <= 0xdbff ) {
			if( index + 1 < text.size() ) {
				const char32_t low = static_cast<char32_t>(text[index + 1]);
				if( low >= 0xdc00 && low <= 0xdfff ) {
					codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
					++index;
				} else codepoint = 0xfffd;
			} else codepoint = 0xfffd;
		} else if( codepoint >= 0xdc00 && codepoint <= 0xdfff ) {
			codepoint = 0xfffd;
		}
		AppendUtf8(result, codepoint);
	}
	return result;
}

std::string EncodeTerminalKey( const TerminalKeyEvent& event )
{
	std::string result;
	switch( event.virtualKey ) {
	case VK_RETURN: result = "\r"; break;
	case VK_BACK: result.assign(1, '\x7f'); break;
	case VK_TAB: result = event.shift ? "\x1b[Z" : "\t"; break;
	case VK_ESCAPE: result.assign(1, '\x1b'); break;
	case VK_UP: result = ModifiedCsi('A', event); break;
	case VK_DOWN: result = ModifiedCsi('B', event); break;
	case VK_RIGHT: result = ModifiedCsi('C', event); break;
	case VK_LEFT: result = ModifiedCsi('D', event); break;
	case VK_HOME: result = ModifiedCsi('H', event); break;
	case VK_END: result = ModifiedCsi('F', event); break;
	case VK_INSERT: result = ModifiedTilde(2, event); break;
	case VK_DELETE: result = ModifiedTilde(3, event); break;
	case VK_PRIOR: result = ModifiedTilde(5, event); break;
	case VK_NEXT: result = ModifiedTilde(6, event); break;
	case VK_F1: result = "\x1bOP"; break;
	case VK_F2: result = "\x1bOQ"; break;
	case VK_F3: result = "\x1bOR"; break;
	case VK_F4: result = "\x1bOS"; break;
	case VK_F5: result = ModifiedTilde(15, event); break;
	case VK_F6: result = ModifiedTilde(17, event); break;
	case VK_F7: result = ModifiedTilde(18, event); break;
	case VK_F8: result = ModifiedTilde(19, event); break;
	case VK_F9: result = ModifiedTilde(20, event); break;
	case VK_F10: result = ModifiedTilde(21, event); break;
	case VK_F11: result = ModifiedTilde(23, event); break;
	case VK_F12: result = ModifiedTilde(24, event); break;
	default:
		if( event.control && event.virtualKey >= 'A' && event.virtualKey <= 'Z' ) {
			result.assign(1, static_cast<char>(event.virtualKey - 'A' + 1));
		} else if( event.control && (event.virtualKey == VK_SPACE || event.virtualKey == '2') ) {
			result.assign(1, '\0');
		} else if( event.control && event.virtualKey >= VK_OEM_4 && event.virtualKey <= VK_OEM_7 ) {
			static constexpr std::array<char, 4> controls{ '\x1b', '\x1c', '\x1d', '\x1f' };
			result.assign(1, controls[event.virtualKey - VK_OEM_4]);
		}
		break;
	}
	if( event.alt && !result.empty() && result.front() != '\x1b' ) result.insert(result.begin(), '\x1b');
	return result;
}

std::string EncodeTerminalPaste( std::wstring_view text, bool bracketedPaste )
{
	auto bytes = EncodeTerminalText(text);
	if( !bracketedPaste ) return bytes;
	std::string result = "\x1b[200~";
	result += bytes;
	result += "\x1b[201~";
	return result;
}

std::string EncodeTerminalMouse( const TerminalMouseEvent& event, const TerminalModes& modes )
{
	if( !ShouldReportMouse(event, modes) ) return {};
	const auto code = MouseCode(event);
	const auto column = std::min<std::size_t>(event.column + 1, 9999);
	const auto row = std::min<std::size_t>(event.row + 1, 9999);
	if( modes.mouseSgrEncoding ) {
		char buffer[48]{};
		const char final = event.action == TerminalMouseAction::Release ? 'm' : 'M';
		std::snprintf(buffer, sizeof(buffer), "\x1b[<%u;%zu;%zu%c", code, column, row, final);
		return buffer;
	}
	std::string result = "\x1b[M";
	result.push_back(static_cast<char>(std::min(255u, code + 32)));
	result.push_back(static_cast<char>(std::min<std::size_t>(255, column + 32)));
	result.push_back(static_cast<char>(std::min<std::size_t>(255, row + 32)));
	return result;
}

bool TerminalKeyNeedsTextDelivery( const TerminalKeyEvent& event, bool mapsToCharacter ) noexcept
{
	if( !event.keyDown || !mapsToCharacter ) return false;
	// Ctrl / Alt 付きは制御シーケンスの領分で、EncodeTerminalKey と Alt 印字可能キーの
	// 経路が先に所有する。ここが引き取るのは素の文字入力と Shift 修飾だけ。
	return !event.control && !event.alt;
}

TerminalShortcutAction ResolveTerminalShortcut( const TerminalKeyEvent& event, bool hasSelection ) noexcept
{
	if( event.virtualKey == VK_INSERT && event.shift && !event.alt ) return TerminalShortcutAction::Paste;
	if( !event.control || event.alt ) return TerminalShortcutAction::None;
	if( event.virtualKey == 'C' ) {
		if( event.shift || hasSelection ) return TerminalShortcutAction::Copy;
		return TerminalShortcutAction::SendInterrupt;
	}
	if( event.virtualKey == VK_INSERT ) return TerminalShortcutAction::Copy;
	if( event.virtualKey == 'V' ) return TerminalShortcutAction::Paste;
	return TerminalShortcutAction::None;
}

TerminalRightClickAction ResolveTerminalRightClick( bool hasSelection, bool mouseReporting, bool shift ) noexcept
{
	if( mouseReporting && !shift ) return TerminalRightClickAction::SendToApplication;
	return hasSelection ? TerminalRightClickAction::CopySelection : TerminalRightClickAction::PasteClipboard;
}

} // namespace terminal
