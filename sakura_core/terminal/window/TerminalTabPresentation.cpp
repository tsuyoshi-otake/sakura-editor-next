/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalTabPresentation.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <vector>

namespace terminal {
namespace {

constexpr bool IsBlank( wchar_t character ) noexcept
{
	return character == L' ' || character == L'\t';
}

constexpr bool IsControl( wchar_t character ) noexcept
{
	return character < 0x20 || character == 0x7F;
}

std::wstring StripControlCharacters( std::wstring_view text )
{
	std::wstring result;
	result.reserve(text.size());
	for( const wchar_t character : text ) {
		// A tab would survive DrawTextW as an expanded gap, so it is dropped with
		// the rest of the C0 range rather than translated.
		if( !IsControl(character) ) result.push_back(character);
	}
	return result;
}

std::wstring_view Trim( std::wstring_view text ) noexcept
{
	while( !text.empty() && IsBlank(text.front()) ) text.remove_prefix(1);
	while( !text.empty() && IsBlank(text.back()) ) text.remove_suffix(1);
	return text;
}

bool IsBlankText( std::wstring_view text ) noexcept
{
	return Trim(text).empty();
}

bool IsWordCharacter( wchar_t character ) noexcept
{
	return (character >= L'a' && character <= L'z')
		|| (character >= L'A' && character <= L'Z')
		|| (character >= L'0' && character <= L'9')
		|| character == L'_';
}

bool MatchesIgnoreCase( wchar_t left, wchar_t right ) noexcept
{
	return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(left)))
		== static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(right)));
}

//! Implements the upstream `/claude\\s*code/i` and `/command\\s*code/i`
//! patterns without pulling a regular-expression engine into the resolver.
bool ContainsSpacedWordsIgnoreCase( std::wstring_view text,
	std::wstring_view first, std::wstring_view second ) noexcept
{
	for( std::size_t begin = 0; begin < text.size(); ++begin ) {
		if( begin + first.size() > text.size() ) break;
		bool firstMatches = true;
		for( std::size_t index = 0; index < first.size(); ++index ) {
			if( !MatchesIgnoreCase(text[begin + index], first[index]) ) {
				firstMatches = false;
				break;
			}
		}
		if( !firstMatches ) continue;
		std::size_t secondBegin = begin + first.size();
		while( secondBegin < text.size() && IsBlank(text[secondBegin]) ) ++secondBegin;
		if( secondBegin + second.size() > text.size() ) continue;
		bool secondMatches = true;
		for( std::size_t index = 0; index < second.size(); ++index ) {
			if( !MatchesIgnoreCase(text[secondBegin + index], second[index]) ) {
				secondMatches = false;
				break;
			}
		}
		if( secondMatches ) return true;
	}
	return false;
}

bool ContainsWordIgnoreCase( std::wstring_view text, std::wstring_view word ) noexcept
{
	if( word.empty() || text.size() < word.size() ) return false;
	for( std::size_t begin = 0; begin + word.size() <= text.size(); ++begin ) {
		if( begin > 0 && IsWordCharacter(text[begin - 1]) ) continue;
		if( begin + word.size() < text.size() && IsWordCharacter(text[begin + word.size()]) ) continue;
		bool matches = true;
		for( std::size_t index = 0; index < word.size(); ++index ) {
			if( !MatchesIgnoreCase(text[begin + index], word[index]) ) {
				matches = false;
				break;
			}
		}
		if( matches ) return true;
	}
	return false;
}

