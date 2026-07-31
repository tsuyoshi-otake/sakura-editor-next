/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlIpcSecurity.h"

#include <Aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr std::wstring_view kPipePrefix = L"\\\\.\\pipe\\SakuraControl-";
constexpr std::wstring_view kMappingPrefix = L"Local\\SakuraControlEndpoint-";
constexpr std::size_t kProfileHashCharacters = 64;

class UniqueHandle final {
public:
	explicit UniqueHandle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~UniqueHandle()
	{
		if (m_value && m_value != INVALID_HANDLE_VALUE) {
			::CloseHandle(m_value);
		}
	}
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }

private:
	HANDLE m_value = nullptr;
};

class AlgorithmHandle final {
public:
	~AlgorithmHandle()
	{
		if (m_value) {
			::BCryptCloseAlgorithmProvider(m_value, 0);
		}
	}
	[[nodiscard]] BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	[[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }

private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

bool NtSuccess(NTSTATUS status) noexcept
{
	return status >= 0;
}

bool IsAsciiHex(std::wstring_view value) noexcept
{
	return value.size() == kProfileHashCharacters && std::all_of(value.begin(), value.end(), [](wchar_t character) {
		return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
	});
}

bool IsSafeSuffix(std::wstring_view value) noexcept
{
	return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t character) {
		return (character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') || character == L'-' || character == L'_';
	});
}

std::wstring FormatError(std::wstring_view operation, DWORD error)
{
	return std::wstring(operation) + L" failed (error " + std::to_wstring(error) + L")";
}

bool GetTokenUserSid(HANDLE token, std::vector<std::uint8_t>& storage, PSID& sid, std::wstring& diagnostic)
{
	DWORD bytes = 0;
	::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
	if (bytes == 0) {
		diagnostic = FormatError(L"GetTokenInformation size", ::GetLastError());
		return false;
	}
	storage.resize(bytes);
	if (!::GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) {
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

bool GetCurrentUserSid(std::vector<std::uint8_t>& storage, PSID& sid, std::wstring& diagnostic)
{
	HANDLE rawToken = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
		diagnostic = FormatError(L"OpenProcessToken", ::GetLastError());
		return false;
	}
	UniqueHandle token(rawToken);
	return GetTokenUserSid(token.Get(), storage, sid, diagnostic);
}

bool IsExpectedAllowAce(const ACE_HEADER& header, PSID currentUser) noexcept
{
	if (header.AceType != ACCESS_ALLOWED_ACE_TYPE || header.AceFlags != 0 ||
		header.AceSize < sizeof(ACCESS_ALLOWED_ACE)) {
		return false;
	}
	const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(&header);
	const PSID aceSid = const_cast<DWORD*>(&ace->SidStart);
	return ace->Mask != 0 && ::IsValidSid(aceSid) && ::EqualSid(aceSid, currentUser);
}

} // namespace

std::wstring ComputeCanonicalProfileHash(const std::filesystem::path& profileDirectory)
{
	if (profileDirectory.empty()) {
		return {};
	}
	std::error_code error;
	auto canonical = std::filesystem::absolute(profileDirectory, error);
	if (error) {
		canonical = profileDirectory;
		error.clear();
	}
	const auto resolved = std::filesystem::weakly_canonical(canonical, error);
	if (!error) {
		canonical = resolved;
	}
	std::wstring identity = canonical.lexically_normal().wstring();
	if (identity.empty() || identity.find(L'\0') != std::wstring::npos ||
		identity.size() > (std::numeric_limits<ULONG>::max)() / sizeof(wchar_t) ||
		identity.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return {};
	}
	const int lowerLength = ::LCMapStringEx(
		LOCALE_NAME_INVARIANT,
		LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
		identity.data(),
		static_cast<int>(identity.size()),
		nullptr,
		0,
		nullptr,
		nullptr,
		0);
	if (lowerLength <= 0) {
		return {};
	}
	std::wstring lowercaseIdentity(static_cast<std::size_t>(lowerLength), L'\0');
	if (::LCMapStringEx(
		LOCALE_NAME_INVARIANT,
		LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
		identity.data(),
		static_cast<int>(identity.size()),
		lowercaseIdentity.data(),
		lowerLength,
		nullptr,
		nullptr,
		0) != lowerLength) {
		return {};
	}
	identity = std::move(lowercaseIdentity);

	AlgorithmHandle algorithm;
	if (!NtSuccess(::BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return {};
	}
	std::array<std::uint8_t, 32> digest{};
	if (!NtSuccess(::BCryptHash(
		algorithm.Get(), nullptr, 0,
		reinterpret_cast<PUCHAR>(identity.data()), static_cast<ULONG>(identity.size() * sizeof(wchar_t)),
		digest.data(), static_cast<ULONG>(digest.size())))) {
		return {};
	}
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring hash;
	hash.reserve(digest.size() * 2);
	for (const auto byte : digest) {
		hash.push_back(digits[byte >> 4]);
		hash.push_back(digits[byte & 0x0f]);
	}
	return hash;
}

std::wstring BuildControlPipeName(std::wstring_view profileHash)
{
	return IsAsciiHex(profileHash) ? std::wstring(kPipePrefix) + std::wstring(profileHash) : std::wstring{};
}

std::wstring BuildControlEndpointMappingName(std::wstring_view profileHash)
{
	return IsAsciiHex(profileHash) ? std::wstring(kMappingPrefix) + std::wstring(profileHash) : std::wstring{};
}

bool IsSafeControlPipeName(std::wstring_view pipeName) noexcept
{
	return pipeName.starts_with(kPipePrefix) &&
		IsAsciiHex(pipeName.substr(kPipePrefix.size())) &&
		IsSafeSuffix(pipeName.substr(kPipePrefix.size()));
}

bool IsSafeControlEndpointMappingName(std::wstring_view mappingName) noexcept
{
	return mappingName.starts_with(kMappingPrefix) &&
		IsAsciiHex(mappingName.substr(kMappingPrefix.size())) &&
		IsSafeSuffix(mappingName.substr(kMappingPrefix.size()));
}

bool VerifyCurrentUserOnlyDacl(HANDLE object, std::wstring& diagnostic)
{
	if (!object || object == INVALID_HANDLE_VALUE) {
		diagnostic = L"Endpoint object handle is invalid";
		return false;
	}
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	const DWORD result = ::GetSecurityInfo(
		object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
		nullptr, nullptr, &dacl, nullptr, &descriptor);
	struct DescriptorGuard final {
		PSECURITY_DESCRIPTOR value = nullptr;
		~DescriptorGuard() { if (value) ::LocalFree(value); }
	} descriptorGuard{ descriptor };
	if (result != ERROR_SUCCESS || !dacl || !::IsValidAcl(dacl)) {
		diagnostic = FormatError(L"Read endpoint DACL", result);
		return false;
	}
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	if (!::GetSecurityDescriptorControl(descriptor, &control, &revision) || (control & SE_DACL_PROTECTED) == 0) {
		diagnostic = L"Endpoint DACL is not protected from inherited access";
		return false;
	}
	std::vector<std::uint8_t> sidStorage;
	PSID currentUser = nullptr;
	if (!GetCurrentUserSid(sidStorage, currentUser, diagnostic)) {
		return false;
	}
	ACL_SIZE_INFORMATION info{};
	if (!::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
		diagnostic = FormatError(L"Inspect endpoint DACL", ::GetLastError());
		return false;
	}
	if (info.AceCount == 0) {
		diagnostic = L"Endpoint DACL does not grant access to the current user";
		return false;
	}
	for (DWORD index = 0; index < info.AceCount; ++index) {
		ACE_HEADER* header = nullptr;
		if (!::GetAce(dacl, index, reinterpret_cast<void**>(&header)) || !header ||
			!IsExpectedAllowAce(*header, currentUser)) {
			diagnostic = L"Endpoint DACL grants access outside the current user";
			return false;
		}
	}
	diagnostic.clear();
	return true;
}

bool VerifyNamedPipeClientCurrentUser(HANDLE pipe, std::wstring& diagnostic)
{
	if (!pipe || pipe == INVALID_HANDLE_VALUE) {
		diagnostic = L"Named-pipe handle is invalid";
		return false;
	}
	if (!::ImpersonateNamedPipeClient(pipe)) {
		diagnostic = FormatError(L"ImpersonateNamedPipeClient", ::GetLastError());
		return false;
	}

	bool accepted = false;
	HANDLE rawToken = nullptr;
	if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken)) {
		UniqueHandle clientToken(rawToken);
		std::vector<std::uint8_t> clientStorage;
		std::vector<std::uint8_t> currentStorage;
		PSID clientUser = nullptr;
		PSID currentUser = nullptr;
		std::wstring tokenDiagnostic;
		if (GetTokenUserSid(clientToken.Get(), clientStorage, clientUser, tokenDiagnostic) &&
			GetCurrentUserSid(currentStorage, currentUser, tokenDiagnostic) &&
			::EqualSid(clientUser, currentUser)) {
			accepted = true;
		} else {
			diagnostic = tokenDiagnostic.empty() ? L"Named-pipe client is not the current user" : tokenDiagnostic;
		}
	} else {
		diagnostic = FormatError(L"OpenThreadToken after pipe impersonation", ::GetLastError());
	}

	const DWORD revertError = ::RevertToSelf() ? ERROR_SUCCESS : ::GetLastError();
	if (revertError != ERROR_SUCCESS) {
		diagnostic = FormatError(L"RevertToSelf after pipe impersonation", revertError);
		return false;
	}
	if (!accepted && diagnostic.empty()) {
		diagnostic = L"Named-pipe client is not the current user";
	}
	return accepted;
}

} // namespace platform::controlipc
