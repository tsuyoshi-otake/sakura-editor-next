/*! @file */
#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <Aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace platform::harnessbridge {
namespace {

std::wstring ErrorText(const wchar_t* operation, const DWORD error)
{
	return std::wstring(operation) + L" failed (error " + std::to_wstring(error) + L")";
}

bool IsHexHash(const std::wstring_view value) noexcept
{
	if (value.size() != 64) return false;
	return std::all_of(value.begin(), value.end(), [](const wchar_t c) {
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
	});
}

bool GetCurrentUserSid(std::vector<std::uint8_t>& storage, PSID& sid, std::wstring& diagnostic)
{
	HANDLE token = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
		diagnostic = ErrorText(L"OpenProcessToken", ::GetLastError());
		return false;
	}
	DWORD size = 0;
	::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	if (size == 0) {
		::CloseHandle(token);
		diagnostic = ErrorText(L"GetTokenInformation size", ::GetLastError());
		return false;
	}
	storage.resize(size);
	if (!::GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
		::CloseHandle(token);
		diagnostic = ErrorText(L"GetTokenInformation", ::GetLastError());
		return false;
	}
	::CloseHandle(token);
	sid = reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid;
	if (!sid || !::IsValidSid(sid)) {
		diagnostic = L"Current user SID is invalid";
		return false;
	}
	return true;
}

} // namespace

std::wstring ComputeHarnessEndpointHash(const std::wstring_view profileIdentity,
	const std::span<const std::uint8_t> editorIdentity, const std::uint64_t bridgeEpoch)
{
	if (profileIdentity.empty() || editorIdentity.empty()) return {};
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
	DWORD objectLength = 0;
	DWORD returned = 0;
	if (!BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0))) {
		::BCryptCloseAlgorithmProvider(algorithm, 0);
		return {};
	}
	std::vector<std::uint8_t> object(objectLength);
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0))) {
		::BCryptCloseAlgorithmProvider(algorithm, 0);
		return {};
	}
	const auto update = [&](const void* data, const ULONG size) {
		return BCRYPT_SUCCESS(::BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)), size, 0));
	};
	const std::uint8_t separator = 0;
	bool ok = update(profileIdentity.data(), static_cast<ULONG>(profileIdentity.size() * sizeof(wchar_t)))
		&& update(&separator, sizeof(separator))
		&& update(editorIdentity.data(), static_cast<ULONG>(editorIdentity.size()))
		&& update(&bridgeEpoch, sizeof(bridgeEpoch));
	std::array<std::uint8_t, 32> digest{};
	if (ok) ok = BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
	::BCryptDestroyHash(hash);
	::BCryptCloseAlgorithmProvider(algorithm, 0);
	if (!ok) return {};
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring result;
	result.reserve(64);
	for (const auto byte : digest) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	return result;
}

std::wstring BuildHarnessPipeName(const std::wstring_view endpointHash)
{
	return IsHexHash(endpointHash) ? std::wstring(kHarnessBridgePipePrefix) + std::wstring(endpointHash) : std::wstring{};
}

std::wstring BuildHarnessEndpointMappingName(const std::wstring_view endpointHash)
{
	return IsHexHash(endpointHash) ? std::wstring(kHarnessBridgeMappingPrefix) + std::wstring(endpointHash) : std::wstring{};
}

bool IsSafeHarnessPipeName(const std::wstring_view pipeName) noexcept
{
	return pipeName.size() == kHarnessBridgePipePrefix.size() + 64
		&& pipeName.substr(0, kHarnessBridgePipePrefix.size()) == kHarnessBridgePipePrefix
		&& IsHexHash(pipeName.substr(kHarnessBridgePipePrefix.size()));
}

bool IsSafeHarnessEndpointMappingName(const std::wstring_view mappingName) noexcept
{
	return mappingName.size() == kHarnessBridgeMappingPrefix.size() + 64
		&& mappingName.substr(0, kHarnessBridgeMappingPrefix.size()) == kHarnessBridgeMappingPrefix
		&& IsHexHash(mappingName.substr(kHarnessBridgeMappingPrefix.size()));
}

HarnessBridgeSecurityAttributes::~HarnessBridgeSecurityAttributes()
{
	if (m_acl) ::LocalFree(m_acl);
}