bool IsPathShapedTitle( std::wstring_view text ) noexcept
{
	if( text.empty() ) return false;
	if( text.front() == L'/' ) return true;
	if( text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\' ) return true;
	if( text.size() >= 3
		&& ((text[0] >= L'a' && text[0] <= L'z') || (text[0] >= L'A' && text[0] <= L'Z'))
		&& text[1] == L':' && (text[2] == L'/' || text[2] == L'\\') ) return true;
	return text.starts_with(L"./") || text.starts_with(L".\\")
		|| text.starts_with(L"../") || text.starts_with(L"..\\");
}

int ScaleDip( const int dip, const unsigned int dpi ) noexcept
{
	const auto effectiveDpi = dpi == 0 ? 96u : dpi;
	const auto scaled = (static_cast<std::int64_t>(dip) * effectiveDpi + 48) / 96;
	return static_cast<int>(std::clamp<std::int64_t>(scaled, 0, std::numeric_limits<int>::max()));
}

enum class SegmentKind : std::uint8_t {
	Text,
	Value,
	Separator,
};

struct Segment final {
	SegmentKind kind{ SegmentKind::Text };
	std::wstring text;
};

bool IsVariableNameCharacter( wchar_t character ) noexcept
{
	return (character >= L'a' && character <= L'z')
		|| (character >= L'A' && character <= L'Z')
		|| (character >= L'0' && character <= L'9');
}

//! Resolves one variable name.
//!
//! `std::nullopt` means the name is not a variable this build knows, and the
//! caller omits that segment, matching VS Code's labels.template tokenizer. A
//! known but unavailable variable resolves to an empty string so surrounding
//! conditional separators collapse.
std::optional<std::wstring> ResolveVariable( std::wstring_view name,
	const TerminalTabPresentationContext& context )
{
	const auto optional = []( const std::optional<std::wstring>& value ) {
		return value ? *value : std::wstring();
	};
	if( name == L"process" ) return context.processName;
	if( name == L"sequence" ) return context.sequenceTitle;
	if( name == L"cwd" ) {
		if( context.currentCwd ) return *context.currentCwd;
		return optional(context.initialCwd);
	}
	if( name == L"cwdFolder" ) return optional(context.cwdFolder);
	if( name == L"workspaceFolder" ) return optional(context.workspaceFolder);
	if( name == L"workspaceFolderName" ) return optional(context.workspaceFolderName);
	if( name == L"local" ) return optional(context.local);
	if( name == L"task" ) return optional(context.task);
	if( name == L"shellType" ) return optional(context.shellType);
	if( name == L"progress" ) return optional(context.progress);
	if( name == L"shellCommand" ) return optional(context.shellCommand);
	if( name == L"shellPromptInput" ) return optional(context.shellPromptInput);
	return std::nullopt;
}

std::vector<Segment> Tokenize( std::wstring_view templateText,
	const TerminalTabPresentationContext& context )
{
	std::vector<Segment> segments;
	std::wstring literal;
	const auto flushLiteral = [&segments, &literal] {
		if( literal.empty() ) return;
		segments.push_back({ SegmentKind::Text, literal });
		literal.clear();
	};
	std::size_t index = 0;
	while( index < templateText.size() ) {
		if( templateText[index] != L'$' || index + 1 >= templateText.size() || templateText[index + 1] != L'{' ) {
			literal.push_back(templateText[index]);
			++index;
			continue;
		}
		std::size_t nameBegin = index + 2;
		std::size_t nameEnd = nameBegin;
		while( nameEnd < templateText.size() && IsVariableNameCharacter(templateText[nameEnd]) ) ++nameEnd;
		if( nameEnd >= templateText.size() || templateText[nameEnd] != L'}' || nameEnd == nameBegin ) {
			// Unterminated or malformed: the raw characters stay literal text.
			literal.push_back(templateText[index]);
			++index;
			continue;
		}
		const auto name = templateText.substr(nameBegin, nameEnd - nameBegin);
		if( name == L"separator" ) {
			flushLiteral();
			segments.push_back({ SegmentKind::Separator, {} });
		} else if( const auto value = ResolveVariable(name, context) ) {
			flushLiteral();
			segments.push_back({ SegmentKind::Value, StripControlCharacters(*value) });
		} else {
			// Unknown variables are omitted. Keeping their source text would make
			// a title that upstream resolves to `${process}` fall through to a
			// visibly invalid literal instead.
		}
		index = nameEnd + 1;
	}
	flushLiteral();
	return segments;
}

std::wstring Join( const std::vector<Segment>& segments, std::wstring_view separator )
{
	std::wstring result;
	std::wstring pendingBlankText;
	bool hasContent = false;
	bool pendingSeparator = false;
	for( const auto& segment : segments ) {
		if( segment.kind == SegmentKind::Separator ) {
			// A separator before any content, or a run of separators produced by
			// empty variables between them, collapses to at most one.
			if( hasContent ) pendingSeparator = true;
			continue;
		}
		const bool blank = IsBlankText(segment.text);
		if( segment.kind == SegmentKind::Value && segment.text.empty() ) continue;
		if( blank ) {
			// Whitespace-only literal glue only survives between two real values.
			pendingBlankText.append(segment.text);
			continue;
		}
		if( hasContent ) {
			if( pendingSeparator ) {
				result.append(separator);
			} else {
				result.append(pendingBlankText);
			}
		}
		pendingBlankText.clear();
		pendingSeparator = false;
		result.append(segment.text);
		hasContent = true;
	}
	return result;
}

std::wstring Finalize( std::wstring text )
{
	auto trimmed = std::wstring(Trim(StripControlCharacters(text)));
	if( trimmed.size() > kMaximumTerminalTabTextLength ) trimmed.resize(kMaximumTerminalTabTextLength);
	return trimmed;
}

} // namespace

