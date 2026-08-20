/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalTabPresentation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <vector>

namespace terminal {
namespace {

//! Recognized Agent CLI product titles, compared case-insensitively against the
//! trimmed OSC title. The list is intentionally short: every entry is a CLI
//! this repository has actually observed announcing itself this way.
constexpr std::array kAgentCliTitles{
	std::wstring_view(L"claude code"),
	std::wstring_view(L"claude"),
};

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

//! Allocation-free so the recognition predicate can stay noexcept.
bool EqualsIgnoreCase( std::wstring_view left, std::wstring_view right ) noexcept
{
	if( left.size() != right.size() ) return false;
	for( std::size_t index = 0; index < left.size(); ++index ) {
		const auto lowered = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(left[index])));
		if( lowered != right[index] ) return false;
	}
	return true;
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
//! caller keeps the `${name}` text verbatim the way VS Code does. A known but
//! unavailable variable resolves to an empty string so the surrounding
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
			literal.append(templateText.substr(index, nameEnd + 1 - index));
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
	auto trimmed = std::wstring(Trim(text));
	if( trimmed.size() > kMaximumTerminalTabTextLength ) trimmed.resize(kMaximumTerminalTabTextLength);
	return trimmed;
}

} // namespace

bool IsRecognizedAgentCliTitle( std::wstring_view sequenceTitle ) noexcept
{
	const auto trimmed = Trim(sequenceTitle);
	if( trimmed.empty() || trimmed.size() > kMaximumTerminalTabTextLength ) return false;
	// pwsh reports its working directory through OSC 0. A path is a shell title,
	// never an Agent CLI announcing a product name.
	if( trimmed.find_first_of(L"\\/:") != std::wstring_view::npos ) return false;
	return std::any_of(kAgentCliTitles.begin(), kAgentCliTitles.end(),
		[trimmed](std::wstring_view candidate) { return EqualsIgnoreCase(trimmed, candidate); });
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

} // namespace terminal
