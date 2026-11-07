/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalAgentPresence.h"

#include <algorithm>
#include <cwctype>

namespace terminal {
namespace {

constexpr bool IsBlank( wchar_t character ) noexcept
{
	return character == L' ' || character == L'\t';
}

std::wstring_view Trim( std::wstring_view text ) noexcept
{
	while( !text.empty() && IsBlank(text.front()) ) text.remove_prefix(1);
	while( !text.empty() && IsBlank(text.back()) ) text.remove_suffix(1);
	return text;
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

bool EqualsIgnoreCase( std::wstring_view left, std::wstring_view right ) noexcept
{
	return left.size() == right.size()
		&& std::equal(left.begin(), left.end(), right.begin(), MatchesIgnoreCase);
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

//! The tab label's rule (TerminalTabPresentation): C0 controls and DEL.
constexpr bool IsControlCharacter( wchar_t character ) noexcept
{
	return character < 0x20 || character == 0x7F;
}

constexpr bool IsSessionTitleBlank( wchar_t character ) noexcept
{
	return IsBlank(character) || character == 0x00A0 || character == 0x3000;
}

//! Status decoration an agent CLI puts in front of its session name. Claude
//! Code shows U+2733 when idle and animates U+25D0..U+25D3 while working.
constexpr bool IsLeadingStatusGlyph( wchar_t character ) noexcept
{
	return character == L'*' || character == 0x00B7 || character == 0x2022 || character == 0x2219
		|| (character >= 0x25A0 && character <= 0x25FF)	// Geometric Shapes
		|| (character >= 0x2600 && character <= 0x27BF)	// Miscellaneous Symbols, Dingbats
		|| (character >= 0x2800 && character <= 0x28FF);	// Braille Patterns (spinners)
}

std::optional<TerminalAgentKind> ResolveKind(
	const std::optional<TerminalAgentKind> titleKind,
	const std::span<const TerminalBackendJobProcess> jobProcesses ) noexcept
{
	const TerminalBackendJobProcess* outermost = nullptr;
	std::optional<TerminalAgentKind> outermostKind;
	for( const auto& process : jobProcesses ) {
		const auto kind = TerminalAgentKindFromImageFileName(process.imageFileName);
		if( !kind ) continue;
		if( outermost == nullptr || process.creationTime < outermost->creationTime ) {
			outermost = &process;
			outermostKind = kind;
		}
	}
	return outermostKind ? outermostKind : titleKind;
}

//! Pi 1.3.0 writes `<U+03C0> - <folder>` or `<U+03C0> - <session> - <folder>`
//! through OSC 0 (an app rebranded through piConfigName writes its own name
//! instead and is not recognized). Pi's own prefix is checked first, so a
//! folder named after another product does not override it.
std::optional<TerminalAgentKind> PresenceKindFromTitle( const std::wstring_view sequenceTitle ) noexcept
{
	const auto trimmed = Trim(sequenceTitle);
	if( trimmed.size() <= kMaximumTerminalAgentTitleLength && trimmed.starts_with(L"\u03C0 -") ) {
		return TerminalAgentKind::Pi;
	}
	return TerminalAgentKindFromTitle(sequenceTitle);
}

} // namespace

std::wstring_view TerminalAgentDisplayName( const TerminalAgentKind kind ) noexcept
{
	switch( kind ) {
	case TerminalAgentKind::ClaudeCode: return L"Claude Code";
	case TerminalAgentKind::CommandCode: return L"Command Code";
	case TerminalAgentKind::Codex: return L"Codex";
	case TerminalAgentKind::Copilot: return L"Copilot";
	case TerminalAgentKind::Devin: return L"Devin";
	case TerminalAgentKind::Gemini: return L"Gemini";
	case TerminalAgentKind::OpenCode: return L"OpenCode";
	case TerminalAgentKind::Pi: return L"Pi";
	}
	return {};
}

std::optional<TerminalAgentKind> TerminalAgentKindFromImageFileName(
	const std::wstring_view imageFileName ) noexcept
{
	if( EqualsIgnoreCase(imageFileName, L"claude.exe") ) return TerminalAgentKind::ClaudeCode;
	if( EqualsIgnoreCase(imageFileName, L"codex.exe") ) return TerminalAgentKind::Codex;
	if( EqualsIgnoreCase(imageFileName, L"devin.exe") ) return TerminalAgentKind::Devin;
	if( EqualsIgnoreCase(imageFileName, L"opencode.exe") ) return TerminalAgentKind::OpenCode;
	return std::nullopt;
}

std::optional<TerminalAgentKind> TerminalAgentKindFromTitle(
	const std::wstring_view sequenceTitle ) noexcept
{
	const auto trimmed = Trim(sequenceTitle);
	if( trimmed.empty() || trimmed.size() > kMaximumTerminalAgentTitleLength ) return std::nullopt;
	// pwsh reports its working directory through OSC 0. A rooted path is a shell
	// title, never an Agent CLI announcing a product name. Do not reject a valid
	// upstream Agent CLI title merely because it contains punctuation such as
	// `Claude Code: project`.
	if( IsPathShapedTitle(trimmed) ) return std::nullopt;
	// These are the exact OSC-title patterns used by current VS Code. Codex is
	// intentionally absent: upstream allows it by detected shell type, but the
	// CLI does not report an OSC title, so a raw title cannot prove that identity.
	if( ContainsSpacedWordsIgnoreCase(trimmed, L"claude", L"code") ) return TerminalAgentKind::ClaudeCode;
	if( ContainsSpacedWordsIgnoreCase(trimmed, L"command", L"code") ) return TerminalAgentKind::CommandCode;
	if( ContainsWordIgnoreCase(trimmed, L"copilot") ) return TerminalAgentKind::Copilot;
	if( ContainsWordIgnoreCase(trimmed, L"gemini") ) return TerminalAgentKind::Gemini;
	return std::nullopt;
}

std::wstring TerminalAgentSessionTitle( const std::wstring_view sequenceTitle )
{
	std::wstring cleaned;
	cleaned.reserve(sequenceTitle.size());
	for( const wchar_t character : sequenceTitle ) {
		if( !IsControlCharacter(character) ) cleaned.push_back(character);
	}
	std::wstring_view view(cleaned);
	while( !view.empty() && (IsSessionTitleBlank(view.front()) || IsLeadingStatusGlyph(view.front())) ) {
		view.remove_prefix(1);
	}
	while( !view.empty() && IsSessionTitleBlank(view.back()) ) view.remove_suffix(1);
	if( view.empty() || view.size() > kMaximumTerminalAgentTitleLength || IsPathShapedTitle(view) ) {
		return {};
	}
	return std::wstring(view);
}

bool TerminalAgentPresenceTracker::Resolve( TabState& state ) noexcept
{
	const auto next = ResolveKind(state.titleKind, state.agentProcesses);
	const bool changed = next != state.resolved;
	state.resolved = next;
	return changed;
}

bool TerminalAgentPresenceTracker::ApplyTitle(
	const std::uint64_t tabId, const std::wstring_view sequenceTitle )
{
	auto& state = m_tabs[tabId];
	state.titleKind = PresenceKindFromTitle(sequenceTitle);
	auto sessionTitle = TerminalAgentSessionTitle(sequenceTitle);
	const bool sessionTitleChanged = sessionTitle != state.sessionTitle;
	state.sessionTitle = std::move(sessionTitle);
	const bool presenceChanged = Resolve(state);
	return presenceChanged || (sessionTitleChanged && state.resolved.has_value());
}

bool TerminalAgentPresenceTracker::ApplyJobObservation(
	const std::uint64_t tabId, const std::optional<TerminalBackendJobProcesses>& observation )
{
	auto& state = m_tabs[tabId];
	state.agentProcesses.clear();
	if( observation ) {
		for( const auto& process : observation->processes ) {
			if( TerminalAgentKindFromImageFileName(process.imageFileName) ) {
				state.agentProcesses.push_back(process);
			}
		}
	}
	return Resolve(state);
}

bool TerminalAgentPresenceTracker::Retain( const std::span<const std::uint64_t> liveTabIds )
{
	bool changed = false;
	for( auto current = m_tabs.begin(); current != m_tabs.end(); ) {
		if( std::find(liveTabIds.begin(), liveTabIds.end(), current->first) != liveTabIds.end() ) {
			++current;
			continue;
		}
		changed = changed || current->second.resolved.has_value();
		current = m_tabs.erase(current);
	}
	return changed;
}

std::optional<TerminalAgentKind> TerminalAgentPresenceTracker::Presence(
	const std::uint64_t tabId ) const noexcept
{
	const auto found = m_tabs.find(tabId);
	return found == m_tabs.end() ? std::nullopt : found->second.resolved;
}

std::wstring TerminalAgentPresenceTracker::SessionTitle( const std::uint64_t tabId ) const
{
	const auto found = m_tabs.find(tabId);
	return found == m_tabs.end() ? std::wstring{} : found->second.sessionTitle;
}

std::vector<std::uint32_t> TerminalAgentPresenceTracker::AgentProcessIds(
	const std::uint64_t tabId ) const
{
	std::vector<std::uint32_t> result;
	const auto found = m_tabs.find(tabId);
	if( found == m_tabs.end() ) return result;
	result.reserve(found->second.agentProcesses.size());
	for( const auto& process : found->second.agentProcesses ) result.push_back(process.processId);
	return result;
}

} // namespace terminal
