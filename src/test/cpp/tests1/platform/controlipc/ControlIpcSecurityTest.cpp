/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/security/CurrentUserSecurityAttributes.h>

#include <array>
#include <cstdint>
#include <string>
#include <thread>

namespace platform::controlipc {
namespace {

using ::platform::security::CurrentUserSecurityAttributes;

class Handle final {
public:
	explicit Handle(HANDLE value = nullptr) noexcept : m_value(value) {}
	~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }

private:
	HANDLE m_value = nullptr;
};

std::wstring UniquePipeName()
{
	return L"\\\\.\\pipe\\SakuraControlSecurityTest-" + std::to_wstring(::GetCurrentProcessId()) +
		L"-" + std::to_wstring(::GetTickCount64());
}

} // namespace

TEST(ControlIpcSecurity, CanonicalProfileHashIsStableAndDoesNotExposePath)
{
	const auto first = ComputeCanonicalProfileHash(L"C:\\Profiles\\Unit\\.");
	const auto trailingSeparator = ComputeCanonicalProfileHash(L"C:\\Profiles\\Unit\\");
	const auto second = ComputeCanonicalProfileHash(L"c:\\profiles\\unit");

	ASSERT_EQ(64u, first.size());
	EXPECT_EQ(first, second);
	EXPECT_EQ(first, trailingSeparator);
	EXPECT_EQ(std::wstring::npos, first.find(L"profile"));
	EXPECT_TRUE(std::all_of(first.begin(), first.end(), [](wchar_t character) {
		return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
	}));
	EXPECT_TRUE(ComputeCanonicalProfileHash({}).empty());
}

TEST(ControlIpcSecurity, EndpointNamesAcceptOnlyTheFixedPrefixAndHashAlphabet)
{
	const std::wstring hash(64, L'a');
	const auto pipe = BuildControlPipeName(hash);
	const auto mapping = BuildControlEndpointMappingName(hash);

	EXPECT_TRUE(IsSafeControlPipeName(pipe));
	EXPECT_TRUE(IsSafeControlEndpointMappingName(mapping));
	EXPECT_FALSE(IsSafeControlPipeName(L"\\\\.\\pipe\\SakuraControl-" + std::wstring(63, L'a')));
	EXPECT_FALSE(IsSafeControlPipeName(L"\\\\.\\pipe\\SakuraControl-" + std::wstring(63, L'a') + L"!"));
	EXPECT_FALSE(IsSafeControlPipeName(L"\\\\.\\pipe\\SakuraExtensionHost-" + hash));
	EXPECT_FALSE(IsSafeControlEndpointMappingName(L"Global\\SakuraControlEndpoint-" + hash));
	EXPECT_TRUE(BuildControlPipeName(L"bad").empty());
	EXPECT_TRUE(BuildControlEndpointMappingName(L"BAD").empty());
}

TEST(ControlIpcSecurity, CreatedObjectDaclIsCurrentUserOnlyAndNonInheriting)
{
	CurrentUserSecurityAttributes security;
	std::wstring diagnostic;
	ASSERT_TRUE(security.Initialize(diagnostic)) << diagnostic.c_str();
	ASSERT_NE(nullptr, security.Attributes());
	EXPECT_FALSE(security.Attributes()->bInheritHandle);

	Handle mapping(::CreateFileMappingW(INVALID_HANDLE_VALUE, security.Attributes(), PAGE_READWRITE, 0, 4096, nullptr));
	ASSERT_NE(nullptr, mapping.Get());
	EXPECT_TRUE(VerifyCurrentUserOnlyDacl(mapping.Get(), diagnostic)) << diagnostic.c_str();
}

TEST(ControlIpcSecurity, NamedPipeClientValidationAcceptsTheCurrentUserAndRevertsImpersonation)
{
	CurrentUserSecurityAttributes security;
	std::wstring diagnostic;
	ASSERT_TRUE(security.Initialize(diagnostic)) << diagnostic.c_str();
	const auto name = UniquePipeName();
	Handle server(::CreateNamedPipeW(
		name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 1024, 1024, 0, security.Attributes()));
	ASSERT_NE(INVALID_HANDLE_VALUE, server.Get());
	Handle release(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	ASSERT_NE(nullptr, release.Get());

	std::thread client([&] {
		Handle connection(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
		if (connection.Get() != INVALID_HANDLE_VALUE) {
			const std::uint8_t marker = 0x53;
			DWORD written = 0;
			::WriteFile(connection.Get(), &marker, sizeof(marker), &written, nullptr);
			::WaitForSingleObject(release.Get(), 5000);
		}
	});
	const BOOL connected = ::ConnectNamedPipe(server.Get(), nullptr);
	const DWORD connectError = connected ? ERROR_SUCCESS : ::GetLastError();
	if (!connected && connectError != ERROR_PIPE_CONNECTED) {
		::SetEvent(release.Get());
		client.join();
		FAIL() << "ConnectNamedPipe failed (error " << connectError << ")";
	}
	std::uint8_t marker = 0;
	DWORD read = 0;
	EXPECT_TRUE(::ReadFile(server.Get(), &marker, sizeof(marker), &read, nullptr));
	EXPECT_EQ(sizeof(marker), read);
	EXPECT_EQ(0x53, marker);
	EXPECT_TRUE(VerifyNamedPipeClientCurrentUser(server.Get(), diagnostic)) << diagnostic.c_str();
	HANDLE threadToken = nullptr;
	EXPECT_FALSE(::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &threadToken));
	if (threadToken) {
		::CloseHandle(threadToken);
	}
	::SetEvent(release.Get());
	client.join();
}

} // namespace platform::controlipc
