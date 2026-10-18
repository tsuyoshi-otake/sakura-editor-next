/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "senp/SenpRuntimeService.h"

#include "senp/SenpChildJob.h"
#include "senp/SenpManagementService.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace senp {
namespace {

using platform::serialization::JsoncValue;

constexpr DWORD kProtocolTimeoutMilliseconds = 1000;
constexpr std::size_t kMaximumFrameBytes = 4U * 1024U * 1024U;
//! Longest admitted line, in UTF-16 code units.
constexpr std::size_t kMaximumLineUnits = 64U * 1024U;
//! Most slots one `decorations` reply may carry. The bundled indent
//! extension caps its own output per request at the same count.
constexpr std::size_t kMaximumReplySlots = 10000;
//! Byte bound on the pending queue. An entry is charged its key twice (the
//! queue and its dedupe set each hold a copy) plus its own size.
constexpr std::size_t kMaximumPendingBytes = 4U * 1024U * 1024U;
//! Byte bound on the decoration cache. An entry is charged its key twice
//! (the map and its LRU list each hold a copy), its decoration vector's
//! capacity, and its own size.
constexpr std::size_t kMaximumCacheBytes = 16U * 1024U * 1024U;
//! One `decorate` frame carries at most kMaximumBatchLines lines. Past its
//! first line it carries at most kMaximumBatchUnits UTF-16 units, so an
//! extension that emits at most one slot per unit (the indent extension
//! emits one per indent level) never exceeds kMaximumReplySlots because
//! of how lines were grouped; a longer first line travels alone, exactly
//! as it would unbatched.
constexpr std::size_t kMaximumBatchLines = 64;
constexpr std::size_t kMaximumBatchUnits = kMaximumReplySlots;
//! The longest key: "<generation>:<tabSize>:" plus the line's UTF-8, at
//! most 3 bytes per UTF-16 unit.
constexpr std::size_t kMaximumKeyBytes = 20U + 1U + 2U + 1U + kMaximumLineUnits * 3U;

//! Worst-case `decorate` frame for `units` UTF-16 units over `lines` lines:
//! AppendQuoted's widest expansion is a control character escaped to six
//! bytes (\u00XX), each line adds its `{"line":N,"text":""},` wrapper, and
//! the frame adds its header.
constexpr std::size_t WorstCaseDecorateFrameBytes(std::size_t units, std::size_t lines) noexcept
{
	return units * 6U + lines * 32U + 128U;
}
static_assert(WorstCaseDecorateFrameBytes(kMaximumLineUnits, 1) <= kMaximumFrameBytes,
	"a lone longest line always fits one decorate frame");
static_assert(WorstCaseDecorateFrameBytes(kMaximumBatchUnits, kMaximumBatchLines) <= kMaximumFrameBytes,
	"a full batch always fits one decorate frame");

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

// The v1 transport is a pair of anonymous pipes from CreatePipe, which do not
// support overlapped I/O, so the event-driven wait used by the v2 Transfer is
// not available without changing the transport. This bounded PeekNamedPipe
// poll stands instead: it is capped by kProtocolTimeoutMilliseconds, runs only
// on the v1 worker thread, and checks host-process exit on every iteration.
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

class HostProcess final : public detail::ISenpExtensionHost {
public:
	explicit HostProcess(std::wstring hostExecutable) : m_hostExecutable(std::move(hostExecutable)) {}
	~HostProcess() { Close(); }
	HostProcess(const HostProcess&) = delete;
	HostProcess& operator=(const HostProcess&) = delete;

