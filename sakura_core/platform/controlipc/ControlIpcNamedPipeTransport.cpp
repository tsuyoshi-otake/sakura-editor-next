/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include <sakura/controlipc/ControlIpcTransport.h>

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/security/CurrentUserSecurityAttributes.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace platform::controlipc {
namespace {

using ::platform::security::CurrentUserSecurityAttributes;

constexpr auto kMaximumCallerTimeout = std::chrono::seconds(60);
constexpr DWORD kMaximumBusyPipeAttempts = 4;
constexpr DWORD kCancellationDrainMilliseconds = 2000;
thread_local const void* g_currentSessionOwner = nullptr;

class ScopedSessionOwner final {
public:
	explicit ScopedSessionOwner(const void* owner) noexcept : m_previous(std::exchange(g_currentSessionOwner, owner)) {}
	~ScopedSessionOwner() { g_currentSessionOwner = m_previous; }
	ScopedSessionOwner(const ScopedSessionOwner&) = delete;
	ScopedSessionOwner& operator=(const ScopedSessionOwner&) = delete;

private:
	const void* m_previous;
};

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
		if (this != &other) Reset(other.Release());
		return *this;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }
	[[nodiscard]] HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (*this) ::CloseHandle(m_value);
		m_value = value;
	}
private:
	HANDLE m_value = nullptr;
};

std::wstring FormatError(std::wstring_view operation, DWORD error)
{
	return std::wstring(operation) + L" failed (error " + std::to_wstring(error) + L")";
}

ControlIpcTransportResult Failure(EControlIpcTransportDisconnectReason reason, DWORD error,
	std::wstring_view operation)
{
	return { false, reason, error, FormatError(operation, error) };
}

DWORD RemainingMilliseconds(std::chrono::steady_clock::time_point deadline) noexcept
{
	const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
	if (remaining <= 0) return 0;
	return static_cast<DWORD>((std::min)(remaining, static_cast<long long>((std::numeric_limits<DWORD>::max)() - 1)));
}

bool IsValidOptions(const ControlIpcNamedPipeOptions& options, std::wstring& diagnostic) noexcept
{
	if (!IsSafeControlPipeName(options.pipeName)) diagnostic = L"Control pipe name is not an accepted endpoint name";
	else if (options.maximumSessions == 0 || options.maximumSessions > 63) diagnostic = L"maximumSessions must be between 1 and 63";
	else if (options.maximumQueuedBytes == 0 || options.maximumQueuedBytes > kControlIpcMaximumFrameBytes) diagnostic = L"maximumQueuedBytes is out of range";
	else if (options.readBufferBytes == 0 || options.readBufferBytes > options.maximumQueuedBytes) diagnostic = L"readBufferBytes is out of range";
	else if (options.ioTimeout <= std::chrono::milliseconds::zero() || options.ioTimeout > kMaximumCallerTimeout) diagnostic = L"ioTimeout is out of range";
	else return true;
	return false;
}

enum class EWaitIoResult { Completed, Stopped, TimedOut, Failed };

EWaitIoResult WaitForOverlapped(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stopEvent,
	std::chrono::steady_clock::time_point deadline, DWORD& transferred, DWORD& error) noexcept
{
	const HANDLE completion = overlapped.hEvent;
	const HANDLE waits[] = { stopEvent, completion };
	const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, RemainingMilliseconds(deadline));
	const DWORD waitError = wait == WAIT_FAILED ? ::GetLastError() : ERROR_SUCCESS;
	if (wait == WAIT_OBJECT_0 + 1) {
		if (::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) return EWaitIoResult::Completed;
		error = ::GetLastError();
		if (error == ERROR_OPERATION_ABORTED && ::WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return EWaitIoResult::Stopped;
		return EWaitIoResult::Failed;
	}
	::CancelIoEx(pipe, &overlapped);
	// CancelIoEx guarantees a completion packet/event for an accepted cancellable I/O. Bound the normal
	// drain, then use the kernel completion wait only as the memory-safety fallback: OVERLAPPED is stack-owned.
	// A driver that violates this contract can extend this fallback despite the caller deadline.
	if (::WaitForSingleObject(completion, kCancellationDrainMilliseconds) != WAIT_OBJECT_0) {
		::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
	}
	if (wait == WAIT_OBJECT_0) return EWaitIoResult::Stopped;
	if (wait == WAIT_TIMEOUT) return EWaitIoResult::TimedOut;
	error = waitError;
	return EWaitIoResult::Failed;
}

