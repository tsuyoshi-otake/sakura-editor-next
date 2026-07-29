/*! @file */
#include "StdAfx.h"
#include "terminal/session/TerminalSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
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
constexpr auto kWorkerJoinTimeout = std::chrono::milliseconds(500);

struct ByteChunk {
	std::vector<std::uint8_t> bytes;
	std::size_t offset = 0;
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

TerminalStartResult TerminalStartResult::Success()
{
	return { true, 0, {} };
}

TerminalStartResult TerminalStartResult::Failure( std::uint32_t errorCode, std::wstring diagnostic )
{
	return { false, errorCode, std::move(diagnostic) };
}

struct CTerminalSession::Impl {
	struct SharedState {
		mutable std::mutex stateMutex;
		TerminalSessionState state = TerminalSessionState::Idle;
		std::uint32_t lastError = 0;

		mutable std::mutex callbackMutex;
		TerminalSessionCallbacks callbacks;

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
		std::chrono::steady_clock::time_point resizeDue{};

		std::atomic<bool> stopRequested{ false };
		std::atomic<bool> acceptingInput{ false };
		std::atomic<bool> backendStarted{ false };
		std::atomic<bool> backendClosed{ false };
		// Reader and writer failures can wake the peer by closing the backend.
		// Exactly one worker must own the terminal outcome so the peer cannot
		// replace a real failure with the close-induced EOF it observes later.
		std::atomic<bool> workerFinalizationClaimed{ false };
	};

	explicit Impl( std::unique_ptr<ITerminalBackend> backendValue, TerminalSessionCallbacks callbackValue )
		: backend(std::move(backendValue)), shared(std::make_shared<SharedState>())
	{
		shared->callbacks = std::move(callbackValue);
	}

	std::shared_ptr<ITerminalBackend> backend;
	std::shared_ptr<SharedState> shared;
	std::thread reader;
	std::thread writer;
	std::mutex lifecycleMutex;
	std::once_flag closeOnce;

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
		try {
			const std::lock_guard lock(state->callbackMutex);
			callbacks = state->callbacks;
		} catch( ... ) {}
		InvokeNoThrow( callbacks.stateChanged, next, errorCode );
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
		InvokeNoThrow(callback);
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

	static void FinalizeFromWorker( const std::shared_ptr<SharedState>& state, const std::shared_ptr<ITerminalBackend>& terminalBackend,
		TerminalSessionState terminalState, std::uint32_t errorCode = 0 ) noexcept
	{
		bool expected = false;
		if( !state->workerFinalizationClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel) ) return;
		if( StateOf(state) == TerminalSessionState::Running ) TransitionState(state, TerminalSessionState::Closing);
		StopWorkersState(state);
		if( terminalState == TerminalSessionState::Failed ) {
			try { terminalBackend->ForceTerminate(); } catch( ... ) {}
		}
		CloseBackendOnceState(state, terminalBackend);
		TransitionState(state, terminalState, errorCode);
	}

	static void CancelSynchronousIoNoThrow( std::thread& worker ) noexcept
	{
		if( !worker.joinable() ) return;
		const auto handle = reinterpret_cast<HANDLE>(worker.native_handle());
		::CancelSynchronousIo(handle);
	}

	static bool JoinBounded( std::thread& worker ) noexcept
	{
		if( !worker.joinable() ) return true;
		const auto handle = reinterpret_cast<HANDLE>(worker.native_handle());
		const auto wait = ::WaitForSingleObject( handle, static_cast<DWORD>(kWorkerJoinTimeout.count()) );
		if( wait == WAIT_OBJECT_0 ) {
			worker.join();
			return true;
		}
		::CancelSynchronousIo(handle);
		if( ::WaitForSingleObject(handle, static_cast<DWORD>(kWorkerJoinTimeout.count())) == WAIT_OBJECT_0 ) {
			worker.join();
			return true;
		}
		// Workers capture shared state and the backend by value, never `this`.
		// Detaching is the finite-time last resort for a broken backend.
		worker.detach();
		return false;
	}

