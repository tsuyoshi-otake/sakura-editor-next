/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpEffectRuntime.h"
#include "senp/SenpChildJob.h"
#include <sakura/security/CurrentUserSecurityAttributes.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

namespace senp {
namespace {
using Time = CSenpRuntimeSession::Time;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class Handle final {
public:
	Handle() = default;
	explicit Handle(HANDLE value) : m_value(value) {}
	~Handle() { Reset(); }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	HANDLE Get() const { return m_value; }
	bool Valid() const { return m_value && m_value != INVALID_HANDLE_VALUE; }
	void Reset(HANDLE value = nullptr) {
		if (Valid()) ::CloseHandle(m_value);
		m_value = value;
	}
private:
	HANDLE m_value{};
};

std::wstring Quote(std::wstring_view value)
{
	std::wstring result(1, L'"');
	std::size_t slashes{};
	for (const auto ch : value) {
		if (ch == L'\\') { ++slashes; continue; }
		result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		result.push_back(ch);
		slashes = 0;
	}
	result.append(slashes * 2, L'\\');
	result.push_back(L'"');
	return result;
}

//! Bound for a frame no pending invocation waits on. An Ack is write-only and
//! never enters the guest, and a Deactivate is already clamped by the shorter
//! stop deadline, so this constant only has to cover kernel pipe I/O.
constexpr std::chrono::milliseconds kPassiveExchangeDeadline{ 1000 };

DWORD Remaining(Time deadline, const DWORD cap)
{
	const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
	return static_cast<DWORD>(std::clamp<std::int64_t>(milliseconds, 0, cap));
}

bool PipePair(std::wstring_view suffix, bool parentWrites, Handle& parent, Handle& child)
{
	GUID guid{};
	std::array<wchar_t, 40> guidText{};
	if (FAILED(::CoCreateGuid(&guid)) || ::StringFromGUID2(guid, guidText.data(), static_cast<int>(guidText.size())) == 0) return false;
	const auto name = L"\\\\.\\pipe\\sakura-senp-v2-" + std::wstring(guidText.data()) + std::wstring(suffix);
	platform::security::CurrentUserSecurityAttributes protectedPipe;
	std::wstring diagnostic;
	if (!protectedPipe.Initialize(diagnostic)) return false;
	parent.Reset(::CreateNamedPipeW(name.c_str(),
		(parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 65536, 65536, 0, protectedPipe.Attributes()));
	if (!parent.Valid()) return false;
	SECURITY_ATTRIBUTES childSide{ sizeof(childSide), nullptr, FALSE };
	child.Reset(::CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE,
		0, &childSide, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!child.Valid()) return false;
	Handle connected(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!connected.Valid()) return false;
	OVERLAPPED connection{};
	connection.hEvent = connected.Get();
	// The local client was opened synchronously, so it has already connected.
	if (::ConnectNamedPipe(parent.Get(), &connection)) return true;
	const auto error = ::GetLastError();
	if (error == ERROR_PIPE_CONNECTED) return true;
	if (error == ERROR_IO_PENDING) {
		(void)::CancelIoEx(parent.Get(), &connection);
		DWORD ignored{};
		(void)::GetOverlappedResult(parent.Get(), &connection, &ignored, TRUE);
	}
	return false;
}

//! Used exclusively by the worker. Every I/O cancellation is drained before
//! stack-owned OVERLAPPED or buffers leave scope; all handles have one owner.
class HostConnection final {
public:
	~HostConnection() { (void)Close(); }
	bool Open(const EffectRuntimeLaunch& launch)
	{
		if (launch.moduleSha256.size() != 64 || !std::ranges::all_of(launch.moduleSha256, [](wchar_t ch) {
			return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
		})) return false;
		if (launch.hostExecutable.find(L'\0') != std::wstring::npos || launch.modulePath.find(L'\0') != std::wstring::npos
			|| !std::filesystem::path(launch.hostExecutable).is_absolute() || !std::filesystem::path(launch.modulePath).is_absolute()
			|| !std::filesystem::is_regular_file(launch.hostExecutable) || !std::filesystem::is_regular_file(launch.modulePath)) return false;
		// Child-side handles are created non-inheritable; the inherit flag is
		// set only for the minimal window around CreateProcessW below.
		Handle childInput, childOutput;
		if (!PipePair(L"-input", true, m_input, childInput) || !PipePair(L"-output", false, m_output, childOutput)) return false;
		SECURITY_ATTRIBUTES childSide{ sizeof(childSide), nullptr, FALSE };
		Handle error(::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&childSide, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!error.Valid()) return false;
		m_job.Reset(::CreateJobObjectW(nullptr, nullptr));
		if (!m_job.Valid() || !ConfigureChildJob(m_job.Get(), kExtensionHostJobLimits)) return false;
		m_waitSlice = static_cast<DWORD>(std::clamp<std::int64_t>(launch.waitSlice.count(), 1, 60000));
		auto command = Quote(launch.hostExecutable) + L" --component " + Quote(launch.modulePath)
			+ L" --component-sha256 " + Quote(launch.moduleSha256) + L" --protocol 2";
		const auto process = detail::LaunchSenpChildProcess(launch.hostExecutable, command,
			{ childInput.Get(), childOutput.Get(), error.Get() },
			CREATE_NO_WINDOW, m_job.Get());
		childInput.Reset();
		childOutput.Reset();
		error.Reset();
		if (!process) return false;
		m_process.Reset(process->hProcess);
		Handle initialThread(process->hThread);
		m_pid = process->dwProcessId;
		return true;
	}

	bool Exchange(const RuntimeFrame& frame, HANDLE cancel, Time deadline, std::string& response)
	{
		if (frame.bytes.empty() || frame.bytes.size() > effect::kMaximumFrameBytes) return false;
		auto length = static_cast<std::uint32_t>(frame.bytes.size());
		if (!Transfer(m_input.Get(), true, &length, sizeof(length), cancel, deadline)
			|| !Transfer(m_input.Get(), true, const_cast<char*>(frame.bytes.data()), frame.bytes.size(), cancel, deadline)) return false;
		if (!frame.expectsReply) return true;
		length = 0;
		if (!Transfer(m_output.Get(), false, &length, sizeof(length), cancel, deadline)
			|| length == 0 || length > effect::kMaximumFrameBytes) return false;
		response.resize(length);
		return Transfer(m_output.Get(), false, response.data(), response.size(), cancel, deadline);
	}

	HANDLE Process() const { return m_process.Get(); }
	DWORD Pid() const { return m_pid; }
	bool Close() noexcept
	{
		bool exited = true;
		if (m_process.Valid() && ::WaitForSingleObject(m_process.Get(), 0) != WAIT_OBJECT_0) {
			(void)::TerminateJobObject(m_job.Get(), ERROR_CANCELLED);
			exited = ::WaitForSingleObject(m_process.Get(), 2000) == WAIT_OBJECT_0;
		}
		m_input.Reset();
		m_output.Reset();
		m_job.Reset();
		m_process.Reset();
		return exited;
	}

private:
	bool Transfer(HANDLE pipe, bool writing, void* buffer, std::size_t length, HANDLE cancel, Time deadline)
	{
		Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!event.Valid()) return false;
		auto cursor = static_cast<std::byte*>(buffer);
		while (length > 0) {
			if (Clock::now() >= deadline || ::WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return false;
			::ResetEvent(event.Get());
			OVERLAPPED overlapped{};
			overlapped.hEvent = event.Get();
			const auto chunk = static_cast<DWORD>((std::min)(length, std::size_t{ 65536 }));
			const auto issued = writing ? ::WriteFile(pipe, cursor, chunk, nullptr, &overlapped)
				: ::ReadFile(pipe, cursor, chunk, nullptr, &overlapped);
			if (!issued && ::GetLastError() != ERROR_IO_PENDING) return false;
			DWORD transferred{};
			if (!issued) {
				const std::array<HANDLE, 3> waits{ event.Get(), cancel, m_process.Get() };
				// Wait in bounded slices - never INFINITE - until the absolute
				// deadline, so a cold start longer than one slice is not read
				// as a hung host. Only the deadline, cancel or the peer's own
				// exit ends the exchange.
				for (;;) {
					const auto observed = ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()),
						waits.data(), FALSE, Remaining(deadline, m_waitSlice));
					if (observed == WAIT_OBJECT_0) break;
					if (observed == WAIT_TIMEOUT && Clock::now() < deadline) continue;
					(void)::CancelIoEx(pipe, &overlapped);
					(void)::TerminateJobObject(m_job.Get(), ERROR_CANCELLED);
					// CancelIoEx requests cancellation; kernel completion owns the
					// final buffer lifetime. The peer is also terminated to close
					// this local byte pipe. Never return with an outstanding I/O.
					(void)::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
					return false;
				}
			}
			if (!::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) || transferred == 0) return false;
			cursor += transferred;
			length -= transferred;
		}
		return true;
	}
	Handle m_input, m_output, m_process, m_job;
	DWORD m_pid{};
	DWORD m_waitSlice{ 10000 };
};
} // namespace

