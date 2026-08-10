/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/controlipc/ControlIpcSecurity.h>

#include <Aclapi.h>

#if __has_include("platform/controlipc/ControlIpcSecurity.h")
#error "sakura_controlipc_security_tests can reach the removed private contract"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace platform::controlipc;

class Handle final {
public:
	explicit Handle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }

private:
	HANDLE m_value;
};

class CurrentUserObjectSecurity final {
public:
	~CurrentUserObjectSecurity() { if (m_acl) ::LocalFree(m_acl); }
	CurrentUserObjectSecurity(const CurrentUserObjectSecurity&) = delete;
	CurrentUserObjectSecurity& operator=(const CurrentUserObjectSecurity&) = delete;
	CurrentUserObjectSecurity() = default;

	bool Initialize()
	{
		HANDLE rawToken = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
		Handle token(rawToken);
		DWORD bytes = 0;
		::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
		if (bytes == 0) return false;
		std::vector<std::uint8_t> storage(bytes);
		if (!::GetTokenInformation(token.Get(), TokenUser, storage.data(), bytes, &bytes)) return false;
		PSID currentUser = reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid;
		if (!currentUser || !::IsValidSid(currentUser)) return false;
		EXPLICIT_ACCESSW access{};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = static_cast<wchar_t*>(currentUser);
		if (::SetEntriesInAclW(1, &access, nullptr, &m_acl) != ERROR_SUCCESS) return false;
		if (!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION)
			|| !::SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE)
			|| !::SetSecurityDescriptorControl(&m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return false;
		m_attributes = { sizeof(m_attributes), &m_descriptor, FALSE };
		return true;
	}

	[[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept { return &m_attributes; }

private:
	PACL m_acl = nullptr;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
};

bool CanonicalProfileIdentityIsStableAndOpaque()
{
	const auto first = ComputeCanonicalProfileHash(L"C:\\Profiles\\Contract\\.");
	const auto trailingSeparator = ComputeCanonicalProfileHash(L"C:\\Profiles\\Contract\\");
	const auto second = ComputeCanonicalProfileHash(L"c:\\profiles\\contract");
	return first.size() == 64 && first == second && first == trailingSeparator && first.find(L"profile") == std::wstring::npos
		&& std::all_of(first.begin(), first.end(), [](wchar_t character) {
			return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
		}) && ComputeCanonicalProfileHash({}).empty();
}

bool EndpointNamesRejectNonCanonicalInputs()
{
	const std::wstring hash(64, L'a');
	return IsSafeControlPipeName(BuildControlPipeName(hash))
		&& IsSafeControlEndpointMappingName(BuildControlEndpointMappingName(hash))
		&& !IsSafeControlPipeName(L"\\\\.\\pipe\\SakuraControl-" + std::wstring(63, L'a'))
		&& !IsSafeControlPipeName(L"\\\\.\\pipe\\SakuraControl-" + std::wstring(63, L'a') + L"!")
		&& !IsSafeControlEndpointMappingName(L"Global\\SakuraControlEndpoint-" + hash)
		&& BuildControlPipeName(L"bad").empty();
}

bool CreatedObjectHasProtectedCurrentUserDacl()
{
	CurrentUserObjectSecurity security;
	std::wstring diagnostic;
	if (!security.Initialize()) return false;
	Handle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, security.Attributes(), PAGE_READWRITE, 0, 4096, nullptr));
	return mapping.Get() != nullptr && VerifyCurrentUserOnlyDacl(mapping.Get(), diagnostic) && diagnostic.empty();
}

bool NamedPipePeerVerificationAlwaysRevertsImpersonation()
{
	CurrentUserObjectSecurity security;
	std::wstring diagnostic;
	if (!security.Initialize()) return false;
	const std::wstring name = L"\\\\.\\pipe\\SakuraControlSecurityContract-"
		+ std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
	Handle server(::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 1024, 1024, 0, security.Attributes()));
	if (server.Get() == INVALID_HANDLE_VALUE) return false;
	Handle release(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!release.Get()) return false;
	std::atomic<bool> clientReady = false;
	std::thread client([&] {
		Handle connection(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
		if (connection.Get() == INVALID_HANDLE_VALUE) return;
		const std::uint8_t marker = 0x53;
		DWORD written = 0;
		clientReady = ::WriteFile(connection.Get(), &marker, sizeof(marker), &written, nullptr) && written == sizeof(marker);
		::WaitForSingleObject(release.Get(), 5000);
	});
	const BOOL connected = ::ConnectNamedPipe(server.Get(), nullptr);
	const DWORD connectError = connected ? ERROR_SUCCESS : ::GetLastError();
	std::uint8_t marker = 0;
	DWORD read = 0;
	const bool readOk = (connected || connectError == ERROR_PIPE_CONNECTED)
		&& ::ReadFile(server.Get(), &marker, sizeof(marker), &read, nullptr) && read == sizeof(marker);
	const bool verified = readOk && VerifyNamedPipeClientCurrentUser(server.Get(), diagnostic);
	HANDLE threadToken = nullptr;
	const bool reverted = !::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &threadToken);
	if (threadToken) ::CloseHandle(threadToken);
	::SetEvent(release.Get());
	client.join();
	return clientReady && marker == 0x53 && verified && reverted;
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept : m_name(name), m_run(run) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	const std::string_view m_name;
	bool (*const m_run)();
};

constexpr std::array kTests{
	TestCase{ "CanonicalProfileIdentityIsStableAndOpaque", CanonicalProfileIdentityIsStableAndOpaque },
	TestCase{ "EndpointNamesRejectNonCanonicalInputs", EndpointNamesRejectNonCanonicalInputs },
	TestCase{ "CreatedObjectHasProtectedCurrentUserDacl", CreatedObjectHasProtectedCurrentUserDacl },
	TestCase{ "NamedPipePeerVerificationAlwaysRevertsImpersonation", NamedPipePeerVerificationAlwaysRevertsImpersonation },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "ControlIpcSecurityContract.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}
	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "ControlIpcSecurityContract." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
