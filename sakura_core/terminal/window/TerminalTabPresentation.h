/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace terminal {

//! VS Code's terminal.integrated.tabs.* presentation contract.
//!
//! Defaults match upstream VS Code exactly, including the surrounding spaces of
//! the separator. `title` resolves the tab/dropdown label and `description`
//! resolves the dimmed text drawn to its right.
struct TerminalTabPresentationSettings final {
	std::wstring titleTemplate{ L"${process}" };
	std::wstring descriptionTemplate{ L"${task}${separator}${local}${separator}${cwdFolder}" };
	std::wstring separator{ L" - " };
	//! terminal.integrated.tabs.allowAgentCliTitle. When enabled, a recognized
	//! Agent CLI's OSC title wins over the configured title template.
	bool allowAgentCliTitle{ true };
};

//! Values a tab can supply for `${...}` expansion.
//!
//! An unset optional means "this value is not available", which removes the
//! variable and any separator that would be left dangling beside it. Never
//! populate a member by guessing it from another one; an absent value must stay
//! absent until the subsystem that really owns it exists.
struct TerminalTabPresentationContext final {
	//! ${process}. Currently the launch executable stem.
	std::wstring processName;
	//! ${sequence}. The raw OSC 0/2 title, which is not a display title.
	std::wstring sequenceTitle;
	std::optional<std::wstring> initialCwd;
	std::optional<std::wstring> currentCwd;
	std::optional<std::wstring> cwdFolder;
	std::optional<std::wstring> workspaceFolder;
	std::optional<std::wstring> workspaceFolderName;
	std::optional<std::wstring> local;
	std::optional<std::wstring> task;
	std::optional<std::wstring> shellType;
	std::optional<std::wstring> progress;
	std::optional<std::wstring> shellCommand;
	std::optional<std::wstring> shellPromptInput;
	//! True when `sequenceTitle` came from a recognized Agent CLI rather than
	//! from an ordinary shell. See IsRecognizedAgentCliTitle.
	bool recognizedAgentCli{};
};

struct ResolvedTerminalTabPresentation final {
	std::wstring title;
	std::wstring description;

	[[nodiscard]] friend bool operator==( const ResolvedTerminalTabPresentation&,
		const ResolvedTerminalTabPresentation& ) = default;
};

//! Maximum characters of either resolved field. Templates are user-writable and
//! OSC titles are process-writable, so both are bounded before they reach GDI.
inline constexpr std::size_t kMaximumTerminalTabTextLength = 256;

//! Deliberately narrow allow-list. An Agent CLI announces a product name, so a
//! path-shaped OSC title (what pwsh reports) is never accepted. Do not widen
//! this without evidence that the CLI in question sets the title this way.
[[nodiscard]] bool IsRecognizedAgentCliTitle( std::wstring_view sequenceTitle ) noexcept;

//! Pure template resolution. Same settings and context always resolve to the
//! same pair, with no dangling or duplicated conditional separator, no control
//! characters, and a title that falls back to `${process}` when it resolves
//! empty. The description may legitimately resolve empty.
[[nodiscard]] ResolvedTerminalTabPresentation ResolveTerminalTabPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationContext& context );

} // namespace terminal
