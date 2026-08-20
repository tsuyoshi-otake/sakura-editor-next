/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace terminal {

//! Optional prefix-key dispatchers for users whose muscle memory comes from a
//! terminal multiplexer. VS Code has no equivalent setting, so this is a
//! documented fork extension; see terminal/CLAUDE.md.
enum class TerminalShortcutPreset : std::uint8_t {
	None,
	Tmux,
	Screen,
};

//! Terminal-panel action a preset chord resolves to. Every value maps onto a
//! capability the panel already owns; a preset never invents one.
enum class TerminalPresetAction : std::uint8_t {
	None,
	NewTerminal,
	SplitRight,
	SplitDown,
	ClosePane,
	CloseTerminal,
	NextPane,
	PreviousPane,
	NextTerminal,
	PreviousTerminal,
	SelectTerminal,
	FocusTerminalList,
};

struct TerminalPresetKey {
	std::uint32_t virtualKey{};
	bool shift{};
	bool control{};
	bool alt{};
};

struct TerminalPresetResolution {
	//! True when the panel owns this key and must not forward it to the shell.
	bool consumed{};
	//! Prefix state after this key. Always false once an action is produced.
	bool prefixArmed{};
	TerminalPresetAction action{ TerminalPresetAction::None };
	//! Zero-based terminal index for TerminalPresetAction::SelectTerminal.
	std::size_t terminalIndex{};
};

//! The prefix virtual key: Ctrl+B for tmux, Ctrl+A for GNU Screen.
[[nodiscard]] std::uint32_t TerminalShortcutPresetPrefixKey( TerminalShortcutPreset preset ) noexcept;

//! Resolves one key against the preset's chord table.
//! Pressing the prefix twice is deliberately reported as not consumed, so the
//! ordinary encoder sends the literal control byte to the shell exactly as
//! tmux's send-prefix and Screen's `C-a a` do.
[[nodiscard]] TerminalPresetResolution ResolveTerminalPresetKey(
	TerminalShortcutPreset preset, bool prefixArmed, const TerminalPresetKey& key ) noexcept;

//! Stable persisted identifiers: `none`, `tmux`, `screen`.
[[nodiscard]] std::wstring_view TerminalShortcutPresetId( TerminalShortcutPreset preset ) noexcept;
[[nodiscard]] std::optional<TerminalShortcutPreset> ParseTerminalShortcutPreset( std::wstring_view id ) noexcept;

} // namespace terminal
