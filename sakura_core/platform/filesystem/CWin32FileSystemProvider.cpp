/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "platform/filesystem/CWin32FileSystemProvider.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cwchar>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace platform::filesystem {
namespace {

constexpr std::size_t kWatchQueueCapacity = 256;
constexpr DWORD kWatchBufferBytes = 16 * 1024;
constexpr unsigned int kTemporaryCreateAttempts = 32;

// Win32 has no compare-version-and-rename primitive for arbitrary cooperating
// and non-cooperating processes.  This mutex closes the race among Sakura
// writers in this process; the path is revalidated immediately before the
// atomic publish for optimistic conflict detection against external writers.
std::mutex g_conditionalReplaceMutex;
std::atomic_uint64_t g_temporarySequence{ 0 };

class CFindHandleGuard final {
public:
	explicit CFindHandleGuard(HANDLE handle) noexcept : m_handle(handle) {}
	~CFindHandleGuard()
	{
		if (m_handle != INVALID_HANDLE_VALUE) (void)::FindClose(m_handle);
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }

	CFindHandleGuard(const CFindHandleGuard&) = delete;
	CFindHandleGuard& operator=(const CFindHandleGuard&) = delete;

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class CKernelHandleGuard final {
public:
	explicit CKernelHandleGuard(HANDLE handle) noexcept : m_handle(handle) {}
	~CKernelHandleGuard()
	{
		if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) (void)::CloseHandle(m_handle);
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
	void Release() noexcept { m_handle = INVALID_HANDLE_VALUE; }

	CKernelHandleGuard(const CKernelHandleGuard&) = delete;
	CKernelHandleGuard& operator=(const CKernelHandleGuard&) = delete;

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool HasPrefixCaseInsensitive(std::wstring_view value, std::wstring_view prefix) noexcept
{
	return value.size() >= prefix.size() && _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

[[nodiscard]] bool IsDevicePath(std::wstring_view path) noexcept
{
	return HasPrefixCaseInsensitive(path, L"\\\\.\\") || HasPrefixCaseInsensitive(path, L"\\\\?\\");
}

[[nodiscard]] bool IsAbsoluteDrivePath(std::wstring_view path) noexcept
{
	const bool driveLetter = path.size() >= 1 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
	return path.size() >= 3 && driveLetter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

[[nodiscard]] std::wstring ToExtendedPath(std::wstring path)
{
	// The prefix is added only after URI validation.  It is an implementation
	// detail for long Win32 paths, not a device path accepted from a caller.
	if (path.size() < MAX_PATH || IsDevicePath(path)) return path;
	if (HasPrefixCaseInsensitive(path, L"\\\\")) {
		return L"\\\\?\\UNC\\" + path.substr(2);
	}
	return L"\\\\?\\" + path;
}

[[nodiscard]] FileResult<std::wstring> ToLocalPath(const platform::uri::Uri& resource)
{
	if (resource.Scheme() != L"file" || resource.Query().has_value() || resource.Fragment().has_value()) {
		return FileResult<std::wstring>::Failure(EFileResultStatus::InvalidUri, L"a local provider accepts only plain file: URIs");
	}
	auto path = resource.ToWindowsPath();
	if (!path || IsDevicePath(*path.value)) {
		return FileResult<std::wstring>::Failure(EFileResultStatus::InvalidUri, L"the URI is not a local Windows file path");
	}
	// Uri::ToWindowsPath deliberately performs no filesystem normalization.  Do
	// not let a syntactically valid file:///C: URI become drive-relative Win32
	// I/O through this provider.
	if (!HasPrefixCaseInsensitive(*path.value, L"\\\\") && !IsAbsoluteDrivePath(*path.value)) {
		return FileResult<std::wstring>::Failure(EFileResultStatus::InvalidUri, L"the file URI does not contain an absolute Windows path");
	}
	return FileResult<std::wstring>::Success(std::move(*path.value));
}

[[nodiscard]] EFileResultStatus MapWin32Error(DWORD error) noexcept
{
	switch (error) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
		return EFileResultStatus::NotFound;
	case ERROR_DIRECTORY:
		return EFileResultStatus::NotDirectory;
	case ERROR_ACCESS_DENIED:
	case ERROR_NETWORK_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		return EFileResultStatus::PermissionDenied;
	case ERROR_OPERATION_ABORTED:
		return EFileResultStatus::Cancelled;
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
		return EFileResultStatus::InvalidUri;
	default:
		return EFileResultStatus::Failed;
	}
}

template <class TValue>
[[nodiscard]] FileResult<TValue> FailureFromLastError(std::wstring_view operation)
{
	const DWORD error = ::GetLastError();
	return FileResult<TValue>::Failure(MapWin32Error(error), std::wstring(operation) + L" failed (Win32 error " + std::to_wstring(error) + L")");
}

[[nodiscard]] std::uint64_t FileTimeValue(const FILETIME& value) noexcept
{
	return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

void AppendLittleEndian(
	std::array<std::uint8_t, FileVersionToken::kMaximumBytes>& bytes,
	std::size_t& offset,
	std::uint64_t value,
	std::size_t width) noexcept
{
	for (std::size_t index = 0; index < width; ++index) {
		bytes[offset++] = static_cast<std::uint8_t>(value & 0xffu);
		value >>= 8;
	}
}

struct Win32HandleSnapshot {
	FileVersionToken version;
	std::uint64_t size = 0;
	DWORD attributes = 0;
};

//! Obtain native identity and metadata from the same open handle.
[[nodiscard]] FileResult<Win32HandleSnapshot> SnapshotHandle(HANDLE handle)
{
	BY_HANDLE_FILE_INFORMATION information{};
	if (!::GetFileInformationByHandle(handle, &information)) {
		return FailureFromLastError<Win32HandleSnapshot>(L"GetFileInformationByHandle");
	}

	FILE_ID_INFO fileId{};
	const bool hasExtendedFileId = ::GetFileInformationByHandleEx(
		handle, FileIdInfo, &fileId, sizeof(fileId)) != FALSE;
	FILE_BASIC_INFO basic{};
	const bool hasBasicInformation = ::GetFileInformationByHandleEx(
		handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;

	std::array<std::uint8_t, FileVersionToken::kMaximumBytes> opaque{};
	std::size_t offset = 0;
	opaque[offset++] = 1; // Win32 token schema.
	opaque[offset++] = static_cast<std::uint8_t>(
		(hasExtendedFileId ? 1u : 0u) | (hasBasicInformation ? 2u : 0u));

	AppendLittleEndian(
		opaque, offset,
		hasExtendedFileId ? fileId.VolumeSerialNumber : information.dwVolumeSerialNumber,
		sizeof(std::uint64_t));
	if (hasExtendedFileId) {
		for (const auto value : fileId.FileId.Identifier) opaque[offset++] = value;
	} else {
		AppendLittleEndian(
			opaque, offset,
			(static_cast<std::uint64_t>(information.nFileIndexHigh) << 32)
				| information.nFileIndexLow,
			sizeof(std::uint64_t));
		AppendLittleEndian(opaque, offset, 0, sizeof(std::uint64_t));
	}

	const std::uint64_t size =
		(static_cast<std::uint64_t>(information.nFileSizeHigh) << 32)
		| information.nFileSizeLow;
	AppendLittleEndian(opaque, offset, size, sizeof(std::uint64_t));
	AppendLittleEndian(
		opaque, offset,
		hasBasicInformation
			? static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart)
			: FileTimeValue(information.ftLastWriteTime),
		sizeof(std::uint64_t));
	AppendLittleEndian(
		opaque, offset,
		hasBasicInformation
			? static_cast<std::uint64_t>(basic.ChangeTime.QuadPart)
			: 0,
		sizeof(std::uint64_t));
	AppendLittleEndian(opaque, offset, information.dwFileAttributes, sizeof(std::uint32_t));

	auto token = FileVersionToken::FromOpaqueBytes(
		std::span<const std::uint8_t>(opaque.data(), offset));
	if (!token) {
		return FileResult<Win32HandleSnapshot>::Failure(
			EFileResultStatus::Failed, L"native file version exceeded the bounded token representation");
	}
	return FileResult<Win32HandleSnapshot>::Success({
		.version = std::move(*token),
		.size = size,
		.attributes = information.dwFileAttributes,
	});
}

[[nodiscard]] FileResult<Win32HandleSnapshot> SnapshotPath(
	const std::wstring& extendedPath)
{
	HANDLE handle = ::CreateFileW(
		extendedPath.c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return FailureFromLastError<Win32HandleSnapshot>(L"CreateFileW for file version");
	}
	CKernelHandleGuard guard(handle);
	return SnapshotHandle(handle);
}

[[nodiscard]] FileConditionalReplaceResult ReplaceFailure(
	std::wstring_view operation,
	DWORD error)
{
	const std::wstring diagnostic =
		std::wstring(operation) + L" failed (Win32 error " + std::to_wstring(error) + L")";
	switch (error) {
	case ERROR_INVALID_FUNCTION:
	case ERROR_NOT_SUPPORTED:
	case ERROR_CALL_NOT_IMPLEMENTED:
		return FileConditionalReplaceResult::Unsupported(diagnostic);
	default:
		return FileConditionalReplaceResult::Failure(diagnostic);
	}
}

[[nodiscard]] FileResult<void> WriteAll(HANDLE handle, const FileBytes& bytes)
{
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto remaining = bytes.size() - offset;
		const DWORD request = static_cast<DWORD>(
			(std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD written = 0;
		if (!::WriteFile(handle, bytes.data() + offset, request, &written, nullptr)) {
			return FailureFromLastError<void>(L"WriteFile for conditional replace");
		}
		if (written == 0 || written > request) {
			return FileResult<void>::Failure(
				EFileResultStatus::Failed, L"WriteFile returned a short write");
		}
		offset += written;
	}
	return FileResult<void>::Success();
}

class CTemporaryFileGuard final {
public:
	CTemporaryFileGuard(HANDLE handle, std::wstring path)
		: m_handle(handle)
		, m_path(std::move(path))
	{
	}

	~CTemporaryFileGuard()
	{
		if (m_handle != INVALID_HANDLE_VALUE) (void)::CloseHandle(m_handle);
		if (m_ownsPath) (void)::DeleteFileW(m_path.c_str());
	}

	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
	void MarkPublished() noexcept { m_ownsPath = false; }

	//! Release the writable handle and keep only an attribute handle.
	//! ReplaceFileW opens the staged replacement with restrictive sharing, so a
	//! surviving data handle fails publication with ERROR_SHARING_VIOLATION.  An
	//! attribute-only open takes no part in the Win32 share check, yet the handle
	//! still follows the file object through the rename and can therefore report
	//! the committed identity after publication.
	[[nodiscard]] bool ReopenForAttributesOnly() noexcept
	{
		if (m_handle != INVALID_HANDLE_VALUE) {
			(void)::CloseHandle(m_handle);
			m_handle = INVALID_HANDLE_VALUE;
		}
		HANDLE handle = ::CreateFileW(
			m_path.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;
		m_handle = handle;
		return true;
	}

	CTemporaryFileGuard(const CTemporaryFileGuard&) = delete;
	CTemporaryFileGuard& operator=(const CTemporaryFileGuard&) = delete;

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
	std::wstring m_path;
	bool m_ownsPath = true;
};

[[nodiscard]] EFileEntryType EntryType(DWORD attributes) noexcept
{
	// A reparse point is intentionally a leaf.  In particular, directory
	// symbolic links are never followed by enumeration or watches in this slice.
	if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return EFileEntryType::SymbolicLink;
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return EFileEntryType::Directory;
	if ((attributes & FILE_ATTRIBUTE_NORMAL) != 0 || attributes != INVALID_FILE_ATTRIBUTES) return EFileEntryType::File;
	return EFileEntryType::Other;
}

[[nodiscard]] std::uint64_t FileSize(DWORD high, DWORD low) noexcept
{
	return (static_cast<std::uint64_t>(high) << 32) | low;
}

[[nodiscard]] std::wstring AppendPath(const std::wstring& directory, std::wstring_view child)
{
	std::wstring result = directory;
	if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
	result.append(child);
	return result;
}

[[nodiscard]] FileResult<platform::uri::Uri> ChildUri(const std::wstring& directory, std::wstring_view name)
{
	auto child = platform::uri::Uri::FromWindowsPath(AppendPath(directory, name));
	if (!child) {
		return FileResult<platform::uri::Uri>::Failure(EFileResultStatus::Failed, L"Win32 child name could not be represented as a URI");
	}
	return FileResult<platform::uri::Uri>::Success(std::move(*child.value));
}

class CWin32FileWatch final : public IFileWatch {
public:
	CWin32FileWatch(platform::uri::Uri root, std::wstring nativeRoot, HANDLE directory)
		: m_root(std::move(root))
		, m_nativeRoot(std::move(nativeRoot))
		, m_directory(directory)
		, m_stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
	{
	}

	~CWin32FileWatch() override
	{
		(void)Cancel();
		JoinAndClose();
	}

	[[nodiscard]] FileResult<void> Start()
	{
		if (m_stopEvent == nullptr) return FileResult<void>::Failure(EFileResultStatus::Failed, L"could not create the directory watch stop event");
		try {
			m_worker = std::thread(&CWin32FileWatch::WorkerMain, this);
			std::unique_lock lock(m_mutex);
			if (!m_eventsChanged.wait_for(lock, std::chrono::seconds(5), [this] { return m_started; })) {
				return FileResult<void>::Failure(EFileResultStatus::Failed, L"directory watch worker did not arm within five seconds");
			}
			return m_startedSuccessfully ? FileResult<void>::Success() : FileResult<void>::Failure(m_startFailure, L"ReadDirectoryChangesW could not arm the watch");
		} catch (...) {
			return FileResult<void>::Failure(EFileResultStatus::Failed, L"could not start the directory watch worker");
		}
	}

	FileResult<void> Cancel() override
	{
		{
			std::lock_guard lock(m_mutex);
			if (m_cancelled) return FileResult<void>::Failure(EFileResultStatus::Cancelled, L"watch is already cancelled");
			m_cancelled = true;
			m_events.clear();
			if (!m_disposedDelivered) {
				m_events.push_back({ .type = EFileWatchEventType::Disposed, .uri = m_root });
			}
		}
		m_eventsChanged.notify_all();
		if (m_stopEvent != nullptr) (void)::SetEvent(m_stopEvent);
		if (m_directory != INVALID_HANDLE_VALUE) (void)::CancelIoEx(m_directory, nullptr);
		JoinAndClose();
		return FileResult<void>::Success();
	}

	FileResult<FileWatchEvent> Next() override
	{
		std::unique_lock lock(m_mutex);
		m_eventsChanged.wait(lock, [this] { return !m_events.empty() || m_disposedDelivered; });
		if (m_events.empty()) {
			return FileResult<FileWatchEvent>::Failure(EFileResultStatus::Cancelled, L"watch has reached its terminal state");
		}
		auto event = std::move(m_events.front());
		m_events.pop_front();
		if (event.type == EFileWatchEventType::Disposed) m_disposedDelivered = true;
		return FileResult<FileWatchEvent>::Success(std::move(event));
	}

private:
	void JoinAndClose() noexcept
	{
		std::lock_guard joinLock(m_joinMutex);
		if (m_worker.joinable()) m_worker.join();
		if (m_directory != INVALID_HANDLE_VALUE) {
			::CloseHandle(m_directory);
			m_directory = INVALID_HANDLE_VALUE;
		}
		if (m_stopEvent != nullptr) {
			::CloseHandle(m_stopEvent);
			m_stopEvent = nullptr;
		}
	}

	[[nodiscard]] bool IsCancelled() const
	{
		std::lock_guard lock(m_mutex);
		return m_cancelled;
	}

	void Push(FileWatchEvent event)
	{
		std::lock_guard lock(m_mutex);
		if (m_cancelled) return;
		if (m_events.size() >= kWatchQueueCapacity) {
			m_events.clear();
			m_events.push_back({ .type = EFileWatchEventType::Overflow, .uri = m_root });
			m_events.push_back({ .type = EFileWatchEventType::RescanRequired, .uri = m_root });
			m_eventsChanged.notify_all();
			return;
		}
		m_events.push_back(std::move(event));
		m_eventsChanged.notify_one();
	}

	void PushOverflowAndRescan()
	{
		std::lock_guard lock(m_mutex);
		if (m_cancelled) return;
		m_events.clear();
		m_events.push_back({ .type = EFileWatchEventType::Overflow, .uri = m_root });
		m_events.push_back({ .type = EFileWatchEventType::RescanRequired, .uri = m_root });
		m_eventsChanged.notify_all();
	}

	void PushTerminalRescan()
	{
		std::lock_guard lock(m_mutex);
		if (m_cancelled || m_terminalQueued || m_disposedDelivered) return;
		m_events.clear();
		m_events.push_back({ .type = EFileWatchEventType::Overflow, .uri = m_root });
		m_events.push_back({ .type = EFileWatchEventType::RescanRequired, .uri = m_root });
		m_events.push_back({ .type = EFileWatchEventType::Disposed, .uri = m_root });
		m_terminalQueued = true;
		m_eventsChanged.notify_all();
	}

	void WorkerMain() noexcept
	{
		HANDLE completionEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (completionEvent == nullptr) {
			{
				std::lock_guard lock(m_mutex);
				m_started = true;
				m_startedSuccessfully = false;
			}
			m_eventsChanged.notify_all();
			PushTerminalRescan();
			return;
		}

		std::vector<std::byte> buffer(kWatchBufferBytes);
		std::optional<platform::uri::Uri> oldRename;
		bool firstRead = true;
		for (;;) {
			if (IsCancelled()) break;
			OVERLAPPED overlapped{};
			overlapped.hEvent = completionEvent;
			DWORD ignored{};
			::ResetEvent(completionEvent);
			const bool readQueued = ::ReadDirectoryChangesW(m_directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
				&ignored, &overlapped, nullptr) != FALSE;
			const DWORD readError = readQueued ? ERROR_SUCCESS : ::GetLastError();
			if (firstRead) {
				std::lock_guard lock(m_mutex);
				m_started = true;
				m_startedSuccessfully = readQueued;
				m_startFailure = MapWin32Error(readError);
				firstRead = false;
				m_eventsChanged.notify_all();
			}
			if (!readQueued) {
				if (readError == ERROR_OPERATION_ABORTED || IsCancelled()) break;
				PushTerminalRescan();
				break;
			}

			HANDLE waitHandles[] = { m_stopEvent, completionEvent };
			if (::WaitForMultipleObjects(static_cast<DWORD>(std::size(waitHandles)), waitHandles, FALSE, INFINITE) == WAIT_OBJECT_0) {
				(void)::CancelIoEx(m_directory, &overlapped);
				break;
			}

			DWORD bytes{};
			if (!::GetOverlappedResult(m_directory, &overlapped, &bytes, FALSE)) {
				if (::GetLastError() == ERROR_OPERATION_ABORTED || IsCancelled()) break;
				PushTerminalRescan();
				break;
			}
			if (bytes == 0) {
				PushOverflowAndRescan();
				continue;
			}

			for (DWORD offset = 0; offset < bytes;) {
				constexpr auto headerBytes = static_cast<DWORD>(offsetof(FILE_NOTIFY_INFORMATION, FileName));
				const DWORD remaining = bytes - offset;
				if (remaining < headerBytes) {
					PushOverflowAndRescan();
					break;
				}
				const auto* notification = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
				if ((notification->FileNameLength % sizeof(wchar_t)) != 0 ||
					notification->FileNameLength > remaining - headerBytes) {
					PushOverflowAndRescan();
					break;
				}
				const std::wstring_view name(notification->FileName, notification->FileNameLength / sizeof(wchar_t));
				auto child = ChildUri(m_nativeRoot, name);
				if (!child) {
					PushOverflowAndRescan();
				} else {
					if (notification->Action != FILE_ACTION_RENAMED_NEW_NAME && oldRename) {
						Push({ .type = EFileWatchEventType::Deleted, .uri = std::move(*oldRename) });
						oldRename.reset();
					}
					switch (notification->Action) {
					case FILE_ACTION_ADDED:
						Push({ .type = EFileWatchEventType::Created, .uri = std::move(*child.value) });
						break;
					case FILE_ACTION_REMOVED:
						Push({ .type = EFileWatchEventType::Deleted, .uri = std::move(*child.value) });
						break;
					case FILE_ACTION_MODIFIED:
						Push({ .type = EFileWatchEventType::Changed, .uri = std::move(*child.value) });
						break;
					case FILE_ACTION_RENAMED_OLD_NAME:
						oldRename = std::move(*child.value);
						break;
					case FILE_ACTION_RENAMED_NEW_NAME:
						if (oldRename) {
							Push({ .type = EFileWatchEventType::Renamed, .uri = std::move(*child.value), .previousUri = std::move(*oldRename) });
							oldRename.reset();
						} else {
							Push({ .type = EFileWatchEventType::Created, .uri = std::move(*child.value) });
						}
						break;
					default:
						PushOverflowAndRescan();
						break;
					}
				}
				if (notification->NextEntryOffset == 0) break;
				if (notification->NextEntryOffset < headerBytes + notification->FileNameLength ||
					notification->NextEntryOffset > remaining ||
					(notification->NextEntryOffset % alignof(DWORD)) != 0) {
					PushOverflowAndRescan();
					break;
				}
				offset += notification->NextEntryOffset;
			}
		}
		::CloseHandle(completionEvent);
	}

	platform::uri::Uri m_root;
	std::wstring m_nativeRoot;
	HANDLE m_directory = INVALID_HANDLE_VALUE;
	HANDLE m_stopEvent = nullptr;
	std::thread m_worker;
	mutable std::mutex m_mutex;
	std::mutex m_joinMutex;
	std::condition_variable m_eventsChanged;
	std::deque<FileWatchEvent> m_events;
	bool m_cancelled = false;
	bool m_disposedDelivered = false;
	bool m_started = false;
	bool m_startedSuccessfully = false;
	bool m_terminalQueued = false;
	EFileResultStatus m_startFailure = EFileResultStatus::Failed;
};

} // namespace

FileSystemCapabilities CWin32FileSystemProvider::Capabilities() const noexcept
{
	return EFileSystemCapability::Stat
		| EFileSystemCapability::Enumerate
		| EFileSystemCapability::Read
		| EFileSystemCapability::Write
		| EFileSystemCapability::AtomicReplace
		| EFileSystemCapability::Watch;
}

FileResult<FileStat> CWin32FileSystemProvider::Stat(const platform::uri::Uri& resource)
{
	auto path = ToLocalPath(resource);
	if (!path) return FileResult<FileStat>::Failure(path.status, std::move(path.diagnostic));

	WIN32_FILE_ATTRIBUTE_DATA data{};
	const auto extendedPath = ToExtendedPath(std::move(*path.value));
	if (!::GetFileAttributesExW(extendedPath.c_str(), GetFileExInfoStandard, &data)) {
		return FailureFromLastError<FileStat>(L"GetFileAttributesExW");
	}
	return FileResult<FileStat>::Success({
		.uri = resource,
		.type = EntryType(data.dwFileAttributes),
		.size = FileSize(data.nFileSizeHigh, data.nFileSizeLow),
	});
}

FileResult<std::vector<DirectoryEntry>> CWin32FileSystemProvider::Enumerate(const platform::uri::Uri& directory)
{
	auto path = ToLocalPath(directory);
	if (!path) return FileResult<std::vector<DirectoryEntry>>::Failure(path.status, std::move(path.diagnostic));
	const std::wstring nativePath = std::move(*path.value);
	const auto attributes = ::GetFileAttributesW(ToExtendedPath(nativePath).c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) return FailureFromLastError<std::vector<DirectoryEntry>>(L"GetFileAttributesW");
	if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported, L"directory enumeration does not follow reparse points");
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::NotDirectory, L"enumeration requires a directory URI");
	}
	const std::wstring search = ToExtendedPath(AppendPath(nativePath, L"*"));

	WIN32_FIND_DATAW data{};
	HANDLE find = ::FindFirstFileExW(search.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) return FailureFromLastError<std::vector<DirectoryEntry>>(L"FindFirstFileExW");
	CFindHandleGuard findGuard(find);

	std::vector<DirectoryEntry> entries;
	do {
		if (std::wcscmp(data.cFileName, L".") == 0 || std::wcscmp(data.cFileName, L"..") == 0) continue;
		auto child = ChildUri(nativePath, data.cFileName);
		if (!child) {
			return FileResult<std::vector<DirectoryEntry>>::Failure(child.status, std::move(child.diagnostic));
		}
		FileStat stat{ .uri = *child.value, .type = EntryType(data.dwFileAttributes), .size = FileSize(data.nFileSizeHigh, data.nFileSizeLow) };
		entries.push_back({ .uri = *child.value, .name = data.cFileName, .stat = std::move(stat) });
	} while (::FindNextFileW(findGuard.Get(), &data));

	const DWORD lastError = ::GetLastError();
	if (lastError != ERROR_NO_MORE_FILES) {
		return FileResult<std::vector<DirectoryEntry>>::Failure(MapWin32Error(lastError), L"FindNextFileW failed (Win32 error " + std::to_wstring(lastError) + L")");
	}
	return FileResult<std::vector<DirectoryEntry>>::Success(std::move(entries));
}

FileResult<FileBytes> CWin32FileSystemProvider::Read(
	const platform::uri::Uri& resource,
	const FileReadOptions& options)
{
	auto path = ToLocalPath(resource);
	if (!path) return FileResult<FileBytes>::Failure(path.status, std::move(path.diagnostic));
	if (options.maximumBytes == 0) {
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"file read requires a nonzero maximumBytes");
	}

	const auto extendedPath = ToExtendedPath(std::move(*path.value));
	const auto attributes = ::GetFileAttributesW(extendedPath.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) return FailureFromLastError<FileBytes>(L"GetFileAttributesW");
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		return FileResult<FileBytes>::Failure(EFileResultStatus::NotDirectory, L"file read requires a file URI");
	}