	static void ReaderLoop( std::shared_ptr<SharedState> state, std::shared_ptr<ITerminalBackend> terminalBackend ) noexcept
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
					FinalizeFromWorker( state, terminalBackend, TerminalSessionState::Exited );
					return;
				}
				if( result.status == TerminalBackendReadStatus::Failed ) {
					const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
					FinalizeFromWorker( state, terminalBackend, terminalState, result.errorCode );
					return;
				}
				if( result.bytesTransferred == 0 || result.bytesTransferred > capacity ) {
					FinalizeFromWorker( state, terminalBackend, TerminalSessionState::Failed, ERROR_INVALID_DATA );
					return;
				}

				bool notify = false;
				{
					const std::lock_guard lock(state->outputMutex);
					ByteChunk chunk;
					chunk.bytes.assign( buffer.begin(), buffer.begin() + result.bytesTransferred );
					state->outputBytes += chunk.bytes.size();
					state->output.emplace_back(std::move(chunk));
					if( !state->outputNotificationPending ) {
						state->outputNotificationPending = true;
						notify = true;
					}
				}
				if( notify ) NotifyOutputState(state);
			}
		} catch( const std::bad_alloc& ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( state, terminalBackend, terminalState, ERROR_NOT_ENOUGH_MEMORY );
		} catch( ... ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( state, terminalBackend, terminalState, ERROR_UNHANDLED_EXCEPTION );
		}
	}

	static void WriterLoop( std::shared_ptr<SharedState> state, std::shared_ptr<ITerminalBackend> terminalBackend ) noexcept
	{
		try {
			while( !state->stopRequested.load(std::memory_order_acquire) ) {
				std::vector<std::uint8_t> bytes;
				std::optional<TerminalSize> resize;
				{
					std::unique_lock lock(state->inputMutex);
					for( ;; ) {
						if( state->stopRequested.load(std::memory_order_acquire) ) return;
						const auto now = std::chrono::steady_clock::now();
						if( state->pendingResize && now >= state->resizeDue ) {
							resize = state->pendingResize;
							state->pendingResize.reset();
							break;
						}
						if( !state->input.empty() ) {
							const auto& front = state->input.front();
							const auto count = std::min( kBackendWriteBytes, front.bytes.size() - front.offset );
							bytes.assign( front.bytes.begin() + front.offset, front.bytes.begin() + front.offset + count );
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
						FinalizeFromWorker( state, terminalBackend, terminalState, result.errorCode );
						return;
					}
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
					FinalizeFromWorker( state, terminalBackend, terminalState, error );
					return;
				}

				{
					const std::lock_guard lock(state->inputMutex);
					if( state->input.empty() ) continue;
					auto& front = state->input.front();
					front.offset += result.bytesTransferred;
					state->inputBytes -= result.bytesTransferred;
					if( front.offset == front.bytes.size() ) state->input.pop_front();
				}
			}
		} catch( const std::bad_alloc& ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( state, terminalBackend, terminalState, ERROR_NOT_ENOUGH_MEMORY );
		} catch( ... ) {
			const auto terminalState = StateOf(state) == TerminalSessionState::Closing ? TerminalSessionState::Exited : TerminalSessionState::Failed;
			FinalizeFromWorker( state, terminalBackend, terminalState, ERROR_UNHANDLED_EXCEPTION );
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
		}

		if( shared->backendStarted.load(std::memory_order_acquire) && !shared->backendClosed.load(std::memory_order_acquire) ) {
			try { backend->RequestGracefulClose(); } catch( ... ) {}
			bool exited = false;
			try { exited = backend->WaitForExit(CTerminalSession::kGracefulCloseTimeout); } catch( ... ) {}
			if( !exited ) {
				try { backend->ForceTerminate(); } catch( ... ) {}
				try { backend->WaitForExit(CTerminalSession::kForcedCloseTimeout); } catch( ... ) {}
			}
		}

		StopWorkers();
		CloseBackendOnce();
		CancelSynchronousIoNoThrow(reader);
		CancelSynchronousIoNoThrow(writer);
		const bool readerJoined = JoinBounded(reader);
		const bool writerJoined = JoinBounded(writer);
		if( (!readerJoined || !writerJoined) && State() == TerminalSessionState::Closing ) {
			Transition( TerminalSessionState::Failed, ERROR_TIMEOUT );
		} else if( State() == TerminalSessionState::Closing ) {
			Transition(TerminalSessionState::Exited);
		}
		{
			const std::lock_guard lock(shared->callbackMutex);
			shared->callbacks = {};
		}
	}
};

CTerminalSession::CTerminalSession( std::unique_ptr<ITerminalBackend> backend, TerminalSessionCallbacks callbacks )
	: m_impl(std::make_unique<Impl>(std::move(backend), std::move(callbacks)))
{
	if( !m_impl->backend ) throw std::invalid_argument("CTerminalSession requires a backend");
}