ControlIpcTransportResult ReadSome(HANDLE pipe, HANDLE stopEvent, std::span<std::uint8_t> buffer,
	std::chrono::milliseconds timeout, DWORD& read)
{
	read = 0;
	UniqueHandle completion(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!completion) return Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create read event");
	OVERLAPPED overlapped{};
	overlapped.hEvent = completion.Get();
	if (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped)) {
		if (!::GetOverlappedResult(pipe, &overlapped, &read, TRUE)) return Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Complete pipe read");
	} else if (::GetLastError() == ERROR_IO_PENDING) {
		DWORD error = ERROR_SUCCESS;
		switch (WaitForOverlapped(pipe, overlapped, stopEvent, std::chrono::steady_clock::now() + timeout, read, error)) {
		case EWaitIoResult::Completed: break;
		case EWaitIoResult::Stopped: return { false, EControlIpcTransportDisconnectReason::Stopped, ERROR_OPERATION_ABORTED, L"Pipe read stopped" };
		case EWaitIoResult::TimedOut: return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Pipe read deadline exceeded" };
		case EWaitIoResult::Failed: return Failure(EControlIpcTransportDisconnectReason::IoError, error, L"Complete pipe read");
		}
	} else {
		const DWORD error = ::GetLastError();
		return { false, error == ERROR_BROKEN_PIPE ? EControlIpcTransportDisconnectReason::PeerClosed : EControlIpcTransportDisconnectReason::IoError,
			error, FormatError(L"Read pipe", error) };
	}
	if (read == 0) return { false, EControlIpcTransportDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE, L"Pipe peer closed" };
	return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
}

ControlIpcTransportResult WriteAll(HANDLE pipe, HANDLE stopEvent, std::span<const std::uint8_t> bytes,
	std::chrono::milliseconds timeout)
{
	std::size_t offset = 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (offset < bytes.size()) {
		UniqueHandle completion(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!completion) return Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create write event");
		OVERLAPPED overlapped{};
		overlapped.hEvent = completion.Get();
		DWORD written = 0;
		const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		if (::WriteFile(pipe, bytes.data() + offset, chunk, nullptr, &overlapped)) {
			if (!::GetOverlappedResult(pipe, &overlapped, &written, TRUE)) return Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Complete pipe write");
		} else if (::GetLastError() == ERROR_IO_PENDING) {
			DWORD error = ERROR_SUCCESS;
			switch (WaitForOverlapped(pipe, overlapped, stopEvent, deadline, written, error)) {
			case EWaitIoResult::Completed: break;
			case EWaitIoResult::Stopped: return { false, EControlIpcTransportDisconnectReason::Stopped, ERROR_OPERATION_ABORTED, L"Pipe write stopped" };
			case EWaitIoResult::TimedOut: return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Pipe write deadline exceeded" };
			case EWaitIoResult::Failed: return Failure(EControlIpcTransportDisconnectReason::IoError, error, L"Complete pipe write");
			}
		} else {
			const DWORD error = ::GetLastError();
			return { false, error == ERROR_BROKEN_PIPE ? EControlIpcTransportDisconnectReason::PeerClosed : EControlIpcTransportDisconnectReason::IoError,
				error, FormatError(L"Write pipe", error) };
		}
		if (written == 0) return { false, EControlIpcTransportDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE, L"Pipe peer closed during write" };
		offset += written;
	}
	return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
}

ControlIpcTransportResult DecodeAndDispatch(CControlIpcFrameDecoder& decoder, std::span<const std::uint8_t> bytes,
	const ControlIpcSessionContext& context, IControlIpcSessionHandler& handler, HANDLE pipe, HANDLE stopEvent,
	const ControlIpcNamedPipeOptions& options, bool& shouldClose)
{
	const auto decoded = decoder.Feed(bytes);
	if (decoded.outcome != EControlIpcDecodeOutcome::NeedMoreData && decoded.outcome != EControlIpcDecodeOutcome::Decoded) {
		return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC frame decoder rejected input" };
	}
	for (const auto& frame : decoded.frames) {
		ControlIpcFrameDispatchResult dispatch;
		try {
			dispatch = handler.HandleFrame(context, frame);
		} catch (...) {
			return { false, EControlIpcTransportDisconnectReason::CallbackFailed, ERROR_UNHANDLED_EXCEPTION, L"Control IPC frame handler threw an exception" };
		}
		std::vector<std::uint8_t> output;
		for (const auto& response : dispatch.responseFrames) {
			auto encoded = EncodeControlIpcFrame(response);
			if (encoded.outcome != EControlIpcEncodeOutcome::Encoded || encoded.bytes.size() > options.maximumQueuedBytes - output.size()) {
				return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC response is malformed or exceeds the queued-byte limit" };
			}
			output.insert(output.end(), encoded.bytes.begin(), encoded.bytes.end());
		}
		if (!output.empty()) {
			auto written = WriteAll(pipe, stopEvent, output, options.ioTimeout);
			if (!written.success) return written;
		}
		if (dispatch.decision == EControlIpcSessionDecision::Close) shouldClose = true;
		if (shouldClose) break;
	}
	return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
}

} // namespace

