/*!
    @file TerminalSessionRetirementService.h
    @brief Admission-bounded ownership for terminal session retirement.
*/
#pragma once

#include "terminal/session/TerminalSession.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace terminal {

//! State of a session's handoff to the terminal retirement service.
//!
//! Retired means that the service has accepted ownership; it does not mean
//! that the session is already quiescent.  The observation phase distinguishes
//! the queued/joining/completed portions of that lifecycle.
enum class TerminalSessionRetirementStatus : std::uint8_t {
	Pending,
	Retired,
	Quarantined,
	CapacityExhausted,
	InvalidSession,
	InvalidReservation,
	Failed,
};

enum class TerminalSessionRetirementPhase : std::uint8_t {
	Reserved,
	Queued,
	Joining,
	Completed,
	Quarantined,
	Failed,
};

struct TerminalSessionRetirementSnapshot final {
	TerminalSessionRetirementStatus status{ TerminalSessionRetirementStatus::Pending };
	TerminalSessionRetirementPhase phase{ TerminalSessionRetirementPhase::Reserved };
	TerminalSessionState sessionState{ TerminalSessionState::Idle };
	TerminalSessionCloseWaitStatus waitStatus{ TerminalSessionCloseWaitStatus::InProgress };
};

class TerminalSessionRetirementObservation final {
public:
	[[nodiscard]] TerminalSessionRetirementSnapshot Snapshot() const noexcept
	{
		return {
			m_status.load(std::memory_order_acquire),
			m_phase.load(std::memory_order_acquire),
			m_sessionState.load(std::memory_order_acquire),
			m_waitStatus.load(std::memory_order_acquire),
		};
	}

private:
	friend class TerminalSessionRetirementService;

	void SetStatus( const TerminalSessionRetirementStatus status ) noexcept
	{
		m_status.store(status, std::memory_order_release);
	}

	void SetPhase( const TerminalSessionRetirementPhase phase ) noexcept
	{
		m_phase.store(phase, std::memory_order_release);
	}

	void SetSessionState( const TerminalSessionState state ) noexcept
	{
		m_sessionState.store(state, std::memory_order_release);
	}

	void SetWaitStatus( const TerminalSessionCloseWaitStatus status ) noexcept
	{
		m_waitStatus.store(status, std::memory_order_release);
	}

	std::atomic<TerminalSessionRetirementStatus> m_status{ TerminalSessionRetirementStatus::Pending };
	std::atomic<TerminalSessionRetirementPhase> m_phase{ TerminalSessionRetirementPhase::Reserved };
	std::atomic<TerminalSessionState> m_sessionState{ TerminalSessionState::Idle };
	std::atomic<TerminalSessionCloseWaitStatus> m_waitStatus{ TerminalSessionCloseWaitStatus::InProgress };
};

struct TerminalSessionRetirementResult final {
	TerminalSessionRetirementStatus status{ TerminalSessionRetirementStatus::Failed };
	std::shared_ptr<TerminalSessionRetirementObservation> observation;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return status == TerminalSessionRetirementStatus::Retired;
	}
};

//! Process-lifetime, admission-bounded owner for sessions removed by the UI.
//!
//! A reservation is obtained before a terminal session is started.  Once the
//! UI has requested close, Handoff() moves the session into one of the fixed
//! slots and returns immediately.  Reapers perform the only WaitForClose() and
//! retain ownership in the slot if the wait cannot produce a quiescent result;
//! a live session is never detached or destroyed on the UI thread.
class TerminalSessionRetirementService final {
public:
	static constexpr std::size_t kMaximumSessions = 32;
	static constexpr std::size_t kReaperThreads = 4;

	class Reservation final {
	public:
		Reservation() noexcept = default;
		Reservation( const Reservation& ) = delete;
		Reservation& operator=( const Reservation& ) = delete;

		Reservation( Reservation&& other ) noexcept
		{
			MoveFrom(std::move(other));
		}

		Reservation& operator=( Reservation&& other ) noexcept
		{
			if( this != &other ) {
				Reset();
				MoveFrom(std::move(other));
			}
			return *this;
		}

		~Reservation() noexcept
		{
			Reset();
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return m_owner != nullptr && m_observation != nullptr;
		}

