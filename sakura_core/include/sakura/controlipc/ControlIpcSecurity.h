/*! @file
	@brief Current-user identity and object-security contract for Control IPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace platform::controlipc {

//! Complete SHA-256 identity of a canonical, case-folded profile path.
//! The original path must never appear in an IPC object name or endpoint payload.
std::wstring ComputeCanonicalProfileHash(const std::filesystem::path& profileDirectory);

//! Build and validate the only accepted Control IPC pipe and endpoint object names.
std::wstring BuildControlPipeName(std::wstring_view profileHash);
std::wstring BuildControlEndpointMappingName(std::wstring_view profileHash);
bool IsSafeControlPipeName(std::wstring_view pipeName) noexcept;
bool IsSafeControlEndpointMappingName(std::wstring_view mappingName) noexcept;

//! Verify that a kernel object has a protected, non-inheriting DACL containing
//! only allow ACEs for the current user.
bool VerifyCurrentUserOnlyDacl(HANDLE object, std::wstring& diagnostic);

//! Verify a named-pipe peer against the current user after the first bounded read.
//! Impersonation is reverted on every terminal branch before this function returns.
bool VerifyNamedPipeClientCurrentUser(HANDLE pipe, std::wstring& diagnostic);

} // namespace platform::controlipc
