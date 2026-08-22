/*! @file */
#include "StdAfx.h"
#include "terminal/session/TerminalSession.h"
#include "terminal/TerminalWorkerRetirementService.h"

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

TerminalStartResult TerminalStartResult::Aborted()
{
	return Failure(ERROR_CANCELLED, L"Terminal start was cancelled by a close request.");
}

struct CTerminalSession::Impl : std::enable_shared_from_this<CTerminalSession::Impl> {
	struct SharedState {
		mutable std::mutex stateMutex;
		TerminalSessionState state = TerminalSessionState::Idle;
		std::uint32_t lastError = 0;

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

	explicit Impl( std::unique_ptr<ITerminalBackend> backendValue, TerminalSessionCallbacks callbackValue )
		: backend(std::move(backendValue)), shared(std::make_shared<SharedState>())
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
						FinalizeFromWorker( self, state, terminalBackend, terminalState, result.errorCode );
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
					FinalizeFromWorker( self, state, terminalBackend, terminalState, error );
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

CTerminalSession::CTerminalSession( std::unique_ptr<ITerminalBackend> backend, TerminalSessionCallbacks callbacks )
	: m_impl(std::make_shared<Impl>(std::move(backend), std::move(callbacks)))
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
	const auto impl = m_impl;
	if( !impl ) return {};
	std::vector<std::uint8_t> result;
	result.reserve(kMaximumDrainBytes);
	bool renotify = false;
	const auto deadline = std::chrono::steady_clock::now() + kMaximumDrainTime;
	{
		const std::lock_guard lock(impl->shared->outputMutex);
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
	}
	if( GetQueuedOutputBytes() <= kOutputLowWaterBytes ) impl->shared->outputSpaceAvailable.notify_one();
	if( renotify ) impl->NotifyOutput();
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
