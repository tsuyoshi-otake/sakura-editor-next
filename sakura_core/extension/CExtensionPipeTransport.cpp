/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionPipeTransport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

namespace {

class UniqueHandle final {
public:
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
	~UniqueHandle() { Reset(); }
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.Release()) {}
	UniqueHandle& operator=(UniqueHandle&& other) noexcept
	{
		if (this != &other) {
			Reset(other.Release());
		}
		return *this;
	}
	HANDLE Get() const noexcept { return m_value; }
	explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }
	HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (*this) {
			::CloseHandle(m_value);
		}
		m_value = value;
	}
private:
	HANDLE m_value = nullptr;
};

std::wstring FormatWindowsError(DWORD error)
{
	wchar_t* raw = nullptr;
	const DWORD count = ::FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		error,
		0,
		reinterpret_cast<wchar_t*>(&raw),
		0,
		nullptr);
	std::wstring result = count && raw ? std::wstring(raw, count) : L"Windows error " + std::to_wstring(error);
	if (raw) {
		::LocalFree(raw);
	}
	while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
		result.pop_back();
	}
	return result;
}

SExtensionPipeConnectResult ConnectFailure(DWORD error, std::wstring operation)
{
	return { false, error, 0, std::move(operation) + L": " + FormatWindowsError(error) };
}

SExtensionPipeWriteResult WriteFailure(DWORD error, std::wstring operation)
{
	return { false, error, std::move(operation) + L": " + FormatWindowsError(error) };
}

bool ReadCurrentUserSid(std::vector<std::uint8_t>& storage, PSID& sid)
{
	UniqueHandle token;
	HANDLE rawToken = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
		return false;
	}
	token.Reset(rawToken);
	DWORD bytes = 0;
	::GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
	if (bytes == 0) {
		return false;
	}
	storage.resize(bytes);
	if (!::GetTokenInformation(token.Get(), TokenUser, storage.data(), bytes, &bytes)) {
		return false;
	}
	sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
	return ::IsValidSid(sid) != FALSE;
}

bool IsAllowedAceType(BYTE type) noexcept
{
	return type == ACCESS_ALLOWED_ACE_TYPE ||
		type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
		type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
		type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
}

PSID GetAllowedAceSid(ACE_HEADER* header) noexcept
{
	switch (header->AceType) {
	case ACCESS_ALLOWED_ACE_TYPE:
	case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
		return &reinterpret_cast<ACCESS_ALLOWED_ACE*>(header)->SidStart;
	case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
	case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE: {
		auto* ace = reinterpret_cast<ACCESS_ALLOWED_OBJECT_ACE*>(header);
		auto* sidBytes = reinterpret_cast<std::uint8_t*>(&ace->Flags) + sizeof(ace->Flags);
		if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) {
			sidBytes += sizeof(GUID);
		}
		if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) {
			sidBytes += sizeof(GUID);
		}
		return sidBytes;
	}
	default:
		return nullptr;
	}
}

std::wstring DescribeDacl(PSECURITY_DESCRIPTOR descriptor)
{
	wchar_t* raw = nullptr;
	ULONG length = 0;
	if (!::ConvertSecurityDescriptorToStringSecurityDescriptorW(
		descriptor,
		SDDL_REVISION_1,
		DACL_SECURITY_INFORMATION,
		&raw,
		&length)) {
		return L"DACL unavailable";
	}
	struct StringGuard {
		wchar_t* value;
		~StringGuard() { if (value) ::LocalFree(value); }
	} guard{ raw };
	return std::wstring(raw, length);
}

