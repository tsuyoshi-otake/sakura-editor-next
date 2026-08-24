/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "senp/SenpRuntimeService.h"

#include "senp/SenpManagementService.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace senp {
namespace {

using platform::serialization::JsoncValue;

constexpr DWORD kProtocolTimeoutMilliseconds = 1000;
constexpr std::size_t kMaximumFrameBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumCachedLines = 4096;
constexpr std::size_t kMaximumPendingLines = 1024;

class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE value) noexcept : m_value(value) {}
	~ScopedHandle() { Reset(); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) Reset(std::exchange(other.m_value, nullptr));
		return *this;
	}
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value);
		m_value = value;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_value; }
	[[nodiscard]] bool Valid() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }

private:
	HANDLE m_value = nullptr;
};

std::wstring QuoteArgument(std::wstring_view value)
{
	if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
	std::wstring result(1, L'\"');
	std::size_t slashes = 0;
	for (const wchar_t ch : value) {
		if (ch == L'\\') { ++slashes; continue; }
		if (ch == L'\"') {
			result.append(slashes * 2 + 1, L'\\');
			result.push_back(ch);
			slashes = 0;
			continue;
		}
		result.append(slashes, L'\\');
		slashes = 0;
		result.push_back(ch);
	}
	result.append(slashes * 2, L'\\');
	result.push_back(L'\"');
	return result;
}

std::wstring HostExecutable()
{
	std::array<wchar_t, 32768> path{};
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size()) return {};
	return (std::filesystem::path(std::wstring(path.data(), length)).parent_path()
		/ L"sakura-senp-host.exe").native();
}

bool WriteAll(HANDLE pipe, const void* bytes, std::size_t length) noexcept
{
	const auto* cursor = static_cast<const std::byte*>(bytes);
	while (length > 0) {
		DWORD written = 0;
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, 64U * 1024U));
		if (!::WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0) return false;
		cursor += written;
		length -= written;
	}
	return true;
}

bool ReadExactTimed(HANDLE pipe, HANDLE process, void* bytes, std::size_t length) noexcept
{
	auto* cursor = static_cast<std::byte*>(bytes);
	const ULONGLONG deadline = ::GetTickCount64() + kProtocolTimeoutMilliseconds;
	while (length > 0) {
		DWORD available = 0;
		if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
		if (available == 0) {
			if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0 || ::GetTickCount64() >= deadline) return false;
			::Sleep(1);
			continue;
		}
		DWORD read = 0;
		const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(length, available));
		if (!::ReadFile(pipe, cursor, wanted, &read, nullptr) || read == 0) return false;
		cursor += read;
		length -= read;
	}
	return true;
}

class HostProcess final {
public:
	HostProcess() = default;
	~HostProcess() { Close(); }
	HostProcess(const HostProcess&) = delete;
	HostProcess& operator=(const HostProcess&) = delete;
	HostProcess(HostProcess&&) = default;
	HostProcess& operator=(HostProcess&&) = default;