CTerminalSession::~CTerminalSession()
{
	Close();
}

TerminalStartResult CTerminalSession::Start( const TerminalLaunchOptions& options )
{
	const std::lock_guard lifecycleLock(m_impl->lifecycleMutex);
	if( m_impl->State() != TerminalSessionState::Idle ) return TerminalStartResult::Failure( ERROR_INVALID_STATE, L"Terminal session has already been started or closed." );
	m_impl->Transition(TerminalSessionState::Starting);
	if( options.executablePath.empty() || options.initialSize.columns == 0 || options.initialSize.rows == 0 ) {
		m_impl->Transition( TerminalSessionState::Failed, ERROR_INVALID_PARAMETER );
		m_impl->CloseBackendOnce();
		return TerminalStartResult::Failure( ERROR_INVALID_PARAMETER, L"Terminal launch options are invalid." );
	}

	TerminalStartResult result;
	try {
		result = m_impl->backend->Start(options);
	} catch( ... ) {
		result = TerminalStartResult::Failure( ERROR_UNHANDLED_EXCEPTION, L"Terminal backend initialization raised an exception." );
	}
	if( !result.succeeded ) {
		m_impl->Transition( TerminalSessionState::Failed, result.errorCode );
		m_impl->CloseBackendOnce();
		return result;
	}
	m_impl->shared->backendStarted.store(true, std::memory_order_release);
	m_impl->Transition(TerminalSessionState::Running);
	m_impl->shared->acceptingInput.store(true, std::memory_order_release);

	try {
		const auto state = m_impl->shared;
		const auto backend = m_impl->backend;
		m_impl->reader = std::thread( [state, backend] { Impl::ReaderLoop(state, backend); } );
		m_impl->writer = std::thread( [state, backend] { Impl::WriterLoop(state, backend); } );
	} catch( const std::system_error& error ) {
		m_impl->Transition( TerminalSessionState::Failed, static_cast<std::uint32_t>(error.code().value()) );
		m_impl->StopWorkers();
		m_impl->CloseBackendOnce();
		Impl::CancelSynchronousIoNoThrow(m_impl->reader);
		Impl::JoinBounded(m_impl->reader);
		return TerminalStartResult::Failure( static_cast<std::uint32_t>(error.code().value()), L"Unable to create terminal I/O workers." );
	}
	return result;
}

void CTerminalSession::Close() noexcept
{
	if( !m_impl ) return;
	std::call_once( m_impl->closeOnce, [this] { m_impl->CloseImpl(); } );
}

TerminalQueueInputResult CTerminalSession::QueueInput( std::span<const std::uint8_t> bytes )
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
	{
		const std::lock_guard lock(m_impl->shared->inputMutex);
		if( !m_impl->shared->acceptingInput.load(std::memory_order_acquire) ) return false;
		m_impl->shared->pendingResize = size;
		m_impl->shared->resizeDue = std::chrono::steady_clock::now() + kResizeCoalesceDelay;
	}
	m_impl->shared->inputAvailable.notify_one();
	return true;
}

std::vector<std::uint8_t> CTerminalSession::DrainOutput()
{
	std::vector<std::uint8_t> result;
	result.reserve(kMaximumDrainBytes);
	bool renotify = false;
	const auto deadline = std::chrono::steady_clock::now() + kMaximumDrainTime;
	{
		const std::lock_guard lock(m_impl->shared->outputMutex);
		m_impl->shared->outputNotificationPending = false;
		while( !m_impl->shared->output.empty() && result.size() < kMaximumDrainBytes && std::chrono::steady_clock::now() < deadline ) {
			auto& front = m_impl->shared->output.front();
			const auto count = std::min( kMaximumDrainBytes - result.size(), front.bytes.size() - front.offset );
			result.insert( result.end(), front.bytes.begin() + front.offset, front.bytes.begin() + front.offset + count );
			front.offset += count;
			m_impl->shared->outputBytes -= count;
			if( front.offset == front.bytes.size() ) m_impl->shared->output.pop_front();
		}
		if( !m_impl->shared->output.empty() ) {
			m_impl->shared->outputNotificationPending = true;
			renotify = true;
		}
	}
	if( GetQueuedOutputBytes() <= kOutputLowWaterBytes ) m_impl->shared->outputSpaceAvailable.notify_one();
	if( renotify ) m_impl->NotifyOutput();
	return result;
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