		[[nodiscard]] std::shared_ptr<TerminalSessionRetirementObservation> Observation() const noexcept
		{
			return m_observation;
		}

		void Reset() noexcept;

	private:
		friend class TerminalSessionRetirementService;

		Reservation(
			TerminalSessionRetirementService* owner,
			const std::size_t index,
			const std::uint64_t generation,
			std::shared_ptr<TerminalSessionRetirementObservation> observation) noexcept
			: m_owner(owner)
			, m_index(index)
			, m_generation(generation)
			, m_observation(std::move(observation))
		{
		}

		void Disarm() noexcept
		{
			m_owner = nullptr;
		}

		void MoveFrom( Reservation&& other ) noexcept
		{
			m_owner = other.m_owner;
			m_index = other.m_index;
			m_generation = other.m_generation;
			m_observation = std::move(other.m_observation);
			other.m_owner = nullptr;
		}

		TerminalSessionRetirementService* m_owner = nullptr;
		std::size_t m_index = 0;
		std::uint64_t m_generation = 0;
		std::shared_ptr<TerminalSessionRetirementObservation> m_observation;
	};

	static TerminalSessionRetirementService& Instance() noexcept
	{
		// A session can remain in third-party/ConPTY shutdown after the window has
		// gone away.  Deliberately leak the service so static destruction cannot
		// turn that external stall into a process-shutdown join.
		static TerminalSessionRetirementService* const service = new TerminalSessionRetirementService();
		return *service;
	}

	//! Reserves a finalizer slot without waiting for any existing session.
	[[nodiscard]] std::optional<Reservation> TryReserve() noexcept
	{
		std::lock_guard lock(m_mutex);
		if( m_reaperCount == 0 ) return std::nullopt;
		for( std::size_t index = 0; index < m_slots.size(); ++index ) {
			auto& slot = m_slots[index];
			if( slot.state != ESlotState::Free ) continue;
			std::shared_ptr<TerminalSessionRetirementObservation> observation;
			try {
				observation = std::make_shared<TerminalSessionRetirementObservation>();
			} catch( ... ) {
				return std::nullopt;
			}
			slot.state = ESlotState::Reserved;
			slot.observation = observation;
			++m_admittedCount;
			return Reservation(this, index, slot.generation, std::move(observation));
		}
		return std::nullopt;
	}

	//! Transfers a close-requested session to a fixed retirement slot.
	//!
	//! The caller must invoke CTerminalSession::BeginClose() before this method.
	//! Handoff itself only moves ownership and signals a reaper; it never waits.
	[[nodiscard]] TerminalSessionRetirementResult Handoff(
		std::unique_ptr<CTerminalSession>& session,
		Reservation&& reservation) noexcept
	{
		const auto observation = reservation.m_observation;
		if( !session ) {
			MarkRejected(observation, TerminalSessionRetirementStatus::InvalidSession);
			return { TerminalSessionRetirementStatus::InvalidSession, observation };
		}

		std::lock_guard lock(m_mutex);
		if( !IsValidReservationLocked(reservation) ) {
			MarkRejected(observation, TerminalSessionRetirementStatus::InvalidReservation);
			return { TerminalSessionRetirementStatus::InvalidReservation, observation };
		}
		if( m_queuedCount >= m_queue.size() ) {
			MarkRejected(observation, TerminalSessionRetirementStatus::CapacityExhausted);
			return { TerminalSessionRetirementStatus::CapacityExhausted, observation };
		}

		auto& slot = m_slots[reservation.m_index];
		slot.session = std::move(session);
		slot.state = ESlotState::Queued;
		m_queue[(m_queueHead + m_queuedCount) % m_queue.size()] = reservation.m_index;
		++m_queuedCount;
		observation->SetStatus(TerminalSessionRetirementStatus::Retired);
		observation->SetPhase(TerminalSessionRetirementPhase::Queued);
		reservation.Disarm();
		m_ready.notify_one();
		return { TerminalSessionRetirementStatus::Retired, observation };
	}