	HANDLE handle = ::CreateFileW(extendedPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return FailureFromLastError<FileBytes>(L"CreateFileW for file read");

	LARGE_INTEGER size{};
	if (!::GetFileSizeEx(handle, &size)) {
		const auto failure = FailureFromLastError<FileBytes>(L"GetFileSizeEx");
		::CloseHandle(handle);
		return failure;
	}
	if (size.QuadPart < 0) {
		::CloseHandle(handle);
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"GetFileSizeEx returned a negative file size");
	}
	const auto byteCount = static_cast<std::uint64_t>(size.QuadPart);
	if (byteCount > options.maximumBytes || byteCount > (std::numeric_limits<std::size_t>::max)()) {
		::CloseHandle(handle);
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"file size exceeds the requested read limit");
	}

	FileBytes bytes;
	if (byteCount > bytes.max_size()) {
		::CloseHandle(handle);
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"file size exceeds the platform allocation limit");
	}
	try {
		bytes.resize(static_cast<std::size_t>(byteCount));
	}
	catch (const std::bad_alloc&) {
		::CloseHandle(handle);
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"file read allocation failed");
	}
	catch (const std::length_error&) {
		::CloseHandle(handle);
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"file size exceeds the platform allocation limit");
	}

	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto remaining = bytes.size() - offset;
		const DWORD request = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD read = 0;
		if (!::ReadFile(handle, bytes.data() + offset, request, &read, nullptr)) {
			const auto failure = FailureFromLastError<FileBytes>(L"ReadFile");
			::CloseHandle(handle);
			return failure;
		}
		if (read == 0 || read > request) {
			::CloseHandle(handle);
			return FileResult<FileBytes>::Failure(EFileResultStatus::Failed, L"ReadFile returned a short read");
		}
		offset += read;
	}

	::CloseHandle(handle);
	return FileResult<FileBytes>::Success(std::move(bytes));
}