struct CSenpEffectRuntime::Impl final {
	explicit Impl(EffectRuntimeLaunch value) : launch(std::move(value)), session(launch.generation),
		changed(::CreateEventW(nullptr, TRUE, FALSE, nullptr)), cancelIo(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

	void Worker()
	{
		const auto waitSlice = static_cast<DWORD>(std::clamp<std::int64_t>(launch.waitSlice.count(), 1, 60000));
		HostConnection process;
		try {
			if (process.Open(launch)) {
				{
					std::lock_guard lock(mutex);
					pid = process.Pid();
					processExitConfirmed = false;
				}
				for (;;) {
					std::optional<RuntimeFrame> frame;
					Time deadline;
					DWORD idleWait = INFINITE;
					{
						std::lock_guard lock(mutex);
						const auto now = Clock::now();
						session.Expire(now);
						if (session.Phase() == RuntimePhase::Stopped) break;
						if (session.Phase() == RuntimePhase::Stopping && !stopDeadline) stopDeadline = now + 250ms;
						// An exchange is bound by the work it serves: the earliest
						// pending invocation deadline (an ordinary request's <= 10 s,
						// the activation's cold-start budget while starting). With
						// nothing pending only passive frames travel - acks and the
						// Deactivate, which never make the guest run (Deactivate is
						// also under the shorter stop deadline) - so they get the
						// small named bound that covers kernel pipe I/O alone. A
						// hung host still fails closed at its invocation deadline
						// and becomes an owner terminal (HostUnavailable); this
						// worker is off the UI thread, so the longer bound cannot
						// stall paint or input.
						deadline = now + kPassiveExchangeDeadline;
						if (const auto expires = session.NextDeadline()) {
							deadline = *expires;
							idleWait = Remaining(*expires, waitSlice);
						}
						if (stopDeadline) deadline = (std::min)(deadline, *stopDeadline);
						::ResetEvent(changed.Get());
						::ResetEvent(cancelIo.Get());
						frame = session.TakeOutgoing();
					}
					if (frame) {
						std::string response;
						const auto exchanged = process.Exchange(*frame, cancelIo.Get(), deadline, response);
						std::lock_guard lock(mutex);
						session.Expire(Clock::now());
						if (!exchanged) { session.TransportFailed(); break; }
						if (frame->expectsReply) session.Receive(response);
					} else {
						const std::array<HANDLE, 2> waits{ changed.Get(), process.Process() };
						const auto result = ::WaitForMultipleObjects(2, waits.data(), FALSE, idleWait);
						if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) break;
					}
				}
			}
		} catch (...) {
			// This single owner also finalizes launch, allocation and I/O failures.
		}
		const auto exited = process.Close();
		std::lock_guard lock(mutex);
		session.TransportFailed();
		processExitConfirmed = exited;
		workerExited = true;
	}

private:
	// State owned exclusively by CSenpEffectRuntime and this Impl, which
	// together own the worker's lifecycle; the outer class is the sole
	// friend rather than a public data bag any future caller could reach.
	friend class ::senp::CSenpEffectRuntime;

