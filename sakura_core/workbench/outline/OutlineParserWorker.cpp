/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "workbench/outline/OutlineParserWorker.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace workbench::outline {

namespace {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t NowUs() noexcept
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now().time_since_epoch()).count());
}

[[nodiscard]] bool IsCancelled( const OutlineParserWorker::CancelToken& token ) noexcept
{
	return token != nullptr && token->load(std::memory_order_acquire);
}

[[nodiscard]] OutlineParserWorker::ParseFunction RequireParser(
	OutlineParserWorker::ParseFunction parser )
{
	if( !parser ) throw std::invalid_argument("Outline parser function is required");
	return parser;
}

} // namespace

struct OutlineParserWorker::NotificationGate final {
	std::mutex mutex;
	HWND window = nullptr;
	bool alive = false;
	std::unique_ptr<OutlineWorkerResult> pendingResult;
	bool messagePosted = false;
};

struct OutlineParserWorker::SharedState final {
	std::mutex mutex;
	std::condition_variable wake;
	bool accepting = false;
	bool closed = false;
	bool stop = false;
	bool promotionPaused = false;
	std::optional<Job> active;
	std::optional<Job> pending;
	std::uint64_t nextGeneration = 0;
	std::uint64_t startedCount = 0;
	std::uint64_t completedCount = 0;
	std::uint64_t cancelledCount = 0;
	std::uint64_t failedCount = 0;
	std::uint64_t supersededCount = 0;
	std::uint64_t lastGeneration = 0;
	OutlineWorkerTerminal lastTerminal = OutlineWorkerTerminal::Closed;
	OutlinePhaseTimings lastTimings{};
};

OutlineParserWorker::OutlineParserWorker( ParseFunction parser )
	: m_shared(std::make_shared<SharedState>())
	, m_gate(std::make_shared<NotificationGate>())
	, m_parser(RequireParser(std::move(parser)))
	, m_ownerThread(std::this_thread::get_id())
	, m_worker(&OutlineParserWorker::WorkerMain, this)
{
}

OutlineParserWorker::~OutlineParserWorker()
{
	Close();
}

void OutlineParserWorker::SetNotificationWindow( HWND window, bool accepting ) noexcept
{
	{
		std::lock_guard lock(m_gate->mutex);
		m_gate->window = accepting && window != nullptr ? window : nullptr;
		m_gate->alive = accepting && window != nullptr;
		if( !m_gate->alive ) {
			m_gate->pendingResult.reset();
			m_gate->messagePosted = false;
		}
	}

	std::lock_guard lock(m_shared->mutex);
	if( !m_shared->closed ) m_shared->accepting = accepting && window != nullptr;
}

OutlineWorkerSubmitResult OutlineParserWorker::Submit(
	Snapshot snapshot,
	int outlineType,
	int listType,
	std::uint64_t snapshotCaptureUs )
{
	if( snapshot == nullptr || !snapshot->IsValid() ) {
		return { OutlineWorkerRequestStatus::InvalidSnapshot, 0 };
	}

	OutlineWorkerRequestStatus status = OutlineWorkerRequestStatus::Closed;
	std::uint64_t generation = 0;
	{
		std::lock_guard lock(m_shared->mutex);
		if( m_shared->closed || !m_shared->accepting ) return { status, 0 };
		if( m_shared->nextGeneration == (std::numeric_limits<std::uint64_t>::max)() ) {
			return { OutlineWorkerRequestStatus::GenerationExhausted, 0 };
		}

		auto matches = [&]( const Job& job ) noexcept {
			return job.outlineType == outlineType
				&& job.listType == listType
				&& job.snapshot != nullptr
				&& job.snapshot->documentVersion == snapshot->documentVersion;
		};
		if( m_shared->active && matches(*m_shared->active) ) {
			return { OutlineWorkerRequestStatus::ActiveDeduplicated, m_shared->active->generation };
		}
		if( m_shared->pending && matches(*m_shared->pending) ) {
			return { OutlineWorkerRequestStatus::PendingDeduplicated, m_shared->pending->generation };
		}

		Job job;
		job.snapshot = std::move(snapshot);
		job.outlineType = outlineType;
		job.listType = listType;
		// Generation zero is reserved for "no request".  The max value is a
		// valid final generation; the next request fails closed above instead of
		// wrapping back to zero and colliding with an old result.
		job.generation = ++m_shared->nextGeneration;
		generation = job.generation;
		job.submittedAtUs = NowUs();
		job.snapshotCaptureUs = snapshotCaptureUs;
		job.cancelToken = std::make_shared<std::atomic_bool>(false);

		if( m_shared->active ) {
			m_shared->active->cancelToken->store(true, std::memory_order_release);
			if( m_shared->pending ) {
				++m_shared->supersededCount;
				m_shared->pending.reset();
				status = OutlineWorkerRequestStatus::PendingReplaced;
			}else{
				status = OutlineWorkerRequestStatus::Queued;
			}
			m_shared->pending.emplace(std::move(job));
		}else{
			if( m_shared->pending ) {
				// The worker has not promoted the first request yet.  This is still
				// a latest-wins replacement, not a new start; report it explicitly
				// so counters and deterministic callers agree with the queue state.
				++m_shared->supersededCount;
				m_shared->pending.reset();
				status = OutlineWorkerRequestStatus::PendingReplaced;
			}
			m_shared->pending.emplace(std::move(job));
			if( status == OutlineWorkerRequestStatus::Closed ) {
				status = OutlineWorkerRequestStatus::Started;
			}
		}
	}

	m_shared->wake.notify_one();
	return { status, generation };
}