FileResult<FileContentSnapshot> CWin32FileSystemProvider::ReadVersioned(
	const platform::uri::Uri& resource,
	const FileReadOptions& options)
{
	auto path = ToLocalPath(resource);
	if (!path) {
		return FileResult<FileContentSnapshot>::Failure(
			path.status, std::move(path.diagnostic));
	}
	if (options.maximumBytes == 0) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"versioned file read requires a nonzero maximumBytes");
	}

	const auto extendedPath = ToExtendedPath(std::move(*path.value));
	// FILE_FLAG_BACKUP_SEMANTICS is required to obtain a directory handle at all.
	// Without it a directory URI fails the open with ERROR_ACCESS_DENIED and the
	// caller would observe PermissionDenied instead of the NotDirectory terminal
	// that the handle-based check below owns.
	HANDLE handle = ::CreateFileW(
		extendedPath.c_str(),
		GENERIC_READ | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT
			| FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return FailureFromLastError<FileContentSnapshot>(
			L"CreateFileW for versioned file read");
	}
	CKernelHandleGuard guard(handle);

	auto before = SnapshotHandle(handle);
	if (!before) {
		return FileResult<FileContentSnapshot>::Failure(
			before.status, std::move(before.diagnostic));
	}
	if ((before.value->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::NotDirectory, L"versioned file read requires a file URI");
	}
	if ((before.value->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Unsupported,
			L"versioned file read does not follow reparse points");
	}
	if (before.value->size > options.maximumBytes
		|| before.value->size > (std::numeric_limits<std::size_t>::max)()) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"file size exceeds the requested versioned read limit");
	}

	FileBytes bytes;
	if (before.value->size > bytes.max_size()) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"file size exceeds the platform allocation limit");
	}
	try {
		bytes.resize(static_cast<std::size_t>(before.value->size));
	}
	catch (const std::bad_alloc&) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"versioned file read allocation failed");
	}
	catch (const std::length_error&) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"file size exceeds the platform allocation limit");
	}

	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto remaining = bytes.size() - offset;
		const DWORD request = static_cast<DWORD>(
			(std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD read = 0;
		if (!::ReadFile(handle, bytes.data() + offset, request, &read, nullptr)) {
			return FailureFromLastError<FileContentSnapshot>(
				L"ReadFile for versioned file read");
		}
		if (read == 0 || read > request) {
			return FileResult<FileContentSnapshot>::Failure(
				EFileResultStatus::Failed, L"ReadFile returned a short versioned read");
		}
		offset += read;
	}

	auto after = SnapshotHandle(handle);
	if (!after) {
		return FileResult<FileContentSnapshot>::Failure(
			after.status, std::move(after.diagnostic));
	}
	if (!(before.value->version == after.value->version)
		|| before.value->size != after.value->size) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"file changed while the versioned read was in progress");
	}
	return FileResult<FileContentSnapshot>::Success({
		.bytes = std::move(bytes),
		.version = std::move(after.value->version),
	});
}

