/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace platform {

//! Returns true only for a Windows path rooted at a drive or UNC/device share.
bool IsAbsoluteWindowsPath( std::wstring_view path );

//! Resolves an executable through the process PATH without implicit CWD lookup.
//! The returned path is absolute and canonical when the file can be opened.
std::optional<std::wstring> ResolveWindowsExecutable( std::wstring_view executableName );

//! Testable PATH variant of ResolveWindowsExecutable.  Empty and relative PATH
//! components are ignored rather than being interpreted relative to the CWD.
std::optional<std::wstring> ResolveWindowsExecutableFromPath(
	std::wstring_view executableName,
	std::wstring_view pathValue
);

//! Injectable path-candidate variant used by deterministic platform tests and
//! callers that already own canonicalization/file policy.
using WindowsExecutableCandidateResolver = std::function<std::optional<std::wstring>(std::wstring_view)>;
std::optional<std::wstring> ResolveWindowsExecutableFromPath(
	std::wstring_view executableName,
	std::wstring_view pathValue,
	const WindowsExecutableCandidateResolver& candidateResolver
);

} // namespace platform
