/*! @file */
#include <sakura/harnessbridge/HarnessBridgeTransport.h>

#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <thread>

namespace platform::harnessbridge {
namespace {

HarnessBridgeTransportResult Failure(const EHarnessBridgeDisconnectReason reason, const DWORD error = 0) noexcept
{
	return { false, reason, error };
}

bool DeadlineReached(const std::chrono::steady_clock::time_point deadline) noexcept
{
	return deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= deadline;
}

HarnessBridgeTransportResult ReadExact(HANDLE pipe, HANDLE stopEvent, std::vector<std::uint8_t>& destination,
	const std::size_t offset, const std::size_t count, const std::chrono::steady_clock::time_point deadline)
{
	std::size_t readTotal = 0;
	while (readTotal < count) {
		if (DeadlineReached(deadline)) return Failure(EHarnessBridgeDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT);
		if (stopEvent && ::WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) return Failure(EHarnessBridgeDisconnectReason::Stopped, ERROR_OPERATION_ABORTED);
		DWORD available = 0;
		if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
			const auto error = ::GetLastError();
			return Failure(error == ERROR_BROKEN_PIPE ? EHarnessBridgeDisconnectReason::PeerClosed : EHarnessBridgeDisconnectReason::IoError, error);
		}
		if (available == 0) { ::Sleep(1); continue; }
		DWORD read = 0;
		const DWORD requested = static_cast<DWORD>((std::min)(count - readTotal, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		if (!::ReadFile(pipe, destination.data() + offset + readTotal, requested, &read, nullptr)) {
			const auto error = ::GetLastError();
			return Failure(error == ERROR_BROKEN_PIPE ? EHarnessBridgeDisconnectReason::PeerClosed : EHarnessBridgeDisconnectReason::IoError, error);
		}
		if (read == 0) return Failure(EHarnessBridgeDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE);
		readTotal += read;
	}
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult ReadFrame(HANDLE pipe, HANDLE stopEvent, const std::size_t maximumFrameBytes,
	HarnessBridgeFrame& frame, const std::chrono::steady_clock::time_point deadline)
{
	std::vector<std::uint8_t> prefix(4);
	auto result = ReadExact(pipe, stopEvent, prefix, 0, prefix.size(), deadline);
	if (!result.success) return result;
	const std::uint32_t frameBytes = static_cast<std::uint32_t>(prefix[0])
		| (static_cast<std::uint32_t>(prefix[1]) << 8)
		| (static_cast<std::uint32_t>(prefix[2]) << 16)
		| (static_cast<std::uint32_t>(prefix[3]) << 24);
	if (frameBytes < kHarnessBridgeHeaderBytes || frameBytes > maximumFrameBytes) return Failure(EHarnessBridgeDisconnectReason::ProtocolError, ERROR_BAD_LENGTH);
	std::vector<std::uint8_t> encoded(4 + frameBytes);
	std::copy(prefix.begin(), prefix.end(), encoded.begin());
	result = ReadExact(pipe, stopEvent, encoded, 4, frameBytes, deadline);
	if (!result.success) return result;
	CHarnessBridgeFrameDecoder decoder(maximumFrameBytes);
	const auto decoded = decoder.Feed(encoded);
	if (decoded.outcome != EHarnessBridgeDecodeOutcome::Decoded || decoded.frames.size() != 1) return Failure(EHarnessBridgeDisconnectReason::ProtocolError, ERROR_INVALID_DATA);
	frame = decoded.frames.front();
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult WriteFrame(HANDLE pipe, const HarnessBridgeFrame& frame,
	const std::chrono::steady_clock::time_point deadline)
{
	const auto encoded = EncodeHarnessBridgeFrame(frame);
	if (encoded.outcome != EHarnessBridgeEncodeOutcome::Encoded) return Failure(EHarnessBridgeDisconnectReason::ProtocolError, ERROR_INVALID_DATA);
	std::size_t offset = 0;
	while (offset < encoded.bytes.size()) {
		if (DeadlineReached(deadline)) return Failure(EHarnessBridgeDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT);
		DWORD written = 0;
		const DWORD count = static_cast<DWORD>((std::min)(encoded.bytes.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
		if (!::WriteFile(pipe, encoded.bytes.data() + offset, count, &written, nullptr)) {
			const auto error = ::GetLastError();
			return Failure(error == ERROR_BROKEN_PIPE ? EHarnessBridgeDisconnectReason::PeerClosed : EHarnessBridgeDisconnectReason::IoError, error);
		}
		if (written == 0) return Failure(EHarnessBridgeDisconnectReason::PeerClosed, ERROR_BROKEN_PIPE);
		offset += written;
	}
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

} // namespace

class CHarnessBridgeNamedPipeServer::Impl final {
public:
	explicit Impl(std::shared_ptr<IHarnessBridgeSessionFactory> factory)
		: factory(std::move(factory)) {}
	~Impl() { Stop(); }

	struct Session final {
		HANDLE pipe = INVALID_HANDLE_VALUE;
		std::thread thread;
		std::atomic<bool> finished{ false };
	};

	std::shared_ptr<IHarnessBridgeSessionFactory> factory;
	HarnessBridgeTransportOptions options;
	HANDLE stopEvent = nullptr;
	std::thread acceptThread;
	mutable std::mutex mutex;
	std::vector<std::shared_ptr<Session>> sessions;
	std::atomic<std::uint64_t> nextSession{ 1 };
	std::atomic<bool> stopping{ false };

	void Stop() noexcept
	{
		if (!stopEvent && !acceptThread.joinable()) return;
		stopping.store(true, std::memory_order_release);
		if (stopEvent) ::SetEvent(stopEvent);
		if (acceptThread.joinable()) {
			::CancelSynchronousIo(reinterpret_cast<HANDLE>(acceptThread.native_handle()));
			acceptThread.join();
		}
		std::vector<std::shared_ptr<Session>> owned;
		{
			std::lock_guard lock(mutex);
			owned.swap(sessions);
		}
		for (const auto& session : owned) if (session->thread.joinable()) {
			::CancelSynchronousIo(reinterpret_cast<HANDLE>(session->thread.native_handle()));
		}
		for (const auto& session : owned) {
			if (session->thread.joinable()) session->thread.join();
			if (session->pipe != INVALID_HANDLE_VALUE) ::CloseHandle(session->pipe);
		}
		if (stopEvent) ::CloseHandle(stopEvent);
		stopEvent = nullptr;
		stopping.store(false, std::memory_order_release);
	}

	HANDLE CreatePipe(const bool firstInstance) const
	{
		HarnessBridgeSecurityAttributes security;
		std::wstring ignored;
		if (!security.Initialize(ignored)) return INVALID_HANDLE_VALUE;
		return ::CreateNamedPipeW(options.pipeName.c_str(), PIPE_ACCESS_DUPLEX | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			static_cast<DWORD>(options.maximumSessions),
			static_cast<DWORD>((std::min)(options.maximumQueuedBytes, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))),
			static_cast<DWORD>((std::min)(options.maximumQueuedBytes, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))),
			0, security.Attributes());
	}

	void RunSession(const std::shared_ptr<Session>& session, const HarnessBridgeSessionContext context,
		HarnessBridgeFrame first)
	{
		struct Finished final {
			std::atomic<bool>& value;
			~Finished() { value.store(true, std::memory_order_release); }
		} finished{ session->finished };
		std::unique_ptr<IHarnessBridgeSessionHandler> handler;
		try {
			handler = factory->CreateSession(context, first);
		} catch (...) {
			return;
		}
		if (!handler) { session->finished.store(true, std::memory_order_release); return; }
		HarnessBridgeFrame current = std::move(first);
		while (!stopping.load(std::memory_order_acquire)) {
			std::vector<HarnessBridgeFrame> responses;
			HarnessBridgeTransportResult handled;
			try {
				handled = handler->HandleFrame(context, current, responses);
			} catch (...) {
				break;
			}
			if (!handled.success || responses.size() > 64) break;
			std::size_t total = 0;
			bool terminalSeen = false;
			for (const auto& response : responses) {
				if (response.header.requestId != current.header.requestId
					|| (response.header.flags & EHarnessBridgeFrameFlags::Request) != EHarnessBridgeFrameFlags::None
					|| (terminalSeen && (response.header.flags & EHarnessBridgeFrameFlags::Terminal) != EHarnessBridgeFrameFlags::None)) return;
				if ((response.header.flags & EHarnessBridgeFrameFlags::Terminal) != EHarnessBridgeFrameFlags::None) terminalSeen = true;
				const auto encoded = EncodeHarnessBridgeFrame(response);
				if (encoded.outcome != EHarnessBridgeEncodeOutcome::Encoded || encoded.bytes.size() > options.maximumQueuedBytes - total) {
					return;
				}
				total += encoded.bytes.size();
				if (!WriteFrame(session->pipe, response,
					std::chrono::steady_clock::now() + options.ioTimeout).success) return;
			}
			if (!ReadFrame(session->pipe, stopEvent, options.maximumQueuedBytes, current,
				std::chrono::steady_clock::now() + options.ioTimeout).success) break;
		}
	}

	void ReapFinished()
	{
		std::vector<std::shared_ptr<Session>> finished;
		{
			std::lock_guard lock(mutex);
			auto it = sessions.begin();
			while (it != sessions.end()) {
				if ((*it)->finished.load(std::memory_order_acquire)) { finished.push_back(*it); it = sessions.erase(it); }
				else ++it;
			}
		}
		for (const auto& session : finished) {
			if (session->thread.joinable()) session->thread.join();
			if (session->pipe != INVALID_HANDLE_VALUE) { ::DisconnectNamedPipe(session->pipe); ::CloseHandle(session->pipe); session->pipe = INVALID_HANDLE_VALUE; }
		}
	}

	void AcceptLoop(HANDLE initialPipe)
	{
		if (stopping.load(std::memory_order_acquire)) {
			if (initialPipe != INVALID_HANDLE_VALUE) ::CloseHandle(initialPipe);
			return;
		}
		HANDLE nextPipe = initialPipe;
		while (!stopping.load(std::memory_order_acquire)) {
			ReapFinished();
			HANDLE pipe = nextPipe;
			nextPipe = INVALID_HANDLE_VALUE;
			if (pipe == INVALID_HANDLE_VALUE) pipe = CreatePipe(false);
			if (pipe == INVALID_HANDLE_VALUE) break;
			BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
			const auto connectError = connected ? ERROR_SUCCESS : ::GetLastError();
			if (!connected && connectError != ERROR_PIPE_CONNECTED) { ::CloseHandle(pipe); if (stopping.load()) break; continue; }
			if (stopping.load()) { ::DisconnectNamedPipe(pipe); ::CloseHandle(pipe); break; }
			std::uint32_t clientPid = 0;
			if (!::GetNamedPipeClientProcessId(pipe, reinterpret_cast<PULONG>(&clientPid)) || clientPid == 0) { ::DisconnectNamedPipe(pipe); ::CloseHandle(pipe); continue; }
			HarnessBridgeFrame first;
			std::wstring securityDiagnostic;
			if (!ReadFrame(pipe, stopEvent, options.maximumQueuedBytes, first, std::chrono::steady_clock::now() + options.ioTimeout).success
				|| !VerifyHarnessNamedPipeClientCurrentUser(pipe, securityDiagnostic)) {
				::DisconnectNamedPipe(pipe); ::CloseHandle(pipe); continue;
			}
			std::lock_guard lock(mutex);
			if (sessions.size() >= options.maximumSessions) { ::DisconnectNamedPipe(pipe); ::CloseHandle(pipe); continue; }
			auto session = std::make_shared<Session>();
			session->pipe = pipe;
			const HarnessBridgeSessionContext context{ nextSession.fetch_add(1), clientPid };
			session->thread = std::thread([this, session, context, first = std::move(first)]() mutable { RunSession(session, context, std::move(first)); });
			sessions.push_back(std::move(session));
		}
	}
};

CHarnessBridgeNamedPipeServer::CHarnessBridgeNamedPipeServer(std::shared_ptr<IHarnessBridgeSessionFactory> factory)
	: m_impl(std::make_unique<Impl>(std::move(factory))) {}
CHarnessBridgeNamedPipeServer::~CHarnessBridgeNamedPipeServer() = default;

HarnessBridgeTransportResult CHarnessBridgeNamedPipeServer::Start(const HarnessBridgeTransportOptions& options)
{
	if (!m_impl->factory || !IsSafeHarnessPipeName(options.pipeName) || options.maximumSessions == 0 || options.maximumSessions > 63
		|| options.maximumQueuedBytes < kHarnessBridgeHeaderBytes || options.maximumQueuedBytes > kHarnessBridgeMaximumFrameBytes
		|| options.readBufferBytes == 0 || options.ioTimeout.count() <= 0) return Failure(EHarnessBridgeDisconnectReason::IoError, ERROR_INVALID_PARAMETER);
	if (m_impl->acceptThread.joinable()) return Failure(EHarnessBridgeDisconnectReason::ResourceExhausted, ERROR_ALREADY_INITIALIZED);
	m_impl->options = options;
	m_impl->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!m_impl->stopEvent) return Failure(EHarnessBridgeDisconnectReason::IoError, ::GetLastError());
	m_impl->stopping.store(false, std::memory_order_release);
	const HANDLE initialPipe = m_impl->CreatePipe(true);
	if (initialPipe == INVALID_HANDLE_VALUE) {
		::CloseHandle(m_impl->stopEvent);
		m_impl->stopEvent = nullptr;
		return Failure(EHarnessBridgeDisconnectReason::IoError, ::GetLastError());
	}
	try {
		m_impl->acceptThread = std::thread([impl = m_impl.get(), initialPipe] { impl->AcceptLoop(initialPipe); });
	} catch (...) {
		::CloseHandle(initialPipe);
		::CloseHandle(m_impl->stopEvent);
		m_impl->stopEvent = nullptr;
		return Failure(EHarnessBridgeDisconnectReason::IoError, ERROR_NOT_ENOUGH_MEMORY);
	}
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

void CHarnessBridgeNamedPipeServer::Stop() noexcept { m_impl->Stop(); }
bool CHarnessBridgeNamedPipeServer::IsRunning() const noexcept { return m_impl->acceptThread.joinable() && !m_impl->stopping.load(); }
std::size_t CHarnessBridgeNamedPipeServer::ActiveSessionCount() const noexcept
{
	std::lock_guard lock(m_impl->mutex);
	std::size_t count = 0;
	for (const auto& session : m_impl->sessions) if (!session->finished.load()) ++count;
	return count;
}

class CHarnessBridgeNamedPipeClient::Impl final {
public:
	HANDLE pipe = INVALID_HANDLE_VALUE;
	HANDLE stopEvent = nullptr;
	std::mutex mutex;
	void Close() noexcept
	{
		if (stopEvent) ::SetEvent(stopEvent);
		if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
		if (stopEvent) ::CloseHandle(stopEvent);
		pipe = INVALID_HANDLE_VALUE;
		stopEvent = nullptr;
	}
};

CHarnessBridgeNamedPipeClient::CHarnessBridgeNamedPipeClient() : m_impl(std::make_unique<Impl>()) {}
CHarnessBridgeNamedPipeClient::~CHarnessBridgeNamedPipeClient() { Close(); }

HarnessBridgeTransportResult CHarnessBridgeNamedPipeClient::Connect(std::wstring pipeName,
	const std::uint32_t expectedServerProcessId, const std::chrono::milliseconds deadline)
{
	std::lock_guard lock(m_impl->mutex);
	if (!IsSafeHarnessPipeName(pipeName) || expectedServerProcessId == 0 || deadline.count() <= 0) return Failure(EHarnessBridgeDisconnectReason::ConnectFailed, ERROR_INVALID_PARAMETER);
	m_impl->Close();
	m_impl->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!m_impl->stopEvent) return Failure(EHarnessBridgeDisconnectReason::IoError, ::GetLastError());
	const auto end = std::chrono::steady_clock::now() + deadline;
	while (!DeadlineReached(end)) {
		m_impl->pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (m_impl->pipe != INVALID_HANDLE_VALUE) break;
		const auto error = ::GetLastError();
		if (error != ERROR_PIPE_BUSY) { m_impl->Close(); return Failure(error == ERROR_ACCESS_DENIED ? EHarnessBridgeDisconnectReason::AccessDenied : EHarnessBridgeDisconnectReason::ConnectFailed, error); }
		::WaitNamedPipeW(pipeName.c_str(), 10);
	}
	if (m_impl->pipe == INVALID_HANDLE_VALUE) { m_impl->Close(); return Failure(EHarnessBridgeDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT); }
	std::wstring diagnostic;
	if (!VerifyHarnessCurrentUserOnlyDacl(m_impl->pipe, diagnostic)) { m_impl->Close(); return Failure(EHarnessBridgeDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED); }
	DWORD actualServer = 0;
	if (!::GetNamedPipeServerProcessId(m_impl->pipe, reinterpret_cast<PULONG>(&actualServer)) || actualServer != expectedServerProcessId) { m_impl->Close(); return Failure(EHarnessBridgeDisconnectReason::AccessDenied, ERROR_ACCESS_DENIED); }
	DWORD mode = PIPE_READMODE_BYTE;
	if (!::SetNamedPipeHandleState(m_impl->pipe, &mode, nullptr, nullptr)) { m_impl->Close(); return Failure(EHarnessBridgeDisconnectReason::IoError, ::GetLastError()); }
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult CHarnessBridgeNamedPipeClient::Exchange(const HarnessBridgeFrame& request,
	std::vector<HarnessBridgeFrame>& responses, const std::chrono::milliseconds deadline)
{
	std::lock_guard lock(m_impl->mutex);
	responses.clear();
	if (m_impl->pipe == INVALID_HANDLE_VALUE) return Failure(EHarnessBridgeDisconnectReason::ConnectFailed, ERROR_INVALID_HANDLE);
	const auto end = std::chrono::steady_clock::now() + deadline;
	auto result = WriteFrame(m_impl->pipe, request, end);
	if (!result.success) { m_impl->Close(); return result; }
	while (!DeadlineReached(end)) {
		HarnessBridgeFrame response;
		result = ReadFrame(m_impl->pipe, m_impl->stopEvent, kHarnessBridgeMaximumFrameBytes, response, end);
		if (!result.success) { m_impl->Close(); return result; }
		if (response.header.requestId != request.header.requestId || (response.header.flags & EHarnessBridgeFrameFlags::Request) != EHarnessBridgeFrameFlags::None) {
			m_impl->Close(); return Failure(EHarnessBridgeDisconnectReason::ProtocolError, ERROR_INVALID_DATA);
		}
		responses.push_back(std::move(response));
		const auto& last = responses.back();
		// Challenge and Ready are complete handshake turns even though they are
		// not terminal operation frames. Waiting for Terminal here would prevent
		// the client from sending Authenticate and deadlock the handshake.
		if ((last.header.flags & EHarnessBridgeFrameFlags::Terminal) != EHarnessBridgeFrameFlags::None
			|| last.header.kind == EHarnessBridgeFrameKind::Challenge
			|| last.header.kind == EHarnessBridgeFrameKind::Ready) {
			return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
		}
	}
	m_impl->Close();
	return Failure(EHarnessBridgeDisconnectReason::DeadlineExceeded, ERROR_SEM_TIMEOUT);
}

void CHarnessBridgeNamedPipeClient::Close() noexcept
{
	if (!m_impl) return;
	std::lock_guard lock(m_impl->mutex);
	m_impl->Close();
}

bool CHarnessBridgeNamedPipeClient::IsConnected() const noexcept
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->pipe != INVALID_HANDLE_VALUE;
}

} // namespace platform::harnessbridge