	[[nodiscard]] bool Open(std::wstring_view modulePath, std::wstring_view moduleSha256) override
	{
		Close();
		const bool validDigest = moduleSha256.size() == 64
			&& std::ranges::all_of(moduleSha256, [](wchar_t ch) {
				return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
			});
		const auto& executable = !m_hostExecutable.empty() ? m_hostExecutable : HostExecutable();
		if (!validDigest || executable.empty() || !std::filesystem::is_regular_file(executable)
			|| !std::filesystem::is_regular_file(std::filesystem::path(modulePath))) return false;
		// Child-side handles are created non-inheritable; the inherit flag is
		// set only for the minimal window around CreateProcessW below.
		SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, FALSE };
		ScopedHandle childInput;
		ScopedHandle childOutput;
		if (!::CreatePipe(childInput.Put(), m_input.Put(), &security, 0)
			|| !::CreatePipe(m_output.Put(), childOutput.Put(), &security, 0)) return false;
		ScopedHandle nullError(::CreateFileW(L"NUL", GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!nullError.Valid()) return false;

		m_job.Reset(::CreateJobObjectW(nullptr, nullptr));
		if (!m_job.Valid() || !ConfigureChildJob(m_job.Get(), kExtensionHostJobLimits)) return false;

		SIZE_T attributeBytes = 0;
		(void)::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
		if (attributeBytes == 0) return false;
		std::vector<std::uint8_t> attributeStorage(attributeBytes);
		auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
		if (!::InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) return false;
		struct AttributeListGuard final {
			LPPROC_THREAD_ATTRIBUTE_LIST value{};
			~AttributeListGuard() { if (value != nullptr) ::DeleteProcThreadAttributeList(value); }
		} attributeGuard{ attributes };
		const std::array<HANDLE, 3> inheritedHandles{ childInput.Get(), childOutput.Get(), nullError.Get() };
		if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			const_cast<HANDLE*>(inheritedHandles.data()), sizeof(inheritedHandles), nullptr, nullptr)) return false;
		HANDLE jobHandle = m_job.Get();
		if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
			&jobHandle, sizeof(jobHandle), nullptr, nullptr)) return false;

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = childInput.Get();
		startup.StartupInfo.hStdOutput = childOutput.Get();
		startup.StartupInfo.hStdError = nullError.Get();
		startup.lpAttributeList = attributes;
		std::wstring command = QuoteArgument(executable) + L" --component " + QuoteArgument(modulePath)
			+ L" --component-sha256 " + QuoteArgument(moduleSha256);
		auto launchConfig = detail::BuildSenpChildLaunchConfig(executable);
		const std::array<HANDLE, 3> childSide{ childInput.Get(), childOutput.Get(), nullError.Get() };
		if (!detail::SetChildHandlesInheritable(childSide)) return false;
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
			TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | launchConfig.environmentFlags,
			launchConfig.environment.data(), launchConfig.workingDirectory.c_str(),
			&startup.StartupInfo, &process);
		childInput.Reset();
		childOutput.Reset();
		nullError.Reset();
		if (!created) return false;
		m_process.Reset(process.hProcess);
		m_thread.Reset(process.hThread);
		m_assignedToJob = true;
		std::string response;
		return Exchange(R"({"type":"hello","protocol":1})", response)
			&& detail::AcceptExtensionHello(response);
	}

	[[nodiscard]] bool Decorate(std::uint32_t tabSize,
		const std::vector<detail::DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, IndentDecoration>>& slots) override
	{
		std::string request = R"({"type":"decorate","revision":1,"tabSize":)";
		request += std::to_string(tabSize);
		request += R"(,"lines":[)";
		for (std::size_t index = 0; index < lines.size(); ++index) {
			if (index != 0) request.push_back(',');
			request += R"({"line":)";
			request += std::to_string(lines[index].line);
			request += R"(,"text":)";
			workbench::commands::json::AppendQuoted(request, lines[index].text);
			request.push_back('}');
		}
		request += R"(]})";
		std::string response;
		if (!Exchange(request, response)) return false;
		return detail::ParseDecorationSlots(response,
			static_cast<std::uint32_t>(lines.size()), slots);
	}

	void Close() noexcept override
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

	const std::wstring m_hostExecutable;
	ScopedHandle m_input;
	ScopedHandle m_output;
	ScopedHandle m_process;
	ScopedHandle m_thread;
	ScopedHandle m_job;
	bool m_assignedToJob = false;
};

