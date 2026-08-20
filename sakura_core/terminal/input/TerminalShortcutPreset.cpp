/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/input/TerminalShortcutPreset.h"

#include <windows.h>

namespace terminal {
namespace {

//! Shifted `"` is Shift+VK_OEM_7 on a US layout and Shift+'2' on a JIS layout.
//! Both are accepted so the chord survives a layout change; the same reasoning
//! covers `|`, which is Shift+VK_OEM_5 on both.
bool IsDoubleQuote( const TerminalPresetKey& key ) noexcept
{
	return key.shift && (key.virtualKey == VK_OEM_7 || key.virtualKey == '2');
}

bool IsVerticalBar( const TerminalPresetKey& key ) noexcept
{
	return key.shift && key.virtualKey == VK_OEM_5;
}

bool IsLetter( const TerminalPresetKey& key, std::uint32_t letter ) noexcept
{
	// Shift is ignored for letters. A preset that demanded an exact case would
	// silently drop the chord under Caps Lock or a layout that shifts the key.
	return key.virtualKey == letter;
}

TerminalPresetResolution Action( TerminalPresetAction action, std::size_t index = 0 ) noexcept
{
	TerminalPresetResolution resolution;
	resolution.consumed = true;
	resolution.prefixArmed = false;
	resolution.action = action;
	resolution.terminalIndex = index;
	return resolution;
}

TerminalPresetResolution Swallowed() noexcept
{
	TerminalPresetResolution resolution;
	resolution.consumed = true;
	return resolution;
}

TerminalPresetResolution PassThrough( bool prefixArmed = false ) noexcept
{
	TerminalPresetResolution resolution;
	resolution.prefixArmed = prefixArmed;
	return resolution;
}

std::optional<std::size_t> DigitIndex( const TerminalPresetKey& key ) noexcept
{
	if( key.shift || key.control || key.alt ) return std::nullopt;
	if( key.virtualKey >= '0' && key.virtualKey <= '9' ) return static_cast<std::size_t>(key.virtualKey - '0');
	if( key.virtualKey >= VK_NUMPAD0 && key.virtualKey <= VK_NUMPAD9 ) {
		return static_cast<std::size_t>(key.virtualKey - VK_NUMPAD0);
	}
	return std::nullopt;
}

TerminalPresetResolution ResolveTmux( const TerminalPresetKey& key ) noexcept
{
	if( const auto index = DigitIndex(key) ) return Action(TerminalPresetAction::SelectTerminal, *index);
	if( key.shift && key.virtualKey == '5' ) return Action(TerminalPresetAction::SplitRight);
	if( IsDoubleQuote(key) ) return Action(TerminalPresetAction::SplitDown);
	if( key.shift && key.virtualKey == '7' ) return Action(TerminalPresetAction::CloseTerminal);
	if( IsLetter(key, 'C') ) return Action(TerminalPresetAction::NewTerminal);
	if( IsLetter(key, 'X') ) return Action(TerminalPresetAction::ClosePane);
	if( IsLetter(key, 'O') ) return Action(TerminalPresetAction::NextPane);
	if( key.virtualKey == VK_OEM_1 && !key.shift ) return Action(TerminalPresetAction::PreviousPane);
	if( IsLetter(key, 'N') ) return Action(TerminalPresetAction::NextTerminal);
	if( IsLetter(key, 'P') ) return Action(TerminalPresetAction::PreviousTerminal);
	if( IsLetter(key, 'W') ) return Action(TerminalPresetAction::FocusTerminalList);
	return Swallowed();
}

TerminalPresetResolution ResolveScreen( const TerminalPresetKey& key ) noexcept
{
	if( const auto index = DigitIndex(key) ) return Action(TerminalPresetAction::SelectTerminal, *index);
	if( IsVerticalBar(key) ) return Action(TerminalPresetAction::SplitRight);
	if( IsDoubleQuote(key) ) return Action(TerminalPresetAction::FocusTerminalList);
	if( IsLetter(key, 'S') ) return Action(TerminalPresetAction::SplitDown);
	if( IsLetter(key, 'C') ) return Action(TerminalPresetAction::NewTerminal);
	if( IsLetter(key, 'X') ) return Action(TerminalPresetAction::ClosePane);
	if( IsLetter(key, 'K') ) return Action(TerminalPresetAction::CloseTerminal);
	if( key.virtualKey == VK_TAB ) return Action(TerminalPresetAction::NextPane);
	if( IsLetter(key, 'N') ) return Action(TerminalPresetAction::NextTerminal);
	if( IsLetter(key, 'P') ) return Action(TerminalPresetAction::PreviousTerminal);
	return Swallowed();
}

bool IsModifierKey( std::uint32_t virtualKey ) noexcept
{
	switch( virtualKey ) {
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
	case VK_MENU: case VK_LMENU: case VK_RMENU:
	case VK_CAPITAL: case VK_LWIN: case VK_RWIN:
		return true;
	default:
		return false;
	}
}

} // namespace

std::uint32_t TerminalShortcutPresetPrefixKey( TerminalShortcutPreset preset ) noexcept
{
	switch( preset ) {
	case TerminalShortcutPreset::Tmux: return 'B';
	case TerminalShortcutPreset::Screen: return 'A';
	case TerminalShortcutPreset::None: break;
	}
	return 0;
}

TerminalPresetResolution ResolveTerminalPresetKey(
	TerminalShortcutPreset preset, bool prefixArmed, const TerminalPresetKey& key ) noexcept
{
	if( preset == TerminalShortcutPreset::None ) return PassThrough();
	const auto prefixKey = TerminalShortcutPresetPrefixKey(preset);
	const bool isPrefixChord = key.control && !key.alt && key.virtualKey == prefixKey;
	if( !prefixArmed ) {
		if( isPrefixChord ) {
			TerminalPresetResolution resolution;
			resolution.consumed = true;
			resolution.prefixArmed = true;
			return resolution;
		}
		return PassThrough();
	}
	// A held modifier is not the second key of the chord; the prefix stays armed
	// so Ctrl+B then Shift+5 still resolves.
	if( IsModifierKey(key.virtualKey) ) return PassThrough(true);
	// send-prefix: the literal control byte reaches the shell through the ordinary
	// encoder, which is also what a real multiplexer running inside needs.
	if( isPrefixChord ) return PassThrough();
	if( key.virtualKey == VK_ESCAPE || (key.control && key.virtualKey == 'G') ) return Swallowed();
	if( key.control || key.alt ) return Swallowed();
	return preset == TerminalShortcutPreset::Tmux ? ResolveTmux(key) : ResolveScreen(key);
}

std::wstring_view TerminalShortcutPresetId( TerminalShortcutPreset preset ) noexcept
{
	switch( preset ) {
	case TerminalShortcutPreset::Tmux: return L"tmux";
	case TerminalShortcutPreset::Screen: return L"screen";
	case TerminalShortcutPreset::None: break;
	}
	return L"none";
}

std::optional<TerminalShortcutPreset> ParseTerminalShortcutPreset( std::wstring_view id ) noexcept
{
	if( id == L"none" ) return TerminalShortcutPreset::None;
	if( id == L"tmux" ) return TerminalShortcutPreset::Tmux;
	if( id == L"screen" ) return TerminalShortcutPreset::Screen;
	return std::nullopt;
}

} // namespace terminal
