/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace terminal {

//! The typed values behind terminal.integrated.tabs.hideCondition.
enum class TerminalTabsHideCondition : std::uint8_t {
	Never,
	SingleTerminal,
	SingleGroup,
};

//! The typed values behind terminal.integrated.tabs.location.
enum class TerminalTabsLocation : std::uint8_t {
	Left,
	Right,
};

//! The typed values shared by terminal.integrated.tabs.showActiveTerminal and
//! terminal.integrated.tabs.showActions. The native terminal chrome consumes
//! these values through the pure policy helper below; the enum keeps
//! unsupported states from being represented as ad-hoc booleans.
enum class TerminalTabsShowCondition : std::uint8_t {
	Always,
	SingleTerminal,
	SingleTerminalOrNarrow,
	Never,
};

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
	//! terminal.integrated.tabs.enabled.
	bool tabsEnabled{ true };
	//! terminal.integrated.tabs.hideCondition.
	TerminalTabsHideCondition hideCondition{ TerminalTabsHideCondition::SingleTerminal };
	//! terminal.integrated.tabs.location.
	TerminalTabsLocation location{ TerminalTabsLocation::Right };
	//! terminal.integrated.tabs.showActiveTerminal.
	TerminalTabsShowCondition showActiveTerminal{ TerminalTabsShowCondition::SingleTerminalOrNarrow };
	//! terminal.integrated.tabs.showActions.
	TerminalTabsShowCondition showActions{ TerminalTabsShowCondition::SingleTerminalOrNarrow };
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

//! Snapshot shared by the terminal tab list and the sessions dropdown. It is a
//! value boundary between raw manager state and the presentation resolver; no
//! surface may rebuild a title from the manager fields independently.
class TerminalTabPresentationSnapshot final {
public:
	TerminalTabPresentationSnapshot(
		std::wstring processName,
		std::wstring profileLabel,
		std::wstring sequenceTitle,
		std::wstring initialWorkingDirectory )
		: m_processName(std::move(processName))
		, m_profileLabel(std::move(profileLabel))
		, m_sequenceTitle(std::move(sequenceTitle))
		, m_initialWorkingDirectory(std::move(initialWorkingDirectory))
	{
	}

	[[nodiscard]] const std::wstring& ProcessName() const noexcept { return m_processName; }
	[[nodiscard]] const std::wstring& ProfileLabel() const noexcept { return m_profileLabel; }
	[[nodiscard]] const std::wstring& SequenceTitle() const noexcept { return m_sequenceTitle; }
	[[nodiscard]] const std::wstring& InitialWorkingDirectory() const noexcept { return m_initialWorkingDirectory; }

private:
	std::wstring m_processName;
	std::wstring m_profileLabel;
	std::wstring m_sequenceTitle;
	std::wstring m_initialWorkingDirectory;
};

struct ResolvedTerminalTabPresentation final {
	std::wstring title;
	std::wstring description;

	[[nodiscard]] friend bool operator==( const ResolvedTerminalTabPresentation&,
		const ResolvedTerminalTabPresentation& ) = default;
};

//! Small platform-neutral rectangles used by the terminal tab row layout. The
//! painter converts them to RECT only at the Win32 boundary, keeping geometry
//! tests independent of a window, font, or configuration service.
struct TerminalTabPresentationRect final {
	const int left{};
	const int top{};
	const int right{};
	const int bottom{};

	[[nodiscard]] constexpr int Width() const noexcept { return right - left; }
	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
};

class TerminalTabRowLayoutInput final {
public:
	constexpr TerminalTabRowLayoutInput(
		TerminalTabPresentationRect row,
		unsigned int dpi = 96,
		bool split = false,
		bool hasIcon = false,
		bool hasDescription = false,
		bool hasStatus = false ) noexcept
		: m_row(row)
		, m_dpi(dpi)
		, m_split(split)
		, m_hasIcon(hasIcon)
		, m_hasDescription(hasDescription)
		, m_hasStatus(hasStatus)
	{
	}

	[[nodiscard]] constexpr const TerminalTabPresentationRect& Row() const noexcept { return m_row; }
	[[nodiscard]] constexpr unsigned int Dpi() const noexcept { return m_dpi; }
	[[nodiscard]] constexpr bool IsSplit() const noexcept { return m_split; }
	[[nodiscard]] constexpr bool HasIcon() const noexcept { return m_hasIcon; }
	[[nodiscard]] constexpr bool HasDescription() const noexcept { return m_hasDescription; }
	[[nodiscard]] constexpr bool HasStatus() const noexcept { return m_hasStatus; }

private:
	const TerminalTabPresentationRect m_row;
	const unsigned int m_dpi;
	const bool m_split;
	const bool m_hasIcon;
	const bool m_hasDescription;
	const bool m_hasStatus;
};

class TerminalTabRowLayout final {
public:
	constexpr TerminalTabRowLayout(
		TerminalTabPresentationRect splitIndent = {},
		TerminalTabPresentationRect icon = {},
		TerminalTabPresentationRect title = {},
		TerminalTabPresentationRect description = {},
		TerminalTabPresentationRect status = {} ) noexcept
		: m_splitIndent(splitIndent)
		, m_icon(icon)
		, m_title(title)
		, m_description(description)
		, m_status(status)
	{
	}

	[[nodiscard]] constexpr const TerminalTabPresentationRect& SplitIndent() const noexcept { return m_splitIndent; }
	[[nodiscard]] constexpr const TerminalTabPresentationRect& Icon() const noexcept { return m_icon; }
	[[nodiscard]] constexpr const TerminalTabPresentationRect& Title() const noexcept { return m_title; }
	[[nodiscard]] constexpr const TerminalTabPresentationRect& Description() const noexcept { return m_description; }
	[[nodiscard]] constexpr const TerminalTabPresentationRect& Status() const noexcept { return m_status; }

private:
	const TerminalTabPresentationRect m_splitIndent;
	const TerminalTabPresentationRect m_icon;
	const TerminalTabPresentationRect m_title;
	const TerminalTabPresentationRect m_description;
	const TerminalTabPresentationRect m_status;
};

//! Maximum characters of either resolved field. Templates are user-writable and
//! OSC titles are process-writable, so both are bounded before they reach GDI.
inline constexpr std::size_t kMaximumTerminalTabTextLength = 256;

//! Deliberately narrow allow-list. An Agent CLI announces a product name, so a
//! path-shaped OSC title (what pwsh reports) is never accepted. Do not widen
//! this without evidence that the CLI in question sets the title this way.
[[nodiscard]] bool IsRecognizedAgentCliTitle( std::wstring_view sequenceTitle ) noexcept;

//! Typed configuration parsing helpers. Invalid strings are rejected so the
//! caller can retain the registered VS Code default rather than inventing a
//! fallback policy.
[[nodiscard]] std::optional<TerminalTabsHideCondition> ParseTerminalTabsHideCondition(
	std::wstring_view value ) noexcept;
[[nodiscard]] std::optional<TerminalTabsLocation> ParseTerminalTabsLocation(
	std::wstring_view value ) noexcept;
[[nodiscard]] std::optional<TerminalTabsShowCondition> ParseTerminalTabsShowCondition(
	std::wstring_view value ) noexcept;

//! Pure visibility predicate for terminal.integrated.tabs.hideCondition. The
//! `singleGroup` case intentionally uses groupCount, not terminalCount.
[[nodiscard]] bool ShouldShowTerminalTabs(
	const TerminalTabPresentationSettings& settings,
	std::size_t terminalCount,
	std::size_t groupCount ) noexcept;

//! Pure policy for the active-terminal summary and terminal chrome actions.
//! `tabsNarrow` is the resolved view state, not a configuration lookup. The
//! count is deliberately passed by the caller so this policy remains usable by
//! both the header and a future list action-bar projection.
[[nodiscard]] bool ShouldShowTerminalTabPolicy(
	TerminalTabsShowCondition condition,
	std::size_t terminalCount,
	bool tabsNarrow ) noexcept;

//! Pure bounded row geometry. Lower-priority description space is dropped
//! before the title, and every returned rectangle is non-inverted and contained
//! by the input row.
[[nodiscard]] TerminalTabRowLayout CalculateTerminalTabRowLayout(
	const TerminalTabRowLayoutInput& input ) noexcept;

//! Pure template resolution. Same settings and context always resolve to the
//! same pair, with no dangling or duplicated conditional separator, no control
//! characters, and a title that falls back to `${process}` when it resolves
//! empty. The description may legitimately resolve empty.
[[nodiscard]] ResolvedTerminalTabPresentation ResolveTerminalTabPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationContext& context );

//! Shared snapshot-to-presentation seam used by both native consumers. The
//! wrappers make list/dropdown drift observable in tests while keeping one
//! resolver and one raw-state projection.
[[nodiscard]] ResolvedTerminalTabPresentation ResolveTerminalTabListPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationSnapshot& snapshot );
[[nodiscard]] ResolvedTerminalTabPresentation ResolveTerminalTabDropdownPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationSnapshot& snapshot );

} // namespace terminal