bool HasCurrentUserOnlyDacl(HANDLE pipe, std::wstring& diagnostic)
{
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	const DWORD error = ::GetSecurityInfo(
		pipe,
		SE_KERNEL_OBJECT,
		DACL_SECURITY_INFORMATION,
		nullptr,
		nullptr,
		&dacl,
		nullptr,
		&descriptor);
	struct DescriptorGuard {
		PSECURITY_DESCRIPTOR value;
		~DescriptorGuard() { if (value) ::LocalFree(value); }
	} descriptorGuard{ descriptor };
	if (error != ERROR_SUCCESS || !dacl || !::IsValidAcl(dacl)) {
		diagnostic = L"Read extension host pipe DACL: " + FormatWindowsError(error);
		return false;
	}

	std::vector<std::uint8_t> userStorage;
	PSID currentUser = nullptr;
	if (!ReadCurrentUserSid(userStorage, currentUser)) {
		diagnostic = L"Read current user SID while validating extension host pipe";
		return false;
	}

	ACL_SIZE_INFORMATION aclInfo{};
	if (!::GetAclInformation(dacl, &aclInfo, sizeof(aclInfo), AclSizeInformation)) {
		diagnostic = L"Inspect extension host pipe ACL: " + FormatWindowsError(::GetLastError());
		return false;
	}
	bool currentUserAllowed = false;
	for (DWORD index = 0; index < aclInfo.AceCount; ++index) {
		ACE_HEADER* header = nullptr;
		if (!::GetAce(dacl, index, reinterpret_cast<void**>(&header)) || !header) {
			diagnostic = L"Read extension host pipe ACE: " + FormatWindowsError(::GetLastError());
			return false;
		}
		if (!IsAllowedAceType(header->AceType)) {
			continue;
		}
		const PSID allowedSid = GetAllowedAceSid(header);
		if (!allowedSid || !::IsValidSid(allowedSid) || !::EqualSid(allowedSid, currentUser)) {
			diagnostic = L"Extension host pipe allows a principal other than the current user: " +
				DescribeDacl(descriptor);
			return false;
		}
		currentUserAllowed = true;
	}
	if (!currentUserAllowed) {
		diagnostic = L"Extension host pipe does not grant access to the current user: " + DescribeDacl(descriptor);
		return false;
	}
	return true;
}

bool IsSafePipeName(std::wstring_view pipeName)
{
	static constexpr std::wstring_view prefix = L"\\\\.\\pipe\\sakura-exthost-";
	if (!pipeName.starts_with(prefix) || pipeName.size() <= prefix.size() || pipeName.find(L'\0') != std::wstring_view::npos) {
		return false;
	}
	return std::all_of(pipeName.begin() + static_cast<std::ptrdiff_t>(prefix.size()), pipeName.end(), [](wchar_t character) {
		return (character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') || character == L'-' || character == L'_';
	});
}

DWORD MillisecondsToDword(std::chrono::milliseconds timeout) noexcept
{
	return static_cast<DWORD>(std::clamp<std::int64_t>(
		timeout.count(), 0, static_cast<std::int64_t>(MAXDWORD) - 1));
}

} // namespace

class CExtensionPipeTransport::Impl {
public:
	explicit Impl(IExtensionPipeTransportSink& target) : sink(target) {}

	void ReadLoop() noexcept
	{
		std::array<std::uint8_t, 64 * 1024> buffer{};
		DWORD closeError = ERROR_SUCCESS;
		for (;;) {
			if (stopping.load(std::memory_order_acquire)) {
				break;
			}
			UniqueHandle completed(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (!completed) {
				closeError = ::GetLastError();
				break;
			}
			OVERLAPPED overlapped{};
			overlapped.hEvent = completed.Get();
			DWORD bytesRead = 0;
			BOOL read = ::ReadFile(
				pipe.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);
			if (!read) {
				const DWORD error = ::GetLastError();
				if (error != ERROR_IO_PENDING) {
					closeError = error;
					break;
				}
				const HANDLE waits[] = { stopEvent.Get(), completed.Get() };
				const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
				if (wait == WAIT_OBJECT_0) {
					::CancelIoEx(pipe.Get(), &overlapped);
					::WaitForSingleObject(completed.Get(), INFINITE);
					break;
				}
				if (wait != WAIT_OBJECT_0 + 1 ||
					!::GetOverlappedResult(pipe.Get(), &overlapped, &bytesRead, FALSE)) {
					closeError = ::GetLastError();
					break;
				}
			}
			if (bytesRead == 0) {
				closeError = ERROR_BROKEN_PIPE;
				break;
			}
			sink.OnExtensionPipeBytes(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + bytesRead));
		}
		if (!stopping.load(std::memory_order_acquire) && !closedNotified.exchange(true)) {
			if (closeError == ERROR_SUCCESS) {
				closeError = ERROR_BROKEN_PIPE;
			}
			sink.OnExtensionPipeClosed(closeError, L"Read extension host pipe: " + FormatWindowsError(closeError));
		}
	}