	//! Requeues sessions that were retained after a non-quiescent wait result.
	//! This remains bounded by the fixed slot and queue arrays.
	std::size_t RetryQuarantined() noexcept
	{
		std::size_t queued = 0;
		{
			std::lock_guard lock(m_mutex);
			for( auto& slot : m_slots ) {
				if( slot.state != ESlotState::Quarantined || m_queuedCount >= m_queue.size() ) break;
				slot.state = ESlotState::Queued;
				if( slot.observation ) {
					slot.observation->SetStatus(TerminalSessionRetirementStatus::Retired);
					slot.observation->SetPhase(TerminalSessionRetirementPhase::Queued);
				}
				m_queue[(m_queueHead + m_queuedCount) % m_queue.size()] = static_cast<std::size_t>(&slot - m_slots.data());
				++m_queuedCount;
				++queued;
			}
		}
		if( queued != 0 ) m_ready.notify_all();
		return queued;
	}

	[[nodiscard]] std::size_t ReservedOrPendingCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		return m_admittedCount;
	}

	[[nodiscard]] std::size_t QuarantinedCount() const noexcept
	{
		std::lock_guard lock(m_mutex);
		std::size_t count = 0;
		for( const auto& slot : m_slots ) if( slot.state == ESlotState::Quarantined ) ++count;
		return count;
	}

private:
	enum class ESlotState : std::uint8_t {
		Free,
		Reserved,
		Queued,
		Joining,
		Quarantined,
	};

	struct Slot final {
		ESlotState state{ ESlotState::Free };
		std::uint64_t generation{ 1 };
		std::unique_ptr<CTerminalSession> session;
		std::shared_ptr<TerminalSessionRetirementObservation> observation;
	};

	TerminalSessionRetirementService() noexcept
	{
		for( auto& reaper : m_reapers ) {
			try {
				reaper = std::jthread([this](std::stop_token stopToken) { ReaperMain(stopToken); });
				++m_reaperCount;
			} catch( ... ) {
				break;
			}
		}
	}

	static void MarkRejected(
		const std::shared_ptr<TerminalSessionRetirementObservation>& observation,
		const TerminalSessionRetirementStatus status) noexcept
	{
		if( !observation ) return;
		observation->SetStatus(status);
		observation->SetPhase(TerminalSessionRetirementPhase::Failed);
	}

	[[nodiscard]] bool IsValidReservationLocked( const Reservation& reservation ) const noexcept
	{
		return reservation.m_owner == this
			&& reservation.m_index < m_slots.size()
			&& m_slots[reservation.m_index].state == ESlotState::Reserved
			&& m_slots[reservation.m_index].generation == reservation.m_generation
			&& reservation.m_observation != nullptr;
	}

	void ReleaseReservation( const std::size_t index, const std::uint64_t generation ) noexcept
	{
		std::lock_guard lock(m_mutex);
		if( index >= m_slots.size() ) return;
		auto& slot = m_slots[index];
		if( slot.state != ESlotState::Reserved || slot.generation != generation ) return;
		if( slot.observation ) MarkRejected(slot.observation, TerminalSessionRetirementStatus::Failed);
		ReleaseSlotLocked(slot);
	}

	void ReleaseSlotLocked( Slot& slot ) noexcept
	{
		slot.session.reset();
		slot.observation.reset();
		slot.state = ESlotState::Free;
		++slot.generation;
		if( slot.generation == 0 ) slot.generation = 1;
		if( m_admittedCount != 0 ) --m_admittedCount;
	}

	void ReaperMain( const std::stop_token stopToken ) noexcept
	{
		for( ;; ) {
			std::size_t index = m_slots.size();
			std::unique_ptr<CTerminalSession> session;
			std::shared_ptr<TerminalSessionRetirementObservation> observation;
			{
				std::unique_lock lock(m_mutex);
				m_ready.wait(lock, [this, stopToken] {
					return stopToken.stop_requested() || m_queuedCount != 0;
				});
				if( m_queuedCount == 0 && stopToken.stop_requested() ) return;
				index = m_queue[m_queueHead];
				m_queueHead = (m_queueHead + 1) % m_queue.size();
				--m_queuedCount;
				auto& slot = m_slots[index];
				slot.state = ESlotState::Joining;
				session = std::move(slot.session);
				observation = std::move(slot.observation);
			}

			if( observation ) observation->SetPhase(TerminalSessionRetirementPhase::Joining);
			bool completed = false;
			if( session ) {
				const auto wait = session->WaitForClose(std::chrono::steady_clock::time_point::max());
				if( observation ) {
					observation->SetWaitStatus(wait.status);
					observation->SetSessionState(session->GetState());
				}
				completed = wait.IsQuiescent();
			}
			if( observation ) {
				observation->SetStatus(completed
					? TerminalSessionRetirementStatus::Retired
					: TerminalSessionRetirementStatus::Quarantined);
				observation->SetPhase(completed
					? TerminalSessionRetirementPhase::Completed
					: TerminalSessionRetirementPhase::Quarantined);
			}

			std::lock_guard lock(m_mutex);
			auto& slot = m_slots[index];
			slot.session = std::move(session);
			slot.observation = std::move(observation);
			if( completed ) ReleaseSlotLocked(slot);
			else slot.state = ESlotState::Quarantined;
		}
	}

	mutable std::mutex m_mutex;
	std::condition_variable m_ready;
	std::array<Slot, kMaximumSessions> m_slots{};
	std::array<std::size_t, kMaximumSessions> m_queue{};
	std::size_t m_queueHead = 0;
	std::size_t m_queuedCount = 0;
	std::size_t m_admittedCount = 0;
	std::array<std::jthread, kReaperThreads> m_reapers{};
	std::size_t m_reaperCount = 0;
};

