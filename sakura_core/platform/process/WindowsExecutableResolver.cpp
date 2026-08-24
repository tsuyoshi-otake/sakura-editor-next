/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/process/WindowsExecutableResolver.h"

#include <cwctype>
#include <iterator>
#include <utility>
#include <vector>
#include <windows.h>

namespace platform {
namespace {

bool IsExecutableName( std::wstring_view name )
{
	if( name.empty() ) return false;
	return name.find_first_of(L"\\\\/:") == std::wstring_view::npos
		&& name != L"."
		&& name != L"..";
}

std::wstring GetEnvironment( const wchar_t* name )
{
	const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
	if( required == 0 ) return {};
	std::vector<wchar_t> value(static_cast<std::size_t>(required) + 1, L'\0');
	const DWORD length = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
	if( length == 0 ) return {};
	return std::wstring(value.data(), length);
}

std::wstring StripExtendedPathPrefix( std::wstring value )
{
	if( value.rfind(L"\\\\?\\UNC\\", 0) == 0 ) value.replace(0, 8, L"\\\\");
	else if( value.rfind(L"\\\\?\\", 0) == 0 ) value.erase(0, 4);
	return value;
}

std::optional<std::wstring> GetCanonicalExistingPath( const std::wstring& candidate )
{
	const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
	if( attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ) return std::nullopt;

	HANDLE file = ::CreateFileW(candidate.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if( file == INVALID_HANDLE_VALUE ) {
		wchar_t absolute[32768]{};
		const DWORD length = ::GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(std::size(absolute)), absolute, nullptr);
		if( length == 0 || length >= std::size(absolute) ) return std::nullopt;
		std::wstring result(absolute, length);
		return IsAbsoluteWindowsPath(result) ? std::optional<std::wstring>(std::move(result)) : std::nullopt;
	}

	std::vector<wchar_t> buffer(32768, L'\0');
	DWORD length = ::GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
	::CloseHandle(file);
	if( length == 0 ) return std::nullopt;
	if( length >= buffer.size() ) {
		buffer.assign(static_cast<std::size_t>(length) + 1, L'\0');
		HANDLE retryFile = ::CreateFileW(candidate.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if( retryFile == INVALID_HANDLE_VALUE ) return std::nullopt;
		length = ::GetFinalPathNameByHandleW(retryFile, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
		::CloseHandle(retryFile);
		if( length == 0 || length >= buffer.size() ) return std::nullopt;
	}

	std::wstring result(buffer.data(), length);
	result = StripExtendedPathPrefix(std::move(result));
	return IsAbsoluteWindowsPath(result) ? std::optional<std::wstring>(std::move(result)) : std::nullopt;
}

std::optional<std::wstring> BuildExecutableCandidate( std::wstring_view executableName, std::wstring_view directory )
{
	if( directory.empty() || !IsAbsoluteWindowsPath(directory) ) return std::nullopt;
	std::wstring candidate(directory);
	if( candidate.back() != L'\\' && candidate.back() != L'/' ) candidate.push_back(L'\\');
	candidate.append(executableName);
	return candidate;
}

} // namespace

bool IsAbsoluteWindowsPath( std::wstring_view path )
{
	if( path.size() >= 3
		&& std::iswalpha(path[0]) != 0
		&& path[1] == L':'
		&& (path[2] == L'\\' || path[2] == L'/') ) return true;
	return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::optional<std::wstring> ResolveWindowsExecutable( std::wstring_view executableName )
{
	if( !IsExecutableName(executableName) ) return std::nullopt;
	return ResolveWindowsExecutableFromPath(executableName, GetEnvironment(L"PATH"));
}

std::optional<std::wstring> ResolveWindowsExecutableFromPath(
	std::wstring_view executableName,
	std::wstring_view pathValue
)
{
	return ResolveWindowsExecutableFromPath(executableName, pathValue,
		[]( std::wstring_view candidate ) { return GetCanonicalExistingPath(std::wstring(candidate)); });
}

std::optional<std::wstring> ResolveWindowsExecutableFromPath(
	std::wstring_view executableName,
	std::wstring_view pathValue,
	const WindowsExecutableCandidateResolver& candidateResolver
)
{
	if( !IsExecutableName(executableName) || !candidateResolver ) return std::nullopt;
	std::size_t begin = 0;
	while( begin <= pathValue.size() ) {
		const std::size_t separator = pathValue.find(L';', begin);
		const std::size_t end = separator == std::wstring_view::npos ? pathValue.size() : separator;
		auto directory = pathValue.substr(begin, end - begin);
		// Quoted absolute entries are common in manually configured Windows PATH
		// values. Strip only one balanced outer pair; empty, relative, and
		// malformed entries still fail the absolute-path policy below.
		if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"') {
			directory.remove_prefix(1);
			directory.remove_suffix(1);
		}
		if( const auto candidate = BuildExecutableCandidate(executableName, directory) ) {
			if( const auto resolved = candidateResolver(*candidate) ) return resolved;
		}
		if( separator == std::wstring_view::npos ) break;
		begin = separator + 1;
	}
	return std::nullopt;
}

} // namespace platform