namespace {

TerminalTabPresentationContext ContextFromSnapshot(
	const TerminalTabPresentationSnapshot& snapshot )
{
	TerminalTabPresentationContext context;
	context.processName = snapshot.ProcessName().empty()
		? snapshot.ProfileLabel()
		: snapshot.ProcessName();
	context.sequenceTitle = snapshot.SequenceTitle();
	if( !snapshot.InitialWorkingDirectory().empty() ) {
		context.initialCwd = snapshot.InitialWorkingDirectory();
	}
	context.recognizedAgentCli = IsRecognizedAgentCliTitle(context.sequenceTitle);
	return context;
}

} // namespace

bool IsRecognizedAgentCliTitle( std::wstring_view sequenceTitle ) noexcept
{
	const auto trimmed = Trim(sequenceTitle);
	if( trimmed.empty() || trimmed.size() > kMaximumTerminalTabTextLength ) return false;
	// pwsh reports its working directory through OSC 0. A rooted path is a shell
	// title, never an Agent CLI announcing a product name. Do not reject a valid
	// upstream Agent CLI title merely because it contains punctuation such as
	// `Claude Code: project`.
	if( IsPathShapedTitle(trimmed) ) return false;
	// These are the exact OSC-title patterns used by current VS Code. Codex is
	// intentionally absent: upstream allows it by detected shell type, but the
	// CLI does not report an OSC title, so a raw title cannot prove that identity.
	return ContainsSpacedWordsIgnoreCase(trimmed, L"claude", L"code")
		|| ContainsSpacedWordsIgnoreCase(trimmed, L"command", L"code")
		|| ContainsWordIgnoreCase(trimmed, L"copilot")
		|| ContainsWordIgnoreCase(trimmed, L"gemini");
}

std::optional<TerminalTabsHideCondition> ParseTerminalTabsHideCondition(
	std::wstring_view value ) noexcept
{
	if( value == L"never" ) return TerminalTabsHideCondition::Never;
	if( value == L"singleTerminal" ) return TerminalTabsHideCondition::SingleTerminal;
	if( value == L"singleGroup" ) return TerminalTabsHideCondition::SingleGroup;
	return std::nullopt;
}

std::optional<TerminalTabsLocation> ParseTerminalTabsLocation( std::wstring_view value ) noexcept
{
	if( value == L"left" ) return TerminalTabsLocation::Left;
	if( value == L"right" ) return TerminalTabsLocation::Right;
	return std::nullopt;
}

std::optional<TerminalTabsShowCondition> ParseTerminalTabsShowCondition(
	std::wstring_view value ) noexcept
{
	if( value == L"always" ) return TerminalTabsShowCondition::Always;
	if( value == L"singleTerminal" ) return TerminalTabsShowCondition::SingleTerminal;
	if( value == L"singleTerminalOrNarrow" ) return TerminalTabsShowCondition::SingleTerminalOrNarrow;
	if( value == L"never" ) return TerminalTabsShowCondition::Never;
	return std::nullopt;
}

bool ShouldShowTerminalTabs(
	const TerminalTabPresentationSettings& settings,
	std::size_t terminalCount,
	std::size_t groupCount ) noexcept
{
	if( !settings.tabsEnabled ) return false;
	switch( settings.hideCondition ) {
	case TerminalTabsHideCondition::Never:
		return true;
	case TerminalTabsHideCondition::SingleTerminal:
		return terminalCount > 1;
	case TerminalTabsHideCondition::SingleGroup:
		return groupCount > 1;
	default:
		return false;
	}
}

bool ShouldShowTerminalTabPolicy(
	TerminalTabsShowCondition condition,
	std::size_t groupCount,
	bool tabsNarrow ) noexcept
{
	switch( condition ) {
	case TerminalTabsShowCondition::Always:
		return true;
	case TerminalTabsShowCondition::SingleTerminal:
		return groupCount == 1;
	case TerminalTabsShowCondition::SingleTerminalOrNarrow:
		return groupCount == 1 || tabsNarrow;
	case TerminalTabsShowCondition::Never:
		return false;
	default:
		return false;
	}
}

bool ShouldShowActiveTerminalHeader(
	TerminalTabsShowCondition condition,
	std::size_t groupCount,
	bool tabsNarrow,
	bool activeGroupSplit ) noexcept
{
	if( condition == TerminalTabsShowCondition::SingleTerminalOrNarrow && activeGroupSplit ) return true;
	return ShouldShowTerminalTabPolicy(condition, groupCount, tabsNarrow);
}

