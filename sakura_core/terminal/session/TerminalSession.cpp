/*! @file */
#include "StdAfx.h"
#include "terminal/session/TerminalSession.h"
#include "terminal/TerminalWorkerRetirementService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <windows.h>

namespace terminal {
namespace {

constexpr std::size_t kBackendReadBytes = 64u * 1024u;
constexpr std::size_t kBackendWriteBytes = 16u * 1024u;
constexpr auto kBackendReadTimeout = std::chrono::milliseconds(100);
constexpr auto kResizeCoalesceDelay = std::chrono::milliseconds(16);
constexpr std::size_t kMinimumDiagnosticFileBytes = 4096u;
constexpr std::size_t kMinimumDiagnosticQueuedEvents = 16u;

std::atomic<std::uint64_t> g_diagnosticSessionSequence{ 0 };

struct ByteChunk {
	std::vector<std::uint8_t> bytes;
	std::size_t offset = 0;
	TerminalInputSource source{ TerminalInputSource::Interactive };
};

std::string Sha256Hex( std::span<const std::uint8_t> bytes ) noexcept
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::string result;
	if( ::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ) return result;
	if( ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0 ) {
		::BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}
	auto* data = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data()));
	if( bytes.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())
		|| ::BCryptHashData(hash, data, static_cast<ULONG>(bytes.size()), 0) < 0 ) {
		::BCryptDestroyHash(hash);
		::BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}
	std::array<UCHAR, 32> digest{};
	if( ::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 ) {
		constexpr char digits[] = "0123456789abcdef";
		result.reserve(digest.size() * 2);
		for( const auto byte : digest ) {
			result.push_back(digits[byte >> 4]);
			result.push_back(digits[byte & 0x0f]);
		}
	}
	::BCryptDestroyHash(hash);
	::BCryptCloseAlgorithmProvider(algorithm, 0);
	return result;
}

class TerminalDiagnosticRecorder final {
public:
	explicit TerminalDiagnosticRecorder( TerminalDiagnosticOptions options ) noexcept
		: m_maximumFileBytes((std::max)(options.maximumFileBytes, kMinimumDiagnosticFileBytes))
		, m_maximumQueuedEvents((std::max)(options.maximumQueuedEvents, kMinimumDiagnosticQueuedEvents))
		, m_startedAt(std::chrono::steady_clock::now())
	{
		try {
			const auto eventName = L"Local\\SakuraEditorNext.TerminalTrace." + std::to_wstring(::GetCurrentProcessId());
			m_activationEvent = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
			if( options.Enabled() ) Activate(options.directory);
		} catch( ... ) {
			// Tracing is optional and must never affect terminal construction.
		}
	}

	~TerminalDiagnosticRecorder() noexcept
	{
		Stop();
		if( m_activationEvent != nullptr ) ::CloseHandle(m_activationEvent);
	}