OutlineWorkerCancellationResult OutlineParserWorker::CancelObsolete(
	OutlineDocumentVersion currentVersion ) noexcept
{
	OutlineWorkerCancellationResult result;
	std::lock_guard lock(m_shared->mutex);
	if( m_shared->closed ) return result;
	if( m_shared->active && m_shared->active->snapshot != nullptr
		&& m_shared->active->snapshot->documentVersion != currentVersion ) {
		m_shared->active->cancelToken->store(true, std::memory_order_release);
		result.activeCancelled = true;
	}
	if( m_shared->pending && m_shared->pending->snapshot != nullptr
		&& m_shared->pending->snapshot->documentVersion != currentVersion ) {
		m_shared->lastGeneration = m_shared->pending->generation;
		m_shared->lastTerminal = OutlineWorkerTerminal::Superseded;
		++m_shared->supersededCount;
		m_shared->pending.reset();
		result.pendingDiscarded = true;
	}
	return result;
}

void OutlineParserWorker::SetPromotionPausedForTest( bool paused ) noexcept
{
	{
		std::lock_guard lock(m_shared->mutex);
		m_shared->promotionPaused = paused;
	}
	m_shared->wake.notify_all();
}

void OutlineParserWorker::SetNextGenerationForTest( std::uint64_t nextGeneration ) noexcept
{
	std::lock_guard lock(m_shared->mutex);
	m_shared->nextGeneration = nextGeneration;
}

std::unique_ptr<OutlineWorkerResult> OutlineParserWorker::TakePendingResult(
	OutlineWorkerResult* raw ) noexcept
{
	std::lock_guard lock(m_gate->mutex);
	if( m_gate->pendingResult == nullptr ) return nullptr;
	if( raw != nullptr && m_gate->pendingResult.get() != raw ) return nullptr;
	std::unique_ptr<OutlineWorkerResult> result = std::move(m_gate->pendingResult);
	m_gate->messagePosted = false;
	return result;
}

OutlineWorkerStateSnapshot OutlineParserWorker::GetStateSnapshot() const noexcept
{
	OutlineWorkerStateSnapshot snapshot;
	{
		std::lock_guard lock(m_gate->mutex);
		snapshot.acceptingNotifications = m_gate->alive && m_gate->window != nullptr;
		snapshot.resultPending = m_gate->pendingResult != nullptr;
		snapshot.resultMessagePosted = m_gate->messagePosted;
	}
	std::lock_guard lock(m_shared->mutex);
	snapshot.closed = m_shared->closed;
	snapshot.active = m_shared->active.has_value();
	snapshot.pending = m_shared->pending.has_value();
	if( m_shared->active ) {
		snapshot.activeGeneration = m_shared->active->generation;
		snapshot.activeVersion = m_shared->active->snapshot->documentVersion;
	}
	if( m_shared->pending ) {
		snapshot.pendingGeneration = m_shared->pending->generation;
		snapshot.pendingVersion = m_shared->pending->snapshot->documentVersion;
	}
	snapshot.startedCount = m_shared->startedCount;
	snapshot.completedCount = m_shared->completedCount;
	snapshot.cancelledCount = m_shared->cancelledCount;
	snapshot.failedCount = m_shared->failedCount;
	snapshot.supersededCount = m_shared->supersededCount;
	snapshot.lastGeneration = m_shared->lastGeneration;
	snapshot.lastTerminal = m_shared->lastTerminal;
	snapshot.lastTimings = m_shared->lastTimings;
	return snapshot;
}

void OutlineParserWorker::Close() noexcept
{
	{
		std::lock_guard lock(m_gate->mutex);
		m_gate->alive = false;
		m_gate->window = nullptr;
		m_gate->pendingResult.reset();
		m_gate->messagePosted = false;
	}
	{
		std::lock_guard lock(m_shared->mutex);
		if( !m_shared->closed ) {
			m_shared->accepting = false;
			m_shared->closed = true;
			m_shared->stop = true;
			if( m_shared->active ) m_shared->active->cancelToken->store(true, std::memory_order_release);
			m_shared->pending.reset();
		}
	}
	m_shared->wake.notify_all();
	if( m_worker.joinable() ) {
		// Close is owned by the dialog/UI thread. A worker-thread close cannot
		// join itself; terminating this invalid lifecycle is safer than detaching
		// a thread that could outlive the owning object.
		if( std::this_thread::get_id() != m_ownerThread || m_worker.get_id() == std::this_thread::get_id() ) std::terminate();
		m_worker.join();
	}
}