TerminalTabRowLayout CalculateTerminalTabRowLayout(
	const TerminalTabRowLayoutInput& input ) noexcept
{
	std::optional<TerminalTabPresentationRect> splitIndent;
	std::optional<TerminalTabPresentationRect> icon;
	std::optional<TerminalTabPresentationRect> title;
	std::optional<TerminalTabPresentationRect> description;
	std::optional<TerminalTabPresentationRect> status;
	const auto& row = input.Row();
	if( row.right <= row.left || row.bottom <= row.top ) {
		return {};
	}

	const int gap = ScaleDip(4, input.Dpi());
	int cursor = row.left;
	int right = row.right;
	const auto width = [&] { return std::max(0, right - cursor); };
	const auto makeRect = [&row]( int left, int top, int rightEdge, int bottom ) {
		return TerminalTabPresentationRect {
			std::clamp(left, row.left, row.right),
			std::clamp(top, row.top, row.bottom),
			std::clamp(rightEdge, row.left, row.right),
			std::clamp(bottom, row.top, row.bottom),
		};
	};

	if( input.IsSplit() ) {
		const int indentWidth = std::min(width(), ScaleDip(12, input.Dpi()));
		splitIndent.emplace(makeRect(cursor, row.top, cursor + indentWidth, row.bottom));
		cursor += indentWidth;
	}

	if( input.HasIcon() ) {
		const int iconWidth = std::min(width(), ScaleDip(16, input.Dpi()));
		if( iconWidth > 0 ) {
			icon.emplace(makeRect(cursor, row.top, cursor + iconWidth, row.bottom));
			cursor += iconWidth;
			if( width() > gap ) cursor += gap;
		}
	}

	// Status is the lowest-priority slot. Keep it only when the title can still
	// retain a real cell after the icon/indent and the status gap; otherwise the
	// title remains readable and the status is omitted entirely.
	if( input.HasStatus() ) {
		const int statusWidth = std::min(width(), ScaleDip(16, input.Dpi()));
		if( statusWidth > 0 && width() > statusWidth + gap + 1 ) {
			right -= statusWidth;
			status.emplace(makeRect(right, row.top, right + statusWidth, row.bottom));
			if( right - cursor > gap ) right -= gap;
		}
	}

	const int available = std::max(0, right - cursor);
	if( input.HasDescription() && available > gap + 1 ) {
		const int titleWidth = std::max(1, static_cast<int>((static_cast<long long>(available) * 2) / 3));
		const int descriptionLeft = std::min(right, cursor + titleWidth + gap);
		title.emplace(makeRect(cursor, row.top, cursor + titleWidth, row.bottom));
		if( descriptionLeft < right ) {
			description.emplace(makeRect(descriptionLeft, row.top, right, row.bottom));
		}
	} else {
		title.emplace(makeRect(cursor, row.top, right, row.bottom));
	}
	return {
		splitIndent.value_or(TerminalTabPresentationRect{}),
		icon.value_or(TerminalTabPresentationRect{}),
		title.value_or(TerminalTabPresentationRect{}),
		description.value_or(TerminalTabPresentationRect{}),
		status.value_or(TerminalTabPresentationRect{}),
	};
}

ResolvedTerminalTabPresentation ResolveTerminalTabPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationContext& context )
{
	const auto separator = StripControlCharacters(settings.separator);
	// The Agent CLI override replaces the whole template exactly once, before any
	// expansion, so a configured title can never be partially merged with the
	// sequence title.
	const bool agentCliOverride = settings.allowAgentCliTitle
		&& context.recognizedAgentCli
		&& !context.sequenceTitle.empty();
	const std::wstring_view titleTemplate = agentCliOverride
		? std::wstring_view(L"${sequence}")
		: std::wstring_view(settings.titleTemplate);

	ResolvedTerminalTabPresentation resolved;
	resolved.title = Finalize(Join(Tokenize(titleTemplate, context), separator));
	if( resolved.title.empty() ) {
		resolved.title = Finalize(StripControlCharacters(context.processName));
	}
	resolved.description = Finalize(Join(Tokenize(settings.descriptionTemplate, context), separator));
	return resolved;
}

ResolvedTerminalTabPresentation ResolveTerminalTabListPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationSnapshot& snapshot )
{
	return ResolveTerminalTabPresentation(settings, ContextFromSnapshot(snapshot));
}

ResolvedTerminalTabPresentation ResolveTerminalTabDropdownPresentation(
	const TerminalTabPresentationSettings& settings,
	const TerminalTabPresentationSnapshot& snapshot )
{
	return ResolveTerminalTabPresentation(settings, ContextFromSnapshot(snapshot));
}

} // namespace terminal