class CControlIpcNamedPipeServer::Impl {
public:
	explicit Impl(std::shared_ptr<IControlIpcFrameHandler> value) : handler(std::move(value)) {}
	struct Session final {
		std::uint64_t id = 0;
		UniqueHandle pipe;
		std::thread worker;
		std::atomic<bool> finished = false;
	};

	void ReapFinishedSessions()
	{
		std::vector<std::shared_ptr<Session>> finished;
		{
			std::lock_guard lock(mutex);
			auto first = std::remove_if(sessions.begin(), sessions.end(), [&](const auto& session) {
				if (!session->finished.load(std::memory_order_acquire)) return false;
				finished.push_back(session);
				return true;
			});
			sessions.erase(first, sessions.end());
		}
		for (const auto& session : finished) if (session->worker.joinable()) session->worker.join();
	}

	void Record(const ControlIpcTransportResult& result) noexcept
	{
		try {
			std::lock_guard lock(mutex);
			if (completed.size() == 64) completed.erase(completed.begin());
			completed.push_back(result);
		} catch (...) {
			// Diagnostics must never terminate an I/O worker when allocation is exhausted.
		}
	}

	void RunSession(std::shared_ptr<Session> session) noexcept
	{
		// Stop() transfers session ownership before it joins workers. A registry lookup can
		// therefore no longer identify a callback thread during that join window; keep the
		// execution identity on the worker itself so callback-initiated Stop() only cancels.
		const ScopedSessionOwner sessionOwner(this);
		ControlIpcTransportResult terminal;
		terminal.reason = EControlIpcTransportDisconnectReason::IoError;
		terminal.errorCode = ERROR_GEN_FAILURE;
		try {
			std::vector<std::uint8_t> buffer(options.readBufferBytes);
			DWORD firstRead = 0;
			terminal = ReadSome(session->pipe.Get(), stopEvent.Get(), buffer, options.ioTimeout, firstRead);
			if (terminal.success) {
				std::wstring diagnostic;
				if (!VerifyNamedPipeClientCurrentUser(session->pipe.Get(), diagnostic)) {
					terminal = { false, EControlIpcTransportDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED, std::move(diagnostic) };
				}
			}
			if (terminal.success) {
				ULONG clientProcessId = 0;
				if (!::GetNamedPipeClientProcessId(session->pipe.Get(), &clientProcessId)) {
					terminal = Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Read named-pipe client process identity");
				}
				if (terminal.success) {
					const ControlIpcSessionContext context{ session->id, clientProcessId };
					auto sessionHandler = handler->CreateSession(context);
					if (!sessionHandler) terminal = { false, EControlIpcTransportDisconnectReason::CallbackFailed, ERROR_INVALID_FUNCTION, L"Control IPC session factory returned no handler" };
					if (terminal.success) {
						CControlIpcFrameDecoder decoder(options.maximumQueuedBytes);
						bool close = false;
						terminal = DecodeAndDispatch(decoder, std::span(buffer).first(firstRead), context, *sessionHandler,
							session->pipe.Get(), stopEvent.Get(), options, close);
						while (terminal.success && !close && !stopping.load(std::memory_order_acquire)) {
							DWORD read = 0;
							terminal = ReadSome(session->pipe.Get(), stopEvent.Get(), buffer, options.ioTimeout, read);
							if (terminal.success) terminal = DecodeAndDispatch(decoder, std::span(buffer).first(read), context, *sessionHandler,
								session->pipe.Get(), stopEvent.Get(), options, close);
						}
						if (terminal.success && (close || stopping.load(std::memory_order_acquire))) {
							terminal = { true, stopping.load(std::memory_order_acquire) ? EControlIpcTransportDisconnectReason::Stopped : EControlIpcTransportDisconnectReason::None,
								ERROR_SUCCESS, close ? L"Session callback closed connection" : L"Server stopped" };
						}
					}
				}
			}
		} catch (...) {
			terminal = {};
			terminal.reason = EControlIpcTransportDisconnectReason::CallbackFailed;
			terminal.errorCode = ERROR_UNHANDLED_EXCEPTION;
		}
		Record(terminal);
		activeSessions.fetch_sub(1, std::memory_order_acq_rel);
		session->finished.store(true, std::memory_order_release);
	}