struct PendingLine final {
	//! "<generation>:<tabSize>:<UTF-8 line>"; the only copy of the line text,
	//! re-decoded from `textOffset` when the worker builds the frame.
	std::string key;
	std::size_t textOffset = 0;
	//! The line's UTF-16 length, counted against kMaximumBatchUnits.
	std::size_t units = 0;
	std::uint32_t tabSize = 4;
	std::uintptr_t repaintWindow = 0;
	//! Accounted bytes against kMaximumPendingBytes.
	std::size_t bytes = 0;
};
// The longest admitted line always fits an empty pending queue, so
// admission never starves a line that is alone in the queue.
static_assert(2U * kMaximumKeyBytes + sizeof(PendingLine) <= kMaximumPendingBytes,
	"the longest admitted line always fits the pending byte cap");

//! The `(id, modulePath, moduleSha256)` triple that decides whether a host
//! survives a management revision change.
struct ExtensionIdentity final {
	std::wstring id;
	std::wstring modulePath;
	std::wstring moduleSha256;
	[[nodiscard]] auto operator<=>(const ExtensionIdentity&) const = default;
};

//! Retired identities and the diagnostic each one surfaces.
using RetiredHosts = std::map<ExtensionIdentity, std::wstring>;

struct LiveHost final {
	ExtensionIdentity identity;
	std::unique_ptr<detail::ISenpExtensionHost> host;
};

class CWin32SenpRuntimeService final : public ISenpRuntimeService {
public:
	explicit CWin32SenpRuntimeService(ISenpManagementService& management,
		detail::SenpRuntimeHooks hooks)
		: m_management(management), m_hooks(std::move(hooks)) {}
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
			m_pendingBytes = 0;
			m_pendingKeys.clear();
			InvalidateCacheLocked();
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
			InvalidateCacheLocked();
			m_pending.clear();
			m_pendingBytes = 0;
			m_pendingKeys.clear();
		} catch (...) {
		}
	}

	[[nodiscard]] std::optional<std::vector<IndentDecoration>> RequestIndentDecorations(
		std::wstring_view line, std::uint32_t tabSize, std::uintptr_t repaintWindow) override
	{
		if (line.size() > kMaximumLineUnits || tabSize == 0 || tabSize > 32) {
			return std::vector<IndentDecoration>{};
		}
		// The key is the only stored copy of the text: a line whose UTF-8
		// encoding cannot round-trip (an unpaired surrogate, or any other
		// lossy conversion) is never queued, so the worker never decorates
		// text that differs from what was painted.
		const auto utf8 = workbench::commands::json::ToUtf8(line);
		const auto roundTrip = workbench::commands::json::ToWideStrict(utf8);
		if (!roundTrip || *roundTrip != line) return std::vector<IndentDecoration>{};
		std::lock_guard lock(m_mutex);
		// Package changes explicitly advance this runtime-owned generation. Paint
		// never takes the management-service lock or copies the extension catalog.
		std::string key = std::to_string(m_extensionSetGeneration);
		key.push_back(':');
		key += std::to_string(tabSize);
		key.push_back(':');
		const std::size_t textOffset = key.size();
		key += utf8;
		if (m_snapshot.state != ERuntimeState::Ready && m_snapshot.state != ERuntimeState::ReadyWithDiagnostics) {
			return std::vector<IndentDecoration>{};
		}
		if (const auto found = m_cache.find(key); found != m_cache.end()) {
			m_cacheOrder.splice(m_cacheOrder.begin(), m_cacheOrder, found->second.order);
			return found->second.decorations;
		}
		if (m_pendingKeys.contains(key)) return std::nullopt;
		const std::size_t bytes = 2U * key.size() + sizeof(PendingLine);
		if (m_pendingBytes + bytes > kMaximumPendingBytes) return std::nullopt;
		m_pendingKeys.insert(key);
		m_pendingBytes += bytes;
		m_pending.push_back({ std::move(key), textOffset, line.size(), tabSize, repaintWindow, bytes });
		m_changed.notify_one();
		return std::nullopt;
	}

	[[nodiscard]] RuntimeSnapshot Snapshot() const override
	{
		std::lock_guard lock(m_mutex);
		return m_snapshot;
	}

