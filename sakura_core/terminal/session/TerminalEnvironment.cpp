/*! @file @brief Bounded ConPTY child environment construction. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/session/TerminalEnvironment.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <utility>
#include <windows.h>

namespace terminal {
namespace {

constexpr std::size_t kMaximumOverrides = 64;
constexpr std::size_t kMaximumPathDirectories = 16;
constexpr std::size_t kMaximumVariableNameCharacters = 128;
constexpr std::size_t kMaximumVariableValueCharacters = 16u * 1024u;
// CreateProcessW accepts at most 32,767 characters including the final NUL.
constexpr std::size_t kMaximumEnvironmentCharacters = 32767;

bool ContainsNul( std::wstring_view value ) noexcept
{
	return value.find(L'\0') != std::wstring_view::npos;
}

bool IsValidVariableName( std::wstring_view name ) noexcept
{
	return !name.empty() && name.size() <= kMaximumVariableNameCharacters
		&& name.find(L'=') == std::wstring_view::npos && !ContainsNul(name);
}

bool IsValidVariableValue( std::wstring_view value ) noexcept
{
	return value.size() <= kMaximumVariableValueCharacters && !ContainsNul(value);
}

bool IsHiddenDriveEntry( std::wstring_view value ) noexcept
{
	return value.size() >= 4 && value[0] == L'=' && value[2] == L':' && value[3] == L'=';
}

std::wstring_view EntryName( std::wstring_view entry ) noexcept
{
	if( IsHiddenDriveEntry(entry) ) return entry.substr(0, 3);
	const auto separator = entry.find(L'=');
	return separator == std::wstring_view::npos ? std::wstring_view{} : entry.substr(0, separator);
}

bool EqualName( std::wstring_view left, std::wstring_view right ) noexcept
{
	if( left.size() != right.size() ) return false;
	return _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::vector<std::wstring>::iterator FindEntry(
	std::vector<std::wstring>& entries,
	std::wstring_view name ) noexcept
{
	return std::find_if(entries.begin(), entries.end(), [name](const std::wstring& entry) {
		return EqualName(EntryName(entry), name);
	});
}

std::vector<std::wstring>::const_iterator FindEntry(
	const std::vector<std::wstring>& entries,
	std::wstring_view name ) noexcept
{
	return std::find_if(entries.begin(), entries.end(), [name](const std::wstring& entry) {
		return EqualName(EntryName(entry), name);
	});
}

void SetEntry( std::vector<std::wstring>& entries, std::wstring_view name, std::wstring_view value )
{
	std::wstring entry;
	entry.reserve(name.size() + value.size() + 1);
	entry.append(name);
	entry.push_back(L'=');
	entry.append(value);
	const auto found = FindEntry(entries, name);
	if( found == entries.end() ) entries.emplace_back(std::move(entry));
	else *found = std::move(entry);
}

void RemoveEntry( std::vector<std::wstring>& entries, std::wstring_view name )
{
	entries.erase(std::remove_if(entries.begin(), entries.end(), [name](const std::wstring& entry) {
		return EqualName(EntryName(entry), name);
	}), entries.end());
}

bool ContainsOverrideName(
	std::span<const TerminalEnvironmentOverride> overrides,
	std::size_t before,
	std::wstring_view name ) noexcept
{
	for( std::size_t index = 0; index < before; ++index ) {
		if( EqualName(overrides[index].name, name) ) return true;
	}
	return false;
}

bool IsValidPathDirectory( std::wstring_view value )
{
	if( value.empty() || value.size() > kMaximumVariableValueCharacters || ContainsNul(value)
		|| value.find(L';') != std::wstring_view::npos ) return false;
	return std::filesystem::path(value).is_absolute();
}

std::wstring NormalizePathDirectory( std::wstring_view value )
{
	auto normalized = std::filesystem::path(value).lexically_normal().native();
	while( normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/') ) {
		normalized.pop_back();
	}
	return normalized;
}

bool PathContainsDirectory( std::wstring_view path, std::wstring_view directory )
{
	const auto wanted = NormalizePathDirectory(directory);
	for( std::size_t offset = 0; offset <= path.size(); ) {
		const auto separator = path.find(L';', offset);
		const auto length = separator == std::wstring_view::npos ? path.size() - offset : separator - offset;
		const auto segment = path.substr(offset, length);
		if( IsValidPathDirectory(segment) && EqualName(NormalizePathDirectory(segment), wanted) ) return true;
		if( separator == std::wstring_view::npos ) break;
		offset = separator + 1;
	}
	return false;
}

TerminalEnvironmentBuildResult Failure( TerminalEnvironmentBuildStatus status )
{
	return { status, {} };
}

} // namespace

TerminalEnvironmentBuildResult BuildTerminalEnvironmentBlock(
	const TerminalLaunchOptions& options,
	std::span<const std::wstring> inheritedEntries )
{
	if( options.environmentOverrides.size() > kMaximumOverrides ) {
		return Failure(TerminalEnvironmentBuildStatus::InvalidOverride);
	}
	if( options.prependPathDirectories.size() > kMaximumPathDirectories ) {
		return Failure(TerminalEnvironmentBuildStatus::InvalidPathDirectory);
	}

	std::vector<std::wstring> entries;
	entries.reserve(inheritedEntries.size() + options.environmentOverrides.size() + 3);
	for( const auto& entry : inheritedEntries ) {
		if( entry.empty() || ContainsNul(entry) || entry.find(L'=') == std::wstring::npos ) continue;
		const auto name = EntryName(entry);
		if( name.empty() || FindEntry(entries, name) != entries.end() ) continue;
		entries.push_back(entry);
	}

	// An embedded terminal has real ConPTY capabilities even when Sakura itself
	// was launched by a color-disabled automation host.
	RemoveEntry(entries, L"NO_COLOR");
	const auto term = FindEntry(entries, L"TERM");
	if( term == entries.end() || term->size() <= 5 || _wcsicmp(term->c_str() + 5, L"dumb") == 0 ) {
		SetEntry(entries, L"TERM", L"xterm-256color");
	}
	const auto colorTerm = FindEntry(entries, L"COLORTERM");
	if( colorTerm == entries.end() || colorTerm->size() <= 10 ) SetEntry(entries, L"COLORTERM", L"truecolor");
	if( FindEntry(entries, L"TERM_PROGRAM") == entries.end() ) SetEntry(entries, L"TERM_PROGRAM", L"sakura-editor");

	for( std::size_t index = 0; index < options.environmentOverrides.size(); ++index ) {
		const auto& mutation = options.environmentOverrides[index];
		if( !IsValidVariableName(mutation.name)
			|| (mutation.value && !IsValidVariableValue(*mutation.value))
			|| ContainsOverrideName(options.environmentOverrides, index, mutation.name) ) {
			return Failure(TerminalEnvironmentBuildStatus::InvalidOverride);
		}
		if( mutation.value ) SetEntry(entries, mutation.name, *mutation.value);
		else RemoveEntry(entries, mutation.name);
	}

	if( !options.prependPathDirectories.empty() ) {
		std::wstring prefix;
		const auto path = FindEntry(entries, L"PATH");
		const auto inheritedPath = path != entries.end() && path->size() > 5
			? std::wstring_view(*path).substr(5) : std::wstring_view{};
		for( const auto& directory : options.prependPathDirectories ) {
			if( !IsValidPathDirectory(directory) ) {
				return Failure(TerminalEnvironmentBuildStatus::InvalidPathDirectory);
			}
			if( PathContainsDirectory(inheritedPath, directory)
				|| (!prefix.empty() && PathContainsDirectory(prefix, directory)) ) continue;
			if( !prefix.empty() ) prefix.push_back(L';');
			prefix.append(NormalizePathDirectory(directory));
		}
		if( !prefix.empty() && !inheritedPath.empty() ) {
			prefix.push_back(L';');
			prefix.append(inheritedPath);
		}
		if( !prefix.empty() ) SetEntry(entries, L"PATH", prefix);
	}

	std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
		return _wcsicmp(left.c_str(), right.c_str()) < 0;
	});
	std::size_t characterCount = 1;
	for( const auto& entry : entries ) {
		if( entry.size() + 1 > kMaximumEnvironmentCharacters - characterCount ) {
			return Failure(TerminalEnvironmentBuildStatus::TooLarge);
		}
		characterCount += entry.size() + 1;
	}

	TerminalEnvironmentBuildResult result;
	result.status = TerminalEnvironmentBuildStatus::Succeeded;
	result.block.reserve(characterCount);
	for( const auto& entry : entries ) {
		result.block.insert(result.block.end(), entry.begin(), entry.end());
		result.block.push_back(L'\0');
	}
	result.block.push_back(L'\0');
	return result;
}

TerminalEnvironmentBuildResult BuildTerminalEnvironmentBlock(
	const TerminalLaunchOptions& options ) noexcept
{
	LPWCH raw = ::GetEnvironmentStringsW();
	if( raw == nullptr ) return Failure(TerminalEnvironmentBuildStatus::ReadFailed);
	try {
		std::vector<std::wstring> entries;
		for( const wchar_t* entry = raw; *entry != L'\0'; entry += std::wcslen(entry) + 1 ) {
			entries.emplace_back(entry);
		}
		::FreeEnvironmentStringsW(raw);
		return BuildTerminalEnvironmentBlock(options, entries);
	} catch( ... ) {
		::FreeEnvironmentStringsW(raw);
		return Failure(TerminalEnvironmentBuildStatus::TooLarge);
	}
}

} // namespace terminal