	std::shared_ptr<IControlIpcFrameHandler> handler;
	std::shared_ptr<CurrentUserSecurityAttributes> security;
	ControlIpcNamedPipeOptions options;
	UniqueHandle stopEvent;
	UniqueHandle acceptPipe;
	std::thread acceptThread;
	std::atomic<bool> running = false;
	std::atomic<bool> stopping = false;
	std::atomic<std::size_t> activeSessions = 0;
	std::atomic<std::size_t> rejectedSessions = 0;
	std::atomic<std::uint64_t> nextSessionId = 1;
	std::mutex stopMutex;
	std::mutex acceptMutex;
	mutable std::mutex mutex;
	std::vector<std::shared_ptr<Session>> sessions;
	std::vector<ControlIpcTransportResult> completed;

	[[nodiscard]] bool IsCallingSessionWorker() noexcept
	{
		return g_currentSessionOwner == this;
	}

	void RequestCancellation() noexcept
	{
		running.store(false, std::memory_order_release);
		stopping.store(true, std::memory_order_release);
		try {
			if (stopEvent) ::SetEvent(stopEvent.Get());
			{
				std::lock_guard acceptLock(acceptMutex);
				if (acceptPipe) ::CancelIoEx(acceptPipe.Get(), nullptr);
			}
			std::lock_guard lock(mutex);
			for (const auto& session : sessions) if (session->pipe) ::CancelIoEx(session->pipe.Get(), nullptr);
		} catch (...) {
			// The atomic stopping state remains observable even if a synchronization primitive fails.
		}
	}
};

CControlIpcNamedPipeServer::CControlIpcNamedPipeServer(std::shared_ptr<IControlIpcFrameHandler> handler) :
	m_impl(std::make_unique<Impl>(std::move(handler))) {}

CControlIpcNamedPipeServer::~CControlIpcNamedPipeServer()
{
	Stop();
}

