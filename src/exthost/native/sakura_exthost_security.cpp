/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

// This module deliberately uses only the stable N-API registration ABI.  It
// resolves the one error-reporting function dynamically, so the binary has no
// build-time dependency on a particular Node.js SDK or node.lib.
struct napi_env__;
struct napi_value__;
using napi_env = napi_env__*;
using napi_value = napi_value__*;
using napi_status = int;
using NapiThrowError = napi_status(__cdecl*)(napi_env, const char*, const char*);

namespace {

constexpr wchar_t kPipePrefix[] = LR"(\\.\pipe\sakura-exthost-)";
constexpr std::size_t kMaximumPipeNameCharacters = 512;

using CreateNamedPipeWFunction = HANDLE(WINAPI*)(
	LPCWSTR,
	DWORD,
	DWORD,
	DWORD,
	DWORD,
	DWORD,
	DWORD,
	LPSECURITY_ATTRIBUTES);

wchar_t g_pipeName[kMaximumPipeNameCharacters]{};
SECURITY_DESCRIPTOR g_securityDescriptor{};
SECURITY_ATTRIBUTES g_securityAttributes{};
PACL g_acl = nullptr;
CreateNamedPipeWFunction g_originalCreateNamedPipeW = nullptr;
char g_error[512]{};

void SetError(const char* operation, DWORD error)
{
	std::snprintf(g_error, sizeof(g_error), "%s failed with Windows error %lu", operation, error);
}

bool ReadTrustedPipeName()
{
	const DWORD markerLength = ::GetEnvironmentVariableW(L"SAKURA_EXTENSION_HOST", nullptr, 0);
	if (markerLength != 2) {
		std::snprintf(g_error, sizeof(g_error), "SAKURA_EXTENSION_HOST is not set to 1");
		return false;
	}
	wchar_t marker[2]{};
	if (::GetEnvironmentVariableW(L"SAKURA_EXTENSION_HOST", marker, 2) != 1 || marker[0] != L'1') {
		std::snprintf(g_error, sizeof(g_error), "SAKURA_EXTENSION_HOST is not set to 1");
		return false;
	}

	const DWORD length = ::GetEnvironmentVariableW(
		L"SAKURA_PIPE_NAME", g_pipeName, static_cast<DWORD>(std::size(g_pipeName)));
	if (length == 0 || length >= std::size(g_pipeName)) {
		std::snprintf(g_error, sizeof(g_error), "SAKURA_PIPE_NAME is missing or too long");
		return false;
	}
	if (std::wcsncmp(g_pipeName, kPipePrefix, std::size(kPipePrefix) - 1) != 0) {
		std::snprintf(g_error, sizeof(g_error), "SAKURA_PIPE_NAME has an unexpected prefix");
		return false;
	}
	return true;
}

bool BuildCurrentUserSecurityDescriptor()
{
	HANDLE token = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
		SetError("OpenProcessToken", ::GetLastError());
		return false;
	}

	DWORD tokenBytes = 0;
	::GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
	if (tokenBytes == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		const DWORD error = ::GetLastError();
		::CloseHandle(token);
		SetError("GetTokenInformation(size)", error);
		return false;
	}

	auto* tokenUser = static_cast<TOKEN_USER*>(::LocalAlloc(LPTR, tokenBytes));
	if (tokenUser == nullptr) {
		::CloseHandle(token);
		SetError("LocalAlloc(token user)", ERROR_OUTOFMEMORY);
		return false;
	}
	if (!::GetTokenInformation(token, TokenUser, tokenUser, tokenBytes, &tokenBytes)) {
		const DWORD error = ::GetLastError();
		::LocalFree(tokenUser);
		::CloseHandle(token);
		SetError("GetTokenInformation", error);
		return false;
	}
	::CloseHandle(token);

	const DWORD sidBytes = ::GetLengthSid(tokenUser->User.Sid);
	const DWORD aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sidBytes;
	g_acl = static_cast<PACL>(::LocalAlloc(LPTR, aclBytes));
	if (g_acl == nullptr) {
		::LocalFree(tokenUser);
		SetError("LocalAlloc(ACL)", ERROR_OUTOFMEMORY);
		return false;
	}
	if (!::InitializeAcl(g_acl, aclBytes, ACL_REVISION)
		|| !::AddAccessAllowedAceEx(g_acl, ACL_REVISION, 0, GENERIC_ALL, tokenUser->User.Sid)) {
		const DWORD error = ::GetLastError();
		::LocalFree(tokenUser);
		SetError("Build current-user ACL", error);
		return false;
	}
	::LocalFree(tokenUser);