	void Stop() noexcept
	{
		m_enabled.store(false, std::memory_order_release);
		{
			const std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_available.notify_all();
		if( m_worker.joinable() ) {
			const auto workerHandle = reinterpret_cast<HANDLE>(m_worker.native_handle());
			if( ::WaitForSingleObject(workerHandle, 250) == WAIT_TIMEOUT ) {
				{
					const std::lock_guard lock(m_mutex);
					m_events.clear();
				}
				::CancelSynchronousIo(workerHandle);
				m_available.notify_all();
			}
			m_worker.join();
		}
	}

	TerminalDiagnosticRecorder( const TerminalDiagnosticRecorder& ) = delete;
	TerminalDiagnosticRecorder& operator=( const TerminalDiagnosticRecorder& ) = delete;

	[[nodiscard]] std::wstring Path() const
	{
		if( !m_enabled.load(std::memory_order_acquire) && !m_worker.joinable() ) return {};
		return m_currentPath.wstring();
	}

	void RecordRead( std::span<const std::uint8_t> bytes, std::size_t queueBefore, std::size_t queueAfter ) noexcept
	{
		m_totalReadBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
		m_totalReadEvents.fetch_add(1, std::memory_order_relaxed);
		if( !EnsureEnabled() ) return;
		RecordBytes("pty_read", bytes, "\"queue_before\":" + std::to_string(queueBefore) +
			",\"queue_after\":" + std::to_string(queueAfter));
	}

	void RecordWrite( std::span<const std::uint8_t> bytes, TerminalInputSource source, std::size_t queueAfter ) noexcept
	{
		m_totalWrittenBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
		if( source == TerminalInputSource::Protocol ) m_totalProtocolBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
		if( !EnsureEnabled() ) return;
		RecordBytes("pty_write", bytes, "\"source\":\"" + std::string(source == TerminalInputSource::Protocol ? "protocol" : "interactive") +
			"\",\"queue_after\":" + std::to_string(queueAfter));
	}

	void RecordDrain( std::span<const std::uint8_t> bytes, std::size_t queueBefore, std::size_t queueAfter, std::uint64_t durationMicros ) noexcept
	{
		if( !EnsureEnabled() ) return;
		RecordBytes("ui_drain", bytes, "\"queue_before\":" + std::to_string(queueBefore) +
			",\"queue_after\":" + std::to_string(queueAfter) +
			",\"duration_us\":" + std::to_string(durationMicros));
	}

	void RecordResize( std::string_view kind, TerminalSize size, std::size_t coalesced = 0 ) noexcept
	{
		m_totalResizeEvents.fetch_add(1, std::memory_order_relaxed);
		if( !EnsureEnabled() ) return;
		Record(kind, "\"columns\":" + std::to_string(size.columns) +
			",\"rows\":" + std::to_string(size.rows) +
			",\"coalesced\":" + std::to_string(coalesced));
	}

	void RecordState( TerminalSessionState state, std::uint32_t errorCode ) noexcept
	{
		if( !EnsureEnabled() ) return;
		Record("session_state", "\"state\":" + std::to_string(static_cast<unsigned int>(state)) +
			",\"error\":" + std::to_string(errorCode));
	}

	void RecordModel( const TerminalModelDiagnosticSnapshot& value ) noexcept
	{
		m_totalAppended.fetch_add(value.scrollbackAppended, std::memory_order_relaxed);
		m_totalEvicted.fetch_add(value.scrollbackEvicted, std::memory_order_relaxed);
		m_lastScrollbackRows.store(value.scrollbackRows, std::memory_order_relaxed);
		m_lastScrollbackLimit.store(value.scrollbackLimit, std::memory_order_relaxed);
		if( value.scrollbackEvicted != 0 ) {
			std::uint64_t expected = 0;
			const auto elapsed = ElapsedMicros();
			m_firstEvictionMicros.compare_exchange_strong(expected, (std::max<std::uint64_t>)(1, elapsed), std::memory_order_relaxed);
			m_lastEvictionMicros.store(elapsed, std::memory_order_relaxed);
		}
		if( !EnsureEnabled() ) return;
		Record("model_publish",
			"\"bytes_drained\":" + std::to_string(value.bytesDrained) +
			",\"scrollback_appended\":" + std::to_string(value.scrollbackAppended) +
			",\"scrollback_evicted\":" + std::to_string(value.scrollbackEvicted) +
			",\"scrollback_cleared\":" + Boolean(value.scrollbackCleared) +
			",\"scrollback_rows\":" + std::to_string(value.scrollbackRows) +
			",\"scrollback_limit\":" + std::to_string(value.scrollbackLimit) +
			",\"dirty_rows\":" + std::to_string(value.dirtyRows) +
			",\"columns\":" + std::to_string(value.columns) +
			",\"rows\":" + std::to_string(value.rows) +
			",\"protocol_pending\":" + Boolean(value.protocolInputPending) +
			",\"protocol_rejected\":" + Boolean(value.protocolInputRejected) +
			",\"synchronized_commit\":" + Boolean(value.synchronizedOutputCommitted) +
			",\"alternate_screen\":" + Boolean(value.alternateScreen));
	}

	void RecordViewport( const TerminalViewportDiagnosticSnapshot& value ) noexcept
	{
		if( !EnsureEnabled() ) return;
		Record("viewport_publish",
			"\"scroll_offset\":" + std::to_string(value.scrollOffset) +
			",\"top_row\":" + std::to_string(value.topRow) +
			",\"total_rows\":" + std::to_string(value.totalRows) +
			",\"visible_rows\":" + std::to_string(value.visibleRows));
	}

private:
	static std::string Boolean( bool value ) { return value ? "true" : "false"; }

	[[nodiscard]] std::uint64_t ElapsedMicros() const noexcept
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - m_startedAt).count());
	}

	[[nodiscard]] std::filesystem::path DefaultDirectory() const
	{
		std::array<wchar_t, 32768> buffer{};
		const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
		if( length == 0 || length >= buffer.size() ) return {};
		return std::filesystem::path(std::wstring_view(buffer.data(), length)) / L"sakura-editor" / L"terminal-traces";
	}

	[[nodiscard]] std::filesystem::path ActivationDirectory() const
	{
		const auto fallback = DefaultDirectory();
		if( fallback.empty() ) return {};
		const auto controlPath = fallback.parent_path() /
			(L"terminal-trace-" + std::to_wstring(::GetCurrentProcessId()) + L".ini");
		std::array<wchar_t, 32768> value{};
		const DWORD length = ::GetPrivateProfileStringW(
			L"trace", L"directory", L"", value.data(), static_cast<DWORD>(value.size()), controlPath.c_str());
		if( length == 0 || length >= value.size() - 1 ) return fallback;
		return std::filesystem::path(std::wstring_view(value.data(), length));
	}

	bool EnsureEnabled() noexcept
	{
		if( m_enabled.load(std::memory_order_acquire) ) return true;
		if( m_activationEvent == nullptr ) return false;
		const auto now = ::GetTickCount64();
		auto next = m_nextActivationProbeMillis.load(std::memory_order_relaxed);
		if( now < next || !m_nextActivationProbeMillis.compare_exchange_strong(next, now + 1000, std::memory_order_relaxed) ) return false;
		if( ::WaitForSingleObject(m_activationEvent, 0) != WAIT_OBJECT_0 ) return false;
		try { Activate(ActivationDirectory()); } catch( ... ) {}
		return m_enabled.load(std::memory_order_acquire);
	}

	void Activate( const std::filesystem::path& directory )
	{
		if( directory.empty() || m_enabled.load(std::memory_order_acquire) ) return;
		const std::lock_guard activationLock(m_activationMutex);
		if( m_enabled.load(std::memory_order_relaxed) ) return;
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if( error ) return;
		const auto identity = std::to_wstring(::GetCurrentProcessId()) + L"-" +
			std::to_wstring(g_diagnosticSessionSequence.fetch_add(1, std::memory_order_relaxed) + 1);
		m_currentPath = directory / (L"terminal-pty-" + identity + L".jsonl");
		m_previousPath = directory / (L"terminal-pty-" + identity + L".previous.jsonl");
		{
			std::ofstream stream(m_currentPath, std::ios::binary | std::ios::trunc);
			if( !stream ) return;
			stream << Header();
			stream.flush();
			if( !stream ) return;
		}
		m_worker = std::thread([this] { WriterLoop(); });
		m_enabled.store(true, std::memory_order_release);
	}

	std::string Header() const
	{
		return "{\"version\":1,\"kind\":\"trace_header\",\"raw_content\":false,\"pid\":" +
			std::to_string(::GetCurrentProcessId()) + ",\"maximum_file_bytes\":" +
			std::to_string(m_maximumFileBytes) +
			",\"session_age_us\":" + std::to_string(ElapsedMicros()) +
			",\"total_read_bytes\":" + std::to_string(m_totalReadBytes.load(std::memory_order_relaxed)) +
			",\"total_read_events\":" + std::to_string(m_totalReadEvents.load(std::memory_order_relaxed)) +
			",\"total_written_bytes\":" + std::to_string(m_totalWrittenBytes.load(std::memory_order_relaxed)) +
			",\"total_protocol_bytes\":" + std::to_string(m_totalProtocolBytes.load(std::memory_order_relaxed)) +
			",\"total_resize_events\":" + std::to_string(m_totalResizeEvents.load(std::memory_order_relaxed)) +
			",\"total_scrollback_appended\":" + std::to_string(m_totalAppended.load(std::memory_order_relaxed)) +
			",\"total_scrollback_evicted\":" + std::to_string(m_totalEvicted.load(std::memory_order_relaxed)) +
			",\"first_eviction_us\":" + std::to_string(m_firstEvictionMicros.load(std::memory_order_relaxed)) +
			",\"last_eviction_us\":" + std::to_string(m_lastEvictionMicros.load(std::memory_order_relaxed)) +
			",\"scrollback_rows\":" + std::to_string(m_lastScrollbackRows.load(std::memory_order_relaxed)) +
			",\"scrollback_limit\":" + std::to_string(m_lastScrollbackLimit.load(std::memory_order_relaxed)) + "}\n";
	}

	void RecordBytes( std::string_view kind, std::span<const std::uint8_t> bytes, std::string fields ) noexcept
	{
		if( !m_enabled.load(std::memory_order_acquire) ) return;
		try {
			const auto digest = Sha256Hex(bytes);
			fields += ",\"bytes\":" + std::to_string(bytes.size()) + ",\"sha256\":\"" + digest + "\"";
			Record(kind, std::move(fields));
		} catch( ... ) {
			m_dropped.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Record( std::string_view kind, std::string fields ) noexcept
	{
		if( !m_enabled.load(std::memory_order_acquire) ) return;
		try {
			const auto sequence = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto elapsed = ElapsedMicros();
			const auto utc = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			const auto dropped = m_dropped.exchange(0, std::memory_order_relaxed);
			std::string line = "{\"version\":1,\"seq\":" + std::to_string(sequence) +
				",\"mono_us\":" + std::to_string(elapsed) + ",\"utc_us\":" + std::to_string(utc) +
				",\"kind\":\"" + std::string(kind) + "\"," +
				std::move(fields) + ",\"dropped_before\":" + std::to_string(dropped) + "}\n";
			{
				const std::lock_guard lock(m_mutex);
				if( m_stop || m_events.size() >= m_maximumQueuedEvents ) {
					m_dropped.fetch_add(dropped + 1, std::memory_order_relaxed);
					return;
				}
				m_events.emplace_back(std::move(line));
			}
			m_available.notify_one();
		} catch( ... ) {
			m_dropped.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void WriterLoop() noexcept
	{
		try {
			std::ofstream stream(m_currentPath, std::ios::binary | std::ios::app);
			std::error_code error;
			std::uintmax_t fileBytes = std::filesystem::file_size(m_currentPath, error);
			if( error ) fileBytes = 0;
			for( ;; ) {
				std::string line;
				{
					std::unique_lock lock(m_mutex);
					m_available.wait(lock, [&] { return m_stop || !m_events.empty(); });
					if( m_events.empty() && m_stop ) break;
					line = std::move(m_events.front());
					m_events.pop_front();
				}
				if( !stream || fileBytes + line.size() > m_maximumFileBytes ) {
					stream.close();
					error.clear();
					std::filesystem::copy_file(m_currentPath, m_previousPath,
						std::filesystem::copy_options::overwrite_existing, error);
					stream.open(m_currentPath, std::ios::binary | std::ios::trunc);
					const auto header = Header();
					stream << header;
					fileBytes = header.size();
				}
				stream << line;
				stream.flush();
				fileBytes += line.size();
			}
		} catch( ... ) {
			// Diagnostics must never change terminal lifecycle or I/O outcomes.
		}
	}

	const std::size_t m_maximumFileBytes;
	const std::size_t m_maximumQueuedEvents;
	const std::chrono::steady_clock::time_point m_startedAt;
	std::filesystem::path m_currentPath;
	std::filesystem::path m_previousPath;
	HANDLE m_activationEvent{};
	std::atomic<bool> m_enabled{ false };
	std::atomic<ULONGLONG> m_nextActivationProbeMillis{ 0 };
	std::atomic<std::uint64_t> m_sequence{ 0 };
	std::atomic<std::uint64_t> m_dropped{ 0 };
	std::atomic<std::uint64_t> m_totalReadBytes{ 0 };
	std::atomic<std::uint64_t> m_totalReadEvents{ 0 };
	std::atomic<std::uint64_t> m_totalWrittenBytes{ 0 };
	std::atomic<std::uint64_t> m_totalProtocolBytes{ 0 };
	std::atomic<std::uint64_t> m_totalResizeEvents{ 0 };
	std::atomic<std::uint64_t> m_totalAppended{ 0 };
	std::atomic<std::uint64_t> m_totalEvicted{ 0 };
	std::atomic<std::uint64_t> m_firstEvictionMicros{ 0 };
	std::atomic<std::uint64_t> m_lastEvictionMicros{ 0 };
	std::atomic<std::size_t> m_lastScrollbackRows{ 0 };
	std::atomic<std::size_t> m_lastScrollbackLimit{ 0 };
	std::mutex m_activationMutex;
	std::mutex m_mutex;
	std::condition_variable m_available;
	std::deque<std::string> m_events;
	bool m_stop{};
	std::thread m_worker;
};

bool IsAllowedTransition( TerminalSessionState from, TerminalSessionState to )
{
	switch( from ) {
	case TerminalSessionState::Idle:
		return to == TerminalSessionState::Starting || to == TerminalSessionState::Closing;
	case TerminalSessionState::Starting:
		return to == TerminalSessionState::Running || to == TerminalSessionState::Closing || to == TerminalSessionState::Failed;
	case TerminalSessionState::Running:
		return to == TerminalSessionState::Closing || to == TerminalSessionState::Exited || to == TerminalSessionState::Failed;
	case TerminalSessionState::Closing:
		return to == TerminalSessionState::Exited || to == TerminalSessionState::Failed;
	case TerminalSessionState::Exited:
	case TerminalSessionState::Failed:
		return false;
	}
	return false;
}

template<typename Callback, typename... Args>
void InvokeNoThrow( const Callback& callback, Args&&... args ) noexcept
{
	if( !callback ) return;
	try {
		callback( std::forward<Args>(args)... );
	} catch( ... ) {
		// A UI notification must never terminate an I/O worker.
	}
}

} // namespace

TerminalDiagnosticOptions TerminalDiagnosticOptions::FromEnvironment() noexcept
{
	TerminalDiagnosticOptions options;
	try {
		const DWORD required = ::GetEnvironmentVariableW(L"SAKURA_TERMINAL_TRACE_DIR", nullptr, 0);
		if( required <= 1 ) return options;
		std::wstring value(required, L'\0');
		const DWORD written = ::GetEnvironmentVariableW(
			L"SAKURA_TERMINAL_TRACE_DIR", value.data(), static_cast<DWORD>(value.size()));
		if( written == 0 || written >= value.size() ) return options;
		value.resize(written);
		options.directory = std::move(value);
	} catch( ... ) {
		options.directory.clear();
	}
	return options;
}

TerminalStartResult TerminalStartResult::Success()
{
	return { true, 0, {} };
}

TerminalStartResult TerminalStartResult::Failure( std::uint32_t errorCode, std::wstring diagnostic )
{
	return { false, errorCode, std::move(diagnostic) };
}

TerminalStartResult TerminalStartResult::Aborted()
{
	return Failure(ERROR_CANCELLED, L"Terminal start was cancelled by a close request.");
}

struct CTerminalSession::Impl : std::enable_shared_from_this<CTerminalSession::Impl> {
	struct SharedState {
		explicit SharedState( TerminalDiagnosticOptions diagnosticOptions ) noexcept
			: diagnostics(std::move(diagnosticOptions))
		{
		}

		mutable std::mutex stateMutex;
		TerminalSessionState state = TerminalSessionState::Idle;
		std::uint32_t lastError = 0;
		TerminalDiagnosticRecorder diagnostics;

		mutable std::mutex callbackMutex;
		TerminalSessionCallbacks callbacks;
		mutable std::mutex workerIdentityMutex;
		std::thread::id readerWorkerId{};
		std::thread::id writerWorkerId{};

		mutable std::mutex outputMutex;
		std::condition_variable outputSpaceAvailable;
		std::deque<ByteChunk> output;
		std::size_t outputBytes = 0;
		bool outputNotificationPending = false;

		mutable std::mutex inputMutex;
		std::condition_variable inputAvailable;
		std::deque<ByteChunk> input;
		std::size_t inputBytes = 0;
		std::optional<TerminalSize> pendingResize;
		std::size_t pendingResizeRequests = 0;
		std::chrono::steady_clock::time_point resizeDue{};

		std::atomic<bool> stopRequested{ false };
		// Set before the close worker is constructed so Start can observe a close
		// requested by its synchronous Starting callback without racing that worker.
		std::atomic<bool> closeRequested{ false };
		std::atomic<bool> acceptingInput{ false };
		std::atomic<bool> backendStarted{ false };
		std::atomic<bool> startedSuccessfully{ false };
		std::atomic<bool> backendClosed{ false };
		// Reader and writer failures can wake the peer by closing the backend.
		// Exactly one worker must own the terminal outcome so the peer cannot
		// replace a real failure with the close-induced EOF it observes later.
		std::atomic<bool> workerFinalizationClaimed{ false };

		mutable std::mutex completionMutex;
		std::optional<TerminalSessionCompletionResult> completion;
		bool completionDelivered = false;
	};

	// A thread-local linked stack makes callback-origin self-wait detection both
	// allocation-free and correct for nested callbacks belonging to different
	// sessions.  This is deliberately independent of SharedState lifetime.
	struct CallbackFrame {
		const SharedState* state;
		CallbackFrame* previous;

		explicit CallbackFrame( const SharedState* current ) noexcept
			: state(current), previous(ActiveCallbackFrame())
		{
			ActiveCallbackFrame() = this;
		}
		~CallbackFrame() noexcept { ActiveCallbackFrame() = previous; }

		static CallbackFrame*& ActiveCallbackFrame() noexcept
		{
			static thread_local CallbackFrame* frame = nullptr;
			return frame;
		}
	};

	explicit Impl(
		std::unique_ptr<ITerminalBackend> backendValue,
		TerminalSessionCallbacks callbackValue,
		TerminalDiagnosticOptions diagnosticOptions )
		: backend(std::move(backendValue)), shared(std::make_shared<SharedState>(std::move(diagnosticOptions)))
	{
		shared->callbacks = std::move(callbackValue);
		if( auto reservation = TerminalWorkerRetirementService::Instance().TryReserve() ) {
			closeWorkerRetirement.emplace(std::move(*reservation));
		}
	}

	std::shared_ptr<ITerminalBackend> backend;
	std::shared_ptr<SharedState> shared;
	std::thread reader;
	std::thread writer;
	std::thread closeWorker;
	std::mutex lifecycleMutex;
	std::mutex closeMutex;
	std::condition_variable closeCompleted;
	bool closeStarted = false;
	bool closeFinished = false;
	std::optional<TerminalWorkerRetirementService::Reservation> closeWorkerRetirement;
	std::chrono::steady_clock::time_point closeFinishedAt{};
	std::thread::id closeWorkerId{};

	~Impl() noexcept
	{
		// A live worker always owns a shared Impl.  Normal external destruction
		// therefore reaches BeginClose first and the lifecycle worker joins the I/O
		// workers before this object is released.  Keep this final assertion strict:
		// silently detaching here would release ownership while native terminal work
		// is still live.  Callback-origin destruction transfers the close-worker
		// handle to TerminalWorkerRetirementService before the last shared owner is
		// released, so these handles must already be non-joinable.
		if( reader.joinable() || writer.joinable() || closeWorker.joinable() ) std::terminate();
	}

	static TerminalSessionState StateOf( const std::shared_ptr<SharedState>& state ) noexcept
	{
		const std::lock_guard lock(state->stateMutex);
		return state->state;
	}

	TerminalSessionState State() const noexcept
	{
		return StateOf(shared);
	}

	static bool TransitionState( const std::shared_ptr<SharedState>& state, TerminalSessionState next, std::uint32_t errorCode = 0 ) noexcept
	{
		TerminalSessionCallbacks callbacks;
		{
			const std::lock_guard lock(state->stateMutex);
			if( state->state == next ) return true;
			if( !IsAllowedTransition(state->state, next) ) return false;
			state->state = next;
			state->lastError = errorCode;
		}
		state->diagnostics.RecordState(next, errorCode);
		try {
			const std::lock_guard lock(state->callbackMutex);
			callbacks = state->callbacks;
		} catch( ... ) {}
		InvokeCallback(state, callbacks.stateChanged, next, errorCode);
		return true;
	}

	bool Transition( TerminalSessionState next, std::uint32_t errorCode = 0 ) noexcept
	{
		return TransitionState(shared, next, errorCode);
	}

	static void NotifyOutputState( const std::shared_ptr<SharedState>& state ) noexcept
	{
		std::function<void()> callback;
		try {
			const std::lock_guard lock(state->callbackMutex);
			callback = state->callbacks.outputAvailable;
		} catch( ... ) {}
		InvokeCallback(state, callback);
	}

	template<typename Callback, typename... Args>
	static void InvokeCallback(const std::shared_ptr<SharedState>& state, const Callback& callback, Args&&... args) noexcept
	{
		if( !callback ) return;
		CallbackFrame frame(state.get());
		InvokeNoThrow(callback, std::forward<Args>(args)...);
	}

	void NotifyOutput() noexcept { NotifyOutputState(shared); }

	static void StopWorkersState( const std::shared_ptr<SharedState>& state ) noexcept
	{
		state->stopRequested.store(true, std::memory_order_release);
		state->acceptingInput.store(false, std::memory_order_release);
		state->outputSpaceAvailable.notify_all();
		state->inputAvailable.notify_all();
	}

	void StopWorkers() noexcept { StopWorkersState(shared); }

	static void CloseBackendOnceState( const std::shared_ptr<SharedState>& state, const std::shared_ptr<ITerminalBackend>& terminalBackend ) noexcept
	{
		bool expected = false;
		if( state->backendClosed.compare_exchange_strong(expected, true) ) {
			try {
				terminalBackend->Close();
			} catch( ... ) {
				// Close is specified noexcept, but keep finalization safe even for a
				// faulty test or third-party backend.
			}
		}
	}

	void CloseBackendOnce() noexcept { CloseBackendOnceState(shared, backend); }

	static void ClaimCompletion( const std::shared_ptr<SharedState>& state, TerminalSessionCompletionResult result ) noexcept
	{
		try {
			const std::lock_guard lock(state->completionMutex);
			if( !state->completion ) state->completion = result;
		} catch( ... ) {
			// Completion delivery is best effort only when memory/locking is already
			// compromised; lifecycle ownership still reaches its close terminal state.
		}
	}

	static void RecordObservedExitCode( const std::shared_ptr<SharedState>& state, std::uint32_t exitCode ) noexcept
	{
		try {
			const std::lock_guard lock(state->completionMutex);
			if( state->completion && (state->completion->kind == TerminalSessionCompletionKind::Exited || state->completion->kind == TerminalSessionCompletionKind::Closed) ) {
				state->completion->exitCode = exitCode;
			}
		} catch( ... ) {}
	}

	static TerminalSessionCompletionResult CompletionOf( const std::shared_ptr<SharedState>& state ) noexcept
	{
		try {
			const std::lock_guard lock(state->completionMutex);
			return state->completion.value_or(TerminalSessionCompletionResult{});
		} catch( ... ) {
			return {};
		}
	}

	static void FinalizeFromWorker( const std::shared_ptr<Impl>& self, const std::shared_ptr<SharedState>& state, const std::shared_ptr<ITerminalBackend>& terminalBackend,
		TerminalSessionState terminalState, std::uint32_t errorCode = 0, std::uint32_t exitCode = 0 ) noexcept
	{
		bool expected = false;
		if( !state->workerFinalizationClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel) ) return;
		ClaimCompletion(state, { terminalState == TerminalSessionState::Failed ? TerminalSessionCompletionKind::Failed : TerminalSessionCompletionKind::Exited, exitCode, errorCode });
		if( StateOf(state) == TerminalSessionState::Running ) TransitionState(state, TerminalSessionState::Closing);
		StopWorkersState(state);
		if( terminalState == TerminalSessionState::Failed ) {
			try { terminalBackend->ForceTerminate(); } catch( ... ) {}
		}
		TransitionState(state, terminalState, errorCode);
		// State change remains the early UI signal.  The durable completion is
		// delivered by the lifecycle worker only after all ownership quiesces.
		self->BeginClose();
	}

	static void CancelSynchronousIoNoThrow( std::thread& worker ) noexcept
	{
		if( !worker.joinable() ) return;
		const auto handle = reinterpret_cast<HANDLE>(worker.native_handle());
		::CancelSynchronousIo(handle);
	}

	static void JoinUntilQuiescent( std::thread& worker ) noexcept
	{
		if( !worker.joinable() ) return;
		const auto handle = reinterpret_cast<HANDLE>(worker.native_handle());
		::CancelSynchronousIo(handle);
		// A close result must never outlive an I/O worker.  A hostile backend may
		// ignore cancellation, in which case retaining ownership and waiting is the
		// only truthful outcome; detaching would make a later task adapter claim
		// quiescence while backend work still exists.
		worker.join();
	}

	static void ReaderLoop( std::shared_ptr<Impl> self, std::shared_ptr<SharedState> state, std::shared_ptr<ITerminalBackend> terminalBackend ) noexcept
	{
		std::array<std::uint8_t, kBackendReadBytes> buffer{};
		try {
			while( !state->stopRequested.load(std::memory_order_acquire) ) {
				std::size_t capacity = 0;
				{
					std::unique_lock lock(state->outputMutex);
					if( state->outputBytes >= CTerminalSession::kOutputHighWaterBytes ) {
						state->outputSpaceAvailable.wait( lock, [&] {
							return state->stopRequested.load(std::memory_order_acquire) ||
								state->outputBytes <= CTerminalSession::kOutputLowWaterBytes;
						} );
					}
					if( state->stopRequested.load(std::memory_order_acquire) ) break;
					capacity = std::min( buffer.size(), CTerminalSession::kOutputHighWaterBytes - state->outputBytes );
				}

				TerminalBackendReadResult result;
				try {
					result = terminalBackend->ReadOutput( std::span<std::uint8_t>(buffer.data(), capacity), kBackendReadTimeout );
				} catch( ... ) {
					result = { TerminalBackendReadStatus::Failed, 0, ERROR_UNHANDLED_EXCEPTION };
				}

				if( result.status == TerminalBackendReadStatus::Timeout ) continue;
				if( result.status == TerminalBackendReadStatus::EndOfFile ) {
					if( state->stopRequested.load(std::memory_order_acquire) ) return;
					// EOF merely closes the ConPTY output channel.  The root process may
					// still be alive, so observe its exit (and job descendants) before
					// publishing a natural terminal outcome.
					for( ;; ) {
						TerminalBackendExitResult exit;
						try { exit = terminalBackend->WaitForExit(kBackendReadTimeout); }
						catch( ... ) { exit = { TerminalBackendExitStatus::Failed, 0, ERROR_UNHANDLED_EXCEPTION }; }
						if( exit.status == TerminalBackendExitStatus::TimedOut ) {
							if( state->stopRequested.load(std::memory_order_acquire) ) return;
							continue;
						}
						if( exit.status == TerminalBackendExitStatus::Exited ) FinalizeFromWorker( self, state, terminalBackend, TerminalSessionState::Exited, 0, exit.exitCode );
						else FinalizeFromWorker( self, state, terminalBackend, TerminalSessionState::Failed, exit.errorCode == 0 ? ERROR_GEN_FAILURE : exit.errorCode );
						return;
					}
				}
				if( result.status == TerminalBackendReadStatus::Failed ) {
					const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
					FinalizeFromWorker( self, state, terminalBackend, terminalState, result.errorCode );
					return;
				}
				if( result.bytesTransferred == 0 || result.bytesTransferred > capacity ) {
					FinalizeFromWorker( self, state, terminalBackend, TerminalSessionState::Failed, ERROR_INVALID_DATA );
					return;
				}

				bool notify = false;
				std::size_t queueBefore = 0;
				std::size_t queueAfter = 0;
				{
					const std::lock_guard lock(state->outputMutex);
					queueBefore = state->outputBytes;
					ByteChunk chunk;
					chunk.bytes.assign( buffer.begin(), buffer.begin() + result.bytesTransferred );
					state->outputBytes += chunk.bytes.size();
					queueAfter = state->outputBytes;
					state->output.emplace_back(std::move(chunk));
					if( !state->outputNotificationPending ) {
						state->outputNotificationPending = true;
						notify = true;
					}
				}
				state->diagnostics.RecordRead(
					std::span<const std::uint8_t>(buffer.data(), result.bytesTransferred), queueBefore, queueAfter);
				if( notify ) NotifyOutputState(state);
			}
		} catch( const std::bad_alloc& ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( self, state, terminalBackend, terminalState, ERROR_NOT_ENOUGH_MEMORY );
		} catch( ... ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( self, state, terminalBackend, terminalState, ERROR_UNHANDLED_EXCEPTION );
		}
	}

	static void WriterLoop( std::shared_ptr<Impl> self, std::shared_ptr<SharedState> state, std::shared_ptr<ITerminalBackend> terminalBackend ) noexcept
	{
		try {
			while( !state->stopRequested.load(std::memory_order_acquire) ) {
				std::vector<std::uint8_t> bytes;
				std::optional<TerminalSize> resize;
				TerminalInputSource inputSource = TerminalInputSource::Interactive;
				std::size_t coalescedResizeRequests = 0;
				{
					std::unique_lock lock(state->inputMutex);
					for( ;; ) {
						if( state->stopRequested.load(std::memory_order_acquire) ) return;
						const auto now = std::chrono::steady_clock::now();
						if( state->pendingResize && now >= state->resizeDue ) {
							resize = state->pendingResize;
							coalescedResizeRequests = state->pendingResizeRequests;
							state->pendingResize.reset();
							state->pendingResizeRequests = 0;
							break;
						}
						if( !state->input.empty() ) {
							const auto& front = state->input.front();
							const auto count = std::min( kBackendWriteBytes, front.bytes.size() - front.offset );
							bytes.assign( front.bytes.begin() + front.offset, front.bytes.begin() + front.offset + count );
							inputSource = front.source;
							break;
						}
						if( state->pendingResize ) state->inputAvailable.wait_until(lock, state->resizeDue);
						else state->inputAvailable.wait(lock);
					}
				}

				if( !state->acceptingInput.load(std::memory_order_acquire) ) return;
				if( resize ) {
					TerminalBackendOperationResult result;
					try {
						result = terminalBackend->Resize(*resize);
					} catch( ... ) {
						result = { false, ERROR_UNHANDLED_EXCEPTION };
					}
					if( !result.succeeded ) {
						const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
						FinalizeFromWorker( self, state, terminalBackend, terminalState, result.errorCode );
						return;
					}
					state->diagnostics.RecordResize("resize_apply", *resize, coalescedResizeRequests);
					continue;
				}

				TerminalBackendWriteResult result;
				try {
					result = terminalBackend->WriteInput(bytes);
				} catch( ... ) {
					result = { TerminalBackendWriteStatus::Failed, 0, ERROR_UNHANDLED_EXCEPTION };
				}
				if( result.status != TerminalBackendWriteStatus::Completed || result.bytesTransferred == 0 || result.bytesTransferred > bytes.size() ) {
					const auto error = result.errorCode == 0 ? static_cast<std::uint32_t>(ERROR_BROKEN_PIPE) : result.errorCode;
					const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
					FinalizeFromWorker( self, state, terminalBackend, terminalState, error );
					return;
				}

				std::size_t queueAfter = 0;
				{
					const std::lock_guard lock(state->inputMutex);
					if( state->input.empty() ) continue;
					auto& front = state->input.front();
					front.offset += result.bytesTransferred;
					state->inputBytes -= result.bytesTransferred;
					queueAfter = state->inputBytes;
					if( front.offset == front.bytes.size() ) state->input.pop_front();
				}
				state->diagnostics.RecordWrite(
					std::span<const std::uint8_t>(bytes.data(), result.bytesTransferred), inputSource, queueAfter);
			}
		} catch( const std::bad_alloc& ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( self, state, terminalBackend, terminalState, ERROR_NOT_ENOUGH_MEMORY );
		} catch( ... ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( self, state, terminalBackend, terminalState, ERROR_UNHANDLED_EXCEPTION );
		}
	}

	void CloseImpl() noexcept
	{
		const std::lock_guard lifecycleLock(lifecycleMutex);
		const auto originalState = State();
		if( originalState == TerminalSessionState::Idle || originalState == TerminalSessionState::Starting || originalState == TerminalSessionState::Running ) {
			Transition(TerminalSessionState::Closing);
		}
		shared->acceptingInput.store(false, std::memory_order_release);
		{
			const std::lock_guard lock(shared->inputMutex);
			shared->input.clear();
			shared->inputBytes = 0;
			shared->pendingResize.reset();
			shared->pendingResizeRequests = 0;
		}

		const auto completion = CompletionOf(shared);
		if( shared->backendStarted.load(std::memory_order_acquire) && !shared->backendClosed.load(std::memory_order_acquire)
			&& completion.kind == TerminalSessionCompletionKind::Closed ) {
			try { backend->RequestGracefulClose(); } catch( ... ) {}
			TerminalBackendExitResult exit;
			try { exit = backend->WaitForExit(CTerminalSession::kGracefulCloseTimeout); }
			catch( ... ) { exit = { TerminalBackendExitStatus::Failed, 0, ERROR_UNHANDLED_EXCEPTION }; }
			if( exit.status == TerminalBackendExitStatus::TimedOut ) {
				try { backend->ForceTerminate(); } catch( ... ) {}
				try { exit = backend->WaitForExit(CTerminalSession::kForcedCloseTimeout); }
				catch( ... ) { exit = { TerminalBackendExitStatus::Failed, 0, ERROR_UNHANDLED_EXCEPTION }; }
			}
			if( exit.status == TerminalBackendExitStatus::Exited ) RecordObservedExitCode(shared, exit.exitCode);
		}

		StopWorkers();
		CloseBackendOnce();
		CancelSynchronousIoNoThrow(reader);
		CancelSynchronousIoNoThrow(writer);
		JoinUntilQuiescent(reader);
		JoinUntilQuiescent(writer);
		if( State() == TerminalSessionState::Closing ) {
			Transition(TerminalSessionState::Exited);
		}
		shared->diagnostics.Stop();
	}

	void DeliverCompletion() noexcept
	{
		if( !shared->startedSuccessfully.load(std::memory_order_acquire) ) return;
		TerminalSessionCallbacks callbacks;
		TerminalSessionCompletionResult result;
		try {
			{
				const std::lock_guard completionLock(shared->completionMutex);
				if( shared->completionDelivered ) return;
				if( !shared->completion ) shared->completion = TerminalSessionCompletionResult{};
				result = *shared->completion;
				shared->completionDelivered = true;
			}
			{
				const std::lock_guard callbackLock(shared->callbackMutex);
				callbacks = shared->callbacks;
				shared->callbacks = {};
			}
		} catch( ... ) {
			return;
		}
		InvokeCallback(shared, callbacks.completed, result);
	}

	void RunCloseWorkerBody() noexcept
	{
		{
			const std::lock_guard workerLock(shared->workerIdentityMutex);
			closeWorkerId = std::this_thread::get_id();
		}
		CloseImpl();
		{
			const std::lock_guard closeLock(closeMutex);
			closeFinished = true;
			closeFinishedAt = std::chrono::steady_clock::now();
		}
		closeCompleted.notify_all();
		// `closeFinished` is observable before this callback. This lets an
		// external waiter take ownership without a completion callback ever
		// racing live reader/writer or backend work.
		DeliverCompletion();
	}

	void RetireCloseTask(
		const std::shared_ptr<Impl>& self,
		TerminalWorkerRetirementService::Reservation&& reservation ) noexcept
	{
		std::function<void()> task = [self] { self->RunCloseWorkerBody(); };
		const auto status = TerminalWorkerRetirementService::Instance().RetireTask(
			std::move(reservation), std::move(task));
		if( status != TerminalWorkerRetirementStatus::Retired ) std::terminate();
	}

	void RetireCloseWorker( const std::shared_ptr<Impl>& self ) noexcept
	{
		std::thread worker;
		std::optional<TerminalWorkerRetirementService::Reservation> reservation;
		{
			const std::lock_guard lock(closeMutex);
			// An external waiter may have taken the handle after closeFinished was
			// published.  In that case it owns the join and the pre-admission slot is
			// no longer needed.
			if( !closeWorker.joinable() ) {
				closeWorkerRetirement.reset();
				return;
			}
			if( !closeWorkerRetirement ) std::terminate();
			worker = std::move(closeWorker);
			reservation = std::move(closeWorkerRetirement);
		}

		const auto status = TerminalWorkerRetirementService::Instance().Retire(
			std::move(worker), std::move(*reservation), std::shared_ptr<void>(self));
		if( status != TerminalWorkerRetirementStatus::Retired ) std::terminate();
	}

	void BeginClose() noexcept
	{
		shared->closeRequested.store(true, std::memory_order_release);
		std::lock_guard lock(closeMutex);
			if( closeStarted ) return;
			closeStarted = true;
			if( !closeWorkerRetirement ) {
				// An idle/failed session has no workers to retire.  Complete that
				// terminal state synchronously; Start() rejects live admission without
				// a slot, so reaching this branch for a running session is an internal
				// ownership breach rather than a condition under which it is safe to
				// wait or detach.
				const auto state = State();
				if( state == TerminalSessionState::Idle ) {
					Transition(TerminalSessionState::Closing);
					Transition(TerminalSessionState::Exited);
				}
				if( state == TerminalSessionState::Idle || state == TerminalSessionState::Failed
					|| state == TerminalSessionState::Exited ) {
					closeFinished = true;
					closeFinishedAt = std::chrono::steady_clock::now();
					closeCompleted.notify_all();
					return;
				}
				std::terminate();
			}
			const auto self = shared_from_this();
			std::optional<TerminalWorkerRetirementService::Reservation> fallbackReservation;
			try {
				closeWorker = std::thread([self] {
					self->RunCloseWorkerBody();
					self->RetireCloseWorker(self);
				});
			}
			catch( ... ) {
				// If a close thread cannot be constructed, execute the same body on a
				// fixed reaper thread. The reservation was admitted before Start, so
				// this fallback remains nonblocking for the UI and never detaches.
				if( !closeWorkerRetirement ) std::terminate();
				fallbackReservation = std::move(closeWorkerRetirement);
			}
		if( fallbackReservation ) RetireCloseTask(self, std::move(*fallbackReservation));
	}

	void RequestClose() noexcept
	{
		if( shared->startedSuccessfully.load(std::memory_order_acquire) ) ClaimCompletion(shared, {});
		BeginClose();
	}

	[[nodiscard]] bool IsWorkerThread() const noexcept
	{
		const auto current = std::this_thread::get_id();
		for( auto* frame = CallbackFrame::ActiveCallbackFrame(); frame != nullptr; frame = frame->previous ) {
			if( frame->state == shared.get() ) return true;
		}
		const std::lock_guard workerLock(shared->workerIdentityMutex);
		return current == shared->readerWorkerId || current == shared->writerWorkerId || current == closeWorkerId;
	}

	[[nodiscard]] TerminalSessionCloseResult WaitForClose( const std::chrono::steady_clock::time_point deadline ) noexcept
	{
		RequestClose();
		if( IsWorkerThread() ) return { TerminalSessionCloseWaitStatus::InProgress };

		std::thread completedWorker;
		bool exceededDeadline = false;
		try {
			std::unique_lock lock(closeMutex);
			if( !closeCompleted.wait_until(lock, deadline, [this] { return closeFinished; }) ) {
				exceededDeadline = true;
				// The deadline is an escalation/reporting boundary, not permission
				// to release a live backend or worker.
				closeCompleted.wait(lock, [this] { return closeFinished; });
			}
			if( closeFinishedAt > deadline ) exceededDeadline = true;
			if( closeWorker.joinable() ) {
				completedWorker = std::move(closeWorker);
				// The external waiter now owns the only joinable handle.  Release the
				// reserved reaper slot so a later callback cannot attempt a second
				// handoff.
				closeWorkerRetirement.reset();
			}
		}
		catch( ... ) {
			// CloseImpl is noexcept and ownership remains in this object.  Retry the
			// wait on a later external call rather than reporting false quiescence.
			return { TerminalSessionCloseWaitStatus::InProgress };
		}
		if( completedWorker.joinable() ) completedWorker.join();
		return { exceededDeadline ? TerminalSessionCloseWaitStatus::DeadlineExceeded : TerminalSessionCloseWaitStatus::Closed };
	}
};

CTerminalSession::CTerminalSession(
	std::unique_ptr<ITerminalBackend> backend,
	TerminalSessionCallbacks callbacks,
	TerminalDiagnosticOptions diagnostics )
	: m_impl(std::make_shared<Impl>(std::move(backend), std::move(callbacks), std::move(diagnostics)))
{
	if( !m_impl->backend ) throw std::invalid_argument("CTerminalSession requires a backend");
}

CTerminalSession::~CTerminalSession()
{
	// Destruction may run on a workbench/UI owner.  BeginClose only publishes the
	// close request; the pre-admitted lifecycle worker and its fixed reaper owner
	// perform all backend and worker joins without blocking this thread.
	BeginClose();
}

TerminalStartResult CTerminalSession::Start( const TerminalLaunchOptions& options )
{
	const auto impl = m_impl;
	if( !impl ) return TerminalStartResult::Aborted();
	std::unique_lock lifecycleLock(impl->lifecycleMutex);
	if( impl->State() != TerminalSessionState::Idle ) return TerminalStartResult::Failure( ERROR_INVALID_STATE, L"Terminal session has already been started or closed." );
	impl->Transition(TerminalSessionState::Starting);
	if( impl->shared->closeRequested.load(std::memory_order_acquire) ) {
		impl->Transition(TerminalSessionState::Closing);
		return TerminalStartResult::Aborted();
	}
	if( options.executablePath.empty() || options.initialSize.columns == 0 || options.initialSize.rows == 0 ) {
		impl->Transition( TerminalSessionState::Failed, ERROR_INVALID_PARAMETER );
		impl->CloseBackendOnce();
		return TerminalStartResult::Failure( ERROR_INVALID_PARAMETER, L"Terminal launch options are invalid." );
	}
	if( !impl->closeWorkerRetirement ) {
		// A started session must have a nonblocking destruction owner.  Refuse
		// admission when the fixed retirement capacity is exhausted rather than
		// creating a worker that would later need to detach or block the UI.
		impl->Transition( TerminalSessionState::Failed, ERROR_BUSY );
		impl->CloseBackendOnce();
		return TerminalStartResult::Failure( ERROR_BUSY, L"Terminal retirement capacity is exhausted." );
	}

	TerminalStartResult result;
	try {
		result = impl->backend->Start(options);
	} catch( ... ) {
		result = TerminalStartResult::Failure( ERROR_UNHANDLED_EXCEPTION, L"Terminal backend initialization raised an exception." );
	}
	if( !result.succeeded ) {
		impl->Transition( TerminalSessionState::Failed, result.errorCode );
		impl->CloseBackendOnce();
		return result;
	}
	impl->shared->backendStarted.store(true, std::memory_order_release);
	impl->shared->startedSuccessfully.store(true, std::memory_order_release);
	impl->Transition(TerminalSessionState::Running);
	if( impl->shared->closeRequested.load(std::memory_order_acquire) ) {
		impl->Transition(TerminalSessionState::Closing);
		return TerminalStartResult::Aborted();
	}
	impl->shared->acceptingInput.store(true, std::memory_order_release);

	try {
		const auto state = impl->shared;
		const auto backend = impl->backend;
		impl->reader = std::thread( [impl, state, backend] {
			{
				const std::lock_guard workerLock(state->workerIdentityMutex);
				state->readerWorkerId = std::this_thread::get_id();
			}
			Impl::ReaderLoop(impl, state, backend);
		} );
		impl->writer = std::thread( [impl, state, backend] {
			{
				const std::lock_guard workerLock(state->workerIdentityMutex);
				state->writerWorkerId = std::this_thread::get_id();
			}
			Impl::WriterLoop(impl, state, backend);
		} );
		// The reader can invoke an output callback immediately after construction.
		// If that callback requests close (or destroys the outer session), do not
		// report a successful start after the just-created workers are cancelled.
		if( impl->shared->closeRequested.load(std::memory_order_acquire) ) {
			impl->Transition(TerminalSessionState::Closing);
			return TerminalStartResult::Aborted();
		}
	} catch( const std::system_error& error ) {
		impl->Transition( TerminalSessionState::Failed, static_cast<std::uint32_t>(error.code().value()) );
		impl->StopWorkers();
		impl->CloseBackendOnce();
		Impl::CancelSynchronousIoNoThrow(impl->reader);
		Impl::JoinUntilQuiescent(impl->reader);
		return TerminalStartResult::Failure( static_cast<std::uint32_t>(error.code().value()), L"Unable to create terminal I/O workers." );
	}
	return result;
}

void CTerminalSession::Close() noexcept
{
	const auto impl = m_impl;
	if( !impl ) return;
	impl->RequestClose();
	(void)impl->WaitForClose(std::chrono::steady_clock::time_point::max());
}

void CTerminalSession::BeginClose() noexcept
{
	const auto impl = m_impl;
	if( impl ) impl->RequestClose();
}

TerminalSessionCloseResult CTerminalSession::WaitForClose( const std::chrono::steady_clock::time_point deadline ) noexcept
{
	const auto impl = m_impl;
	return impl ? impl->WaitForClose(deadline) : TerminalSessionCloseResult{};
}

std::optional<TerminalBackendProcessIdentity> CTerminalSession::GetProcessIdentity() const noexcept
{
	const auto impl = m_impl;
	if( !impl || !impl->backend ) return std::nullopt;
	return impl->backend->GetProcessIdentity();
}

bool CTerminalSession::OwnsProcess(
	const std::uint32_t processId, const std::uint64_t creationTime ) const noexcept
{
	const auto impl = m_impl;
	return impl && impl->backend && impl->backend->OwnsProcess(processId, creationTime);
}

TerminalQueueInputResult CTerminalSession::QueueInput(
	std::span<const std::uint8_t> bytes,
	TerminalInputSource source )
{
	if( !m_impl->shared->acceptingInput.load(std::memory_order_acquire) || m_impl->State() != TerminalSessionState::Running ) return TerminalQueueInputResult::NotRunning;
	if( bytes.empty() ) return TerminalQueueInputResult::Accepted;
	{
		const std::lock_guard lock(m_impl->shared->inputMutex);
		// Close/failure disables input before taking this mutex. Recheck after
		// acquisition so a producer cannot enqueue behind the final queue clear.
		if( !m_impl->shared->acceptingInput.load(std::memory_order_acquire) ) return TerminalQueueInputResult::NotRunning;
		if( bytes.size() > kInputLimitBytes - m_impl->shared->inputBytes ) return TerminalQueueInputResult::QueueFull;
		ByteChunk chunk;
		chunk.bytes.assign(bytes.begin(), bytes.end());
		chunk.source = source;
		m_impl->shared->inputBytes += chunk.bytes.size();
		m_impl->shared->input.emplace_back(std::move(chunk));
	}
	m_impl->shared->inputAvailable.notify_one();
	return TerminalQueueInputResult::Accepted;
}

bool CTerminalSession::RequestResize( TerminalSize size )
{
	if( size.columns == 0 || size.rows == 0 || !m_impl->shared->acceptingInput.load(std::memory_order_acquire)
		|| m_impl->State() != TerminalSessionState::Running ) return false;
	std::size_t pendingRequests = 0;
	{
		const std::lock_guard lock(m_impl->shared->inputMutex);
		if( !m_impl->shared->acceptingInput.load(std::memory_order_acquire) ) return false;
		m_impl->shared->pendingResize = size;
		++m_impl->shared->pendingResizeRequests;
		pendingRequests = m_impl->shared->pendingResizeRequests;
		m_impl->shared->resizeDue = std::chrono::steady_clock::now() + kResizeCoalesceDelay;
	}
	m_impl->shared->diagnostics.RecordResize("resize_request", size, pendingRequests);
	m_impl->shared->inputAvailable.notify_one();
	return true;
}

std::vector<std::uint8_t> CTerminalSession::DrainOutput()
{
	const auto impl = m_impl;
	if( !impl ) return {};
	std::vector<std::uint8_t> result;
	result.reserve(kMaximumDrainBytes);
	bool renotify = false;
	const auto started = std::chrono::steady_clock::now();
	const auto deadline = started + kMaximumDrainTime;
	std::size_t queueBefore = 0;
	std::size_t queueAfter = 0;
	{
		const std::lock_guard lock(impl->shared->outputMutex);
		queueBefore = impl->shared->outputBytes;
		impl->shared->outputNotificationPending = false;
		while( !impl->shared->output.empty() && result.size() < kMaximumDrainBytes && std::chrono::steady_clock::now() < deadline ) {
			auto& front = impl->shared->output.front();
			const auto count = std::min( kMaximumDrainBytes - result.size(), front.bytes.size() - front.offset );
			result.insert( result.end(), front.bytes.begin() + front.offset, front.bytes.begin() + front.offset + count );
			front.offset += count;
			impl->shared->outputBytes -= count;
			if( front.offset == front.bytes.size() ) impl->shared->output.pop_front();
		}
		if( !impl->shared->output.empty() ) {
			impl->shared->outputNotificationPending = true;
			renotify = true;
		}
		queueAfter = impl->shared->outputBytes;
	}
	impl->shared->diagnostics.RecordDrain(result, queueBefore, queueAfter,
		static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - started).count()));
	if( GetQueuedOutputBytes() <= kOutputLowWaterBytes ) impl->shared->outputSpaceAvailable.notify_one();
	if( renotify ) impl->NotifyOutput();
	return result;
}