ControlIpcTransportResult CControlIpcNamedPipeServer::Start(const ControlIpcNamedPipeOptions& options)
{
	std::lock_guard stopLock(m_impl->stopMutex);
	if (m_impl->running.load(std::memory_order_acquire)) return { false, EControlIpcTransportDisconnectReason::IoError, ERROR_ALREADY_EXISTS, L"Control IPC server is already running" };
	if (m_impl->acceptThread.joinable() || !m_impl->sessions.empty()) return { false, EControlIpcTransportDisconnectReason::IoError, ERROR_BUSY, L"Control IPC server has not completed shutdown" };
	if (!m_impl->handler) return { false, EControlIpcTransportDisconnectReason::CallbackFailed, ERROR_INVALID_PARAMETER, L"Control IPC server requires a frame handler" };
	std::wstring diagnostic;
	if (!IsValidOptions(options, diagnostic)) return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_PARAMETER, std::move(diagnostic) };
	try {
		m_impl->security = std::make_shared<CurrentUserSecurityAttributes>();
		if (!m_impl->security->Initialize(diagnostic)) {
			m_impl->security.reset();
			return { false, EControlIpcTransportDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED, std::move(diagnostic) };
		}
		m_impl->stopEvent.Reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!m_impl->stopEvent) {
			const auto failure = Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create server stop event");
			m_impl->security.reset();
			return failure;
		}
		m_impl->options = options;
		m_impl->acceptPipe.Reset(::CreateNamedPipeW(options.pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			static_cast<DWORD>(options.maximumSessions + 1), static_cast<DWORD>(options.maximumQueuedBytes),
			static_cast<DWORD>(options.maximumQueuedBytes), 0, m_impl->security->Attributes()));
		if (!m_impl->acceptPipe) {
			const auto failure = Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create control pipe");
			m_impl->stopEvent.Reset();
			m_impl->security.reset();
			return failure;
		}
		m_impl->stopping.store(false, std::memory_order_release);
		m_impl->running.store(true, std::memory_order_release);
		m_impl->acceptThread = std::thread([impl = m_impl.get()] {
		try {
		while (!impl->stopping.load(std::memory_order_acquire)) {
			impl->ReapFinishedSessions();
			HANDLE acceptPipe = nullptr;
			{
				std::lock_guard acceptLock(impl->acceptMutex);
				if (!impl->acceptPipe) {
					impl->acceptPipe.Reset(::CreateNamedPipeW(impl->options.pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
					static_cast<DWORD>(impl->options.maximumSessions + 1), static_cast<DWORD>(impl->options.maximumQueuedBytes),
					static_cast<DWORD>(impl->options.maximumQueuedBytes), 0, impl->security->Attributes()));
				}
				acceptPipe = impl->acceptPipe.Get();
			}
			if (!acceptPipe) { impl->Record(Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create control pipe")); break; }
			UniqueHandle completion(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (!completion) { impl->Record(Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create accept event")); break; }
			OVERLAPPED overlapped{};
			overlapped.hEvent = completion.Get();
			bool connected = ::ConnectNamedPipe(acceptPipe, &overlapped) != FALSE;
			DWORD connectError = connected ? ERROR_SUCCESS : ::GetLastError();
			if (!connected && connectError == ERROR_PIPE_CONNECTED) connected = true;
			if (!connected && connectError == ERROR_IO_PENDING) {
				DWORD ignored = 0;
				DWORD error = ERROR_SUCCESS;
				const auto waited = WaitForOverlapped(acceptPipe, overlapped, impl->stopEvent.Get(),
					std::chrono::steady_clock::now() + impl->options.ioTimeout, ignored, error);
				connected = waited == EWaitIoResult::Completed;
				if (!connected && waited != EWaitIoResult::Stopped && waited != EWaitIoResult::TimedOut) impl->Record(Failure(EControlIpcTransportDisconnectReason::IoError, error, L"Complete control pipe accept"));
			}
			if (!connected) { std::lock_guard acceptLock(impl->acceptMutex); impl->acceptPipe.Reset(); continue; }
			if (impl->stopping.load(std::memory_order_acquire)) break;
			if (impl->activeSessions.load(std::memory_order_acquire) >= impl->options.maximumSessions) {
				++impl->rejectedSessions;
				std::lock_guard acceptLock(impl->acceptMutex);
				impl->acceptPipe.Reset();
				continue;
			}
			auto session = std::make_shared<Impl::Session>();
			session->id = impl->nextSessionId.fetch_add(1, std::memory_order_relaxed);
			{
				std::lock_guard acceptLock(impl->acceptMutex);
				session->pipe.Reset(impl->acceptPipe.Release());
			}
			bool sessionStarted = false;
			{
				std::lock_guard lock(impl->mutex);
				impl->activeSessions.fetch_add(1, std::memory_order_acq_rel);
				try {
					// Publish before starting while the session mutex is held: Stop can see either no
					// session or a fully owned joinable worker, never a transient worker.
					impl->sessions.push_back(session);
					session->worker = std::thread([impl, session] { impl->RunSession(session); });
					sessionStarted = true;
				} catch (...) {
					if (!impl->sessions.empty() && impl->sessions.back() == session) impl->sessions.pop_back();
					impl->activeSessions.fetch_sub(1, std::memory_order_acq_rel);
				}
			}
			if (!sessionStarted) {
				ControlIpcTransportResult failure;
				failure.reason = EControlIpcTransportDisconnectReason::ResourceExhausted;
				failure.errorCode = ERROR_NOT_ENOUGH_MEMORY;
				impl->Record(failure);
				continue;
			}
		}
		} catch (...) {
			ControlIpcTransportResult failure;
			failure.reason = EControlIpcTransportDisconnectReason::IoError;
			failure.errorCode = ERROR_NOT_ENOUGH_MEMORY;
			impl->Record(failure);
			impl->stopping.store(true, std::memory_order_release);
		}
		impl->running.store(false, std::memory_order_release);
	});
		return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
	} catch (...) {
		m_impl->running.store(false, std::memory_order_release);
		m_impl->stopping.store(true, std::memory_order_release);
		m_impl->acceptPipe.Reset();
		m_impl->stopEvent.Reset();
		m_impl->security.reset();
		return { false, EControlIpcTransportDisconnectReason::IoError, ERROR_NOT_ENOUGH_MEMORY, {} };
	}
}

void CControlIpcNamedPipeServer::Stop() noexcept
{
	// A session callback can request cancellation, but cannot synchronously join another
	// callback worker without risking an AB/BA stopMutex deadlock.
	if (m_impl->IsCallingSessionWorker()) {
		m_impl->RequestCancellation();
		return;
	}
	std::lock_guard stopLock(m_impl->stopMutex);
	m_impl->RequestCancellation();
	if (m_impl->acceptThread.joinable() && m_impl->acceptThread.get_id() != std::this_thread::get_id()) m_impl->acceptThread.join();
	std::vector<std::shared_ptr<Impl::Session>> sessions;
	{
		std::lock_guard lock(m_impl->mutex);
		// The accept worker is joined, so no publisher remains. swap() transfers worker
		// ownership without allocating while Stop() is still noexcept.
		sessions.swap(m_impl->sessions);
	}
	for (const auto& session : sessions) {
		if (!session->worker.joinable()) continue;
		session->worker.join();
	}
	{
		std::lock_guard acceptLock(m_impl->acceptMutex);
		m_impl->acceptPipe.Reset();
	}
	m_impl->stopEvent.Reset();
}

bool CControlIpcNamedPipeServer::IsRunning() const noexcept { return m_impl->running.load(std::memory_order_acquire); }
std::size_t CControlIpcNamedPipeServer::ActiveSessionCount() const noexcept { return m_impl->activeSessions.load(std::memory_order_acquire); }
std::size_t CControlIpcNamedPipeServer::RejectedSessionCount() const noexcept { return m_impl->rejectedSessions.load(std::memory_order_acquire); }
std::vector<ControlIpcTransportResult> CControlIpcNamedPipeServer::CompletedSessions() const { std::lock_guard lock(m_impl->mutex); return m_impl->completed; }

class CControlIpcNamedPipeClient::Impl {
public:
	void MarkDisconnected() noexcept
	{
		connected.store(false, std::memory_order_release);
		if (stopEvent) ::SetEvent(stopEvent.Get());
		if (pipe) ::CancelIoEx(pipe.Get(), nullptr);
	}
	UniqueHandle pipe;
	UniqueHandle stopEvent;
	CControlIpcFrameDecoder decoder;
	std::mutex stateMutex;
	std::mutex ioMutex;
	std::atomic<bool> connected = false;
};

CControlIpcNamedPipeClient::CControlIpcNamedPipeClient() : m_impl(std::make_unique<Impl>()) {}
CControlIpcNamedPipeClient::~CControlIpcNamedPipeClient() { Close(); }

ControlIpcTransportResult CControlIpcNamedPipeClient::Connect(std::wstring pipeName, std::uint32_t expectedServerProcessId,
	std::chrono::milliseconds deadline)
{
	if (deadline <= std::chrono::milliseconds::zero() || deadline > kMaximumCallerTimeout) return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Connect deadline is invalid" };
	if (!IsSafeControlPipeName(pipeName) || expectedServerProcessId == 0) return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_INVALID_PARAMETER, L"Control pipe endpoint is invalid" };
	Close();
	const auto end = std::chrono::steady_clock::now() + deadline;
	HANDLE rawPipe = INVALID_HANDLE_VALUE;
	for (DWORD attempt = 0; attempt < kMaximumBusyPipeAttempts; ++attempt) {
		rawPipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr, OPEN_EXISTING,
			// The server's mandatory current-user SID check uses ImpersonateNamedPipeClient
			// after the first bounded read, so the client must permit that verification.
			FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
		if (rawPipe != INVALID_HANDLE_VALUE) break;
		const DWORD error = ::GetLastError();
		if (error != ERROR_PIPE_BUSY || RemainingMilliseconds(end) == 0) return Failure(EControlIpcTransportDisconnectReason::ConnectFailed, error, L"Open control pipe");
		const DWORD backoff = (std::min)(RemainingMilliseconds(end),
			static_cast<DWORD>(25u << static_cast<unsigned int>(attempt)));
		if (!::WaitNamedPipeW(pipeName.c_str(), backoff) && ::GetLastError() != ERROR_SEM_TIMEOUT) return Failure(EControlIpcTransportDisconnectReason::ConnectFailed, ::GetLastError(), L"Wait for control pipe");
	}
	if (rawPipe == INVALID_HANDLE_VALUE) return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_PIPE_BUSY, L"Control pipe remained busy after bounded connect attempts" };
	UniqueHandle candidate(rawPipe);
	std::wstring diagnostic;
	if (!VerifyCurrentUserOnlyDacl(candidate.Get(), diagnostic)) return { false, EControlIpcTransportDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED, std::move(diagnostic) };
	ULONG serverPid = 0;
	if (!::GetNamedPipeServerProcessId(candidate.Get(), &serverPid) || serverPid != expectedServerProcessId) return { false, EControlIpcTransportDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED, L"Control pipe server process identity does not match the endpoint" };
	UniqueHandle stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!stop) return Failure(EControlIpcTransportDisconnectReason::IoError, ::GetLastError(), L"Create client stop event");
	std::lock_guard ioLock(m_impl->ioMutex);
	std::lock_guard stateLock(m_impl->stateMutex);
	m_impl->pipe.Reset(candidate.Release());
	m_impl->stopEvent.Reset(stop.Release());
	m_impl->decoder.Reset();
	m_impl->connected.store(true, std::memory_order_release);
	return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
}