	if (!::InitializeSecurityDescriptor(&g_securityDescriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !::SetSecurityDescriptorDacl(&g_securityDescriptor, TRUE, g_acl, FALSE)) {
		SetError("Build security descriptor", ::GetLastError());
		return false;
	}
	g_securityAttributes.nLength = sizeof(g_securityAttributes);
	g_securityAttributes.lpSecurityDescriptor = &g_securityDescriptor;
	g_securityAttributes.bInheritHandle = FALSE;
	return true;
}

HANDLE WINAPI SecureCreateNamedPipeW(
	LPCWSTR pipeName,
	DWORD openMode,
	DWORD pipeMode,
	DWORD maximumInstances,
	DWORD outputBufferSize,
	DWORD inputBufferSize,
	DWORD defaultTimeout,
	LPSECURITY_ATTRIBUTES securityAttributes)
{
	if (pipeName != nullptr && std::wcscmp(pipeName, g_pipeName) == 0) {
		securityAttributes = &g_securityAttributes;
	}
	return g_originalCreateNamedPipeW(
		pipeName,
		openMode,
		pipeMode,
		maximumInstances,
		outputBufferSize,
		inputBufferSize,
		defaultTimeout,
		securityAttributes);
}

bool PatchNodeImport()
{
	auto* module = reinterpret_cast<std::byte*>(::GetModuleHandleW(nullptr));
	if (module == nullptr) {
		SetError("GetModuleHandleW", ::GetLastError());
		return false;
	}
	const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
		std::snprintf(g_error, sizeof(g_error), "Node executable has an invalid DOS header");
		return false;
	}
	const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dosHeader->e_lfanew);
	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
		std::snprintf(g_error, sizeof(g_error), "Node executable has an invalid NT header");
		return false;
	}
	const auto& importDirectory =
		ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (importDirectory.VirtualAddress == 0 || importDirectory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
		std::snprintf(g_error, sizeof(g_error), "Node executable has no import directory");
		return false;
	}

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + importDirectory.VirtualAddress);
	for (; descriptor->Name != 0; ++descriptor) {
		const DWORD lookupRva = descriptor->OriginalFirstThunk != 0
			? descriptor->OriginalFirstThunk
			: descriptor->FirstThunk;
		auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA*>(module + lookupRva);
		auto* address = reinterpret_cast<IMAGE_THUNK_DATA*>(module + descriptor->FirstThunk);
		for (; lookup->u1.AddressOfData != 0; ++lookup, ++address) {
			if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) {
				continue;
			}
			const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
				module + lookup->u1.AddressOfData);
			if (std::strcmp(reinterpret_cast<const char*>(import->Name), "CreateNamedPipeW") != 0) {
				continue;
			}

			g_originalCreateNamedPipeW = reinterpret_cast<CreateNamedPipeWFunction>(address->u1.Function);
			DWORD previousProtection = 0;
			if (!::VirtualProtect(&address->u1.Function, sizeof(address->u1.Function), PAGE_READWRITE,
					&previousProtection)) {
				SetError("VirtualProtect(IAT)", ::GetLastError());
				return false;
			}
			address->u1.Function = reinterpret_cast<ULONG_PTR>(&SecureCreateNamedPipeW);
			DWORD ignoredProtection = 0;
			::VirtualProtect(&address->u1.Function, sizeof(address->u1.Function), previousProtection,
				&ignoredProtection);
			::FlushInstructionCache(::GetCurrentProcess(), &address->u1.Function, sizeof(address->u1.Function));

			HMODULE pinnedModule = nullptr;
			if (!::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
					reinterpret_cast<LPCWSTR>(&g_securityAttributes), &pinnedModule)) {
				SetError("GetModuleHandleExW(PIN)", ::GetLastError());
				return false;
			}
			return true;
		}
	}

	std::snprintf(g_error, sizeof(g_error), "Node executable does not import CreateNamedPipeW");
	return false;
}

bool InstallSecurityHook()
{
	return ReadTrustedPipeName() && BuildCurrentUserSecurityDescriptor() && PatchNodeImport();
}

} // namespace

extern "C" __declspec(dllexport) napi_value napi_register_module_v1(napi_env env, napi_value exports)
{
	if (InstallSecurityHook()) {
		return exports;
	}
	const HMODULE node = ::GetModuleHandleW(nullptr);
	const auto throwError = node == nullptr
		? nullptr
		: reinterpret_cast<NapiThrowError>(::GetProcAddress(node, "napi_throw_error"));
	if (throwError != nullptr) {
		throwError(env, "ERR_SAKURA_PIPE_SECURITY", g_error[0] == '\0' ? "security hook failed" : g_error);
	}
	return nullptr;
}
