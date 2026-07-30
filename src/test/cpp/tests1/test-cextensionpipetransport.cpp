/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionPipeTransport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

using namespace std::chrono_literals;

class CurrentUserPipeSecurity final {
public:
	CurrentUserPipeSecurity()
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return;
		}
		DWORD bytes = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
		m_user.resize(bytes);
		if (!bytes || !::GetTokenInformation(token, TokenUser, m_user.data(), bytes, &bytes)) {
			::CloseHandle(token);
			return;
		}
		::CloseHandle(token);
		const auto sid = reinterpret_cast<TOKEN_USER*>(m_user.data())->User.Sid;
		const DWORD aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + ::GetLengthSid(sid);
		m_acl.resize(aclBytes);
		auto* acl = reinterpret_cast<ACL*>(m_acl.data());
		if (!::InitializeAcl(acl, aclBytes, ACL_REVISION) ||
			!::AddAccessAllowedAceEx(acl, ACL_REVISION, 0, GENERIC_ALL, sid) ||
			!::InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
			!::SetSecurityDescriptorDacl(&m_descriptor, TRUE, acl, FALSE)) {
			return;
		}
		m_attributes.nLength = sizeof(m_attributes);
		m_attributes.lpSecurityDescriptor = &m_descriptor;
		m_attributes.bInheritHandle = FALSE;
		m_valid = true;
	}
	SECURITY_ATTRIBUTES* Get() noexcept { return m_valid ? &m_attributes : nullptr; }
private:
	std::vector<std::uint8_t> m_user;
	std::vector<std::uint8_t> m_acl;
	SECURITY_DESCRIPTOR m_descriptor{};
	SECURITY_ATTRIBUTES m_attributes{};
	bool m_valid = false;
};

class RecordingSink final : public IExtensionPipeTransportSink {
public:
	void OnExtensionPipeBytes(std::vector<std::uint8_t> value) noexcept override
	{
		std::lock_guard lock(mutex);
		bytes.insert(bytes.end(), value.begin(), value.end());
		callbackThread = std::this_thread::get_id();
		condition.notify_all();
	}
	void OnExtensionPipeClosed(std::uint32_t error, std::wstring) noexcept override
	{
		std::lock_guard lock(mutex);
		closeError = error;
		condition.notify_all();
	}
	bool WaitForBytes(std::size_t count)
	{
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, 2s, [&] { return bytes.size() >= count; });
	}
	std::mutex mutex;
	std::condition_variable condition;
	std::vector<std::uint8_t> bytes;
	std::thread::id callbackThread;
	std::uint32_t closeError = 0;
};

std::wstring UniquePipeName(const wchar_t* suffix)
{
	static std::atomic_uint32_t sequence{};
	return L"\\\\.\\pipe\\sakura-exthost-test-" + std::to_wstring(::GetCurrentProcessId()) +
		L"-" + std::to_wstring(++sequence) + L"-" + suffix;
}

HANDLE CreateTestPipe(const std::wstring& name, SECURITY_ATTRIBUTES* security)
{
	return ::CreateNamedPipeW(
		name.c_str(),
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		4096,
		4096,
		0,
		security);
}

} // namespace

TEST(CExtensionPipeTransport, ConnectsOnlyToExpectedPidAndReadsOnDedicatedThread)
{
	CurrentUserPipeSecurity security;
	ASSERT_NE(nullptr, security.Get());
	const auto pipeName = UniquePipeName(L"roundtrip");
	const HANDLE server = CreateTestPipe(pipeName, security.Get());
	ASSERT_NE(INVALID_HANDLE_VALUE, server);
	const std::vector<std::uint8_t> fromServer{ 1, 2, 3, 4 };
	const std::vector<std::uint8_t> fromClient{ 5, 6, 7 };
	std::vector<std::uint8_t> serverReceived(fromClient.size());
	std::thread serverThread([&] {
		const BOOL connected = ::ConnectNamedPipe(server, nullptr);
		ASSERT_TRUE(connected || ::GetLastError() == ERROR_PIPE_CONNECTED);
		DWORD written = 0;
		ASSERT_TRUE(::WriteFile(server, fromServer.data(), static_cast<DWORD>(fromServer.size()), &written, nullptr));
		DWORD read = 0;
		ASSERT_TRUE(::ReadFile(server, serverReceived.data(), static_cast<DWORD>(serverReceived.size()), &read, nullptr));
		EXPECT_EQ(serverReceived.size(), read);
	});

	RecordingSink sink;
	CExtensionPipeTransport transport(sink);
	const auto callerThread = std::this_thread::get_id();
	const auto connected = transport.Connect(pipeName, ::GetCurrentProcessId(), 2s);
	ASSERT_TRUE(connected.success) << connected.errorCode;
	ASSERT_TRUE(sink.WaitForBytes(fromServer.size()));
	EXPECT_EQ(fromServer, sink.bytes);
	EXPECT_NE(callerThread, sink.callbackThread);
	const auto sent = transport.Send(fromClient, 2s);
	EXPECT_TRUE(sent.success) << sent.errorCode;
	serverThread.join();
	EXPECT_EQ(fromClient, serverReceived);
	transport.Close();
	::CloseHandle(server);
}

TEST(CExtensionPipeTransport, RejectsServerPidMismatch)
{
	CurrentUserPipeSecurity security;
	ASSERT_NE(nullptr, security.Get());
	const auto pipeName = UniquePipeName(L"pid");
	const HANDLE server = CreateTestPipe(pipeName, security.Get());
	ASSERT_NE(INVALID_HANDLE_VALUE, server);
	RecordingSink sink;
	CExtensionPipeTransport transport(sink);

	const auto connected = transport.Connect(pipeName, ::GetCurrentProcessId() + 1, 500ms);
	EXPECT_FALSE(connected.success);
	EXPECT_EQ(ERROR_ACCESS_DENIED, connected.errorCode);
	EXPECT_FALSE(transport.IsConnected());
	::CloseHandle(server);
}

TEST(CExtensionPipeTransport, RejectsDefaultDaclThatAllowsOtherPrincipals)
{
	const auto pipeName = UniquePipeName(L"dacl");
	const HANDLE server = CreateTestPipe(pipeName, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, server);
	RecordingSink sink;
	CExtensionPipeTransport transport(sink);

	const auto connected = transport.Connect(pipeName, ::GetCurrentProcessId(), 500ms);
	EXPECT_FALSE(connected.success);
	EXPECT_EQ(ERROR_ACCESS_DENIED, connected.errorCode);
	EXPECT_FALSE(transport.IsConnected());
	::CloseHandle(server);
}

TEST(CExtensionPipeTransport, RejectsUnsafePipeNameWithoutPolling)
{
	RecordingSink sink;
	CExtensionPipeTransport transport(sink);
	const auto connected = transport.Connect(L"\\\\.\\pipe\\other", ::GetCurrentProcessId(), 5s);
	EXPECT_FALSE(connected.success);
	EXPECT_EQ(ERROR_INVALID_PARAMETER, connected.errorCode);
}