void CTerminalSession::RecordModelDiagnostic( const TerminalModelDiagnosticSnapshot& snapshot ) noexcept
{
	if( m_impl ) m_impl->shared->diagnostics.RecordModel(snapshot);
}

void CTerminalSession::RecordViewportDiagnostic( const TerminalViewportDiagnosticSnapshot& snapshot ) noexcept
{
	if( m_impl ) m_impl->shared->diagnostics.RecordViewport(snapshot);
}

std::wstring CTerminalSession::GetDiagnosticTracePath() const
{
	return m_impl ? m_impl->shared->diagnostics.Path() : std::wstring{};
}

TerminalSessionState CTerminalSession::GetState() const noexcept
{
	return m_impl->State();
}

std::uint32_t CTerminalSession::GetLastError() const noexcept
{
	const std::lock_guard lock(m_impl->shared->stateMutex);
	return m_impl->shared->lastError;
}

std::size_t CTerminalSession::GetQueuedOutputBytes() const noexcept
{
	const std::lock_guard lock(m_impl->shared->outputMutex);
	return m_impl->shared->outputBytes;
}

std::size_t CTerminalSession::GetQueuedInputBytes() const noexcept
{
	const std::lock_guard lock(m_impl->shared->inputMutex);
	return m_impl->shared->inputBytes;
}

bool CTerminalSession::IsOutputNotificationPending() const noexcept
{
	const std::lock_guard lock(m_impl->shared->outputMutex);
	return m_impl->shared->outputNotificationPending;
}

} // namespace terminal