ControlIpcTransportResult CControlIpcNamedPipeClient::Send(const ControlIpcFrame& frame, std::chrono::milliseconds deadline)
{
	if (deadline <= std::chrono::milliseconds::zero() || deadline > kMaximumCallerTimeout) return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Send deadline is invalid" };
	auto encoded = EncodeControlIpcFrame(frame);
	if (encoded.outcome != EControlIpcEncodeOutcome::Encoded) return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC frame cannot be encoded" };
	std::lock_guard ioLock(m_impl->ioMutex);
	if (!m_impl->connected.load(std::memory_order_acquire)) return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_INVALID_HANDLE, L"Control IPC client is not connected" };
	auto result = WriteAll(m_impl->pipe.Get(), m_impl->stopEvent.Get(), encoded.bytes, deadline);
	if (!result.success) m_impl->MarkDisconnected();
	return result;
}

ControlIpcTransportResult CControlIpcNamedPipeClient::Receive(std::vector<ControlIpcFrame>& frames, std::chrono::milliseconds deadline)
{
	frames.clear();
	if (deadline <= std::chrono::milliseconds::zero() || deadline > kMaximumCallerTimeout) return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Receive deadline is invalid" };
	std::lock_guard ioLock(m_impl->ioMutex);
	if (!m_impl->connected.load(std::memory_order_acquire)) return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_INVALID_HANDLE, L"Control IPC client is not connected" };
	std::vector<std::uint8_t> bytes;
	try {
		bytes.resize(16 * 1024);
	} catch (...) {
		return { false, EControlIpcTransportDisconnectReason::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY, {} };
	}
	const auto end = std::chrono::steady_clock::now() + deadline;
	for (;;) {
		const DWORD remaining = RemainingMilliseconds(end);
		if (remaining == 0) { m_impl->MarkDisconnected(); return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Receive deadline exceeded" }; }
		DWORD read = 0;
		auto result = ReadSome(m_impl->pipe.Get(), m_impl->stopEvent.Get(), bytes, std::chrono::milliseconds(remaining), read);
		if (!result.success) { m_impl->MarkDisconnected(); return result; }
		try {
			auto decoded = m_impl->decoder.Feed(std::span(bytes).first(read));
			if (decoded.outcome != EControlIpcDecodeOutcome::NeedMoreData && decoded.outcome != EControlIpcDecodeOutcome::Decoded) { m_impl->MarkDisconnected(); return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC response frame is malformed" }; }
			if (!decoded.frames.empty()) { frames = std::move(decoded.frames); return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} }; }
		} catch (...) {
			m_impl->MarkDisconnected();
			return { false, EControlIpcTransportDisconnectReason::IoError, ERROR_NOT_ENOUGH_MEMORY, {} };
		}
	}
}