bool HarnessBridgeSecurityAttributes::Initialize(std::wstring& diagnostic)
{
	if (m_initialized) { diagnostic.clear(); return true; }
	std::vector<std::uint8_t> sidStorage;
	PSID currentUser = nullptr;
	if (!GetCurrentUserSid(sidStorage, currentUser, diagnostic)) return false;
	EXPLICIT_ACCESSW access{};
	access.grfAccessPermissions = GENERIC_ALL;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = static_cast<LPWSTR>(currentUser);
	const auto aclResult = ::SetEntriesInAclW(1, &access, nullptr, &m_acl);
	if (aclResult != ERROR_SUCCESS) { diagnostic = ErrorText(L"SetEntriesInAcl", aclResult); return false; }
	if (!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !::SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE)
		|| !::SetSecurityDescriptorControl(&m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
		diagnostic = ErrorText(L"Initialize security descriptor", ::GetLastError());
		::LocalFree(m_acl);
		m_acl = nullptr;
		return false;
	}
	m_attributes = { sizeof(m_attributes), &m_descriptor, FALSE };
	m_initialized = true;
	diagnostic.clear();
	return true;
}

SECURITY_ATTRIBUTES* HarnessBridgeSecurityAttributes::Attributes() noexcept
{
	return m_initialized ? &m_attributes : nullptr;
}

bool VerifyHarnessCurrentUserOnlyDacl(const HANDLE object, std::wstring& diagnostic)
{
	if (!object || object == INVALID_HANDLE_VALUE) { diagnostic = L"Invalid security object"; return false; }
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	PACL acl = nullptr;
	const auto status = ::GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
		nullptr, nullptr, &acl, nullptr, &descriptor);
	if (status != ERROR_SUCCESS) { diagnostic = ErrorText(L"GetSecurityInfo", status); return false; }
	bool protectedDacl = false;
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	const bool controlOk = ::GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE;
	protectedDacl = controlOk && (control & SE_DACL_PROTECTED) != 0;
	std::vector<std::uint8_t> sidStorage;
	PSID currentUser = nullptr;
	bool valid = protectedDacl && acl && GetCurrentUserSid(sidStorage, currentUser, diagnostic);
	if (valid) {
		for (DWORD index = 0; index < acl->AceCount; ++index) {
			void* rawAce = nullptr;
			if (!::GetAce(acl, index, &rawAce)) { valid = false; break; }
			const auto* header = static_cast<const ACE_HEADER*>(rawAce);
			if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags != 0) { valid = false; break; }
			const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
			if (!::EqualSid(currentUser, reinterpret_cast<PSID>(const_cast<ULONG*>(&ace->SidStart)))) { valid = false; break; }
		}
	}
	if (descriptor) ::LocalFree(descriptor);
	if (!valid) diagnostic = L"DACL is not protected current-user-only";
	else diagnostic.clear();
	return valid;
}

bool VerifyHarnessNamedPipeClientCurrentUser(const HANDLE pipe, std::wstring& diagnostic)
{
	if (!pipe || pipe == INVALID_HANDLE_VALUE) { diagnostic = L"Invalid pipe"; return false; }
	if (!::ImpersonateNamedPipeClient(pipe)) { diagnostic = ErrorText(L"ImpersonateNamedPipeClient", ::GetLastError()); return false; }
	bool valid = false;
	HANDLE token = nullptr;
	std::vector<std::uint8_t> expectedStorage;
	PSID expected = nullptr;
	std::wstring ignored;
	if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
		DWORD size = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<std::uint8_t> actualStorage(size);
		if (size && ::GetTokenInformation(token, TokenUser, actualStorage.data(), size, &size)
			&& GetCurrentUserSid(expectedStorage, expected, ignored)) {
			const auto actual = reinterpret_cast<const TOKEN_USER*>(actualStorage.data())->User.Sid;
			valid = actual && expected && ::EqualSid(actual, expected) != FALSE;
		}
	}
	if (token) ::CloseHandle(token);
	const auto revert = ::RevertToSelf();
	if (!revert) { diagnostic = ErrorText(L"RevertToSelf", ::GetLastError()); return false; }
	if (!valid) diagnostic = L"Pipe peer is not the current user";
	else diagnostic.clear();
	return valid;
}

} // namespace platform::harnessbridge