void OutlineParserWorker::PostResult( std::unique_ptr<OutlineWorkerResult> result ) noexcept
{
	if( result == nullptr ) return;
	// Cancelled, superseded and closed outcomes are accounted for internally.
	// Failed is deliberately delivered: the latest request needs an observable
	// terminal state so the UI can clear its in-flight marker without destroying
	// the last committed model.  The gate still owns at most one result and one
	// posted message.
	if( result->terminal != OutlineWorkerTerminal::Parsed
		&& result->terminal != OutlineWorkerTerminal::Failed ) return;
	std::lock_guard lock(m_gate->mutex);
	if( !m_gate->alive || m_gate->window == nullptr ) return;
	if( m_gate->pendingResult != nullptr
		&& m_gate->pendingResult->generation > result->generation ) return;
	m_gate->pendingResult = std::move(result);
	if( m_gate->messagePosted ) return;
	m_gate->messagePosted = true;
	if( !::PostMessageW(m_gate->window, kWorkerResultMessage, 0, 0) ) {
		m_gate->pendingResult.reset();
		m_gate->messagePosted = false;
	}
}

void OutlineParserWorker::WorkerMain() noexcept
{
	for(;;) {
		Job job;
		{
			std::unique_lock lock(m_shared->mutex);
			m_shared->wake.wait(lock, [this] {
				return m_shared->stop || (m_shared->pending.has_value() && !m_shared->promotionPaused);
			});
			if( m_shared->stop ) return;
			job = std::move(*m_shared->pending);
			m_shared->pending.reset();
			m_shared->active.emplace(job);
			++m_shared->startedCount;
		}

		auto result = std::make_unique<OutlineWorkerResult>();
		result->generation = job.generation;
		result->parse.documentVersion = job.snapshot->documentVersion;
		const auto parseBegin = NowUs();
		try {
			result->parse = m_parser(*job.snapshot, job.outlineType, job.listType, job.cancelToken);
			result->parse.documentVersion = job.snapshot->documentVersion;
			result->parse.timings.snapshotCaptureUs = job.snapshotCaptureUs;
			result->parse.timings.queueWaitUs = parseBegin >= job.submittedAtUs
				? parseBegin - job.submittedAtUs : 0;
			// Worker total is capture start -> DTO-ready.  UI commit/appearance/
			// selection are added once by CDlgFuncList before publication.
			result->parse.timings.totalUs = result->parse.timings.snapshotCaptureUs
				+ result->parse.timings.queueWaitUs
				+ result->parse.timings.parserUs
				+ result->parse.timings.dtoConstructionUs;
			result->terminal = OutlineWorkerTerminal::Parsed;
		}catch( ... ) {
			result->parse.timings.snapshotCaptureUs = job.snapshotCaptureUs;
			result->parse.timings.queueWaitUs = parseBegin >= job.submittedAtUs
				? parseBegin - job.submittedAtUs : 0;
			result->parse.timings.totalUs = result->parse.timings.snapshotCaptureUs
				+ result->parse.timings.queueWaitUs;
			result->terminal = OutlineWorkerTerminal::Failed;
		}

		bool closed = false;
		const bool cancelled = IsCancelled(job.cancelToken);
		{
			std::lock_guard lock(m_shared->mutex);
			closed = m_shared->closed;
			if( m_shared->active && m_shared->active->generation == job.generation ) m_shared->active.reset();
			if( closed ) {
				result->terminal = OutlineWorkerTerminal::Closed;
			}else if( cancelled ) {
				result->terminal = OutlineWorkerTerminal::Cancelled;
			}else if( result->terminal != OutlineWorkerTerminal::Failed ) {
				result->terminal = OutlineWorkerTerminal::Parsed;
			}
			switch( result->terminal ) {
			case OutlineWorkerTerminal::Parsed:
				++m_shared->completedCount;
				break;
			case OutlineWorkerTerminal::Cancelled:
			case OutlineWorkerTerminal::Closed:
				++m_shared->cancelledCount;
				break;
			case OutlineWorkerTerminal::Failed:
				++m_shared->failedCount;
				break;
			default:
				break;
			}
			m_shared->lastGeneration = result->generation;
			m_shared->lastTerminal = result->terminal;
			m_shared->lastTimings = result->parse.timings;
		}
		if( !closed ) PostResult(std::move(result));
	}
}

} // namespace workbench::outline