//! Couples a pre-admitted retirement slot to one live terminal session.
//!
//! Once a session has been adopted, every destruction path requests close and
//! hands it to the service.  A failed handoff is fatal rather than silently
//! releasing a live worker, which keeps the ownership invariant explicit.
class TerminalSessionRetirementLease final {
public:
	static std::unique_ptr<TerminalSessionRetirementLease> TryCreate() noexcept
	{
		auto& service = TerminalSessionRetirementService::Instance();
		auto reservation = service.TryReserve();
		if( !reservation ) return nullptr;
		try {
			return std::unique_ptr<TerminalSessionRetirementLease>(
				new TerminalSessionRetirementLease(service, std::move(*reservation)));
		} catch( ... ) {
			return nullptr;
		}
	}

	TerminalSessionRetirementLease( const TerminalSessionRetirementLease& ) = delete;
	TerminalSessionRetirementLease& operator=( const TerminalSessionRetirementLease& ) = delete;

	~TerminalSessionRetirementLease() noexcept
	{
		if( !m_session ) return;
		const auto result = RetireNow();
		if( !result.Accepted() || m_session ) std::terminate();
	}

	void Adopt( std::unique_ptr<CTerminalSession> session ) noexcept
	{
		m_session = std::move(session);
	}

	[[nodiscard]] CTerminalSession* Session() noexcept { return m_session.get(); }
	[[nodiscard]] const CTerminalSession* Session() const noexcept { return m_session.get(); }

	//! Requests close and transfers the session without waiting.
	[[nodiscard]] TerminalSessionRetirementResult RetireNow() noexcept
	{
		if( !m_session ) return { TerminalSessionRetirementStatus::InvalidSession, m_observation };
		if( !m_reservation ) return { TerminalSessionRetirementStatus::InvalidReservation, m_observation };
		m_session->BeginClose();
		auto result = m_service.Handoff(m_session, std::move(*m_reservation));
		if( result.Accepted() ) {
			m_reservation.reset();
			m_observation = std::move(result.observation);
		}
		return result;
	}

	[[nodiscard]] std::shared_ptr<TerminalSessionRetirementObservation> Observation() const noexcept
	{
		return m_observation;
	}

private:
	TerminalSessionRetirementLease(
		TerminalSessionRetirementService& service,
		TerminalSessionRetirementService::Reservation&& reservation) noexcept
		: m_service(service)
		, m_observation(reservation.Observation())
		, m_reservation(std::move(reservation))
	{
	}

	TerminalSessionRetirementService& m_service;
	std::unique_ptr<CTerminalSession> m_session;
	std::shared_ptr<TerminalSessionRetirementObservation> m_observation;
	std::optional<TerminalSessionRetirementService::Reservation> m_reservation;
};

inline void TerminalSessionRetirementService::Reservation::Reset() noexcept
{
	if( m_owner != nullptr ) {
		m_owner->ReleaseReservation(m_index, m_generation);
		m_owner = nullptr;
	}
	m_observation.reset();
}

} // namespace terminal