private:
	void Repaint(std::uintptr_t repaintWindow) noexcept
	{
		if (repaintWindow == 0) return;
		if (m_hooks.repaint) {
			m_hooks.repaint(repaintWindow);
			return;
		}
		const HWND repaint = reinterpret_cast<HWND>(repaintWindow);
		if (::IsWindow(repaint)) ::InvalidateRect(repaint, nullptr, FALSE);
	}

	//! Drops every cached result. Called only when the effective host set or
	//! the extension-set generation changed - never as an overflow policy,
	//! where oldest-first eviction is the contract.
	void InvalidateCacheLocked()
	{
		m_cache.clear();
		m_cacheOrder.clear();
		m_cacheBytes = 0;
	}

	void StoreResultLocked(std::string key, std::vector<IndentDecoration> decorations)
	{
		const std::size_t bytes = 2U * key.size()
			+ decorations.capacity() * sizeof(IndentDecoration) + sizeof(CacheEntry);
		m_cacheOrder.push_front(key);
		m_cache.emplace(std::move(key), CacheEntry{ m_cacheOrder.begin(), std::move(decorations), bytes });
		m_cacheBytes += bytes;
		// Least recently used first. The entry just stored always stays, even
		// when it alone exceeds the cap (many hosts, each contributing up to
		// kMaximumReplySlots), so its repaint finds a value instead of
		// re-queueing the same work.
		while (m_cacheBytes > kMaximumCacheBytes && m_cacheOrder.size() > 1) {
			const auto oldest = m_cache.find(m_cacheOrder.back());
			m_cacheBytes -= oldest->second.bytes;
			m_cache.erase(oldest);
			m_cacheOrder.pop_back();
		}
	}

	void PublishHostSetLocked(std::size_t activeHosts, std::wstring diagnostic)
	{
		m_snapshot.activeHosts = activeHosts;
		m_snapshot.diagnostic = std::move(diagnostic);
		m_snapshot.state = m_snapshot.diagnostic.empty()
			? ERuntimeState::Ready : ERuntimeState::ReadyWithDiagnostics;
		++m_snapshot.revision;
	}

	//! The snapshot names the first retired identity's reason; no retired
	//! identity means every desired host is live.
	[[nodiscard]] static std::wstring RetirementDiagnostic(const RetiredHosts& retired)
	{
		return retired.empty() ? std::wstring{} : retired.begin()->second;
	}

	//! R0's explicit failure terminal: the host is closed, its identity is
	//! retired (never reopened while the same identity is desired), and the
	//! cache is invalidated because the effective host set changed.
	[[nodiscard]] std::vector<LiveHost>::iterator RetireHost(
		std::vector<LiveHost>& hosts, std::vector<LiveHost>::iterator failed,
		RetiredHosts& retired)
	{
		retired.emplace(failed->identity,
			L"A SENP extension failed while computing editor decorations: " + failed->identity.id);
		failed->host->Close();
		const auto next = hosts.erase(failed);
		std::lock_guard lock(m_mutex);
		InvalidateCacheLocked();
		PublishHostSetLocked(hosts.size(), RetirementDiagnostic(retired));
		return next;
	}

	//! R3's per-host restart: diff the desired identities against the live
	//! set, close only removed/changed hosts, open only added/changed ones,
	//! and invalidate the cache only when the effective set changed.
	void ReconcileHosts(const ManagementSnapshot& management, std::vector<LiveHost>& hosts,
		RetiredHosts& retired,
		const std::function<std::unique_ptr<detail::ISenpExtensionHost>()>& factory)
	{
		std::set<ExtensionIdentity> desired;
		for (const auto& extension : management.extensions) {
			if (!extension.enabled || !extension.contributesIndentDecorations) continue;
			desired.insert({ extension.id, extension.modulePath, extension.moduleSha256 });
		}
		bool changed = false;
		for (auto it = hosts.begin(); it != hosts.end();) {
			if (desired.contains(it->identity)) ++it;
			else {
				it->host->Close();
				it = hosts.erase(it);
				changed = true;
			}
		}
		// A retired identity stays closed while it remains desired; once the
		// package set drops it, its retirement record and diagnostic go too.
		std::erase_if(retired, [&](const auto& entry) { return !desired.contains(entry.first); });
		for (const auto& identity : desired) {
			if (retired.contains(identity)
				|| std::ranges::find(hosts, identity, &LiveHost::identity) != hosts.end()) continue;
			auto host = factory();
			if (host->Open(identity.modulePath, identity.moduleSha256)) {
				hosts.push_back({ identity, std::move(host) });
			} else {
				retired.emplace(identity,
					L"An enabled SENP extension host could not be started: " + identity.id);
			}
			changed = true;
		}
		auto diagnostic = RetirementDiagnostic(retired);
		std::lock_guard lock(m_mutex);
		if (changed) InvalidateCacheLocked();
		// Releasing a retirement changes no live host, but its diagnostic
		// must still clear.
		if (changed || diagnostic != m_snapshot.diagnostic) {
			PublishHostSetLocked(hosts.size(), std::move(diagnostic));
		}
	}

	void ProcessBatch(std::vector<PendingLine>& batch, std::vector<LiveHost>& hosts,
		RetiredHosts& retired)
	{
		// Re-derive each line's text from its key. Admission proved the UTF-8
		// round trip, so the strict decode is engaged by construction.
		const auto tabSize = batch.front().tabSize;
		std::vector<detail::DecorateLine> lines(batch.size());
		for (std::size_t index = 0; index < batch.size(); ++index) {
			lines[index] = { static_cast<std::uint32_t>(index),
				workbench::commands::json::ToWideStrict(
					std::string_view(batch[index].key).substr(batch[index].textOffset)).value() };
		}
		// One frame per host per batch. A failed host retires (R0); the
		// batch's lines still complete from the remaining hosts.
		std::vector<std::vector<IndentDecoration>> results(batch.size());
		for (auto it = hosts.begin(); it != hosts.end();) {
			std::vector<std::pair<std::uint32_t, IndentDecoration>> slots;
			if (!it->host->Decorate(tabSize, lines, slots)) {
				it = RetireHost(hosts, it, retired);
			} else {
				for (const auto& slot : slots) {
					if (slot.first < results.size()) results[slot.first].push_back(slot.second);
				}
				++it;
			}
		}
		// Every line reaches a stored result: merged, sorted, and cached, so
		// a repaint finds a value instead of re-queueing the same work.
		for (std::size_t index = 0; index < batch.size(); ++index) {
			auto& decorations = results[index];
			std::sort(decorations.begin(), decorations.end(), [](const auto& left, const auto& right) {
				return std::tie(left.visualStart, left.depth, left.visualLength)
					< std::tie(right.visualStart, right.depth, right.visualLength);
			});
			{
				std::lock_guard lock(m_mutex);
				m_pendingKeys.erase(batch[index].key);
				StoreResultLocked(std::move(batch[index].key), std::move(decorations));
			}
			Repaint(batch[index].repaintWindow);
		}
	}

	void WorkerMain() noexcept
	{
		const auto factory = [this]() -> std::unique_ptr<detail::ISenpExtensionHost> {
			if (m_hooks.hostFactory) return m_hooks.hostFactory();
			return std::make_unique<HostProcess>(m_hooks.hostExecutable);
		};
		std::vector<LiveHost> hosts;
		RetiredHosts retired;
		std::uint64_t managementRevision = 0;
		for (;;) {
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock, [this] { return m_stop || !m_pending.empty(); });
				if (m_stop) break;
			}
			// Reconcile before draining: requests keep queueing while this
			// polls, and a changed revision takes one Snapshot() (R2).
			if (const auto revision = m_management.Revision(); revision != managementRevision) {
				ReconcileHosts(m_management.Snapshot(), hosts, retired, factory);
				managementRevision = revision;
			}
			std::vector<PendingLine> batch;
			{
				std::lock_guard lock(m_mutex);
				// NotifyExtensionsChanged may have emptied the queue while
				// the reconcile ran.
				if (m_pending.empty()) continue;
				// Front tabSize only, FIFO (R2): other tabSizes wait for the
				// next pass instead of widening this frame.
				// The first line always goes; later lines join only while the
				// frame stays within kMaximumBatchUnits.
				const auto tabSize = m_pending.front().tabSize;
				std::size_t units = 0;
				while (!m_pending.empty() && batch.size() < kMaximumBatchLines
					&& m_pending.front().tabSize == tabSize
					&& (batch.empty() || units + m_pending.front().units <= kMaximumBatchUnits)) {
					units += m_pending.front().units;
					m_pendingBytes -= m_pending.front().bytes;
					batch.push_back(std::move(m_pending.front()));
					m_pending.pop_front();
				}
			}
			ProcessBatch(batch, hosts, retired);
		}
	}

	struct CacheEntry final {
		std::list<std::string>::iterator order;
		std::vector<IndentDecoration> decorations;
		std::size_t bytes = 0;
	};

	ISenpManagementService& m_management;
	const detail::SenpRuntimeHooks m_hooks;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	bool m_stop = false;
	std::deque<PendingLine> m_pending;
	std::size_t m_pendingBytes = 0;
	std::set<std::string> m_pendingKeys;
	//! LRU order, most recently used first; a cache hit moves the entry to
	//! the front. Keyed by the same exact text key as the pending dedupe set.
	std::list<std::string> m_cacheOrder;
	std::unordered_map<std::string, CacheEntry> m_cache;
	std::size_t m_cacheBytes = 0;
	std::uint64_t m_extensionSetGeneration = 0;
	RuntimeSnapshot m_snapshot;
};

} // namespace