	EffectRuntimeLaunch launch;
	// Not `mutable`: every lock site reaches this mutex through
	// m_impl->mutex on a std::unique_ptr<Impl>, whose operator-> yields a
	// non-const Impl* even when the owning CSenpEffectRuntime method (e.g.
	// Snapshot() const) is itself const. unique_ptr does not propagate
	// const to its pointee, so the qualifier was never load-bearing here.
	std::mutex mutex;
	std::mutex joinMutex;
	CSenpRuntimeSession session;
	Handle changed, cancelIo;
	std::thread worker;
	DWORD pid{};
	bool workerExited{ true };
	bool processExitConfirmed{ true };
	std::optional<Time> stopDeadline;
};

CSenpEffectRuntime::CSenpEffectRuntime(EffectRuntimeLaunch launch) : m_impl(std::make_unique<Impl>(std::move(launch))) {}
CSenpEffectRuntime::~CSenpEffectRuntime() { Stop(effect::StopReason::Shutdown); Join(); }

InvocationAdmission CSenpEffectRuntime::Start()
{
	auto& s = *m_impl;
	std::lock_guard lifecycle(s.joinMutex);
	std::lock_guard lock(s.mutex);
	if (!s.changed.Valid() || !s.cancelIo.Valid()) return { AdmissionStatus::Unavailable };
	const auto now = Clock::now();
	// The activation may spend the launch's cold-start budget (default
	// kMaximumColdStart): it covers the whole handshake, not one invocation.
	auto result = s.session.Begin(s.launch.extensionId, s.launch.context, now + s.launch.coldStartBudget, now);
	if (result.status != AdmissionStatus::Accepted) return result;
	try {
		s.workerExited = false;
		s.worker = std::thread([&s] { s.Worker(); });
	} catch (...) {
		s.session.TransportFailed();
		s.workerExited = true;
	}
	return result;
}