	IExtensionPipeTransportSink& sink;
	UniqueHandle pipe;
	UniqueHandle stopEvent;
	std::thread reader;
	std::mutex writeMutex;
	std::atomic_bool stopping{ true };
	std::atomic_bool closedNotified{ false };
};

CExtensionPipeTransport::CExtensionPipeTransport(IExtensionPipeTransportSink& sink)
	: m_impl(std::make_unique<Impl>(sink))
{
}

CExtensionPipeTransport::~CExtensionPipeTransport()
{
	Close();
}

SExtensionPipeConnectResult CExtensionPipeTransport::Connect(
	std::wstring pipeName,
	std::uint32_t expectedServerProcessId,
	std::chrono::milliseconds timeout)
{
	Close();
	if (!IsSafePipeName(pipeName) || expectedServerProcessId == 0 || timeout.count() <= 0) {
		return ConnectFailure(ERROR_INVALID_PARAMETER, L"Validate extension host pipe connection");
	}

	const auto deadline = std::chrono::steady_clock::now() + timeout;
	std::chrono::milliseconds retryDelay{ 10 };
	UniqueHandle pipe;
	DWORD lastError = ERROR_FILE_NOT_FOUND;
	while (std::chrono::steady_clock::now() < deadline) {
		pipe.Reset(::CreateFileW(
			pipeName.c_str(),
			GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
			nullptr));
		if (pipe) {
			break;
		}
		lastError = ::GetLastError();
		if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY) {
			return ConnectFailure(lastError, L"Open extension host pipe");
		}
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now());
		if (remaining.count() <= 0) {
			break;
		}
		const auto wait = (std::min)(retryDelay, remaining);
		if (lastError == ERROR_PIPE_BUSY) {
			::WaitNamedPipeW(pipeName.c_str(), MillisecondsToDword(wait));
		}
		else {
			::Sleep(MillisecondsToDword(wait));
		}
		retryDelay = (std::min)(retryDelay * 2, std::chrono::milliseconds(100));
	}
	if (!pipe) {
		return ConnectFailure(lastError == ERROR_SUCCESS ? ERROR_SEM_TIMEOUT : lastError, L"Open extension host pipe");
	}
	std::wstring daclDiagnostic;
	if (::GetFileType(pipe.Get()) != FILE_TYPE_PIPE || !HasCurrentUserOnlyDacl(pipe.Get(), daclDiagnostic)) {
		if (daclDiagnostic.empty()) {
			daclDiagnostic = L"Validate extension host pipe DACL";
		}
		return { false, ERROR_ACCESS_DENIED, 0, std::move(daclDiagnostic) };
	}
	ULONG serverProcessId = 0;
	if (!::GetNamedPipeServerProcessId(pipe.Get(), &serverProcessId)) {
		return ConnectFailure(::GetLastError(), L"Read extension host pipe server PID");
	}
	if (serverProcessId != expectedServerProcessId) {
		return ConnectFailure(ERROR_ACCESS_DENIED, L"Validate extension host pipe server PID");
	}

	UniqueHandle stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!stopEvent) {
		return ConnectFailure(::GetLastError(), L"Create extension host pipe stop event");
	}
	m_impl->pipe = std::move(pipe);
	m_impl->stopEvent = std::move(stopEvent);
	m_impl->closedNotified.store(false);
	m_impl->stopping.store(false, std::memory_order_release);
	try {
		m_impl->reader = std::thread([impl = m_impl.get()] { impl->ReadLoop(); });
	}
	catch (...) {
		m_impl->stopping.store(true);
		m_impl->pipe.Reset();
		m_impl->stopEvent.Reset();
		return ConnectFailure(ERROR_NOT_ENOUGH_MEMORY, L"Start extension host pipe read thread");
	}
	return { true, ERROR_SUCCESS, serverProcessId, {} };
}