	[[nodiscard]] bool Open(std::wstring_view modulePath)
	{
		Close();
		const auto executable = HostExecutable();
		if (executable.empty() || !std::filesystem::is_regular_file(executable)
			|| !std::filesystem::is_regular_file(std::filesystem::path(modulePath))) return false;
		SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
		ScopedHandle childInput;
		ScopedHandle childOutput;
		if (!::CreatePipe(childInput.Put(), m_input.Put(), &security, 0)
			|| !::CreatePipe(m_output.Put(), childOutput.Put(), &security, 0)) return false;
		::SetHandleInformation(m_input.Get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(m_output.Get(), HANDLE_FLAG_INHERIT, 0);
		ScopedHandle nullError(::CreateFileW(L"NUL", GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = childInput.Get();
		startup.hStdOutput = childOutput.Get();
		startup.hStdError = nullError.Valid() ? nullError.Get() : childOutput.Get();
		std::wstring command = QuoteArgument(executable) + L" --component " + QuoteArgument(modulePath);
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
			TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		childInput.Reset();
		childOutput.Reset();
		if (!created) return false;
		m_process.Reset(process.hProcess);
		m_thread.Reset(process.hThread);
		m_job.Reset(::CreateJobObjectW(nullptr, nullptr));
		m_assignedToJob = false;
		if (m_job.Valid()) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			m_assignedToJob = ::SetInformationJobObject(m_job.Get(), JobObjectExtendedLimitInformation,
				&limits, sizeof(limits)) != FALSE
				&& ::AssignProcessToJobObject(m_job.Get(), m_process.Get()) != FALSE;
		}
		std::string response;
		return Exchange(R"({"type":"hello","protocol":1})", response)
			&& response.find(R"("type":"hello")") != std::string::npos;
	}

	[[nodiscard]] bool Decorate(std::wstring_view line, std::uint32_t tabSize,
		std::vector<IndentDecoration>& target)
	{
		std::string request = R"({"type":"decorate","revision":1,"tabSize":)";
		request += std::to_string(tabSize);
		request += R"(,"lines":[{"line":0,"text":)";
		workbench::commands::json::AppendQuoted(request, line);
		request += R"(}]})";
		std::string response;
		if (!Exchange(request, response)) return false;
		return ParseDecorations(response, target);
	}

	void Close() noexcept
	{
		if (m_process.Valid() && ::WaitForSingleObject(m_process.Get(), 0) != WAIT_OBJECT_0) {
			std::string ignored;
			(void)Exchange(R"({"type":"shutdown"})", ignored);
			if (::WaitForSingleObject(m_process.Get(), 200) != WAIT_OBJECT_0) {
				if (m_assignedToJob) (void)::TerminateJobObject(m_job.Get(), ERROR_CANCELLED);
				else (void)::TerminateProcess(m_process.Get(), ERROR_CANCELLED);
				(void)::WaitForSingleObject(m_process.Get(), 1000);
			}
		}
		m_input.Reset();
		m_output.Reset();
		m_thread.Reset();
		m_process.Reset();
		m_job.Reset();
		m_assignedToJob = false;
	}

private:
	[[nodiscard]] bool Exchange(std::string_view request, std::string& response)
	{
		if (!m_input.Valid() || !m_output.Valid() || !m_process.Valid()
			|| request.empty() || request.size() > kMaximumFrameBytes) return false;
		const auto length = static_cast<std::uint32_t>(request.size());
		if (!WriteAll(m_input.Get(), &length, sizeof(length))
			|| !WriteAll(m_input.Get(), request.data(), request.size())) return false;
		std::uint32_t responseLength = 0;
		if (!ReadExactTimed(m_output.Get(), m_process.Get(), &responseLength, sizeof(responseLength))
			|| responseLength == 0 || responseLength > kMaximumFrameBytes) return false;
		response.resize(responseLength);
		return ReadExactTimed(m_output.Get(), m_process.Get(), response.data(), response.size());
	}

	[[nodiscard]] static bool ParseDecorations(std::string_view response,
		std::vector<IndentDecoration>& target)
	{
		const auto parsed = platform::serialization::CJsoncDocument::Parse(response);
		const auto* root = parsed.Succeeded() ? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
		if (root == nullptr) return false;
		const auto type = root->find(L"type");
		const auto slots = root->find(L"slots");
		const auto* typeText = type == root->end() ? nullptr : std::get_if<std::wstring>(&type->second.Value());
		const auto* array = slots == root->end() ? nullptr : std::get_if<JsoncValue::Array>(&slots->second.Value());
		if (typeText == nullptr || *typeText != L"decorations" || array == nullptr || array->size() > 10000) return false;
		target.clear();
		target.reserve(array->size());
		for (const auto& value : *array) {
			const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
			if (object == nullptr) return false;
			const auto integer = [object](std::wstring_view name) -> const std::int64_t* {
				const auto found = object->find(name);
				return found == object->end() ? nullptr : std::get_if<std::int64_t>(&found->second.Value());
			};
			const auto* line = integer(L"line");
			const auto* start = integer(L"visualStart");
			const auto* length = integer(L"visualLength");
			const auto* depth = integer(L"depth");
			if (line == nullptr || *line != 0 || start == nullptr || length == nullptr || depth == nullptr
				|| *start < 0 || *length <= 0 || *depth < 0
				|| *start > UINT32_MAX || *length > UINT32_MAX || *depth > UINT32_MAX) return false;
			target.push_back({ static_cast<std::uint32_t>(*start),
				static_cast<std::uint32_t>(*length), static_cast<std::uint32_t>(*depth) });
		}
		return true;
	}

	ScopedHandle m_input;
	ScopedHandle m_output;
	ScopedHandle m_process;
	ScopedHandle m_thread;
	ScopedHandle m_job;
	bool m_assignedToJob = false;
};

struct PendingLine final {
	std::string key;
	std::wstring text;
	std::uint32_t tabSize = 4;
	std::uintptr_t repaintWindow = 0;
};

class CWin32SenpRuntimeService final : public ISenpRuntimeService {
public:
	explicit CWin32SenpRuntimeService(ISenpManagementService& management) : m_management(management) {}
	~CWin32SenpRuntimeService() override { Stop(); }

	[[nodiscard]] bool Start() override
	{
		std::lock_guard lock(m_mutex);
		if (m_snapshot.state == ERuntimeState::Ready || m_snapshot.state == ERuntimeState::ReadyWithDiagnostics) return true;
		if (m_snapshot.state == ERuntimeState::Stopped) return false;
		m_stop = false;
		m_worker = std::thread([this] { WorkerMain(); });
		m_snapshot.state = ERuntimeState::Ready;
		++m_snapshot.revision;
		return true;
	}

	void Stop() noexcept override
	{
		try {
			{
				std::lock_guard lock(m_mutex);
				if (m_snapshot.state == ERuntimeState::Stopped) return;
				m_stop = true;
			}
			m_changed.notify_all();
			if (m_worker.joinable()) m_worker.join();
			std::lock_guard lock(m_mutex);
			m_pending.clear();
			m_pendingKeys.clear();
			m_cache.clear();
			m_snapshot.activeHosts = 0;
			m_snapshot.state = ERuntimeState::Stopped;
			++m_snapshot.revision;
		} catch (...) {
		}
	}

	void NotifyExtensionsChanged() noexcept override
	{
		try {
			std::lock_guard lock(m_mutex);
			++m_extensionSetGeneration;
			m_cache.clear();
			m_pending.clear();
			m_pendingKeys.clear();
		} catch (...) {
		}
	}

	[[nodiscard]] std::optional<std::vector<IndentDecoration>> RequestIndentDecorations(
		std::wstring_view line, std::uint32_t tabSize, std::uintptr_t repaintWindow) override
	{
		if (line.size() > 64U * 1024U || tabSize == 0 || tabSize > 32) return std::vector<IndentDecoration>{};
		std::lock_guard lock(m_mutex);
		// Package changes explicitly advance this runtime-owned generation. Paint
		// never takes the management-service lock or copies the extension catalog.
		std::string key = std::to_string(m_extensionSetGeneration);
		key.push_back(':');
		key += std::to_string(tabSize);
		key.push_back(':');
		key += workbench::commands::json::ToUtf8(line);
		if (m_snapshot.state != ERuntimeState::Ready && m_snapshot.state != ERuntimeState::ReadyWithDiagnostics) {
			return std::vector<IndentDecoration>{};
		}
		if (const auto found = m_cache.find(key); found != m_cache.end()) return found->second;
		if (m_pendingKeys.contains(key) || m_pending.size() >= kMaximumPendingLines) return std::nullopt;
		m_pendingKeys.insert(key);
		m_pending.push_back({ key, std::wstring(line), tabSize, repaintWindow });
		m_changed.notify_one();
		return std::nullopt;
	}

	[[nodiscard]] RuntimeSnapshot Snapshot() const override
	{
		std::lock_guard lock(m_mutex);
		return m_snapshot;
	}

private:
	void WorkerMain() noexcept
	{
		std::vector<HostProcess> hosts;
		std::uint64_t managementRevision = 0;
		for (;;) {
			PendingLine work;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock, [this] { return m_stop || !m_pending.empty(); });
				if (m_stop) break;
				work = std::move(m_pending.front());
				m_pending.pop_front();
			}
			const auto management = m_management.Snapshot();
			if (management.revision != managementRevision) {
				hosts.clear();
				std::wstring diagnostic;
				for (const auto& extension : management.extensions) {
					if (!extension.enabled || !extension.contributesIndentDecorations) continue;
					HostProcess host;
					if (host.Open(extension.modulePath)) hosts.push_back(std::move(host));
					else diagnostic = L"An enabled SENP extension host could not be started";
				}
				managementRevision = management.revision;
				std::lock_guard lock(m_mutex);
				m_cache.clear();
				m_snapshot.activeHosts = hosts.size();
				m_snapshot.diagnostic = std::move(diagnostic);
				m_snapshot.state = m_snapshot.diagnostic.empty()
					? ERuntimeState::Ready : ERuntimeState::ReadyWithDiagnostics;
				++m_snapshot.revision;
			}
			std::vector<IndentDecoration> decorations;
			bool succeeded = true;
			for (auto& host : hosts) {
				std::vector<IndentDecoration> contributed;
				if (!host.Decorate(work.text, work.tabSize, contributed)) {
					succeeded = false;
					break;
				}
				decorations.insert(decorations.end(), contributed.begin(), contributed.end());
			}
			std::sort(decorations.begin(), decorations.end(), [](const auto& left, const auto& right) {
				return std::tie(left.visualStart, left.depth, left.visualLength)
					< std::tie(right.visualStart, right.depth, right.visualLength);
			});
			{
				std::lock_guard lock(m_mutex);
				m_pendingKeys.erase(work.key);
				if (succeeded) {
					if (m_cache.size() >= kMaximumCachedLines) m_cache.clear();
					m_cache.emplace(std::move(work.key), std::move(decorations));
				} else {
					m_snapshot.state = ERuntimeState::ReadyWithDiagnostics;
					m_snapshot.diagnostic = L"A SENP extension failed while computing editor decorations";
					++m_snapshot.revision;
				}
			}
			const HWND repaint = reinterpret_cast<HWND>(work.repaintWindow);
			if (repaint != nullptr && ::IsWindow(repaint)) ::InvalidateRect(repaint, nullptr, FALSE);
		}
	}

	ISenpManagementService& m_management;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	bool m_stop = false;
	std::deque<PendingLine> m_pending;
	std::set<std::string> m_pendingKeys;
	std::map<std::string, std::vector<IndentDecoration>> m_cache;
	std::uint64_t m_extensionSetGeneration = 0;
	RuntimeSnapshot m_snapshot;
};

} // namespace

std::unique_ptr<ISenpRuntimeService> CreateWin32SenpRuntimeService(
	ISenpManagementService& management)
{
	return std::make_unique<CWin32SenpRuntimeService>(management);
}

} // namespace senp
