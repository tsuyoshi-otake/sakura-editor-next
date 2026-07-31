/*! @file
	@brief Reusable protected current-user Win32 security attributes.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <string>

namespace platform::security {

//! Owns an ACL and descriptor which grant access only to the interactive current
//! user. SECURITY_ATTRIBUTES::bInheritHandle is always FALSE and the DACL is
//! protected from inherited ACEs. Keep the object alive while creating an object.
class CurrentUserSecurityAttributes final {
public:
	CurrentUserSecurityAttributes() = default;
	~CurrentUserSecurityAttributes();
	CurrentUserSecurityAttributes(const CurrentUserSecurityAttributes&) = delete;
	CurrentUserSecurityAttributes& operator=(const CurrentUserSecurityAttributes&) = delete;

	bool Initialize(std::wstring& diagnostic);
	[[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept;
	[[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

private:
	PACL m_acl = nullptr;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
	bool m_initialized = false;
};

} // namespace platform::security
