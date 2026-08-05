/*! @file
	@brief Control-process IPC の current-user 専用セキュリティ境界
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CONTROLIPC_SECURITY_4C6A61C4_6A49_4F4B_9A38_0B6A33D0ECCD_H_
#define SAKURA_CONTROLIPC_SECURITY_4C6A61C4_6A49_4F4B_9A38_0B6A33D0ECCD_H_
#pragma once

#include <Windows.h>

#include <sakura/security/CurrentUserSecurityAttributes.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace platform::controlipc {

//! The complete SHA-256 hexadecimal identity of a canonical, case-folded profile path.
//! It is deliberately a one-way identifier: callers must never place the original path
//! in an IPC name or shared endpoint payload.
std::wstring ComputeCanonicalProfileHash(const std::filesystem::path& profileDirectory);

//! Builds the only accepted control-process named-pipe and shared-mapping names.
std::wstring BuildControlPipeName(std::wstring_view profileHash);
std::wstring BuildControlEndpointMappingName(std::wstring_view profileHash);
bool IsSafeControlPipeName(std::wstring_view pipeName) noexcept;
bool IsSafeControlEndpointMappingName(std::wstring_view mappingName) noexcept;

//! Compatibility alias. New platform adapters should include the stable security
//! header directly instead of depending on the control IPC subsystem.
using CurrentUserSecurityAttributes = ::platform::security::CurrentUserSecurityAttributes;

//! Verifies that a kernel object's DACL has one or more non-inheriting allow ACEs for
//! the current user and no other ACEs/principals. This protects consumers from opening a
//! spoofed endpoint created with a permissive ACL.
bool VerifyCurrentUserOnlyDacl(HANDLE object, std::wstring& diagnostic);

//! Server-side named-pipe peer validation. Windows requires the server to have read at
//! least one message from the connected client before impersonation is available. Call
//! this after a bounded protocol read and before dispatching any service operation. The
//! function always reverts impersonation before returning, including every failure path.
bool VerifyNamedPipeClientCurrentUser(HANDLE pipe, std::wstring& diagnostic);

} // namespace platform::controlipc

#endif /* SAKURA_CONTROLIPC_SECURITY_4C6A61C4_6A49_4F4B_9A38_0B6A33D0ECCD_H_ */