SExtensionPipeWriteResult CExtensionPipeTransport::Send(
	std::span<const std::uint8_t> bytes,
	std::chrono::milliseconds timeout)
{
	if (bytes.empty()) {
		return { true, ERROR_SUCCESS, {} };
	}
	std::lock_guard lock(m_impl->writeMutex);
	if (!m_impl->pipe || m_impl->stopping.load(std::memory_order_acquire) || timeout.count() <= 0) {
		return WriteFailure(ERROR_PIPE_NOT_CONNECTED, L"Write extension host pipe");
	}

	std::size_t offset = 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (offset < bytes.size()) {
		const auto chunk = static_cast<DWORD>((std::min)(
			bytes.size() - offset,
			static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		UniqueHandle completed(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!completed) {
			return WriteFailure(::GetLastError(), L"Create extension host pipe write event");
		}
		OVERLAPPED overlapped{};
		overlapped.hEvent = completed.Get();
		DWORD written = 0;
		BOOL write = ::WriteFile(m_impl->pipe.Get(), bytes.data() + offset, chunk, &written, &overlapped);
		if (!write) {
			const DWORD error = ::GetLastError();
			if (error != ERROR_IO_PENDING) {
				return WriteFailure(error, L"Write extension host pipe");
			}
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now());
			if (remaining.count() <= 0) {
				::CancelIoEx(m_impl->pipe.Get(), &overlapped);
				::WaitForSingleObject(completed.Get(), INFINITE);
				return WriteFailure(ERROR_SEM_TIMEOUT, L"Write extension host pipe");
			}
			const HANDLE waits[] = { m_impl->stopEvent.Get(), completed.Get() };
			const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, MillisecondsToDword(remaining));
			if (wait != WAIT_OBJECT_0 + 1) {
				::CancelIoEx(m_impl->pipe.Get(), &overlapped);
				::WaitForSingleObject(completed.Get(), INFINITE);
				return WriteFailure(
					wait == WAIT_TIMEOUT ? ERROR_SEM_TIMEOUT : ERROR_OPERATION_ABORTED,
					L"Write extension host pipe");
			}
			if (!::GetOverlappedResult(m_impl->pipe.Get(), &overlapped, &written, FALSE)) {
				return WriteFailure(::GetLastError(), L"Complete extension host pipe write");
			}
		}
		if (written == 0) {
			return WriteFailure(ERROR_BROKEN_PIPE, L"Write extension host pipe");
		}
		offset += written;
	}
	return { true, ERROR_SUCCESS, {} };
}

void CExtensionPipeTransport::Close() noexcept
{
	m_impl->stopping.store(true, std::memory_order_release);
	if (m_impl->stopEvent) {
		::SetEvent(m_impl->stopEvent.Get());
	}
	if (m_impl->pipe) {
		::CancelIoEx(m_impl->pipe.Get(), nullptr);
	}
	if (m_impl->reader.joinable()) {
		if (m_impl->reader.get_id() == std::this_thread::get_id()) {
			return;
		}
		m_impl->reader.join();
	}
	std::lock_guard lock(m_impl->writeMutex);
	m_impl->pipe.Reset();
	m_impl->stopEvent.Reset();
}

bool CExtensionPipeTransport::IsConnected() const noexcept
{
	return m_impl->pipe && !m_impl->stopping.load(std::memory_order_acquire);
}