FileConditionalReplaceResult CWin32FileSystemProvider::ConditionalAtomicReplace(
	const platform::uri::Uri& resource,
	const FileBytes& bytes,
	const FileConditionalReplaceOptions& options)
{
	if (options.expectation == EFileConditionalReplaceExpectation::Current
		&& options.expectedVersion.Empty()) {
		return FileConditionalReplaceResult::Failure(
			L"expected-current conditional replace requires a nonempty version token");
	}

	auto path = ToLocalPath(resource);
	if (!path) {
		return FileConditionalReplaceResult::Failure(std::move(path.diagnostic));
	}
	const auto extendedPath = ToExtendedPath(std::move(*path.value));
	const auto separator = extendedPath.find_last_of(L"\\/");
	if (separator == std::wstring::npos || separator + 1 >= extendedPath.size()) {
		return FileConditionalReplaceResult::Failure(
			L"conditional atomic replace requires a file path with a parent directory");
	}
	const std::wstring parent = extendedPath.substr(0, separator);

	HANDLE temporaryHandle = INVALID_HANDLE_VALUE;
	std::wstring temporaryPath;
	DWORD createError = ERROR_FILE_EXISTS;
	for (unsigned int attempt = 0; attempt < kTemporaryCreateAttempts; ++attempt) {
		const auto sequence = g_temporarySequence.fetch_add(1, std::memory_order_relaxed);
		const std::wstring temporaryName =
			L".sakura-cas-" + std::to_wstring(::GetCurrentProcessId())
			+ L"-" + std::to_wstring(::GetTickCount64())
			+ L"-" + std::to_wstring(sequence) + L".tmp";
		temporaryPath = ToExtendedPath(AppendPath(parent, temporaryName));
		temporaryHandle = ::CreateFileW(
			temporaryPath.c_str(),
			GENERIC_WRITE | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
			nullptr);
		if (temporaryHandle != INVALID_HANDLE_VALUE) break;
		createError = ::GetLastError();
		if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) break;
	}
	if (temporaryHandle == INVALID_HANDLE_VALUE) {
		return ReplaceFailure(L"CreateFileW for conditional replace staging", createError);
	}
	CTemporaryFileGuard temporary(temporaryHandle, temporaryPath);

	auto written = WriteAll(temporary.Get(), bytes);
	if (!written) {
		return FileConditionalReplaceResult::Failure(std::move(written.diagnostic));
	}
	if (!::FlushFileBuffers(temporary.Get())) {
		return ReplaceFailure(
			L"FlushFileBuffers for conditional replace staging", ::GetLastError());
	}
	auto staged = SnapshotHandle(temporary.Get());
	if (!staged) {
		return FileConditionalReplaceResult::Failure(std::move(staged.diagnostic));
	}
	// The staged content is durable from here on, so the writable handle has no
	// remaining purpose and only blocks ReplaceFileW.  Reacquire it for
	// attributes and prove the reopened handle is still the same staged file.
	if (!temporary.ReopenForAttributesOnly()) {
		return ReplaceFailure(
			L"reopening the conditional replace staging file for attributes", ::GetLastError());
	}
	auto restaged = SnapshotHandle(temporary.Get());
	if (!restaged) {
		return FileConditionalReplaceResult::Failure(std::move(restaged.diagnostic));
	}
	if (!(restaged.value->version == staged.value->version)) {
		return FileConditionalReplaceResult::Failure(
			L"the conditional replace staging file changed before publication");
	}

	std::scoped_lock publishLock(g_conditionalReplaceMutex);
	auto observed = SnapshotPath(extendedPath);
	if (options.expectation == EFileConditionalReplaceExpectation::Missing) {
		if (observed) {
			return FileConditionalReplaceResult::Conflict(
				L"expected-missing conditional replace found an existing resource");
		}
		if (observed.status != EFileResultStatus::NotFound) {
			return observed.status == EFileResultStatus::Unsupported
				? FileConditionalReplaceResult::Unsupported(std::move(observed.diagnostic))
				: FileConditionalReplaceResult::Failure(std::move(observed.diagnostic));
		}
	} else {
		if (!observed) {
			if (observed.status == EFileResultStatus::NotFound) {
				return FileConditionalReplaceResult::Conflict(
					L"expected-current conditional replace found a missing resource");
			}
			return observed.status == EFileResultStatus::Unsupported
				? FileConditionalReplaceResult::Unsupported(std::move(observed.diagnostic))
				: FileConditionalReplaceResult::Failure(std::move(observed.diagnostic));
		}
		if ((observed.value->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			return FileConditionalReplaceResult::Failure(
				L"conditional atomic replace cannot replace a directory");
		}
		if ((observed.value->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
			return FileConditionalReplaceResult::Unsupported(
				L"conditional atomic replace does not follow reparse points");
		}
		if (!(observed.value->version == options.expectedVersion)) {
			return FileConditionalReplaceResult::Conflict(
				L"the resource version changed before conditional replace");
		}
	}

	// The check above and Win32 publish are not one kernel transaction for an
	// arbitrary external writer.  ReplaceFileW/MoveFileExW is the atomic
	// publication point; a non-cooperating writer can still win the narrow gap.
	const BOOL published =
		options.expectation == EFileConditionalReplaceExpectation::Missing
		? ::MoveFileExW(
			temporaryPath.c_str(), extendedPath.c_str(), MOVEFILE_WRITE_THROUGH)
		: ::ReplaceFileW(
			extendedPath.c_str(), temporaryPath.c_str(), nullptr,
			REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
	if (!published) {
		const DWORD publishError = ::GetLastError();
		const auto current = SnapshotPath(extendedPath);
		if (options.expectation == EFileConditionalReplaceExpectation::Missing) {
			if (current
				|| publishError == ERROR_FILE_EXISTS
				|| publishError == ERROR_ALREADY_EXISTS) {
				return FileConditionalReplaceResult::Conflict(
					L"resource appeared before expected-missing publish");
			}
		} else {
			if (!current && current.status == EFileResultStatus::NotFound) {
				return FileConditionalReplaceResult::Conflict(
					L"resource disappeared before expected-current publish");
			}
			if (current && !(current.value->version == options.expectedVersion)) {
				return FileConditionalReplaceResult::Conflict(
					L"resource changed before expected-current publish");
			}
		}
		return ReplaceFailure(L"atomic conditional publish", publishError);
	}
	temporary.MarkPublished();

	// Keep the staged handle open across publish.  Its post-publish metadata is
	// the committed file's version even if an external writer immediately
	// supersedes the path before the path readback below.
	auto committed = SnapshotHandle(temporary.Get());
	if (!committed) {
		return FileConditionalReplaceResult::Failure(
			L"conditional publish succeeded but committed-version readback failed: "
			+ committed.diagnostic);
	}
	auto readback = SnapshotPath(extendedPath);
	if (!readback || !(readback.value->version == committed.value->version)) {
		return FileConditionalReplaceResult::Success(
			std::move(committed.value->version),
			L"conditional publish succeeded but an external change superseded the path before readback");
	}
	return FileConditionalReplaceResult::Success(
		std::move(committed.value->version));
}

FileResult<std::unique_ptr<IFileWatch>> CWin32FileSystemProvider::Watch(
	const platform::uri::Uri& resource,
	const FileWatchOptions& options)
{
	auto path = ToLocalPath(resource);
	if (!path) return FileResult<std::unique_ptr<IFileWatch>>::Failure(path.status, std::move(path.diagnostic));
	if (options.recursive) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported, L"recursive Win32 watches are not implemented in the first provider slice");
	}
	const std::wstring nativePath = std::move(*path.value);
	const auto extendedPath = ToExtendedPath(nativePath);
	const auto attributes = ::GetFileAttributesW(extendedPath.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) return FailureFromLastError<std::unique_ptr<IFileWatch>>(L"GetFileAttributesW");
	if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported, L"directory watches do not follow reparse points");
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::NotDirectory, L"watch requires a directory URI");
	}
	HANDLE handle = ::CreateFileW(extendedPath.c_str(), FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return FailureFromLastError<std::unique_ptr<IFileWatch>>(L"CreateFileW for directory watch");

	CKernelHandleGuard handleGuard(handle);
	auto watch = std::make_unique<CWin32FileWatch>(resource, nativePath, handle);
	handleGuard.Release();
	auto started = watch->Start();
	if (!started) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(started.status, std::move(started.diagnostic));
	}
	return FileResult<std::unique_ptr<IFileWatch>>::Success(std::move(watch));
}

} // namespace platform::filesystem