ControlIpcTransportResult CControlIpcNamedPipeClient::Exchange(const ControlIpcFrame& request,
	std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline)
{
	if (deadline <= std::chrono::milliseconds::zero() || deadline > kMaximumCallerTimeout) return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Exchange deadline is invalid" };
	if (!HasFlag(request.header.flags, EControlIpcFlags::Request) ||
		HasFlag(request.header.flags, EControlIpcFlags::Response) || HasFlag(request.header.flags, EControlIpcFlags::Terminal)) {
		return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Exchange requires a non-terminal request frame" };
	}
	const auto end = std::chrono::steady_clock::now() + deadline;
	auto encoded = EncodeControlIpcFrame(request);
	if (encoded.outcome != EControlIpcEncodeOutcome::Encoded) return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC request cannot be encoded" };
	responses.clear();
	std::vector<std::uint8_t> bytes;
	try {
		bytes.resize(16 * 1024);
	} catch (...) {
		return { false, EControlIpcTransportDisconnectReason::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY, {} };
	}
	std::lock_guard ioLock(m_impl->ioMutex);
	if (!m_impl->connected.load(std::memory_order_acquire)) return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_INVALID_HANDLE, L"Control IPC client is not connected" };
	std::size_t responseBytes = 0;
	const DWORD sendRemaining = RemainingMilliseconds(end);
	if (sendRemaining == 0) { m_impl->MarkDisconnected(); responses.clear(); return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Exchange deadline exceeded before write" }; }
	auto result = WriteAll(m_impl->pipe.Get(), m_impl->stopEvent.Get(), encoded.bytes, std::chrono::milliseconds(sendRemaining));
	if (!result.success) { m_impl->MarkDisconnected(); responses.clear(); return result; }
	for (;;) {
		const DWORD remaining = RemainingMilliseconds(end);
		if (remaining == 0) { m_impl->MarkDisconnected(); responses.clear(); return { false, EControlIpcTransportDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT, L"Exchange deadline exceeded" }; }
		DWORD read = 0;
		result = ReadSome(m_impl->pipe.Get(), m_impl->stopEvent.Get(), bytes, std::chrono::milliseconds(remaining), read);
		if (!result.success) { m_impl->MarkDisconnected(); responses.clear(); return result; }
		try {
			auto decoded = m_impl->decoder.Feed(std::span(bytes).first(read));
			if (decoded.outcome != EControlIpcDecodeOutcome::NeedMoreData && decoded.outcome != EControlIpcDecodeOutcome::Decoded) { m_impl->MarkDisconnected(); responses.clear(); return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Control IPC exchange response is malformed" }; }
			bool sawTerminal = false;
			for (auto& frame : decoded.frames) {
				if (sawTerminal) {
					m_impl->MarkDisconnected();
					responses.clear();
					return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Frames after a terminal P0 exchange response are unsupported" };
				}
				if (!HasFlag(frame.header.flags, EControlIpcFlags::Response) || frame.header.requestId != request.header.requestId) {
					m_impl->MarkDisconnected();
					responses.clear();
					return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA, L"Unsolicited or mis-correlated frame is unsupported by P0 exchange" };
				}
				const auto encodedFrameBytes = kControlIpcLengthPrefixBytes + kControlIpcHeaderBytes + frame.payload.size();
				if (encodedFrameBytes > kControlIpcMaximumFrameBytes - responseBytes) {
					m_impl->MarkDisconnected();
					responses.clear();
					return { false, EControlIpcTransportDisconnectReason::ProtocolError, ERROR_INVALID_DATA,
						L"Control IPC exchange responses exceed the cumulative byte limit" };
				}
				responseBytes += encodedFrameBytes;
				const bool terminal = HasFlag(frame.header.flags, EControlIpcFlags::Terminal);
				responses.push_back(std::move(frame));
				sawTerminal = terminal;
			}
			if (sawTerminal) return { true, EControlIpcTransportDisconnectReason::None, ERROR_SUCCESS, {} };
		} catch (...) {
			m_impl->MarkDisconnected();
			responses.clear();
			return { false, EControlIpcTransportDisconnectReason::IoError, ERROR_NOT_ENOUGH_MEMORY, {} };
		}
	}
}

void CControlIpcNamedPipeClient::Close() noexcept
{
	{
		std::lock_guard lock(m_impl->stateMutex);
		m_impl->connected.store(false, std::memory_order_release);
		if (m_impl->stopEvent) ::SetEvent(m_impl->stopEvent.Get());
		if (m_impl->pipe) ::CancelIoEx(m_impl->pipe.Get(), nullptr);
	}
	std::lock_guard ioLock(m_impl->ioMutex);
	std::lock_guard stateLock(m_impl->stateMutex);
	m_impl->pipe.Reset();
	m_impl->stopEvent.Reset();
	m_impl->decoder.Reset();
}

bool CControlIpcNamedPipeClient::IsConnected() const noexcept { return m_impl->connected.load(std::memory_order_acquire); }

} // namespace platform::controlipc