InvocationAdmission CSenpEffectRuntime::Submit(effect::OperationContext context, effect::Event event, Time deadline)
{
	std::lock_guard lock(m_impl->mutex);
	auto result = m_impl->session.Submit(std::move(context), std::move(event), deadline, Clock::now());
	if (result.status == AdmissionStatus::Accepted) ::SetEvent(m_impl->changed.Get());
	return result;
}

bool CSenpEffectRuntime::Cancel(std::wstring_view operationId)
{
	std::lock_guard lock(m_impl->mutex);
	const auto result = m_impl->session.Cancel(operationId);
	::SetEvent(m_impl->changed.Get());
	return result;
}

void CSenpEffectRuntime::Stop(effect::StopReason reason)
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->session.Stop(reason);
	if (!m_impl->stopDeadline) m_impl->stopDeadline = Clock::now() + 250ms;
	::SetEvent(m_impl->changed.Get());
	::SetEvent(m_impl->cancelIo.Get());
}

void CSenpEffectRuntime::Join()
{
	Stop(effect::StopReason::Shutdown);
	std::lock_guard lifecycle(m_impl->joinMutex);
	if (m_impl->worker.joinable()) m_impl->worker.join();
}

std::optional<InvocationResult> CSenpEffectRuntime::TakeCompleted()
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->session.Expire(Clock::now());
	return m_impl->session.TakeCompleted();
}

EffectRuntimeSnapshot CSenpEffectRuntime::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	return { m_impl->session.Phase(), m_impl->session.PendingCount(), m_impl->session.CompletionCount(),
		m_impl->pid, m_impl->workerExited, m_impl->processExitConfirmed };
}

} // namespace senp