namespace detail {

bool AcceptExtensionHello(std::string_view response)
{
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(response);
	const auto* root = parsed.Succeeded() ? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
	if (root == nullptr || root->size() != 3) return false;
	const auto type = root->find(L"type");
	const auto protocol = root->find(L"protocol");
	const auto abi = root->find(L"abi");
	const auto* typeText = type == root->end() ? nullptr : std::get_if<std::wstring>(&type->second.Value());
	const auto* version = protocol == root->end() ? nullptr : std::get_if<std::int64_t>(&protocol->second.Value());
	const auto* abiText = abi == root->end() ? nullptr : std::get_if<std::wstring>(&abi->second.Value());
	return typeText != nullptr && *typeText == L"hello" && version != nullptr && *version == 1
		&& abiText != nullptr && *abiText == L"sakura:senp/extension@1.0.0";
}

bool ParseDecorationSlots(std::string_view response, std::uint32_t lineCount,
	std::vector<std::pair<std::uint32_t, IndentDecoration>>& slots)
{
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(response);
	const auto* root = parsed.Succeeded() ? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
	if (root == nullptr) return false;
	const auto type = root->find(L"type");
	const auto members = root->find(L"slots");
	const auto* typeText = type == root->end() ? nullptr : std::get_if<std::wstring>(&type->second.Value());
	const auto* array = members == root->end() ? nullptr : std::get_if<JsoncValue::Array>(&members->second.Value());
	if (typeText == nullptr || *typeText != L"decorations" || array == nullptr
		|| array->size() > kMaximumReplySlots) return false;
	slots.clear();
	slots.reserve(array->size());
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
		if (line == nullptr || *line < 0 || *line >= lineCount
			|| start == nullptr || length == nullptr || depth == nullptr
			|| *start < 0 || *length <= 0 || *depth < 0
			|| *start > UINT32_MAX || *length > UINT32_MAX || *depth > UINT32_MAX) return false;
		slots.push_back({ static_cast<std::uint32_t>(*line),
			{ static_cast<std::uint32_t>(*start),
				static_cast<std::uint32_t>(*length), static_cast<std::uint32_t>(*depth) } });
	}
	return true;
}

} // namespace detail

std::unique_ptr<ISenpRuntimeService> CreateWin32SenpRuntimeService(
	ISenpManagementService& management, detail::SenpRuntimeHooks hooks)
{
	return std::make_unique<CWin32SenpRuntimeService>(management, std::move(hooks));
}

} // namespace senp
