/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/security/CurrentUserSecurityAttributes.h"

#include <Aclapi.h>

#include <vector>

namespace platform::security {
namespace {

std::wstring FormatError(std::wstring_view operation, DWORD error)
{
	return std::wstring(operation) + L" failed (error " + std::to_wstring(error) + L")";
}

class UniqueHandle final {
public:
	explicit UniqueHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~UniqueHandle()
	{
		if (m_value && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value);
	}
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }

private:
	HANDLE m_value = nullptr;
};

bool GetCurrentUserSid(std::vector<std::uint8_t>& storage, PSID& sid, std::wstring& diagnostic)
{
	HANDLE rawToken = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
		diagnostic = FormatError(L"OpenProcessToken", ::GetLastError());
		return false;
	}
	UniqueHandle token(rawToken);
	DWORD bytes = 0;
	::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
	if (bytes == 0) {
		diagnostic = FormatError(L"GetTokenInformation size", ::GetLastError());
		return false;
	}
	storage.resize(bytes);
	if (!::GetTokenInformation(token.Get(), TokenUser, storage.data(), bytes, &bytes)) {
		diagnostic = FormatError(L"GetTokenInformation", ::GetLastError());
		return false;
	}
	sid = reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid;
	if (!sid || !::IsValidSid(sid)) {
		diagnostic = L"Token user SID is invalid";
		return false;
	}
	return true;
}

} // namespace

CurrentUserSecurityAttributes::~CurrentUserSecurityAttributes()
{
	if (m_acl) ::LocalFree(m_acl);
}

bool CurrentUserSecurityAttributes::Initialize(std::wstring& diagnostic)
{
	if (m_initialized) {
		diagnostic.clear();
		return true;
	}
	std::vector<std::uint8_t> sidStorage;
	PSID currentUser = nullptr;
	if (!GetCurrentUserSid(sidStorage, currentUser, diagnostic)) return false;
	EXPLICIT_ACCESSW access{};
	access.grfAccessPermissions = GENERIC_ALL;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = static_cast<wchar_t*>(currentUser);
	const DWORD aclResult = ::SetEntriesInAclW(1, &access, nullptr, &m_acl);
	if (aclResult != ERROR_SUCCESS) {
		diagnostic = FormatError(L"SetEntriesInAcl", aclResult);
		return false;
	}
	if (!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !::SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE)
		|| !::SetSecurityDescriptorControl(&m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
		diagnostic = FormatError(L"Initialize current-user security descriptor", ::GetLastError());
		::LocalFree(m_acl);
		m_acl = nullptr;
		return false;
	}
	m_attributes = { sizeof(m_attributes), &m_descriptor, FALSE };
	m_initialized = true;
	diagnostic.clear();
	return true;
}

SECURITY_ATTRIBUTES* CurrentUserSecurityAttributes::Attributes() noexcept
{
	return m_initialized ? &m_attributes : nullptr;
}

} // namespace platform::security
